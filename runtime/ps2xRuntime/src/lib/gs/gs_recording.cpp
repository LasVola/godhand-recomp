#include "runtime/gs/gs_recording.h"
#include "runtime/ps2_memory.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <type_traits>
#include <xmmintrin.h>

namespace
{
    constexpr char kMagic[8] = {'P', 'S', '2', 'X', 'G', 'S', 'R', '1'};

    static_assert(std::is_trivially_copyable_v<GSPrimitiveBatch>);
    static_assert(std::is_trivially_copyable_v<GSTex0Reg>);
    static_assert(std::is_trivially_copyable_v<GSTexClutReg>);
    static_assert(std::is_trivially_copyable_v<GSTransferCommand>);
    static_assert(std::is_trivially_copyable_v<GSTransferSnapshot>);
    static_assert(std::is_trivially_copyable_v<GSContext>);

    template <typename T>
    void appendPod(std::vector<uint8_t> &out, const T &value)
    {
        const auto *bytes = reinterpret_cast<const uint8_t *>(&value);
        out.insert(out.end(), bytes, bytes + sizeof(T));
    }

    template <typename T>
    bool readPod(const uint8_t *data, size_t size, size_t &offset, T &out)
    {
        if (offset + sizeof(T) > size)
            return false;
        std::memcpy(&out, data + offset, sizeof(T));
        offset += sizeof(T);
        return true;
    }

    struct WriteVramArgs
    {
        uint32_t psm;
        uint32_t base;
        uint32_t bw;
        uint32_t x;
        uint32_t y;
        uint32_t value;
    };

    struct ClearArgs
    {
        GSContext context;
        uint32_t rgba;
        uint32_t result;
    };

    struct LoadClutArgs
    {
        GSTex0Reg tex0;
        GSTexClutReg texclut;
    };

    std::string describeMismatch(const char *what, uint32_t expected, uint32_t actual)
    {
        char text[160];
        std::snprintf(text, sizeof(text), "%s: expected 0x%08X, got 0x%08X", what, expected, actual);
        return text;
    }
}

namespace GSRecording
{
    uint64_t HashVram(const uint8_t *vram, size_t size)
    {
        uint64_t lanes[4] = {0x9E3779B97F4A7C15ull, 0xC2B2AE3D27D4EB4Full, 0x165667B19E3779F9ull, 0x27D4EB2F165667C5ull};
        const size_t words = size / 8u;
        for (size_t i = 0; i + 4u <= words; i += 4u)
        {
            for (size_t lane = 0; lane < 4u; ++lane)
            {
                uint64_t word;
                std::memcpy(&word, vram + (i + lane) * 8u, 8u);
                lanes[lane] = (lanes[lane] ^ word) * 0x100000001B3ull;
                lanes[lane] ^= lanes[lane] >> 29u;
            }
        }
        return lanes[0] ^ (lanes[1] * 3u) ^ (lanes[2] * 5u) ^ (lanes[3] * 7u);
    }

    void AppendState(std::vector<uint8_t> &out, const GSBackendState &state)
    {
        appendPod(out, state.clut);
        appendPod(out, state.clutCbp);
        appendPod(out, state.cachePageBase);
        appendPod(out, state.cacheBytes);
        appendPod(out, state.transfer);
        appendPod(out, state.transferState);
        const uint64_t l2hSize = state.localToHost.size();
        appendPod(out, l2hSize);
        out.insert(out.end(), state.localToHost.begin(), state.localToHost.end());
        appendPod(out, state.localToHostReadPos);
    }

    bool ParseState(const uint8_t *data, size_t size, size_t &offset, GSBackendState &out)
    {
        uint64_t l2hSize = 0u;
        if (!readPod(data, size, offset, out.clut) ||
            !readPod(data, size, offset, out.clutCbp) ||
            !readPod(data, size, offset, out.cachePageBase) ||
            !readPod(data, size, offset, out.cacheBytes) ||
            !readPod(data, size, offset, out.transfer) ||
            !readPod(data, size, offset, out.transferState) ||
            !readPod(data, size, offset, l2hSize))
            return false;
        if (offset + l2hSize > size)
            return false;
        out.localToHost.assign(data + offset, data + offset + l2hSize);
        offset += static_cast<size_t>(l2hSize);
        return readPod(data, size, offset, out.localToHostReadPos);
    }

