#include "gs_golden.h"
#include "gs_reference_backend.h"
#include "runtime/gs/gs_cpu_backend.h"
#include "runtime/gs/gs_threaded_backend.h"
#include "runtime/gs/gs_gpu_backend.h"
#include "gs_sampler.h"
#include "runtime/gs/gs_frontend.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <iostream>
#include <chrono>
#include <map>
#include <memory>
#include <string>
#include <xmmintrin.h>

namespace GSRaster
{
    std::array<uint64_t, 8> &FallbackCounters();
}

namespace
{
    std::unique_ptr<GSRasterBackend> makeBackend(const std::string &name)
    {
        if (name == "ref")
            return std::make_unique<GSReferenceBackend>();
        if (name == "cpu")
            return std::make_unique<GSCpuBackend>();
        if (name == "thread")
            return std::make_unique<GSThreadedBackend>(std::make_unique<GSCpuBackend>());
        if (name == "gpu")
            return std::make_unique<GSGpuBackend>();
        if (name == "gpuuncached")
        {
            auto backend = std::make_unique<GSGpuBackend>();
            backend->SetTexturePageCacheEnabled(false);
            return backend;
        }
        if (name == "gpuoriginal")
        {
            auto backend = std::make_unique<GSGpuBackend>();
            backend->SetTexturePageCacheEnabled(false);
            backend->SetFastTriangleSetupEnabled(false);
            return backend;
        }
        if (name == "gputhreadoriginal")
        {
            auto backend = std::make_unique<GSGpuBackend>();
            backend->SetTexturePageCacheEnabled(false);
            backend->SetFastTriangleSetupEnabled(false);
            return std::make_unique<GSThreadedBackend>(std::move(backend));
        }
        if (name == "gputhread")
            return std::make_unique<GSThreadedBackend>(std::make_unique<GSGpuBackend>());
        if (name == "gputhreadcompute")
        {
            auto backend = std::make_unique<GSGpuBackend>();
            backend->SetHardwareRasterAllowed(false);
            return std::make_unique<GSThreadedBackend>(std::move(backend));
        }
        return nullptr;
    }

