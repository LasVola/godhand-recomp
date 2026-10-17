#include "runtime/gs/gs_frontend.h"
#include "runtime/gs/gs_cpu_backend.h"
#include "runtime/gs/gs_recording.h"
#include "runtime/gs/gs_threaded_backend.h"
#include "ps2_log.h"
#include "runtime/ps2_memory.h"
#include "runtime/ps2_async_vif.h"
#include <atomic>
#include <vector>
#include <mutex>
#include <deque>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>

// GS SIGNAL stall. On hardware, a SIGNAL that arrives while CSR.SIGNAL is still set stops the GS: nothing after it in
// the GIF stream is processed until the EE clears the bit. Urban Reign relies on it: its draw list continues past
// SIGNALs whose interrupt handler kicks the next chunks, and the final FB0 -> display copy must run after them.
// When a PACKED A+D SIGNAL meets a pending CSR.SIGNAL, the rest of the packet (starting at that SIGNAL) and every packet
// submitted afterwards are deferred; ps2xGsReleaseStall() replays them in order once the EE clears the bit.
// PS2X_GS_SIGNAL_STALL=0 disables it. The state is file-local so gs_frontend.h (in the generated-code PCH) is unchanged.
uint32_t g_gsTraceSrc = 0xFFFFFFFFu; // guest address of the submitted GIF packet (copy for path 3)
thread_local int g_gsTracePath = -1; // GIF path being processed (set by GifArbiter), shown in PS2X_GS_TRACE draws
// Incremented on every local->host (TRXDIR=1) transfer; ps2_memory.cpp only touches the GS for VIF1 FIFO reads
// while a download is pending, so VIF1_STAT polls stay cheap.
std::atomic<uint32_t> g_gsDownloadSerial{0u};

namespace
{
    struct GsSignalStall
    {
        bool active = false;
        std::deque<std::vector<uint8_t>> queue;
        GS *owner = nullptr;
        GSRegisters *regs = nullptr; // owner's privileged registers (CSR), for ps2xGsStallNeedsRelease
    };
    GsSignalStall g_stall;
    std::mutex g_stallMutex;
    thread_local bool t_stallReplaying = false;
    thread_local bool t_stalledAgain = false;

    bool gsSignalStallEnabled()
    {
        static const bool enabled = [] { const char *v = std::getenv("PS2X_GS_SIGNAL_STALL"); return !v || *v != '0'; }();
        return enabled;
    }

    void appendGifTag(std::vector<uint8_t> &out, uint32_t nloop, bool eop, uint8_t flg, uint32_t nreg, uint64_t regs)
    {
        const uint64_t lo = static_cast<uint64_t>(nloop & 0x7FFFu) | (static_cast<uint64_t>(eop ? 1u : 0u) << 15) |
                            (static_cast<uint64_t>(flg & 3u) << 58) | (static_cast<uint64_t>(nreg & 0xFu) << 60);
        uint8_t bytes[16];
        std::memcpy(bytes, &lo, 8);
        std::memcpy(bytes + 8, &regs, 8);
        out.insert(out.end(), bytes, bytes + 16);
    }
}

void ps2xGsReleaseStall();

// GIF tag continuation. The GIF keeps a tag's remaining NLOOP across DMA transfers: games build long REGLIST/PACKED
// packets in a scratchpad double buffer and send them in chunks where only the first chunk carries the GIFtag
// (Urban Reign's FB -> display copy). When a packet ends mid-tag, the rest of the tag is resumed with the next packet.
// PS2X_GIF_CONTINUATION=0 restores the old behaviour (the tail of the tag is dropped).
namespace
{
    struct GifTagContinuation
    {
        bool active = false;
        uint64_t tagLo = 0u;
        uint64_t tagHi = 0u;
        uint32_t loopsLeft = 0u; // loops not finished yet, including the partial one
        uint32_t regIndex = 0u;  // first register of the partial loop
        bool padOdd = false;     // REGLIST: the original tag had an odd register count (8-byte pad at its end)
    };
    GifTagContinuation g_gifCont;

    bool gifContinuationEnabled()
    {
        static const bool enabled = [] { const char *v = std::getenv("PS2X_GIF_CONTINUATION"); return !v || *v != '0'; }();
        return enabled;
    }

    void saveGifContinuation(uint64_t tagLo, uint64_t tagHi, uint32_t loopsLeft, uint32_t regIndex, bool padOdd)
    {
        if (!gifContinuationEnabled() || loopsLeft == 0u)
            return;
        g_gifCont = {true, tagLo, tagHi, loopsLeft, regIndex, padOdd};
    }
}

// Read by the VIF1 DIRECT handler (ps2_vif1_interpreter.cpp): with the continuation on, the GIF itself resumes an
// IMAGE tag cut at the end of a DIRECT, so VIF1 must not also wrap the next DIRECT in a synthesized IMAGE tag.
bool ps2xGsGifContinuationEnabled()
{
    return gifContinuationEnabled();
}

// Defined in ps2_memory.cpp: SIGNAL delivery with stall (queue) semantics.
void ps2xGsSignal(GSRegisters &gs, uint32_t id, uint32_t mask);
bool ps2xGsSignalBlocked(const GSRegisters &gs);
void ps2xGsFinish(GSRegisters &gs);

namespace
{
    // PS2X_GS_SIGNAL_LOG=1: log SIGNAL/FINISH/LABEL register writes with the CSR state they found (first 400).
    bool gsSignalTraceAll()
    {
        static const bool enabled = [] { const char *v = std::getenv("PS2X_GS_SIGNAL_LOG"); return v && *v == '2'; }();
        return enabled;
    }

    bool gsSignalLogEnabled()
    {
        static const bool enabled = [] { const char *v = std::getenv("PS2X_GS_SIGNAL_LOG"); return v && *v == '1'; }();
        return enabled;
    }

    void gsSignalLog(const char *reg, uint64_t value, uint64_t csrBefore)
    {
        static std::atomic<int> count{0};
        if (count.fetch_add(1) < 400)
            std::fprintf(stderr, "[gs-signal] %s value=0x%016llx csr_before=0x%llx\n", reg, static_cast<unsigned long long>(value),
                         static_cast<unsigned long long>(csrBefore));
    }
}


namespace
{
    struct GsTraceConfig
    {
        uint64_t firstTick = 0;
        uint64_t endTick = 0;
        uint64_t lastTick = UINT64_MAX;
        std::FILE *file = nullptr;
        std::string vramPath;
    };

    GsTraceConfig &gsTrace()
    {
        static GsTraceConfig config = []
        {
            GsTraceConfig result;
            const char *value = std::getenv("PS2X_GS_TRACE");
            unsigned long long first = 0;
            unsigned long long count = 0;
            char path[512]{};
            if (value && std::sscanf(value, "%llu:%llu:%511[^\n]", &first, &count, path) == 3)
            {
                result.firstTick = first;
                result.endTick = first + count;
                result.file = std::fopen(path, "w");
                result.vramPath = std::string(path) + ".vram";
            }
            return result;
        }();
        return config;
    }

    std::FILE *gsTraceFile(const GSRegisters *regs, const uint8_t *vram = nullptr, uint32_t vramSize = 0u)
    {
        GsTraceConfig &config = gsTrace();
        if (!config.file || !regs)
            return nullptr;
        const uint64_t tick = regs->vsyncTick.load(std::memory_order_acquire);
        if (tick < config.firstTick)
            return nullptr;
        if (tick >= config.endTick)
        {
            std::fclose(config.file);
            config.file = nullptr;
            return nullptr;
        }
        if (tick != config.lastTick)
        {
            if (config.lastTick == UINT64_MAX && vram && vramSize)
            {
                if (std::FILE *dump = std::fopen(config.vramPath.c_str(), "wb"))
                {
                    std::fwrite(vram, 1, vramSize, dump);
                    std::fclose(dump);
                }
            }
            config.lastTick = tick;
            std::fprintf(config.file,
                         "V tick=%llu pmode=%llx smode2=%llx dispfb1=%llx display1=%llx dispfb2=%llx display2=%llx bgcolor=%llx\n",
                         static_cast<unsigned long long>(tick),
                         static_cast<unsigned long long>(regs->pmode),
                         static_cast<unsigned long long>(regs->smode2),
                         static_cast<unsigned long long>(regs->dispfb1),
                         static_cast<unsigned long long>(regs->display1),
                         static_cast<unsigned long long>(regs->dispfb2),
                         static_cast<unsigned long long>(regs->display2),
                         static_cast<unsigned long long>(regs->bgcolor));
        }
        return config.file;
    }

    void traceDraw(std::FILE *file, const GSPrimitiveBatch &batch)
    {
        std::fprintf(file, "P%d src=%08x ", g_gsTracePath, g_gsTraceSrc);
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
            std::fprintf(file, " | %.2f,%.2f z=%.0f rgba=%02x%02x%02x%02x uv=%.2f,%.2f st=%g,%g q=%g",
                         v.x, v.y, v.z, v.r, v.g, v.b, v.a, v.u / 16.0, v.v / 16.0, v.s, v.t, v.q);
        }
        std::fputc('\n', file);
    }

    void traceTransfer(std::FILE *file, const GSBitBltBuf &b, const GSTrxPos &pos, const GSTrxReg &reg, uint32_t dir)
    {
        std::fprintf(file, "T dir=%u src=%x/%u/%02x dst=%x/%u/%02x ss=%u,%u ds=%u,%u size=%ux%u\n",
                     dir, b.sbp, b.sbw, b.spsm, b.dbp, b.dbw, b.dpsm, pos.ssax, pos.ssay, pos.dsax, pos.dsay, reg.rrw, reg.rrh);
    }
}

namespace
{
    GSPrimReg decodePrimRegister(uint64_t value)
    {
        GSPrimReg prim{};
        prim.type = static_cast<GSPrimType>(value & 0x7u);
        prim.iip = ((value >> 3) & 1u) != 0u;
        prim.tme = ((value >> 4) & 1u) != 0u;
        prim.fge = ((value >> 5) & 1u) != 0u;
        prim.abe = ((value >> 6) & 1u) != 0u;
        prim.aa1 = ((value >> 7) & 1u) != 0u;
        prim.fst = ((value >> 8) & 1u) != 0u;
        prim.ctxt = ((value >> 9) & 1u) != 0u;
        prim.fix = ((value >> 10) & 1u) != 0u;
        return prim;
    }

    static inline uint64_t loadLE64(const uint8_t *p)
    {
        uint64_t v;
        std::memcpy(&v, p, 8);
        return v;
    }

    struct PackedGifPacketTag
    {
        uint64_t lo = 0u;
        uint64_t hi = 0u;
        uint32_t payloadOffset = 0u;
        uint32_t nloop = 0u;
        uint32_t nreg = 0u;
        uint8_t regs[16]{};
    };