    bool ParseCheckpoint(const Record &record, Checkpoint &out)
    {
        const uint8_t *data = record.payload.data();
        const size_t size = record.payload.size();
        size_t offset = 0u;
        uint32_t vramSize = 0u;
        if (!readPod(data, size, offset, out.tick) || !readPod(data, size, offset, out.display) ||
            !readPod(data, size, offset, vramSize))
            return false;
        if (offset + vramSize > size)
            return false;
        out.vram.assign(data + offset, data + offset + vramSize);
        offset += vramSize;
        return ParseState(data, size, offset, out.state) && offset == size;
    }

    bool Reader::Open(const std::string &path)
    {
        m_file.reset(std::fopen(path.c_str(), "rb"));
        if (!m_file)
        {
            m_error = "cannot open " + path;
            return false;
        }
        std::setvbuf(m_file.get(), nullptr, _IOFBF, 1u << 20u);
        char magic[8]{};
        if (std::fread(magic, 1, sizeof(magic), m_file.get()) != sizeof(magic) ||
            std::memcmp(magic, kMagic, sizeof(magic)) != 0)
        {
            m_error = "bad magic in " + path;
            m_file.reset();
            return false;
        }
        return true;
    }

    bool Reader::Next(Record &out)
    {
        if (!m_file)
            return false;
        uint8_t op = 0u;
        uint32_t size = 0u;
        if (std::fread(&op, 1, 1, m_file.get()) != 1u)
            return false;
        if (std::fread(&size, sizeof(size), 1, m_file.get()) != 1u)
        {
            m_error = "truncated record header";
            return false;
        }
        out.op = static_cast<Op>(op);
        out.payload.resize(size);
        if (size != 0u && std::fread(out.payload.data(), 1, size, m_file.get()) != size)
        {
            m_error = "truncated record payload";
            return false;
        }
        return true;
    }

    ApplyResult Apply(GSRasterBackend &backend, const Record &record)
    {
        const uint8_t *data = record.payload.data();
        const size_t size = record.payload.size();
        size_t offset = 0u;
        ApplyResult result;
        auto fail = [&](std::string message)
        {
            result.ok = false;
            result.message = std::move(message);
            return result;
        };

        switch (record.op)
        {
        case Op::Submit:
        {
            GSPrimitiveBatch batch{};
            if (!readPod(data, size, offset, batch))
                return fail("short submit record");
            backend.Submit(batch);
            break;
        }
        case Op::LoadClut:
        {
            LoadClutArgs args{};
            if (!readPod(data, size, offset, args))
                return fail("short clut record");
            backend.LoadClut(args.tex0, args.texclut);
            break;
        }
        case Op::BeginTransfer:
        {
            GSTransferCommand command{};
            if (!readPod(data, size, offset, command))
                return fail("short transfer record");
            backend.BeginTransfer(command);
            break;
        }
        case Op::Upload:
            backend.UploadImage(data, static_cast<uint32_t>(size));
            break;
        case Op::TextureFlush:
            backend.TextureFlush();
            break;
        case Op::Clear:
        {
            ClearArgs args{};
            if (!readPod(data, size, offset, args))
                return fail("short clear record");
            const uint32_t actual = backend.ClearFramebuffer(args.context, args.rgba) ? 1u : 0u;
            if (actual != args.result)
                return fail(describeMismatch("ClearFramebuffer result", args.result, actual));
            break;
        }
        case Op::WriteVram:
        {
            WriteVramArgs args{};
            if (!readPod(data, size, offset, args))
                return fail("short write record");
            backend.WriteVram(args.psm, args.base, args.bw, args.x, args.y, args.value);
            break;
        }
        case Op::ReadVram:
        {
            WriteVramArgs args{};
            if (!readPod(data, size, offset, args))
                return fail("short read record");
            const uint32_t actual = backend.ReadVram(args.psm, args.base, args.bw, args.x, args.y);
            if (actual != args.value)
                return fail(describeMismatch("ReadVram", args.value, actual));
            break;
        }
        case Op::Consume:
        {
            uint32_t maxBytes = 0u;
            if (!readPod(data, size, offset, maxBytes))
                return fail("short consume record");
            std::vector<uint8_t> bytes(maxBytes);
            const uint32_t count = backend.ConsumeLocalToHostBytes(bytes.data(), maxBytes);
            const size_t expected = size - offset;
            if (count != expected || std::memcmp(bytes.data(), data + offset, count) != 0)
                return fail("local-to-host bytes differ");
            break;
        }
        case Op::Reset:
            backend.Reset();
            break;
        case Op::State:
        case Op::Field:
        case Op::End:
        case Op::Hash:
            break;
        case Op::Mxcsr:
        {
            uint32_t csr = 0u;
            if (!readPod(data, size, offset, csr))
                return fail("short mxcsr record");
            _mm_setcsr(csr);
            break;
        }
        default:
            return fail("unknown record");
        }
        return result;
    }
}

