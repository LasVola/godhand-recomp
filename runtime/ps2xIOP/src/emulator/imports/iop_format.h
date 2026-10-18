#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

#include "../core/iop_cpu.h"
#include "../core/iop_memory.h"

namespace ps2x::iop::detail
{
    // Variable arguments of an IOP printf-family call. MIPS o32: argument words 0-3 are in a0-a3, the rest on the
    // caller's stack at sp+16, sp+20, ... (the import stub does not move sp). A va_list (vsprintf) is a pointer to the
    // next word in memory.
    class IopVarArgs
    {
    public:
        // Register arguments from index `first` (e.g. printf: 1, after the format in a0; sprintf: 2).
        IopVarArgs(const IopMemory &memory, const IopCpuState &cpu, uint32_t first) noexcept
            : m_memory(memory), m_cpu(&cpu), m_index(first)
        {
        }

        // A va_list in guest memory.
        IopVarArgs(const IopMemory &memory, uint32_t vaList) noexcept
            : m_memory(memory), m_list(vaList)
        {
        }

        uint32_t next32()
        {
            if (m_cpu == nullptr)
            {
                const uint32_t value = m_memory.read32(m_list);
                m_list += 4u;
                return value;
            }
            const uint32_t index = m_index++;
            return index < 4u ? m_cpu->gpr[4u + index] : m_memory.read32(m_cpu->gpr[29] + index * 4u);
        }

        uint64_t next64()
        {
            // 64-bit values start on an even word (o32 register pairs and stack slots alike).
            if (m_cpu == nullptr)
                m_list = (m_list + 7u) & ~7u;
            else
                m_index = (m_index + 1u) & ~1u;
            const uint64_t low = next32();
            const uint64_t high = next32();
            return low | (high << 32u);
        }

    private:
        const IopMemory &m_memory;
        const IopCpuState *m_cpu = nullptr;
        uint32_t m_index = 0u;
        uint32_t m_list = 0u;
    };

    // printf-style formatting for IOP log calls and sprintf: flags, width, precision (also '*'), h/hh/l/ll/z/j/t
    // lengths, d i u o x X c s p %. The IOP has no FPU and its SDKs print no floats; e/f/g consume a double and print
    // "<float>". %n is ignored. Strings are read from IOP memory (at most 4096 bytes each).
    inline std::string formatIopString(const IopMemory &memory, const std::string &format, IopVarArgs &args)
    {
        std::string out;
        out.reserve(format.size() + 16u);
        for (size_t i = 0; i < format.size(); ++i)
        {
            const char ch = format[i];
            if (ch != '%')
            {
                out.push_back(ch);
                continue;
            }
            const size_t start = i++;
            if (i >= format.size())
            {
                out.push_back('%');
                break;
            }
            if (format[i] == '%')
            {
                out.push_back('%');
                continue;
            }

            std::string spec = "%";
            while (i < format.size() && (format[i] == '-' || format[i] == '+' || format[i] == ' ' || format[i] == '#' || format[i] == '0'))
                spec.push_back(format[i++]);
            if (i < format.size() && format[i] == '*')
            {
                spec += std::to_string(static_cast<int32_t>(args.next32()));
                ++i;
            }
            else
            {
                while (i < format.size() && format[i] >= '0' && format[i] <= '9')
                    spec.push_back(format[i++]);
            }
            if (i < format.size() && format[i] == '.')
            {
                spec.push_back(format[i++]);
                if (i < format.size() && format[i] == '*')
                {
                    spec += std::to_string(static_cast<int32_t>(args.next32()));
                    ++i;
                }
                else
                {
                    while (i < format.size() && format[i] >= '0' && format[i] <= '9')
                        spec.push_back(format[i++]);
                }
            }
            bool wide = false;
            while (i < format.size() && (format[i] == 'h' || format[i] == 'l' || format[i] == 'z' || format[i] == 'j' ||
                                         format[i] == 't' || format[i] == 'L' || format[i] == 'q'))
            {
                if (format[i] == 'l' && i + 1 < format.size() && format[i + 1] == 'l')
                {
                    wide = true;
                    ++i;
                }
                else if (format[i] == 'L' || format[i] == 'q' || format[i] == 'j')
                    wide = true;
                ++i;
            }
            if (i >= format.size())
            {
                out.append(format, start, std::string::npos);
                break;
            }

            const char conversion = format[i];
            char buffer[128];
            switch (conversion)
            {
            case 'd':
            case 'i':
                if (wide)
                    std::snprintf(buffer, sizeof(buffer), (spec + "lld").c_str(), static_cast<long long>(args.next64()));
                else
                    std::snprintf(buffer, sizeof(buffer), (spec + "d").c_str(), static_cast<int32_t>(args.next32()));
                out += buffer;
                break;
            case 'u':
            case 'o':
            case 'x':
            case 'X':
                if (wide)
                    std::snprintf(buffer, sizeof(buffer), (spec + "ll" + conversion).c_str(),
                                  static_cast<unsigned long long>(args.next64()));
                else
                    std::snprintf(buffer, sizeof(buffer), (spec + conversion).c_str(), static_cast<unsigned int>(args.next32()));
                out += buffer;
                break;
            case 'p':
                std::snprintf(buffer, sizeof(buffer), "0x%08x", static_cast<unsigned int>(args.next32()));
                out += buffer;
                break;
            case 'c':
                std::snprintf(buffer, sizeof(buffer), (spec + "c").c_str(), static_cast<int>(args.next32() & 0xFFu));
                out += buffer;
                break;
            case 's':
            {
                const uint32_t address = args.next32();
                const std::string text = address != 0u ? memory.readString(address, 4096u) : std::string("(null)");
                const int needed = std::snprintf(nullptr, 0, (spec + "s").c_str(), text.c_str());
                if (needed > 0 && needed <= 8192)
                {
                    std::string formatted(static_cast<size_t>(needed) + 1u, '\0');
                    std::snprintf(formatted.data(), formatted.size(), (spec + "s").c_str(), text.c_str());
                    formatted.resize(static_cast<size_t>(needed));
                    out += formatted;
                }
                break;
            }
            case 'e':
            case 'E':
            case 'f':
            case 'F':
            case 'g':
            case 'G':
                (void)args.next64();
                out += "<float>";
                break;
            case 'n':
                (void)args.next32();
                break;
            default:
                // Unknown conversion: keep the text as written.
                out.append(format, start, i - start + 1u);
                break;
            }
        }
        return out;
    }
}
