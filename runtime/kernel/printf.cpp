// The kernel's C runtime formatting exports (sprintf, _vsnprintf, vsprintf, vswprintf), formatting
// guest arguments. Variadic arguments occupy 8-byte slots: r3..r10, then the caller's stack from
// r1 + 0x50; a va_list points at such slots. Doubles are passed as bit patterns in those slots.
#include "guest_abi.h"
#include "log.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace
{
    class Arguments
    {
    public:
        // Variadic arguments starting at argument index `first` of the current call.
        Arguments(PPCContext& ctx, size_t first) : m_ctx(&ctx), m_index(first) {}
        // Arguments in a guest va_list.
        explicit Arguments(uint32_t vaList) : m_vaList(vaList) {}

        uint64_t Next()
        {
            if (m_vaList)
            {
                const uint64_t value = *guest::Translate<be<uint64_t>>(m_vaList);
                m_vaList += 8;
                return value;
            }
            const size_t index = m_index++;
            if (index < 8)
                return guest::detail::ArgumentRegister(*m_ctx, index).u64;
            return *guest::Translate<be<uint64_t>>(m_ctx->r1.u32 + 0x50 + uint32_t(index - 8) * 8);
        }

    private:
        PPCContext* m_ctx = nullptr;
        size_t m_index = 0;
        uint32_t m_vaList = 0;
    };

    std::string Narrow(const be<uint16_t>* text, int maxLength = -1)
    {
        std::string result;
        for (int i = 0; text && text[i] != 0 && (maxLength < 0 || i < maxLength); i++)
        {
            const uint16_t c = text[i];
            result.push_back(c < 0x80 ? char(c) : '?');
        }
        return result;
    }

    // Formats a guest format string. `wideFormat` selects vswprintf semantics (%s is a wide string).
    std::string Format(const std::string& format, Arguments& args, bool wideFormat)
    {
        std::string out;
        for (size_t i = 0; i < format.size(); i++)
        {
            if (format[i] != '%')
            {
                out.push_back(format[i]);
                continue;
            }
            if (i + 1 < format.size() && format[i + 1] == '%')
            {
                out.push_back('%');
                i++;
                continue;
            }

            // %[flags][width][.precision][length]type
            std::string spec = "%";
            size_t j = i + 1;
            while (j < format.size() && strchr("-+ #0", format[j]))
                spec.push_back(format[j++]);
            if (j < format.size() && format[j] == '*')
            {
                spec += std::to_string(int32_t(args.Next()));
                j++;
            }
            while (j < format.size() && isdigit((unsigned char)format[j]))
                spec.push_back(format[j++]);
            int precision = -1;
            if (j < format.size() && format[j] == '.')
            {
                spec.push_back(format[j++]);
                if (j < format.size() && format[j] == '*')
                {
                    precision = int32_t(args.Next());
                    spec += std::to_string(precision);
                    j++;
                }
                else
                {
                    std::string digits;
                    while (j < format.size() && isdigit((unsigned char)format[j]))
                        digits.push_back(format[j++]);
                    precision = digits.empty() ? 0 : std::stoi(digits);
                    spec += digits;
                }
            }

            bool longLong = false, wide = wideFormat, narrow = false;
            while (j < format.size())
            {
                const char c = format[j];
                if (c == 'l' && j + 1 < format.size() && format[j + 1] == 'l') { longLong = true; j += 2; }
                else if (c == 'I' && format.compare(j, 3, "I64") == 0) { longLong = true; j += 3; }
                else if (c == 'l' || c == 'w') { wide = true; j++; }
                else if (c == 'h') { narrow = true; wide = false; j++; }
                else if (c == 'L' || c == 'z' || c == 't' || c == 'j') { j++; }
                else break;
            }
            if (j >= format.size())
                break;
            const char type = format[j];
            i = j;

            char buffer[512];
            switch (type)
            {
            case 'd':
            case 'i':
            {
                const uint64_t value = args.Next();
                snprintf(buffer, sizeof(buffer), (spec + "lld").c_str(), longLong ? (long long)value : (long long)int32_t(value));
                out += buffer;
                break;
            }
            case 'u':
            case 'x':
            case 'X':
            case 'o':
            {
                const uint64_t value = args.Next();
                snprintf(buffer, sizeof(buffer), (spec + "ll" + type).c_str(), longLong ? (unsigned long long)value : (unsigned long long)uint32_t(value));
                out += buffer;
                break;
            }
            case 'p':
                snprintf(buffer, sizeof(buffer), "%08X", uint32_t(args.Next()));
                out += buffer;
                break;
            case 'c':
            case 'C':
                out.push_back(char(args.Next()));
                break;
            case 'f':
            case 'F':
            case 'e':
            case 'E':
            case 'g':
            case 'G':
            case 'a':
            case 'A':
            {
                const uint64_t bits = args.Next();
                snprintf(buffer, sizeof(buffer), (spec + type).c_str(), std::bit_cast<double>(bits));
                out += buffer;
                break;
            }
            case 's':
            case 'S':
            {
                const uint32_t address = uint32_t(args.Next());
                // %S flips the width relative to the format's own.
                const bool wideString = type == 'S' ? !wideFormat : (narrow ? false : wide);
                std::string text;
                if (!address)
                    text = "(null)";
                else if (wideString)
                    text = Narrow(guest::Translate<be<uint16_t>>(address), precision);
                else
                    text = precision >= 0 ? std::string(guest::Translate<char>(address), strnlen(guest::Translate<char>(address), size_t(precision)))
                                          : std::string(guest::Translate<char>(address));
                snprintf(buffer, sizeof(buffer), (spec.substr(0, spec.find('.')) + "s").c_str(), text.c_str());
                out += buffer;
                break;
            }
            case 'n':
                args.Next(); // writing back the count isn't supported
                break;
            default:
                LOG_WARN("printf: unsupported conversion '%c' in \"%s\"", type, format.c_str());
                break;
            }
        }
        return out;
    }

    // Copies `text` into a guest narrow buffer with C-runtime _vsnprintf semantics.
    int32_t StoreNarrow(char* buffer, int64_t capacity, const std::string& text)
    {
        if (!buffer)
            return int32_t(text.size());
        if (capacity < 0)
        {
            memcpy(buffer, text.c_str(), text.size() + 1);
            return int32_t(text.size());
        }
        const size_t count = std::min<size_t>(text.size(), size_t(capacity));
        memcpy(buffer, text.data(), count);
        if (count < size_t(capacity))
            buffer[count] = 0;
        return text.size() > size_t(capacity) ? -1 : int32_t(text.size());
    }

    int32_t sprintf_(char* buffer, const char* format, PPCContext* ctx)
    {
        Arguments args(*ctx, 2);
        return StoreNarrow(buffer, -1, Format(format ? format : "", args, false));
    }

    int32_t _vsnprintf_(char* buffer, uint32_t count, const char* format, uint32_t vaList)
    {
        Arguments args(vaList);
        return StoreNarrow(buffer, count, Format(format ? format : "", args, false));
    }

    int32_t vsprintf_(char* buffer, const char* format, uint32_t vaList)
    {
        Arguments args(vaList);
        return StoreNarrow(buffer, -1, Format(format ? format : "", args, false));
    }

    int32_t vswprintf_(be<uint16_t>* buffer, const be<uint16_t>* format, uint32_t vaList)
    {
        Arguments args(vaList);
        const std::string text = Format(Narrow(format), args, true);
        if (buffer)
        {
            for (size_t i = 0; i < text.size(); i++)
                buffer[i] = uint16_t(uint8_t(text[i]));
            buffer[text.size()] = 0;
        }
        return int32_t(text.size());
    }

    void KiApcNormalRoutineNop(uint32_t, uint32_t) {}
}

GUEST_EXPORT(sprintf, sprintf_)
GUEST_EXPORT(_vsnprintf, _vsnprintf_)
GUEST_EXPORT(vsprintf, vsprintf_)
GUEST_EXPORT(vswprintf, vswprintf_)
GUEST_EXPORT(KiApcNormalRoutineNop, KiApcNormalRoutineNop)
