// Guest threads that spin waiting for others, as the 360's dedicated hardware threads allowed: on a PC each would
// burn a host core, so these replacements sleep until there's something to do (DOA4-specific addresses).
#include "game/frame_times.h"
#include "gpu/command_processor.h"
#include "guest_abi.h"
#include "util/wake.h"

extern "C" PPC_FUNC(__imp__sub_82761AB8); // D3D's GPU hang check
extern "C" PPC_FUNC(__imp__sub_823FC650); // posts a job
extern "C" PPC_FUNC(__imp__sub_827922E0); // XAPI's WaitForMultipleObjects

namespace
{
    // The job ring: the index of the next job, then 16 entries of { state, two arguments } (12 bytes each),
    // under a critical section. States: 0 free, 1 posted, 2 running, 3 done (until the poster frees it).
    constexpr uint32_t kJobRing = 0x830ADB08;
    constexpr uint32_t kJobLock = 0x830ADBCC;
    constexpr uint32_t kJobEntries = 16;
    constexpr uint32_t kJobEntrySize = 12;

    util::WakeCounter g_jobsPosted;

    void EnterJobLock(PPCContext& ctx, uint8_t* base)
    {
        ctx.r3.u64 = kJobLock;
        __imp__RtlEnterCriticalSection(ctx, base);
    }

    void LeaveJobLock(PPCContext& ctx, uint8_t* base)
    {
        ctx.r3.u64 = kJobLock;
        __imp__RtlLeaveCriticalSection(ctx, base);
    }
}

// D3D's GPU hang check, called on every turn of its loops that wait for the GPU (for the ring's read pointer before
// reusing command buffer space, and for fences). The main thread waits there for most of each frame: let it sleep
// until the command processor makes progress first. DEBUG_FRAME_TIMES counts the main thread's time here as waiting
// for the GPU (frame_times.h).
PPC_FUNC(sub_82761AB8)
{
    const bool timed = game::frametimes::Recording() && game::frametimes::OnMainThread();
    const int64_t start = timed ? game::frametimes::Now() : 0;
    gpu::WaitForProgress();
    __imp__sub_82761AB8(ctx, base);
    if (timed)
        game::frametimes::AddGpuWait(game::frametimes::Now() - start);
}

// XAPI's WaitForMultipleObjects (r3 handles, at r4, r5 all of them, r6 the timeout): at 0x824F4AE4 the update
// (sub_824F4878) waits for the five threads it started on the frame's jobs; DEBUG_FRAME_TIMES counts it.
PPC_FUNC(sub_827922E0)
{
    const bool timed = uint32_t(ctx.lr) == 0x824F4AE8 && game::frametimes::Recording() && game::frametimes::OnMainThread();
    const int64_t start = timed ? game::frametimes::Now() : 0;
    __imp__sub_827922E0(ctx, base);
    if (timed)
        game::frametimes::AddWorkerWait(game::frametimes::Now() - start);
}

// Posts a job: r3 and r4 are its arguments. Returns its entry, or -1 if the ring is full.
PPC_FUNC(sub_823FC650)
{
    __imp__sub_823FC650(ctx, base);
    g_jobsPosted.Notify();
}

// The job thread: runs the posted jobs in order, with sub_8277EFC8. The original polls the next entry under the
// lock for as long as the game runs; this is the same loop, but it sleeps until a job is posted when there's none.
// (Jobs are only posted into the next entry when it's free, so the next entry is the only one to watch.)
PPC_FUNC(sub_823FC5A0)
{
    const uint32_t caller = ctx.r1.u32;
    ctx.r1.u64 = caller - 160; // the original's stack frame, with its back chain
    guest::StoreU32(ctx.r1.u32, caller);
    for (;;)
    {
        const uint64_t posted = g_jobsPosted.Value();
        EnterJobLock(ctx, base);
        const uint32_t next = guest::LoadU32(kJobRing);
        const uint32_t entry = kJobRing + 4 + next * kJobEntrySize;
        const bool ready = *guest::ToHost(entry) == 1;
        if (ready)
        {
            guest::BeforeHostWrite(entry, 1);
            *guest::ToHost(entry) = 2;
            const uint32_t argument0 = guest::LoadU32(entry + 4), argument1 = guest::LoadU32(entry + 8);
            LeaveJobLock(ctx, base);
            ctx.r3.u64 = argument0;
            ctx.r4.u64 = argument1;
            sub_8277EFC8(ctx, base);
            EnterJobLock(ctx, base);
            guest::BeforeHostWrite(entry, 1);
            *guest::ToHost(entry) = 3;
            guest::StoreU32(kJobRing, (guest::LoadU32(kJobRing) + 1) % kJobEntries);
        }
        LeaveJobLock(ctx, base);
        if (!ready)
            g_jobsPosted.Wait(posted, 2); // (the timeout in case a job is posted some other way)
    }
}
