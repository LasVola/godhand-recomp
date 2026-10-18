#include "iop_stdio.h"

#include "iop_format.h"
#include "../core/iop_cpu.h"
#include "../core/iop_memory.h"
#include "ps2x/iop/iop_host.h"

#include <string>

namespace ps2x::iop::detail
{
    IopStdio::IopStdio(IopHost &host, IopMemory &memory) noexcept
        : m_host(host), m_memory(memory)
    {
    }

    bool IopStdio::dispatchImport(uint16_t ordinal, IopCpuState &cpu)
    {
        const uint32_t a0 = cpu.gpr[4];
        const uint32_t a1 = cpu.gpr[5];
        const auto setV0 = [&](uint32_t value)
        {
            cpu.gpr[2] = value;
        };
        const auto logString = [&](std::string_view prefix, uint32_t address, uint32_t resultBias = 0u)
        {
            const std::string text = m_memory.readString(address, 2048u);
            m_host.log(LogLevel::Info, std::string(prefix) + text);
            setV0(static_cast<uint32_t>(text.size()) + resultBias);
        };

        // printf family: the arguments are substituted (iop_format.h). Before 2026-10-17 the raw format was logged
        // ("CRI ADX Driver : SPU CORE Number = %d").
        const auto logFormatted = [&](std::string_view prefix, uint32_t formatAddress, IopVarArgs args)
        {
            const std::string text = formatIopString(m_memory, m_memory.readString(formatAddress, 2048u), args);
            m_host.log(LogLevel::Info, std::string(prefix) + text);
            setV0(static_cast<uint32_t>(text.size()));
        };

        switch (ordinal)
        {
        case 4: // printf(format, ...)
            logFormatted("[IOP printf] ", a0, IopVarArgs(m_memory, cpu, 1u));
            return true;
        case 5: // getchar
        case 10:
            setV0(0xFFFFFFFFu);
            return true;
        case 6: // putchar
            m_host.log(LogLevel::Info, std::string("[IOP putchar] ") + static_cast<char>(a0 & 0xFFu));
            setV0(a0 & 0xFFu);
            return true;
        case 7: // puts
            logString("[IOP puts] ", a0, 1u);
            return true;
        case 8: // gets
        case 13:
            setV0(0u);
            return true;
        case 9: // fdprintf(fd, format, ...)
            logFormatted("[IOP fdprintf] ", a1, IopVarArgs(m_memory, cpu, 2u));
            return true;
        case 11:
            setV0(a0 & 0xFFu);
            return true;
        case 12: // fdputs
            logString("[IOP fdputs] ", a0);
            return true;
        case 14: // vfdprintf(fd, format, va_list)
            logFormatted("[IOP vfdprintf] ", a1, IopVarArgs(m_memory, cpu.gpr[6]));
            return true;
        default:
            return false;
        }
    }
}
