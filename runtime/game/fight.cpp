// DOA4's fights, for netplay (netplay/game_state.h, GameState::fightState). sub_823DF800 starts a fight's main task
// (sub_823DFA10), and sub_823DF880 ends it. The main task's loop yields to the scheduler at its top each frame
// (0x823DFA70), then runs the handler of its phase (the byte at 0x82FCC952, which it sets from the next phase's at
// 0x830ACF85 as it starts the frame, with 0x830ACF87 set in a frame the phase changed; handlers at 0x82C657F4), or at a
// match's end sub_823E54E0 or sub_823E5220 (the byte at 0x82FCC966 3 or 2). A handler that lasts more than a frame
// yields from inside, in loops of its own, some with their counters in registers, so the main task's fiber holds state
// on the host there (fibers.cpp). In the scripted versus fight (ANALYSIS.md, Phase 6):
// - 0 loads the fight (sub_823E0138): it waits for the files and sets the fight up over a few frames, sets the next phase
//   to 1, then offline saves the profile (sub_8230D7C8, which adds the play time to the settings and starts a thread,
//   82731758, to write the save) and waits for that (at 0x823E077C): the fight's frames 0-10;
// - 1 runs the intro (sub_823E07A0): in a match's first round each character's in turn (sub_823E2F08, about 120 frames
//   each, the counter in r15) and then GET READY (sub_823E3248, 60 frames counted in r26), from fight frame 11 to 312; in
//   later rounds GET READY alone;
// - 2 the fighting (sub_823E0BD0, which returns every frame and counts the fight's frames at 0x830AD024 through every
//   round);
// - 3 a KO (sub_823E0D00, 120 frames counted at 0x830ACF94), 4 the KO's replay (sub_823E10B0, 300 frames), 5 the round's
//   result (sub_823E11D8, 420 frames), which sets the next phase to 1 for the next round or to 0 at the match's end,
//   then sub_823E54E0 for the match's result; 6 (sub_823EA4E8) didn't come.
// Loading takes each machine its own number of frames; what follows it doesn't, so netplay counts a fight's frames
// from the first past it: the first frame boundary, after the main task has loaded, at which its phase or its next isn't
// 0. As the fight starts, its random numbers are seeded for netplay (random.h).
//
// The fight's presentation asks the audio engine (sub_82777348, which finds the sounds playing in a group) whether a
// player's voice line is still playing: each character's intro (sub_823E2F08, which offline ends once its animation and
// voice are done), sub_823E7CF0 (once a skip is asked for, whether the voice just ended, to fade out) and sub_825100E0.
// Voices play on the audio engine's time, so an intro takes a frame more or less from run to run. Online, the game
// doesn't wait for them: sub_823E2F08 ends on the animation alone (with 0x8310B784 or the byte at 0x8303683A set), and
// sub_825100E0 doesn't ask (with the online flag at +472 of 0x82E6D978). With netplay::NoVoiceWaits(), those three
// questions are answered with no voice playing. And the players' timelines (sub_82373980, most likely the faces' lip
// sync, which run with the voice lines) wait for their sound to play (sub_82779690, which finds the sounds playing):
// with NoVoiceWaits they start as they're armed.
//
// The fight's checksum (GameState::fightChecksum) covers the values its state comes down to, which hold no addresses: the
// round's fighting frames (0x830AD024, 16-bit: the clock, kept through a round's end, restarted with the next round), the
// main task's phase and next phase, the players' records at 0x83033B40 (two, or four in a tag battle; 156 bytes each: the
// position at +4, the height at +8 and the depth at +12, angles at +0x14-+0x1C, health at +0x24, 16-bit, from 240; the
// move's state after; what the floor under the player is at +0x6C and +0x70, but not the address of its collision data at
// +0x74, kFloorData, which depends on the stage's slot) and the fight's random number generator (0x833F7600: its words,
// the next word's address, in the image, and the words left). Not the players' tasks' records at 0x8320D110 (4,336 bytes
// each), which hold addresses among their floats.
//
// What makes a fight the one it is (GameState::fightSetup), at its first frame:
// - the stage (the byte at 0x82FCC96E: 0x1A Waterfall Valley, 0x25 Tritower Heliport, 0x0E Seaside Market, 0x18 Savannah
//   Safari), and the mode: 0x83035CC0-0x83035CC3 (the game's mode at 0x83035CC0, 3 story, 4 time attack, 5 versus; at
//   0x83035CC1 the human players, set as a fight loads), 0x83035CC6 and 0x83035CC7, and 0x82FCC93B and 0x82FCC966,
//   which the attract demo's setup sub_82387078 sets with the stage. Not 0x83035CC4 and 0x83035CC5: the stages in the
//   game's two stage slots, the fight's in either, beside the one loaded before it (0x82FCC967 says which slot: sub_8231F908),
//   which differ between machines that loaded other stages before;
// - the sides' characters and costumes: 30 bytes a side from 0x83036844, a character and its costume at +0 and +1, the
//   tag partner's at +2 and +3 (the attract demo's setup, sub_823E8AF8 and sub_823F10B0 write them), and the tag flag at
//   0x8303683A;
// - the match's rules, which a mode's setup (sub_823F70A0 for Versus) gives sub_823E39F0 from the game's settings: the
//   difficulty (0x82FCC8EE), the round time's choice and seconds (0x82FCCC3E, 0x83033A7A), the life gauge's choice and
//   the health it gives (0x8304006B, 0x8303691C), its flags (0x83031930), and the match points' choice and the rounds to
//   win (0x83036B3F, 0x82FCC8E3);
// - both players' button layouts, as sub_822F3178 gives them to the fight and the AI: a port's custom layout, or one of
//   four presets (0x821FA660, 16 bytes each), or an arcade stick's (0x821FA6A0) when the port's controller is one (the
//   pad library's records from 0x8357C2D8, 600 bytes a port: connected at +88, the subtype at +69; sub_824F3CF0), and in a
//   session's Versus each player's own (versus.cpp);
// - the four random number generators (random.cpp), seeded as the fight's task starts, before loading, which takes each
//   machine its own number of frames.
//
// The game's settings: the save's settings block, which the pointer at 0x83035CC8 gives (into the signed-in profile's
// record of 67,840 bytes at 0x831A90D8, from its +31080; the save file is that record). It's 36,752 bytes (sub_82306318
// sets the defaults; the word at +16544 is its size, which sub_82306758 checks): the game options first, a mode's at a
// time as SETTINGS > GAME SETTINGS lists them (their screens' update functions sub_8251D8F0 to sub_8251E1C8): Story
// Mode's difficulty at +0; Time Attack's at +4-+8 and Versus Mode's at +20-+24 (difficulty, match points in single and
// tag matches, life gauge, round time), Survival's at +12-+14 and Team Battle's at +16-+18 (difficulty, life gauge, round
// time), and the other options at +28-+31 (system voice, then 1, 0 and the player's age, 13-99, which only the settings
// screen reads). Then the volumes (+32-+34), the languages (+36, +37), the four ports' button layouts (+38, 20 bytes
// each: 8 buttons' functions, the scheme at +16: 0-3 a preset, 4 the custom layout), the gamma, brightness and contrast
// (+118-+120), then the records, rankings and what's unlocked (versus.cpp). A versus fight takes Versus Mode's options
// and both ports' button layouts from the settings of the machine it runs on, but in a session's Versus, where each
// player's layout and the host's options apply on both machines (versus.cpp).
#include "game/fight.h"