    uint32_t crc32(const uint8_t *data, size_t size, uint32_t crc = 0u)
    {
        static uint32_t table[256];
        static bool ready = false;
        if (!ready)
        {
            for (uint32_t i = 0; i < 256u; ++i)
            {
                uint32_t c = i;
                for (int k = 0; k < 8; ++k)
                    c = (c & 1u) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
                table[i] = c;
            }
            ready = true;
        }
        crc = ~crc;
        for (size_t i = 0; i < size; ++i)
            crc = table[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
        return ~crc;
    }

    bool writePng(const std::string &path, const uint8_t *rgba, uint32_t width, uint32_t height)
    {
        std::vector<uint8_t> raw;
        raw.reserve(static_cast<size_t>(height) * (width * 3u + 1u));
        for (uint32_t y = 0; y < height; ++y)
        {
            raw.push_back(0u);
            for (uint32_t x = 0; x < width; ++x)
            {
                const uint8_t *px = rgba + (static_cast<size_t>(y) * width + x) * 4u;
                raw.insert(raw.end(), px, px + 3);
            }
        }
        std::vector<uint8_t> z{0x78u, 0x01u};
        uint32_t a = 1u, b = 0u;
        for (uint8_t v : raw)
        {
            a = (a + v) % 65521u;
            b = (b + a) % 65521u;
        }
        for (size_t offset = 0;; offset += 65535u)
        {
            const size_t n = std::min<size_t>(65535u, raw.size() - offset);
            const bool last = offset + n >= raw.size();
            z.push_back(last ? 1u : 0u);
            z.push_back(static_cast<uint8_t>(n));
            z.push_back(static_cast<uint8_t>(n >> 8));
            z.push_back(static_cast<uint8_t>(~n));
            z.push_back(static_cast<uint8_t>(~n >> 8));
            z.insert(z.end(), raw.begin() + static_cast<std::ptrdiff_t>(offset), raw.begin() + static_cast<std::ptrdiff_t>(offset + n));
            if (last)
                break;
        }
        const uint32_t adler = (b << 16) | a;
        for (int s = 24; s >= 0; s -= 8)
            z.push_back(static_cast<uint8_t>(adler >> s));
        std::FILE *file = std::fopen(path.c_str(), "wb");
        if (!file)
            return false;
        const uint8_t signature[8] = {0x89u, 0x50u, 0x4Eu, 0x47u, 0x0Du, 0x0Au, 0x1Au, 0x0Au};
        std::fwrite(signature, 1, 8, file);
        auto chunk = [&](const char *type, const std::vector<uint8_t> &payload)
        {
            const uint32_t length = static_cast<uint32_t>(payload.size());
            const uint8_t len[4] = {static_cast<uint8_t>(length >> 24), static_cast<uint8_t>(length >> 16), static_cast<uint8_t>(length >> 8),
                                    static_cast<uint8_t>(length)};
            std::fwrite(len, 1, 4, file);
            std::vector<uint8_t> body(type, type + 4);
            body.insert(body.end(), payload.begin(), payload.end());
            std::fwrite(body.data(), 1, body.size(), file);
            const uint32_t crc = crc32(body.data(), body.size());
            const uint8_t c[4] = {static_cast<uint8_t>(crc >> 24), static_cast<uint8_t>(crc >> 16), static_cast<uint8_t>(crc >> 8),
                                  static_cast<uint8_t>(crc)};
            std::fwrite(c, 1, 4, file);
        };
        std::vector<uint8_t> header(13, 0u);
        for (int i = 0; i < 4; ++i)
        {
            header[i] = static_cast<uint8_t>(width >> (24 - 8 * i));
            header[4 + i] = static_cast<uint8_t>(height >> (24 - 8 * i));
        }
        header[8] = 8u;
        header[9] = 2u;
        chunk("IHDR", header);
        chunk("IDAT", z);
        chunk("IEND", {});
        std::fclose(file);
        return true;
    }

    GSPresentationRequest requestFor(const GSRecording::Checkpoint &checkpoint)
    {
        GSPresentationRequest request{};
        request.pmode = checkpoint.display[0];
        request.smode2 = checkpoint.display[1];
        request.dispfb1 = checkpoint.display[2];
        request.display1 = checkpoint.display[3];
        request.dispfb2 = checkpoint.display[4];
        request.display2 = checkpoint.display[5];
        request.bgcolor = checkpoint.display[6];
        request.vsyncTick = checkpoint.tick;
        return request;
    }

    int compareBackends(const GSGolden::Recording &recording, const std::string &nameA, const std::string &nameB, const std::string &dumpDir)
    {
        auto backendA = makeBackend(nameA);
        auto backendB = makeBackend(nameB);
        if (!backendA || !backendB)
        {
            std::cerr << "unknown backend\n";
            return 2;
        }
        std::vector<uint8_t> vramA = recording.initial.vram;
        std::vector<uint8_t> vramB = recording.initial.vram;
        GSRasterBackend *backends[2] = {backendA.get(), backendB.get()};
        std::vector<uint8_t> *vrams[2] = {&vramA, &vramB};
        double seconds[2] = {0.0, 0.0};
        for (int i = 0; i < 2; ++i)
        {
            backends[i]->Initialize(vrams[i]->data(), static_cast<uint32_t>(vrams[i]->size()));
            if (auto *access = dynamic_cast<GSBackendStateAccess *>(backends[i]))
                access->ImportState(recording.initial.state);
        }
        uint32_t field = 0u;
        int status = 0;
        for (const GSRecording::Record &record : recording.records)
        {
            if (record.op == GSRecording::Op::Hash)
                continue;
            if (record.op == GSRecording::Op::End)
                break;
            if (record.op != GSRecording::Op::Field)
            {
                for (int i = 0; i < 2; ++i)
                {
                    const auto start = std::chrono::steady_clock::now();
                    GSRecording::Apply(*backends[i], record);
                    seconds[i] += std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
                }
                continue;
            }
            GSRecording::Checkpoint checkpoint;
            if (!GSRecording::ParseCheckpoint(record, checkpoint))
                continue;
            PresentationFrame frames[2];
            for (int i = 0; i < 2; ++i)
            {
                const auto start = std::chrono::steady_clock::now();
                backends[i]->Flush();
                backends[i]->Sync(GSSyncReason::DebugReadback);
                frames[i] = backends[i]->Present(requestFor(checkpoint));
                seconds[i] += std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            }
            size_t vramDiff = 0u;
            for (size_t k = 0; k < vramA.size(); ++k)
                vramDiff += vramA[k] != vramB[k];
            size_t pixelDiff = 0u;
            double absSum = 0.0;
            int maxDiff = 0;
            std::vector<uint8_t> diff;
            if (frames[0].width == frames[1].width && frames[0].height == frames[1].height && frames[0])
            {
                const size_t count = static_cast<size_t>(frames[0].width) * frames[0].height;
                diff.assign(count * 4u, 0u);
                for (size_t p = 0; p < count; ++p)
                {
                    int worst = 0;
                    for (int c = 0; c < 3; ++c)
                    {
                        const int d = std::abs(int(frames[0].pixels[p * 4u + c]) - int(frames[1].pixels[p * 4u + c]));
                        worst = std::max(worst, d);
                        absSum += d;
                    }
                    pixelDiff += worst != 0;
                    maxDiff = std::max(maxDiff, worst);
                    const uint8_t v = static_cast<uint8_t>(std::min(255, worst * 8));
                    diff[p * 4u] = v;
                    diff[p * 4u + 1u] = v;
                    diff[p * 4u + 2u] = v;
                    diff[p * 4u + 3u] = 255u;
                }
                std::printf("field %u (tick %llu): %ux%u pixels differing %zu (%.2f%%), mean abs %.3f, max %d; VRAM bytes differing %zu\n",
                            field, static_cast<unsigned long long>(checkpoint.tick), frames[0].width, frames[0].height, pixelDiff,
                            100.0 * static_cast<double>(pixelDiff) / static_cast<double>(count), absSum / (3.0 * static_cast<double>(count)), maxDiff,
                            vramDiff);
            }
            else
            {
                std::printf("field %u: frame size mismatch %ux%u vs %ux%u; VRAM bytes differing %zu\n", field, frames[0].width, frames[0].height,
                            frames[1].width, frames[1].height, vramDiff);
                status = 1;
            }
            if (vramDiff || pixelDiff)
                status = 1;
            if (!dumpDir.empty())
            {
                const std::string base = dumpDir + "/field" + std::to_string(field);
                if (frames[0])
                    writePng(base + "_" + nameA + ".png", frames[0].pixels.data(), frames[0].width, frames[0].height);
                if (frames[1])
                    writePng(base + "_" + nameB + ".png", frames[1].pixels.data(), frames[1].width, frames[1].height);
                if (!diff.empty())
                    writePng(base + "_diff.png", diff.data(), frames[0].width, frames[0].height);
            }
            ++field;
        }
        std::printf("time %s %.3f s, %s %.3f s\n", nameA.c_str(), seconds[0], nameB.c_str(), seconds[1]);
        if (auto *gpu = dynamic_cast<GSGpuBackend *>(backendB.get()))
        {
            const auto &s = gpu->GetStats();
            std::printf("gpu: prims %llu batches %llu tiles %llu clut %llu flushes target %llu texture %llu other %llu\n",
                        (unsigned long long)s.prims, (unsigned long long)s.batches, (unsigned long long)s.tiles, (unsigned long long)s.clutLoads,
                        (unsigned long long)s.flushTarget, (unsigned long long)s.flushTexture, (unsigned long long)s.flushOther);
        }
        return status;
    }

    void writeDraw(std::FILE *file, const GSPrimitiveBatch &batch)
    {
        const GSDrawState &st = batch.state;
        const GSContext &c = st.context;
        const GSPrimReg &p = st.prim;
        std::fprintf(file,
                     "D prim=%u iip=%d tme=%d fge=%d abe=%d aa1=%d fst=%d ctxt=%d fix=%d"
                     " frame=%x/%u/%02x/%08x zbuf=%x/%02x/%d"
                     " tex0=%x/%u/%02x/%ux%u/tcc%u/tfx%u/cbp%x/cpsm%02x/csm%u/csa%u/cld%u"
                     " tex1=%llx clamp=%llx alpha=%llx test=%llx fba=%llx scissor=%u-%u,%u-%u ofs=%u,%u"
                     " texa=%u/%d/%u pabe=%d dthe=%llx colclamp=%llx",
                     static_cast<unsigned>(p.type), p.iip, p.tme, p.fge, p.abe, p.aa1, p.fst, p.ctxt, p.fix,
                     c.frame.fbp, c.frame.fbw, c.frame.psm, c.frame.fbmsk, c.zbuf.zbp, c.zbuf.psm, c.zbuf.zmask,
                     c.tex0.tbp0, c.tex0.tbw, c.tex0.psm, 1u << c.tex0.tw, 1u << c.tex0.th, c.tex0.tcc, c.tex0.tfx,
                     c.tex0.cbp, c.tex0.cpsm, c.tex0.csm, c.tex0.csa, c.tex0.cld,
                     static_cast<unsigned long long>(c.tex1), static_cast<unsigned long long>(c.clamp),
                     static_cast<unsigned long long>(c.alpha), static_cast<unsigned long long>(c.test),
                     static_cast<unsigned long long>(c.fba), c.scissor.x0, c.scissor.x1, c.scissor.y0, c.scissor.y1,
                     c.xyoffset.ofx, c.xyoffset.ofy, st.texa.ta0, st.texa.aem, st.texa.ta1, st.pabe,
                     static_cast<unsigned long long>(st.dthe), static_cast<unsigned long long>(st.colclamp));
        for (uint32_t i = 0; i < batch.vertexCount; ++i)
        {
            const GSVertex &v = batch.vertices[i];
            std::fprintf(file, " | %.2f,%.2f z=%.0f rgba=%02x%02x%02x%02x uv=%.2f,%.2f st=%g,%g q=%g f=%u",
                         v.x, v.y, v.z, v.r, v.g, v.b, v.a, v.u / 16.0, v.v / 16.0, v.s, v.t, v.q, static_cast<unsigned>(v.fog));
        }
        std::fputc('\n', file);
    }

    int writeText(const GSGolden::Recording &recording, const std::string &path)
    {
        std::FILE *file = std::fopen(path.c_str(), "w");
        if (!file)
            return 1;
        auto writeField = [&](const GSRecording::Checkpoint &cp)
        {
            std::fprintf(file,
                         "V tick=%llu pmode=%llx smode2=%llx dispfb1=%llx display1=%llx dispfb2=%llx display2=%llx bgcolor=%llx\n",
                         static_cast<unsigned long long>(cp.tick),
                         static_cast<unsigned long long>(cp.display[0]), static_cast<unsigned long long>(cp.display[1]),
                         static_cast<unsigned long long>(cp.display[2]), static_cast<unsigned long long>(cp.display[3]),
                         static_cast<unsigned long long>(cp.display[4]), static_cast<unsigned long long>(cp.display[5]),
                         static_cast<unsigned long long>(cp.display[6]));
        };
        writeField(recording.initial);
        for (const GSRecording::Record &record : recording.records)
        {
            switch (record.op)
            {
            case GSRecording::Op::Submit:
            {
                GSPrimitiveBatch batch{};
                std::memcpy(&batch, record.payload.data(), sizeof(batch));
                writeDraw(file, batch);
                break;
            }
            case GSRecording::Op::BeginTransfer:
            {
                GSTransferCommand cmd{};
                std::memcpy(&cmd, record.payload.data(), sizeof(cmd));
                const GSBitBltBuf &b = cmd.bitbltbuf;
                std::fprintf(file, "T dir=%u src=%x/%u/%02x dst=%x/%u/%02x ss=%u,%u ds=%u,%u size=%ux%u\n",
                             cmd.direction, b.sbp, b.sbw, b.spsm, b.dbp, b.dbw, b.dpsm, cmd.trxpos.ssax,
                             cmd.trxpos.ssay, cmd.trxpos.dsax, cmd.trxpos.dsay, cmd.trxreg.rrw, cmd.trxreg.rrh);
                break;
            }
            case GSRecording::Op::Upload:
                std::fprintf(file, "I %u ", static_cast<unsigned>(record.payload.size()));
                for (uint8_t byte : record.payload)
                    std::fprintf(file, "%02x", byte);
                std::fputc('\n', file);
                break;
            case GSRecording::Op::Field:
            {
                GSRecording::Checkpoint cp;
                if (GSRecording::ParseCheckpoint(record, cp))
                    writeField(cp);
                break;
            }
            default:
                break;
            }
        }
        std::fclose(file);
        return 0;
    }
}

namespace
{
    std::string profileKey(const GSRecording::Record &record)
    {
        if (record.op != GSRecording::Op::Submit)
            return "op" + std::to_string(static_cast<int>(record.op));
        GSPrimitiveBatch batch{};
        std::memcpy(&batch, record.payload.data(), sizeof(batch));
        const GSContext &c = batch.state.context;
        const GSPrimReg &p = batch.state.prim;
        char key[256];
        std::snprintf(key, sizeof(key), "prim%u tme%d fst%d lin%d tpsm%02x abe%d fpsm%02x test%05llx alpha%02llx fge%d iip%d",
                      static_cast<unsigned>(p.type), p.tme, p.fst, batch.state.linearFilter, p.tme ? c.tex0.psm : 0xFF, p.abe,
                      c.frame.psm, static_cast<unsigned long long>(c.test & 0x7FFFF),
                      static_cast<unsigned long long>(c.alpha & 0xFF), p.fge, p.iip);
        return key;
    }

