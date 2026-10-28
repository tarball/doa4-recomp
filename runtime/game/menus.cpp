// DOA4's menus, for a netplay session's lockstep in Versus mode (netplay/game_state.h, GameState::menuState;
// ANALYSIS.md, Phase 6).
//
// The mode task (task slot 0, sub_82402BF8) runs the game's phases from a table at 0x82C5A098 (enter, update and leave
// functions, 12 bytes a phase), the phase at 0x830AD1B2 and the next at 0x830AD3BB: 0 boots (the logos and the intro
// movie; sub_82403518 waits for files), 1 the title (sub_82403700, sub_82403800), 3 the front end (sub_82403950 starts
// the scene manager), 4 a fight (sub_82403BB0 starts its main task), 7 and 8 on the way from a fight back to the front
// end.
//
// The title's state is at 0x8303B560: +0 is 1 while it runs, +1 is 0 at the title, 1 in its attract sequence
// (cinematics and demo fights, which stream their files) and 2 as it leaves for the front end. Its input (sub_82404B08)
// takes the first of the four pads with Start or A pressed and makes it player 1's, then +5 counts 30 frames down.
//
// The front end's scene manager (task slot 2, sub_8253D740, its state at 0x8310B618) runs 18 menu screens from a table at
// 0x821B4DB0, 36 bytes a screen: enter at +0, update at +4, leave functions at +16 and +28, an argument at +32. The
// screens: 0 mode select, 1 and 2 a mode's menu (Versus: Single or Tag Battle), 3 Versus's entry (each player picks a
// side), 4 character select with the costumes, 5 stage select, 6 to 10 the settings (8 is VIDEO, video_menu.cpp, 9 the
// controller's buttons), 11 to 17 others. Its state: +0 the current screen (signed), +1 the previous, +2 the next, +3 a
// stack of 10 and its depth at +16 (entries past the depth are left from screens visited before); +28 is 1 while an
// enter or leave function runs; +32 and +36 the transition asked for; +40 the phase: 0 fading in (for +56 frames, 30 or
// 60), 1 running, 2 and 3 leaving (fading out, then waiting for the checks at +84, +120 of them, such as the screen's
// fade, sub_8252C890); +48 and +52 count the frames since the screen entered; +68 points to the table; +136 counts the
// screens; +152 is an enter pending. A screen's update takes input in phases 0 and 1 (sub_8252C7A8, character select's
// sub_82522950) but not while it leaves.
//
// Mode select (its state at 0x831037A0) keeps the mode under its cursor at +12 (0x831037AC), and as it leaves, the game
// mode chosen at +16: Versus is mode 4 there (game mode 5 in Single Battle), and leads to its menu, screen 1
// (sub_8252E058); there, with two controllers connected (sub_8252C6E0), Single or Tag Battle leads to the entry, screen 3
// (sub_8252E560), then character select and stage select, and stage select to the fight. A fight's end shows the match's
// result and a menu: Character Select, Again, Quit and Save Replay (sub_823E6768, which sets 0x83036816 to 2, 1, 0): Again
// starts the next fight at once (still the mode task's phase 4), Character Select restarts the front end at character
// select (sub_8253D960, through phases 7 and 8), with the stack as it was, and Quit at mode select. The pause menu's Select
// Character and Quit do the same (after a confirmation). Back (B) leads from character select to the entry, the entry to
// Versus's menu, Versus's menu to mode select, and mode select to the title. +12 still says Versus at mode select, so the
// flow (GameState::MenuState::flow) is Versus's screens 1 to 5 with Versus chosen, the loading and leaving that begins or
// ends at one of them, and the fights started from them; a session plays it in lockstep, and each machine the rest of its
// menus on its own (netplay/session.h).
//
// Entering a screen (sub_82536D88) calls its enter function, which loads the screen's files through CRI's loader
// (thread 829C96B0, 128 KB a read) and yields until they're in: character select's (sub_82521478) the characters'
// portraits, models and animations (chr*.afs, anm.afs, mot.afs), stage select's (sub_825379D0) the previews (spr.afs).
// That, booting, the title's attract sequence and the way from the title to the front end (the scene manager's own
// loading) are what take each machine its own number of frames. What a screen does from its first frame past its enter
// function until it leaves depends on its input alone: a slower disk while the screens run (the stage preview a cursor
// asks for, streamed from stg.afs) changed nothing, and leaving takes a fixed fade. So each screen of the flow is played
// in lockstep from that frame on (netplay::CurrentSegment), and the rest is left to each machine's time. Character
// select, entered from Versus's menu or the entry, starts afresh (the cursors, the players' states, the stage list:
// sub_82537458), and the entry too, whatever screens each machine visited before.
//
// RANDOM picks come from three shuffled bags of 20 bytes: { array, count, capacity, next, swaps }, the characters' at
// 0x82E76278 and the costumes' at 0x82E7628C (sub_82523120, through sub_8253E440 as stage select leaves) and the stages'
// at 0x82E76374 (sub_825382A8, through the versus fight's setup). Each draw takes the next entry, and an exhausted bag is
// refilled in order and shuffled (sub_825211D0, sub_8252D9E8) with the fight's random number generator (0x833F7600),
// which nothing else in the menus draws from: the stages' bag first as the front end starts (sub_82537458, 100 swaps),
// the others at their first draw. Seeded at boot from the clock, it differs between machines, and DOA Online seeds it
// from the session as its character select starts (sub_82521478 at 0x825215B8). In a session the menus seed all four
// generators as each screen of the flow starts, and empty the bags, so that their next draws shuffle them afresh from
// the session's seed.
//
// DEBUG_MENU_LOG=1 logs the menus' state whenever it changes: the mode task's phase, the title's, the scene manager's
// screens, stack and phase, mode select's mode, what follows a fight, the game mode, the tag flag and the mode chosen.
//
// A session goes straight into Versus (GameState::intoFlow; ANALYSIS.md, Phase 6): once the game has booted (the player
// may skip its logos and movie as ever), the session presses for this machine's player, in place of their own input:
// Start at the title (or in its attract sequence), then at mode select Right until the mode under the cursor is Versus,
// and A; on any other screen outside the flow, B, back toward mode select. A press lasts 2 frames, one every 12 at most,
// so each screen takes it once. The game itself does the rest as for a player: the title signs in the profile and makes
// pad 1 player 1's, and mode select sets Versus's game mode as it leaves.
#include "game/fight.h"
#include "game/random.h"
#include "guest_abi.h"
#include "log.h"
#include "netplay/frame.h"
#include "netplay/game_state.h"

