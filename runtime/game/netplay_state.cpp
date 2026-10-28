// DOA4's state for netplay (runtime/netplay/game_state.h): whose writes make up the state a savestate holds, what
// differs between runs of the same frames without changing them, and the fibers' host state.
#include "game/fibers.h"
#include "game/fight.h"
#include "game/journal.h"
#include "game/random.h"
#include "gpu/command_processor.h"
#include "guest_abi.h"
#include "kernel/threads.h"
#include "log.h"
#include "netplay/frame.h"
#include "netplay/game_state.h"
#include "netplay/heaps.h"
#include "netplay/replay.h"
#include "netplay/rollback_test.h"
#include "netplay/session.h"

#include <algorithm>
#include <atomic>
#include <bitset>
#include <map>
#include <memory>

#include <windows.h>

namespace
{
    bool IsGameThread(uint32_t startAddress)
    {
        switch (startAddress)
        {
        case 0x827929B0: // the main thread
        case 0x82370638: // worker pools: CPU skinning (five threads)
        case 0x82573A40: // five threads
        case 0x8234A160: // four
        case 0x82374990: // four
        case 0x825689E8: // five
        case 0x82573958:
            return true;
        default:
            return false;
        }
    }

    // The game's file loader (sub_82401CD8, each frame after the tasks) reads a request at a time through CRI's file
    // system, the read's handle at 0x830B679C, and polls the read each frame (sub_824026E8, through CRI's status
    // sub_829BB570: 3 once read, 4 failed), so a load is done in the frame each machine's disk finishes it in. In a fight,
    // the match's result menu waits so for its sprites (spr.afs, sub_8238A1D8, sub_8238A020): with every read 20 ms slower
    // on one machine its menu opened 23 frames later, and the same presses chose Again on one machine and Quit on the
    // other. In a fight a session plays in lockstep, its boundary waits for the read, asking CRI's status as the game
    // does (which runs CRI's server, sub_829BCD60), so that the game finds it done in the same frame on both machines.
    // CRI's read of a file the game's loader opened (sub_829BB3C8 from 0x824026B4: the handle, the sectors, the
    // destination), which the loader polls each frame (sub_824026E8).
    constexpr uint32_t kLoaderRead = 0x829BB3C8;

    bool LoaderReading()
    {
        PPCContext* ctx = netplay::BoundaryContext();
        const std::optional<netplay::Segment> segment = netplay::CurrentSegment();
        if (!ctx || !segment || !segment->fight || ((!segment->flow || !netplay::session::PlaysFlow()) && !netplay::rollbacktest::Enabled()))
            return false;
        // Not in a replay: its calls into CRI are answered from the record, so CRI's handles are as the present has them,
        // and a read the replayed frame has under way won't move (story mode's replays waited out the 10 s limit at
        // each boundary). But for a read the replay started itself, a call the record didn't hold (a session's rollback,
        // whose replay runs other input: the match's result menu's sprites, which a press can ask for frames sooner),
        // which CRI runs for real, and whose polls in the frames after are made for real too: the game must find it done
        // in the frame after it started, as on the other machine.
        if (netplay::replay::Active() && !netplay::journal::MadeInReplay(kLoaderRead))
            return false;
        const uint32_t handle = guest::LoadU32(0x830B679C);
        if (!handle)
            return false;
        const uint64_t lr = ctx->lr;
        kernel::CallGuest(*ctx, 0x829BB570, { handle });
        ctx->lr = lr;
        const int32_t status = int32_t(ctx->r3.u32);
        return status >= 0 && status != 3 && status != 4;
    }

    // The game's asynchronous operations (sub_827315F8's, the save's content and file operations among them) each run on
    // a short-lived thread, 82731758, whose end the caller polls each frame (sub_82731830: WaitForSingleObject with no
    // wait). At a fight's start the fight waits for the profile's save that way (fight.cpp), so the frame its intro
    // starts at followed each machine's disk: with every write 100 ms slower on one machine, a session desynced at fight
    // frame 11. Running: its host thread not finished (started or about to), and not blocked in a wait. And in a fight in
    // lockstep, the file loader's read (LoaderReading).
    bool AsyncWorkRunning()
    {
        bool running = false;
        kernel::ForEachThread([&](kernel::GuestThread& thread) {
            if (!running && thread.StartAddress() == 0x82731758 && thread.waits.load(std::memory_order_acquire) == 0 && thread.NativeHandle() &&
                WaitForSingleObject(thread.NativeHandle(), 0) == WAIT_TIMEOUT)
                running = true;
        });
        return running || LoaderReading();
    }

