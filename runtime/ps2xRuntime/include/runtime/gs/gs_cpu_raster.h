#pragma once

#include "runtime/gs/gs_swizzle.h"
#include "runtime/gs/gs_types.h"

#include <array>
#include <cstdint>

namespace GSRaster
{
    struct PageSpan
    {
        uint32_t first = 0u;
        uint32_t last = 0u;
        bool valid = false;
    };

    struct AlphaInfo
    {
        bool any = false;
        bool fb = false;
        bool writeAlpha = false;
        bool depth = false;
        bool preserve = false;
        bool frmw = false;
    };

    struct PixelSetup
    {
        uint8_t *vram = nullptr;
        const GSSwizzle::Format *frame = nullptr;
        const GSSwizzle::Format *zbuf = nullptr;
        uint32_t fbp = 0u;
        uint32_t fbw = 1u;
        uint32_t zbp = 0u;
        uint8_t fpsm = 0u;
        uint8_t zpsm = 0u;
        uint16_t sx0 = 0u;
        uint16_t sx1 = 0u;
        uint16_t sy0 = 0u;
        uint16_t sy1 = 0u;
        bool fge = false;
        uint8_t fogR = 0u;
        uint8_t fogG = 0u;
        uint8_t fogB = 0u;
        uint64_t test = 0u;
        uint32_t ztest = 0u;
        bool abe = false;
        bool pabe = false;
        bool colclamp = true;
        uint8_t asel = 0u;
        uint8_t bsel = 0u;
        uint8_t csel = 0u;
        uint8_t dsel = 0u;
        uint8_t fix = 0u;
        bool fba = false;
        bool frame16 = false;
        uint32_t fbmsk = 0u;
        bool zmask = false;
        std::array<AlphaInfo, 256> alphaInfo{};
        bool alphaKeyValid = false;
        uint64_t alphaKeyTest = 0u;
        uint32_t alphaKeyFbmsk = 0u;
        uint8_t alphaKeyFpsm = 0u;
        bool alphaKeyZmask = false;
        bool alphaKeyAbe = false;
    };

    struct WriteSpans
    {
        PageSpan frame;
        PageSpan depth;
    };

    struct AxisSample
    {
        int c0 = 0;
        int c1 = 0;
        float frac = 0.0f;
        GSSwizzle::AxisPart p0;
        GSSwizzle::AxisPart p1;
    };

    struct PixelColumn
    {
        GSSwizzle::AxisPart frame;
        GSSwizzle::AxisPart depth;
    };

    struct PixelRow
    {
        GSSwizzle::AxisPart frame;
        GSSwizzle::AxisPart depth;
    };

    struct TextureSampler
    {
        enum class Mode : uint8_t
        {
            Direct32,
            Direct16,
            Indexed,
        };

        uint8_t *vram = nullptr;
        const GSSwizzle::Format *format = nullptr;
        uint8_t psm = 0u;
        uint32_t tbp = 0u;
        uint32_t tbw = 0u;
        int texW = 1;
        int texH = 1;
        uint8_t wrapU = 0u;
        uint8_t wrapV = 0u;
        uint16_t minU = 0u;
        uint16_t maxU = 0u;
        uint16_t minV = 0u;
        uint16_t maxV = 0u;
        GSTexaReg texa{};
        Mode mode = Mode::Direct32;
        std::array<uint32_t, 256> palette{};
        PageSpan footprint;
        bool stale = false;
        uint32_t stalePage = 0u;
        const uint8_t *staleBytes = nullptr;
        bool paletteKeyValid = false;
        uint64_t paletteKeyVersion = 0u;
        uint32_t paletteKeyTexa = 0u;
        uint8_t paletteKeyPsm = 0u;
        uint8_t paletteKeyCpsm = 0u;
        uint8_t paletteKeyCsm = 0u;
        uint8_t paletteKeyCsa = 0u;
    };
}