GSRecordingBackend::GSRecordingBackend(std::unique_ptr<GSRasterBackend> inner,
                                       const GSRegisters *regs,
                                       uint64_t firstTick,
                                       uint64_t tickCount,
                                       const std::string &path)
    : m_inner(std::move(inner)), m_regs(regs), m_firstTick(firstTick), m_endTick(firstTick + tickCount)
{
    const char *hashEvery = std::getenv("PS2X_GS_RECORD_HASH");
    m_hashEveryCommand = hashEvery && *hashEvery && *hashEvery != '0';
    if (const char *extra = std::getenv("PS2X_GS_RECORD_CHECKPOINTS"))
    {
        const char *cursor = extra;
        while (*cursor)
        {
            char *end = nullptr;
            const unsigned long long index = std::strtoull(cursor, &end, 10);
            if (end == cursor)
                break;
            m_extraCheckpoints.push_back(index);
            cursor = (*end == ',') ? end + 1 : end;
        }
    }
    m_file = std::fopen(path.c_str(), "wb");
    if (m_file)
    {
        std::setvbuf(m_file, nullptr, _IOFBF, 4u << 20u);
        std::fwrite(kMagic, 1, sizeof(kMagic), m_file);
    }
    else
    {
        m_finished = true;
    }
}

GSRecordingBackend::~GSRecordingBackend()
{
    if (m_file)
        std::fclose(m_file);
}

std::unique_ptr<GSRasterBackend> GSRecordingBackend::WrapFromEnvironment(std::unique_ptr<GSRasterBackend> inner,
                                                                         const GSRegisters *regs)
{
    const char *value = std::getenv("PS2X_GS_RECORD");
    if (!value || !regs)
        return inner;
    const std::string spec(value);
    size_t start = 0u;
    while (start < spec.size())
    {
        const size_t end = std::min(spec.find(';', start), spec.size());
        const std::string window = spec.substr(start, end - start);
        start = end + 1u;
        unsigned long long first = 0u;
        unsigned long long count = 0u;
        char path[512]{};
        if (std::sscanf(window.c_str(), "%llu:%llu:%511[^\n]", &first, &count, path) == 3)
            inner = std::make_unique<GSRecordingBackend>(std::move(inner), regs, first, count, path);
    }
    return inner;
}

