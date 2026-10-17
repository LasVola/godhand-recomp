#include "iop_vblank.h"

#include "../core/iop_cpu.h"
#include "../iop_emulator_const.h"
#include "../core/iop_kernel.h"
#include "../services/iop_rpc.h"

#include <algorithm>
#include <cstdlib>

namespace ps2x::iop::detail
{
    namespace
    {
        constexpr uint32_t kKeFoundHandler = static_cast<uint32_t>(-104);
        constexpr uint32_t kKeNotFoundHandler = static_cast<uint32_t>(-105);

        // Index of the last edge at or before `cycle` (edges sit at k * period + phase).
        uint64_t edgeIndex(uint64_t cycle, uint64_t phase) noexcept
        {
            return cycle < phase ? 0u : (cycle - phase) / kVblankPeriodCycles + 1u;
        }

        uint64_t nextEdge(uint64_t cycle, uint64_t phase) noexcept
        {
            return edgeIndex(cycle, phase) * kVblankPeriodCycles + phase;
        }

        bool handlersEnabled()
        {
            static const bool enabled = []
            {
                const char *value = std::getenv("PS2X_IOP_VBLANK_HANDLERS");
                return !value || *value != '0';
            }();
            return enabled;
        }
    }

    IopVblank::IopVblank(IopKernel &kernel) noexcept
        : m_kernel(kernel)
    {
    }

    void IopVblank::reset() noexcept
    {
        m_handlers.clear();
        m_lastCycle = 0u;
        m_nextOrder = 0u;
        m_servicing = false;
    }

    bool IopVblank::dispatchImport(uint16_t ordinal, IopCpuState &cpu, uint64_t currentCycle)
    {
        switch (ordinal)
        {
        case 4: // WaitVblankStart
        case 5: // WaitVblankEnd
        case 6: // WaitVblank
        case 7: // WaitNonVblank
        {
            const bool waitForEnd = ordinal == 5u || ordinal == 7u;
            const uint64_t phase = waitForEnd ? kVblankEndPhaseCycles : 0u;
            const uint64_t fieldStart = currentCycle - (currentCycle % kVblankPeriodCycles);
            uint64_t wakeCycle = fieldStart + phase;
            if (wakeCycle <= currentCycle)
                wakeCycle += kVblankPeriodCycles;
            m_kernel.delayCurrentUntil(wakeCycle, cpu);
            cpu.gpr[2] = 0u;
            return true;
        }
        case 8: // RegisterVblankHandler(edge, priority, handler, arg)
        {
            const uint32_t edge = cpu.gpr[4] != 0u ? 1u : 0u;
            const uint32_t function = cpu.gpr[6];
            const auto found = std::find_if(m_handlers.begin(), m_handlers.end(), [&](const Handler &h)
                                            { return h.edge == edge && h.function == function; });
            if (found != m_handlers.end())
            {
                cpu.gpr[2] = kKeFoundHandler;
                return true;
            }
            if (m_handlers.empty())
                m_lastCycle = currentCycle;
            if (function != 0u && handlersEnabled())
                m_handlers.push_back({edge, static_cast<int32_t>(cpu.gpr[5]), function, cpu.gpr[7], cpu.gpr[28], m_nextOrder++});
            cpu.gpr[2] = 0u;
            return true;
        }
        case 9: // ReleaseVblankHandler(edge, handler)
        {
            const uint32_t edge = cpu.gpr[4] != 0u ? 1u : 0u;
            const uint32_t function = cpu.gpr[5];
            const auto before = m_handlers.size();
            std::erase_if(m_handlers, [&](const Handler &h)
                          { return h.edge == edge && h.function == function; });
            cpu.gpr[2] = m_handlers.size() != before || !handlersEnabled() ? 0u : kKeNotFoundHandler;
            return true;
        }
        default:
            return false;
        }
    }

    void IopVblank::runEdge(uint32_t edge, IopGuestExecutor &executor)
    {
        std::vector<Handler> due;
        for (const Handler &h : m_handlers)
            if (h.edge == edge)
                due.push_back(h);
        std::stable_sort(due.begin(), due.end(), [](const Handler &a, const Handler &b)
                         { return a.priority != b.priority ? a.priority < b.priority : a.order < b.order; });
        for (const Handler &h : due)
        {
            const auto live = [&]
            {
                return std::find_if(m_handlers.begin(), m_handlers.end(), [&](const Handler &x)
                                    { return x.order == h.order; });
            };
            if (live() == m_handlers.end())
                continue; // released by an earlier handler
            const uint32_t result = executor.executeGuestFunctionWithBudget(h.function, h.argument, 0u, 0u, 0u, h.gp, 100000u);
            // vblank.irx unregisters a handler that returns 0.
            if (result == 0u)
            {
                const auto it = live();
                if (it != m_handlers.end())
                    m_handlers.erase(it);
            }
        }
    }

    void IopVblank::serviceDue(uint64_t currentCycle, IopGuestExecutor &executor)
    {
        if (m_servicing)
            return;
        if (m_handlers.empty() || currentCycle <= m_lastCycle)
        {
            m_lastCycle = std::max(m_lastCycle, currentCycle);
            return;
        }
        m_servicing = true;
        struct ServiceGuard
        {
            bool &flag;
            ~ServiceGuard() { flag = false; }
        } guard{m_servicing};

        const uint64_t last = m_lastCycle;
        m_lastCycle = currentCycle;
        // Missed fields collapse into one call per edge, like a late interrupt on hardware.
        const bool start = edgeIndex(currentCycle, 0u) > edgeIndex(last, 0u);
        const bool end = edgeIndex(currentCycle, kVblankEndPhaseCycles) > edgeIndex(last, kVblankEndPhaseCycles);
        if (start)
            runEdge(0u, executor);
        if (end)
            runEdge(1u, executor);
    }

    uint64_t IopVblank::nextEventCycle(uint64_t fallback) const noexcept
    {
        if (m_handlers.empty())
            return fallback;
        return std::min({fallback, nextEdge(m_lastCycle, 0u), nextEdge(m_lastCycle, kVblankEndPhaseCycles)});
    }
}
