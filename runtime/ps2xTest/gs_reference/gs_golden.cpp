#include "gs_golden.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <sstream>
#include <xmmintrin.h>

namespace
{
    struct MxcsrScope
    {
        unsigned saved = _mm_getcsr();
        ~MxcsrScope() { _mm_setcsr(saved); }
    };

    bool sameTransfer(const GSTransferCommand &a, const GSTransferCommand &b)
    {
        return a.bitbltbuf.sbp == b.bitbltbuf.sbp && a.bitbltbuf.sbw == b.bitbltbuf.sbw &&
               a.bitbltbuf.spsm == b.bitbltbuf.spsm && a.bitbltbuf.dbp == b.bitbltbuf.dbp &&
               a.bitbltbuf.dbw == b.bitbltbuf.dbw && a.bitbltbuf.dpsm == b.bitbltbuf.dpsm &&
               a.trxpos.ssax == b.trxpos.ssax && a.trxpos.ssay == b.trxpos.ssay &&
               a.trxpos.dsax == b.trxpos.dsax && a.trxpos.dsay == b.trxpos.dsay &&
               a.trxpos.dir == b.trxpos.dir && a.trxreg.rrw == b.trxreg.rrw &&
               a.trxreg.rrh == b.trxreg.rrh && a.direction == b.direction;
    }

    bool sameTransferState(const GSTransferSnapshot &a, const GSTransferSnapshot &b)
    {
        return a.x == b.x && a.y == b.y && a.totalPixels == b.totalPixels &&
               a.copiedPixels == b.copiedPixels && a.direction == b.direction &&
               a.localToHostPendingBytes == b.localToHostPendingBytes;
    }

    const char *opName(GSRecording::Op op)
    {
        switch (op)
        {
        case GSRecording::Op::State: return "State";
        case GSRecording::Op::Submit: return "Submit";
        case GSRecording::Op::LoadClut: return "LoadClut";
        case GSRecording::Op::BeginTransfer: return "BeginTransfer";
        case GSRecording::Op::Upload: return "Upload";
        case GSRecording::Op::TextureFlush: return "TextureFlush";
        case GSRecording::Op::Clear: return "Clear";
        case GSRecording::Op::WriteVram: return "WriteVram";
        case GSRecording::Op::Consume: return "Consume";
        case GSRecording::Op::Field: return "Field";
        case GSRecording::Op::ReadVram: return "ReadVram";
        case GSRecording::Op::Reset: return "Reset";
        case GSRecording::Op::End: return "End";
        case GSRecording::Op::Hash: return "Hash";
        case GSRecording::Op::Mxcsr: return "Mxcsr";
        }
        return "?";
    }

    std::string describeRecord(const GSRecording::Record &record)
    {
        std::ostringstream out;
        out << opName(record.op);
        if (record.op == GSRecording::Op::Submit && record.payload.size() >= sizeof(GSPrimitiveBatch))
        {
            GSPrimitiveBatch batch{};
            std::memcpy(&batch, record.payload.data(), sizeof(batch));
            const GSContext &c = batch.state.context;
            const GSPrimReg &p = batch.state.prim;
            out << " prim=" << static_cast<unsigned>(p.type) << " iip=" << p.iip << " tme=" << p.tme
                << " fge=" << p.fge << " abe=" << p.abe << " fst=" << p.fst
                << std::hex << " fbp=" << c.frame.fbp << " psm=" << static_cast<unsigned>(c.frame.psm)
                << " fbmsk=" << c.frame.fbmsk << " zbp=" << c.zbuf.zbp << " zpsm=" << static_cast<unsigned>(c.zbuf.psm)
                << " zmask=" << c.zbuf.zmask << " tbp=" << c.tex0.tbp0 << " tpsm=" << static_cast<unsigned>(c.tex0.psm)
                << " tex1=" << c.tex1 << " clamp=" << c.clamp << " alpha=" << c.alpha << " test=" << c.test
                << std::dec;
            for (uint32_t i = 0; i < batch.vertexCount; ++i)
            {
                const GSVertex &v = batch.vertices[i];
                out << " | " << v.x << "," << v.y << " z=" << v.z;
            }
        }
        else
        {
            out << " bytes=" << record.payload.size();
        }
        return out.str();
    }
}

