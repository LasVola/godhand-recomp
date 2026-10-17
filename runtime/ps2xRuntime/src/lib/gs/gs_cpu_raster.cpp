#include "runtime/gs/gs_cpu_backend.h"
#include "gs_cpu_backend_internal.h"
#include "gs_cpu_raster_inline.h"
#include "gs_sprite_rules.h"
#include "gs_triangle_rules.h"
#include "runtime/gs/gs_band_pool.h"

#include <algorithm>
#include <climits>
#include <cstdint>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>

using namespace GSCpuInternal;

namespace GSRaster
{
    std::array<uint64_t, 8> &FallbackCounters()
    {
        static std::array<uint64_t, 8> counters{};
        return counters;
    }

    bool FastPathsEnabled()
    {
        static const bool enabled = []
        {
            const char *value = std::getenv("PS2X_GS_FAST");
            return !(value && *value == '0');
        }();
        return enabled;
    }

    PageSpan Footprint(const GSSwizzle::Format &f, uint32_t base, uint32_t bw, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1)
    {
        const uint32_t pagesPerRow = (bw * 64u) >> f.pageShiftX;
        PageSpan span;
        span.first = (base >> 5u) + (y0 >> f.pageShiftY) * pagesPerRow + (x0 >> f.pageShiftX);
        span.last = (base >> 5u) + (y1 >> f.pageShiftY) * pagesPerRow + (x1 >> f.pageShiftX) + ((base & 31u) != 0u ? 1u : 0u);
        span.valid = true;
        return span;
    }

    bool Overlaps(const PageSpan &a, const PageSpan &b)
    {
        if (!a.valid || !b.valid)
            return false;
        const uint32_t lengthA = a.last - a.first;
        const uint32_t lengthB = b.last - b.first;
        if (lengthA >= 511u || lengthB >= 511u)
            return true;
        const int64_t a0 = a.first & 511u;
        const int64_t a1 = a0 + lengthA;
        const int64_t b0 = b.first & 511u;
        const int64_t b1 = b0 + lengthB;
        for (int64_t shift = -512; shift <= 512; shift += 512)
        {
            if (a0 <= b1 + shift && b0 + shift <= a1)
                return true;
        }
        return false;
    }

    bool Contains(const PageSpan &span, uint32_t page)
    {
        if (!span.valid)
            return false;
        const uint32_t length = span.last - span.first;
        if (length >= 511u)
            return true;
        const uint32_t offset = (page - (span.first & 511u)) & 511u;
        return offset <= length;
    }

    bool SetupPixel(PixelSetup &s, uint8_t *vram, const GSDrawState &state)
    {
        const GSContext &ctx = state.context;
        s.vram = vram;
        s.fpsm = ctx.frame.psm;
        s.zpsm = ctx.zbuf.psm;
        s.frame = &GSSwizzle::GetFormat(s.fpsm);
        s.zbuf = &GSSwizzle::GetFormat(s.zpsm);
        if (s.frame->kind == GSSwizzle::Kind::Invalid || s.zbuf->kind == GSSwizzle::Kind::Invalid)
            return false;
        s.fbp = GSInternal::framePageBaseToBlock(ctx.frame.fbp);
        s.fbw = std::max<uint32_t>(ctx.frame.fbw, 1u);
        s.zbp = GSInternal::framePageBaseToBlock(ctx.zbuf.zbp);
        s.sx0 = ctx.scissor.x0;
        s.sx1 = ctx.scissor.x1;
        s.sy0 = ctx.scissor.y0;
        s.sy1 = ctx.scissor.y1;
        s.fge = state.prim.fge;
        s.fogR = state.fogR;
        s.fogG = state.fogG;
        s.fogB = state.fogB;
        s.test = ctx.test;
        s.ztest = (((ctx.test >> 16) & 1u) != 0u ? static_cast<uint32_t>((ctx.test >> 17) & 3u) : 1u) /* ZTE=0: depth test off (ALWAYS), as PCSX2 */;
        s.abe = state.prim.abe;
        s.pabe = state.pabe;
        s.colclamp = (state.colclamp & 1u) != 0u;
        const uint64_t alphaReg = ctx.alpha;
        s.asel = static_cast<uint8_t>(alphaReg & 3u);
        s.bsel = static_cast<uint8_t>((alphaReg >> 2) & 3u);
        s.csel = static_cast<uint8_t>((alphaReg >> 4) & 3u);
        s.dsel = static_cast<uint8_t>((alphaReg >> 6) & 3u);
        s.fix = static_cast<uint8_t>((alphaReg >> 32) & 0xFFu);
        s.fba = (ctx.fba & 0x1ull) != 0ull && ctx.frame.psm != GS_PSM_CT24;
        s.frame16 = GSInternal::bitsPerPixel(s.fpsm) == 16u;
        s.fbmsk = ctx.frame.fbmsk;
        s.zmask = ctx.zbuf.zmask;
        if (s.alphaKeyValid && s.alphaKeyTest == ctx.test && s.alphaKeyFbmsk == s.fbmsk && s.alphaKeyFpsm == s.fpsm &&
            s.alphaKeyZmask == s.zmask && s.alphaKeyAbe == s.abe)
            return true;
        s.alphaKeyValid = true;
        s.alphaKeyTest = ctx.test;
        s.alphaKeyFbmsk = s.fbmsk;
        s.alphaKeyFpsm = s.fpsm;
        s.alphaKeyZmask = s.zmask;
        s.alphaKeyAbe = s.abe;
        const bool dateRead = ((ctx.test >> 14) & 0x1u) != 0u &&
                              (s.fpsm == GS_PSM_CT32 || s.fpsm == GS_PSM_CT16 || s.fpsm == GS_PSM_CT16S);
        for (uint32_t a = 0; a < 256u; ++a)
        {
            const PixelWriteMask mask = classifyAlphaTest(ctx.test, static_cast<uint8_t>(a), s.fpsm);
            AlphaInfo &info = s.alphaInfo[a];
            info.any = mask.writesAnything();
            info.fb = mask.writesFramebuffer();
            info.writeAlpha = mask.writeAlpha;
            info.depth = mask.writeDepth && !s.zmask;
            info.preserve = mask.writeRgb && !mask.writeAlpha && s.fpsm == GS_PSM_CT32;
            info.frmw = dateRead || (info.fb && (s.fbmsk != 0u || s.abe || info.preserve));
        }
        return true;
    }

