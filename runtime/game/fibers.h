#pragma once

// DOA4's XAPI fibers on host stacks of the runtime's own (fibers.cpp), for netplay's savestates.
#include "netplay/game_state.h"

#include <string>
#include <utility>
#include <vector>

namespace game
{
    // Saves the fibers the runtime started, all suspended (at a frame boundary, on the main thread's own fiber): each
    // one's context, and its host stack from its stack pointer up, which holds the registers its recompiled code keeps
    // while suspended. Its load first checks that the same fibers exist, all suspended; if not, it loads nothing and
    // returns why. Then each gets its context, host stack and stack pointer back, wherever it's suspended now, so it
    // resumes where it was at the save. The free stack: below each fiber's stack pointer.
    netplay::HostState SaveFibers(std::string& error);
    // Each fiber's host state at its last switch, by record (GameState::hostStates).
    std::vector<std::pair<uint32_t, uint64_t>> FiberHostStates();
    // The guest stacks of the fibers the runtime started, [limit, base) (GameState::savedMemory).
    std::vector<std::pair<uint32_t, uint32_t>> FiberStacks();
}
