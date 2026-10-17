#include "runtime/ps2_pad.h"
#include "ps2_host_input.h"
#include <cstring>

namespace
{
    constexpr uint8_t kPadAnalogMarker = 0x73;
}

bool PSPadBackend::readState(int port, int /*slot*/, uint8_t *data, size_t size)
{
    if (!data || size < 32)
        return false;

    const ps2x::iop::PadInputState state = ps2_host_input::padState(static_cast<uint32_t>(port));
    const uint16_t btns = static_cast<uint16_t>(~state.buttons);

    std::memset(data, 0, 32);
    data[0] = 0x01;
    data[1] = kPadAnalogMarker;
    data[2] = static_cast<uint8_t>(btns & 0xFF);
    data[3] = static_cast<uint8_t>(btns >> 8);
    data[4] = state.rightX;
    data[5] = state.rightY;
    data[6] = state.leftX;
    data[7] = state.leftY;
    return true;
}
