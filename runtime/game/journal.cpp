// DOA4's calls into the libraries whose state stays outside savestates, journaled for replays (runtime/netplay/replay.h:
// recorded in a frame's first run, answered from the record in its replay instead of being made again).
//
// The audio. The game plays everything through a sound engine of its own on top of XACT and XAudio: one object, 0x8319F9D0
// in .data (its pointer at 0x831A7AC8), constructed by sub_8276C2E8 and set up at boot by sub_8276CEA8 (XAudio, then XACT
// with D:\Sound\doa4_sound.xgs, then the engine's own sub_8276C450). Its code lies at 0x8276C450-0x82774000 and
// 0x827757C8-0x82781900, among the game's own containers and drawing helpers (whose objects are elsewhere: 0x8357xxxx,
// the game heap). Its parts: the category and volume manager at [engine+96] (0x831A4EC0, a critical section at +16) and
// the list of sounds at [engine+104] (0x831A3000: records with an ID at +32, a state at +40, volumes at +48 and +52;
// a critical section at +7824). Its vtable (0x822BAF28) has seven methods: +8 plays a sound (sub_82771EB0, which
// returns its ID), +12 is its frame (sub_8276D0C8), which the game's audio thread (825414B0) runs each time XAudio's
// render driver asks for a frame, keeping the engine's clock (QueryPerformanceCounter at +0x58).
// The game's side is its sound code at 0x82541000-0x82546000 and its callers: it fills parameter blocks on its stack
// (sub_8276D6E8, sub_827797E0, which touch nothing else) and calls the engine: a sound effect or voice through the
// engine's +8 (sub_825440E0), the system voice and the announcer through sub_82775F40 (which prepares a stream from a
// wave bank, through sub_82778B08 and sub_82775A10: a buffer from the memory pool at 0x82F52800, sub_8277C718, and
// XACT's sub_82986C48, which reads the wave bank's header on the calling thread), fades (sub_82777288, sub_8277BB60),
// the categories' volumes (sub_8277BC78, about 195 calls as each fight starts and as its replay ends), stops by category
// (sub_827771D0, sub_8277B888), the sounds playing in a category (sub_82779498, each frame from sub_82541538, and
// sub_82777348 for the voices' questions, fight.cpp); and it calls XAudio's voices (sub_82966FD8) and XACT's cues and
// waves itself: prepares and plays (sub_82541A90 and sub_82541B08: XACT's sub_82986C18 and sub_82986BE8), and releases
// them through their vtables (sub_82541B80 at 0x82541C1C, sub_82541C38). In a fight (the scripted versus fight, 2,800
// fight frames from its start): one query a frame, two parameter blocks and one play a sound (about 200), ten voices
// with their streams, 32 fades, about 600 volume calls at the fight's transitions, a handful of stops.
// What each side keeps: the game's (in its savestate) the sounds' IDs and its cues' pointers, in its own records and the
// sound code's tables (0x8310B8xx, the players' lists at 0x830BD520); the engine (outside: its object, its sounds,
// XACT's cues and wave banks, XAudio's voices, all written by its own code and the audio thread too) everything else, on
// its own time. So after a load the game holds IDs and pointers from the save, while the engine has moved on: the first
// run started sounds (a replay would play them again and get other IDs), prepared streams (a replay found the KO
// replay's announcer prepared already, and its frame went another way), and released cues (a replay released them
// again, through a stale pointer: the story mode crash in sub_82992DF8, called from 0x82541C1C).
//
// Journaled, then: the game's calls (from its code, on the main thread, outside netplay's frame boundary and not from
// inside another journaled call) into
// - the sound engine, when the object (r3) is the engine or one of its two parts;
// - XAudio and XMA (0x82966F70-0x82972000) and XACT (0x82986000-0x82996000): their entry points, and their functions
//   in vtables (the cues' and waves' methods);
// - CRI's file system and movies (0x829B9000-0x829FD000), through which the game's file loader (sub_82401CD8) reads
//   and polls: a read the first run made isn't made again, and the polls get the first run's answers;
// - the asynchronous file operations (sub_82731790 starts one on a thread of its own, 82731758, sub_82731830 polls it),
//   through which the profile is saved as a fight starts: a replay doesn't start a second save;
// - XAPI's physical memory (XPhysicalAlloc and XPhysicalFree, heap.cpp), whose blocks hold the models the game loads
//   and releases: a replay gets the blocks its first run did, and frees none a second time (the frees are deferred).
// tools/gen_journal_hooks.py lists the functions (journal_hooks.inc). The game's network calls into XHV and XAudio from
// 0x8243xxxx-0x8245xxxx are journaled too (the same entry points): their state is outside savestates as well.
//
// For replays with other input than the first run's (rollback), the journal compares calls by the argument registers each
// function reads (journal_arguments.inc, from the same tool's liveness analysis), and tells it more of two:
// - A sound (the engine's play, sub_82771EB0, the vtable's +8: r3 the engine, r4 its parameters, 544 bytes the game's
//   sub_825440E0 fills on its stack after sub_8276D6E8's defaults). The engine makes a node for it in the category
//   manager's list ([engine+96] = 0x831A4EC0: the list from +12, next at +288; a node's ID at +16, its sound number +20,
//   category +24, state +28: 1 free, 2 made, 3 playing, 4 stopping; +32 2 for the sound, 3 for a send to the stage's
//   reverb, which play makes beside it when [0x8310B830] is set) and returns its ID: the counter at +4 of the manager
//   (0x831A4EC4), which play increments, outside savestates (the engine's .data, data_owners.inc), so a replay's new
//   sounds never get an ID the game has seen. The parameters that make it the sound it is: its number, the banks and the
//   cue (+0-+19), its priority (+148) and category (+228). In fights the game drops the ID (motion scripts, the voices,
//   the emitters). A sound the replay plays within 3 frames of the abandoned timeline's is that sound, and a sound the
//   replay doesn't play is faded out over 3 frames and stopped (sub_8277BB60 on its ID, mode 3: as the game stops its
//   sounds), rather than left to play out: a hit's sound or a voice for a hit that didn't happen. A one-shot that has
//   ended has no node left, and the fade does nothing.
// - A stream (sub_82775F40: the system voice and the announcer, prepared from a wave bank; its parameters' +528 the sound
//   number, the stream's ID, +544 the category, +564 the priority): one the replay doesn't prepare is stopped
//   (sub_827771D0, mode 4: the streams of that sound number), unless the new timeline prepared that sound too.
// The other calls in a fight (the per-frame query of the streams sub_82779498, fades, the categories' volumes, stops by
// category) can't be undone, and needn't be for the fight: none of the engine's answers reach its simulation, but the
// voices' waits, which NoVoiceWaits answers (fight.cpp).
#include "game/journal.h"

