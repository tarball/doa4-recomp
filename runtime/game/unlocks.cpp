// What DOA4's save unlocks (ANALYSIS.md, Phase 6: Lockstep only in Versus, and The complete save): everything unlocked in
// memory while a session's machine plays Versus, or with DEBUG_UNLOCK_ALL=1, without changing the save; and with
// DEBUG_COMPLETE_SAVE=1, a save holding everything the game itself unlocks, which the game writes.
//
// The profile's record is 67,840 bytes a user (0x831A90D8 + 67,840 x user; the user signed in at 0x831A4F30), of which
// the first 67,832 are saved (ups.dat, padded to 80 KB, with no checksum). The settings block (the pointer at 0x83035CC8)
// is the record's +31080, 36,752 bytes (its size word at +16544). What's unlocked is from +16320, 23 bytes a list, one a
// character's slot (0x82C59FF8 maps the 25 character IDs to slots, 0x82C5A014 slots to characters):
//   +16320  Movie Theater (mode select's entry 9: each story's ending), set as the first story is cleared.
//   +16322  each slot's costumes unlocked: two at first (sub_82306318's defaults from 0x82E40AB8, whose low nibble is the
//           costumes and bits 4-5 locked), up to 0x82C34888's count for the character, which sub_824082F0 adds one to as
//           modes are cleared (sub_82408198: story on Normal or harder with the rules at least the defaults, Time Attack's
//           single matches likewise).
//   +16345  whether the character is (sub_82408678, which announces it). Slots 3, 6, 7, 8, 12, 21 and
//           22 start locked: Ein, Gen Fu, Helena, Leon, Tengu, Alpha-152 and SPARTAN-458. sub_824083F0 unlocks Ein as
//           Hitomi's story is cleared, Gen Fu as Eliot's is, Leon as Zack's is, SPARTAN-458 as Helena's is (each on
//           Normal or harder: sub_824086D0), Helena once every story but those six characters' is cleared on Normal or
//           harder (+16391), and Tengu once Time Attack's single matches are cleared that way with every character but
//           Tengu and Alpha-152 (+16414). Nothing unlocks Alpha-152, the story's last opponent.
//   +16368  how many times each character's story was cleared (to 99): the stories are those of 0x82C34400's list, the
//           17 characters with an ending (0x82C5A030 gives each slot's movie, 20 none). Any unlocks stage 0x0C, one lets
//           its ending be skipped (sub_824046D8), and the Movie Theater lists the endings of those cleared (sub_8253A968).
//   +16391  each character's story cleared on Normal or harder (sub_82407FB8).
//   +16414  each character's Time Attack single matches cleared on Normal or harder (sub_82407FB8).
//   +16437  each character's system voice (sub_82408748, as Survival is cleared, game mode 9, which announces it), and
//           slot 21's, Alpha-152's (+16458), once all but slots 3 and 21 are. The SETTINGS screen lists the voices but
//           Ein's (sub_82520708).
// The rest of +16320-+16483 holds play counts (+16460, sub_823E37A0). Stage select's list (0x82C32AF8, 17 entries of 16
// bytes, RANDOM `FF` 12th, built by sub_82537458) takes stage 0x13 once SPARTAN-458 is unlocked (sub_82524490), 0x0C once
// any story is cleared (sub_82524540), and 0x17 once everything is (sub_82537350: every slot's costumes but Alpha-152's,
// every story in 0x82C34400's list, every system voice but Ein's). Unlocks announce themselves through a queue of 8
// (type, ID) pairs at 0x830B8F08, its count at 0x830B8F20 (sub_82408870: 0 a costume, 1 a character, 2 a system voice,
// 3 the Movie Theater, 5 an achievement). The records (Time Attack's and Survival's, from +124) and the online section
// (+16560: DOA Online's profile, Zack's Shop's goods as bitmaps from its +3200 and its Zack money, a 64-bit count at its
// +20168) gate nothing offline.
//
// Character select (sub_82525E80), its portraits (sub_823A4FC8 and others), RANDOM and the mode setups read the lists
// directly. In a session's Versus, or always with DEBUG_UNLOCK_ALL=1, each list gets everything unlocked in memory
// (Alpha-152 too), and the save's own bytes go back as the flow ends. The profile manager (sub_8230DCD8, each frame from
// the mode task) copies the record into its buffer to save it (`[0x82FE04F8]`, at 0x8230E090) and loads into it from
// there: it runs with the save's own bytes in place, so no save ever holds the session's unlocks, and a load replaces them
// as it would.
//
// DEBUG_COMPLETE_SAVE=1 sets what the game would hold once everything is unlocked: the Movie Theater, every costume,
// every character but Alpha-152, every story cleared once (on Normal), Time Attack cleared with every character and every
// system voice; play counts, records and the online section stay as they are. It does so as the profile manager is idle
// with the profile loaded (or made), then asks the game to save it (sub_8230E930, as the game does when it saves), so the
// game writes ups.dat itself. Use it in a data directory of its own (tools\completesave.ps1).
#include "game/fight.h"
#include "guest_abi.h"
#include "log.h"
#include "netplay/frame.h"
#include "netplay/session.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <iterator>