#include "game/journal.h"
#include "game/random.h"
#include "guest_abi.h"
#include "log.h"
#include "netplay/frame.h"

#include <cstring>

extern "C" PPC_FUNC(__imp__sub_823DF800);
extern "C" PPC_FUNC(__imp__sub_823DF880);
extern "C" PPC_FUNC(__imp__sub_82777348);
extern "C" PPC_FUNC(__imp__sub_82373980);

namespace
{
    enum class Fight
    {
        kNone,
        kStarted, // the main task started, not loading yet: the phase bytes are the last fight's
        kLoading,
        kFighting,
    };
    Fight g_fight = Fight::kNone; // main thread

    uint8_t Phase()
    {
        return *guest::ToHost(0x82FCC952);
    }

    uint8_t NextPhase()
    {
        return *guest::ToHost(0x830ACF85);
    }

    constexpr uint32_t kPlayers = 0x83033B40, kPlayerSize = 156; // the players' records
    // A player's record holds at +0x74 the address of the collision data of the floor they stand on (sub_8241A010 takes it
    // from sub_8255A078's answer), which lies in the memory of the stage's slot (0x82FCC967), the two 7 MB apart
    // (0x3D44xxxx in slot 0 and 0x3DB4xxxx in slot 1 in one run). Which slot a fight's stage is in depends on what the
    // machine loaded before (stage select's previews, and whether the chosen stage's had finished loading), so two
    // machines' addresses can differ in a fight that is the same on both.
    constexpr uint32_t kFloorData = 0x74;
    constexpr uint32_t kSettingsPointer = 0x83035CC8;
    constexpr uint32_t kGenerators[] = { 0x8357B5C0, 0x833F7600, 0x8321B198, 0x83420020 };
    constexpr uint32_t kGeneratorState = 2504;                  // the words, the next word's address and the words left