#include "guest_abi.h"
#include "kernel/threads.h"
#include "log.h"
#include "netplay/frame.h"

#include <algorithm>
#include <cstring>

namespace
{
    constexpr uint32_t kMainThread = 0x827929B0;
    constexpr uint32_t kEnginePointer = 0x831A7AC8;
    constexpr uint32_t kAsyncStart = 0x82731790, kAsyncPoll = 0x82731830;
    constexpr uint32_t kPlay = 0x82771EB0, kPrepareStream = 0x82775F40;
    constexpr uint32_t kFadeOut = 0x8277BB60;    // (manager, value, value, mode, frames): fades out the matching nodes, then stops them
    constexpr uint32_t kStopStreams = 0x827771D0; // (engine, value, value, mode): stops the matching streams
    constexpr uint32_t kFadeFrames = 3;
    constexpr uint32_t kNearFrames = 3;
    constexpr uint32_t kContentObject = 512; // XAPI's content object's bytes, its root's name at +490 the last

    bool InEngine(uint32_t address)
    {
        return (address >= 0x8276C450 && address < 0x82774000) || (address >= 0x827757C8 && address < 0x82781900);
    }

    // The libraries: D3D and the sound engine's region, XAPI, XNet and the others up to the C runtime's end. The game's
    // code is the rest: before D3D, the pad library's functions between the sound engine's parts, and after the C
    // runtime (the movie player's and other code at 0x82A10000-0x82B60000).
    bool GameCode(uint32_t address)
    {
        if (address < 0x822F0000 || address >= 0x82B60000)
            return false;
        if (address >= 0x8274C280 && address < 0x82A10000)
            return (address >= 0x8276B5F0 && address < 0x8276C450) || (address >= 0x82774000 && address < 0x827757C8);
        return true;
    }

    bool IsEngineObject(uint32_t object)
    {
        const uint32_t engine = guest::LoadU32(kEnginePointer);
        return engine && (object == engine || object == guest::LoadU32(engine + 96) || object == guest::LoadU32(engine + 104));
    }