extern "C" PPC_FUNC(__imp__sub_8230DCD8);

namespace
{
    constexpr uint32_t kSettingsPointer = 0x83035CC8;
    constexpr uint32_t kRecords = 0x831A90D8, kRecordSize = 67840, kSettingsInRecord = 31080;
    constexpr uint32_t kRecordLoaded = 0x108FC; // a byte of the record: its data is the save's (loaded or made)
    constexpr uint32_t kSignedInUser = 0x831A4F30;
    constexpr uint32_t kSettingsSize = 16544, kSettingsSizeValue = 36752; // the settings block's size word
    // The profile manager: its phase (0 idle), and its queue of 8 commands (0x10 | user saves, 0x20 | user loads, 0x40 |
    // user makes a profile's save), the next at +8.
    constexpr uint32_t kManagerPhase = 0x82FE0500, kManagerQueue = 0x8320C6E0;

    constexpr uint32_t kMovieTheater = 16320;
    constexpr uint32_t kCostumes = 16322, kCharacters = 16345, kStories = 16368, kStoriesNormal = 16391, kTimeAttackNormal = 16414,
                       kVoices = 16437;
    constexpr uint32_t kUnlockLists[] = { kCostumes, kCharacters, kStories, kVoices };
    constexpr uint32_t kSlots = 23;
    constexpr uint32_t kSlotCharacters = 0x82C5A014, kCharacterSlots = 0x82C59FF8, kCostumeCounts = 0x82C34888;
    constexpr uint32_t kStoryList = 0x82C34400, kStoryListEntries = 18, kStoryListStride = 40;
    constexpr uint32_t kAlpha152 = 21; // the slot nothing unlocks

    uint32_t Settings()
    {
        return guest::LoadU32(kSettingsPointer);
    }

    // A byte of the settings, to read or write (a write the store map may want to see first: guest_memory.h).
    uint8_t& Byte(uint32_t settings, uint32_t offset)
    {
        guest::BeforeHostWrite(settings + offset, 1);
        return *guest::ToHost(settings + offset);
    }

    uint8_t CostumeCount(uint32_t slot)
    {
        const uint8_t character = *guest::ToHost(kSlotCharacters + slot);
        return character <= 24 ? *guest::ToHost(kCostumeCounts + character) : 0;
    }

    // Whether the slot's character has a story (0x82C34400's list).
    bool HasStory(uint32_t slot)
    {
        for (uint32_t entry = 0; entry < kStoryListEntries; entry++)
        {
            const uint8_t character = *guest::ToHost(kStoryList + kStoryListStride * entry);
            if (character <= 24 && *guest::ToHost(kCharacterSlots + character) == slot)
                return true;
        }
        return false;
    }

    // Everything unlocked in memory (UpdateUnlocks).
    struct Unlocks
    {
        bool applied = false;
        uint32_t settings = 0; // the block they're applied to
        uint8_t own[std::size(kUnlockLists)][kSlots] = {};
    };
    Unlocks g_unlocks; // main thread

