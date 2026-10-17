#pragma once

#include "runtime/gs/gs_cpu_raster.h"
#include "gs_cpu_backend_internal.h"

#include <array>
#include <cstdint>
#include <smmintrin.h>

namespace GSRaster
{
    bool FastPathsEnabled();
    std::array<uint64_t, 8> &FallbackCounters();
    PageSpan Footprint(const GSSwizzle::Format &f, uint32_t base, uint32_t bw, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1);
    bool Overlaps(const PageSpan &a, const PageSpan &b);
    bool Contains(const PageSpan &span, uint32_t page);
    bool SetupPixel(PixelSetup &s, uint8_t *vram, const GSDrawState &state);
    WriteSpans WriteFootprint(const PixelSetup &s, int x0, int y0, int x1, int y1, bool mayWriteDepth);
    bool TextureSetup(TextureSampler &t, uint8_t *vram, const GSDrawState &state, const std::array<uint16_t, 512> &clut,
                      uint64_t clutVersion);
    uint32_t LookupClut(const std::array<uint16_t, 512> &clut, const GSTexaReg &texa, uint8_t index, uint8_t cpsm,
                        uint8_t csm, uint8_t csa, uint8_t sourcePsm);
    long ExactLround(float value);

    inline bool Overlaps(const WriteSpans &writes, const PageSpan &reads)
    {
        return Overlaps(writes.frame, reads) || Overlaps(writes.depth, reads);
    }

    inline bool Contains(const WriteSpans &writes, uint32_t page)
    {
        return Contains(writes.frame, page) || Contains(writes.depth, page);
    }

    inline PixelRow MakeRow(const PixelSetup &s, uint32_t y)
    {
        return {GSSwizzle::RowPart(*s.frame, s.fbp, s.fbw, y), GSSwizzle::RowPart(*s.zbuf, s.zbp, s.fbw, y)};
    }

    inline PixelColumn MakeColumn(const PixelSetup &s, uint32_t x)
    {
        return {GSSwizzle::ColumnPart(*s.frame, x), GSSwizzle::ColumnPart(*s.zbuf, x)};
    }

    inline void MakeTextureParts(const TextureSampler &t, AxisSample &sample, bool isRow)
    {
        if (isRow)
        {
            sample.p0 = GSSwizzle::RowPart(*t.format, t.tbp, t.tbw, static_cast<uint32_t>(sample.c0));
            sample.p1 = GSSwizzle::RowPart(*t.format, t.tbp, t.tbw, static_cast<uint32_t>(sample.c1));
        }
        else
        {
            sample.p0 = GSSwizzle::ColumnPart(*t.format, static_cast<uint32_t>(sample.c0));
            sample.p1 = GSSwizzle::ColumnPart(*t.format, static_cast<uint32_t>(sample.c1));
        }
    }

    inline uint32_t FetchAt(TextureSampler &t, const GSSwizzle::AxisPart &row, const GSSwizzle::AxisPart &column, uint32_t &lastByte)
    {
        const GSSwizzle::Location loc = GSSwizzle::Combine(*t.format, row, column);
        lastByte = loc.byte;
        const uint8_t *source = t.vram + loc.byte;
        if (t.stale)
        {
            if ((loc.byte & ~(GSSwizzle::kPageBytes - 1u)) == t.stalePage)
                source = t.staleBytes + (loc.byte & (GSSwizzle::kPageBytes - 1u));
            else
                t.stale = false;
        }
        const uint32_t raw = GSSwizzle::LoadAt(*t.format, source, loc.shift);
        switch (t.mode)
        {
        case TextureSampler::Mode::Direct32:
            return GSCpuInternal::applyTexa(t.texa, t.psm, raw);
        case TextureSampler::Mode::Direct16:
            return GSCpuInternal::applyTexa(t.texa, t.psm, GSCpuInternal::Rgba5551ToRgba8888(static_cast<uint16_t>(raw)));
        default:
            return t.palette[raw & 0xFFu];
        }
    }

    inline uint32_t Fetch(TextureSampler &t, int u, int v, uint32_t &lastByte)
    {
        return FetchAt(t, GSSwizzle::RowPart(*t.format, t.tbp, t.tbw, static_cast<uint32_t>(v)),
                       GSSwizzle::ColumnPart(*t.format, static_cast<uint32_t>(u)), lastByte);
    }

    inline uint8_t LerpChannel(uint8_t c00, uint8_t c10, uint8_t c01, uint8_t c11, float fx, float fy)
    {
        const float top = static_cast<float>(c00) + (static_cast<float>(c10) - static_cast<float>(c00)) * fx;
        const float bottom = static_cast<float>(c01) + (static_cast<float>(c11) - static_cast<float>(c01)) * fx;
        return GSInternal::clampU8(static_cast<int>(ExactLround(top + (bottom - top) * fy)));
    }