namespace GSGolden
{
    bool Load(const std::string &path, Recording &out, std::string &error)
    {
        GSRecording::Reader reader;
        if (!reader.Open(path))
        {
            error = reader.Error();
            return false;
        }
        out = {};
        GSRecording::Record record;
        bool haveInitial = false;
        while (reader.Next(record))
        {
            if (record.op == GSRecording::Op::State)
            {
                if (!GSRecording::ParseCheckpoint(record, out.initial))
                {
                    error = "bad initial state";
                    return false;
                }
                haveInitial = true;
                continue;
            }
            if (record.op == GSRecording::Op::Submit)
                ++out.submits;
            if (record.op == GSRecording::Op::Field || record.op == GSRecording::Op::End)
                ++out.checkpoints;
            out.records.push_back(std::move(record));
            record = {};
        }
        if (!reader.Error().empty())
        {
            error = reader.Error();
            return false;
        }
        if (!haveInitial)
        {
            error = "recording has no initial state";
            return false;
        }
        return true;
    }

    std::string CompareVram(const uint8_t *expected, const uint8_t *actual, size_t size)
    {
        if (std::memcmp(expected, actual, size) == 0)
            return {};
        size_t first = SIZE_MAX;
        size_t last = 0u;
        size_t count = 0u;
        for (size_t i = 0; i < size; ++i)
        {
            if (expected[i] != actual[i])
            {
                first = std::min(first, i);
                last = i;
                ++count;
            }
        }
        std::ostringstream out;
        out << count << " VRAM bytes differ, first at 0x" << std::hex << first << " (page 0x" << (first >> 13)
            << ", block 0x" << ((first >> 8) & 0x1F) << ") expected " << static_cast<unsigned>(expected[first])
            << " got " << static_cast<unsigned>(actual[first]) << ", last at 0x" << last;
        return out.str();
    }

    std::string CompareState(const GSBackendState &expected, const GSBackendState &actual)
    {
        if (expected.clut != actual.clut)
            return "CLUT differs";
        if (expected.clutCbp != actual.clutCbp)
            return "CLUT CBP differs";
        if (expected.cachePageBase != actual.cachePageBase)
        {
            std::ostringstream out;
            out << "texture cache page differs: expected 0x" << std::hex << expected.cachePageBase << " got 0x"
                << actual.cachePageBase;
            return out.str();
        }
        if (expected.cachePageBase != UINT32_MAX && expected.cacheBytes != actual.cacheBytes)
            return "texture cache contents differ";
        if (!sameTransfer(expected.transfer, actual.transfer))
            return "transfer command differs";
        if (!sameTransferState(expected.transferState, actual.transferState))
            return "transfer state differs";
        if (expected.localToHost != actual.localToHost || expected.localToHostReadPos != actual.localToHostReadPos)
            return "local-to-host buffer differs";
        return {};
    }