bool GSRecordingBackend::Recording()
{
    if (m_finished || !m_regs)
        return false;
    const uint64_t tick = m_regs->vsyncTick.load(std::memory_order_acquire);
    if (!m_started)
    {
        if (tick < m_firstTick)
            return false;
        m_started = true;
        m_lastTick = tick;
        WriteCheckpoint(GSRecording::Op::State, tick);
    }
    if (tick != m_lastTick)
    {
        m_lastTick = tick;
        if (tick >= m_endTick)
        {
            WriteCheckpoint(GSRecording::Op::End, tick);
            std::fclose(m_file);
            m_file = nullptr;
            m_finished = true;
            return false;
        }
        WriteCheckpoint(GSRecording::Op::Field, tick);
    }
    const uint32_t csr = _mm_getcsr();
    if (csr != m_lastMxcsr)
    {
        m_lastMxcsr = csr;
        WriteRecord(GSRecording::Op::Mxcsr, &csr, sizeof(csr));
    }
    return true;
}

void GSRecordingBackend::WriteRecord(GSRecording::Op op, const void *payload, size_t size)
{
    if (!m_file)
        return;
    const uint8_t opByte = static_cast<uint8_t>(op);
    const uint32_t size32 = static_cast<uint32_t>(size);
    std::fwrite(&opByte, 1, 1, m_file);
    std::fwrite(&size32, sizeof(size32), 1, m_file);
    if (size != 0u)
        std::fwrite(payload, 1, size, m_file);
}

void GSRecordingBackend::WriteRecord(GSRecording::Op op, const std::vector<uint8_t> &payload)
{
    WriteRecord(op, payload.data(), payload.size());
}

void GSRecordingBackend::WriteCheckpoint(GSRecording::Op op, uint64_t tick)
{
    m_inner->Flush();
    m_inner->Sync(GSSyncReason::DebugReadback);
    std::vector<uint8_t> vram;
    m_inner->SnapshotVram(vram);
    GSBackendState state;
    if (auto *access = dynamic_cast<GSBackendStateAccess *>(m_inner.get()))
        access->ExportState(state);
    m_scratch.clear();
    appendPod(m_scratch, tick);
    const std::array<uint64_t, 7> display{m_regs->pmode, m_regs->smode2, m_regs->dispfb1, m_regs->display1,
                                          m_regs->dispfb2, m_regs->display2, m_regs->bgcolor};
    appendPod(m_scratch, display);
    appendPod(m_scratch, static_cast<uint32_t>(vram.size()));
    m_scratch.insert(m_scratch.end(), vram.begin(), vram.end());
    GSRecording::AppendState(m_scratch, state);
    WriteRecord(op, m_scratch);
    std::fflush(m_file);
}

void GSRecordingBackend::WriteHash()
{
    const uint64_t index = m_commandIndex++;
    if (!m_file)
        return;
    if (std::find(m_extraCheckpoints.begin(), m_extraCheckpoints.end(), index) != m_extraCheckpoints.end())
        WriteCheckpoint(GSRecording::Op::Field, m_lastTick);
    if (!m_hashEveryCommand || !m_vram)
        return;
    m_inner->Flush();
    m_inner->Sync(GSSyncReason::DebugReadback);
    const uint64_t hash = GSRecording::HashVram(m_vram, m_vramSize);
    WriteRecord(GSRecording::Op::Hash, &hash, sizeof(hash));
}

void GSRecordingBackend::Initialize(uint8_t *vram, uint32_t vramSize)
{
    m_vram = vram;
    m_vramSize = vramSize;
    m_inner->Initialize(vram, vramSize);
}

void GSRecordingBackend::Reset()
{
    if (Recording())
        WriteRecord(GSRecording::Op::Reset, nullptr, 0u);
    m_inner->Reset();
    if (m_started && !m_finished)
        WriteHash();
}

void GSRecordingBackend::Submit(const GSPrimitiveBatch &batch)
{
    if (Recording())
        WriteRecord(GSRecording::Op::Submit, &batch, sizeof(batch));
    m_inner->Submit(batch);
    if (m_started && !m_finished)
        WriteHash();
}