#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <string>

namespace
{
    constexpr uint32_t kModePhase = 0x830AD1B2, kModeNextPhase = 0x830AD3BB, kChosenMode = 0x82FCC93B;
    constexpr uint32_t kTitle = 0x8303B560;
    constexpr uint32_t kScenes = 0x8310B618;
    constexpr uint32_t kPlayerPads = 0x830B8C80;           // the players' pads and which are active (48 bytes)
    constexpr uint32_t kHovered = 0x83103074;              // character select: the characters under the cursors
    constexpr uint32_t kSelectPlayers = 0x83103240;        // character select: 140 bytes a player
    constexpr uint32_t kModeCursor = 0x831037A4;           // mode select's cursor
    constexpr uint32_t kModeEntry = 0x831037AC;            // mode select's mode, under the cursor or chosen (u32)
    constexpr uint32_t kVersusEntry = 4;
    constexpr uint32_t kStageChoice = 0x8310B710;          // stage select's choice (0xFF: RANDOM)
    constexpr uint32_t kFightGenerator = 0x833F7600, kGeneratorState = 2504;
    constexpr uint32_t kBags[] = { 0x82E76278, 0x82E7628C, 0x82E76374 };
    constexpr uint32_t kTitleScreen = 0x100, kFrontEndScreens = 0x300;

    uint8_t Byte(uint32_t address)
    {
        return *guest::ToHost(address);
    }