    Report Replay(const Recording &recording, GSRasterBackend &backend, bool compare, bool checkHashes)
    {
        MxcsrScope mxcsr;
        Report report;
        std::vector<uint8_t> vram = recording.initial.vram;
        backend.Initialize(vram.data(), static_cast<uint32_t>(vram.size()));
        if (auto *access = dynamic_cast<GSBackendStateAccess *>(&backend))
            access->ImportState(recording.initial.state);

        using Clock = std::chrono::steady_clock;
        Clock::duration busy{};
        auto start = Clock::now();
        for (size_t i = 0; i < recording.records.size(); ++i)
        {
            const GSRecording::Record &record = recording.records[i];
            if (record.op == GSRecording::Op::Field || record.op == GSRecording::Op::End)
            {
                backend.Flush();
                backend.Sync(GSSyncReason::DebugReadback);
                busy += Clock::now() - start;
                if (compare)
                {
                    GSRecording::Checkpoint checkpoint;
                    if (!GSRecording::ParseCheckpoint(record, checkpoint))
                    {
                        report.ok = false;
                        report.message = "bad checkpoint record";
                        return report;
                    }
                    std::string difference = CompareVram(checkpoint.vram.data(), vram.data(), vram.size());
                    if (difference.empty())
                    {
                        GSBackendState state;
                        if (auto *access = dynamic_cast<GSBackendStateAccess *>(&backend))
                        {
                            access->ExportState(state);
                            difference = CompareState(checkpoint.state, state);
                        }
                    }
                    if (!difference.empty())
                    {
                        if (const char *dump = std::getenv("PS2X_GS_GOLDEN_DUMP"))
                        {
                            if (std::FILE *file = std::fopen((std::string(dump) + ".expected").c_str(), "wb"))
                            {
                                std::fwrite(checkpoint.vram.data(), 1, checkpoint.vram.size(), file);
                                std::fclose(file);
                            }
                            if (std::FILE *file = std::fopen((std::string(dump) + ".actual").c_str(), "wb"))
                            {
                                std::fwrite(vram.data(), 1, vram.size(), file);
                                std::fclose(file);
                            }
                        }
                        report.ok = false;
                        report.firstFailingRecord = i;
                        report.message = "record " + std::to_string(i) + " field " + std::to_string(checkpoint.tick) + ": " + difference;
                        report.seconds = std::chrono::duration<double>(busy).count();
                        return report;
                    }
                    ++report.checkpointsCompared;
                }
                start = Clock::now();
                continue;
            }
            if (record.op == GSRecording::Op::Hash)
            {
                if (!compare || !checkHashes || record.payload.size() != sizeof(uint64_t))
                    continue;
                backend.Flush();
                backend.Sync(GSSyncReason::DebugReadback);
                uint64_t expected = 0u;
                std::memcpy(&expected, record.payload.data(), sizeof(expected));
                if (GSRecording::HashVram(vram.data(), vram.size()) != expected)
                {
                    report.ok = false;
                    report.firstFailingRecord = i;
                    report.message = "VRAM hash differs after record " + std::to_string(i - 1) + " (" +
                                     describeRecord(recording.records[i - 1]) + ")";
                    if (const char *dump = std::getenv("PS2X_GS_GOLDEN_DUMP"))
                    {
                        if (std::FILE *file = std::fopen((std::string(dump) + ".actual").c_str(), "wb"))
                        {
                            std::fwrite(vram.data(), 1, vram.size(), file);
                            std::fclose(file);
                        }
                    }
                    return report;
                }
                continue;
            }
            const GSRecording::ApplyResult result = GSRecording::Apply(backend, record);
            if (!result.ok)
            {
                report.ok = false;
                report.firstFailingRecord = i;
                report.message = "record " + std::to_string(i) + ": " + result.message;
                return report;
            }
        }
        backend.Flush();
        backend.Sync(GSSyncReason::DebugReadback);
        busy += Clock::now() - start;
        report.seconds = std::chrono::duration<double>(busy).count();
        return report;
    }

    Report Lockstep(const Recording &recording, GSRasterBackend &reference, GSRasterBackend &candidate)
    {
        MxcsrScope mxcsr;
        Report report;
        std::vector<uint8_t> refVram = recording.initial.vram;
        std::vector<uint8_t> candVram = recording.initial.vram;
        reference.Initialize(refVram.data(), static_cast<uint32_t>(refVram.size()));
        candidate.Initialize(candVram.data(), static_cast<uint32_t>(candVram.size()));
        for (GSRasterBackend *backend : {&reference, &candidate})
            if (auto *access = dynamic_cast<GSBackendStateAccess *>(backend))
                access->ImportState(recording.initial.state);

        for (size_t i = 0; i < recording.records.size(); ++i)
        {
            const GSRecording::Record &record = recording.records[i];
            GSRecording::Apply(reference, record);
            GSRecording::Apply(candidate, record);
            candidate.Flush();
            candidate.Sync(GSSyncReason::DebugReadback);
            std::string difference = CompareVram(refVram.data(), candVram.data(), refVram.size());
            if (difference.empty())
            {
                GSBackendState a;
                GSBackendState b;
                auto *ra = dynamic_cast<GSBackendStateAccess *>(&reference);
                auto *ca = dynamic_cast<GSBackendStateAccess *>(&candidate);
                if (ra && ca && ra->ExportState(a) && ca->ExportState(b))
                    difference = CompareState(a, b);
            }
            if (!difference.empty())
            {
                report.ok = false;
                report.firstFailingRecord = i;
                report.message = "record " + std::to_string(i) + " (" + describeRecord(record) + "): " + difference;
                return report;
            }
        }
        return report;
    }

    std::vector<std::string> ListRecordings(const std::string &directory)
    {
        std::vector<std::string> out;
        std::error_code ec;
        for (const auto &entry : std::filesystem::directory_iterator(directory, ec))
        {
            if (entry.is_regular_file() && entry.path().extension() == ".gsr")
                out.push_back(entry.path().string());
        }
        std::sort(out.begin(), out.end());
        return out;
    }

    std::string DefaultDirectory()
    {
        if (const char *dir = std::getenv("PS2X_GS_GOLDEN_DIR"))
            return dir;
        return "../../Analysis/oracle/gs_golden";
    }
}
