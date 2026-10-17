#pragma once

#include <cstdint>

namespace GSSharedPresent
{
    struct Frame
    {
        uint32_t texture = 0;
        uint32_t width = 0;
        uint32_t height = 0;
        uint64_t sequence = 0;
        uint64_t renderSequence = 0;
        uint64_t contentHash = 0;
        bool hashValid = false;
    };

    void CaptureHostContext();
    void RestoreHostContext();
    bool Active();
    bool Acquire(Frame &frame);
    void ShutdownHost();
}