void GSRecordingBackend::LoadClut(const GSTex0Reg &tex0, const GSTexClutReg &texclut)
{
    if (Recording())
    {
        const LoadClutArgs args{tex0, texclut};
        WriteRecord(GSRecording::Op::LoadClut, &args, sizeof(args));
    }
    m_inner->LoadClut(tex0, texclut);
    if (m_started && !m_finished)
        WriteHash();
}

void GSRecordingBackend::BeginTransfer(const GSTransferCommand &command)
{
    if (Recording())
        WriteRecord(GSRecording::Op::BeginTransfer, &command, sizeof(command));
    m_inner->BeginTransfer(command);
    if (m_started && !m_finished)
        WriteHash();
}

void GSRecordingBackend::UploadImage(const uint8_t *data, uint32_t sizeBytes)
{
    if (Recording())
        WriteRecord(GSRecording::Op::Upload, data, sizeBytes);
    m_inner->UploadImage(data, sizeBytes);
    if (m_started && !m_finished)
        WriteHash();
}

void GSRecordingBackend::Flush()
{
    m_inner->Flush();
}

void GSRecordingBackend::TextureFlush()
{
    if (Recording())
        WriteRecord(GSRecording::Op::TextureFlush, nullptr, 0u);
    m_inner->TextureFlush();
    if (m_started && !m_finished)
        WriteHash();
}

void GSRecordingBackend::Sync(GSSyncReason reason)
{
    m_inner->Sync(reason);
}

PresentationFrame GSRecordingBackend::Present(const GSPresentationRequest &request)
{
    return m_inner->Present(request);
}

bool GSRecordingBackend::ClearFramebuffer(const GSContext &context, uint32_t rgba)
{
    const bool recording = Recording();
    const bool result = m_inner->ClearFramebuffer(context, rgba);
    if (recording)
    {
        const ClearArgs args{context, rgba, result ? 1u : 0u};
        WriteRecord(GSRecording::Op::Clear, &args, sizeof(args));
        WriteHash();
    }
    return result;
}

uint32_t GSRecordingBackend::ConsumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes)
{
    const bool recording = Recording();
    const uint32_t count = m_inner->ConsumeLocalToHostBytes(dst, maxBytes);
    if (recording)
    {
        m_scratch.clear();
        appendPod(m_scratch, maxBytes);
        if (dst)
            m_scratch.insert(m_scratch.end(), dst, dst + count);
        WriteRecord(GSRecording::Op::Consume, m_scratch);
    }
    return count;
}

uint32_t GSRecordingBackend::ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const
{
    auto *self = const_cast<GSRecordingBackend *>(this);
    const bool recording = self->Recording();
    const uint32_t value = m_inner->ReadVram(psm, base, bw, x, y);
    if (recording)
    {
        const WriteVramArgs args{psm, base, bw, x, y, value};
        self->WriteRecord(GSRecording::Op::ReadVram, &args, sizeof(args));
    }
    return value;
}

void GSRecordingBackend::WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value)
{
    if (Recording())
    {
        const WriteVramArgs args{psm, base, bw, x, y, value};
        WriteRecord(GSRecording::Op::WriteVram, &args, sizeof(args));
    }
    m_inner->WriteVram(psm, base, bw, x, y, value);
    if (m_started && !m_finished)
        WriteHash();
}

void GSRecordingBackend::SnapshotVram(std::vector<uint8_t> &out) const
{
    m_inner->SnapshotVram(out);
}

GSTransferSnapshot GSRecordingBackend::GetTransferSnapshot() const
{
    return m_inner->GetTransferSnapshot();
}

bool GSRecordingBackend::ExportState(GSBackendState &out)
{
    auto *access = dynamic_cast<GSBackendStateAccess *>(m_inner.get());
    return access && access->ExportState(out);
}

bool GSRecordingBackend::ImportState(const GSBackendState &state)
{
    auto *access = dynamic_cast<GSBackendStateAccess *>(m_inner.get());
    return access && access->ImportState(state);
}
