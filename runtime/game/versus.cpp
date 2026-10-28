// DOA4's Versus mode in a netplay session (ANALYSIS.md, Phase 6): each player's own button layout and the host's rules
// for the flow's fights, without changing anyone's save (netplay/game_state.h, GameState::playerSettings). Everything is
// unlocked in it too (unlocks.cpp).
//
// The save's settings block (the pointer at 0x83035CC8, fight.cpp) holds four controller ports' button layouts at +38,
// 20 bytes a port: 8 buttons' functions (big-endian 16-bit: 0 none, 1-7, or 0x100, which sub_8251E4A8 gives the
// controller screen's choices), the scheme at +16 (0-3 a preset at 0x821FA660, 4 the custom layout), and two flags at
// +17 and +18 (0 or 1). The fight translates a player's buttons (sub_822F2FA8, sub_822F0098) through their port's layout,
// which sub_822F3178 gives: the custom one, an arcade stick's (0x821FA6A0), or the preset; for ports 5-8 the layouts DOA
// Online receives from the other players (0x82FCC660, sub_822F3268; sub_823E9308 sends them). So each machine sends its
// player's layout as resolved there (on port 0, where each machine's player plays its own menus), and in the flow
// sub_822F3178 gives port 0 the host's and port 1 the joining player's, on both machines; the pause menu's command list
// (sub_8272C458) shows them too.
//
// Versus Mode's options are at +20-+24 (difficulty, match points in single and tag matches, life gauge, round time:
// ranges 4, 5, 5, 5 and 6, as the save's check sub_82305CA0 has them); Versus's setup (sub_823F70A0) hands them to
// sub_823E39F0, which sets the match's rules, with the tag match's points and the round time 4 in a tag battle. Nothing
// else reads them but the settings screen. In the flow that call gets the host's.
//
// A session is for Versus: nobody leaves it (GameState::flowBlockedButtons; ANALYSIS.md, Phase 6). Its ways out:
// - B on Versus's menu (Single or Tag Battle, the scene manager's screen 1 or 2), which goes back to mode select. B on
//   its other screens stays in Versus (the entry, character select, stage select, each back to the one before).
// - Quit in the pause menu. Its state is at 0x830B8CE4 (sub_82405170 resets it): +0 is 1 while paused, +3 the cursor,
//   the byte at +12 the count of the items and +16 their list, bytes ending with 0xFF (0x82C59CE0 in Versus: 0, 1, 2, 3),
//   each item's message 617 + its type: 0 Continue, 1 Command List, 2 Select Character, 3 Quit (after a confirmation, to
//   the title), 4 Save Replay, 5-13 the COM level's, 14 Again, 15 Select Replay, 16 Camera Type, 17 Online Options, 18
//   Quit Session.
// - Quit in the match's result menu (sub_823E6768, which Versus's end, sub_823F74C8, runs once its sprites are in): its
//   state at 0x830AC784, +0 and +1 its columns and rows, +2 the cursor (1 at first), +3 its step (2 while it takes
//   input), +16 its items, bytes (0x82C627B0: 1 Character Select, 2 Again, 0 Quit, 4 Save Replay; 0x82C65CD0 without Save
//   Replay). In step 2 a frame moves the cursor by the directions pressed, then takes A or Start (the game's bits 0x110,
//   of any active player) on the item under it: Quit sets what follows a fight (0x83036816) to 0, back to the title.
// So in the flow the session takes B and Back out of both players' input on Versus's menu, and A and Start while the
// pause menu's or the result menu's cursor is on Quit, and while A or Start is held in either menu, the directions (and
// the left stick), which could move the cursor onto Quit in the same frame. Character select (from either menu), stage
// select and Again stay in Versus and work as ever.
#include "game/fight.h"
#include "guest_abi.h"
#include "log.h"
#include "netplay/game_state.h"

#include <cstdio>
#include <cstring>
#include <string>

extern "C" PPC_FUNC(__imp__sub_822F3178);
extern "C" PPC_FUNC(__imp__sub_823E39F0);

namespace
{
    constexpr uint32_t kSettingsPointer = 0x83035CC8;
    constexpr uint32_t kButtonLayouts = 38, kVersusRules = 20;
    constexpr uint32_t kLayoutPresets = 0x821FA660, kArcadeStickLayout = 0x821FA6A0;
    constexpr uint32_t kControllers = 0x8357C2D8; // the pad library's records, 600 bytes a port
    constexpr uint32_t kTagFlag = 0x8303683A;
    constexpr uint32_t kVersusSetupReturn = 0x823F7124; // sub_823F70A0's call of sub_823E39F0

