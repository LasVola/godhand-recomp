#pragma once

#include "ps2x/iop/iop_pad.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace ps2x::iop::detail
{
    class VirtualDualShock2
    {
    public:
        static constexpr uint8_t kAddress = 0x01u;

        enum class Mode : uint8_t
        {
            Digital,
            Analog,
            DualShock2,
        };

        void reset() noexcept;

        [[nodiscard]] size_t transfer(std::span<const uint8_t> command,
                                      std::span<uint8_t> response,
                                      const PadInputState &input) noexcept;

        [[nodiscard]] Mode mode() const noexcept { return m_mode; }
        [[nodiscard]] bool inConfig() const noexcept { return m_config; }
        [[nodiscard]] uint8_t modeId() const noexcept;
        [[nodiscard]] uint8_t smallMotor() const noexcept { return m_smallMotor; }
        [[nodiscard]] uint8_t largeMotor() const noexcept { return m_largeMotor; }

    private:
        [[nodiscard]] uint8_t dataByte(uint8_t command, size_t index, uint8_t in, const PadInputState &input) noexcept;
        [[nodiscard]] uint8_t pollByte(size_t index, uint8_t in, const PadInputState &input) noexcept;
        void finishCommand(uint8_t command, std::span<const uint8_t> data) noexcept;

        Mode m_mode = Mode::Digital;
        bool m_config = false;
        uint8_t m_selector = 0u;
        uint32_t m_pressureMask = 0u;
        std::array<uint8_t, 6> m_vibrationMap{0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu};
        uint8_t m_smallMotor = 0u;
        uint8_t m_largeMotor = 0u;
    };
}
