#include "runtime/gs/gs_threaded_backend.h"
#include "runtime/gs/gs_cpu_backend.h"
#include "runtime/gs/gs_gpu_backend.h"
#include "runtime/gs/gs_pcsx2_backend.h"
#include "runtime/gs/gs_shared_present.h"
#include "ThreadPriority.h"
#include "ThreadNaming.h"

#include <atomic>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <tuple>
#include <vector>
#include <xmmintrin.h>

namespace
{
    // Asynchronous readback of tiny local->host transfers (docs/PROGRESS.md §50). Urban Reign reads an 8x8 block of the
    // depth buffer about once a frame (its light-flare visibility test, sub_003155F8). Synchronously, each one waits for
    // the GS thread to finish everything queued and for the GPU (~9.5% of the game thread in the heaviest map). For such
    // transfers the game gets the data of the previous transfer with the same buffer, format and size (one frame
    // late; the position is not part of the key since the flare moves with the camera), and the GS thread reads the
    // new block in the background. The first time, it is read synchronously as before. Other downloads stay exact.
    // PS2X_GS_ASYNC_READBACK=0 turns it off; =all also covers 8x8 color blocks.
    int asyncReadbackMode()
    {
        static const int mode = []
        {
            const char *v = std::getenv("PS2X_GS_ASYNC_READBACK");
            if (v && *v == '0')
                return 0;
            if (v && std::strcmp(v, "all") == 0)
                return 2;
            return 1;
        }();
        return mode;
    }

    bool isDepthFormat(uint8_t psm)
    {
        return psm == 0x30u || psm == 0x31u || psm == 0x32u || psm == 0x3Au;
    }

    uint32_t bitsPerPixel(uint8_t psm)
    {
        switch (psm)
        {
        case 0x00u: case 0x30u: return 32u;
        case 0x01u: case 0x31u: return 24u;
        case 0x02u: case 0x0Au: case 0x32u: case 0x3Au: return 16u;
        case 0x13u: return 8u;
        case 0x14u: return 4u;
        default: return 0u;
        }
    }

    using ReadbackKey = std::tuple<uint32_t, uint8_t, uint8_t, uint16_t, uint16_t>; // sbp, sbw, spsm, w, h

    struct AsyncReadback
    {
        std::mutex mutex;
        std::map<ReadbackKey, std::vector<uint8_t>> cache;
        // EE side: bytes of a cache hit being consumed instead of the GS thread's transfer.
        std::vector<uint8_t> serve;
        size_t servePos = 0u;
        bool serving = false;
        // EE side: a first (synchronous) transfer whose bytes go into the cache as they are consumed.
        bool capturing = false;
        ReadbackKey captureKey{};
        size_t captureExpected = 0u;
        std::vector<uint8_t> capture;
        // GS thread side: refreshes queued by cache hits.
        std::deque<std::pair<ReadbackKey, GSTransferCommand>> refresh;
        GSRasterBackend *inner = nullptr; // the backend whose GS thread runs the refreshes
        uint64_t hits = 0u, misses = 0u;
    };
    AsyncReadback g_readback;

    constexpr size_t kPublishBytes = 64u * 1024u;
    constexpr size_t kMaxPendingChunks = 256u;
    std::atomic<bool> s_enabledByDefault{false};
    std::atomic<bool> s_gpuByDefault{false};

    bool discardDraws()
    {
        static const bool discard = []
        {
            const char *value = std::getenv("PS2X_GS_DISCARD_DRAWS");
            return value && *value && *value != '0';
        }();
        return discard;
    }

    struct LoadClutArgs
    {
        GSTex0Reg tex0;
        GSTexClutReg texclut;
    };

    struct WriteVramArgs
    {
        uint32_t psm;
        uint32_t base;
        uint32_t bw;
        uint32_t x;
        uint32_t y;
        uint32_t value;
    };

    template <typename T>
    T readAt(const uint8_t *data)
    {
        T value;
        std::memcpy(&value, data, sizeof(T));
        return value;
    }
}