    // Whether the scene manager's screen belongs to Versus mode's flow: its menu (Single or Tag Battle), the entry, and
    // character and stage select, with Versus chosen at mode select.
    bool FlowScreen(int32_t screen)
    {
        return screen >= 1 && screen <= 5 && guest::LoadU32(kModeEntry) == kVersusEntry;
    }

    netplay::GameState::MenuState Menus()
    {
        using State = netplay::GameState::MenuState;
        const uint8_t phase = Byte(kModePhase), next = Byte(kModeNextPhase);
        if (phase == 0)
            return { State::kLocal };
        if (phase != next)
            return { State::kLoading };
        if (phase == 1)
        {
            // The title, never in the flow.
            if (Byte(kTitle + 1) == 1)
                return { State::kLocal };
            if (Byte(kTitle) != 1 || Byte(kTitle + 1) != 0)
                return { State::kLoading, 0, false, true };
            return { State::kReady, kTitleScreen, false, true };
        }
        if (phase != 3)
            return { State::kLoading }; // a fight, or on the way to or from one: the flow stays as it was
        const int8_t current = int8_t(Byte(kScenes));
        if (guest::LoadU32(kScenes + 68) == 0 || current < 0 || uint32_t(current) >= guest::LoadU32(kScenes + 136) ||
            guest::LoadU32(kScenes + 28) != 0 || guest::LoadU32(kScenes + 152) != 0 || guest::LoadU32(kScenes + 40) > 1 ||
            guest::LoadU32(kScenes + 48) == 0)
        {
            // A screen loads, or leaves for the next: in the flow if either belongs to it (mode select leaving for
            // Versus's menu, or Versus's menu leaving for mode select).
            const int8_t following = int8_t(Byte(kScenes + 2));
            if (guest::LoadU32(kScenes + 68) == 0 || (current < 0 && following < 0))
                return { State::kLoading };
            return { State::kLoading, 0, FlowScreen(current) || FlowScreen(following), true };
        }
        return { State::kReady, kFrontEndScreens | uint32_t(current), FlowScreen(current), true };
    }

    uint64_t MenuChecksum()
    {
        uint64_t hash = 0xCBF29CE484222325ull; // FNV-1a
        const auto add = [&hash](uint32_t address, uint32_t length) {
            const uint8_t* bytes = guest::ToHost(address);
            for (uint32_t i = 0; i < length; i++)
                hash = (hash ^ bytes[i]) * 0x100000001B3ull;
        };
        add(kModePhase, 1);
        add(kModeNextPhase, 1);
        add(kChosenMode, 1);
        add(kPlayerPads, 48);
        // The screen's own state, which only it keeps up to date (others may have written it on each machine's time).
        const uint32_t screen = Menus().screen;
        if (screen == kTitleScreen)
        {
            add(kTitle, 2);     // running, at the title
            add(kTitle + 5, 2); // the countdown from a press, and what blocks input (+3, its background's fade, follows loading)
        }
        else
        {
            // The screens, and the stack as deep as it goes: past its depth it holds the screens each machine visited
            // on its own, as the transitions asked for (+32, +36) may be.
            add(kScenes, 3);
            const uint32_t depth = guest::LoadU32(kScenes + 16);
            add(kScenes + 16, 4);
            add(kScenes + 3, depth < 10 ? depth : 10);
            add(kScenes + 40, 4); // the phase
            add(kScenes + 48, 4); // the frames since the screen entered
            switch (screen)
            {
            case kFrontEndScreens | 0: // mode select
                add(kModeCursor, 4);
                break;
            case kFrontEndScreens | 4: // character select
                add(kHovered, 16);
                for (uint32_t player = 0; player < 4; player++)
                {
                    add(kSelectPlayers + 140 * player, 4);        // its state: choosing, chosen, its costume
                    add(kSelectPlayers + 140 * player + 0x1C, 4); // the cursor and the character under it
                }
                break;
            case kFrontEndScreens | 5: // stage select
                add(kStageChoice, 4);
                break;
            }
        }
        add(kFightGenerator, kGeneratorState);
        for (const uint32_t bag : kBags)
        {
            add(bag + 4, 16);
            const uint32_t entries = guest::LoadU32(bag), count = guest::LoadU32(bag + 4);
            if (entries && count <= 64)
                add(entries, 4 * count);
        }
        return hash;
    }