    template <typename Visitor>
    bool visitPackedGifPacket(const uint8_t *data, uint32_t sizeBytes, Visitor &&visitor)
    {
        uint32_t offset = 0u;
        while (offset + 16u <= sizeBytes)
        {
            PackedGifPacketTag tag{};
            tag.lo = loadLE64(data + offset);
            tag.hi = loadLE64(data + offset + 8u);

            const uint8_t flg = static_cast<uint8_t>((tag.lo >> 58u) & 0x3u);
            if (flg != GIF_FMT_PACKED)
                return false;

            tag.nloop = static_cast<uint32_t>(tag.lo & 0x7FFFu);
            tag.nreg = static_cast<uint32_t>((tag.lo >> 60u) & 0xFu);
            if (tag.nreg == 0u)
                tag.nreg = 16u;

            const uint64_t payloadBytes64 =
                static_cast<uint64_t>(tag.nloop) * static_cast<uint64_t>(tag.nreg) * 16ull;
            if (payloadBytes64 > 0xFFFFFFFFull)
                return false;

            offset += 16u;
            const uint32_t payloadBytes = static_cast<uint32_t>(payloadBytes64);
            if (payloadBytes > sizeBytes - offset)
                return false;

            tag.payloadOffset = offset;
            for (uint32_t i = 0u; i < tag.nreg; ++i)
                tag.regs[i] = static_cast<uint8_t>((tag.hi >> (i * 4u)) & 0xFu);

            if (!visitor(tag))
                return false;

            offset += payloadBytes;
        }

        return offset == sizeBytes;
    }

    bool validatePackedGifPacket(const uint8_t *data, uint32_t sizeBytes)
    {
        return visitPackedGifPacket(data, sizeBytes, [](const PackedGifPacketTag &)
                                    { return true; });
    }

    std::atomic<uint32_t> s_debugGifPacketCount{0};
    std::atomic<uint32_t> s_debugGsRegisterCount{0};
    std::atomic<uint32_t> s_debugGsPackedVertexCount{0};
    std::atomic<uint32_t> s_debugGsVertexKickCount{0};
    std::atomic<uint32_t> s_debugCopyRegCount{0};
    std::atomic<uint32_t> s_debugTexaWriteCount{0};
    std::atomic<uint32_t> s_debugCvFontUploadCount{0};
    std::atomic<uint32_t> s_debugLocalCopyCount{0};
}


GS::GS()
    : m_backend(GSThreadedBackend::MakeDefault())
{
    m_rawGif = m_backend && m_backend->WantsRawGif();
    reset();
}

void GS::init(uint8_t *vram, uint32_t vramSize, GSRegisters *privRegs)
{
    m_localMemoryStorage = vram;
    m_localMemorySize = vramSize;
    m_privRegs = privRegs;
    if (!m_backend)
        m_backend = GSThreadedBackend::MakeDefault();
    if (!dynamic_cast<GSRecordingBackend *>(m_backend.get()))
        m_backend = GSRecordingBackend::WrapFromEnvironment(std::move(m_backend), privRegs);
    m_rawGif = m_backend->WantsRawGif();
    m_backend->Initialize(vram, vramSize);
    reset();
}

void GS::reset()
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    std::memset(m_ctx, 0, sizeof(m_ctx));
    m_prim = {};
    m_primRegister = {};
    m_prmodeRegister = {};
    m_curR = 0x80;
    m_curG = 0x80;
    m_curB = 0x80;
    m_curA = 0x80;
    m_curQ = 1.0f;
    m_curS = 0.0f;
    m_curT = 0.0f;
    m_curU = 0;
    m_curV = 0;
    m_curFog = 0;
    m_fogR = 0;
    m_fogG = 0;
    m_fogB = 0;
    m_prmodecont = true;
    m_pabe = false;
    m_scanmsk = 0u;
    m_dimx = 0u;
    m_dthe = 0u;
    m_colclamp = 0u;
    m_texa = {0u, false, 0u};
    m_texclut = {0u, 0u, 0u};
    m_bitbltbuf = {};
    m_trxpos = {};
    m_trxreg = {};
    m_trxdir = 3;
    m_vtxCount = 0;
    m_vtxIndex = 0;
    m_preferredDisplaySourceFrame = {};
    m_preferredDisplayDestFbp = 0;
    m_hasPreferredDisplaySource = false;
    if (m_backend)
    {
        m_backend->Flush();
        m_backend->Sync(GSSyncReason::Reset);
        m_backend->Reset();
    }
    {
        std::lock_guard<std::mutex> presentationLock(m_presentationMutex);
        m_hostPresentationFrame.clear();
        m_hostPresentationWidth = 0u;
        m_hostPresentationHeight = 0u;
        m_hostPresentationDisplayFbp = 0u;
        m_hostPresentationSourceFbp = 0u;
        m_hostPresentationUsedPreferred = false;
        m_hasHostPresentationFrame = false;
    }

    m_debugHistoryWrite = 0;
    m_debugHistoryCount = 0;
    m_debugNextSeq = 1;
    m_debugFrameIndex = 0;
    m_debugLastVsyncTick = UINT64_MAX;

    for (int i = 0; i < 2; ++i)
    {
        m_ctx[i].frame.fbw = 10;
        m_ctx[i].scissor = {0, 639, 0, 447};
        m_ctx[i].xyoffset = {0, 0};
    }
}

GSContext &GS::activeContext()
{
    return m_ctx[m_prim.ctxt ? 1 : 0];
}

void GS::snapshotVRAM()
{
    // Presentation/debug snapshots run outside m_stateMutex so the EE can keep
    // feeding the GS while a backend performs host-side conversion. Keep the
    // selected backend alive and unswappable for the duration of the call.
    std::lock_guard<std::mutex> backendLock(m_backendLifetimeMutex);
    if (!m_backend)
        return;
    std::vector<uint8_t> snapshot;
    m_backend->Sync(GSSyncReason::DebugReadback);
    m_backend->SnapshotVram(snapshot);
    std::lock_guard<std::mutex> lock(m_snapshotMutex);
    m_displaySnapshot.swap(snapshot);
}

const uint8_t *GS::lockDisplaySnapshot(uint32_t &outSize)
{
    m_snapshotMutex.lock();
    if (m_displaySnapshot.empty())
    {
        outSize = 0;
        return nullptr;
    }

    outSize = static_cast<uint32_t>(m_displaySnapshot.size());
    return m_displaySnapshot.data();
}

GSDebugSnapshot GS::getDebugSnapshot() const
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);

    GSDebugSnapshot snapshot{};
    snapshot.ctx[0] = m_ctx[0];
    snapshot.ctx[1] = m_ctx[1];
    snapshot.prim = m_prim;
    snapshot.texa = m_texa;
    snapshot.texclut = m_texclut;
    snapshot.scanmsk = m_scanmsk;
    snapshot.dimx = m_dimx;
    snapshot.dthe = m_dthe;
    snapshot.colclamp = m_colclamp;
    snapshot.bitbltbuf = m_bitbltbuf;
    snapshot.trxpos = m_trxpos;
    snapshot.trxreg = m_trxreg;
    const GSTransferSnapshot transfer = m_backend ? m_backend->GetTransferSnapshot() : GSTransferSnapshot{};
    snapshot.trxdir = transfer.direction;
    snapshot.transferX = transfer.x;
    snapshot.transferY = transfer.y;
    snapshot.transferTotalPixels = transfer.totalPixels;
    snapshot.transferCopiedPixels = transfer.copiedPixels;
    snapshot.lastDisplayBaseBytes = m_lastDisplayBaseBytes;
    snapshot.preferredDisplaySourceFrame = m_preferredDisplaySourceFrame;
    snapshot.preferredDisplayDestFbp = m_preferredDisplayDestFbp;
    snapshot.hasPreferredDisplaySource = m_hasPreferredDisplaySource;
    {
        std::lock_guard<std::mutex> presentationLock(m_presentationMutex);
        snapshot.hostPresentationWidth = m_hostPresentationWidth;
        snapshot.hostPresentationHeight = m_hostPresentationHeight;
        snapshot.hostPresentationDisplayFbp = m_hostPresentationDisplayFbp;
        snapshot.hostPresentationSourceFbp = m_hostPresentationSourceFbp;
        snapshot.hostPresentationUsedPreferred = m_hostPresentationUsedPreferred;
        snapshot.hasHostPresentationFrame = m_hasHostPresentationFrame;
    }
    snapshot.localToHostPendingBytes = transfer.localToHostPendingBytes;
    return snapshot;
}

std::vector<GSDebugHistoryEntry> GS::getDebugHistory() const
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);

    std::vector<GSDebugHistoryEntry> out;
    out.reserve(m_debugHistoryCount);
    const size_t first = (m_debugHistoryWrite + kDebugHistoryCapacity - m_debugHistoryCount) % kDebugHistoryCapacity;
    for (size_t i = 0; i < m_debugHistoryCount; ++i)
    {
        out.push_back(m_debugHistory[(first + i) % kDebugHistoryCapacity]);
    }
    return out;
}

void GS::clearDebugHistory()
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    m_debugHistoryWrite = 0;
    m_debugHistoryCount = 0;
    m_debugNextSeq = 1;
    m_debugFrameIndex = 0;
    m_debugLastVsyncTick = UINT64_MAX;
}

bool GS::isDebugHistoryPaused() const
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    return m_debugHistoryPaused;
}

void GS::setDebugHistoryPaused(bool paused)
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    m_debugHistoryPaused = paused;
}

GSDebugHistoryEntry GS::makeDebugEventUnlocked(GSDebugEventKind kind) const
{
    GSDebugHistoryEntry entry{};
    entry.kind = kind;
    entry.prim = m_prim;
    const uint32_t ci = m_prim.ctxt ? 1u : 0u;
    entry.frame = m_ctx[ci].frame;
    entry.zbuf = m_ctx[ci].zbuf;
    entry.tex0 = m_ctx[ci].tex0;
    entry.scissor = m_ctx[ci].scissor;
    entry.test = m_ctx[ci].test;
    entry.alpha = m_ctx[ci].alpha;
    entry.bitbltbuf = m_bitbltbuf;
    entry.trxpos = m_trxpos;
    entry.trxreg = m_trxreg;
    const GSTransferSnapshot transfer = m_backend ? m_backend->GetTransferSnapshot() : GSTransferSnapshot{};
    entry.trxdir = transfer.direction;
    entry.transferPixels = transfer.totalPixels;
    return entry;
}

void GS::recordDebugEventUnlocked(GSDebugHistoryEntry entry)
{
    if (m_debugHistoryPaused)
    {
        return;
    }

    const uint64_t tick = m_privRegs ? m_privRegs->vsyncTick.load(std::memory_order_acquire) : 0u;
    if (m_debugLastVsyncTick == UINT64_MAX)
    {
        m_debugLastVsyncTick = tick;
    }
    else if (tick != m_debugLastVsyncTick)
    {
        ++m_debugFrameIndex;
        m_debugLastVsyncTick = tick;
    }

    entry.seq = m_debugNextSeq++;
    entry.vsyncTick = tick;
    entry.frameIndex = m_debugFrameIndex;

    m_debugHistory[m_debugHistoryWrite] = entry;
    m_debugHistoryWrite = (m_debugHistoryWrite + 1u) % kDebugHistoryCapacity;
    if (m_debugHistoryCount < kDebugHistoryCapacity)
    {
        ++m_debugHistoryCount;
    }
}

void GS::recordGifTagDebugEventUnlocked(uint32_t sizeBytes, uint32_t nloop, uint8_t flg, uint32_t nreg)
{
    if (m_debugHistoryPaused)
    {
        return;
    }

    GSDebugHistoryEntry entry = makeDebugEventUnlocked(GSDebugEventKind::GifTag);
    entry.gifSizeBytes = sizeBytes;
    entry.gifNloop = nloop;
    entry.gifFlg = flg;
    entry.gifNreg = static_cast<uint8_t>(std::min<uint32_t>(nreg, 16u));
    recordDebugEventUnlocked(entry);
}

