// SETTINGS > VIDEO with the host's graphics options (DOA4-specific addresses).
//
// DOA4's VIDEO screen has four rows: the Gamma, Brightness and Contrast sliders, then Default. Its update function
// (sub_8251F120, from the screen's state machine entry) moves the cursor and adjusts the sliders, and its draw function
// (sub_823A3A70, the per-frame function of the screen's UI task) draws each row's label, value and panel (with the
// slider) and the cursor row's description, all hand-coded for those rows. Both are replaced by ports that add the
// host's options (settings::Graphics) as rows above DOA4's, drawn and driven by the same game functions as its own
// rows, so they look and behave alike. The screen's cursor counts the added rows first. Default resets them too.
#include "guest_abi.h"
#include "settings.h"

#include <algorithm>
#include <bit>
#include <cstdarg>
#include <cstdio>
#include <initializer_list>
#include <map>
#include <string>
#include <string_view>

namespace
{
    // DOA4's settings screens.
    constexpr uint32_t kVideoCursor = 0x83101F8C; // u16: the VIDEO screen's cursor row (in the screens' cursors)
    constexpr uint32_t kMenuId = 0x831A64A0;      // the shown screen's menu, for its descriptions (VIDEO: 14)
    constexpr uint32_t kRowLayout = 0x82C7A53C;   // its row panels: menu, style, x and y (u16), row spacing (u16), slide-in frames
    constexpr uint32_t kRowPanel = 0x8303F068;    // the row panel being drawn: +1 selected (u8), +16 slider (0-100), +20 last row
    constexpr uint32_t kVideoMenu = 14;           // VIDEO for sub_82520DA8 (row counts)
    constexpr uint32_t kVideoLabels = 13;         // VIDEO for sub_825208D8 (row labels)
    constexpr uint32_t kEmptyString = 0x821FBA80;
    constexpr uint32_t kColor = 0xFF888888, kSelectedColor = 0xCC444444; // label and value text
    // VIDEO's values, applied through a gamma ramp, and the game settings (in the save) that keep them in bytes 118-120:
    // gamma x 10, brightness / 5 and contrast x 2.
    constexpr uint32_t kGamma = 0x83100F00, kBrightness = 0x831008F0, kContrast = 0x83100F14;
    constexpr uint32_t kGammaRamp = 0x831008F8;
    constexpr uint32_t kGameSettings = 0x83035CC8;
    constexpr uint32_t kBrightnessMin = 0x82BB7790; // a float in .data (-50)
    constexpr uint32_t kOff = 752, kOn = 751;       // DOA4's messages

    float LoadF32(uint32_t address)
    {
        return std::bit_cast<float>(guest::LoadU32(address));
    }

    void StoreF32(uint32_t address, float value)
    {
        guest::StoreU32(address, std::bit_cast<uint32_t>(value));
    }

    uint16_t LoadU16(uint32_t address)
    {
        const uint8_t* bytes = guest::ToHost(address);
        return uint16_t(bytes[0] << 8 | bytes[1]);
    }

    void StoreU16(uint32_t address, uint16_t value)
    {
        guest::BeforeHostWrite(address, 2);
        uint8_t* bytes = guest::ToHost(address);
        bytes[0] = uint8_t(value >> 8);
        bytes[1] = uint8_t(value);
    }

    void StoreU8(uint32_t address, uint8_t value)
    {
        guest::BeforeHostWrite(address, 1);
        *guest::ToHost(address) = value;
    }

    // Calls guest code with integer arguments from r3 (floats go in ctx.f1 and on beforehand); returns r3.
    uint32_t Call(PPCContext& ctx, uint8_t* base, void (*function)(PPCContext&, uint8_t*), std::initializer_list<uint64_t> arguments = {})
    {
        PPCRegister* const registers[] = { &ctx.r3, &ctx.r4, &ctx.r5, &ctx.r6, &ctx.r7, &ctx.r8, &ctx.r9, &ctx.r10 };
        size_t i = 0;
        for (const uint64_t argument : arguments)
            registers[i++]->u64 = argument;
        function(ctx, base);
        return ctx.r3.u32;
    }

