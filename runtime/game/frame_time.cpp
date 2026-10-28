// DOA4's frame time, fixed for netplay (netplay::FixedFrameTime). Each frame, sub_824F4498 asks D3D how long the last
// frame kept the CPU and the GPU busy, in frames at 59.94 Hz, from 64-entry histories of timestamps that D3D's swap
// (sub_82765AE0) keeps: sub_82765FE8 the CPU's, at the device's +1288, from the end of one swap to the start of the
// next (QueryPerformanceCounter), and sub_82765EF8 the GPU's, at +1292, from the start of a frame's commands to their
// end (the counter, read by callback 0x82765540 when the command processor's interrupts for those two points come). It
// keeps them as percentages of a frame (0x830FE3FC the CPU's, 0x831008E8 the GPU's) and as rates of at most 60
// (0x831008E0, 0x831008E4). Only the GPU's is read, to shed load: sub_826BF030 hides a third of the 198 particles it
// spawns above 90%, two thirds above 95% and all of them above 100%, and sub_8271B318 starts four kinds of the effects
// the characters' motion scripts ask for (sub_8240CE58; types 5-8 of its requests, from the stage's emitters through
// sub_8271A188) only below 85-95%. Measured, ours is the command processor's time for a frame's commands, about a
// quarter of a frame, and it differs from run to run, as would frames replayed with it. Fixed, every frame kept the CPU
// and the GPU busy for half of it, as a machine that keeps up, which sheds nothing (a whole frame, as it was, shed
// both). Neither function writes anything but its own stack frame.
//
// And the tick count: XAPI's GetTickCount (sub_82790138) reads KeTimeStampBundle's milliseconds, the only code that
// reads the bundle, so the state census (netplay/state_census.h) hears of each read here. The game's one read is at the
// frame's end, for timelines of keyframes a frame apart that sub_82373980 plays for each player once a sound it waits for
// plays (most likely lip sync: they run with the intros' voice lines, through jobs for worker pool 82374990 from records
// at 0x8302BC98, 580 bytes a player). Timed by the wall clock, a replayed frame found them further on; with the fixed
// frame time that read gets a clock that counts frames, as the auto-repeat's do (below).
//
// And the pad library's auto-repeat (sub_82774AE0, for each pad as sub_82774D60 reads them): it times each held button
// with QueryPerformanceCounter (sub_82792660, an mftb) against an initial delay and a repeat interval, so a held
// direction moves a menu's cursor at each machine's own pace. With the fixed frame time its three reads get a clock
// that counts frames (netplay::Frame, at 59.94 Hz of the 50 MHz time base), the same on every machine and in a replay.
#include "guest_abi.h"
#include "netplay/frame.h"
#include "netplay/state_census.h"

extern "C" PPC_FUNC(__imp__sub_82765FE8);
extern "C" PPC_FUNC(__imp__sub_82765EF8);
extern "C" PPC_FUNC(__imp__sub_82790138);
extern "C" PPC_FUNC(__imp__sub_82792660);

// The CPU's and the GPU's time for a frame while it's fixed, in frames: under the GPU's 85% from which the game sheds.
constexpr double kFixedBusyTime = 0.5;

PPC_FUNC(sub_82765FE8)
{
    if (netplay::FixedFrameTime())
    {
        ctx.f1.f64 = kFixedBusyTime;
        return;
    }
    __imp__sub_82765FE8(ctx, base);
}

PPC_FUNC(sub_82765EF8)
{
    if (netplay::FixedFrameTime())
    {
        ctx.f1.f64 = kFixedBusyTime;
        return;
    }
    __imp__sub_82765EF8(ctx, base);
}

PPC_FUNC(sub_82790138)
{
    if (netplay::census::Active())
        netplay::census::NoteRead("GetTickCount");
    if (uint32_t(ctx.lr) == 0x824F447C && netplay::FixedFrameTime())
    {
        // The frame's end (sub_824F4398) keeps the tick count at 0x831013E0, by which sub_82373980 plays the players'
        // timelines (most likely the faces' lip sync with the intros' voice lines): with the fixed frame time,
        // milliseconds of frames at 59.94 Hz.
        ctx.r3.u64 = uint32_t(netplay::Frame() * 1001 / 60);
        return;
    }
    __imp__sub_82790138(ctx, base);
}

// QueryPerformanceCounter: r3 is where the count goes; returns 1.
PPC_FUNC(sub_82792660)
{
    switch (uint32_t(ctx.lr))
    {
    case 0x82774B3C: // the pad library's auto-repeat: a button pressed,
    case 0x82774B7C: // held,
    case 0x82774BCC: // repeated
        if (netplay::FixedFrameTime())
        {
            // A frame at 59.94 Hz is 50,000,000 * 1001 / 60,000 ticks of the time base.
            *guest::Translate<be<uint64_t>>(ctx.r3.u32) = netplay::Frame() * 2502500ull / 3;
            ctx.r3.u64 = 1;
            return;
        }
        break;
    }
    __imp__sub_82792660(ctx, base);
}