void GS::recordRegisterDebugEventUnlocked(uint8_t regAddr, uint64_t value)
{
    if (m_debugHistoryPaused)
    {
        return;
    }

    switch (regAddr)
    {
    case GS_REG_PRIM:
    case GS_REG_TEX0_1:
    case GS_REG_TEX0_2:
    case GS_REG_TEX2_1:
    case GS_REG_TEX2_2:
    case GS_REG_TEXA:
    case GS_REG_TEXCLUT:
    case GS_REG_FRAME_1:
    case GS_REG_FRAME_2:
    case GS_REG_ZBUF_1:
    case GS_REG_ZBUF_2:
    case GS_REG_ALPHA_1:
    case GS_REG_ALPHA_2:
    case GS_REG_TEST_1:
    case GS_REG_TEST_2:
    case GS_REG_SCISSOR_1:
    case GS_REG_SCISSOR_2:
    case GS_REG_XYOFFSET_1:
    case GS_REG_XYOFFSET_2:
    case GS_REG_BITBLTBUF:
    case GS_REG_TRXPOS:
    case GS_REG_TRXREG:
    case GS_REG_TRXDIR:
        break;
    default:
        return;
    }

    GSDebugHistoryEntry entry = makeDebugEventUnlocked(GSDebugEventKind::Register);
    entry.reg = regAddr;
    entry.regValue = value;
    recordDebugEventUnlocked(entry);
}

void GS::recordDrawDebugEventUnlocked(int vertexCount)
{
    if (m_debugHistoryPaused)
    {
        return;
    }

    if (vertexCount <= 0)
    {
        return;
    }

    GSDebugHistoryEntry entry = makeDebugEventUnlocked(GSDebugEventKind::Draw);
    entry.vertexCount = static_cast<uint32_t>(vertexCount);

    const int count = std::min(vertexCount, kMaxVerts);
    entry.xMin = entry.xMax = m_vtxQueue[0].x;
    entry.yMin = entry.yMax = m_vtxQueue[0].y;
    entry.zMin = entry.zMax = m_vtxQueue[0].z;
    entry.aMin = entry.aMax = m_vtxQueue[0].a;

    for (int i = 1; i < count; ++i)
    {
        const GSVertex &v = m_vtxQueue[i];
        entry.xMin = std::min(entry.xMin, v.x);
        entry.xMax = std::max(entry.xMax, v.x);
        entry.yMin = std::min(entry.yMin, v.y);
        entry.yMax = std::max(entry.yMax, v.y);
        entry.zMin = std::min(entry.zMin, v.z);
        entry.zMax = std::max(entry.zMax, v.z);
        entry.aMin = std::min(entry.aMin, v.a);
        entry.aMax = std::max(entry.aMax, v.a);
    }

    recordDebugEventUnlocked(entry);
}

void GS::recordTransferDebugEventUnlocked()
{
    if (m_debugHistoryPaused)
    {
        return;
    }

    GSDebugHistoryEntry entry = makeDebugEventUnlocked(GSDebugEventKind::Transfer);
    entry.transferPixels = m_backend ? m_backend->GetTransferSnapshot().totalPixels : 0u;
    recordDebugEventUnlocked(entry);
}

void GS::recordPresentDebugEventUnlocked(uint32_t displayFbp, uint32_t sourceFbp, uint32_t width, uint32_t height, bool usedPreferred)
{
    if (m_debugHistoryPaused)
    {
        return;
    }

    GSDebugHistoryEntry entry = makeDebugEventUnlocked(GSDebugEventKind::Present);
    entry.displayFbp = displayFbp;
    entry.sourceFbp = sourceFbp;
    entry.width = width;
    entry.height = height;
    entry.usedPreferred = usedPreferred;
    recordDebugEventUnlocked(entry);
}

bool GS::getPreferredDisplaySource(GSFrameReg &outSource, uint32_t &outDestFbp) const
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    if (!m_hasPreferredDisplaySource)
    {
        outSource = {};
        outDestFbp = 0u;
        return false;
    }

    outSource = m_preferredDisplaySourceFrame;
    outDestFbp = m_preferredDisplayDestFbp;
    return true;
}

void GS::unlockDisplaySnapshot()
{
    m_snapshotMutex.unlock();
}

uint32_t GS::getLastDisplayBaseBytes() const
{
    return m_lastDisplayBaseBytes;
}

void GS::refreshDisplaySnapshot()
{
    snapshotVRAM();
}

GSPresentationRequest GS::buildPresentationRequestUnlocked() const
{
    GSPresentationRequest request{};
    if (!m_privRegs)
        return request;
    request.pmode = m_privRegs->pmode;
    request.smode2 = m_privRegs->smode2;
    request.dispfb1 = m_privRegs->dispfb1;
    request.display1 = m_privRegs->display1;
    request.dispfb2 = m_privRegs->dispfb2;
    request.display2 = m_privRegs->display2;
    request.bgcolor = m_privRegs->bgcolor;
    request.vsyncTick = m_privRegs->vsyncTick.load(std::memory_order_acquire);
    request.contextFrames[0] = m_ctx[0].frame;
    request.contextFrames[1] = m_ctx[1].frame;
    request.preferredSource = m_preferredDisplaySourceFrame;
    request.preferredDestFbp = m_preferredDisplayDestFbp;
    request.hasPreferredSource = m_hasPreferredDisplaySource;
    return request;
}

void GS::latchHostPresentationFrame()
{
    GSPresentationRequest request{};
    {
        std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
        if (!m_backend || !m_privRegs)
        {
            std::lock_guard<std::mutex> presentationLock(m_presentationMutex);
            m_hostPresentationFrame.clear();
            m_hasHostPresentationFrame = false;
            m_hostPresentationWidth = m_hostPresentationHeight = 0u;
            return;
        }
        request = buildPresentationRequestUnlocked();
    }

    PresentationFrame frame{};
    {
        std::lock_guard<std::mutex> backendLock(m_backendLifetimeMutex);
        if (m_backend)
        {
            m_backend->Flush();
            m_backend->Sync(GSSyncReason::Presentation);
            frame = m_backend->Present(request);
        }
    }

    const bool hasFrame = static_cast<bool>(frame);
    const uint32_t displayFbp = frame.displayFbp;
    const uint32_t sourceFbp = frame.sourceFbp;
    const uint32_t width = frame.width;
    const uint32_t height = frame.height;
    const bool usedPreferred = frame.usedPreferred;
    {
        std::lock_guard<std::mutex> presentationLock(m_presentationMutex);
        m_hostPresentationFrame = std::move(frame.pixels);
        m_hostPresentationWidth = width;
        m_hostPresentationHeight = height;
        m_hostPresentationDisplayFbp = displayFbp;
        m_hostPresentationSourceFbp = sourceFbp;
        m_hostPresentationUsedPreferred = usedPreferred;
        m_hasHostPresentationFrame = hasFrame;
    }

    if (hasFrame)
    {
        std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
        recordPresentDebugEventUnlocked(displayFbp, sourceFbp, width, height, usedPreferred);
    }
}

bool GS::copyLatchedHostPresentationFrame(std::vector<uint8_t> &outPixels,
                                          uint32_t &outWidth,
                                          uint32_t &outHeight,
                                          uint32_t *outDisplayFbp,
                                          uint32_t *outSourceFbp,
                                          bool *outUsedPreferred) const
{
    std::lock_guard<std::mutex> lock(m_presentationMutex);
    if (!m_hasHostPresentationFrame || m_hostPresentationFrame.empty())
    {
        outPixels.clear();
        outWidth = 0u;
        outHeight = 0u;
        if (outDisplayFbp)
            *outDisplayFbp = 0u;
        if (outSourceFbp)
            *outSourceFbp = 0u;
        if (outUsedPreferred)
            *outUsedPreferred = false;
        return false;
    }

    outWidth = m_hostPresentationWidth;
    outHeight = m_hostPresentationHeight;
    if (outDisplayFbp)
        *outDisplayFbp = m_hostPresentationDisplayFbp;
    if (outSourceFbp)
        *outSourceFbp = m_hostPresentationSourceFbp;
    if (outUsedPreferred)
        *outUsedPreferred = m_hostPresentationUsedPreferred;

    const size_t packedBytes = static_cast<size_t>(outWidth) * static_cast<size_t>(outHeight) * 4u;
    if (packedBytes > m_hostPresentationFrame.size())
    {
        outPixels.clear();
        outWidth = 0u;
        outHeight = 0u;
        if (outDisplayFbp)
            *outDisplayFbp = 0u;
        if (outSourceFbp)
            *outSourceFbp = 0u;
        if (outUsedPreferred)
            *outUsedPreferred = false;
        return false;
    }
    outPixels.assign(m_hostPresentationFrame.begin(), m_hostPresentationFrame.begin() + static_cast<std::ptrdiff_t>(packedBytes));
    return true;
}