    // The libraries the game's threads run whose memory stays out of savestates, by where their code lies. Between the
    // game's code and XAPI's come D3D (0x8274C280 on: device state, resources and command buffers) and the game's sound
    // engine (0x8276C450-0x8277E0C0, journal.cpp, with drawing helpers of the game's among it), with the game's math
    // and pad functions in between (0x82774000-0x827757C8, such as sub_82774D60, which reads the pads for sub_824F34D8)
    // that count as the game's; the sound engine's code goes on at 0x827757C8 (sub_82775F40, which plays the system
    // voice and the announcer, and the streams it prepares), then memory pools (0x8277C000 on: sub_8277C718, which the
    // sound engine's streams and the drawing take buffers from). From 0x8277E0C0 to 0x82781900 come the game's own
    // resources and containers (sub_8277EFC8 makes an object from loaded data, sub_8277FDD8 a vector's storage,
    // sub_8277F4C0 indexes one), whose objects the game's frames work on: the game's code, as are the stage's objects
    // the job thread makes as a stage loads (allocatesForGame). XAPI and the C runtime's helpers work for their
    // callers, but for XAPI's heap
    // (0x827944C0-0x82797728: RtlAllocateHeap, sub_827962D0, RtlFreeHeap, sub_82796BC0), which the audio thread
    // allocates from too: its bookkeeping isn't the game's alone, so loading it would roll back the audio thread's
    // allocations (the game heap has books of its own, netplay/heaps.h). After them: XNet, XAudio and XMA, voice, and
    // CRI (0x827E0000-0x829FD000, with operator new among them). From the census of a Sparring fight and the sync
    // tests (ANALYSIS.md, Phase 6); the calls each region makes and the pages it writes tell them apart, not symbols.
    bool IsLibraryFunction(uint32_t function)
    {
        return (function >= 0x8274C280 && function < 0x8276B5F0) || (function >= 0x8276C450 && function < 0x82774000) ||
            (function >= 0x827757C8 && function < 0x8277E0C0) || (function >= 0x827944C0 && function < 0x82797728) ||
            (function >= 0x827E0000 && function < 0x829FD000);
    }

    // D3D's vertex declarations (sub_823074E0, XMemAlloc with D3D's allocator ID): the game's models make them as they
    // load (sub_8276E548, on the job thread) and free them as they're released (sub_82784B88, through sub_823FC8B0 as
    // the story mode's intro ends): the game's, made and freed by its frames.
    bool MakesForGame(uint32_t function)
    {
        return function == 0x823074E0;
    }

    // D3D's device counts a reference to the vertex declaration it has set (SetVertexDeclaration, sub_82751E10: up
    // the new one's count at +4, down the one it held, which is freed at 0), and keeps it at +11408. The declarations
    // are the game's, counts and all (MakesForGame), so its savestates hold the device's one too: a load puts back the
    // one it held at the save, without counting (the counts are the save's). Otherwise the declaration the first run
    // left set would lose a count it never had: the versus fight's sync tests found two counts 3 higher in the replay.
    constexpr uint32_t kDeviceVertexDeclaration = 11408;
    std::atomic<uint32_t> g_device{ 0 }; // the device, from SetVertexDeclaration's first call

    netplay::HostState SaveHost(std::string& error)
    {
        netplay::HostState host = game::SaveFibers(error);
        const uint32_t device = g_device.load(std::memory_order_relaxed);
        if (!host.load || !device)
            return host;
        const uint32_t declaration = guest::LoadU32(device + kDeviceVertexDeclaration);
        host.load = [load = std::move(host.load), device, declaration](std::string* note) {
            std::string error = load(note);
            if (error.empty())
                guest::StoreU32(device + kDeviceVertexDeclaration, declaration);
            return error;
        };
        return host;
    }