    // The ways out of Versus (above).
    constexpr uint32_t kModePhase = 0x830AD1B2;   // the mode task's phase (menus.cpp): 3 the front end, 4 a fight
    constexpr uint32_t kScenes = 0x8310B618;      // the scene manager's state (menus.cpp): +0 the screen
    constexpr uint32_t kFightPhase = 0x82FCC952;  // the fight's main task's phase (fight.cpp): 2 while fighting
    constexpr uint32_t kPause = 0x830B8CE4;       // the pause menu
    constexpr uint32_t kResultMenu = 0x830AC784;  // the match's result menu
    constexpr uint8_t kPauseQuit = 3, kPauseQuitSession = 18, kResultQuit = 0;
    constexpr uint16_t kButtonDirections = 0x000F, kButtonStart = 0x0010, kButtonBack = 0x0020, kButtonA = 0x1000, kButtonB = 0x2000;

    uint8_t Byte(uint32_t address)
    {
        return *guest::ToHost(address);
    }

    // The item under a menu's cursor, from a list of `count` (at most 32) bytes at `items`, or 0xFF.
    uint8_t ItemAt(uint32_t items, uint32_t count, uint32_t cursor)
    {
        if (!items || count > 32 || cursor >= count || !guest::IsAllocated(items) || !guest::IsAllocated(items + count - 1))
            return 0xFF;
        return Byte(items + cursor);
    }

    uint16_t FlowBlockedButtons(uint16_t buttons, bool& stick)
    {
        stick = false;
        const uint8_t phase = Byte(kModePhase);
        if (phase == 3)
        {
            // Versus's menu: B (and Back) go back to mode select.
            const int8_t screen = int8_t(Byte(kScenes));
            return screen == 1 || screen == 2 ? kButtonB | kButtonBack : 0;
        }
        if (phase != 4)
            return 0;
        uint8_t item = 0xFF;
        bool menu = false;
        if (Byte(kPause) == 1)
        {
            menu = true;
            item = ItemAt(guest::LoadU32(kPause + 16), Byte(kPause + 12), Byte(kPause + 3));
            if (item == kPauseQuit || item == kPauseQuitSession)
                return kButtonA | kButtonStart;
        }
        else if (Byte(kFightPhase) != 2 && Byte(kResultMenu + 3) == 2)
        {
            menu = true;
            item = ItemAt(guest::LoadU32(kResultMenu + 16), uint32_t(Byte(kResultMenu)) * Byte(kResultMenu + 1), uint32_t(int8_t(Byte(kResultMenu + 2))));
            if (item == kResultQuit)
                return kButtonA | kButtonStart;
        }
        if (menu && (buttons & (kButtonA | kButtonStart)))
        {
            stick = true;
            return kButtonDirections;
        }
        return 0;
    }

    // The bytes a player's settings travel as (GameState::PlayerSettings).
    enum : size_t
    {
        kFormat = 0,     // 1
        kScheme = 1,     // 0-4, for the log
        kLayout = 2,     // 16 bytes: the 8 buttons' functions as the game holds them
        kRules = 18,     // 5 bytes: Versus Mode's options
        kReserved = 23,  // 0
        kSize = 24,
    };
    constexpr uint8_t kRuleLimits[5] = { 4, 5, 5, 5, 6 };

    uint32_t Settings()
    {
        return guest::LoadU32(kSettingsPointer);
    }

    // A port's button layout as sub_822F3178 gives it: 16 bytes.
    void ResolveLayout(uint32_t settings, uint32_t port, uint8_t* out)
    {
        const uint32_t layout = settings + kButtonLayouts + 20 * port;
        const uint8_t scheme = *guest::ToHost(layout + 16);
        uint32_t source = layout;
        const uint32_t controller = kControllers + 600 * port;
        if (scheme != 4)
        {
            if (*guest::ToHost(controller + 88) && *guest::ToHost(controller + 69) == 3) // an arcade stick (sub_824F3CF0)
                source = kArcadeStickLayout;
            else if (scheme < 4)
                source = kLayoutPresets + 16 * scheme;
        }
        memcpy(out, guest::ToHost(source), 16);
    }

