#pragma once

#include "ps2x/state_io.h"
#include <cstdint>
#include <vector>

namespace ps2x::iop::detail
{
    struct IopCpuState;
    class IopGuestExecutor;
    class IopKernel;

    class IopVblank
    {
    public:
        explicit IopVblank(IopKernel &kernel) noexcept;

        void reset() noexcept;
        [[nodiscard]] bool dispatchImport(uint16_t ordinal, IopCpuState &cpu, uint64_t currentCycle);
        // Runs the handlers registered with RegisterVblankHandler for every VBLANK start/end edge crossed since the
        // last call. PADMAN polls the pads from such a handler, so without it no pad data ever reaches the EE.
        void serviceDue(uint64_t currentCycle, IopGuestExecutor &executor);
        [[nodiscard]] uint64_t nextEventCycle(uint64_t fallback) const noexcept;

        template <typename Ar>
        void serialize(Ar &ar)
        {
            ps2x::state::fields(ar, m_handlers, m_lastCycle, m_nextOrder);
            m_servicing = false;
        }
        // A state without the vblank section (saved before it existed): keep the handlers this session registered and
        // count edges from the restored IOP time.
        void resyncAfterLoad(uint64_t currentCycle) noexcept
        {
            m_lastCycle = currentCycle;
            m_servicing = false;
        }

    private:
        struct Handler
        {
            uint32_t edge = 0u;
            int32_t priority = 0;
            uint32_t function = 0u;
            uint32_t argument = 0u;
            uint32_t gp = 0u;
            uint64_t order = 0u;
        };

        void runEdge(uint32_t edge, IopGuestExecutor &executor);

        IopKernel &m_kernel;
        std::vector<Handler> m_handlers;
        uint64_t m_lastCycle = 0u;
        uint64_t m_nextOrder = 0u;
        bool m_servicing = false;
    };
}