    inline uint32_t LerpScalar(uint32_t c00, uint32_t c10, uint32_t c01, uint32_t c11, float fx, float fy)
    {
        uint32_t out = 0u;
        for (uint32_t shift = 0u; shift < 32u; shift += 8u)
        {
            const uint8_t v = LerpChannel(static_cast<uint8_t>(c00 >> shift), static_cast<uint8_t>(c10 >> shift),
                                          static_cast<uint8_t>(c01 >> shift), static_cast<uint8_t>(c11 >> shift), fx, fy);
            out |= static_cast<uint32_t>(v) << shift;
        }
        return out;
    }

    inline __m128 UnpackChannels(uint32_t color)
    {
        return _mm_cvtepi32_ps(_mm_cvtepu8_epi32(_mm_cvtsi32_si128(static_cast<int>(color))));
    }

    inline uint32_t LerpVector(uint32_t c00, uint32_t c10, uint32_t c01, uint32_t c11, float fx, float fy)
    {
        const __m128 f00 = UnpackChannels(c00);
        const __m128 f10 = UnpackChannels(c10);
        const __m128 f01 = UnpackChannels(c01);
        const __m128 f11 = UnpackChannels(c11);
        const __m128 vfx = _mm_set1_ps(fx);
        const __m128 vfy = _mm_set1_ps(fy);
        const __m128 top = _mm_add_ps(f00, _mm_mul_ps(_mm_sub_ps(f10, f00), vfx));
        const __m128 bottom = _mm_add_ps(f01, _mm_mul_ps(_mm_sub_ps(f11, f01), vfx));
        const __m128 value = _mm_add_ps(top, _mm_mul_ps(_mm_sub_ps(bottom, top), vfy));
        const __m128 magnitude = _mm_andnot_ps(_mm_set1_ps(-0.0f), value);
        if (_mm_movemask_ps(_mm_cmplt_ps(magnitude, _mm_set1_ps(8388608.0f))) != 0xF)
            return LerpScalar(c00, c10, c01, c11, fx, fy);
        __m128i truncated = _mm_cvttps_epi32(value);
        const __m128 fraction = _mm_sub_ps(value, _mm_cvtepi32_ps(truncated));
        const __m128 positive = _mm_cmpge_ps(value, _mm_setzero_ps());
        const __m128 up = _mm_and_ps(positive, _mm_cmpge_ps(fraction, _mm_set1_ps(0.5f)));
        const __m128 down = _mm_andnot_ps(positive, _mm_cmple_ps(fraction, _mm_set1_ps(-0.5f)));
        truncated = _mm_sub_epi32(truncated, _mm_castps_si128(up));
        truncated = _mm_add_epi32(truncated, _mm_castps_si128(down));
        const __m128i words = _mm_packs_epi32(truncated, truncated);
        const __m128i bytes = _mm_packus_epi16(words, words);
        return static_cast<uint32_t>(_mm_cvtsi128_si32(bytes));
    }

    inline uint32_t SampleBilinear(TextureSampler &t, const AxisSample &col, const AxisSample &row, uint32_t &lastByte)
    {
        const uint32_t c00 = FetchAt(t, row.p0, col.p0, lastByte);
        const uint32_t c10 = FetchAt(t, row.p0, col.p1, lastByte);
        const uint32_t c01 = FetchAt(t, row.p1, col.p0, lastByte);
        const uint32_t c11 = FetchAt(t, row.p1, col.p1, lastByte);
        return LerpVector(c00, c10, c01, c11, col.frac, row.frac);
    }

    inline uint32_t SamplePoint(TextureSampler &t, const AxisSample &col, const AxisSample &row, uint32_t &lastByte)
    {
        return FetchAt(t, row.p0, col.p0, lastByte);
    }