GSThreadedBackend::GSThreadedBackend(std::unique_ptr<GSRasterBackend> inner)
    : m_inner(std::move(inner))
{
    m_rawGif = m_inner && m_inner->WantsRawGif();
    const char *compact = std::getenv("PS2X_GS_COMPACT_QUEUE");
    m_compactQueue = !compact || *compact != '0';
    // 64 chunks (4 MB) in flight by default instead of 8: the game thread produces draws in bursts (each VU1 job's
    // XGKICK output) and waited for room ~8.6% of the time in Urban Reign's heaviest 4-fighter map while this thread
    // sat idle the rest of the frame (37.6/40.2 -> 44.6 FPS; 256 gives the same as 64).
    m_queueChunkLimit = 64u;
    if (const char *limit = std::getenv("PS2X_GS_QUEUE_CHUNKS"))
        m_queueChunkLimit = static_cast<size_t>(std::clamp(std::atoi(limit), 1, static_cast<int>(kMaxPendingChunks)));
    m_open.reserve(kPublishBytes + 4096u);
    m_worker = std::thread([this]
                           { WorkerLoop(); });
}

GSThreadedBackend::~GSThreadedBackend()
{
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_stop = true;
        m_pending.clear();
    }
    m_workCv.notify_all();
    if (m_worker.joinable())
        m_worker.join();
}

std::unique_ptr<GSRasterBackend> GSThreadedBackend::MakeDefault()
{
    const char *value = std::getenv("PS2X_GS_THREAD");
    const bool enabled = (value && *value) ? *value != '0' : s_enabledByDefault.load();
    const char *gpu = std::getenv("PS2X_GS_GPU");
    const bool useGpu = (gpu && *gpu) ? *gpu != '0' : s_gpuByDefault.load();
    if (GSPcsx2Backend::Requested())
        return std::make_unique<GSThreadedBackend>(std::make_unique<GSPcsx2Backend>());
    if (useGpu)
        return std::make_unique<GSThreadedBackend>(std::make_unique<GSGpuBackend>());
    if (!enabled)
        return std::make_unique<GSCpuBackend>();
    return std::make_unique<GSThreadedBackend>(std::make_unique<GSCpuBackend>());
}

void GSThreadedBackend::SetEnabledByDefault(bool enabled)
{
    s_enabledByDefault.store(enabled);
}

void GSThreadedBackend::SetGpuByDefault(bool enabled)
{
    s_gpuByDefault.store(enabled);
}

void GSThreadedBackend::Enqueue(Op op, const void *payload, uint32_t size)
{
    std::lock_guard<std::mutex> lock(m_producerMutex);
    const uint32_t csr = _mm_getcsr();
    if (csr != m_lastMxcsr)
    {
        m_lastMxcsr = csr;
        const uint8_t header = static_cast<uint8_t>(Op::Mxcsr);
        const uint32_t csrSize = sizeof(csr);
        m_open.push_back(header);
        m_open.insert(m_open.end(), reinterpret_cast<const uint8_t *>(&csrSize), reinterpret_cast<const uint8_t *>(&csrSize) + 4);
        m_open.insert(m_open.end(), reinterpret_cast<const uint8_t *>(&csr), reinterpret_cast<const uint8_t *>(&csr) + 4);
    }
    const auto append = [&](Op kind, const void *bytes, uint32_t length)
    {
        uint8_t header[5];
        header[0] = static_cast<uint8_t>(kind);
        std::memcpy(header + 1u, &length, sizeof(length));
        m_open.insert(m_open.end(), header, header + sizeof(header));
        if (length)
            m_open.insert(m_open.end(), static_cast<const uint8_t *>(bytes), static_cast<const uint8_t *>(bytes) + length);
    };
    if (op == Op::Submit && m_compactQueue)
    {
        // One primitive per vertex kick, so this runs ~a million times a second on the GIF thread. The records are
        // built in a fixed-size local buffer (copies of known size, inlined) and appended with one insert, instead of
        // 2-3 vector inserts of variable size, each an out-of-line memmove of a few dozen bytes (PROGRESS §70).
        const auto &draw = *static_cast<const GSPrimitiveBatch *>(payload);
        constexpr uint32_t kStateBytes = sizeof(draw.state);
        constexpr uint32_t kVertexBytes = static_cast<uint32_t>(offsetof(GSPrimitiveBatch, state));
        alignas(16) uint8_t record[2u * 5u + kStateBytes + kVertexBytes];
        size_t used = 0u;
        if (!m_drawStateValid || std::memcmp(&m_lastDrawState, &draw.state, kStateBytes) != 0)
        {
            std::memcpy(&m_lastDrawState, &draw.state, kStateBytes);
            m_drawStateValid = true;
            record[used] = static_cast<uint8_t>(Op::DrawState);
            std::memcpy(record + used + 1u, &kStateBytes, sizeof(kStateBytes));
            std::memcpy(record + used + 5u, &draw.state, kStateBytes);
            used += 5u + kStateBytes;
        }
        record[used] = static_cast<uint8_t>(Op::Vertices);
        std::memcpy(record + used + 1u, &kVertexBytes, sizeof(kVertexBytes));
        std::memcpy(record + used + 5u, &draw, kVertexBytes);
        used += 5u + kVertexBytes;
        m_open.insert(m_open.end(), record, record + used);
    }
    else
        append(op, payload, size);
    if (m_open.size() >= kPublishBytes)
        PublishLocked();
}