void GS::processGIFPacket(const uint8_t *data, uint32_t sizeBytes)
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    if (!data || sizeBytes < 16 || !m_backend)
        return;

    if (gsSignalStallEnabled())
    {
        std::lock_guard<std::mutex> stallLock(g_stallMutex);
        g_stall.owner = this;
        g_stall.regs = m_privRegs;
        if (g_stall.active && !t_stallReplaying)
        {
            g_stall.queue.emplace_back(data, data + sizeBytes);
            return;
        }
    }

    if (!g_gifCont.active && tryProcessNativeImageUploadPacket(data, sizeBytes))
    {
        if (m_rawGif)
            m_backend->RawGif(data, sizeBytes);
        m_backend->Flush();
        return;
    }

    PS2_IF_AGRESSIVE_LOGS({
        const uint32_t packetIndex = s_debugGifPacketCount.fetch_add(1, std::memory_order_relaxed);
        if (packetIndex < 48u)
        {
            const uint64_t tagLo = loadLE64(data);
            const uint32_t nloop = static_cast<uint32_t>(tagLo & 0x7FFFu);
            const uint8_t flg = static_cast<uint8_t>((tagLo >> 58) & 0x3u);
            uint32_t nreg = static_cast<uint32_t>((tagLo >> 60) & 0xFu);
            if (nreg == 0u)
                nreg = 16u;
            RUNTIME_LOG("[gs:gif] idx=" << packetIndex
                                        << " size=" << sizeBytes
                                        << " nloop=" << nloop
                                        << " flg=" << static_cast<uint32_t>(flg)
                                        << " nreg=" << nreg
                                        << " ctx0fbp=" << m_ctx[0].frame.fbp
                                        << " ctx1fbp=" << m_ctx[1].frame.fbp
                                        << std::endl);
        }
    });

    uint32_t offset = 0;
    while (offset + 16 <= sizeBytes)
    {
        const bool resumed = g_gifCont.active;
        uint64_t tagLo = 0u;
        uint64_t tagHi = 0u;
        uint32_t nloop = 0u;
        uint32_t startReg = 0u;
        bool regPadOdd = false;
        if (resumed)
        {
            tagLo = g_gifCont.tagLo;
            tagHi = g_gifCont.tagHi;
            nloop = g_gifCont.loopsLeft;
            startReg = g_gifCont.regIndex;
            regPadOdd = g_gifCont.padOdd;
            g_gifCont.active = false;
        }
        else
        {
            tagLo = loadLE64(data + offset);
            tagHi = loadLE64(data + offset + 8);
            offset += 16;
            m_curQ = 1.0f;
            nloop = static_cast<uint32_t>(tagLo & 0x7FFF);
        }

        uint8_t flg = static_cast<uint8_t>((tagLo >> 58) & 0x3);
        uint32_t nreg = static_cast<uint32_t>((tagLo >> 60) & 0xF);
        if (nreg == 0)
            nreg = 16;
        if (!resumed)
            regPadOdd = ((nloop * nreg) & 1u) != 0u;

        recordGifTagDebugEventUnlocked(sizeBytes, nloop, flg, nreg);

        bool pre = ((tagLo >> 46) & 1) != 0;
        if (pre && !resumed)
        {
            writeRegisterUnlocked(GS_REG_PRIM, (tagLo >> 47) & 0x7FF);
        }

        uint8_t regs[16];
        for (uint32_t i = 0; i < nreg; ++i)
            regs[i] = static_cast<uint8_t>((tagHi >> (i * 4)) & 0xF);

        if (flg == GIF_FMT_PACKED)
        {
            for (uint32_t loop = 0; loop < nloop; ++loop)
            {
                for (uint32_t r = (loop == 0u ? startReg : 0u); r < nreg; ++r)
                {
                    if (offset + 16 > sizeBytes)
                    {
                        saveGifContinuation(tagLo, tagHi, nloop - loop, r, false);
                        if (m_rawGif)
                            m_backend->RawGif(data, sizeBytes);
                        m_backend->Flush();
                        return;
                    }
                    uint64_t lo = loadLE64(data + offset);
                    uint64_t hi = loadLE64(data + offset + 8);
                    if (regs[r] == 0xEu && (hi & 0xFFu) == GS_REG_SIGNAL && gsSignalStallEnabled() && m_privRegs &&
                        ps2xGsSignalBlocked(*m_privRegs))
                    {
                        // Stall: keep everything from this SIGNAL on, re-tagged, plus the rest of the packet.
                        if (!t_stallReplaying)
                            ps2x::asyncvif::traceEvent('J', static_cast<uint32_t>(g_gsTracePath) | 0x100u);
                        if (m_rawGif)
                        {
                            // Raw GIF back end: the rest waits as it is and the tag resumes from a continuation, so
                            // the back end receives the original byte stream (a re-tagged rest would not continue
                            // the tag it is in the middle of).
                            g_gifCont = {true, tagLo, tagHi, nloop - loop, r, false};
                            bool releaseNow = false;
                            {
                                std::lock_guard<std::mutex> stallLock(g_stallMutex);
                                g_stall.queue.emplace_front(data + offset, data + sizeBytes);
                                g_stall.active = true;
                                t_stalledAgain = true;
                                releaseNow = !ps2xGsSignalBlocked(*m_privRegs) && !t_stallReplaying;
                            }
                            if (gsSignalLogEnabled())
                                gsSignalLog("SIGNAL stalled", hi, m_privRegs->csr.load());
                            if (offset != 0u)
                                m_backend->RawGif(data, offset);
                            m_backend->Flush();
                            if (releaseNow)
                                ps2xGsReleaseStall();
                            return;
                        }
                        const bool eop = ((tagLo >> 15) & 1u) != 0u;
                        std::vector<uint8_t> rest;
                        uint32_t restLoops = nloop - loop;
                        if (r != 0u)
                        {
                            appendGifTag(rest, 1u, false, GIF_FMT_PACKED, nreg - r, tagHi >> (4u * r));
                            rest.insert(rest.end(), data + offset, data + offset + (nreg - r) * 16u);
                            --restLoops;
                        }
                        const uint32_t from = r != 0u ? offset + (nreg - r) * 16u : offset;
                        if (restLoops != 0u)
                        {
                            const uint32_t to = std::min<uint32_t>(sizeBytes, from + restLoops * nreg * 16u);
                            appendGifTag(rest, restLoops, eop, GIF_FMT_PACKED, nreg, tagHi);
                            rest.insert(rest.end(), data + std::min(from, sizeBytes), data + to);
                        }
                        const uint32_t tagEnd = std::min<uint32_t>(sizeBytes, from + restLoops * nreg * 16u);
                        rest.insert(rest.end(), data + tagEnd, data + sizeBytes);
                        bool releaseNow = false;
                        {
                            std::lock_guard<std::mutex> stallLock(g_stallMutex);
                            g_stall.queue.push_front(std::move(rest));
                            g_stall.active = true;
                            t_stalledAgain = true;
                            // The EE may have cleared CSR.SIGNAL while we were building the remainder.
                            releaseNow = !ps2xGsSignalBlocked(*m_privRegs) && !t_stallReplaying;
                        }
                        if (gsSignalLogEnabled())
                            gsSignalLog("SIGNAL stalled", hi, m_privRegs->csr.load());
                        if (gsSignalTraceAll())
                            std::fprintf(stderr, "[gs-sig2] stall at SIGNAL id=%x mask=%x (deferred %zu bytes, replay=%d)\n",
                                         static_cast<uint32_t>(lo), static_cast<uint32_t>(lo >> 32), g_stall.queue.front().size(),
                                         t_stallReplaying ? 1 : 0);
                        m_backend->Flush();
                        if (releaseNow)
                            ps2xGsReleaseStall();
                        return;
                    }
                    offset += 16;
                    writeRegisterPacked(regs[r], lo, hi);
                }
            }
        }
        else if (flg == GIF_FMT_REGLIST)
        {
            for (uint32_t loop = 0; loop < nloop; ++loop)
            {
                for (uint32_t r = (loop == 0u ? startReg : 0u); r < nreg; ++r)
                {
                    if (offset + 8 > sizeBytes)
                    {
                        saveGifContinuation(tagLo, tagHi, nloop - loop, r, regPadOdd);
                        if (m_rawGif)
                            m_backend->RawGif(data, sizeBytes);
                        m_backend->Flush();
                        return;
                    }
                    writeRegisterUnlocked(regs[r], loadLE64(data + offset));
                    offset += 8;
                }
            }
            if (regPadOdd)
                offset += 8;
        }
        else if (flg == GIF_FMT_IMAGE)
        {
            uint32_t imageBytes = nloop * 16;
            if (offset + imageBytes > sizeBytes)
            {
                imageBytes = (sizeBytes - offset) & ~15u;
                saveGifContinuation(tagLo, tagHi, nloop - imageBytes / 16u, 0u, false);
            }
            processImageData(data + offset, imageBytes);
            offset += imageBytes;
        }
    }
    if (m_rawGif)
        m_backend->RawGif(data, sizeBytes);
    m_backend->Flush();
}

bool GS::processNativePackedGIFPacket(const uint8_t *data, uint32_t sizeBytes)
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    if (!data || sizeBytes < 16u || !m_backend)
        return false;

    if (g_gifCont.active)
    {
        processGIFPacket(data, sizeBytes);
        ++m_nativePackedGIFPacketCount;
        return true;
    }

    if (!validatePackedGifPacket(data, sizeBytes))
        return false;

    if (gsSignalStallEnabled() || m_rawGif)
    {
        // One implementation of the SIGNAL stall (and of the raw GIF forwarding): PACKED packets go through
        // processGIFPacket.
        processGIFPacket(data, sizeBytes);
        ++m_nativePackedGIFPacketCount;
        return true;
    }

    const bool processed = visitPackedGifPacket(data, sizeBytes, [&](const PackedGifPacketTag &tag)
                                                {
        m_curQ = 1.0f;

        recordGifTagDebugEventUnlocked(sizeBytes, tag.nloop, GIF_FMT_PACKED, tag.nreg);

        const bool pre = ((tag.lo >> 46u) & 1u) != 0u;
        if (pre)
            writeRegisterUnlocked(GS_REG_PRIM, (tag.lo >> 47u) & 0x7FFu);

        uint32_t offset = tag.payloadOffset;
        for (uint32_t loop = 0u; loop < tag.nloop; ++loop)
        {
            for (uint32_t r = 0u; r < tag.nreg; ++r)
            {
                const uint64_t lo = loadLE64(data + offset);
                const uint64_t hi = loadLE64(data + offset + 8u);
                offset += 16u;
                writeRegisterPacked(tag.regs[r], lo, hi);
            }
        }

        return true; });

    m_backend->Flush();
    if (!processed)
        return false;

    ++m_nativePackedGIFPacketCount;
    return true;
}

void GS::uploadImageNative(uint64_t bitbltbuf,
                           uint64_t trxpos,
                           uint64_t trxreg,
                           uint64_t trxdir,
                           const uint8_t *data,
                           uint32_t sizeBytes)
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    if (m_rawGif && data && sizeBytes >= 16u)
    {
        // As the GIF packet it stands for, so the raw GIF back end gets it too (and in order with a SIGNAL stall).
        std::vector<uint8_t> packet;
        packet.reserve(96u + sizeBytes);
        const uint64_t setup[4][2] = {{bitbltbuf, GS_REG_BITBLTBUF}, {trxpos, GS_REG_TRXPOS}, {trxreg, GS_REG_TRXREG}, {trxdir, GS_REG_TRXDIR}};
        appendGifTag(packet, 4u, false, GIF_FMT_PACKED, 1u, 0xEull);
        packet.insert(packet.end(), reinterpret_cast<const uint8_t *>(setup), reinterpret_cast<const uint8_t *>(setup) + sizeof(setup));
        appendGifTag(packet, sizeBytes / 16u, true, GIF_FMT_IMAGE, 0u, 0u);
        packet.insert(packet.end(), data, data + (sizeBytes & ~15u));
        processGIFPacket(packet.data(), static_cast<uint32_t>(packet.size()));
        return;
    }
    uploadImageNativeUnlocked(bitbltbuf, trxpos, trxreg, trxdir, data, sizeBytes);
    if (m_backend)
        m_backend->Flush();
}

void GS::uploadImageNativeUnlocked(uint64_t bitbltbuf,
                                   uint64_t trxpos,
                                   uint64_t trxreg,
                                   uint64_t trxdir,
                                   const uint8_t *data,
                                   uint32_t sizeBytes)
{
    if (!data || sizeBytes == 0 || !m_backend)
        return;

    writeRegisterUnlocked(GS_REG_BITBLTBUF, bitbltbuf);
    writeRegisterUnlocked(GS_REG_TRXPOS, trxpos);
    writeRegisterUnlocked(GS_REG_TRXREG, trxreg);
    writeRegisterUnlocked(GS_REG_TRXDIR, trxdir);
    processImageData(data, sizeBytes);
    ++m_nativeImageUploadCount;
}

