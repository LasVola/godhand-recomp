#include "iop_dualshock2.h"

#include <algorithm>

namespace ps2x::iop::detail
{
    namespace
    {
        constexpr uint8_t kCmdSetVref = 0x40u;
        constexpr uint8_t kCmdQueryMask = 0x41u;
        constexpr uint8_t kCmdPoll = 0x42u;
        constexpr uint8_t kCmdConfig = 0x43u;
        constexpr uint8_t kCmdSetMode = 0x44u;
        constexpr uint8_t kCmdQueryModel = 0x45u;
        constexpr uint8_t kCmdQueryAct = 0x46u;
        constexpr uint8_t kCmdQueryComb = 0x47u;
        constexpr uint8_t kCmdQueryMode = 0x4Cu;
        constexpr uint8_t kCmdVibrationMap = 0x4Du;
        constexpr uint8_t kCmdSetPressureMask = 0x4Fu;

        constexpr uint8_t kIdDigital = 0x41u;
        constexpr uint8_t kIdAnalog = 0x73u;
        constexpr uint8_t kIdDualShock2 = 0x79u;
        constexpr uint8_t kIdConfig = 0xF3u;
        constexpr uint8_t kAckMarker = 0x5Au;
        constexpr uint8_t kModelDualShock2 = 0x03u;
        constexpr uint8_t kMotorSmall = 0x00u;
        constexpr uint8_t kMotorLarge = 0x01u;
        constexpr size_t kHeaderBytes = 3u;
    }

    void VirtualDualShock2::reset() noexcept
    {
        *this = VirtualDualShock2{};
    }

    uint8_t VirtualDualShock2::modeId() const noexcept
    {
        if (m_config)
            return kIdConfig;
        switch (m_mode)
        {
        case Mode::Analog:
            return kIdAnalog;
        case Mode::DualShock2:
            return kIdDualShock2;
        default:
            return kIdDigital;
        }
    }

    size_t VirtualDualShock2::transfer(std::span<const uint8_t> command,
                                       std::span<uint8_t> response,
                                       const PadInputState &input) noexcept
    {
        const size_t length = std::min(command.size(), response.size());
        std::fill(response.begin(), response.end(), uint8_t{0xFFu});
        if (length == 0u || command[0] != kAddress)
            return 0u;
        if (length == 1u)
            return 1u;

        const uint8_t opcode = command[1];
        const bool configOnly = opcode != kCmdPoll && opcode != kCmdConfig;
        if ((opcode & 0xF0u) != 0x40u || (configOnly && !m_config))
            return 1u;

        const uint8_t id = modeId();
        response[1] = id;
        if (length == 2u)
            return 2u;
        response[2] = kAckMarker;

        const size_t dataBytes = static_cast<size_t>(id & 0x0Fu) * 2u;
        const size_t acknowledged = std::min(length, kHeaderBytes + dataBytes);
        for (size_t i = kHeaderBytes; i < acknowledged; ++i)
            response[i] = dataByte(opcode, i - kHeaderBytes, command[i], input);
        for (size_t i = acknowledged; i < length; ++i)
            response[i] = 0x00u;

        finishCommand(opcode, command.subspan(std::min(length, kHeaderBytes), acknowledged - std::min(acknowledged, kHeaderBytes)));
        return acknowledged;
    }

    uint8_t VirtualDualShock2::pollByte(size_t index, uint8_t in, const PadInputState &input) noexcept
    {
        if (index < m_vibrationMap.size())
        {
            if (m_vibrationMap[index] == kMotorSmall)
                m_smallMotor = in;
            else if (m_vibrationMap[index] == kMotorLarge)
                m_largeMotor = in;
        }

        const uint16_t wire = static_cast<uint16_t>(~input.buttons);
        switch (index)
        {
        case 0:
            return static_cast<uint8_t>(wire & 0xFFu);
        case 1:
            return static_cast<uint8_t>(wire >> 8u);
        case 2:
            return input.rightX;
        case 3:
            return input.rightY;
        case 4:
            return input.leftX;
        case 5:
            return input.leftY;
        default:
            break;
        }
        const size_t pressure = index - 6u;
        return pressure < input.pressure.size() ? input.pressure[pressure] : uint8_t{0u};
    }

    uint8_t VirtualDualShock2::dataByte(uint8_t command, size_t index, uint8_t in, const PadInputState &input) noexcept
    {
        if (!m_config)
            return pollByte(index, in, input);

        switch (command)
        {
        case kCmdPoll:
            return pollByte(index, in, input);
        case kCmdSetVref:
            return index == 2u ? 0x02u : index == 5u ? kAckMarker : 0x00u;
        case kCmdQueryMask:
        {
            if (m_mode == Mode::Digital)
                return 0x00u;
            constexpr std::array<uint8_t, 6> mask{0xFFu, 0xFFu, 0x03u, 0x00u, 0x00u, kAckMarker};
            return mask[index];
        }
        case kCmdQueryModel:
        {
            const std::array<uint8_t, 6> model{kModelDualShock2, 0x02u, m_mode == Mode::Digital ? uint8_t{0x00u} : uint8_t{0x01u}, 0x02u, 0x01u, 0x00u};
            return model[index];
        }
        case kCmdQueryAct:
        {
            if (index == 0u)
                m_selector = in;
            const bool first = m_selector == 0u;
            const std::array<uint8_t, 6> act{0x00u, 0x00u, 0x01u, first ? uint8_t{0x02u} : uint8_t{0x01u}, first ? uint8_t{0x00u} : uint8_t{0x01u}, first ? uint8_t{0x0Au} : uint8_t{0x14u}};
            return act[index];
        }
        case kCmdQueryComb:
        {
            constexpr std::array<uint8_t, 6> comb{0x00u, 0x00u, 0x02u, 0x00u, 0x01u, 0x00u};
            return comb[index];
        }
        case kCmdQueryMode:
            if (index == 0u)
                m_selector = in;
            return index == 3u ? (m_selector == 0u ? 0x04u : 0x07u) : 0x00u;
        case kCmdVibrationMap:
            return m_vibrationMap[index];
        case kCmdSetPressureMask:
            return index == 5u ? kAckMarker : 0x00u;
        default:
            return 0x00u;
        }
    }

    void VirtualDualShock2::finishCommand(uint8_t command, std::span<const uint8_t> data) noexcept
    {
        const auto at = [&](size_t index) -> uint8_t
        {
            return index < data.size() ? data[index] : uint8_t{0u};
        };

        switch (command)
        {
        case kCmdConfig:
            if (!data.empty())
                m_config = at(0) == 0x01u;
            break;
        case kCmdSetMode:
            if (!m_config || data.empty())
                break;
            if (at(0) == 0x00u)
                m_mode = Mode::Digital;
            else if (at(0) == 0x01u)
                m_mode = m_pressureMask != 0u ? Mode::DualShock2 : Mode::Analog;
            break;
        case kCmdVibrationMap:
            if (!m_config)
                break;
            for (size_t i = 0; i < m_vibrationMap.size() && i < data.size(); ++i)
                m_vibrationMap[i] = data[i];
            break;
        case kCmdSetPressureMask:
            if (!m_config)
                break;
            m_pressureMask = static_cast<uint32_t>(at(0)) | (static_cast<uint32_t>(at(1)) << 8u) |
                             (static_cast<uint32_t>(at(2)) << 16u);
            if (m_mode != Mode::Digital)
                m_mode = m_pressureMask != 0u ? Mode::DualShock2 : Mode::Analog;
            break;
        default:
            break;
        }
    }
}