namespace
{
    // PS2X_GS_WAIT_STATS=1 (round 10 diagnostic): every 2 s, how long producers waited for the GS worker: in
    // PublishLocked for a free chunk slot (queue full) and in Drain (synchronous calls: readbacks, RunOnWorker).
    const bool g_gsWaitStats = [] { const char *v = std::getenv("PS2X_GS_WAIT_STATS"); return v && *v == '1'; }();
    std::atomic<uint64_t> g_publishWaits{0}, g_publishWaitNs{0}, g_publishes{0}, g_drains{0}, g_drainWaitNs{0};
    std::atomic<int64_t> g_gsWaitWindowNs{0};
    std::atomic<int64_t> g_gsWaitEpochNs{0};

    int64_t gsWaitNowNs()
    {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    void gsWaitStatsMaybePrint()
    {
        const int64_t now = gsWaitNowNs();
        int64_t start = g_gsWaitWindowNs.load(std::memory_order_relaxed);
        if (start == 0)
        {
            g_gsWaitWindowNs.compare_exchange_strong(start, now);
            int64_t zero = 0;
            g_gsWaitEpochNs.compare_exchange_strong(zero, now);
            return;
        }
        if (now - start < 2000000000 || !g_gsWaitWindowNs.compare_exchange_strong(start, now))
            return;
        std::fprintf(stderr, "[gs-wait] t=%.2f publishes=%llu queue-full waits=%llu %.1f ms, drains=%llu %.1f ms\n",
                     static_cast<double>(now - g_gsWaitEpochNs.load()) / 1e9,
                     static_cast<unsigned long long>(g_publishes.exchange(0)),
                     static_cast<unsigned long long>(g_publishWaits.exchange(0)), static_cast<double>(g_publishWaitNs.exchange(0)) / 1e6,
                     static_cast<unsigned long long>(g_drains.exchange(0)), static_cast<double>(g_drainWaitNs.exchange(0)) / 1e6);
    }
}

void GSThreadedBackend::PublishLocked()
{
    if (m_open.empty())
        return;
    std::vector<uint8_t> next;
    {
        std::unique_lock<std::mutex> lock(m_queueMutex);
        if (g_gsWaitStats)
        {
            g_publishes.fetch_add(1, std::memory_order_relaxed);
            if (!(m_stop || m_pending.size() < m_queueChunkLimit))
            {
                const int64_t t0 = gsWaitNowNs();
                m_doneCv.wait(lock, [&]
                              { return m_stop || m_pending.size() < m_queueChunkLimit; });
                g_publishWaits.fetch_add(1, std::memory_order_relaxed);
                g_publishWaitNs.fetch_add(static_cast<uint64_t>(gsWaitNowNs() - t0), std::memory_order_relaxed);
            }
        }
        m_doneCv.wait(lock, [&]
                      { return m_stop || m_pending.size() < m_queueChunkLimit; });
        m_pending.push_back(std::move(m_open));
        ++m_published;
        if (!m_spare.empty())
        {
            next = std::move(m_spare.back());
            m_spare.pop_back();
        }
    }
    m_workCv.notify_one();
    if (g_gsWaitStats)
        gsWaitStatsMaybePrint();
    next.clear();
    m_open = std::move(next);
    if (m_open.capacity() < kPublishBytes)
        m_open.reserve(kPublishBytes + 4096u);
}

void GSThreadedBackend::Drain() const
{
    auto *self = const_cast<GSThreadedBackend *>(this);
    uint64_t target = 0u;
    {
        std::lock_guard<std::mutex> lock(m_producerMutex);
        self->PublishLocked();
        std::lock_guard<std::mutex> queueLock(m_queueMutex);
        target = m_published;
    }
    std::unique_lock<std::mutex> lock(m_queueMutex);
    if (g_gsWaitStats)
    {
        const int64_t t0 = gsWaitNowNs();
        m_doneCv.wait(lock, [&]
                      { return m_completed >= target || m_stop; });
        g_drains.fetch_add(1, std::memory_order_relaxed);
        g_drainWaitNs.fetch_add(static_cast<uint64_t>(gsWaitNowNs() - t0), std::memory_order_relaxed);
        lock.unlock();
        gsWaitStatsMaybePrint();
        return;
    }
    m_doneCv.wait(lock, [&]
                  { return m_completed >= target || m_stop; });
}

void GSThreadedBackend::WorkerLoop()
{
    ThreadNaming::SetCurrentThreadName("GSThread");
    ThreadPriority::RaiseCurrentThread(ThreadPriority::Level::AboveNormal);
    GSPrimitiveBatch draw;
    for (;;)
    {
        std::vector<uint8_t> chunk;
        {
            std::unique_lock<std::mutex> lock(m_queueMutex);
            m_workCv.wait(lock, [&]
                          { return m_stop || !m_pending.empty(); });
            if (m_stop)
            {
                m_inner.reset();
                return;
            }
            chunk = std::move(m_pending.front());
            m_pending.pop_front();
        }
        {
            std::lock_guard<std::mutex> exec(m_execMutex);
            Execute(chunk, draw);
        }
        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            ++m_completed;
            if (m_spare.size() < 64u)
                m_spare.push_back(std::move(chunk));
        }
        m_doneCv.notify_all();
    }
}