    bool LocalSettings(uint8_t* bytes)
    {
        const uint32_t settings = Settings();
        if (!settings)
            return false;
        memset(bytes, 0, kSize);
        bytes[kFormat] = 1;
        bytes[kScheme] = *guest::ToHost(settings + kButtonLayouts + 16);
        ResolveLayout(settings, 0, bytes + kLayout);
        memcpy(bytes + kRules, guest::ToHost(settings + kVersusRules), 5);
        return true;
    }

    bool ValidSettings(const uint8_t* bytes)
    {
        if (bytes[kFormat] != 1 || bytes[kScheme] > 4 || bytes[kReserved] != 0)
            return false;
        for (uint32_t button = 0; button < 8; button++)
        {
            const uint32_t function = uint32_t(bytes[kLayout + 2 * button]) << 8 | bytes[kLayout + 2 * button + 1];
            if (function > 7 && function != 0x100)
                return false;
        }
        for (uint32_t i = 0; i < 5; i++)
        {
            if (bytes[kRules + i] >= kRuleLimits[i])
                return false;
        }
        return true;
    }

    std::string DescribeSettings(const uint8_t* bytes)
    {
        std::string text = bytes[kScheme] == 4 ? "custom buttons (" : "buttons preset " + std::to_string(bytes[kScheme]) + " (";
        char part[128];
        for (uint32_t button = 0; button < 8; button++)
        {
            snprintf(part, sizeof(part), "%s%X", button ? " " : "", uint32_t(bytes[kLayout + 2 * button]) << 8 | bytes[kLayout + 2 * button + 1]);
            text += part;
        }
        snprintf(part, sizeof(part), "), Versus: difficulty %u, match points %u (tag %u), life gauge %u, round time %u", bytes[kRules],
            bytes[kRules + 1], bytes[kRules + 2], bytes[kRules + 3], bytes[kRules + 4]);
        return text + part;
    }

    // The flow's settings while a session applies them (main thread): the two players' layouts in guest memory, for
    // sub_822F3178 to give, and the host's rules.
    struct Applied
    {
        bool on = false;
        uint32_t layouts = 0; // ports 0 and 1, 16 bytes each
        uint8_t rules[5] = {};
    };
    Applied g_applied;

    void ApplySettings(const uint8_t* host, const uint8_t* joining)
    {
        Applied& a = g_applied;
        if (!host || !joining)
        {
            a.on = false;
            return;
        }
        if (!a.layouts)
            a.layouts = guest::SystemAlloc(32);
        if (!a.layouts)
        {
            LOG_ERROR("Versus: no guest memory for the players' button layouts; each machine's own apply");
            return;
        }
        memcpy(guest::ToHost(a.layouts), host + kLayout, 16);
        memcpy(guest::ToHost(a.layouts + 16), joining + kLayout, 16);
        memcpy(a.rules, host + kRules, 5);
        a.on = true;
    }
}

namespace game
{
    void RegisterVersus(netplay::GameState& state)
    {
        netplay::GameState::PlayerSettings& p = state.playerSettings;
        p.size = kSize;
        p.local = LocalSettings;
        p.valid = ValidSettings;
        p.describe = DescribeSettings;
        p.apply = ApplySettings;
        state.flowBlockedButtons = FlowBlockedButtons;
    }

    void FightButtonLayout(uint32_t port, uint8_t* out)
    {
        if (g_applied.on && port < 2)
            memcpy(out, guest::ToHost(g_applied.layouts + 16 * port), 16);
        else if (const uint32_t settings = Settings())
            ResolveLayout(settings, port, out);
        else
            memset(out, 0, 16);
    }
}

// A port's button layout: in a session's flow, port 0 gets the host's and port 1 the joining player's.
PPC_FUNC(sub_822F3178)
{
    const uint32_t port = ctx.r3.u32;
    if (g_applied.on && port < 2)
    {
        ctx.r3.u64 = g_applied.layouts + 16 * port;
        return;
    }
    __imp__sub_822F3178(ctx, base);
}

// The match's rules (difficulty, round time, life gauge, match points): from Versus's setup in a session's flow, the
// host's options, as sub_823F70A0 reads them from the save.
PPC_FUNC(sub_823E39F0)
{
    if (g_applied.on && uint32_t(ctx.lr) == kVersusSetupReturn)
    {
        const uint8_t* rules = g_applied.rules;
        const bool tag = *guest::ToHost(kTagFlag) != 0;
        ctx.r3.u64 = rules[0];
        ctx.r4.u64 = tag ? 4 : rules[4];
        ctx.r5.u64 = rules[3];
        ctx.r6.u64 = tag ? rules[2] : rules[1];
    }
    __imp__sub_823E39F0(ctx, base);
}