    uint32_t BigEndian32(const uint8_t* bytes)
    {
        return uint32_t(bytes[0]) << 24 | uint32_t(bytes[1]) << 16 | uint32_t(bytes[2]) << 8 | bytes[3];
    }

    // Whether the category manager's list holds a send node with this ID: the send of the sound played before, which
    // took the counter's value without incrementing it, so that stopping the ID stops that send too (logged once).
    bool SendWithId(uint32_t manager, uint32_t id)
    {
        uint32_t node = guest::LoadU32(manager + 12);
        for (int i = 0; node && i < 512 && guest::IsAllocated(node); i++)
        {
            if (guest::LoadU32(node + 16) == id && guest::LoadU32(node + 32) == 3)
                return true;
            node = guest::LoadU32(node + 288);
        }
        return false;
    }

    // A sound an abandoned timeline started that the new one didn't: faded out and stopped (journal.cpp's top).
    void UndoPlay(PPCContext& ctx, uint8_t* base, const netplay::journal::CallRecord& call, const std::vector<netplay::journal::CallRecord>&)
    {
        (void)base;
        const uint32_t id = uint32_t(call.result);
        const uint32_t engine = guest::LoadU32(kEnginePointer);
        if (id == 0xFFFFFFFFu || id == 0x80004005u || !engine)
            return;
        const uint32_t manager = guest::LoadU32(engine + 96);
        if (SendWithId(manager, id))
        {
            static bool logged = false;
            if (!logged)
            {
                logged = true;
                LOG_INFO("Journal: stopping sound %08X of an abandoned timeline also stops the reverb send of the sound before it (logged once)", id);
            }
        }
        kernel::CallGuest(ctx, kFadeOut, { manager, id, 0, 3, kFadeFrames });
    }

    // A stream an abandoned timeline prepared that the new one didn't: stopped, unless the new timeline holds one of
    // the same sound (stopping the sound number would stop it too).
    void UndoStream(PPCContext& ctx, uint8_t* base, const netplay::journal::CallRecord& call,
        const std::vector<netplay::journal::CallRecord>& current)
    {
        (void)base;
        const uint32_t engine = guest::LoadU32(kEnginePointer);
        if (!engine || call.blockSize < 4)
            return;
        const uint32_t number = BigEndian32(call.block);
        for (const netplay::journal::CallRecord& other : current)
        {
            if (other.function == kPrepareStream && other.blockSize >= 4 && BigEndian32(other.block) == number)
                return;
        }
        kernel::CallGuest(ctx, kStopStreams, { engine, number, 0, 4 });
    }

    // The functions the journal sees, described for it (netplay::journal::Describe): the argument registers each reads
    // (journal_arguments.inc), and the sounds' and streams' parameters, matching and undoing.
    [[maybe_unused]] const bool g_described = [] {
        static constexpr std::pair<uint32_t, uint32_t> kArguments[] = {
#include "game/journal_arguments.inc"
        };
        std::vector<netplay::journal::FunctionInfo> functions;
        functions.reserve(std::size(kArguments));
        for (const auto& [address, arguments] : kArguments)
        {
            netplay::journal::FunctionInfo& info = functions.emplace_back();
            info.address = address;
            info.arguments = arguments;
            if (address == kPlay)
            {
                info.blockRegister = 4;
                info.blockFields = { { 0, 20 }, { 148, 4 }, { 228, 4 } };
                info.nearFrames = kNearFrames;
                info.undo = UndoPlay;
                info.name = "a sound";
            }
            else if (address == kPrepareStream)
            {
                info.blockRegister = 4;
                info.blockFields = { { 528, 4 }, { 544, 4 }, { 564, 4 } };
                info.nearFrames = kNearFrames;
                info.undo = UndoStream;
                info.name = "a stream";
            }
            else if (address >= 0x82769C88 && address <= 0x8276A6F0)
            {
                // XAPI's content objects (above): the object, and sub_8276A060's copy of its data (308 bytes at r4).
                info.objects = { { 3, kContentObject } };
                if (address == 0x8276A060)
                    info.objects.emplace_back(4, 308);
                info.name = "a content object's";
            }
            // Queries, which change nothing but their answer: the sounds and streams playing in a category (each frame,
            // and the voices' waits), the playing stream's position (sub_827718B8, which story mode's dialogue asks each
            // frame), the sounds in a range of categories (sub_8277BEA8, into the caller's buffer: the faces'
            // timelines), and the asynchronous operation's poll.
            info.query = address == 0x82779498 || address == 0x82777348 || address == 0x827718B8 || address == 0x8277BEA8 ||
                address == kAsyncPoll;
        }
        netplay::journal::Describe(std::move(functions));
        return true;
    }();
}