    // A session's way into Versus (above).
    bool IntoFlow(uint64_t frame, uint16_t& buttons)
    {
        constexpr uint16_t kRight = 0x0008, kStart = 0x0010, kA = 0x1000, kB = 0x2000; // XInput's
        using State = netplay::GameState::MenuState;
        static uint64_t pressFrame = 0;
        static uint16_t pressed = 0;
        buttons = 0;
        if (Byte(kModePhase) == 0)
            return false; // booting: the player's own input
        if (pressed && frame - pressFrame < 2)
        {
            buttons = pressed;
            return true;
        }
        if (pressed && frame - pressFrame < 12)
            return true;
        pressed = 0;
        const State menus = Menus();
        uint16_t press = 0;
        if (Byte(kModePhase) == 1 && (Byte(kTitle + 1) == 1 || (menus.kind == State::kReady && menus.screen == kTitleScreen)))
            press = kStart; // the title, or its attract sequence
        else if (menus.kind == State::kReady && menus.screen == kFrontEndScreens)
            press = guest::LoadU32(kModeEntry) == kVersusEntry ? kA : kRight; // mode select
        else if (menus.kind == State::kReady && menus.screen > kFrontEndScreens && !menus.flow)
            press = kB; // another screen: back toward mode select
        if (press)
        {
            pressed = press;
            pressFrame = frame;
            buttons = press;
        }
        return true;
    }

    void SeedMenus(PPCContext& ctx, uint8_t* base, uint64_t seed)
    {
        game::SeedGenerators(ctx, base, seed);
        // The bags refilled in order and used up, as sub_8252D9E8 refills them, so that their next draws shuffle them.
        for (const uint32_t bag : kBags)
        {
            const uint32_t entries = guest::LoadU32(bag), capacity = guest::LoadU32(bag + 8);
            if (!entries || !capacity || capacity > 64)
                continue;
            for (uint32_t i = 0; i < capacity; i++)
                guest::StoreU32(entries + 4 * i, i);
            guest::StoreU32(bag + 4, capacity);
            guest::StoreU32(bag + 12, capacity);
        }
    }
}

namespace game
{
    void RegisterMenus(netplay::GameState& state)
    {
        state.menuState = Menus;
        state.flowName = "Versus";
        state.intoFlow = IntoFlow;
        state.menuChecksum = MenuChecksum;
        state.seedMenus = SeedMenus;
    }

    void LogMenus()
    {
        static const bool on = [] {
            const char* setting = std::getenv(GAME_PREFIX "_DEBUG_MENU_LOG");
            return setting && std::atoi(setting) != 0;
        }();
        if (!on)
            return;
        static std::string last;
        char text[320];
        snprintf(text, sizeof(text),
            "phase %u (next %u), title %u.%u, screen %d (previous %d, next %d), stack %u: %02X %02X %02X %02X %02X %02X, screen phase %u, "
            "mode select's mode %u (game mode %u), after the fight %u, game mode %u, tag %u, mode chosen %u",
            Byte(kModePhase), Byte(kModeNextPhase), Byte(kTitle), Byte(kTitle + 1), int8_t(Byte(kScenes)), int8_t(Byte(kScenes + 1)),
            int8_t(Byte(kScenes + 2)), guest::LoadU32(kScenes + 16), Byte(kScenes + 3), Byte(kScenes + 4), Byte(kScenes + 5), Byte(kScenes + 6),
            Byte(kScenes + 7), Byte(kScenes + 8), guest::LoadU32(kScenes + 40), guest::LoadU32(kModeEntry), guest::LoadU32(kModeEntry + 4),
            Byte(0x83036816), Byte(0x83035CC0), Byte(0x8303683A), Byte(kChosenMode));
        if (last != text)
        {
            last = text;
            LOG_INFO("Menus: %s, at frame %llu", text, (unsigned long long)netplay::Frame());
        }
    }
}