void GSThreadedBackend::Execute(const std::vector<uint8_t> &chunk, GSPrimitiveBatch &draw)
{
    const uint8_t *data = chunk.data();
    const size_t size = chunk.size();
    size_t offset = 0u;
    while (offset + 5u <= size)
    {
        const Op op = static_cast<Op>(data[offset]);
        const uint32_t length = readAt<uint32_t>(data + offset + 1u);
        const uint8_t *payload = data + offset + 5u;
        offset += 5u + length;
        switch (op)
        {
        case Op::Mxcsr:
            _mm_setcsr(readAt<uint32_t>(payload));
            break;
        case Op::Submit:
            if (!discardDraws())
                m_inner->Submit(readAt<GSPrimitiveBatch>(payload));
            break;
        case Op::LoadClut:
        {
            const LoadClutArgs args = readAt<LoadClutArgs>(payload);
            m_inner->LoadClut(args.tex0, args.texclut);
            break;
        }
        case Op::BeginTransfer:
            m_inner->BeginTransfer(readAt<GSTransferCommand>(payload));
            break;
        case Op::Upload:
            m_inner->UploadImage(payload, length);
            break;
        case Op::TextureFlush:
            m_inner->TextureFlush();
            break;
        case Op::WriteVram:
        {
            const WriteVramArgs a = readAt<WriteVramArgs>(payload);
            m_inner->WriteVram(a.psm, a.base, a.bw, a.x, a.y, a.value);
            break;
        }
        case Op::Sync:
            m_inner->Sync(readAt<GSSyncReason>(payload));
            break;
        case Op::DrawState:
            std::memcpy(&draw.state, payload, sizeof(draw.state));
            break;
        case Op::Vertices:
            std::memcpy(&draw, payload, offsetof(GSPrimitiveBatch, state));
            if (!discardDraws())
                m_inner->Submit(draw);
            break;
        case Op::Present:
            m_inner->Present(readAt<GSPresentationRequest>(payload));
            break;
        case Op::Call:
            (*readAt<const std::function<void()> *>(payload))();
            break;
        case Op::RawGif:
            // PCSX2's GIF parser loads whole qwords with aligned vector loads; the queue packs records at any byte.
            if ((reinterpret_cast<uintptr_t>(payload) & 15u) != 0u)
            {
                static thread_local std::vector<__m128> aligned;
                aligned.resize((length + 15u) / 16u);
                std::memcpy(aligned.data(), payload, length);
                m_inner->RawGif(reinterpret_cast<const uint8_t *>(aligned.data()), length);
            }
            else
                m_inner->RawGif(payload, length);
            break;
        case Op::VSync:
            m_inner->VSync(readAt<GSVSyncRegs>(payload));
            break;
        case Op::GsReset:
            m_inner->GsReset();
            break;
        }
    }
}

