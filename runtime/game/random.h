#pragma once

// DOA4's random number generators (random.cpp), for netplay, and a log of their draws for comparing runs.
#include <cstdint>

struct PPCContext;

namespace game
{
    // As a fight starts, with netplay::FightSeed(): seeds the game's generators from it (guest calls on the caller's
    // stack, its registers kept).
    void SeedRandomNumbers(PPCContext& ctx, uint8_t* base);
    // Seeds the game's four generators from `seed`, each with a seed of its own (guest calls on the caller's stack, its
    // registers kept).
    void SeedGenerators(PPCContext& ctx, uint8_t* base, uint64_t seed);
    // At a fight's first frame past loading, with netplay::FightSeed(): the generators seeded again
    // (GameState::seedFight).
    void SeedFirstFrame(PPCContext& ctx, uint8_t* base, uint64_t seed);
    // At each frame boundary, on the main thread. DEBUG_RANDOM_LOG=first,count writes the main thread's draws for
    // `count` frames from frame `first` to dumps/random_log.txt (the generator, the call's return address and the number
    // drawn), and at each boundary a hash of each generator's state and the count of other threads' draws, to compare
    // runs that should match.
    void RandomLogFrame(uint64_t frame);
    // The main loop, around the frame's update (sub_824F4878): the game's other threads draw while it runs, and a draw
    // outside it is logged once, since it could fall in another frame's stream on another machine (SplitRandom).
    void SetInUpdate(bool inUpdate);
}