    inline void ShadePixel(const PixelSetup &s, const PixelRow &row, const PixelColumn &column, int x, int y, uint32_t z,
                           uint8_t r, uint8_t g, uint8_t b, uint8_t a, uint8_t fog)
    {
        if (x < s.sx0 || x > s.sx1 || y < s.sy0 || y > s.sy1)
            return;

        if (s.fge)
        {
            const uint32_t inverseFog = 255u - fog;
            r = static_cast<uint8_t>(((static_cast<uint32_t>(fog) * r) >> 8) + ((inverseFog * s.fogR) >> 8));
            g = static_cast<uint8_t>(((static_cast<uint32_t>(fog) * g) >> 8) + ((inverseFog * s.fogG) >> 8));
            b = static_cast<uint8_t>(((static_cast<uint32_t>(fog) * b) >> 8) + ((inverseFog * s.fogB) >> 8));
        }

        const AlphaInfo &m = s.alphaInfo[a];
        if (!m.any)
            return;

        GSSwizzle::Location frameLoc{0u, 0u};
        bool haveFrameLoc = false;
        uint32_t raw = 0u;
        uint32_t fbrgba = 0u;
        if (m.frmw)
        {
            frameLoc = GSSwizzle::Combine(*s.frame, row.frame, column.frame);
            haveFrameLoc = true;
            raw = GSSwizzle::LoadAt(*s.frame, s.vram + frameLoc.byte, frameLoc.shift);
            fbrgba = raw;
            if (s.frame16)
                fbrgba = GSCpuInternal::Rgba5551ToRgba8888(static_cast<uint16_t>(fbrgba));
            else if (s.fpsm == GS_PSM_CT24)
                fbrgba |= 0x80000000u;
        }

        if (!GSCpuInternal::passesDestinationAlphaTest(s.test, s.fpsm, raw))
            return;

        GSSwizzle::Location depthLoc{0u, 0u};
        bool haveDepthLoc = false;
        switch (s.ztest)
        {
        case 0:
            return;
        case 1:
            break;
        default:
        {
            depthLoc = GSSwizzle::Combine(*s.zbuf, row.depth, column.depth);
            haveDepthLoc = true;
            const uint32_t stored = GSSwizzle::LoadAt(*s.zbuf, s.vram + depthLoc.byte, depthLoc.shift);
            const bool pass = s.ztest == 2u ? z >= stored : z > stored;
            if (!pass)
                return;
            break;
        }
        }

        if (m.fb)
        {
            if (s.abe)
            {
                const uint8_t dr = static_cast<uint8_t>(fbrgba & 0xFFu);
                const uint8_t dg = static_cast<uint8_t>((fbrgba >> 8) & 0xFFu);
                const uint8_t db = static_cast<uint8_t>((fbrgba >> 16) & 0xFFu);
                const uint8_t da = static_cast<uint8_t>((fbrgba >> 24) & 0xFFu);
                if (!(s.pabe && (a & 0x80u) == 0u))
                {
                    auto pick = [](uint8_t sel, int cs, int cd) -> int
                    {
                        if (sel == 0)
                            return cs;
                        if (sel == 1)
                            return cd;
                        return 0;
                    };
                    const int cAlpha = (s.csel == 0) ? a : (s.csel == 1) ? da : s.fix;
                    const int br = ((pick(s.asel, r, dr) - pick(s.bsel, r, dr)) * cAlpha >> 7) + pick(s.dsel, r, dr);
                    const int bg = ((pick(s.asel, g, dg) - pick(s.bsel, g, dg)) * cAlpha >> 7) + pick(s.dsel, g, dg);
                    const int bb = ((pick(s.asel, b, db) - pick(s.bsel, b, db)) * cAlpha >> 7) + pick(s.dsel, b, db);
                    r = s.colclamp ? GSInternal::clampU8(br) : static_cast<uint8_t>(br & 0xFF);
                    g = s.colclamp ? GSInternal::clampU8(bg) : static_cast<uint8_t>(bg & 0xFF);
                    b = s.colclamp ? GSInternal::clampU8(bb) : static_cast<uint8_t>(bb & 0xFF);
                }
            }

            if (m.writeAlpha && s.fba)
                a = static_cast<uint8_t>(a | 0x80u);

            uint32_t pixel = GSCpuInternal::pack32(r, g, b, a);
            if (s.fbmsk != 0u)
                pixel = (pixel & ~s.fbmsk) | (fbrgba & s.fbmsk);
            if (m.preserve)
                pixel = (pixel & 0x00FFFFFFu) | (fbrgba & 0xFF000000u);
            if (s.frame16)
                pixel = GSCpuInternal::Rgba8888ToRgba5551(pixel);
            if (!haveFrameLoc)
                frameLoc = GSSwizzle::Combine(*s.frame, row.frame, column.frame);
            GSSwizzle::StoreAt(*s.frame, s.vram + frameLoc.byte, frameLoc.shift, pixel);
        }

        if (m.depth)
        {
            if (!haveDepthLoc)
                depthLoc = GSSwizzle::Combine(*s.zbuf, row.depth, column.depth);
            GSSwizzle::StoreAt(*s.zbuf, s.vram + depthLoc.byte, depthLoc.shift, z);
        }
    }
}