    WriteSpans WriteFootprint(const PixelSetup &s, int x0, int y0, int x1, int y1, bool mayWriteDepth)
    {
        WriteSpans spans;
        spans.frame = Footprint(*s.frame, s.fbp, s.fbw, static_cast<uint32_t>(x0), static_cast<uint32_t>(y0),
                                static_cast<uint32_t>(x1), static_cast<uint32_t>(y1));
        if (mayWriteDepth)
            spans.depth = Footprint(*s.zbuf, s.zbp, s.fbw, static_cast<uint32_t>(x0), static_cast<uint32_t>(y0),
                                    static_cast<uint32_t>(x1), static_cast<uint32_t>(y1));
        return spans;
    }

    bool TextureSetup(TextureSampler &t, uint8_t *vram, const GSDrawState &state, const std::array<uint16_t, 512> &clut,
                      uint64_t clutVersion)
    {
        const GSContext &ctx = state.context;
        const GSTex0Reg &tex = ctx.tex0;
        t.vram = vram;
        t.psm = tex.psm;
        t.format = &GSSwizzle::GetFormat(tex.psm);
        t.tbp = tex.tbp0;
        t.tbw = tex.tbw;
        t.texW = state.textureWidth;
        t.texH = state.textureHeight;
        const uint64_t clamp = ctx.clamp;
        t.wrapU = static_cast<uint8_t>(clamp & 0x3u);
        t.wrapV = static_cast<uint8_t>((clamp >> 2) & 0x3u);
        t.minU = static_cast<uint16_t>((clamp >> 4) & 0x3FFu);
        t.maxU = static_cast<uint16_t>((clamp >> 14) & 0x3FFu);
        t.minV = static_cast<uint16_t>((clamp >> 24) & 0x3FFu);
        t.maxV = static_cast<uint16_t>((clamp >> 34) & 0x3FFu);
        t.texa = state.texa;
        switch (tex.psm)
        {
        case GS_PSM_CT32:
        case GS_PSM_Z32:
        case GS_PSM_CT24:
        case GS_PSM_Z24:
            t.mode = TextureSampler::Mode::Direct32;
            break;
        case GS_PSM_CT16:
        case GS_PSM_CT16S:
        case GS_PSM_Z16:
        case GS_PSM_Z16S:
            t.mode = TextureSampler::Mode::Direct16;
            break;
        case GS_PSM_T8:
        case GS_PSM_T8H:
        case GS_PSM_T4:
        case GS_PSM_T4HL:
        case GS_PSM_T4HH:
        {
            t.mode = TextureSampler::Mode::Indexed;
            const uint32_t texaKey = static_cast<uint32_t>(state.texa.ta0) | (static_cast<uint32_t>(state.texa.ta1) << 8u) |
                                     (state.texa.aem ? 0x10000u : 0u);
            if (!(t.paletteKeyValid && t.paletteKeyVersion == clutVersion && t.paletteKeyTexa == texaKey &&
                  t.paletteKeyPsm == tex.psm && t.paletteKeyCpsm == tex.cpsm && t.paletteKeyCsm == tex.csm &&
                  t.paletteKeyCsa == tex.csa))
            {
                const uint32_t entries = isFourBitIndexedPsm(tex.psm) ? 16u : 256u;
                for (uint32_t i = 0; i < entries; ++i)
                    t.palette[i] = LookupClut(clut, state.texa, static_cast<uint8_t>(i), tex.cpsm, tex.csm, tex.csa, tex.psm);
                t.paletteKeyValid = true;
                t.paletteKeyVersion = clutVersion;
                t.paletteKeyTexa = texaKey;
                t.paletteKeyPsm = tex.psm;
                t.paletteKeyCpsm = tex.cpsm;
                t.paletteKeyCsm = tex.csm;
                t.paletteKeyCsa = tex.csa;
            }
            break;
        }
        default:
            return false;
        }
        if (t.format->kind == GSSwizzle::Kind::Invalid)
            return false;
        auto maxCoord = [](uint8_t mode, int size, uint16_t minC, uint16_t maxC) -> uint32_t
        {
            switch (mode & 3u)
            {
            case 2:
                return maxC;
            case 3:
                return static_cast<uint32_t>(minC | maxC);
            default:
                return static_cast<uint32_t>(size - 1);
            }
        };
        t.footprint = Footprint(*t.format, t.tbp, t.tbw, 0u, 0u, maxCoord(t.wrapU, t.texW, t.minU, t.maxU),
                                maxCoord(t.wrapV, t.texH, t.minV, t.maxV));
        return true;
    }