bool GS::tryProcessNativeImageUploadPacket(const uint8_t *data, uint32_t sizeBytes)
{
    constexpr uint32_t kSetupRegisters = 4u;
    constexpr uint32_t kPackedAdPayloadBytes = kSetupRegisters * 16u;
    constexpr uint64_t kPackedAdDescriptor = 0x0Eull;

    if (!data || sizeBytes < 16u + kPackedAdPayloadBytes + 16u)
        return false;

    const uint64_t setupTagLo = loadLE64(data);
    const uint64_t setupTagHi = loadLE64(data + 8u);
    const uint32_t setupNloop = static_cast<uint32_t>(setupTagLo & 0x7FFFu);
    const uint8_t setupFlg = static_cast<uint8_t>((setupTagLo >> 58u) & 0x3u);
    uint32_t setupNreg = static_cast<uint32_t>((setupTagLo >> 60u) & 0xFu);
    if (setupNreg == 0u)
        setupNreg = 16u;

    if (setupNloop != kSetupRegisters ||
        setupFlg != GIF_FMT_PACKED ||
        setupNreg != 1u ||
        (setupTagHi & 0xFull) != kPackedAdDescriptor)
    {
        return false;
    }

    uint64_t regs[kSetupRegisters] = {};
    uint32_t offset = 16u;
    constexpr uint8_t expectedRegs[kSetupRegisters] = {
        GS_REG_BITBLTBUF,
        GS_REG_TRXPOS,
        GS_REG_TRXREG,
        GS_REG_TRXDIR,
    };

    for (uint32_t i = 0; i < kSetupRegisters; ++i)
    {
        regs[i] = loadLE64(data + offset);
        const uint64_t reg = loadLE64(data + offset + 8u);
        if ((reg & 0xFFu) != expectedRegs[i])
            return false;
        offset += 16u;
    }

    const uint32_t trxdirMode = static_cast<uint32_t>(regs[3] & 0x3ull);
    const uint32_t rrw = static_cast<uint32_t>(regs[2] & 0xFFFull);
    const uint32_t rrh = static_cast<uint32_t>((regs[2] >> 32u) & 0xFFFull);
    if (trxdirMode != 0u || rrw == 0u || rrh == 0u)
        return false;

    if (offset + 16u > sizeBytes)
        return false;

    const uint64_t imageTagLo = loadLE64(data + offset);
    const uint8_t imageFlg = static_cast<uint8_t>((imageTagLo >> 58u) & 0x3u);
    const uint32_t imageNloop = static_cast<uint32_t>(imageTagLo & 0x7FFFu);
    if (imageFlg != GIF_FMT_IMAGE || imageNloop == 0u)
        return false;

    offset += 16u;
    const uint64_t imageBytes64 = static_cast<uint64_t>(imageNloop) * 16ull;
    if (imageBytes64 > 0xFFFFFFFFull)
        return false;
    const uint32_t imageBytes = static_cast<uint32_t>(imageBytes64);
    if (offset + imageBytes != sizeBytes)
        return false;

    uploadImageNativeUnlocked(regs[0], regs[1], regs[2], regs[3], data + offset, imageBytes);
    return true;
}

void GS::writeRegisterPacked(uint8_t regDesc, uint64_t lo, uint64_t hi)
{
    switch (regDesc)
    {
    case 0x00:
        writeRegisterUnlocked(GS_REG_PRIM, lo & 0x7FF);
        break;
    case 0x01:
        m_curR = static_cast<uint8_t>(lo & 0xFF);
        m_curG = static_cast<uint8_t>((lo >> 32) & 0xFF);
        m_curB = static_cast<uint8_t>(hi & 0xFF);
        m_curA = static_cast<uint8_t>((hi >> 32) & 0xFF);
        break;
    case 0x02:
    {
        uint32_t sBits = static_cast<uint32_t>(lo & 0xFFFFFFFF);
        uint32_t tBits = static_cast<uint32_t>((lo >> 32) & 0xFFFFFFFF);
        uint32_t qBits = static_cast<uint32_t>(hi & 0xFFFFFFFF);
        std::memcpy(&m_curS, &sBits, 4);
        std::memcpy(&m_curT, &tBits, 4);
        std::memcpy(&m_curQ, &qBits, 4);
        if (m_curQ == 0.0f)
            m_curQ = 1.0f;
        break;
    }
    case 0x03:
        m_curU = static_cast<uint16_t>(lo & 0x3FFFu);
        m_curV = static_cast<uint16_t>((lo >> 32) & 0x3FFFu);
        break;
    case 0x04:
    {
        uint16_t x = static_cast<uint16_t>(lo & 0xFFFF);
        uint16_t y = static_cast<uint16_t>((lo >> 32) & 0xFFFF);
        uint32_t z = static_cast<uint32_t>((hi >> 4) & 0xFFFFFF);
        uint8_t f = static_cast<uint8_t>((hi >> 36) & 0xFF);
        bool adk = ((hi >> 47) & 1) != 0;
        PS2_IF_AGRESSIVE_LOGS({
            const uint32_t debugIndex = s_debugGsPackedVertexCount.fetch_add(1, std::memory_order_relaxed);
            if (debugIndex < 64u)
            {
                RUNTIME_LOG("[gs:packed-xyzf] idx=" << debugIndex
                                                    << " x=" << x
                                                    << " y=" << y
                                                    << " z=0x" << std::hex << z
                                                    << std::dec
                                                    << " fog=" << static_cast<uint32_t>(f)
                                                    << " kick=" << static_cast<uint32_t>(!adk ? 1u : 0u)
                                                    << " prim=" << static_cast<uint32_t>(m_prim.type)
                                                    << std::endl);
            }
        });
        GSVertex &vtx = m_vtxQueue[m_vtxCount % kMaxVerts];
        vtx.x = static_cast<float>(x) / 16.0f;
        vtx.y = static_cast<float>(y) / 16.0f;
        vtx.z = static_cast<float>(z);
        vtx.r = m_curR;
        vtx.g = m_curG;
        vtx.b = m_curB;
        vtx.a = m_curA;
        vtx.q = m_curQ;
        vtx.s = m_curS;
        vtx.t = m_curT;
        vtx.u = m_curU;
        vtx.v = m_curV;
        vtx.fog = f;
        vertexKick(!adk);
        break;
    }
    case 0x05:
    {
        uint16_t x = static_cast<uint16_t>(lo & 0xFFFF);
        uint16_t y = static_cast<uint16_t>((lo >> 32) & 0xFFFF);
        uint32_t z = static_cast<uint32_t>(hi & 0xFFFFFFFF);
        bool adk = ((hi >> 47) & 1) != 0;
        PS2_IF_AGRESSIVE_LOGS({
            const uint32_t debugIndex = s_debugGsPackedVertexCount.fetch_add(1, std::memory_order_relaxed);
            if (debugIndex < 64u)
            {
                RUNTIME_LOG("[gs:packed-xyz] idx=" << debugIndex
                                                   << " x=" << x
                                                   << " y=" << y
                                                   << " z=0x" << std::hex << z
                                                   << std::dec
                                                   << " kick=" << static_cast<uint32_t>(!adk ? 1u : 0u)
                                                   << " prim=" << static_cast<uint32_t>(m_prim.type)
                                                   << std::endl);
            }
        });
        GSVertex &vtx = m_vtxQueue[m_vtxCount % kMaxVerts];
        vtx.x = static_cast<float>(x) / 16.0f;
        vtx.y = static_cast<float>(y) / 16.0f;
        vtx.z = static_cast<float>(z);
        vtx.r = m_curR;
        vtx.g = m_curG;
        vtx.b = m_curB;
        vtx.a = m_curA;
        vtx.q = m_curQ;
        vtx.s = m_curS;
        vtx.t = m_curT;
        vtx.u = m_curU;
        vtx.v = m_curV;
        vtx.fog = m_curFog;
        vertexKick(!adk);
        break;
    }
    case 0x0A:
        m_curFog = static_cast<uint8_t>((hi >> 36) & 0xFF);
        break;
    case 0x0C:
    {
        PS2_IF_AGRESSIVE_LOGS({
            const uint32_t debugIndex = s_debugGsPackedVertexCount.fetch_add(1, std::memory_order_relaxed);
            if (debugIndex < 64u)
            {
                RUNTIME_LOG("[gs:packed-xyzf3] idx=" << debugIndex
                                                     << " x=" << static_cast<uint32_t>(lo & 0xFFFFu)
                                                     << " y=" << static_cast<uint32_t>((lo >> 32) & 0xFFFFu)
                                                     << " kick=0"
                                                     << " prim=" << static_cast<uint32_t>(m_prim.type)
                                                     << std::endl);
            }
        });
        GSVertex &vtx = m_vtxQueue[m_vtxCount % kMaxVerts];
        vtx.x = static_cast<float>(lo & 0xFFFF) / 16.0f;
        vtx.y = static_cast<float>((lo >> 32) & 0xFFFF) / 16.0f;
        vtx.z = static_cast<float>((hi >> 4) & 0xFFFFFF);
        vtx.r = m_curR;
        vtx.g = m_curG;
        vtx.b = m_curB;
        vtx.a = m_curA;
        vtx.q = m_curQ;
        vtx.s = m_curS;
        vtx.t = m_curT;
        vtx.u = m_curU;
        vtx.v = m_curV;
        vtx.fog = static_cast<uint8_t>((hi >> 36) & 0xFF);
        vertexKick(false);
        break;
    }
    case 0x0D:
    {
        PS2_IF_AGRESSIVE_LOGS({
            const uint32_t debugIndex = s_debugGsPackedVertexCount.fetch_add(1, std::memory_order_relaxed);
            if (debugIndex < 64u)
            {
                RUNTIME_LOG("[gs:packed-xyz3] idx=" << debugIndex
                                                    << " x=" << static_cast<uint32_t>(lo & 0xFFFFu)
                                                    << " y=" << static_cast<uint32_t>((lo >> 32) & 0xFFFFu)
                                                    << " kick=0"
                                                    << " prim=" << static_cast<uint32_t>(m_prim.type)
                                                    << std::endl);
            }
        });
        GSVertex &vtx = m_vtxQueue[m_vtxCount % kMaxVerts];
        vtx.x = static_cast<float>(lo & 0xFFFF) / 16.0f;
        vtx.y = static_cast<float>((lo >> 32) & 0xFFFF) / 16.0f;
        vtx.z = static_cast<float>(hi & 0xFFFFFFFF);
        vtx.r = m_curR;
        vtx.g = m_curG;
        vtx.b = m_curB;
        vtx.a = m_curA;
        vtx.q = m_curQ;
        vtx.s = m_curS;
        vtx.t = m_curT;
        vtx.u = m_curU;
        vtx.v = m_curV;
        vtx.fog = m_curFog;
        vertexKick(false);
        break;
    }
    case 0x0E:
    {
        uint8_t addr = static_cast<uint8_t>(hi & 0xFF);
        writeRegisterUnlocked(addr, lo);
        break;
    }
    case 0x0F:
        break;
    default:
        writeRegisterUnlocked(regDesc, lo);
        break;
    }
}

void GS::writeRegister(uint8_t regAddr, uint64_t value)
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    if (m_rawGif)
    {
        // An A+D packet, so the raw GIF back end sees the write too.
        std::vector<uint8_t> packet;
        appendGifTag(packet, 1u, true, GIF_FMT_PACKED, 1u, 0xEull);
        const uint64_t ad[2] = {value, regAddr};
        packet.insert(packet.end(), reinterpret_cast<const uint8_t *>(ad), reinterpret_cast<const uint8_t *>(ad) + sizeof(ad));
        processGIFPacket(packet.data(), static_cast<uint32_t>(packet.size()));
        return;
    }
    writeRegisterUnlocked(regAddr, value);
}

void GS::vblank()
{
    if (!m_rawGif || !m_backend || !m_privRegs)
        return;
    const GSRegisters &p = *m_privRegs;
    GSVSyncRegs regs;
    const uint64_t regset1[15] = {p.pmode, p.smode1, p.smode2, p.srfsh, p.synch1, p.synch2, p.syncv, p.dispfb1,
                                  p.display1, p.dispfb2, p.display2, p.extbuf, p.extdata, p.extwrite, p.bgcolor};
    std::memcpy(regs.regset1, regset1, sizeof(regset1));
    regs.csr = p.csr.load(std::memory_order_acquire);
    regs.imr = p.imr;
    regs.busdir = p.busdir;
    regs.siglblid = p.siglblid;
    m_backend->VSync(regs);
}