    bool UnlockAllSwitch()
    {
        static const bool on = [] {
            const char* setting = std::getenv(GAME_PREFIX "_DEBUG_UNLOCK_ALL");
            return setting && std::atoi(setting) != 0;
        }();
        return on;
    }

    void CaptureOwn()
    {
        Unlocks& u = g_unlocks;
        for (size_t list = 0; list < std::size(kUnlockLists); list++)
            memcpy(u.own[list], guest::ToHost(u.settings + kUnlockLists[list]), kSlots);
    }

    void RestoreOwn()
    {
        Unlocks& u = g_unlocks;
        for (size_t list = 0; list < std::size(kUnlockLists); list++)
        {
            guest::BeforeHostWrite(u.settings + kUnlockLists[list], kSlots);
            memcpy(guest::ToHost(u.settings + kUnlockLists[list]), u.own[list], kSlots);
        }
    }

    void UnlockEverything()
    {
        const uint32_t settings = g_unlocks.settings;
        for (uint32_t slot = 0; slot < kSlots; slot++)
        {
            uint8_t& costumes = Byte(settings, kCostumes + slot);
            costumes = std::max(costumes, CostumeCount(slot));
            for (size_t list = 1; list < std::size(kUnlockLists); list++)
                Byte(settings, kUnlockLists[list] + slot) = 1;
        }
    }

    // The complete save (DEBUG_COMPLETE_SAVE=1), from the profile manager's frame on the main thread.
    enum class CompleteSave
    {
        kOff,
        kWaiting, // for the profile loaded (or made) and the manager idle
        kSaving,  // the game's save asked for
        kDone,
    };
    CompleteSave g_complete = CompleteSave::kOff;
    bool g_saveStarted = false;

    bool CompleteSaveSwitch()
    {
        static const bool on = [] {
            const char* setting = std::getenv(GAME_PREFIX "_DEBUG_COMPLETE_SAVE");
            return setting && std::atoi(setting) != 0;
        }();
        return on;
    }

    bool ManagerIdle()
    {
        if (guest::LoadU32(kManagerPhase) != 0)
            return false;
        for (uint32_t i = 0; i < 8; i++)
        {
            if (*guest::ToHost(kManagerQueue + i))
                return false;
        }
        return true;
    }

    // The signed-in user's settings block once its record holds the save, else 0.
    uint32_t LoadedSettings(uint32_t& user)
    {
        const uint32_t settings = Settings();
        user = guest::LoadU32(kSignedInUser);
        if (!settings || user >= 4 || settings != kRecords + kRecordSize * user + kSettingsInRecord)
            return 0;
        if (!*guest::ToHost(kRecords + kRecordSize * user + kRecordLoaded) || guest::LoadU32(settings + kSettingsSize) != kSettingsSizeValue)
            return 0;
        return settings;
    }

    // Sets what the game holds once everything is unlocked; returns the bytes changed.
    uint32_t CompleteUnlocks(uint32_t settings)
    {
        uint32_t changed = 0;
        const auto set = [&](uint32_t offset, uint8_t value) {
            uint8_t& byte = Byte(settings, offset);
            if (byte < value)
            {
                byte = value;
                changed++;
            }
        };
        set(kMovieTheater, 1);
        for (uint32_t slot = 0; slot < kSlots; slot++)
        {
            set(kCostumes + slot, CostumeCount(slot));
            set(kVoices + slot, 1);
            if (slot == kAlpha152)
                continue;
            set(kCharacters + slot, 1);
            set(kTimeAttackNormal + slot, 1);
            if (HasStory(slot))
            {
                set(kStories + slot, 1);
                set(kStoriesNormal + slot, 1);
            }
        }
        return changed;
    }