    uint32_t LookupClut(const std::array<uint16_t, 512> &clut, const GSTexaReg &texa, uint8_t index, uint8_t cpsm,
                        uint8_t csm, uint8_t csa, uint8_t sourcePsm)
    {
        const bool sixteenBit = cpsm == GS_PSM_CT16 || cpsm == GS_PSM_CT16S;
        const uint32_t csaMask = sixteenBit ? 0x1Fu : 0x0Fu;
        const uint32_t clutBase = (static_cast<uint32_t>(csa) & csaMask) << 4u;
        const uint32_t sourceIndex = isFourBitIndexedPsm(sourcePsm)
                                         ? (static_cast<uint32_t>(index) & 0x0Fu)
                                         : static_cast<uint32_t>(index);

        uint32_t clutIndex = (clutBase + sourceIndex) & (sixteenBit ? 0x1FFu : 0x0FFu);
        if (!sixteenBit && csm == 0u && isEightBitIndexedPsm(sourcePsm))
        {
            const uint32_t block = std::min((sourceIndex & 0xF0u) + clutBase, 240u);
            clutIndex = block + (sourceIndex & 0x0Fu);
        }

        switch (cpsm)
        {
        case GS_PSM_CT32:
        {
            const uint32_t raw = static_cast<uint32_t>(clut[clutIndex]) | (static_cast<uint32_t>(clut[clutIndex + 256u]) << 16u);
            return applyTexa(texa, cpsm, raw);
        }
        case GS_PSM_CT24:
        {
            const uint32_t raw = static_cast<uint32_t>(clut[clutIndex]) | (static_cast<uint32_t>(clut[clutIndex + 256u]) << 16u);
            return applyTexa(texa, cpsm, raw & 0x00FFFFFFu);
        }
        case GS_PSM_CT16:
        case GS_PSM_CT16S:
            return applyTexa(texa, cpsm, Rgba5551ToRgba8888(clut[clutIndex]));
        default:
            break;
        }
        return 0xFFFF00FFu;
    }

    long ExactLround(float value)
    {
        if (!(std::fabs(value) < 8388608.0f))
            return std::lround(value);
        long truncated = static_cast<long>(value);
        const float fraction = value - static_cast<float>(truncated);
        if (value >= 0.0f)
        {
            if (fraction >= 0.5f)
                ++truncated;
        }
        else if (fraction <= -0.5f)
        {
            --truncated;
        }
        return truncated;
    }
}

using namespace GSRaster;

