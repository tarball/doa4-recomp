// DOA4's random number generators: four Mersenne twisters (624 words, then the next word's address at +2496, the words
// left at +2500 and a critical section at +2504; sub_82334C60 seeds one, sub_82334CE8 regenerates it), each seeded at
// boot by a static initializer from the clock. The shared one at 0x8357B5C0 (mftb, sub_82B58228) is drawn from through
// five functions with 1,624 call sites: sub_8276B5F0 (a float times a scale), sub_8276B6A8 (a float in [0, 1)),
// sub_8276B770 (a word), sub_8276B818 and sub_8276B8F8 (integers in a range). The fight's at 0x833F7600 (time(NULL),
// sub_82A38728) through sub_824061A0 (55 call sites, all fight code). The others are drawn from inline: 0x8321B198
// (mftb, sub_82A18320) in sub_823415D8 and sub_82342668, and 0x83420020 (mftb, sub_82A3E500) in sub_8241DFF8 and
// sub_82540B80 and the functions only they call. The state census (netplay/state_census.h) hears of each draw here,
// with its thread: the inline ones counted by the words left before and after those functions.
//
// The main thread draws from the shared generator for the fight (the player task's sub_82318730, the AI) and for
// effects. So does worker pool 825689E8, the stage's update: each of its five threads runs one phase of it every frame,
// the stage object's method at vtable +348 with the thread's phase (sub_825689E8; Waterfall Valley's is sub_825AB7B8),
// and the phases draw at once, in an order that varies from run to run, and the fight's numbers after theirs with it.
// With netplay::SplitRandom(), draws on threads other than the main thread come from a stream keyed by the thread and the
// frame instead, restarted every frame, which repeats in a replay as long as each thread does the same work in a frame
// (as that pool's do), and the shared generator's sequence depends on the main thread's draws alone. The numbers are
// computed as the game's functions compute theirs.
//
// Online, the game keeps both machines' fights in step: as an online fight starts it seeds the fight's generator from
// values both share (the input exchange's frame count and a 64-bit word at 0x830C1CB8, and the word at +32 of the
// structure at 0x82E6D978: sub_82403CE8, sub_82521478), and sub_82318730, the one function that draws from both the fight's
// generator and the shared one, takes the fight's in an online fight (the word at +472 of that structure set) while the
// fight runs. With netplay::FightSeed(), each fight's start (sub_823DF800, which starts the fight's task: fight.cpp) seeds
// all four generators from that seed, and sub_82318730 takes the fight's generator as it would online.
#include "game/random.h"

#include "config.h"
#include "guest_abi.h"
#include "kernel/threads.h"
#include "log.h"
#include "netplay/frame.h"
#include "netplay/state_census.h"

#include <atomic>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <string>

extern "C" PPC_FUNC(__imp__sub_824061A0);
extern "C" PPC_FUNC(__imp__sub_8276B5F0);
extern "C" PPC_FUNC(__imp__sub_8276B6A8);
extern "C" PPC_FUNC(__imp__sub_8276B770);
extern "C" PPC_FUNC(__imp__sub_8276B818);
extern "C" PPC_FUNC(__imp__sub_8276B8F8);
extern "C" PPC_FUNC(__imp__sub_823415D8);
extern "C" PPC_FUNC(__imp__sub_82342668);
extern "C" PPC_FUNC(__imp__sub_8241DFF8);
extern "C" PPC_FUNC(__imp__sub_82540B80);

namespace
{
    constexpr uint32_t kMainThread = 0x827929B0; // the main thread's start address
    constexpr uint32_t kShared = 0x8357B5C0, kFight = 0x833F7600, kGenerator8321B198 = 0x8321B198, kGenerator83420020 = 0x83420020;
    constexpr uint32_t kGenerators[] = { kShared, kFight, kGenerator8321B198, kGenerator83420020 };
    constexpr uint32_t kStateSize = 2504;        // the words, the next word's address and the words left
    constexpr uint32_t kWordsLeft = 2500;        // a generator's words left before it regenerates
    constexpr float kScale = 0x1p-32f;           // the shared functions' scale from a word to [0, 1) (0x822BD544)
    constexpr float kFightScale = 0x1p-31f;      // sub_82318730's from the fight's 31 bits (0x822BD8E0)
    constexpr uint64_t kGamma = 0x9E3779B97F4A7C15ull;

