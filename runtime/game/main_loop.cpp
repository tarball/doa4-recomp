// DOA4's main loop (sub_824F41E0) in host code, for netplay's frame boundary (runtime/netplay/frame.h). The guest loop
// initializes (sub_824F4210, sub_824F4560), then runs six functions each frame, with nothing kept in registers between
// iterations: sub_824F4498, sub_824F0F78, sub_824F0B20, sub_824F34D8 (reads the pads), sub_824F4878 (the update and drawing,
// which runs the game's task fibers) and sub_824F4398 (ends the frame). The same calls, with the same guest stack frame
// and return addresses, and netplay::FrameBoundary at the top of each iteration (netplay::AfterUpdate before the end).
// DEBUG_FRAME_TIMES times each part (frame_times.h).
#include "game/fight.h"
#include "game/frame_times.h"
#include "game/random.h"
#include "guest_abi.h"
#include "netplay/frame.h"

PPC_FUNC(sub_824F41E0)
{
    using namespace game::frametimes;
    // The guest prologue: LR saved in the caller's frame, then a 96-byte frame with its back chain.
    const uint32_t caller = ctx.r1.u32;
    guest::StoreU32(caller - 8, uint32_t(ctx.lr));
    ctx.r1.u64 = caller - 96;
    guest::StoreU32(ctx.r1.u32, caller);

    ctx.lr = 0x824F41F0;
    sub_824F4210(ctx, base);
    ctx.lr = 0x824F41F4;
    sub_824F4560(ctx, base);
    for (;;)
    {
        BeginFrame();
        netplay::FrameBoundary(ctx);
        game::LogFightPhase();
        game::LogMenus();
        game::UpdateUnlocks();
        game::RandomLogFrame(netplay::Frame());
        Enter(kTiming);
        ctx.lr = 0x824F41F8;
        sub_824F4498(ctx, base);
        Enter(kRenderBegin);
        ctx.lr = 0x824F41FC;
        sub_824F0F78(ctx, base);
        Enter(kFrame0B20);
        ctx.lr = 0x824F4200;
        sub_824F0B20(ctx, base);
        Enter(kPads);
        ctx.lr = 0x824F4204;
        sub_824F34D8(ctx, base);
        Enter(kUpdate);
        ctx.lr = 0x824F4208;
        game::SetInUpdate(true);
        sub_824F4878(ctx, base);
        game::SetInUpdate(false);
        Enter(kAfterUpdate);
        netplay::AfterUpdate();
        Enter(kEnd);
        ctx.lr = 0x824F420C;
        sub_824F4398(ctx, base);
        EndFrame();
    }
}