bool GSCpuBackend::PrepareCacheForTexturedDraw()
{
    m_sampler.stale = false;
    if (m_textureCache.Page() == UINT32_MAX || m_textureCache.Live())
        return true;
    if (m_textureCache.DematerializeIfClean(m_vram))
        return true;
    m_sampler.stale = true;
    m_sampler.stalePage = m_textureCache.Page();
    m_sampler.staleBytes = m_textureCache.Bytes();
    return true;
}

uint32_t GSCpuBackend::LastBandTexel() const
{
    for (size_t i = m_bandLastTexel.size(); i > 0u; --i)
    {
        if (m_bandLastTexel[i - 1u] != UINT32_MAX)
            return m_bandLastTexel[i - 1u];
    }
    return UINT32_MAX;
}

void GSCpuBackend::FinishTexturedDraw(uint32_t lastTexelByte)
{
    if (lastTexelByte == UINT32_MAX || m_sampler.stale)
        return;
    m_textureCache.Resolve(m_vram, lastTexelByte);
}

bool GSCpuBackend::BandsAllowed(const GSRaster::PixelSetup &px, int x0, int y0, int x1, int y1) const
{
    if (px.frame->kind == GSSwizzle::Kind::Bits4 || px.zbuf->kind == GSSwizzle::Kind::Bits4)
        return false;
    const bool touchesDepth = px.ztest >= 2u || !px.zmask;
    if (!touchesDepth)
        return true;
    const PageSpan frame = Footprint(*px.frame, px.fbp, px.fbw, static_cast<uint32_t>(x0), static_cast<uint32_t>(y0),
                                     static_cast<uint32_t>(x1), static_cast<uint32_t>(y1));
    const PageSpan depth = Footprint(*px.zbuf, px.zbp, px.fbw, static_cast<uint32_t>(x0), static_cast<uint32_t>(y0),
                                     static_cast<uint32_t>(x1), static_cast<uint32_t>(y1));
    return !Overlaps(frame, depth);
}

unsigned GSCpuBackend::PlanBands(int y0, int y1, int columns, bool allowed)
{
    constexpr int64_t kParallelPixels = 8192;
    const int rows = y1 - y0 + 1;
    if (!allowed || rows < 8 || static_cast<int64_t>(rows) * columns < kParallelPixels)
        return 1u;
    if (!m_bandPool)
    {
        const unsigned threads = GSBandPool::ConfiguredThreads();
        if (threads <= 1u)
            return 1u;
        m_bandPool = std::make_unique<GSBandPool>(threads - 1u);
    }
    const unsigned threads = m_bandPool->Threads();
    if (threads <= 1u)
        return 1u;
    return std::max(1u, std::min(threads * 4u, static_cast<unsigned>(rows / 4)));
}

void GSCpuBackend::RunBands(int y0, int y1, unsigned bands, const std::function<void(int, int, unsigned)> &work)
{
    if (bands <= 1u)
    {
        work(y0, y1, 0u);
        return;
    }
    const int rows = y1 - y0 + 1;
    m_bandPool->Run(bands, [&](unsigned band)
                    {
        const int begin = y0 + static_cast<int>(static_cast<int64_t>(rows) * band / bands);
        const int end = y0 + static_cast<int>(static_cast<int64_t>(rows) * (band + 1u) / bands) - 1;
        if (begin <= end)
            work(begin, end, band); });
}