    // SplitMix64's finalizer: a stream's words are Mix(key + n * kGamma).
    uint64_t Mix(uint64_t x)
    {
        x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
        x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
        return x ^ (x >> 31);
    }

    bool OnMainThread()
    {
        const kernel::GuestThread* thread = kernel::CurrentThread();
        return thread && thread->StartAddress() == kMainThread;
    }

    // DEBUG_RANDOM_LOG=first,count.
    struct LogSettings
    {
        uint64_t first = 0, count = 0;
        LogSettings()
        {
            if (const char* setting = std::getenv(GAME_PREFIX "_DEBUG_RANDOM_LOG"))
            {
                char* end = nullptr;
                first = strtoull(setting, &end, 10);
                count = end && *end == ',' ? strtoull(end + 1, nullptr, 10) : 600;
            }
        }
    };
    const LogSettings g_logSettings;
    std::mutex g_logLock;
    FILE* g_log = nullptr;                       // open within the window
    std::atomic<uint64_t> g_otherDraws{ 0 };     // other threads' draws since the last boundary

    // A main thread's draw into the log: the generator (a letter), the return address and the number.
    void LogDraw(char generator, uint32_t site, uint32_t number)
    {
        if (!g_log || !OnMainThread())
            return;
        std::lock_guard lock(g_logLock);
        if (g_log)
            fprintf(g_log, "  %c %08X %08X\n", generator, site, number);
    }

    void NoteDraw()
    {
        if (netplay::census::Active())
            netplay::census::NoteRead("random numbers 8357B5C0 (shared)");
    }

    // Calls a function that draws from `generator` inline, and with a census or the log running reports its draws (fewer
    // than the generator's 624 words: a regeneration restores the words left to 624).
    void CountInline(PPCFunc* function, uint32_t address, PPCContext& ctx, uint8_t* base, uint32_t generator, char letter,
        const char* name)
    {
        if (!netplay::census::Active() && !g_log)
        {
            function(ctx, base);
            return;
        }
        const uint32_t before = guest::LoadU32(generator + kWordsLeft);
        function(ctx, base);
        const uint32_t after = guest::LoadU32(generator + kWordsLeft);
        if (const uint32_t drawn = (before + 624 - after) % 624)
        {
            if (netplay::census::Active())
                netplay::census::NoteRead(name, drawn);
            LogDraw(letter, address, drawn);
        }
    }

    // Worker draws made while the main thread wasn't running the frame's update (game::SetInUpdate): a draw that could
    // fall on either side of a frame boundary, and so in another frame's stream on another machine.
    std::atomic<bool> g_inUpdate{ false };
    std::atomic<uint64_t> g_drawsOutsideUpdate{ 0 };

    // With SplitRandom on a thread other than the main thread: the draw's word, from the thread's stream for this frame.
    // A stream is keyed by what tells the thread apart the same way on every PC: its start address and start context,
    // image addresses, and the threads made before it with the same two (GuestThread::Instance; thread IDs count every
    // thread, the runtime's too, in the order they happen to start). And by the frame: in a fight its frame
    // (netplay::FightFrame), which both machines count from the fight's first frame past loading, outside one the
    // game's frame count, which differs between machines. A stream restarts at every frame boundary (netplay::Boundaries,
    // which a sync test's load doesn't rewind), where the game's threads are blocked in their waits in a session
    // (netplay/frame.h), so each frame's draws come from that frame's stream on both machines.
    // An effect's setup on the main thread (EffectSetup), whose draws come from a stream of their own with SplitRandom.
    thread_local int t_effectSetup = 0;
    std::atomic<uint64_t> g_effectSetupDraws{ 0 };
    // A player's timeline restarting (sub_823740D0: its record), whose draw is a word of its own with SplitRandom.
    thread_local uint32_t t_timeline = 0;

