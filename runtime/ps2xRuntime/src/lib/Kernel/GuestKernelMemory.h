#pragma once

#include <cstdint>

namespace ps2x::kernel_memory
{
    constexpr uint32_t kRpcPacketPoolBase = 0x00040000u;
    constexpr uint32_t kRpcPacketPoolBytes = 0x00001000u;
    constexpr uint32_t kRpcServerPoolBase = 0x00041000u;
    constexpr uint32_t kRpcServerPoolBytes = 0x00002000u;
    constexpr uint32_t kTlsPoolBase = 0x00043000u;
    constexpr uint32_t kTlsPoolBytes = 0x00004000u;
    constexpr uint32_t kBootModePoolBase = 0x00047000u;
    constexpr uint32_t kBootModePoolBytes = 0x00001000u;

    constexpr uint32_t kCallbackStackFloor = 0x00048000u;
    constexpr uint32_t kCallbackStackTop = 0x00074000u;
    constexpr uint32_t kCallbackStackSize = 0x00002000u;

    constexpr uint32_t kLegacyRpcServerPoolBase = 0x01F10000u;
    constexpr uint32_t kLegacyTlsPoolBase = 0x01F20000u;
    constexpr uint32_t kLegacyBootModePoolBase = 0x01F30000u;
    constexpr uint32_t kLegacyPoolBytes = 0x00010000u;

    constexpr bool inLegacyPool(uint32_t address, uint32_t poolBase) noexcept
    {
        const uint32_t physical = address & 0x1FFFFFFFu;
        return physical >= poolBase && physical < poolBase + kLegacyPoolBytes;
    }
}