    // The game's state in physical memory (XPhysicalAlloc's blocks, heap.cpp): the arenas the mode's start makes
    // (sub_82404E20: 274 MB in eleven blocks, which the game's own allocators divide), and a block for each of some files
    // (sub_823FC310 allocates one and has the file loader read the file there). Among the arenas:
    // - The ring the fight records its frames in for the KO's replay: the 0xF30000 bytes at [0x83035E74]. sub_823F8C00
    //   (from the fight's task, sub_824145C0) moves the replay's two samples through its 428-byte frames
    //   ([0x830AD820] and [0x830AD5C4]), and sub_823F9E48 places the players between them. A sync test across the
    //   replay's end differed from its first replayed frame: the frames after it go on writing the ring.
    // - The characters' models (91 MB from [0x83035E50]): XPR resource bundles the file loader reads there, whose offsets
    //   the job thread's sub_8276E430 makes pointers in place, and whose meshes' records the release (sub_823FC8B0,
    //   sub_8277E408) clears. A replay across the story mode's intro end (fight frame 1504, where the intro's models are
    //   released and the fight's loaded) released them a second time, from the cleared records.
    // The arenas also hold what the GPU writes (resolved render targets), which a load puts back too. Not the rest of
    // the game's blocks: the drawing's pool (sub_823BAAA8, 29 MB, rewritten each frame), and the blocks its sound code
    // reads XACT's settings into (sub_82541CF0, sub_825410E8). Putting back all of them crashed the audio thread in the
    // story mode's test, which freed a process heap block a second time.
    std::vector<std::pair<uint32_t, uint32_t>> PhysicalState()
    {
        std::vector<std::pair<uint32_t, uint32_t>> ranges;
        for (const netplay::heaps::PhysicalBlock& block : netplay::heaps::PhysicalBlocks())
        {
            if (block.function == 0x82404E20 || block.function == 0x823FC310)
                ranges.emplace_back(block.block, block.block + block.size);
        }
        return ranges;
    }

    // The job thread (sub_823FC5A0, waits.cpp) runs the jobs a stage's loading posts, which make the stage's objects
    // (sub_8277EFC8): the game's, though the thread isn't one of the game's frames' (its host loop waits outside the
    // guest's waits, so a frame boundary can't tell it's idle).
    bool AllocatesForGame(uint32_t startAddress)
    {
        return startAddress == 0x823FC5A0;
    }

    // The functions that allocate for their callers: XAPI's VirtualAlloc (0x82790220-0x8279026C), XMemAlloc and XMemFree
    // (0x827930E0-0x82793198), its heap with LocalAlloc and LocalFree (0x827944C0-0x82797728), operator new and new[]
    // (0x8299D170-0x8299D238, linked in among the audio library's code), and the C runtime (0x829FD000-0x82A10000: malloc,
    // free, realloc, operator delete).
    bool IsAllocatorFunction(uint32_t function)
    {
        return (function >= 0x82790220 && function < 0x8279026C) || (function >= 0x827930E0 && function < 0x82793198) ||
            (function >= 0x827944C0 && function < 0x82797728) || (function >= 0x8299D170 && function < 0x8299D238) ||
            (function >= 0x829FD000 && function < 0x82A10000);
    }

    // The image's data that's the game's state: the variables only the game's code writes or takes the address of, and
    // those only read that follow one of them (fields of the game's objects, written through pointers), from the code
    // that references them (tools/dataowners.py, which checks them against a state census). Pages holding only those are
    // saved whole, the others' bytes are masked. Less what the game shares with a thread of others: the job ring's
    // critical section (0x830ADBCC, 28 bytes: waits.cpp), which the job thread takes every few milliseconds on its own
    // time, so it may hold it at a frame boundary; and the asynchronous operation's state (0x82E476A0, 320 bytes: +4 and
    // +8 its result, +28 its thread's handle, +32 a name, +292 the function and its arguments), which sub_82731790 sets
    // as it starts one on a thread of its own (82731758, which writes the result), and sub_82731830 polls and clears as it
    // ends: the operation is outside savestates (the profile's save, at a fight's start and a match's end), and the
    // journal answers the start and the polls in a replay. Loaded with the game's state, a replay across the save's
    // start found no operation under way after the answered start (the call's writes to the image aren't recorded), and
    // the game started the save again: a rollback test at a match's end created ups.bak twice.
    struct ImageState
    {
        std::vector<std::pair<uint32_t, uint32_t>> pages; // [start, end), whole pages
        std::vector<std::pair<uint32_t, std::unique_ptr<std::bitset<4096>>>> partial;
    };