    std::optional<uint32_t> OwnWord()
    {
        if (!netplay::SplitRandom())
            return std::nullopt;
        if (t_effectSetup)
            return uint32_t(Mix(0x5E7EFFEC75E7ull + (g_effectSetupDraws.fetch_add(1, std::memory_order_relaxed) + 1) * kGamma) >> 32);
        if (t_timeline)
            return uint32_t(Mix(0x7131E11E7Eull ^ t_timeline ^ uint64_t(netplay::FightSeed().value_or(0)) << 32) >> 32);
        const kernel::GuestThread* thread = kernel::CurrentThread();
        if (!thread || thread->StartAddress() == kMainThread)
            return std::nullopt;
        thread_local uint64_t boundary = ~0ull, key = 0, drawn = 0;
        if (const uint64_t now = netplay::Boundaries(); now != boundary)
        {
            boundary = now;
            const std::optional<uint64_t> fightFrame = netplay::FightFrame();
            const uint64_t frame = fightFrame ? *fightFrame | 1ull << 63 : netplay::Frame(); // fight frames key streams of their own
            const uint64_t identity = Mix(Mix(uint64_t(thread->StartAddress()) << 32 | thread->StartContext()) ^ thread->Instance());
            key = Mix(identity ^ frame);
            drawn = 0;
        }
        if (g_log)
            g_otherDraws.fetch_add(1, std::memory_order_relaxed);
        if (!g_inUpdate.load(std::memory_order_relaxed) && g_drawsOutsideUpdate.fetch_add(1, std::memory_order_relaxed) == 0)
            LOG_WARN("Random numbers: thread %08X (context %08X) drew outside the frame's update, at frame %llu: its draws may "
                     "fall in another frame on another machine", thread->StartAddress(), thread->StartContext(),
                (unsigned long long)netplay::Frame());
        return uint32_t(Mix(key + ++drawn * kGamma) >> 32);
    }

    // sub_82318730's draws from the shared generator, which it takes from the fight's in an online fight while the fight
    // runs (0x830AD0E4 is 1, and the byte at 0x82FCC952 at least 1): with FightSeed, from the fight's in every fight.
    bool FightsOwn(uint32_t site)
    {
        switch (site)
        {
        case 0x82318CA8: case 0x82318CB0: case 0x82318CB8:
        case 0x8231910C: case 0x82319114: case 0x8231911C: case 0x82319124:
            break;
        default:
            return false;
        }
        return netplay::FightSeed() && guest::LoadU32(0x830AD0E4) == 1 && *guest::ToHost(0x82FCC952) >= 1;
    }

    uint64_t HashState(uint32_t generator)
    {
        uint64_t hash = 0xCBF29CE484222325ull; // FNV-1a
        const uint8_t* bytes = guest::ToHost(generator);
        for (uint32_t i = 0; i < kStateSize; i++)
            hash = (hash ^ bytes[i]) * 0x100000001B3ull;
        return hash;
    }
}

namespace game
{
    void SetInUpdate(bool inUpdate)
    {
        g_inUpdate.store(inUpdate, std::memory_order_relaxed);
    }

    void RandomLogFrame(uint64_t frame)
    {
        const LogSettings& settings = g_logSettings;
        if (!settings.count || frame < settings.first)
            return;
        std::lock_guard lock(g_logLock);
        if (frame == settings.first)
        {
            const std::string path = config::DumpPath("random_log.txt");
            g_log = fopen(path.c_str(), "w");
            g_otherDraws = 0;
            LOG_INFO("Random log: from frame %llu, for %llu frames, in %s", (unsigned long long)frame,
                (unsigned long long)settings.count, path.c_str());
        }
        if (!g_log)
            return;
        if (frame >= settings.first + settings.count)
        {
            fclose(g_log);
            g_log = nullptr;
            LOG_INFO("Random log: done");
            return;
        }
        // The frame, the generators' states, other threads' draws, the fight's main task's phase and next phase, the
        // fight's frame counter (0x830AD024), and netplay's fight frame (-1 outside a fight).
        const std::optional<uint64_t> fightFrame = netplay::FightFrame();
        fprintf(g_log, "F %llu A %016llX C %016llX B %016llX D %016llX other threads %llu phase %u/%u counter %u fight frame %lld\n",
            (unsigned long long)frame, (unsigned long long)HashState(kShared), (unsigned long long)HashState(kFight),
            (unsigned long long)HashState(kGenerator8321B198), (unsigned long long)HashState(kGenerator83420020),
            (unsigned long long)g_otherDraws.exchange(0), *guest::ToHost(0x82FCC952), *guest::ToHost(0x830ACF85),
            guest::LoadU32(0x830AD024) >> 16, fightFrame ? (long long)*fightFrame : -1ll);
    }