    std::vector<uint8_t> Bytes(uint32_t address, uint32_t length)
    {
        const uint8_t* bytes = guest::ToHost(address);
        return std::vector<uint8_t>(bytes, bytes + length);
    }

    void Append(std::vector<uint8_t>& to, uint32_t address, uint32_t length)
    {
        const uint8_t* bytes = guest::ToHost(address);
        to.insert(to.end(), bytes, bytes + length);
    }

    // A port's button layout as sub_822F3178 gives it to the fight and the AI (versus.cpp).
    std::vector<uint8_t> ButtonLayout(uint32_t port)
    {
        std::vector<uint8_t> layout(16);
        game::FightButtonLayout(port, layout.data());
        return layout;
    }
}

namespace game
{
    uint64_t FightChecksum()
    {
        uint64_t hash = 0xCBF29CE484222325ull; // FNV-1a
        const auto add = [&hash](uint32_t address, uint32_t length) {
            const uint8_t* bytes = guest::ToHost(address);
            for (uint32_t i = 0; i < length; i++)
                hash = (hash ^ bytes[i]) * 0x100000001B3ull;
        };
        add(0x830AD024, 2);      // the round's fighting frames
        add(0x82FCC952, 1);      // the main task's phase
        add(0x830ACF85, 1);      // and its next
        // The players' records, two of them, or four in a tag battle (the tag flag at 0x8303683A): a single fight never
        // touches the tag partners', which keep what the last tag fight left there (such as the title's attract demo's).
        // Each but its word at kFloorData, an address.
        const uint32_t players = *guest::ToHost(0x8303683A) ? 4 : 2;
        for (uint32_t player = 0; player < players; player++)
        {
            const uint32_t record = kPlayers + kPlayerSize * player;
            add(record, kFloorData);
            add(record + kFloorData + 4, kPlayerSize - kFloorData - 4);
        }
        add(0x833F7600, 2504);   // the fight's random number generator
        return hash;
    }

    std::vector<netplay::StatePart> FightSetup()
    {
        std::vector<netplay::StatePart> parts;
        std::vector<uint8_t> mode = Bytes(0x83035CC0, 4);
        Append(mode, 0x83035CC6, 2);
        Append(mode, 0x82FCC93B, 1);
        Append(mode, 0x82FCC966, 1);
        std::vector<uint8_t> sides = Bytes(0x83036844, 4);
        Append(sides, 0x83036862, 4);
        Append(sides, 0x8303683A, 1);
        std::vector<uint8_t> rules = Bytes(0x82FCC8EE, 1);
        for (const auto& [address, length] : { std::pair{ 0x82FCCC3Eu, 1u }, { 0x83033A7Au, 1u }, { 0x8304006Bu, 1u }, { 0x8303691Cu, 4u },
                 { 0x83031930u, 2u }, { 0x83036B3Fu, 1u }, { 0x82FCC8E3u, 1u } })
            Append(rules, address, length);
        parts.push_back({ "the stage", Bytes(0x82FCC96E, 1) });
        parts.push_back({ "the mode", std::move(mode) });
        parts.push_back({ "the characters and costumes", std::move(sides) });
        parts.push_back({ "the match's rules (difficulty, round time, life gauge, match points)", std::move(rules) });
        if (guest::LoadU32(kSettingsPointer))
        {
            parts.push_back({ "player 1's button layout", ButtonLayout(0) });
            parts.push_back({ "player 2's button layout", ButtonLayout(1) });
        }
        static const char* const kNames[] = { "the shared random number generator (8357B5C0)", "the fight's random number generator (833F7600)",
            "random number generator 8321B198", "random number generator 83420020" };
        for (size_t i = 0; i < std::size(kGenerators); i++)
            parts.push_back({ kNames[i], Bytes(kGenerators[i], kGeneratorState) });
        return parts;
    }

