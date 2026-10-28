#pragma once

// DOA4's calls into the libraries whose state stays outside savestates, which netplay's journal records in a frame's
// first run and answers in its replays (runtime/netplay/replay.h, journal.cpp).
#include "netplay/replay.h"
#include "ppc_recomp_shared.h"

#include <cstdint>

#include <vector>

namespace game
{
    // Whether a call of the guest function at `function` is journaled (journal.cpp).
    bool Journaled(uint32_t function, const PPCContext& ctx);
    // The sounds and streams the sound engine plays (GameState::soundsPlaying).
    std::vector<uint32_t> SoundsPlaying();

    // A call of `function` (the guest function at `address`): through the journal while it records or replays and the
    // call is one it watches, else made.
    inline void JournalCall(uint32_t address, netplay::journal::Function function, PPCContext& ctx, uint8_t* base)
    {
        if (netplay::journal::Enabled() && Journaled(address, ctx))
            netplay::journal::Call(address, function, ctx, base);
        else
            function(ctx, base);
    }

    // A release of a library's object (journal.cpp): deferred while the journal records or replays
    // (netplay::journal::Defer), else made.
    inline void DeferCall(uint32_t address, netplay::journal::Function function, PPCContext& ctx, uint8_t* base)
    {
        if (netplay::journal::Enabled() && Journaled(address, ctx))
            netplay::journal::Defer(address, function, ctx, base);
        else
            function(ctx, base);
    }
}
