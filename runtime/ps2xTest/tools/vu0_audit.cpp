#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/gs/gs_frontend.h"
#include "runtime/ps2_memory.h"
#include "runtime/ps2_vu1.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <xmmintrin.h>
#include <pmmintrin.h>

struct Vu0CorpusEntry
{
    uint32_t raw;
    const char *text;
    void (*fn)(uint8_t *, R5900Context *, PS2Runtime *);
};

#include VU0_CORPUS

namespace
{
    std::mt19937 rng(12345);

    uint32_t randomLane()
    {
        const uint32_t kind = rng() % 100u;
        const uint32_t sign = (rng() & 1u) << 31;
        if (kind < 55u)
        {
            const uint32_t exponent = 127u - 20u + rng() % 41u;
            return sign | (exponent << 23) | (rng() & 0x7FFFFFu);
        }
        if (kind < 65u)
        {
            float v = static_cast<float>(static_cast<int>(rng() % 9u) - 4);
            uint32_t b;
            std::memcpy(&b, &v, 4);
            return b;
        }
        if (kind < 75u)
            return sign;
        if (kind < 80u)
            return sign | (rng() & 0x7FFFFFu);
        if (kind < 85u)
            return sign | ((253u + rng() % 2u) << 23) | (rng() & 0x7FFFFFu);
        if (kind < 90u)
            return sign | ((127u + rng() % 4u) << 23);
        if (kind < 95u)
            return sign | ((1u + rng() % 3u) << 23) | (rng() & 0x7FFFFFu);
        return sign | 0x3F800000u;
    }

    __m128 randomVector()
    {
        alignas(16) uint32_t w[4];
        for (auto &x : w)
            x = randomLane();
        return _mm_castsi128_ps(_mm_load_si128(reinterpret_cast<const __m128i *>(w)));
    }

    float randomScalar()
    {
        uint32_t b = randomLane();
        float f;
        std::memcpy(&f, &b, 4);
        return f;
    }

    bool isLowerOp(uint32_t raw)
    {
        const uint32_t f1 = raw & 0x3Fu;
        if (f1 >= 0x3Cu)
        {
            const uint32_t f2 = ((raw >> 6) & 0x1Fu) << 2 | (raw & 0x3u);
            return f2 >= 0x30u;
        }
        return f1 >= 0x30u;
    }

    uint32_t bitsOf(float f)
    {
        uint32_t b;
        std::memcpy(&b, &f, 4);
        return b;
    }

    std::string hexVec(const float v[4])
    {
        char buf[80];
        std::snprintf(buf, sizeof(buf), "%08x %08x %08x %08x", bitsOf(v[0]), bitsOf(v[1]), bitsOf(v[2]), bitsOf(v[3]));
        return buf;
    }

    int64_t ulpDistance(uint32_t a, uint32_t b)
    {
        auto ord = [](uint32_t x) -> int64_t { return (x & 0x80000000u) ? -static_cast<int64_t>(x & 0x7FFFFFFFu) : static_cast<int64_t>(x); };
        const int64_t d = ord(a) - ord(b);
        return d < 0 ? -d : d;
    }
}