bool GSCpuBackend::DrawSpriteFast(const GSPrimitiveBatch &batch)
{
    if (!FastPathsEnabled() || !m_vram)
        return false;

    const GSDrawState &state = batch.state;
    const GSVertex &v0 = batch.vertices[0];
    const GSVertex &v1 = batch.vertices[1];
    const auto &ctx = state.context;

    const GSSpriteAxis axisX = gsSpriteAxis(v0.x, v1.x, static_cast<float>(ctx.xyoffset.ofx) / 16.0f);
    const GSSpriteAxis axisY = gsSpriteAxis(v0.y, v1.y, static_cast<float>(ctx.xyoffset.ofy) / 16.0f);
    const u32 z1 = static_cast<u32>(v1.z);

    const int unclippedX0 = axisX.first;
    const int unclippedY0 = axisY.first;
    const int unclippedX1 = axisX.last;
    const int unclippedY1 = axisY.last;
    if (unclippedX1 < unclippedX0 || unclippedY1 < unclippedY0)
        return true;

    if (unclippedX1 < ctx.scissor.x0 || unclippedX0 > ctx.scissor.x1 ||
        unclippedY1 < ctx.scissor.y0 || unclippedY0 > ctx.scissor.y1)
        return true;

    const int drawX0 = clampInt(unclippedX0, ctx.scissor.x0, ctx.scissor.x1);
    const int drawY0 = clampInt(unclippedY0, ctx.scissor.y0, ctx.scissor.y1);
    const int drawX1 = clampInt(unclippedX1, ctx.scissor.x0, ctx.scissor.x1);
    const int drawY1 = clampInt(unclippedY1, ctx.scissor.y0, ctx.scissor.y1);

    PixelSetup &px = m_pixelSetup;
    if (!SetupPixel(px, m_vram, state))
    {
        ++FallbackCounters()[1];
        return false;
    }

    const uint8_t r = v1.r, g = v1.g, b = v1.b, a = v1.a;

    if (!state.prim.tme)
    {
        const uint32_t fpsm = ctx.frame.psm;
        const uint32_t zpsm = ctx.zbuf.psm;
        const PixelWriteMask mask = classifyAlphaTest(ctx.test, a, static_cast<uint8_t>(fpsm));
        const uint32_t ztestMethod = (((ctx.test >> 16) & 1u) != 0u ? static_cast<uint32_t>((ctx.test >> 17) & 3u) : 1u) /* ZTE=0: depth test off (ALWAYS), as PCSX2 */;
        const bool preserveDestinationAlpha = mask.writeRgb && !mask.writeAlpha && fpsm == GS_PSM_CT32;
        const bool destinationAlphaTestNeedsRead = ((ctx.test >> 14) & 0x1u) != 0u;
        const bool framebufferRead = destinationAlphaTestNeedsRead ||
                                     (mask.writesFramebuffer() && (ctx.frame.fbmsk != 0 || state.prim.abe || preserveDestinationAlpha));
        const bool writesDepth = mask.writeDepth && !ctx.zbuf.zmask;
        const bool pixelIndependent = fpsm == GS_PSM_CT32 && !framebufferRead && ztestMethod != 2u && ztestMethod != 3u &&
                                      (!writesDepth || zpsm == GS_PSM_Z32);
        if (pixelIndependent)
        {
            ++FallbackCounters()[0];
            return false;
        }

        if (m_textureCache.Live())
        {
            const WriteSpans writes = WriteFootprint(px, drawX0, drawY0, drawX1, drawY1, !ctx.zbuf.zmask);
            if (Contains(writes, m_textureCache.Page() >> 13u))
                m_textureCache.Materialize(m_vram);
        }

        m_pixelColumns.resize(static_cast<size_t>(drawX1 - drawX0 + 1));
        for (int x = drawX0; x <= drawX1; ++x)
            m_pixelColumns[static_cast<size_t>(x - drawX0)] = MakeColumn(px, static_cast<uint32_t>(x));
        const unsigned bands = PlanBands(drawY0, drawY1, drawX1 - drawX0 + 1, BandsAllowed(px, drawX0, drawY0, drawX1, drawY1));
        RunBands(drawY0, drawY1, bands, [&](int yBegin, int yEnd, unsigned)
                 {
            for (int y = yBegin; y <= yEnd; ++y)
            {
                const PixelRow pixelRow = MakeRow(px, static_cast<uint32_t>(y));
                for (int x = drawX0; x <= drawX1; ++x)
                    ShadePixel(px, pixelRow, m_pixelColumns[static_cast<size_t>(x - drawX0)], x, y, z1, r, g, b, a, v1.fog);
            } });
        return true;
    }

    TextureSampler &tex = m_sampler;
    if (!TextureSetup(tex, m_vram, state, m_clut, m_clutVersion))
    {
        ++FallbackCounters()[2];
        return false;
    }
    const int texW = state.textureWidth;
    const int texH = state.textureHeight;
    float u0f, v0f, u1f, v1f;
    if (state.prim.fst)
    {
        u0f = static_cast<float>(v0.u) / 16.0f;
        v0f = static_cast<float>(v0.v) / 16.0f;
        u1f = static_cast<float>(v1.u) / 16.0f;
        v1f = static_cast<float>(v1.v) / 16.0f;
    }
    else
    {
        const float q0 = fabsQ(v0.q);
        const float q1 = fabsQ(v1.q);
        u0f = (v0.s / q0) * static_cast<float>(texW);
        v0f = (v0.t / q0) * static_cast<float>(texH);
        u1f = (v1.s / q1) * static_cast<float>(texW);
        v1f = (v1.t / q1) * static_cast<float>(texH);
    }

    const bool linear = state.linearFilter;
    auto axisSample = [&](float texF, int size, bool isU) -> AxisSample
    {
        float coordinate;
        if (state.prim.fst)
        {
            const int fixedC = static_cast<int>((texF * 16.0f) + 0.5f);
            const uint16_t sampleC = static_cast<uint16_t>(clampInt(fixedC, 0, 0xFFFF));
            coordinate = static_cast<float>(sampleC) / 16.0f;
        }
        else
        {
            const float st = texF / static_cast<float>(size);
            const float invQ = 1.0f / fabsQ(1.0f);
            coordinate = st * invQ * static_cast<float>(size);
        }
        AxisSample out{};
        const uint8_t mode = isU ? tex.wrapU : tex.wrapV;
        const uint16_t minC = isU ? tex.minU : tex.minV;
        const uint16_t maxC = isU ? tex.maxU : tex.maxV;
        if (!linear)
        {
            out.c0 = wrapTextureCoordinate(static_cast<int>(coordinate), size, mode, minC, maxC);
            out.c1 = out.c0;
            out.frac = 0.0f;
            return out;
        }
        const float sample = coordinate - 0.5f;
        const int c0 = static_cast<int>(std::floor(sample));
        out.frac = sample - static_cast<float>(c0);
        out.c0 = wrapTextureCoordinate(c0, size, mode, minC, maxC);
        out.c1 = wrapTextureCoordinate(c0 + 1, size, mode, minC, maxC);
        return out;
    };

    const int columns = drawX1 - drawX0 + 1;
    m_columnSamples.resize(static_cast<size_t>(columns));
    m_pixelColumns.resize(static_cast<size_t>(columns));
    for (int x = drawX0; x <= drawX1; ++x)
    {
        const float tx = gsSpriteParameter(axisX, x);
        const float texUf = u0f + (u1f - u0f) * tx;
        AxisSample sample = axisSample(texUf, texW, true);
        MakeTextureParts(tex, sample, false);
        m_columnSamples[static_cast<size_t>(x - drawX0)] = sample;
        m_pixelColumns[static_cast<size_t>(x - drawX0)] = MakeColumn(px, static_cast<uint32_t>(x));
    }

    const int rows = drawY1 - drawY0 + 1;
    m_rowSamples.resize(static_cast<size_t>(rows));
    for (int y = drawY0; y <= drawY1; ++y)
    {
        const float ty = gsSpriteParameter(axisY, y);
        const float texVf = v0f + (v1f - v0f) * ty;
        AxisSample sample = axisSample(texVf, texH, false);
        MakeTextureParts(tex, sample, true);
        m_rowSamples[static_cast<size_t>(y - drawY0)] = sample;
    }

    int minU = INT32_MAX, maxU = INT32_MIN, minV = INT32_MAX, maxV = INT32_MIN;
    for (const AxisSample &sample : m_columnSamples)
    {
        minU = std::min({minU, sample.c0, sample.c1});
        maxU = std::max({maxU, sample.c0, sample.c1});
    }
    for (const AxisSample &sample : m_rowSamples)
    {
        minV = std::min({minV, sample.c0, sample.c1});
        maxV = std::max({maxV, sample.c0, sample.c1});
    }
    const PageSpan reads = Footprint(*tex.format, tex.tbp, tex.tbw, static_cast<uint32_t>(minU), static_cast<uint32_t>(minV),
                                     static_cast<uint32_t>(maxU), static_cast<uint32_t>(maxV));
    const WriteSpans writes = WriteFootprint(px, drawX0, drawY0, drawX1, drawY1, !ctx.zbuf.zmask);
    if (Overlaps(writes, reads))
    {
        ++FallbackCounters()[3];
        return false;
    }
    if (!PrepareCacheForTexturedDraw())
    {
        ++FallbackCounters()[4];
        return false;
    }

    const GSTex0Reg &tex0 = ctx.tex0;
    const unsigned bands = PlanBands(drawY0, drawY1, columns, !tex.stale && BandsAllowed(px, drawX0, drawY0, drawX1, drawY1));
    m_bandLastTexel.assign(bands, UINT32_MAX);
    RunBands(drawY0, drawY1, bands, [&](int yBegin, int yEnd, unsigned band)
             {
        uint32_t lastTexelByte = UINT32_MAX;
        for (int y = yBegin; y <= yEnd; ++y)
        {
            const AxisSample &row = m_rowSamples[static_cast<size_t>(y - drawY0)];
            const PixelRow pixelRow = MakeRow(px, static_cast<uint32_t>(y));
            for (int x = drawX0; x <= drawX1; ++x)
            {
                const AxisSample &col = m_columnSamples[static_cast<size_t>(x - drawX0)];
                uint32_t texel;
                if (linear)
                    texel = SampleBilinear(tex, col, row, lastTexelByte);
                else
                    texel = SamplePoint(tex, col, row, lastTexelByte);
                const TextureCombineResult color = combineTexture(tex0, r, g, b, a,
                                                                  static_cast<uint8_t>(texel), static_cast<uint8_t>(texel >> 8),
                                                                  static_cast<uint8_t>(texel >> 16), static_cast<uint8_t>(texel >> 24));
                ShadePixel(px, pixelRow, m_pixelColumns[static_cast<size_t>(x - drawX0)], x, y, z1,
                           color.r, color.g, color.b, color.a, v1.fog);
            }
        }
        m_bandLastTexel[band] = lastTexelByte; });
    FinishTexturedDraw(LastBandTexel());
    return true;
}