    std::vector<netplay::GameState::Region> DesyncRegions()
    {
        return {
            { "round frames 830AD024", 0x830AD024, 2 },
            { "phase 82FCC952", 0x82FCC952, 1 },
            { "next phase 830ACF85", 0x830ACF85, 1 },
            { "players 83033B40", 0x83033B40, 4 * 156 },
            { "player tasks 8320D110", 0x8320D110, 4 * 4336 },
            { "sides 83036844", 0x83036844, 60 },
            { "random 833F7600 (fight)", 0x833F7600, kGeneratorState },
            { "random 8357B5C0 (shared)", 0x8357B5C0, kGeneratorState },
            { "random 8321B198", 0x8321B198, kGeneratorState },
            { "random 83420020", 0x83420020, kGeneratorState },
        };
    }

    // After a session ended in a fight (the other player left): DOA Online's mode calls sub_823E7DA0 when a fighter leaves
    // (from its fighting callback, sub_823EF188). It gives the winner's side the match's rounds, zeroes the other side's
    // health, decides the match (0x830ACFBE) and ends the round (the next phase 5), so the match ends the way the game's
    // own flow goes, with its result and back to the menus. Only while a round is fought (phase 2). The winner's side is
    // the active player's (0x830B8C84) whose pad (0x830B8C88) is the user's.
    bool AbandonFight(PPCContext& ctx, uint8_t* base, uint32_t user)
    {
        if (g_fight != Fight::kFighting || Phase() != 2 || NextPhase() != 2)
            return false;
        uint32_t side = user & 1;
        for (uint32_t player = 0; player < 4; player++)
        {
            if (*guest::ToHost(0x830B8C84 + player) && *guest::ToHost(0x830B8C88 + player) == user)
            {
                side = player & 1;
                break;
            }
        }
        const PPCContext saved = ctx;
        ctx.r3.u64 = side;
        sub_823E7DA0(ctx, base);
        ctx = saved;
        LOG_INFO("Fight: the match given to side %u (game user %u) at frame %llu", side, user, (unsigned long long)netplay::Frame());
        return true;
    }

    // Where a press can end the fight (GameState::fightMenu): paused (the pause menu's Select Character, and Quit, which a
    // session's flow blocks: versus.cpp), or the match over: the next phase 0, which the round's result (5) sets at the
    // match's end (only the fight's loading has it otherwise, before its first frame), through its WINNER screen and the
    // result menu, whose Character Select and Again end the fight (in the scripted match from fight frame 4834, the
    // menu's Again at 4865); the phase and next phase are both 0 only once the fight has ended.
    bool FightMenu()
    {
        if (g_fight != Fight::kFighting)
            return false;
        constexpr uint32_t kPause = 0x830B8CE4; // +0 1 while paused (versus.cpp)
        return *guest::ToHost(kPause) == 1 || NextPhase() == 0;
    }

    void LogFightPhase()
    {
        static uint32_t logged = ~0u;
        if (g_fight == Fight::kNone)
        {
            logged = ~0u;
            return;
        }
        const uint32_t phases = Phase() | NextPhase() << 8;
        if (phases == logged)
            return;
        logged = phases;
        char fightFrame[24] = "-";
        if (const std::optional<uint64_t> frame = netplay::FightFrame())
            snprintf(fightFrame, sizeof(fightFrame), "%llu", (unsigned long long)*frame);
        LOG_INFO("Fight: phase %u, next %u, at frame %llu (fight frame %s)", Phase(), NextPhase(), (unsigned long long)netplay::Frame(),
            fightFrame);
    }