    void DescribeUnlocks(uint32_t settings, const char* when)
    {
        uint32_t characters = 0, costumes = 0, costumesMax = 0, stories = 0, voices = 0;
        for (uint32_t slot = 0; slot < kSlots; slot++)
        {
            characters += Byte(settings, kCharacters + slot) != 0;
            costumes += Byte(settings, kCostumes + slot);
            costumesMax += CostumeCount(slot);
            stories += Byte(settings, kStories + slot) != 0;
            voices += Byte(settings, kVoices + slot) != 0;
        }
        LOG_INFO("Unlocks %s: %u of %u characters, %u of %u costumes, %u stories cleared, %u system voices, Movie Theater %s", when,
            characters, kSlots, costumes, costumesMax, stories, voices, Byte(settings, kMovieTheater) ? "on" : "off");
    }

    void UpdateCompleteSave(PPCContext& ctx, uint8_t* base)
    {
        if (g_complete == CompleteSave::kOff && CompleteSaveSwitch())
            g_complete = CompleteSave::kWaiting;
        if (g_complete != CompleteSave::kWaiting && g_complete != CompleteSave::kSaving)
            return;
        uint32_t user = 0;
        const uint32_t settings = LoadedSettings(user);
        const bool idle = ManagerIdle();
        if (g_complete == CompleteSave::kWaiting)
        {
            if (!settings || !idle)
                return;
            DescribeUnlocks(settings, "in the save");
            const uint32_t changed = CompleteUnlocks(settings);
            DescribeUnlocks(settings, "now");
            const PPCContext saved = ctx;
            ctx.r3.u64 = user;
            sub_8230E930(ctx, base);
            const int32_t result = ctx.r3.s32;
            ctx = saved;
            if (result != 0)
            {
                LOG_ERROR("Complete save: the game didn't take the request to save (%d); nothing saved", result);
                g_complete = CompleteSave::kDone;
                return;
            }
            LOG_INFO("Complete save: %u bytes of the save changed, the game asked to save it, at frame %llu", changed,
                (unsigned long long)netplay::Frame());
            g_saveStarted = false;
            g_complete = CompleteSave::kSaving;
            return;
        }
        if (!idle)
        {
            g_saveStarted = true;
            return;
        }
        if (g_saveStarted)
        {
            LOG_INFO("Complete save: written by the game, at frame %llu", (unsigned long long)netplay::Frame());
            g_complete = CompleteSave::kDone;
        }
    }

    // Logs the unlocks of each save the profile manager loads (or makes), for checking a save.
    void LogLoadedUnlocks()
    {
        static uint32_t logged = 0;
        uint32_t user = 0;
        const uint32_t settings = LoadedSettings(user);
        if (!settings || !ManagerIdle())
        {
            if (!settings)
                logged = 0;
            return;
        }
        if (logged == settings)
            return;
        logged = settings;
        DescribeUnlocks(settings, "loaded");
    }
}

namespace game
{
    void UpdateUnlocks()
    {
        Unlocks& u = g_unlocks;
        const bool wanted = UnlockAllSwitch() || netplay::session::PlaysFlow();
        const uint32_t settings = Settings();
        if (u.applied && (!wanted || settings != u.settings))
        {
            RestoreOwn();
            u.applied = false;
            LOG_INFO("Versus: the save's own unlocks again, at frame %llu", (unsigned long long)netplay::Frame());
        }
        if (wanted && !u.applied && settings)
        {
            u.settings = settings;
            CaptureOwn();
            UnlockEverything();
            u.applied = true;
            LOG_INFO("Versus: everything unlocked (%s), in memory only, at frame %llu", UnlockAllSwitch() ? "DEBUG_UNLOCK_ALL" : "the session's Versus",
                (unsigned long long)netplay::Frame());
        }
    }
}

// The profile manager's frame: saves (copying the record into its buffer) and loads with the save's own unlocks in
// place; then the complete save's steps, on the save's own bytes too.
PPC_FUNC(sub_8230DCD8)
{
    Unlocks& u = g_unlocks;
    if (u.applied)
        RestoreOwn();
    __imp__sub_8230DCD8(ctx, base);
    LogLoadedUnlocks();
    UpdateCompleteSave(ctx, base);
    if (!u.applied)
        return;
    u.settings = Settings();
    if (!u.settings)
    {
        u.applied = false;
        return;
    }
    CaptureOwn();
    UnlockEverything();
}