void GSThreadedBackend::RunOnWorker(const std::function<void()> &function) const
{
    auto *self = const_cast<GSThreadedBackend *>(this);
    const std::function<void()> *pointer = &function;
    self->Enqueue(Op::Call, &pointer, sizeof(pointer));
    Drain();
}

void GSThreadedBackend::Initialize(uint8_t *vram, uint32_t vramSize)
{
    RunOnWorker([&] { m_inner->Initialize(vram, vramSize); });
}

void GSThreadedBackend::Reset()
{
    RunOnWorker([&] { m_inner->Reset(); });
}

void GSThreadedBackend::Submit(const GSPrimitiveBatch &batch)
{
    Enqueue(Op::Submit, &batch, sizeof(batch));
}

void GSThreadedBackend::LoadClut(const GSTex0Reg &tex0, const GSTexClutReg &texclut)
{
    const LoadClutArgs args{tex0, texclut};
    Enqueue(Op::LoadClut, &args, sizeof(args));
}

void GSThreadedBackend::BeginTransfer(const GSTransferCommand &command)
{
    const int mode = asyncReadbackMode();
    const GSBitBltBuf &b = command.bitbltbuf;
    const uint32_t bpp = bitsPerPixel(b.spsm);
    // "all" takes blocks up to 16x16: Urban Reign's flare color read becomes 1x16..7x16 when the sun nears the screen
    // edge, and those stayed synchronous (~3% of the game thread with 8 fighters; docs/PROGRESS.md §64).
    const uint32_t tinyLimit = mode == 2 ? 16u : 8u;
    const bool tiny = command.direction == 1u && command.trxreg.rrw != 0u && command.trxreg.rrh != 0u &&
                      command.trxreg.rrw <= tinyLimit && command.trxreg.rrh <= tinyLimit && bpp != 0u;
    if (mode != 0 && tiny && (mode == 2 || isDepthFormat(b.spsm)))
    {
        const ReadbackKey key{b.sbp, b.sbw, b.spsm, static_cast<uint16_t>(command.trxreg.rrw), static_cast<uint16_t>(command.trxreg.rrh)};
        bool hit = false;
        {
            std::lock_guard<std::mutex> lock(g_readback.mutex);
            const auto it = g_readback.cache.find(key);
            hit = it != g_readback.cache.end();
            if (hit)
            {
                g_readback.serve = it->second;
                g_readback.servePos = 0u;
                g_readback.serving = true;
                g_readback.capturing = false;
                g_readback.refresh.emplace_back(key, command);
                g_readback.inner = m_inner.get();
                ++g_readback.hits;
            }
            else
            {
                g_readback.serving = false;
                g_readback.capturing = true;
                g_readback.captureKey = key;
                g_readback.captureExpected = static_cast<size_t>(command.trxreg.rrw) * command.trxreg.rrh * bpp / 8u;
                g_readback.capture.clear();
                ++g_readback.misses;
            }
        }
        if (hit)
        {
            // The GS thread reads the new block when it gets there, in order with the draws before it.
            static const std::function<void()> refresher = []
            {
                for (;;)
                {
                    std::pair<ReadbackKey, GSTransferCommand> job;
                    GSRasterBackend *inner = nullptr;
                    {
                        std::lock_guard<std::mutex> lock(g_readback.mutex);
                        if (g_readback.refresh.empty() || !g_readback.inner)
                            return;
                        job = g_readback.refresh.front();
                        g_readback.refresh.pop_front();
                        inner = g_readback.inner;
                    }
                    // On the GPU back end the read itself does not wait either (a synchronous one waited for
                    // everything queued before it: ~half the GS thread at 2x internal resolution with 8 fighters).
                    if (auto *gpu = dynamic_cast<GSGpuBackend *>(inner))
                    {
                        const ReadbackKey key = job.first;
                        if (gpu->BeginDownloadAsync(job.second, [key](std::vector<uint8_t> &&bytes)
                                                    {
                                                        std::lock_guard<std::mutex> lock(g_readback.mutex);
                                                        g_readback.cache[key] = std::move(bytes);
                                                    }))
                            continue;
                    }
                    // PCSX2's GS: the stream's own transfer is the game's (TRXDIR=1 went through the GIF); the
                    // refresh reads the area beside it (PROGRESS §83).
                    if (auto *pcsx2 = dynamic_cast<GSPcsx2Backend *>(inner))
                    {
                        std::vector<uint8_t> bytes;
                        if (pcsx2->ReadTransfer(job.second, bytes) && !bytes.empty())
                        {
                            std::lock_guard<std::mutex> lock(g_readback.mutex);
                            g_readback.cache[job.first] = std::move(bytes);
                        }
                        continue;
                    }
                    inner->BeginTransfer(job.second);
                    std::vector<uint8_t> bytes(static_cast<size_t>(job.second.trxreg.rrw) * job.second.trxreg.rrh *
                                               bitsPerPixel(job.second.bitbltbuf.spsm) / 8u);
                    const uint32_t got = inner->ConsumeLocalToHostBytes(bytes.data(), static_cast<uint32_t>(bytes.size()));
                    if (got == bytes.size())
                    {
                        std::lock_guard<std::mutex> lock(g_readback.mutex);
                        g_readback.cache[job.first] = std::move(bytes);
                    }
                }
            };
            const std::function<void()> *pointer = &refresher;
            Enqueue(Op::Call, &pointer, sizeof(pointer));
            return;
        }
    }
    else if (command.direction == 1u || command.direction == 2u)
    {
        std::lock_guard<std::mutex> lock(g_readback.mutex);
        g_readback.serving = false;
        g_readback.capturing = false;
    }
    Enqueue(Op::BeginTransfer, &command, sizeof(command));
}