int main(int argc, char **argv)
{
    const int trials = argc > 1 ? std::atoi(argv[1]) : 2000;
    const bool verbose = argc > 2;
    PS2Memory mem;
    if (!mem.initialize())
        return 2;
    GS gs;
    gs.init(mem.getGSVRAM(), static_cast<uint32_t>(PS2_GS_VRAM_SIZE), &mem.gs());
    uint8_t *code = mem.getVU1Code();
    uint8_t *data = mem.getVU1Data();
    PS2Runtime *runtime = nullptr;
    uint8_t *rdram = mem.getRDRAM();

    const unsigned defaultCsr = _mm_getcsr();
    const unsigned gameCsr = (defaultCsr & ~0x6000u) | 0x6000u | 0x8040u;

    int badOps = 0;
    for (const auto &entry : kCorpus)
    {
        const uint32_t raw = entry.raw;
        const std::string text = entry.text;
        if (text.find("vsqi") != std::string::npos || text.find("vlqd") != std::string::npos ||
            text.find("vlqi") != std::string::npos || text.find("vsqd") != std::string::npos ||
            text.find("vilwr") != std::string::npos || text.find("viswr") != std::string::npos)
            continue;
        const bool lower = isLowerOp(raw);
        const uint32_t lowerWord = lower ? (0x80000000u | (raw & 0x01FFFFFFu)) : 0x8000033Cu;
        const uint32_t upperWord = lower ? 0x000002FFu : (raw & 0x01FFFFFFu);

        int mismatches = 0;
        int ulpOnly = 0;
        std::string firstReport;
        for (int trial = 0; trial < trials; ++trial)
        {
            alignas(16) R5900Context ctx{};
            for (int r = 1; r < 32; ++r)
                ctx.vu0_vf[r] = randomVector();
            ctx.vu0_vf[0] = _mm_setr_ps(0.0f, 0.0f, 0.0f, 1.0f);
            ctx.vu0_acc = randomVector();
            ctx.vu0_q = randomScalar();
            ctx.vu0_i = randomScalar();
            for (int r = 1; r < 16; ++r)
                ctx.vi[r] = static_cast<uint16_t>(rng());
            ctx.vu0_clip_flags = rng() & 0xFFFFFFu;
            ctx.vu0_r = _mm_castsi128_ps(_mm_set1_epi32(static_cast<int>(0x3F800000u | (rng() & 0x7FFFFFu))));
            ctx.vu0_status = 0;

            static VU1Interpreter vu;
            vu.reset();
            VU1State &st = vu.state();
            for (int r = 0; r < 32; ++r)
                _mm_storeu_ps(st.vf[r], ctx.vu0_vf[r]);
            for (int r = 0; r < 16; ++r)
                st.vi[r] = static_cast<int16_t>(ctx.vi[r]);
            _mm_storeu_ps(st.acc, ctx.vu0_acc);
            st.q = ctx.vu0_q;
            st.i = ctx.vu0_i;
            st.clip = ctx.vu0_clip_flags;
            alignas(16) uint32_t rw[4];
            _mm_store_si128(reinterpret_cast<__m128i *>(rw), _mm_castps_si128(ctx.vu0_r));
            st.r = rw[0];
            st.status = 0;
            st.mac = 0;

            float before[32][4];
            for (int r = 0; r < 32; ++r)
                _mm_storeu_ps(before[r], ctx.vu0_vf[r]);
            float accBefore[4];
            _mm_storeu_ps(accBefore, ctx.vu0_acc);

            _mm_setcsr(gameCsr);
            entry.fn(rdram, &ctx, runtime);
            _mm_setcsr(defaultCsr);

            auto put = [&](uint32_t pc, uint32_t lo, uint32_t up) {
                mem.write64(PS2_VU1_CODE_BASE + pc, static_cast<uint64_t>(lo) | (static_cast<uint64_t>(up) << 32));
            };
            put(0, lowerWord, upperWord);
            for (uint32_t k = 1; k < 24; ++k)
                put(k * 8, 0x8000033Cu, 0x000002FFu | (k == 22 ? 0x40000000u : 0u));
            vu.execute(code, PS2_VU1_CODE_SIZE, data, PS2_VU1_DATA_SIZE, gs, &mem, 0u, 0u, 0u, 200u);

            std::string report;
            bool bad = false;
            bool onlyUlp = true;
            auto cmpLane = [&](const char *name, int reg, int lane, uint32_t got, uint32_t want) {
                if (got == want)
                    return;
                bad = true;
                if (ulpDistance(got, want) > 1)
                    onlyUlp = false;
                char buf[160];
                std::snprintf(buf, sizeof(buf), "  %s%d.%c macro=%08x ref=%08x\n", name, reg, "xyzw"[lane], got, want);
                report += buf;
            };
            for (int r = 1; r < 32; ++r)
            {
                float got[4];
                _mm_storeu_ps(got, ctx.vu0_vf[r]);
                for (int l = 0; l < 4; ++l)
                    cmpLane("vf", r, l, bitsOf(got[l]), bitsOf(st.vf[r][l]));
            }
            {
                float got[4];
                _mm_storeu_ps(got, ctx.vu0_acc);
                for (int l = 0; l < 4; ++l)
                    cmpLane("acc", 0, l, bitsOf(got[l]), bitsOf(st.acc[l]));
            }
            cmpLane("q", 0, 0, bitsOf(ctx.vu0_q), bitsOf(st.q));
            if ((ctx.vu0_clip_flags & 0xFFFFFFu) != (st.clip & 0xFFFFFFu))
            {
                bad = true;
                onlyUlp = false;
                char buf[96];
                std::snprintf(buf, sizeof(buf), "  clip macro=%06x ref=%06x\n", ctx.vu0_clip_flags & 0xFFFFFFu, st.clip & 0xFFFFFFu);
                report += buf;
            }
            for (int r = 1; r < 16; ++r)
            {
                if (ctx.vi[r] != static_cast<uint16_t>(st.vi[r]))
                {
                    bad = true;
                    onlyUlp = false;
                    char buf[96];
                    std::snprintf(buf, sizeof(buf), "  vi%d macro=%04x ref=%04x\n", r, ctx.vi[r], static_cast<uint16_t>(st.vi[r]));
                    report += buf;
                }
            }
            {
                alignas(16) uint32_t rr[4];
                _mm_store_si128(reinterpret_cast<__m128i *>(rr), _mm_castps_si128(ctx.vu0_r));
                if ((rr[0] & 0x7FFFFFu) != (st.r & 0x7FFFFFu))
                {
                    bad = true;
                    onlyUlp = false;
                    char buf[96];
                    std::snprintf(buf, sizeof(buf), "  R macro=%08x ref=%08x\n", rr[0], st.r);
                    report += buf;
                }
            }
            if (bad)
            {
                ++mismatches;
                if (onlyUlp)
                    ++ulpOnly;
                if (firstReport.empty() || (!onlyUlp && firstReport.find("[ulp]") != std::string::npos))
                {
                    std::string inputs;
                    const uint32_t fs = (raw >> 11) & 31u, ft = (raw >> 16) & 31u;
                    inputs += "  in fs=vf" + std::to_string(fs) + " " + hexVec(before[fs]) + "\n";
                    inputs += "  in ft=vf" + std::to_string(ft) + " " + hexVec(before[ft]) + "\n";
                    inputs += "  in acc " + hexVec(accBefore) + "\n";
                    char buf[80];
                    std::snprintf(buf, sizeof(buf), "  in q=%08x i=%08x\n", bitsOf(st.q), bitsOf(ctx.vu0_i));
                    firstReport = std::string(onlyUlp ? "[ulp]\n" : "[logic]\n") + inputs + buf + report;
                }
            }
        }
        if (mismatches != ulpOnly)
        {
            ++badOps;
            std::printf("%08x %-40s mismatches %d/%d (ulp-only %d)\n", raw, entry.text, mismatches, trials, ulpOnly);
            if (verbose || mismatches != ulpOnly)
                std::printf("%s", firstReport.c_str());
        }
    }
    std::printf("ops with mismatches: %d of %zu\n", badOps, sizeof(kCorpus) / sizeof(kCorpus[0]));
    return 0;
}
