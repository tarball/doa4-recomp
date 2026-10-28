#pragma once

// DOA4's fights (fight.cpp), menus (menus.cpp), Versus mode in a session (versus.cpp) and what's unlocked (unlocks.cpp),
// for netplay and testing.
#include "netplay/game_state.h"

namespace game
{
    // At a frame boundary: whether the frames about to run are a fight's, past its loading, or its loading
    // (GameState::fightState).
    netplay::GameState::FightState FightState();
    // At each frame boundary, after netplay's: logs the fight's phase (and next phase) when it changes.
    void LogFightPhase();
    // At a frame boundary in a fight: a checksum of its state (GameState::fightChecksum).
    uint64_t FightChecksum();
    // At a fight's first frame: what makes it the one it is (GameState::fightSetup).
    std::vector<netplay::StatePart> FightSetup();
    // The state a session keeps for a desync's dump (GameState::desyncRegions).
    std::vector<netplay::GameState::Region> DesyncRegions();
    // After a session ended in a fight: the match won by game user `user`'s player (GameState::abandonFight).
    bool AbandonFight(PPCContext& ctx, uint8_t* base, uint32_t user);
    // At a frame boundary in a fight: whether a press can end the fight there (GameState::fightMenu).
    bool FightMenu();
    // The menus' part of the game's state (menus.cpp): GameState::menuState, menuChecksum and seedMenus.
    void RegisterMenus(netplay::GameState& state);
    // At each frame boundary, after netplay's: DEBUG_MENU_LOG=1 logs the menus' state as it changes (menus.cpp).
    void LogMenus();
    // Versus mode in a session (versus.cpp): GameState::playerSettings.
    void RegisterVersus(netplay::GameState& state);
    // A controller port's button layout as the fight uses it (16 bytes, sub_822F3178's): in a session's Versus, the
    // players' own.
    void FightButtonLayout(uint32_t port, uint8_t* out);
    // At each frame boundary, after netplay's: everything unlocked in memory while a session's machine is in Versus, or
    // with DEBUG_UNLOCK_ALL=1, and the save's own unlocks again after (unlocks.cpp).
    void UpdateUnlocks();
}