    void SeedGenerators(PPCContext& ctx, uint8_t* base, uint64_t seed)
    {
        const PPCRegister r3 = ctx.r3, r4 = ctx.r4;
        for (uint32_t generator : kGenerators)
        {
            ctx.r3.u64 = generator;
            ctx.r4.u64 = uint32_t(Mix(seed ^ generator));
            sub_82334C60(ctx, base);
        }
        ctx.r3 = r3;
        ctx.r4 = r4;
    }

    void SeedRandomNumbers(PPCContext& ctx, uint8_t* base)
    {
        const std::optional<uint32_t> seed = netplay::FightSeed();
        if (!seed)
            return;
        SeedGenerators(ctx, base, uint64_t(*seed) << 32);
        LOG_INFO("Random numbers: seeded from %u as the fight starts, at frame %llu", *seed, (unsigned long long)netplay::Frame());
    }

    void SeedFirstFrame(PPCContext& ctx, uint8_t* base, uint64_t seed)
    {
        SeedGenerators(ctx, base, seed);
        LOG_INFO("Random numbers: seeded again at the fight's first frame past its loading, frame %llu", (unsigned long long)netplay::Frame());
    }
}

// An effect's setup, which runs once its files have streamed in: sub_826591E8 fills a 128x128 noise texture with random
// words (sub_827187B8, 16,384 draws) and two tables of 301 random words and floats (sub_82645F40, 602), and sub_826450F8
// fills those tables too. Streaming finishes frames later on a slower disk: on Seaside Market the noise texture was
// filled at fight frame 340 on one machine and 1171 on another whose reads were slowed, and every number the shared
// generator gave after that differed (ANALYSIS.md, Phase 6). With SplitRandom, their draws come from a stream of their
// own, so the shared generator's sequence doesn't depend on when the files come.
template <PPCFunc* Original>
void EffectSetup(PPCContext& ctx, uint8_t* base)
{
    t_effectSetup++;
    Original(ctx, base);
    t_effectSetup--;
}

extern "C" PPC_FUNC(__imp__sub_826591E8);
extern "C" PPC_FUNC(__imp__sub_826450F8);
extern "C" PPC_FUNC(__imp__sub_823740D0);

PPC_FUNC(sub_826591E8)
{
    EffectSetup<__imp__sub_826591E8>(ctx, base);
}

// A player's timeline (sub_82373980's, records at 0x8302BC98, 580 bytes a player) restarts, drawing when it next begins
// (60 frames on, plus up to 480 more from the shared generator: +540). A fight's setup restarts both fighters' as their
// files come in (sub_823FBE18, each frame of the update, through sub_823FE150, sub_82354F80 and sub_82374548), and so does
// character select's for its previews: on a slower disk the fight's came a frame later, after the stage's update had
// drawn its numbers in that frame rather than before, and the shared generator differed from the fight's first frame
// (ANALYSIS.md, Phase 6). With SplitRandom the draw is a word of its own, the same for a player's timeline whenever it
// restarts (from the record and the fight seed), and the shared generator's sequence doesn't depend on when files come.
PPC_FUNC(sub_823740D0)
{
    t_timeline = ctx.r3.u32;
    __imp__sub_823740D0(ctx, base);
    t_timeline = 0;
}

