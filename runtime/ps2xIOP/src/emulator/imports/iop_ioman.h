#pragma once

#include <cstdint>
#include "ps2x/state_io.h"
#include <string>
#include <vector>

namespace ps2x::iop::detail
{
    struct IopCpuState;
    class IopGuestExecutor;
    class IopMemory;

    class IopIoman
    {
    public:
        explicit IopIoman(IopMemory &memory) noexcept;

        void reset();
        [[nodiscard]] bool dispatchImport(uint16_t ordinal, IopCpuState &cpu, IopGuestExecutor &executor);

        template <typename Ar>
        void serialize(Ar &ar)
        {
            ps2x::state::fields(ar, m_devices);
        }

    private:
        struct Device
        {
            uint32_t address = 0u;
            uint32_t gp = 0u;
            std::string name;

            template <typename Ar>
            void serialize(Ar &ar)
            {
                ps2x::state::fields(ar, address, gp, name);
            }
        };

        IopMemory &m_memory;
        std::vector<Device> m_devices;
    };
}