void GS::writeRegisterUnlocked(uint8_t regAddr, uint64_t value)
{
    const bool interestingReg =
        regAddr == GS_REG_PRIM ||
        regAddr == GS_REG_RGBAQ ||
        regAddr == GS_REG_ST ||
        regAddr == GS_REG_UV ||
        regAddr == GS_REG_XYZ2 ||
        regAddr == GS_REG_XYZ3 ||
        regAddr == GS_REG_XYZF2 ||
        regAddr == GS_REG_XYZF3 ||
        regAddr == GS_REG_TEX0_1 ||
        regAddr == GS_REG_TEX0_2 ||
        regAddr == GS_REG_TEX2_1 ||
        regAddr == GS_REG_TEX2_2 ||
        regAddr == GS_REG_TEXCLUT ||
        regAddr == GS_REG_TEXA ||
        regAddr == GS_REG_XYOFFSET_1 ||
        regAddr == GS_REG_XYOFFSET_2 ||
        regAddr == GS_REG_SCISSOR_1 ||
        regAddr == GS_REG_SCISSOR_2 ||
        regAddr == GS_REG_FRAME_1 ||
        regAddr == GS_REG_FRAME_2 ||
        regAddr == GS_REG_ALPHA_1 ||
        regAddr == GS_REG_ALPHA_2 ||
        regAddr == GS_REG_TEST_1 ||
        regAddr == GS_REG_TEST_2 ||
        regAddr == GS_REG_BITBLTBUF ||
        regAddr == GS_REG_TRXPOS ||
        regAddr == GS_REG_TRXREG ||
        regAddr == GS_REG_TRXDIR;

    PS2_IF_AGRESSIVE_LOGS({
        if (interestingReg)
        {
            const uint32_t debugIndex = s_debugGsRegisterCount.fetch_add(1, std::memory_order_relaxed);
            if (debugIndex < 128u)
            {
                RUNTIME_LOG("[gs:reg] idx=" << debugIndex
                                            << " reg=0x" << std::hex << static_cast<uint32_t>(regAddr)
                                            << " value=0x" << value
                                            << std::dec
                                            << std::endl);
            }
        }
    });

    const bool isCopyRelevantReg =
        regAddr == GS_REG_PRIM ||
        regAddr == GS_REG_TEX0_2 ||
        regAddr == GS_REG_TEX1_2 ||
        regAddr == GS_REG_ALPHA_2 ||
        regAddr == GS_REG_TEST_2 ||
        regAddr == GS_REG_PABE ||
        regAddr == GS_REG_FRAME_2 ||
        regAddr == GS_REG_XYOFFSET_2 ||
        regAddr == GS_REG_SCISSOR_2;
    PS2_IF_AGRESSIVE_LOGS({
        if (isCopyRelevantReg &&
            s_debugCopyRegCount.fetch_add(1u, std::memory_order_relaxed) < 64u)
        {
            RUNTIME_LOG("[gs:copy-reg] reg=0x"
                        << std::hex << static_cast<uint32_t>(regAddr)
                        << " value=0x" << value
                        << std::dec
                        << " primCtxt=" << static_cast<uint32_t>(m_prim.ctxt)
                        << " ctx0fbp=" << m_ctx[0].frame.fbp
                        << " ctx1fbp=" << m_ctx[1].frame.fbp
                        << std::endl);
        }
    });

    switch (regAddr)
    {
    case GS_REG_PRIM:
    {
        m_primRegister = decodePrimRegister(value);
        if (m_prmodecont)
        {
            m_prim = m_primRegister;
        }
        else
        {
            // PRIM always selects the primitive topology. With AC=0, all
            // rendering attributes remain sourced from PRMODE.
            m_prim.type = m_primRegister.type;
        }
        m_vtxCount = 0;
        m_vtxIndex = 0;
        break;
    }
    case GS_REG_RGBAQ:
    {
        m_curR = static_cast<uint8_t>(value & 0xFF);
        m_curG = static_cast<uint8_t>((value >> 8) & 0xFF);
        m_curB = static_cast<uint8_t>((value >> 16) & 0xFF);
        m_curA = static_cast<uint8_t>((value >> 24) & 0xFF);
        uint32_t qBits = static_cast<uint32_t>((value >> 32) & 0xFFFFFFFF);
        std::memcpy(&m_curQ, &qBits, 4);
        if (m_curQ == 0.0f)
            m_curQ = 1.0f;
        break;
    }
    case GS_REG_ST:
    {
        uint32_t sBits = static_cast<uint32_t>(value & 0xFFFFFFFF);
        uint32_t tBits = static_cast<uint32_t>((value >> 32) & 0xFFFFFFFF);
        std::memcpy(&m_curS, &sBits, 4);
        std::memcpy(&m_curT, &tBits, 4);
        break;
    }
    case GS_REG_UV:
    {
        m_curU = static_cast<uint16_t>(value & 0x3FFFu);
        m_curV = static_cast<uint16_t>((value >> 16) & 0x3FFFu);
        break;
    }
    case GS_REG_XYZF2:
    case GS_REG_XYZF3:
    {
        GSVertex &vtx = m_vtxQueue[m_vtxCount % kMaxVerts];
        vtx.x = static_cast<float>(value & 0xFFFF) / 16.0f;
        vtx.y = static_cast<float>((value >> 16) & 0xFFFF) / 16.0f;
        vtx.z = static_cast<double>((value >> 32) & 0xFFFFFF);
        vtx.fog = static_cast<uint8_t>((value >> 56) & 0xFF);
        vtx.r = m_curR;
        vtx.g = m_curG;
        vtx.b = m_curB;
        vtx.a = m_curA;
        vtx.q = m_curQ;
        vtx.s = m_curS;
        vtx.t = m_curT;
        vtx.u = m_curU;
        vtx.v = m_curV;
        vertexKick(regAddr == GS_REG_XYZF2);
        break;
    }
    case GS_REG_XYZ2:
    case GS_REG_XYZ3:
    {
        GSVertex &vtx = m_vtxQueue[m_vtxCount % kMaxVerts];
        vtx.x = static_cast<float>(value & 0xFFFF) / 16.0f;
        vtx.y = static_cast<float>((value >> 16) & 0xFFFF) / 16.0f;
        vtx.z = static_cast<double>((value >> 32) & 0xFFFFFFFF);
        vtx.r = m_curR;
        vtx.g = m_curG;
        vtx.b = m_curB;
        vtx.a = m_curA;
        vtx.q = m_curQ;
        vtx.s = m_curS;
        vtx.t = m_curT;
        vtx.u = m_curU;
        vtx.v = m_curV;
        vtx.fog = m_curFog;
        vertexKick(regAddr == GS_REG_XYZ2);
        break;
    }
    case GS_REG_TEX0_1:
    case GS_REG_TEX0_2:
    {
        int ci = (regAddr == GS_REG_TEX0_2) ? 1 : 0;
        auto &t = m_ctx[ci].tex0;
        t.tbp0 = static_cast<uint32_t>(value & 0x3FFF);
        t.tbw = static_cast<uint8_t>((value >> 14) & 0x3F);
        t.psm = static_cast<uint8_t>((value >> 20) & 0x3F);
        t.tw = static_cast<uint8_t>((value >> 26) & 0xF);
        t.th = static_cast<uint8_t>((value >> 30) & 0xF);
        t.tcc = static_cast<uint8_t>((value >> 34) & 0x1);
        t.tfx = static_cast<uint8_t>((value >> 35) & 0x3);
        t.cbp = static_cast<uint32_t>((value >> 37) & 0x3FFF);
        t.cpsm = static_cast<uint8_t>((value >> 51) & 0xF);
        t.csm = static_cast<uint8_t>((value >> 55) & 0x1);
        t.csa = static_cast<uint8_t>((value >> 56) & 0x1F);
        t.cld = static_cast<uint8_t>((value >> 61) & 0x7);
        if (!m_rawGif)
            m_backend->LoadClut(t, m_texclut);
        break;
    }
    case GS_REG_CLAMP_1:
    case GS_REG_CLAMP_2:
    {
        int ci = (regAddr == GS_REG_CLAMP_2) ? 1 : 0;
        m_ctx[ci].clamp = value;
        break;
    }
    case GS_REG_FOG:
        m_curFog = static_cast<uint8_t>((value >> 56) & 0xFF);
        break;
    case GS_REG_TEX1_1:
    case GS_REG_TEX1_2:
    {
        int ci = (regAddr == GS_REG_TEX1_2) ? 1 : 0;
        m_ctx[ci].tex1 = value;
        break;
    }
    case GS_REG_TEX2_1:
    case GS_REG_TEX2_2:
    {
        int ci = (regAddr == GS_REG_TEX2_2) ? 1 : 0;
        auto &t = m_ctx[ci].tex0;
        t.psm = static_cast<uint8_t>((value >> 20) & 0x3F);
        t.cbp = static_cast<uint32_t>((value >> 37) & 0x3FFF);
        t.cpsm = static_cast<uint8_t>((value >> 51) & 0xF);
        t.csm = static_cast<uint8_t>((value >> 55) & 0x1);
        t.csa = static_cast<uint8_t>((value >> 56) & 0x1F);
        t.cld = static_cast<uint8_t>((value >> 61) & 0x7);
        if (!m_rawGif)
            m_backend->LoadClut(t, m_texclut);
        break;
    }
    case GS_REG_XYOFFSET_1:
    case GS_REG_XYOFFSET_2:
    {
        int ci = (regAddr == GS_REG_XYOFFSET_2) ? 1 : 0;
        m_ctx[ci].xyoffset.ofx = static_cast<uint16_t>(value & 0xFFFF);
        m_ctx[ci].xyoffset.ofy = static_cast<uint16_t>((value >> 32) & 0xFFFF);
        break;
    }
    case GS_REG_PRMODECONT:
    {
        m_prmodecont = (value & 1) != 0;
        const GSPrimType type = m_primRegister.type;
        m_prim = m_prmodecont ? m_primRegister : m_prmodeRegister;
        m_prim.type = type;
        break;
    }
    case GS_REG_PRMODE:
    {
        m_prmodeRegister = decodePrimRegister(value);
        if (!m_prmodecont)
        {
            const GSPrimType type = m_primRegister.type;
            m_prim = m_prmodeRegister;
            m_prim.type = type;
        }
        break;
    }
    case GS_REG_TEXCLUT:
        m_texclut.cbw = static_cast<uint8_t>(value & 0x3Fu);
        m_texclut.cou = static_cast<uint8_t>((value >> 6) & 0x3Fu);
        m_texclut.cov = static_cast<uint16_t>((value >> 12) & 0x3FFu);
        break;
    case GS_REG_SCISSOR_1:
    case GS_REG_SCISSOR_2:
    {
        int ci = (regAddr == GS_REG_SCISSOR_2) ? 1 : 0;
        m_ctx[ci].scissor.x0 = static_cast<uint16_t>(value & 0x7FF);
        m_ctx[ci].scissor.x1 = static_cast<uint16_t>((value >> 16) & 0x7FF);
        m_ctx[ci].scissor.y0 = static_cast<uint16_t>((value >> 32) & 0x7FF);
        m_ctx[ci].scissor.y1 = static_cast<uint16_t>((value >> 48) & 0x7FF);
        break;
    }
    case GS_REG_ALPHA_1:
    case GS_REG_ALPHA_2:
    {
        int ci = (regAddr == GS_REG_ALPHA_2) ? 1 : 0;
        m_ctx[ci].alpha = value;
        break;
    }
    case GS_REG_TEST_1:
    case GS_REG_TEST_2:
    {
        int ci = (regAddr == GS_REG_TEST_2) ? 1 : 0;
        m_ctx[ci].test = value;
        break;
    }
    case GS_REG_FRAME_1:
    case GS_REG_FRAME_2:
    {
        int ci = (regAddr == GS_REG_FRAME_2) ? 1 : 0;
        if (m_ctx[ci].frame.fbp != static_cast<uint32_t>(value & 0x1FF))
            ps2x::asyncvif::traceEvent('C', static_cast<uint32_t>(value & 0x1FF) | (static_cast<uint32_t>(ci) << 12u)); // render target
        m_ctx[ci].frame.fbp = static_cast<uint32_t>(value & 0x1FF);
        m_ctx[ci].frame.fbw = static_cast<uint32_t>((value >> 16) & 0x3F);
        m_ctx[ci].frame.psm = static_cast<uint8_t>((value >> 24) & 0x3F);
        m_ctx[ci].frame.fbmsk = static_cast<uint32_t>((value >> 32) & 0xFFFFFFFF);
        break;
    }
    case GS_REG_ZBUF_1:
    case GS_REG_ZBUF_2:
    {
        int ci = (regAddr == GS_REG_ZBUF_2) ? 1 : 0;
        m_ctx[ci].zbuf.zbp = value & 0x1FF;
        m_ctx[ci].zbuf.psm = ((value >> 24) & 0xF) | 0x30;
        m_ctx[ci].zbuf.zmask = (value >> 32) & 1;
        break;
    }
    case GS_REG_FBA_1:
    case GS_REG_FBA_2:
    {
        int ci = (regAddr == GS_REG_FBA_2) ? 1 : 0;
        m_ctx[ci].fba = value;
        break;
    }
    case GS_REG_BITBLTBUF:
    {
        m_bitbltbuf.sbp = static_cast<uint32_t>(value & 0x3FFF);
        m_bitbltbuf.sbw = static_cast<uint8_t>((value >> 16) & 0x3F);
        m_bitbltbuf.spsm = static_cast<uint8_t>((value >> 24) & 0x3F);
        m_bitbltbuf.dbp = static_cast<uint32_t>((value >> 32) & 0x3FFF);
        m_bitbltbuf.dbw = static_cast<uint8_t>((value >> 48) & 0x3F);
        m_bitbltbuf.dpsm = static_cast<uint8_t>((value >> 56) & 0x3F);
        break;
    }
    case GS_REG_TRXPOS:
    {
        m_trxpos.ssax = static_cast<uint16_t>(value & 0x7FF);
        m_trxpos.ssay = static_cast<uint16_t>((value >> 16) & 0x7FF);
        m_trxpos.dsax = static_cast<uint16_t>((value >> 32) & 0x7FF);
        m_trxpos.dsay = static_cast<uint16_t>((value >> 48) & 0x7FF);
        m_trxpos.dir = static_cast<uint8_t>((value >> 59) & 0x3);
        break;
    }
    case GS_REG_TRXREG:
    {
        m_trxreg.rrw = static_cast<uint16_t>(value & 0xFFF);
        m_trxreg.rrh = static_cast<uint16_t>((value >> 32) & 0xFFF);
        break;
    }
    case GS_REG_TRXDIR:
    {
        m_trxdir = static_cast<uint32_t>(value & 0x3);
        if (std::FILE *trace = gsTraceFile(m_privRegs))
            traceTransfer(trace, m_bitbltbuf, m_trxpos, m_trxreg, m_trxdir);
        // PS2X_GS_DOWNLOAD_LOG=1: log every local->host (TRXDIR=1) transfer with the field it happened in.
        static const bool downloadLog = [] { const char *v = std::getenv("PS2X_GS_DOWNLOAD_LOG"); return v && *v == '1'; }();
        if (m_trxdir == 1u)
            g_gsDownloadSerial.fetch_add(1u, std::memory_order_acq_rel);
        if (downloadLog && m_trxdir == 1u)
            std::fprintf(stderr, "[gs-download] tick=%llu sbp=0x%x sbw=%u spsm=0x%x pos=%u,%u size=%ux%u\n",
                         static_cast<unsigned long long>(m_privRegs ? m_privRegs->vsyncTick.load() : 0ull),
                         m_bitbltbuf.sbp, m_bitbltbuf.sbw, m_bitbltbuf.spsm, m_trxpos.ssax, m_trxpos.ssay,
                         m_trxreg.rrw, m_trxreg.rrh);

        if (m_backend)
        {
            GSTransferCommand command{};
            command.bitbltbuf = m_bitbltbuf;
            command.trxpos = m_trxpos;
            command.trxreg = m_trxreg;
            command.direction = m_trxdir;
            m_backend->BeginTransfer(command);
        }
        recordTransferDebugEventUnlocked();
        break;
    }
    case GS_REG_HWREG:
    {
        uint8_t buf[8];
        std::memcpy(buf, &value, 8);
        processImageData(buf, 8);
        break;
    }
    case GS_REG_PABE:
        m_pabe = (value & 1u) != 0u;
        break;
    case GS_REG_FOGCOL:
        m_fogR = static_cast<uint8_t>(value & 0xFFu);
        m_fogG = static_cast<uint8_t>((value >> 8) & 0xFFu);
        m_fogB = static_cast<uint8_t>((value >> 16) & 0xFFu);
        break;
    case GS_REG_TEXFLUSH:
        if (m_backend && !m_rawGif)
            m_backend->TextureFlush();
        break;
    case GS_REG_SCANMSK:
        m_scanmsk = value;
        break;
    case GS_REG_DIMX:
        m_dimx = value;
        break;
    case GS_REG_DTHE:
        m_dthe = value;
        break;
    case GS_REG_COLCLAMP:
        m_colclamp = value;
        break;
    case GS_REG_MIPTBP1_1:
    case GS_REG_MIPTBP1_2:
    {
        const int ci = (regAddr == GS_REG_MIPTBP1_2) ? 1 : 0;
        m_ctx[ci].miptbp1 = value;
        break;
    }
    case GS_REG_MIPTBP2_1:
    case GS_REG_MIPTBP2_2:
    {
        const int ci = (regAddr == GS_REG_MIPTBP2_2) ? 1 : 0;
        m_ctx[ci].miptbp2 = value;
        break;
    }
    case GS_REG_TEXA:
    {
        m_texa.ta0 = static_cast<uint8_t>(value & 0xFFu);
        m_texa.aem = ((value >> 15) & 0x1u) != 0u;
        m_texa.ta1 = static_cast<uint8_t>((value >> 32) & 0xFFu);
        PS2_IF_AGRESSIVE_LOGS({
            const uint32_t texaIndex = s_debugTexaWriteCount.fetch_add(1u, std::memory_order_relaxed);
            if (texaIndex < 24u)
            {
                RUNTIME_LOG("[gs:texa] idx=" << texaIndex
                                             << " value=0x" << std::hex << value
                                             << " ta0=0x" << ((value >> 0) & 0xFFu)
                                             << " aem=" << ((value >> 15) & 0x1u)
                                             << " ta1=0x" << ((value >> 32) & 0xFFu)
                                             << std::dec
                                             << std::endl);
            }
        });
        break;
    }
    case GS_REG_SIGNAL:
    {
        if (!t_stallReplaying)
            ps2x::asyncvif::traceEvent('J', static_cast<uint32_t>(g_gsTracePath));
        if (gsSignalLogEnabled() && m_privRegs)
            gsSignalLog("SIGNAL", value, m_privRegs->csr.load());
        if (m_backend)
        {
            m_backend->Flush();
            m_backend->Sync(GSSyncReason::Finish);
        }
        if (m_privRegs)
            ps2xGsSignal(*m_privRegs, static_cast<uint32_t>(value & 0xFFFFFFFF), static_cast<uint32_t>(value >> 32));
        break;
    }
    case GS_REG_FINISH:
    {
        ps2x::asyncvif::traceEvent('M', static_cast<uint32_t>(g_gsTracePath));
        if (gsSignalLogEnabled() && m_privRegs)
            gsSignalLog("FINISH", value, m_privRegs->csr.load());
        if (m_backend)
        {
            m_backend->Flush();
            m_backend->Sync(GSSyncReason::Finish);
        }
        if (m_privRegs)
            ps2xGsFinish(*m_privRegs);
        break;
    }
    case GS_REG_LABEL:
    {
        if (gsSignalLogEnabled() && m_privRegs)
            gsSignalLog("LABEL", value, m_privRegs->csr.load());
        if (m_backend)
        {
            m_backend->Flush();
            m_backend->Sync(GSSyncReason::Finish);
        }
        if (m_privRegs)
        {
            uint32_t id = static_cast<uint32_t>(value & 0xFFFFFFFF);
            uint32_t mask = static_cast<uint32_t>(value >> 32);
            uint32_t hi = static_cast<uint32_t>(m_privRegs->siglblid >> 32);
            hi = (hi & ~mask) | (id & mask);
            m_privRegs->siglblid = (static_cast<uint64_t>(hi) << 32) | (m_privRegs->siglblid & 0xFFFFFFFF);
        }
        break;
    }
    case 0x59:
        if (m_privRegs)
            m_privRegs->dispfb1 = value;
        break;
    case 0x5a:
        if (m_privRegs)
            m_privRegs->display1 = value;
        break;
    case 0x5b:
        if (m_privRegs)
            m_privRegs->dispfb2 = value;
        break;
    case 0x5c:
        if (m_privRegs)
            m_privRegs->display2 = value;
        break;
    case 0x5f:
        if (m_privRegs)
            m_privRegs->bgcolor = value;
        break;
    default:
        break;
    }

    recordRegisterDebugEventUnlocked(regAddr, value);
}