namespace game
{
    std::vector<uint32_t> SoundsPlaying()
    {
        std::vector<uint32_t> sounds;
        const uint32_t engine = guest::LoadU32(kEnginePointer);
        if (!engine)
            return sounds;
        // The sounds: the category manager's nodes, made or playing (not those stopping), but sends.
        uint32_t node = guest::LoadU32(guest::LoadU32(engine + 96) + 12);
        for (int i = 0; node && i < 512 && guest::IsAllocated(node) && guest::IsAllocated(node + 291); i++)
        {
            const uint32_t state = guest::LoadU32(node + 28);
            if ((state == 2 || state == 3) && guest::LoadU32(node + 32) == 2)
                sounds.push_back(guest::LoadU32(node + 20));
            node = guest::LoadU32(node + 288);
        }
        // The streams: the eight records from [engine+104] + 16, 976 bytes each, in use and prepared or playing.
        const uint32_t streams = guest::LoadU32(engine + 104);
        for (uint32_t i = 0; streams && i < 8; i++)
        {
            const uint32_t record = streams + 16 + 976 * i;
            const uint32_t state = guest::LoadU32(record + 40);
            if (guest::LoadU32(record + 16) == 1 && (state == 3 || state == 4))
                sounds.push_back(0x80000000u | guest::LoadU32(record + 32));
        }
        std::sort(sounds.begin(), sounds.end());
        return sounds;
    }

    bool Journaled(uint32_t function, const PPCContext& ctx)
    {
        if (netplay::BoundaryContext() || !GameCode(uint32_t(ctx.lr)))
            return false;
        const kernel::GuestThread* thread = kernel::CurrentThread();
        if (!thread || thread->StartAddress() != kMainThread)
            return false;
        if (InEngine(function))
            return IsEngineObject(ctx.r3.u32);
        return true; // XAudio, XACT, CRI, the asynchronous file operations, physical memory
    }
}

#define JOURNALED(address)                                                                \
    extern "C" PPC_FUNC(__imp__sub_##address);                                            \
    PPC_FUNC(sub_##address)                                                               \
    {                                                                                     \
        game::JournalCall(0x##address, __imp__sub_##address, ctx, base);                  \
    }

#include "game/journal_hooks.inc"

// The asynchronous file operations (netplay_state.cpp): sub_82731790 starts one (the save's content and file
// operations) on a thread of its own, its state at 0x82E476A0, and sub_82731830 polls it.
JOURNALED(82731790)
JOURNALED(82731830)

// XAPI's content objects (0x82769C88-0x8276A6F0: r3 an object with its state at +0, an overlapped at +24, an event at
// +36, the content's data from +128 and its root's name at +490), through which the profile manager (sub_8230DCD8)
// creates the save's packages (sub_82769F58: XamContentCreate, mode 2 for ups.bak, which deletes the package and makes it
// anew) before starting the asynchronous operation that writes them, and closes them; the menus enumerate and open
// content too. Made again in a replay across a save's start (a match's end), they created ups.bak a second time while the
// first run's operation wrote it. Journaled, with the object's bytes they change (kContentObject).
JOURNALED(82769C88)
JOURNALED(82769D90)
JOURNALED(82769E08)
JOURNALED(82769EA8)
JOURNALED(82769F58)
JOURNALED(8276A060)
JOURNALED(8276A108)
JOURNALED(8276A190)
JOURNALED(8276A248)
JOURNALED(8276A438)
JOURNALED(8276A508)

// The game's releases of XACT's cues (sub_82541B80) and waves (sub_82541C38): under the engine's lock, each clears the
// engine's records of it (+504 of those whose +496 or +500 is it) and calls its release through its vtable (+0, +16).
// The fight's per-player setup (sub_823FBE18, through sub_823FFFA0) releases the intro's voices as the intro ends,
// and a replay from before then released them again, through pointers to freed objects (story mode's crashes in
// sub_82992DF8 and sub_82541C38). Deferred while the journal records or replays, until the frames are past rolling
// back.
#define DEFERRED(address)                                                               \
    extern "C" PPC_FUNC(__imp__sub_##address);                                          \
    PPC_FUNC(sub_##address)                                                             \
    {                                                                                   \
        game::DeferCall(0x##address, __imp__sub_##address, ctx, base);                  \
    }

DEFERRED(82541B80)
DEFERRED(82541C38)