    // Plays a menu sound (9: the cursor's).
    void PlaySound(PPCContext& ctx, uint8_t* base, uint32_t sound)
    {
        ctx.f1.f64 = 1.0;
        Call(ctx, base, sub_82543010, { sound });
    }

    // Text for guest code: UTF-16BE strings in guest memory, kept for the process (they're few and repeat).
    uint32_t GuestText(const std::u16string& text)
    {
        static std::map<std::u16string, uint32_t> strings;
        static uint32_t block = 0, used = 0, size = 0;
        if (const auto it = strings.find(text); it != strings.end())
            return it->second;
        const uint32_t bytes = uint32_t(text.size() + 1) * 2;
        if (!block || used + bytes > size)
        {
            size = std::max(bytes, 0x1000u);
            block = guest::SystemAlloc(size);
            used = 0;
            if (!block)
                return kEmptyString;
        }
        const uint32_t address = block + used;
        for (size_t i = 0; i < text.size(); i++)
            StoreU16(address + uint32_t(i) * 2, text[i]);
        StoreU16(address + uint32_t(text.size()) * 2, 0);
        used += (bytes + 3) & ~3u;
        strings.emplace(text, address);
        return address;
    }

    std::u16string Format(const char* format, ...)
    {
        char text[256];
        va_list arguments;
        va_start(arguments, format);
        vsnprintf(text, sizeof(text), format, arguments);
        va_end(arguments);
        return std::u16string(text, text + strlen(text));
    }

    // The host's options: each a list of choices that left and right step through, shown with a slider like DOA4's.
    struct Option
    {
        const char* label;
        uint32_t choices;
        uint32_t (*get)(const settings::Graphics& graphics);
        void (*set)(settings::Graphics& graphics, uint32_t choice);
        // The value box's text (as a string, or one of DOA4's messages if `message` gives one) and the description.
        std::u16string (*value)(uint32_t choice);
        uint32_t (*message)(uint32_t choice);
        std::u16string (*description)(uint32_t choice);
    };

    constexpr uint32_t kSamples[] = { 0, 4, 8 };
    constexpr uint32_t kAnisotropy[] = { 0, 2, 4, 8, 16 };

    template <size_t N>
    uint32_t IndexOf(const uint32_t (&values)[N], uint32_t value)
    {
        return uint32_t(std::find(values, values + N, value) - values) % N;
    }

    const Option kOptions[] = {
        {
            "Fullscreen", 2,
            [](const settings::Graphics& graphics) { return graphics.fullscreen ? 1u : 0u; },
            [](settings::Graphics& graphics, uint32_t choice) { graphics.fullscreen = choice != 0; },
            nullptr,
            [](uint32_t choice) { return choice ? kOn : kOff; },
            [](uint32_t choice) {
                return choice ? Format("Fills the screen with a borderless window. Alt+Enter switches back.")
                              : Format("Shows the game in a window. Alt+Enter switches to fullscreen.");
            },
        },
        {
            "Resolution", 4,
            [](const settings::Graphics& graphics) { return graphics.resolutionScale - 1; },
            [](settings::Graphics& graphics, uint32_t choice) { graphics.resolutionScale = choice + 1; },
            [](uint32_t choice) { return Format("%ux", choice + 1); },
            nullptr,
            [](uint32_t choice) {
                return choice == 0 ? Format("Renders at 1280x720, the original resolution.")
                                   : Format("Renders at %ux%u, sharper but harder on the GPU.", 1280 * (choice + 1), 720 * (choice + 1));
            },
        },
        {
            "Anti-Aliasing", 3,
            [](const settings::Graphics& graphics) { return IndexOf(kSamples, graphics.msaaSamples); },
            [](settings::Graphics& graphics, uint32_t choice) { graphics.msaaSamples = kSamples[choice]; },
            [](uint32_t choice) { return Format("%ux", choice == 0 ? 2 : kSamples[choice]); },
            nullptr,
            [](uint32_t choice) {
                return choice == 0 ? Format("Smooths polygon edges with 2x MSAA, as the original.")
                                   : Format("Smooths polygon edges with %ux MSAA.", kSamples[choice]);
            },
        },
        {
            "Alpha to Coverage", 2,
            [](const settings::Graphics& graphics) { return graphics.alphaToCoverage ? 1u : 0u; },
            [](settings::Graphics& graphics, uint32_t choice) { graphics.alphaToCoverage = choice != 0; },
            nullptr,
            [](uint32_t choice) { return choice ? kOn : kOff; },
            [](uint32_t) { return Format("Smooths the edges of leaves and other cut-out shapes with MSAA."); },
        },
        {
            "Texture Filtering", 5,
            [](const settings::Graphics& graphics) { return IndexOf(kAnisotropy, graphics.anisotropy); },
            [](settings::Graphics& graphics, uint32_t choice) { graphics.anisotropy = kAnisotropy[choice]; },
            [](uint32_t choice) { return Format("%ux", kAnisotropy[choice]); },
            [](uint32_t choice) { return choice == 0 ? kOff : 0u; },
            [](uint32_t choice) {
                return choice == 0 ? Format("Filters textures as the original.")
                                   : Format("Keeps textures sharp at an angle with %ux anisotropic filtering.", kAnisotropy[choice]);
            },
        },
    };
    constexpr uint32_t kHostRows = uint32_t(std::size(kOptions));