void GS::vertexKick(bool drawing)
{
    ++m_vtxCount;
    ++m_vtxIndex;

    PS2_IF_AGRESSIVE_LOGS({
        const uint32_t debugIndex = s_debugGsVertexKickCount.fetch_add(1, std::memory_order_relaxed);
        if (debugIndex < 96u)
        {
            RUNTIME_LOG("[gs:kick] idx=" << debugIndex
                                         << " drawing=" << static_cast<uint32_t>(drawing ? 1u : 0u)
                                         << " prim=" << static_cast<uint32_t>(m_prim.type)
                                         << " vtxCount=" << m_vtxCount
                                         << std::endl);
        }
    });

    int needed = 0;
    switch (m_prim.type)
    {
    case GS_PRIM_POINT:
        needed = 1;
        break;
    case GS_PRIM_LINE:
        needed = 2;
        break;
    case GS_PRIM_LINESTRIP:
        needed = 2;
        break;
    case GS_PRIM_TRIANGLE:
        needed = 3;
        break;
    case GS_PRIM_TRISTRIP:
        needed = 3;
        break;
    case GS_PRIM_TRIFAN:
        needed = 3;
        break;
    case GS_PRIM_SPRITE:
        needed = 2;
        break;
    default:
        return;
    }

    if (m_vtxCount < needed)
        return;

    if (drawing && m_backend && !m_rawGif)
    {
        GSPrimitiveBatch batch = buildDrawBatch(needed);
        updatePreferredDisplaySourceForDraw(batch);
        m_backend->Submit(batch);
        if (gsTrace().file && gsTrace().lastTick == UINT64_MAX)
        {
            m_backend->Flush();
            m_backend->Sync(GSSyncReason::DebugReadback);
        }
        if (std::FILE *trace = gsTraceFile(m_privRegs, m_localMemoryStorage, m_localMemorySize))
        {
            traceDraw(trace, batch);
            static uint32_t tracedDraws = 0u;
            static const long dumpDraw = std::getenv("PS2X_GS_TRACE_DUMP_DRAW") ? std::atol(std::getenv("PS2X_GS_TRACE_DUMP_DRAW")) : -1;
            if (static_cast<long>(tracedDraws++) == dumpDraw)
            {
                m_backend->Flush();
                m_backend->Sync(GSSyncReason::DebugReadback);
                if (std::FILE *raw = std::fopen((gsTrace().vramPath + ".draw").c_str(), "wb"))
                {
                    std::fwrite(m_localMemoryStorage, 1, m_localMemorySize, raw);
                    std::fclose(raw);
                }
            }
        }
        recordDrawDebugEventUnlocked(needed);
    }

    switch (m_prim.type)
    {
    case GS_PRIM_LINE:
    case GS_PRIM_TRIANGLE:
    case GS_PRIM_SPRITE:
    case GS_PRIM_POINT:
        m_vtxCount = 0;
        break;
    case GS_PRIM_LINESTRIP:
        m_vtxQueue[0] = m_vtxQueue[1];
        m_vtxCount = 1;
        break;
    case GS_PRIM_TRISTRIP:
        m_vtxQueue[0] = m_vtxQueue[1];
        m_vtxQueue[1] = m_vtxQueue[2];
        m_vtxCount = 2;
        break;
    case GS_PRIM_TRIFAN:
        m_vtxQueue[1] = m_vtxQueue[2];
        m_vtxCount = 2;
        break;
    default:
        m_vtxCount = 0;
        break;
    }
}