    void profile(const GSGolden::Recording &recording, GSRasterBackend &backend)
    {
        std::vector<uint8_t> vram = recording.initial.vram;
        backend.Initialize(vram.data(), static_cast<uint32_t>(vram.size()));
        if (auto *access = dynamic_cast<GSBackendStateAccess *>(&backend))
            access->ImportState(recording.initial.state);
        std::map<std::string, std::pair<double, size_t>> totals;
        double all = 0.0;
        for (const GSRecording::Record &record : recording.records)
        {
            const auto start = std::chrono::steady_clock::now();
            GSRecording::Apply(backend, record);
            const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            auto &entry = totals[profileKey(record)];
            entry.first += seconds;
            entry.second += 1u;
            all += seconds;
        }
        std::vector<std::pair<double, std::string>> sorted;
        for (const auto &kv : totals)
            sorted.push_back({kv.second.first, kv.first + " x" + std::to_string(kv.second.second)});
        std::sort(sorted.rbegin(), sorted.rend());
        std::printf("total %.3f s\n", all);
        const auto &fallbacks = GSRaster::FallbackCounters();
        std::printf("fallbacks: spriteFill %llu spriteSetup %llu spriteTex %llu spriteOverlap %llu spriteCache %llu triTex %llu triOverlap %llu triCache %llu\n",
                    (unsigned long long)fallbacks[0], (unsigned long long)fallbacks[1], (unsigned long long)fallbacks[2],
                    (unsigned long long)fallbacks[3], (unsigned long long)fallbacks[4], (unsigned long long)fallbacks[5],
                    (unsigned long long)fallbacks[6], (unsigned long long)fallbacks[7]);
        for (size_t i = 0; i < sorted.size() && i < 25; ++i)
            std::printf("%8.1f ms %5.1f%%  %s\n", sorted[i].first * 1000.0, 100.0 * sorted[i].first / all, sorted[i].second.c_str());
    }
}

namespace GSRaster
{
    long ExactLround(float value);
}

namespace
{
    int checkLround()
    {
        const unsigned modes[] = {0x1F80u, 0xFFC0u, 0x7F80u};
        const unsigned saved = _mm_getcsr();
        int failures = 0;
        for (unsigned mode : modes)
        {
            _mm_setcsr(mode);
            uint64_t checked = 0u;
            for (uint32_t bits = 0u;; ++bits)
            {
                float value;
                std::memcpy(&value, &bits, 4u);
                if (std::fabs(value) <= 1024.0f)
                {
                    ++checked;
                    if (GSRaster::ExactLround(value) != std::lround(value))
                    {
                        if (failures++ < 10)
                            std::printf("mismatch mode %x value %a: exact %ld lround %ld\n", mode, value,
                                        GSRaster::ExactLround(value), std::lround(value));
                    }
                }
                if (bits == 0xFFFFFFFFu)
                    break;
            }
            std::printf("mode %x: %llu floats checked\n", mode, static_cast<unsigned long long>(checked));
        }
        _mm_setcsr(saved);
        std::printf("%s\n", failures ? "LROUND MISMATCH" : "lround equivalent");
        return failures ? 1 : 0;
    }
}

namespace
{
    int playGsDump(const std::string &path, const std::string &backendName, const std::string &dumpDir)
    {
        std::FILE *file = std::fopen(path.c_str(), "rb");
        if (!file)
        {
            std::cerr << "cannot open " << path << "\n";
            return 2;
        }
        std::vector<uint8_t> data;
        uint8_t chunk[65536];
        size_t got;
        while ((got = std::fread(chunk, 1, sizeof(chunk), file)) > 0)
            data.insert(data.end(), chunk, chunk + got);
        std::fclose(file);
        const size_t vramSize = 4u * 1024u * 1024u;
        if (data.size() < 8u + vramSize + 4u || std::memcmp(data.data(), "PS2XGSD1", 8) != 0)
        {
            std::cerr << "not an exported GS dump\n";
            return 2;
        }
        _mm_setcsr(0xFFC0u);
        std::vector<uint8_t> vram(data.begin() + 8, data.begin() + 8 + vramSize);
        auto owned = makeBackend(backendName);
        if (!owned)
        {
            std::cerr << "unknown backend\n";
            return 2;
        }
        GSRasterBackend *backend = owned.get();
        GS gs;
        gs.setRasterBackend(std::move(owned));
        gs.init(vram.data(), static_cast<uint32_t>(vram.size()), nullptr);
        size_t o = 8u + vramSize;
        uint32_t initCount = 0u;
        std::memcpy(&initCount, data.data() + o, 4u);
        o += 4u;
        for (uint32_t i = 0; i < initCount; ++i)
        {
            uint64_t value = 0u;
            std::memcpy(&value, data.data() + o + 1u, 8u);
            gs.writeRegister(data[o], value);
            o += 9u;
        }
        uint32_t field = 0u;
        uint32_t packets = 0u;
        uint32_t snapshotFirst = 0u;
        uint32_t snapshotLast = 0u;
        uint32_t snapshotEvery = 0u;
        if (const char *spec = std::getenv("GS_REPLAY_SNAPSHOTS"))
            std::sscanf(spec, "%u:%u:%u", &snapshotFirst, &snapshotLast, &snapshotEvery);
        while (o < data.size())
        {
            const uint8_t kind = data[o++];
            if (kind == 0u)
            {
                uint32_t size = 0u;
                std::memcpy(&size, data.data() + o, 4u);
                o += 4u;
                gs.processGIFPacket(data.data() + o, size);
                o += size;
                ++packets;
                if (snapshotEvery != 0u && !dumpDir.empty() && packets >= snapshotFirst && packets <= snapshotLast &&
                    (packets - snapshotFirst) % snapshotEvery == 0u)
                {
                    backend->Flush();
                    backend->Sync(GSSyncReason::DebugReadback);
                    std::vector<uint8_t> snapshot;
                    backend->SnapshotVram(snapshot);
                    if (std::FILE *out = std::fopen((dumpDir + "/snap_" + std::to_string(packets) + ".vram").c_str(), "wb"))
                    {
                        std::fwrite(snapshot.data(), 1, snapshot.size(), out);
                        std::fclose(out);
                    }
                }
                continue;
            }
            if (kind != 1u)
            {
                std::cerr << "bad record " << static_cast<unsigned>(kind) << "\n";
                return 2;
            }
            uint64_t display[7];
            std::memcpy(display, data.data() + o, sizeof(display));
            o += sizeof(display);
            backend->Flush();
            backend->Sync(GSSyncReason::DebugReadback);
            GSPresentationRequest request{};
            request.pmode = display[0];
            request.smode2 = display[1];
            request.dispfb1 = display[2];
            request.display1 = display[3];
            request.dispfb2 = display[4];
            request.display2 = display[5];
            request.bgcolor = display[6];
            const PresentationFrame frame = backend->Present(request);
            if (!dumpDir.empty() && frame)
                writePng(dumpDir + "/dump_field" + std::to_string(field) + ".png", frame.pixels.data(), frame.width, frame.height);
            std::printf("field %u after %u packets\n", field, packets);
            ++field;
        }
        backend->Flush();
        backend->Sync(GSSyncReason::DebugReadback);
        if (!dumpDir.empty())
        {
            std::vector<uint8_t> snapshot;
            backend->SnapshotVram(snapshot);
            if (std::FILE *out = std::fopen((dumpDir + "/dump_end.vram").c_str(), "wb"))
            {
                std::fwrite(snapshot.data(), 1, snapshot.size(), out);
                std::fclose(out);
            }
        }
        return 0;
    }
}

int main(int argc, char **argv)
{
    bool reuse = false;
    if (argc == 2 && std::string(argv[1]) == "--check-lround")
        return checkLround();
    if (argc >= 3 && std::string(argv[1]) == "--gsdump")
        return playGsDump(argv[2], argc >= 4 ? argv[3] : "cpu", argc >= 5 ? argv[4] : "");
    if (argc < 2)
    {
        std::cerr << "usage: gs_replay <recording.gsr> [--backend cpu|ref] [--repeat N] [--no-compare]"
                     " [--lockstep] [--text out.txt]\n";
        return 2;
    }
    const std::string path = argv[1];
    if (const char *csr = std::getenv("GS_REPLAY_MXCSR"))
        _mm_setcsr(static_cast<unsigned>(std::strtoul(csr, nullptr, 16)));
    std::string backendName = "cpu";
    int repeat = 1;
    bool compare = true;
    bool lockstep = false;
    bool doProfile = false;
    bool doSample = false;
    std::string textPath;
    std::string compareA;
    std::string compareB;
    std::string dumpDir;
    for (int i = 2; i < argc; ++i)
    {
        const std::string arg = argv[i];
        if (arg == "--compare-backends" && i + 2 < argc)
        {
            compareA = argv[++i];
            compareB = argv[++i];
            continue;
        }
        if (arg == "--dump" && i + 1 < argc)
        {
            dumpDir = argv[++i];
            continue;
        }
        if (arg == "--backend" && i + 1 < argc)
            backendName = argv[++i];
        else if (arg == "--repeat" && i + 1 < argc)
            repeat = std::max(1, std::atoi(argv[++i]));
        else if (arg == "--reuse")
            reuse = true;
        else if (arg == "--no-compare")
            compare = false;
        else if (arg == "--lockstep")
            lockstep = true;
        else if (arg == "--profile")
            doProfile = true;
        else if (arg == "--sample")
            doSample = true;
        else if (arg == "--text" && i + 1 < argc)
            textPath = argv[++i];
    }

    GSGolden::Recording recording;
    std::string error;
    if (!GSGolden::Load(path, recording, error))
    {
        std::cerr << "load failed: " << error << "\n";
        return 1;
    }
    std::cout << path << ": " << recording.records.size() << " records, " << recording.submits << " draws, "
              << recording.checkpoints << " checkpoints\n";

    if (!textPath.empty())
        return writeText(recording, textPath);

    if (!compareA.empty())
        return compareBackends(recording, compareA, compareB, dumpDir);

    if (doProfile)
    {
        auto backend = makeBackend(backendName);
        profile(recording, *backend);
        return 0;
    }

    if (lockstep)
    {
        auto reference = makeBackend("ref");
        auto candidate = makeBackend(backendName);
        if (!candidate)
        {
            std::cerr << "unknown backend " << backendName << "\n";
            return 2;
        }
        const GSGolden::Report report = GSGolden::Lockstep(recording, *reference, *candidate);
        std::cout << (report.ok ? "lockstep OK" : "lockstep MISMATCH: " + report.message) << "\n";
        return report.ok ? 0 : 1;
    }

    if (doSample)
    {
        GSSampler sampler;
        sampler.Start();
        for (int run = 0; run < repeat; ++run)
        {
            auto backend = makeBackend(backendName);
            GSGolden::Replay(recording, *backend, false);
        }
        sampler.StopAndReport(40);
        return 0;
    }

    int status = 0;
    double best = 1e30;
    std::unique_ptr<GSRasterBackend> shared = reuse ? makeBackend(backendName) : nullptr;
    for (int run = 0; run < repeat; ++run)
    {
        auto backend = reuse ? std::move(shared) : makeBackend(backendName);
        if (!backend)
        {
            std::cerr << "unknown backend " << backendName << "\n";
            return 2;
        }
        const GSGolden::Report report = GSGolden::Replay(recording, *backend, compare);
        best = std::min(best, report.seconds);
        std::printf("run %d [%s]: %.3f s (%.1f ms/field) %s%s\n", run, backendName.c_str(), report.seconds,
                    recording.checkpoints ? 1000.0 * report.seconds / static_cast<double>(recording.checkpoints) : 0.0,
                    compare ? (report.ok ? "bit-exact" : "MISMATCH: ") : "", report.ok ? "" : report.message.c_str());
        if (!report.ok)
            status = 1;
        if (reuse)
            shared = std::move(backend);
    }
    std::printf("best: %.3f s\n", best);
    return status;
}