bool GSCpuBackend::DrawTriangleFast(const GSPrimitiveBatch &batch)
{
    if (!FastPathsEnabled() || !m_vram)
        return false;

    const GSDrawState &state = batch.state;
    const GSVertex &v0 = batch.vertices[0];
    const GSVertex &v1 = batch.vertices[1];
    const GSVertex &v2 = batch.vertices[2];
    const auto &ctx = state.context;

    GSTriangleRaster tri;
    if (!gsTriangleSetup(v0.x, v0.y, v1.x, v1.y, v2.x, v2.y, ctx.xyoffset.ofx, ctx.xyoffset.ofy, tri))
        return true;
    const int minX = std::max(tri.minX, static_cast<int>(ctx.scissor.x0));
    const int maxX = std::min(tri.maxX, static_cast<int>(ctx.scissor.x1));
    const int minY = std::max(tri.minY, static_cast<int>(ctx.scissor.y0));
    const int maxY = std::min(tri.maxY, static_cast<int>(ctx.scissor.y1));
    if (minX > maxX || minY > maxY)
        return true;

    PixelSetup &px = m_pixelSetup;
    if (!SetupPixel(px, m_vram, state))
        return false;

    TextureSampler &tex = m_sampler;
    const WriteSpans writes = WriteFootprint(px, minX, minY, maxX, maxY, !ctx.zbuf.zmask);
    if (state.prim.tme)
    {
        if (!TextureSetup(tex, m_vram, state, m_clut, m_clutVersion))
        {
            ++FallbackCounters()[5];
            return false;
        }
        if (Overlaps(writes, tex.footprint))
        {
            ++FallbackCounters()[6];
            return false;
        }
        if (!PrepareCacheForTexturedDraw())
        {
            ++FallbackCounters()[7];
            return false;
        }
    }
    else if (m_textureCache.Live() && Contains(writes, m_textureCache.Page() >> 13u))
    {
        m_textureCache.Materialize(m_vram);
    }

    const int columns = maxX - minX + 1;
    m_pixelColumns.resize(static_cast<size_t>(columns));
    for (int x = minX; x <= maxX; ++x)
        m_pixelColumns[static_cast<size_t>(x - minX)] = MakeColumn(px, static_cast<uint32_t>(x));

    const GSTex0Reg &tex0 = ctx.tex0;
    const bool linear = state.linearFilter;
    const int texW = state.textureWidth;
    const int texH = state.textureHeight;
    const unsigned bands = PlanBands(minY, maxY, columns, !(state.prim.tme && tex.stale) && BandsAllowed(px, minX, minY, maxX, maxY));
    m_bandLastTexel.assign(bands, UINT32_MAX);
    RunBands(minY, maxY, bands, [&](int yBegin, int yEnd, unsigned band)
             {
    uint32_t lastTexelByte = UINT32_MAX;
    for (int y = yBegin; y <= yEnd; ++y)
    {
        const PixelRow pixelRow = MakeRow(px, static_cast<uint32_t>(y));
        int64_t e[3] = {gsTriangleEdge(tri, 0, minX, y), gsTriangleEdge(tri, 1, minX, y), gsTriangleEdge(tri, 2, minX, y)};
        const int64_t stepX[3] = {tri.a[0] * 16, tri.a[1] * 16, tri.a[2] * 16};
        for (int x = minX; x <= maxX; ++x, e[0] += stepX[0], e[1] += stepX[1], e[2] += stepX[2])
        {
            const size_t i = static_cast<size_t>(x - minX);
            if (!gsTriangleCovers(tri, e))
                continue;
            const float w0 = gsTriangleWeight(tri, e[0]);
            const float w1 = gsTriangleWeight(tri, e[1]);
            const float w2 = gsTriangleWeight(tri, e[2]);

            const double z = v0.z + (v1.z - v0.z) * static_cast<double>(w1) + (v2.z - v0.z) * static_cast<double>(w2);

            uint8_t r, g, b, a;
            if (state.prim.iip)
            {
                r = clampU8(static_cast<int>(v0.r + (v1.r - v0.r) * w1 + (v2.r - v0.r) * w2));
                g = clampU8(static_cast<int>(v0.g + (v1.g - v0.g) * w1 + (v2.g - v0.g) * w2));
                b = clampU8(static_cast<int>(v0.b + (v1.b - v0.b) * w1 + (v2.b - v0.b) * w2));
                a = clampU8(static_cast<int>(v0.a + (v1.a - v0.a) * w1 + (v2.a - v0.a) * w2));
            }
            else
            {
                r = v2.r;
                g = v2.g;
                b = v2.b;
                a = v2.a;
            }

            if (state.prim.tme)
            {
                float texUf;
                float texVf;
                if (state.prim.fst)
                {
                    const uint16_t iu = static_cast<uint16_t>(v0.u * w0 + v1.u * w1 + v2.u * w2);
                    const uint16_t iv = static_cast<uint16_t>(v0.v * w0 + v1.v * w1 + v2.v * w2);
                    texUf = static_cast<float>(iu) / 16.0f;
                    texVf = static_cast<float>(iv) / 16.0f;
                }
                else
                {
                    const float is = v0.s * w0 + v1.s * w1 + v2.s * w2;
                    const float it = v0.t * w0 + v1.t * w1 + v2.t * w2;
                    const float iq = v0.q * w0 + v1.q * w1 + v2.q * w2;
                    const float invQ = 1.0f / fabsQ(iq);
                    texUf = is * invQ * static_cast<float>(texW);
                    texVf = it * invQ * static_cast<float>(texH);
                }

                uint32_t texel;
                if (!linear)
                {
                    const int su = wrapTextureCoordinate(static_cast<int>(texUf), texW, tex.wrapU, tex.minU, tex.maxU);
                    const int sv = wrapTextureCoordinate(static_cast<int>(texVf), texH, tex.wrapV, tex.minV, tex.maxV);
                    texel = Fetch(tex, su, sv, lastTexelByte);
                }
                else
                {
                    const float sampleU = texUf - 0.5f;
                    const float sampleV = texVf - 0.5f;
                    const int iu0 = static_cast<int>(std::floor(sampleU));
                    const int iv0 = static_cast<int>(std::floor(sampleV));
                    AxisSample col;
                    AxisSample row;
                    col.frac = sampleU - static_cast<float>(iu0);
                    row.frac = sampleV - static_cast<float>(iv0);
                    col.c0 = wrapTextureCoordinate(iu0, texW, tex.wrapU, tex.minU, tex.maxU);
                    col.c1 = wrapTextureCoordinate(iu0 + 1, texW, tex.wrapU, tex.minU, tex.maxU);
                    row.c0 = wrapTextureCoordinate(iv0, texH, tex.wrapV, tex.minV, tex.maxV);
                    row.c1 = wrapTextureCoordinate(iv0 + 1, texH, tex.wrapV, tex.minV, tex.maxV);
                    MakeTextureParts(tex, col, false);
                    MakeTextureParts(tex, row, true);
                    texel = SampleBilinear(tex, col, row, lastTexelByte);
                }

                const TextureCombineResult color = combineTexture(tex0, r, g, b, a,
                                                                  static_cast<uint8_t>(texel), static_cast<uint8_t>(texel >> 8),
                                                                  static_cast<uint8_t>(texel >> 16), static_cast<uint8_t>(texel >> 24));
                r = color.r;
                g = color.g;
                b = color.b;
                a = color.a;
            }

            const uint8_t fog = clampU8(static_cast<int>(v0.fog + (v1.fog - v0.fog) * w1 + (v2.fog - v0.fog) * w2));
            ShadePixel(px, pixelRow, m_pixelColumns[i], x, y, static_cast<u32>(std::clamp(z + 0.5, 0.0, 4294967295.0)),
                       r, g, b, a, fog);
        }
    }
    m_bandLastTexel[band] = lastTexelByte; });
    FinishTexturedDraw(LastBandTexel());
    return true;
}