    // The text y of a row (in DOA4's 720x480 menu space): DOA4's rows from 304, 24 apart, with the host's above.
    int32_t RowY(uint32_t row)
    {
        return 304 + 24 * (int32_t(row) - int32_t(kHostRows));
    }

    uint32_t ValueText(PPCContext& ctx, uint8_t* base, const Option& option, uint32_t choice)
    {
        if (option.message)
            if (const uint32_t message = option.message(choice))
                return Call(ctx, base, sub_82388170, { message, 255 });
        return GuestText(option.value(choice));
    }
}

// The VIDEO screen's update, each frame (its state machine entry's): returns 1.
PPC_FUNC(sub_8251F120)
{
    const uint32_t rows = kHostRows + Call(ctx, base, sub_82520DA8, { kVideoMenu });
    Call(ctx, base, sub_8251C2D0, { kVideoCursor, rows }); // up and down
    const uint32_t row = LoadU16(kVideoCursor);
    if (row < kHostRows)
    {
        // Right (first) or left steps through the choices, stopping at the ends, as the sliders do.
        const Option& option = kOptions[row];
        settings::Graphics graphics = settings::GetGraphics();
        const uint32_t choice = option.get(graphics);
        uint32_t next = choice;
        if (Call(ctx, base, sub_8251C1F0))
            next = std::min(choice + 1, option.choices - 1);
        else if (Call(ctx, base, sub_8251C110))
            next = choice > 0 ? choice - 1 : 0;
        if (next != choice)
        {
            option.set(graphics, next);
            settings::SetGraphics(graphics);
            PlaySound(ctx, base, 9);
        }
    }
    else if (row - kHostRows < 3)
    {
        // A slider: maximum, minimum and step.
        static const uint32_t values[] = { kGamma, kBrightness, kContrast };
        const float limits[][3] = { { 2.0f, 1.0f, 0.1f }, { 50.0f, LoadF32(kBrightnessMin), 5.0f }, { 10.0f, -10.0f, 0.5f } };
        const float* slider = limits[row - kHostRows];
        ctx.f1.f64 = slider[0];
        ctx.f2.f64 = slider[1];
        ctx.f3.f64 = slider[2];
        Call(ctx, base, sub_8251C7F8, { values[row - kHostRows] });
    }

    // Keep the values in the game settings, and apply them.
    const uint32_t game = guest::LoadU32(kGameSettings);
    StoreU8(game + 118, uint8_t(int64_t(LoadF32(kGamma) * 10.0f)));
    StoreU8(game + 119, uint8_t(int32_t(LoadF32(kBrightness) * 0.2f)));
    StoreU8(game + 120, uint8_t(int32_t(LoadF32(kContrast) * 2.0f)));
    Call(ctx, base, sub_824F0A08);
    Call(ctx, base, sub_82758F80, { guest::LoadU32(0x83579C64), 0, kGammaRamp });

    if (Call(ctx, base, sub_8251CA28, { row, rows })) // back (B)
    {
        settings::Save();
        Call(ctx, base, sub_82536C88, { 1 });
    }
    else if (Call(ctx, base, sub_8251D0D0, { row, rows })) // Default (A on the last row)
    {
        StoreU8(game + 119, 0);
        StoreU8(game + 120, 0);
        StoreU8(game + 118, 10);
        StoreF32(kGamma, 10.0f * 0.1f);
        StoreF32(kContrast, 0.0f);
        StoreF32(kBrightness, 0.0f);
        Call(ctx, base, sub_824F0A08);
        Call(ctx, base, sub_82758F80, { guest::LoadU32(0x83579C64), 0, kGammaRamp });
        settings::SetGraphics(settings::Graphics{});
    }
    ctx.r3.u64 = 1;
}