    const ImageState& GetImageState()
    {
        static const ImageState state = [] {
            static constexpr std::pair<uint32_t, uint32_t> kGameData[] = {
#include "game/data_owners.inc"
            };
            constexpr uint32_t kJobLock = 0x830ADBCC, kCriticalSectionSize = 28;
            std::map<uint32_t, std::bitset<4096>> masks; // page -> the game's bytes
            for (const auto& [start, end] : kGameData)
            {
                for (uint32_t address = start; address < end;)
                {
                    const uint32_t page = address & ~0xFFFu;
                    const uint32_t stop = std::min(end, page + 0x1000);
                    std::bitset<4096>& mask = masks[page];
                    for (uint32_t i = address - page; i < stop - page; i++)
                        mask.set(i);
                    address = stop;
                }
            }
            for (uint32_t address = kJobLock; address < kJobLock + kCriticalSectionSize; address++)
                masks[address & ~0xFFFu].reset(address & 0xFFF);
            constexpr uint32_t kAsyncOperation = 0x82E476A0, kAsyncOperationSize = 320;
            for (uint32_t address = kAsyncOperation; address < kAsyncOperation + kAsyncOperationSize; address++)
                masks[address & ~0xFFFu].reset(address & 0xFFF);
            ImageState state;
            for (const auto& [page, mask] : masks)
            {
                if (mask.all())
                {
                    if (!state.pages.empty() && state.pages.back().second == page)
                        state.pages.back().second = page + 0x1000;
                    else
                        state.pages.emplace_back(page, page + 0x1000);
                }
                else if (mask.any())
                    state.partial.emplace_back(page, std::make_unique<std::bitset<4096>>(mask));
            }
            return state;
        }();
        return state;
    }

    // All of the game's state in guest memory (GameState::savedMemory): the game heap's memory, the regions it reserved
    // through VirtualAlloc (0x3D035000-0x3FFDA000: the skinning's data among them), its threads' stacks and its task
    // fibers', its physical arenas and files' blocks (PhysicalState), and the image's data that's its own.
    netplay::GameState::SavedMemory SavedMemory()
    {
        netplay::GameState::SavedMemory memory;
        const ImageState& image = GetImageState();
        memory.ranges = image.pages;
        for (const auto& [page, mask] : image.partial)
            memory.partial.emplace_back(page, mask.get());
        for (const auto& range : netplay::heaps::Ranges(netplay::heaps::GameHeap()))
            memory.ranges.push_back(range);
        for (const auto& range : netplay::heaps::GameRegions())
            memory.ranges.push_back(range);
        kernel::ForEachThread([&](kernel::GuestThread& thread) {
            if (IsGameThread(thread.StartAddress()) && thread.StackLow() && thread.StackSize())
                memory.ranges.emplace_back(thread.StackLow(), thread.StackLow() + thread.StackSize());
        });
        for (const auto& range : game::FiberStacks())
            memory.ranges.push_back(range);
        for (const auto& range : PhysicalState())
            memory.ranges.push_back(range);
        return memory;
    }