void GS::processImageData(const uint8_t *data, uint32_t sizeBytes)
{
    if (std::FILE *trace = gsTraceFile(m_privRegs))
    {
        std::fprintf(trace, "I %u ", sizeBytes);
        for (uint32_t i = 0; i < sizeBytes; ++i)
            std::fprintf(trace, "%02x", data[i]);
        std::fputc(0x0A, trace);
    }
    if (m_backend && !m_rawGif)
        m_backend->UploadImage(data, sizeBytes);
}


bool GS::clearFramebufferContext(uint32_t contextIndex, uint32_t rgba)
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    return m_backend && m_backend->ClearFramebuffer(m_ctx[(contextIndex != 0u) ? 1 : 0], rgba);
}

bool GS::clearActiveFramebuffer(uint32_t rgba)
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    return m_backend && m_backend->ClearFramebuffer(activeContext(), rgba);
}

uint32_t GS::consumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes)
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    return m_backend ? m_backend->ConsumeLocalToHostBytes(dst, maxBytes) : 0u;
}

void GS::setRasterBackend(std::unique_ptr<GSRasterBackend> backend)
{
    if (!backend)
        backend = GSThreadedBackend::MakeDefault();

    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    std::lock_guard<std::mutex> backendLock(m_backendLifetimeMutex);
    if (m_backend)
    {
        m_backend->Flush();
        m_backend->Sync(GSSyncReason::Reset);

        // The external 4 MiB GS allocation is the backend hand-off format.
        // This keeps hot backend replacement deterministic even when a future
        // GPU backend keeps a private/mirrored local-memory representation.
        if (m_localMemoryStorage && m_localMemorySize != 0u)
        {
            std::vector<uint8_t> localMemory;
            m_backend->SnapshotVram(localMemory);
            const size_t bytes = std::min<size_t>(localMemory.size(), m_localMemorySize);
            if (bytes != 0u)
                std::memcpy(m_localMemoryStorage, localMemory.data(), bytes);
        }
    }

    m_backend = std::move(backend);
    m_rawGif = m_backend->WantsRawGif();
    m_backend->Initialize(m_localMemoryStorage, m_localMemorySize);
}

uint32_t GS::ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    return m_backend ? m_backend->ReadVram(psm, base, bw, x, y) : 0u;
}

void GS::WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value)
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    if (m_backend)
        m_backend->WriteVram(psm, base, bw, x, y, value);
}

GSPrimitiveBatch GS::buildDrawBatch(int vertexCount) const
{
    GSPrimitiveBatch batch{};
    batch.vertexCount = static_cast<uint8_t>(std::min(vertexCount, 3));
    for (int i = 0; i < batch.vertexCount; ++i)
        batch.vertices[static_cast<size_t>(i)] = m_vtxQueue[i];
    batch.state.context = m_ctx[m_prim.ctxt ? 1 : 0];
    batch.state.prim = m_prim;
    batch.state.texa = m_texa;
    batch.state.texclut = m_texclut;
    batch.state.pabe = m_pabe;
    batch.state.scanmsk = m_scanmsk;
    batch.state.dimx = m_dimx;
    batch.state.dthe = m_dthe;
    batch.state.colclamp = m_colclamp;
    batch.state.fogR = m_fogR;
    batch.state.fogG = m_fogG;
    batch.state.fogB = m_fogB;
    batch.state.textureWidth = static_cast<uint16_t>(1u << std::min<uint32_t>(batch.state.context.tex0.tw, 10u));
    batch.state.textureHeight = static_cast<uint16_t>(1u << std::min<uint32_t>(batch.state.context.tex0.th, 10u));
    const uint64_t tex1 = batch.state.context.tex1;
    const uint8_t mmag = static_cast<uint8_t>((tex1 >> 5u) & 0x1u);
    const uint8_t mmin = static_cast<uint8_t>((tex1 >> 6u) & 0x7u);
    batch.state.linearFilter = mmag != 0u || mmin == 1u || (mmin & 0x4u) != 0u;
    return batch;
}

void GS::updatePreferredDisplaySourceForDraw(const GSPrimitiveBatch &batch)
{
    const GSDrawState &state = batch.state;
    const GSContext &ctx = state.context;
    if (m_hasPreferredDisplaySource && ctx.frame.fbp == m_preferredDisplayDestFbp)
        m_hasPreferredDisplaySource = false;
    if (state.prim.type != GS_PRIM_SPRITE || batch.vertexCount < 2u)
        return;

    const GSVertex &v0 = batch.vertices[0];
    const GSVertex &v1 = batch.vertices[1];
    int x0 = static_cast<int>(v0.x) - (ctx.xyoffset.ofx >> 4);
    int y0 = static_cast<int>(v0.y) - (ctx.xyoffset.ofy >> 4);
    int x1 = static_cast<int>(v1.x) - (ctx.xyoffset.ofx >> 4);
    int y1 = static_cast<int>(v1.y) - (ctx.xyoffset.ofy >> 4);
    if (x0 > x1) std::swap(x0, x1);
    if (y0 > y1) std::swap(y0, y1);
    const int xEnd = x0 + std::max(1, x1 - x0) - 1;
    const int yEnd = y0 + std::max(1, y1 - y0) - 1;
    const uint8_t alphaMode = static_cast<uint8_t>(ctx.alpha & 0xFFu);
    const uint8_t alphaFix = static_cast<uint8_t>((ctx.alpha >> 32u) & 0xFFu);
    const bool displayCopy = state.prim.tme && state.prim.abe && state.prim.fst && state.prim.ctxt &&
                             ctx.frame.fbp != ctx.tex0.tbp0 && alphaMode == 0x64u &&
                             (alphaFix == 0x60u || alphaFix == 0x80u) &&
                             x0 <= 0 && y0 <= 0 && xEnd >= 639 && yEnd >= 447;
    if (displayCopy)
    {
        m_preferredDisplaySourceFrame = {ctx.tex0.tbp0, ctx.tex0.tbw, ctx.tex0.psm, 0u};
        m_preferredDisplayDestFbp = ctx.frame.fbp;
        m_hasPreferredDisplaySource = true;
    }
}

// Save states (ps2_save_state.cpp): GIF data deferred at a stalled SIGNAL is not part of the state, so a save waits
// until no stall is pending, and a load drops whatever the running session had deferred.
bool ps2xGsStallActive()
{
    if (!gsSignalStallEnabled())
        return false;
    std::lock_guard<std::mutex> stallLock(g_stallMutex);
    return g_stall.active || !g_stall.queue.empty();
}

void ps2xGsResetStall()
{
    std::lock_guard<std::mutex> stallLock(g_stallMutex);
    g_stall.active = false;
    g_stall.queue.clear();
    // A tag cut by the stall (raw GIF back end) or by a transfer boundary belongs to the session before the load: the
    // next packet starts with a GIFtag (PCSX2's GS clears its paths in load_vram too).
    g_gifCont.active = false;
}

// True when GIF data is deferred at a SIGNAL that the EE has already acknowledged (CSR.SIGNAL clear) and nobody is
// replaying it: a release that got lost between threads. The MTVU GIF stage polls it as a safety net.
bool ps2xGsStallNeedsRelease()
{
    if (!gsSignalStallEnabled())
        return false;
    std::lock_guard<std::mutex> stallLock(g_stallMutex);
    return g_stall.active && !g_stall.queue.empty() && g_stall.regs && !ps2xGsSignalBlocked(*g_stall.regs);
}

// Called (from ps2_memory.cpp) after the EE clears CSR.SIGNAL: replays deferred GIF data until a SIGNAL stalls again.
void ps2xGsReleaseStall()
{
    if (!gsSignalStallEnabled() || t_stallReplaying)
        return;
    // One replaying thread at a time: the EE (CSR write) and the GIF worker (SIGNAL already acknowledged when it
    // stalled) may both get here now that CSR writes no longer wait for the workers. Whoever holds the flag drains the
    // queue until it is empty or a SIGNAL stalls again.
    // A thread that finds the flag taken asks the holder to run once more (s_retry), so a CSR clear that races with
    // the end of a replay is never lost.
    static std::atomic<bool> s_replaying{false};
    static std::atomic<bool> s_retry{false};
    if (s_replaying.exchange(true, std::memory_order_acq_rel))
    {
        s_retry.store(true, std::memory_order_release);
        return;
    }
    auto drain = []()
    {
        for (;;)
        {
            std::vector<uint8_t> packet;
            GS *gs = nullptr;
            {
                std::lock_guard<std::mutex> stallLock(g_stallMutex);
                if (g_stall.queue.empty())
                {
                    g_stall.active = false;
                    return;
                }
                gs = g_stall.owner;
                if (!gs)
                    return;
                packet = std::move(g_stall.queue.front());
                g_stall.queue.pop_front();
                if (gsSignalTraceAll())
                    std::fprintf(stderr, "[gs-sig2] replay %zu bytes (%zu left)\n", packet.size(), g_stall.queue.size());
            }
            t_stallReplaying = true;
            t_stalledAgain = false;
            gs->processGIFPacket(packet.data(), static_cast<uint32_t>(packet.size()));
            t_stallReplaying = false;
            if (t_stalledAgain)
                return; // a SIGNAL met a pending CSR.SIGNAL again; its remainder is at the front of the queue
        }
    };
    for (;;)
    {
        s_retry.store(false, std::memory_order_release);
        drain();
        s_replaying.store(false, std::memory_order_release);
        if (!s_retry.load(std::memory_order_acquire) || s_replaying.exchange(true, std::memory_order_acq_rel))
            break;
    }
}