PPC_FUNC(sub_826450F8)
{
    EffectSetup<__imp__sub_826450F8>(ctx, base);
}

PPC_FUNC(sub_824061A0)
{
    const uint32_t site = uint32_t(ctx.lr);
    if (netplay::census::Active())
        netplay::census::NoteRead("random numbers 833F7600 (the fight's)");
    __imp__sub_824061A0(ctx, base);
    LogDraw('C', site, ctx.r3.u32);
}

PPC_FUNC(sub_823415D8)
{
    CountInline(__imp__sub_823415D8, 0x823415D8, ctx, base, kGenerator8321B198, 'B', "random numbers 8321B198");
}

PPC_FUNC(sub_82342668)
{
    CountInline(__imp__sub_82342668, 0x82342668, ctx, base, kGenerator8321B198, 'B', "random numbers 8321B198");
}

PPC_FUNC(sub_8241DFF8)
{
    CountInline(__imp__sub_8241DFF8, 0x8241DFF8, ctx, base, kGenerator83420020, 'D', "random numbers 83420020");
}

PPC_FUNC(sub_82540B80)
{
    CountInline(__imp__sub_82540B80, 0x82540B80, ctx, base, kGenerator83420020, 'D', "random numbers 83420020");
}

// A word times the float at r3, scaled to [0, r3).
PPC_FUNC(sub_8276B5F0)
{
    const uint32_t site = uint32_t(ctx.lr);
    NoteDraw();
    if (const std::optional<uint32_t> word = OwnWord())
    {
        const float scale = std::bit_cast<float>(guest::LoadU32(ctx.r3.u32));
        ctx.f1.f64 = float(float(*word) * scale) * kScale;
        return;
    }
    __imp__sub_8276B5F0(ctx, base);
    LogDraw('A', site, std::bit_cast<uint32_t>(float(ctx.f1.f64)));
}

// A float in [0, 1).
PPC_FUNC(sub_8276B6A8)
{
    const uint32_t site = uint32_t(ctx.lr);
    if (FightsOwn(site))
    {
        sub_824061A0(ctx, base); // as sub_82318730 draws online: 31 bits, scaled to [0, 1)
        ctx.f1.f64 = float(int32_t(ctx.r3.u32)) * kFightScale;
        return;
    }
    NoteDraw();
    if (const std::optional<uint32_t> word = OwnWord())
    {
        ctx.f1.f64 = float(*word) * kScale;
        return;
    }
    __imp__sub_8276B6A8(ctx, base);
    LogDraw('A', site, std::bit_cast<uint32_t>(float(ctx.f1.f64)));
}

// A word.
PPC_FUNC(sub_8276B770)
{
    const uint32_t site = uint32_t(ctx.lr);
    NoteDraw();
    if (const std::optional<uint32_t> word = OwnWord())
    {
        ctx.r3.u64 = *word;
        return;
    }
    __imp__sub_8276B770(ctx, base);
    LogDraw('A', site, ctx.r3.u32);
}

// An integer in [0, the word at r3].
PPC_FUNC(sub_8276B818)
{
    const uint32_t site = uint32_t(ctx.lr);
    NoteDraw();
    if (const std::optional<uint32_t> word = OwnWord())
    {
        const float range = float(guest::LoadU32(ctx.r3.u32) + 1);
        ctx.r3.u64 = uint32_t(int64_t(float(float(*word) * range) * kScale));
        return;
    }
    __imp__sub_8276B818(ctx, base);
    LogDraw('A', site, ctx.r3.u32);
}

// An integer in [0, the word at r3).
PPC_FUNC(sub_8276B8F8)
{
    const uint32_t site = uint32_t(ctx.lr);
    NoteDraw();
    if (const std::optional<uint32_t> word = OwnWord())
    {
        const float range = float(guest::LoadU32(ctx.r3.u32));
        ctx.r3.u64 = uint32_t(int64_t(float(float(*word) * range) * kScale));
        return;
    }
    __imp__sub_8276B8F8(ctx, base);
    LogDraw('A', site, ctx.r3.u32);
}