void GSThreadedBackend::RawGif(const uint8_t *data, uint32_t sizeBytes)
{
    if (data && sizeBytes >= 16u)
        Enqueue(Op::RawGif, data, sizeBytes & ~15u);
}

// The field ends: published at once, so the GS thread presents it without waiting for more GIF data.
void GSThreadedBackend::VSync(const GSVSyncRegs &regs)
{
    Enqueue(Op::VSync, &regs, sizeof(regs));
    std::lock_guard<std::mutex> lock(m_producerMutex);
    PublishLocked();
}

void GSThreadedBackend::GsReset()
{
    Enqueue(Op::GsReset, nullptr, 0u);
}

void GSThreadedBackend::UploadImage(const uint8_t *data, uint32_t sizeBytes)
{
    Enqueue(Op::Upload, data, data ? sizeBytes : 0u);
}

void GSThreadedBackend::Flush()
{
}

void GSThreadedBackend::TextureFlush()
{
    Enqueue(Op::TextureFlush, nullptr, 0u);
}

void GSThreadedBackend::Sync(GSSyncReason reason)
{
    if (reason == GSSyncReason::Presentation && GSSharedPresent::Active())
        return;
    if (reason == GSSyncReason::Finish)
    {
        Enqueue(Op::Sync, &reason, sizeof(reason));
        return;
    }
    RunOnWorker([&] { m_inner->Sync(reason); });
}

