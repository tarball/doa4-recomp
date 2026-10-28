#pragma once

// Where a frame's time goes on the main thread (DEBUG_FRAME_TIMES=[F]first[,count], frame_times.cpp), for `count` frames
// (default 600) from game frame `first` (with F, fight frame): the main loop's six calls (main_loop.cpp), each of the
// calls the update (sub_824F4878) makes, the task fibers' runs (the scheduler's switches, fibers.cpp), the wait for the
// worker pools (sub_827922E0 at 0x824F4AE4) and D3D's waits for the GPU (its hang check, waits.cpp), with the main
// thread's CPU time and the worker pools'. One line a frame in dumps/frame_times.txt, then statistics by column, by the
// update's call and by task, apart for frames replayed from a savestate (netplay/replay.h); the means in the log.
#include <cstdint>

namespace game::frametimes
{
    // The main loop's parts, in order (main_loop.cpp).
    enum Part : uint32_t
    {
        kBoundary,    // netplay's frame boundary and the per-frame bookkeeping before the game's calls
        kTiming,      // sub_824F4498: the frame time
        kRenderBegin, // sub_824F0F78
        kFrame0B20,   // sub_824F0B20
        kPads,        // sub_824F34D8: reads the pads
        kUpdate,      // sub_824F4878: the update and drawing (the task fibers among them)
        kAfterUpdate, // netplay's after the update (a sync test's write tracking)
        kEnd,         // sub_824F4398: ends the frame (D3D)
        kPartCount
    };

    // Whether the diagnostic is set up at all, and whether the current frame is being recorded (cheap: the hooks ask
    // before timing anything).
    bool Configured();
    bool Recording();
    // QueryPerformanceCounter's ticks.
    int64_t Now();
    // The main loop: at the top of each iteration, before each part, and after the last.
    void BeginFrame();
    void Enter(Part part);
    void EndFrame();
    // From the hooks, on the main thread: a task fiber's run (from the scheduler's switch to it until it yields back, with
    // where it yielded: the call's return address and its guest stack pointer), the wait for the worker pools, D3D's
    // waits for the GPU.
    void AddTask(uint32_t fiber, uint32_t yieldSite, uint32_t r1, int64_t ticks);
    void AddWorkerWait(int64_t ticks);
    void AddGpuWait(int64_t ticks);
    // Whether the calling thread is the main loop's.
    bool OnMainThread();
}