    [[maybe_unused]] const bool g_registered = [] {
        netplay::GameState state;
        state.isGameThread = IsGameThread;
        state.asyncWorkRunning = AsyncWorkRunning;
        state.isLibraryFunction = IsLibraryFunction;
        state.isAllocatorFunction = IsAllocatorFunction;
        state.allocatesForGame = AllocatesForGame;
        state.makesForGame = MakesForGame;
        state.physicalState = PhysicalState;
        state.savedMemory = SavedMemory;
        // Timers: the frame's start (sub_824F4498) and the update's duration (sub_824F4878); the tick count at the
        // frame's end (sub_824F4398), the clock of sub_82373980's timelines (frame_time.cpp; it counts frames with the
        // fixed frame time); the scheduler's
        // (sub_824F31E8) last timestamp, and the time each of the task table's 24 slots took, in two words that
        // alternate by frame.
        state.unchecked = { { 0x82E76210, 0x82E76218 }, { 0x831013E8, 0x831013F0 }, { 0x831013E0, 0x831013E4 }, { 0x831011D8, 0x831011E0 } };
        for (uint32_t slot = 0; slot < 24; slot++)
            state.unchecked.push_back({ 0x83100F28 + slot * 28 + 16, 0x83100F28 + slot * 28 + 24 });
        state.saveHost = SaveHost;
        state.hostStates = game::FiberHostStates;
        state.fightState = game::FightState;
        state.fightChecksum = game::FightChecksum;
        state.fightSetup = game::FightSetup;
        state.seedFight = game::SeedFirstFrame;
        state.desyncRegions = game::DesyncRegions();
        state.abandonFight = game::AbandonFight;
        state.fightMenu = game::FightMenu;
        state.soundsPlaying = game::SoundsPlaying;
        game::RegisterMenus(state);
        game::RegisterVersus(state);
        // The guest C runtime's math, which the fight's code calls (the player task's sin and cos, atan, atan2), and
        // which calls the host C runtime's fma (ANALYSIS.md, Phase 6, Determinism between PCs). Arguments whose results
        // are in range only: an overflow or underflow sets the guest C runtime's errno, which needs its per-thread data
        // (pow's bases from 1/4 to 4 and exponents up to 32: results within 2^-64 and 2^64).
        state.mathFunctions = {
            { "sin", 0x82A00130, -20, 16, false, 0, 0 },
            { "cos", 0x82A00208, -20, 16, false, 0, 0 },
            { "tan", 0x82A01EA8, -20, 10, false, 0, 0 },
            { "atan", 0x829FFCC0, -30, 30, false, 0, 0 },
            { "atan2", 0x829FFDA8, -30, 30, false, -30, 30 },
            { "pow", 0x82A012B0, -2, 1, true, -6, 4 },
        };
        netplay::RegisterGameState(std::move(state));
        return true;
    }();
}

// D3D's swap callback (sub_8275ACA0, whose argument is the frame's target and scanline), which the command processor's
// swap interrupt calls through the pacing structure at [device+10388] before the frame's wait for the structure's word at
// +4 to be 0 (the WAIT_REG_MEM before the swap). D3D's frame goes: the command processor writes 1 into that word (a
// scratch register's write-back) and waits to read it, the interrupt, then that wait for 0. The callback arms a countdown
// of vblanks (device+15608) at whose end D3D's vblank handler (sub_8275A2C0) writes the 0, or releases the frame at once
// when its time has come: the 0, the countdown not armed, and the vblank count of the release (+15600 to +15604). In a
// frame whose rendering is discarded (a replay, netplay/replay.h) the command processor releases the wait for 0 itself,
// without vblanks, which left the countdown armed: the vblank handler then wrote its 0 a vblank later, over the 1 of the
// next frame, whose wait to read it never ended (a session's machine on two logical processors hung so, its command
// processor running well behind). So a discarded frame's swap is released at once, as D3D releases one whose time has
// come. The callback and the vblank handler run on the GPU's interrupt thread one after the other, and the command
// processor waits for the callback, so no vblank comes between.
extern "C" PPC_FUNC(__imp__sub_8275ACA0);
PPC_FUNC(sub_8275ACA0)
{
    __imp__sub_8275ACA0(ctx, base);
    const uint32_t device = g_device.load(std::memory_order_relaxed);
    if (!device || !gpu::DiscardingCurrentFrame())
        return;
    constexpr uint32_t kPacing = 10388, kVblanks = 15600, kReleasedAt = 15604, kCountdown = 15608;
    const uint32_t pacing = guest::LoadU32(device + kPacing);
    if (!pacing)
        return;
    guest::StoreU32(device + kCountdown, 0);
    guest::StoreU32(pacing + 4, 0);
    guest::StoreU32(device + kReleasedAt, guest::LoadU32(device + kVblanks));
}

// D3D's SetVertexDeclaration (device, declaration): the device, for SaveHost.
extern "C" PPC_FUNC(__imp__sub_82751E10);
PPC_FUNC(sub_82751E10)
{
    if (!g_device.load(std::memory_order_relaxed))
    {
        g_device.store(ctx.r3.u32, std::memory_order_relaxed);
        LOG_INFO("D3D's device at %08X", ctx.r3.u32);
    }
    __imp__sub_82751E10(ctx, base);
}