// The VIDEO screen's UI task, each frame: draws it. r3 is the task (+0 state, 4 when closing; +16 fade, 0-1). Returns 1.
PPC_FUNC(sub_823A3A70)
{
    const uint32_t task = ctx.r3.u32;
    const uint32_t caller = ctx.r1.u32;
    ctx.r1.u64 = caller - 384;
    guest::StoreU32(ctx.r1.u32, caller);
    const uint32_t text = ctx.r1.u32 + 128;   // a text item (36 bytes)
    const uint32_t layout = ctx.r1.u32 + 176; // the row layout, moved up for the host's rows (32 bytes)
    const uint32_t prompt = ctx.r1.u32 + 224; // a description's button prompt (64 characters)

    Call(ctx, base, sub_82388268);
    StoreU8(0x8309A2C9, 1);
    Call(ctx, base, sub_823BF960, { 6, 0 });
    Call(ctx, base, sub_82388268);
    Call(ctx, base, sub_823BF868, { 720, 480 });
    const uint32_t frames = guest::LoadU32(0x831A5974) + 1;
    guest::StoreU32(0x831A5974, frames);
    StoreF32(0x831A5F5C, 1.0f);
    StoreF32(0x82C68CEC, 0.5f);
    StoreF32(0x82C68CF0, 0.5f);
    const float fade = LoadF32(task + 16);
    StoreF32(0x8303EAF4, fade);
    // The cursor's pulse: from 230 to 128 and back over 120 frames.
    StoreF32(0x8303F064, float(int32_t(Call(ctx, base, sub_8238B520, { 230, 128, frames, 60 }))) * (1.0f / 256.0f));

    const settings::Graphics graphics = settings::GetGraphics();
    const float gameValues[] = { LoadF32(kGamma), LoadF32(kBrightness), LoadF32(kContrast) };
    const uint32_t gameRows = Call(ctx, base, sub_82520DA8, { kVideoMenu });
    const uint32_t rows = kHostRows + gameRows;
    const uint32_t cursor = LoadU16(kVideoCursor);

    // Labels: right-aligned at x 340.
    StoreU16(text + 0, 36);
    StoreU16(text + 2, 264);
    guest::StoreU32(text + 4, kEmptyString);
    StoreF32(text + 8, 340.0f);
    StoreF32(text + 12, 0.0f);
    guest::StoreU32(text + 16, 0xFFFFFFFF);
    StoreU8(text + 20, 0x80);
    StoreU8(text + 21, 1);
    StoreU8(text + 22, 0x21);
    StoreU8(text + 23, 0x30);
    StoreF32(text + 24, 0.0f);
    StoreF32(text + 28, 0.85f);
    StoreF32(text + 32, 0.85f);
    for (uint32_t row = 0; row < rows; row++)
    {
        const uint32_t label = row < kHostRows ? GuestText(Format("%s", kOptions[row].label))
                                               : Call(ctx, base, sub_825208D8, { row - kHostRows, kVideoLabels });
        guest::StoreU32(text + 4, label);
        StoreF32(text + 12, float(RowY(row)));
        ctx.f1.f64 = 1.0;
        ctx.f2.f64 = fade;
        guest::StoreU32(text + 16, Call(ctx, base, sub_82388310, { row == cursor ? kSelectedColor : kColor }));
        Call(ctx, base, sub_823BB568, { text, 0 });
    }

    // Values: right-aligned at x 410, the sliders' with one decimal.
    for (uint32_t row = 0; row < rows; row++)
    {
        uint32_t value = 0;
        if (row < kHostRows)
            value = ValueText(ctx, base, kOptions[row], kOptions[row].get(graphics));
        else if (row - kHostRows < 3)
        {
            const double number = gameValues[row - kHostRows];
            ctx.f1.f64 = number;
            Call(ctx, base, sub_82307498, { text, 0x8226ABE8, std::bit_cast<uint64_t>(number) }); // L"%2.1f"
            value = text;
        }
        else
            continue;
        ctx.f1.f64 = fade;
        ctx.f2.f64 = 0.85f;
        ctx.f3.f64 = 0.85f;
        Call(ctx, base, sub_823A7B58,
            { value, 410, uint64_t(int64_t(RowY(row))), 1, 180, 0, row == cursor ? kSelectedColor : kColor, 128 });
    }

    // Panels (label, value box and slider), sliding in over 12 frames.
    const uint32_t slideIn = guest::LoadU32(kRowLayout + 16);
    if (slideIn < 12)
        guest::StoreU32(kRowLayout + 16, slideIn + 1);
    for (uint32_t i = 0; i < 32; i += 4)
        guest::StoreU32(layout + i, guest::LoadU32(kRowLayout + i));
    StoreU16(layout + 10, uint16_t(LoadU16(kRowLayout + 10) - kHostRows * LoadU16(kRowLayout + 12)));
    const uint32_t panels = kHostRows + Call(ctx, base, sub_82520DA8, { guest::LoadU32(kRowLayout) });
    for (uint32_t row = 0; row < panels; row++)
    {
        StoreU8(kRowPanel + 1, row == cursor);
        guest::StoreU32(kRowPanel + 20, row == panels - 1);
        if (row < kHostRows)
            StoreF32(kRowPanel + 16, float(kOptions[row].get(graphics) * 100) / float(kOptions[row].choices - 1));
        else if (row == kHostRows)
            StoreF32(kRowPanel + 16, (gameValues[0] - 1.0f) * 100.0f);
        else if (row == kHostRows + 1)
            StoreF32(kRowPanel + 16, (gameValues[1] + 50.0f) * 0.01f * 100.0f);
        else if (row == kHostRows + 2)
            StoreF32(kRowPanel + 16, (gameValues[2] + 10.0f) * 0.05f * 100.0f);
        Call(ctx, base, sub_823A11A8, { task, layout, row, 0, 0 });
    }

    // The cursor row's description, with the button prompt, unless the screen is closing.
    if (guest::LoadU32(task) != 4)
    {
        if (cursor >= kHostRows)
        {
            ctx.f1.f64 = fade;
            Call(ctx, base, sub_823A1DE0, { guest::LoadU32(kMenuId), cursor - kHostRows });
        }
        else
        {
            // As sub_823A1DE0 shows a slider's.
            for (uint32_t i = 0; i < 128; i += 4)
                guest::StoreU32(prompt + i, 0);
            Call(ctx, base, sub_8278FB80, { prompt, Call(ctx, base, sub_823B2A48, { 0, 1 }) });
            const Option& option = kOptions[cursor];
            const bool shown = guest::LoadU32(0x831A5970) == 0 && guest::LoadU32(0x8303EDE4) == 2;
            Call(ctx, base, sub_823B26D8, { 0, 1, shown ? GuestText(option.description(option.get(graphics))) : 0, prompt, 0 });
        }
    }

    Call(ctx, base, sub_823BF868, { guest::LoadU32(0x8357D0D0), guest::LoadU32(0x8357D0D4) });
    StoreF32(0x831A5F5C, 0.0f);
    StoreF32(0x82C68CEC, 1.0f);
    StoreF32(0x82C68CF0, 1.0f);
    ctx.r1.u64 = caller;
    ctx.r3.u64 = 1;
}