    netplay::GameState::FightState FightState()
    {
        if (g_fight == Fight::kStarted && Phase() == 0 && NextPhase() == 0)
            g_fight = Fight::kLoading;
        else if (g_fight == Fight::kLoading && (Phase() != 0 || NextPhase() != 0))
            g_fight = Fight::kFighting;
        // The title's attract sequence plays demo fights in the mode task's title phase (1), on each machine's own time
        // (menus.cpp); the players' fights are in its fight phase (4), whatever the mode.
        if (*guest::ToHost(0x830AD1B2) != 4)
            return netplay::GameState::FightState::kNone;
        switch (g_fight)
        {
        case Fight::kNone:
            return netplay::GameState::FightState::kNone;
        case Fight::kFighting:
            return netplay::GameState::FightState::kFighting;
        default:
            return netplay::GameState::FightState::kLoading;
        }
    }
}

// A fight starts: its main task (and 0x830AD0E4 set to 1). With netplay::FightSeed (fights that must play out the same on
// two machines), the players' records start the fight with no position, angles or motion: character select places its
// preview models through them as their files load (the cursor's character, from chr*.afs), which a slower disk does
// later or not at all, and the fight's loading moves the players on from there until its intro places them (11 frames
// into the fight), so those frames differed between machines that played the same menus.
PPC_FUNC(sub_823DF800)
{
    __imp__sub_823DF800(ctx, base);
    g_fight = Fight::kStarted;
    LOG_INFO("Fight: started at frame %llu", (unsigned long long)netplay::Frame());
    game::SeedRandomNumbers(ctx, base);
    if (netplay::FightSeed())
    {
        for (uint32_t player = 0; player < 4; player++)
        {
            const uint32_t record = kPlayers + kPlayerSize * player;
            guest::BeforeHostWrite(record, 0x78 + 24);
            memset(guest::ToHost(record + 0x04), 0, 12); // the position
            memset(guest::ToHost(record + 0x14), 0, 12); // the angles
            memset(guest::ToHost(record + 0x78), 0, 24); // two vectors of motion
        }
    }
}

// The sounds playing in a group: sub_82777348(engine, &ids, ...) writes the first one's ID, if any, where its callers put
// -1. The voice queries (the return addresses in sub_823E2F08, sub_823E7CF0 and sub_825100E0) find none, with
// NoVoiceWaits. Others ask the sound engine, through netplay's journal (journal.cpp).
PPC_FUNC(sub_82777348)
{
    switch (uint32_t(ctx.lr))
    {
    case 0x823E3090:
    case 0x823E7D2C:
    case 0x82510148:
        if (netplay::NoVoiceWaits())
            return;
        break;
    }
    game::JournalCall(0x82777348, __imp__sub_82777348, ctx, base);
}

// A player's timeline (r3, records at 0x8302BC98, 580 bytes a player: keyframes a frame apart, timed by the tick count
// the frame's end keeps, frame_time.cpp), each frame through sub_82372840: armed to start with its sound (+88 is 2), it
// asks the audio engine which sounds play (sub_82779690, whose only caller this is) and starts once its sound (+84) is
// among them, on the audio engine's time, and in a replay not with the first run's. With NoVoiceWaits it starts as it's
// armed (+88 1, which starts it at once).
PPC_FUNC(sub_82373980)
{
    if (netplay::NoVoiceWaits() && guest::LoadU32(ctx.r3.u32 + 88) == 2)
        guest::StoreU32(ctx.r3.u32 + 88, 1);
    __imp__sub_82373980(ctx, base);
}

// A fight ends: its main task.
PPC_FUNC(sub_823DF880)
{
    __imp__sub_823DF880(ctx, base);
    if (g_fight != Fight::kNone)
        LOG_INFO("Fight: ended at frame %llu", (unsigned long long)netplay::Frame());
    g_fight = Fight::kNone;
}