PresentationFrame GSThreadedBackend::Present(const GSPresentationRequest &request)
{
    if (GSSharedPresent::Active())
    {
        Enqueue(Op::Present, &request, sizeof(request));
        std::lock_guard<std::mutex> lock(m_producerMutex);
        PublishLocked();
        return {};
    }
    PresentationFrame frame;
    RunOnWorker([&] { frame = m_inner->Present(request); });
    return frame;
}

bool GSThreadedBackend::ClearFramebuffer(const GSContext &context, uint32_t rgba)
{
    bool result = false;
    RunOnWorker([&] { result = m_inner->ClearFramebuffer(context, rgba); });
    return result;
}

// Save state loaded: a readback being served or captured belongs to the session before it (the cache stays).
void ps2xGsResetAsyncReadback()
{
    std::lock_guard<std::mutex> lock(g_readback.mutex);
    g_readback.serving = false;
    g_readback.capturing = false;
    g_readback.refresh.clear();
}

uint32_t GSThreadedBackend::ConsumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes)
{
    {
        std::lock_guard<std::mutex> lock(g_readback.mutex);
        if (g_readback.serving)
        {
            const size_t count = std::min<size_t>(maxBytes, g_readback.serve.size() - g_readback.servePos);
            if (dst && count)
                std::memcpy(dst, g_readback.serve.data() + g_readback.servePos, count);
            g_readback.servePos += count;
            if (g_readback.servePos >= g_readback.serve.size())
                g_readback.serving = false;
            return static_cast<uint32_t>(count);
        }
    }
    uint32_t result = 0u;
    RunOnWorker([&] { result = m_inner->ConsumeLocalToHostBytes(dst, maxBytes); });
    std::lock_guard<std::mutex> lock(g_readback.mutex);
    if (g_readback.capturing && dst && result)
    {
        g_readback.capture.insert(g_readback.capture.end(), dst, dst + result);
        if (g_readback.capture.size() >= g_readback.captureExpected)
        {
            g_readback.capture.resize(g_readback.captureExpected);
            g_readback.cache[g_readback.captureKey] = g_readback.capture;
            g_readback.capturing = false;
        }
    }
    return result;
}

uint32_t GSThreadedBackend::ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const
{
    uint32_t result = 0u;
    RunOnWorker([&] { result = m_inner->ReadVram(psm, base, bw, x, y); });
    return result;
}

void GSThreadedBackend::WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value)
{
    const WriteVramArgs args{psm, base, bw, x, y, value};
    Enqueue(Op::WriteVram, &args, sizeof(args));
}

void GSThreadedBackend::SnapshotVram(std::vector<uint8_t> &out) const
{
    RunOnWorker([&] { m_inner->SnapshotVram(out); });
}

GSTransferSnapshot GSThreadedBackend::GetTransferSnapshot() const
{
    GSTransferSnapshot result;
    RunOnWorker([&] { result = m_inner->GetTransferSnapshot(); });
    return result;
}

bool GSThreadedBackend::ExportState(GSBackendState &out)
{
    bool result = false;
    RunOnWorker([&]
                {
        auto *access = dynamic_cast<GSBackendStateAccess *>(m_inner.get());
        result = access && access->ExportState(out); });
    return result;
}

bool GSThreadedBackend::ImportState(const GSBackendState &state)
{
    bool result = false;
    RunOnWorker([&]
                {
        auto *access = dynamic_cast<GSBackendStateAccess *>(m_inner.get());
        result = access && access->ImportState(state); });
    return result;
}
