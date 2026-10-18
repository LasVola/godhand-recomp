#include "iop_rpc.h"

#include <cstdio>
#include <cstdlib>

#include "../core/iop_cpu.h"
#include "../core/iop_kernel.h"
#include "../core/iop_memory.h"
#include "ps2x/iop/iop_host.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <vector>

namespace ps2x::iop::detail
{
    namespace
    {
        constexpr uint32_t kDefaultServerPriority = 0x40u;
    }

    IopRpcBridge::IopRpcBridge(IopHost &host, IopMemory &memory, IopKernel &kernel) noexcept
        : m_host(host), m_memory(memory), m_kernel(kernel)
    {
    }

    void IopRpcBridge::reset()
    {
        m_servers.clear();
        m_nextDmaId = 1u;
        m_sifInitialized = false;
        m_dmaCallback.reset();
    }

    uint32_t IopRpcBridge::setDma(uint32_t descriptorAddress, uint32_t descriptorCount, uint32_t &bytes)
    {
        constexpr uint32_t kDescriptorSize = 16u;
        constexpr uint32_t kMaxDescriptors = 32u;
        bytes = 0u;
        if (descriptorAddress == 0u || descriptorCount == 0u || descriptorCount > kMaxDescriptors)
            return 0u;

        struct PendingTransfer
        {
            uint32_t source = 0u;
            uint32_t destination = 0u;
            uint32_t size = 0u;
        };

        std::array<uint32_t, kMaxDescriptors * 4u> descriptorWords{};
        const size_t descriptorBytes = static_cast<size_t>(descriptorCount) * kDescriptorSize;
        if (!m_memory.readRam(descriptorAddress, descriptorWords.data(), descriptorBytes))
            return 0u;

        std::array<PendingTransfer, kMaxDescriptors> pending{};
        uint32_t pendingCount = 0u;
        uint32_t largestTransfer = 0u;
        for (uint32_t i = 0u; i < descriptorCount; ++i)
        {
            const uint32_t source = descriptorWords[i * 4u + 0u];
            const uint32_t destination = descriptorWords[i * 4u + 1u];
            const int32_t signedSize = static_cast<int32_t>(descriptorWords[i * 4u + 2u]);
            if (signedSize <= 0)
                continue;

            const uint32_t size = static_cast<uint32_t>(signedSize);
            if (!m_memory.ownsRamRange(source, size))
                return 0u;
            pending[pendingCount++] = {source, destination, size};
            largestTransfer = std::max(largestTransfer, size);
            static const bool logDma = []
            {
                const char *value = std::getenv("PS2X_SIF_DMA_LOG");
                return value && *value && *value != '0';
            }();
            if (logDma)
            {
                // PS2X_SIF_DMA_LOG=1 logs every IOP->EE sceSifSetDma; =2 adds the first 128 bytes.
                char line[512];
                int len = std::snprintf(line, sizeof(line), "[sif-dma] iop 0x%06x -> ee 0x%08x size=%u", source, destination, size);
                static const bool withData = std::getenv("PS2X_SIF_DMA_LOG")[0] == '2';
                for (uint32_t b = 0u; withData && b < std::min<uint32_t>(size, 128u) && len < 500; ++b)
                    len += std::snprintf(line + len, sizeof(line) - len, "%s%02x", (b % 16u) == 0u ? " |" : " ", m_memory.read8(source + b));
                std::fprintf(stderr, "%s\n", line);
            }
        }

        std::vector<uint8_t> scratch(largestTransfer);
        for (uint32_t i = 0u; i < pendingCount; ++i)
        {
            if (!m_host.readGuest(pending[i].destination, scratch.data(), pending[i].size))
                return 0u;
        }

        for (uint32_t i = 0u; i < pendingCount; ++i)
        {
            const PendingTransfer &transfer = pending[i];
            if (!m_memory.readRam(transfer.source, scratch.data(), transfer.size) ||
                !m_host.writeGuest(transfer.destination, scratch.data(), transfer.size))
                return 0u;
            bytes += transfer.size;
        }

        const uint32_t dmaId = m_nextDmaId++;
        if (m_nextDmaId == 0u || m_nextDmaId > static_cast<uint32_t>(std::numeric_limits<int32_t>::max()))
            m_nextDmaId = 1u;
        return dmaId;
    }

    std::optional<IopRpcBridge::DmaCallback> IopRpcBridge::takeDmaCallback() noexcept
    {
        std::optional<DmaCallback> result = m_dmaCallback;
        m_dmaCallback.reset();
        return result;
    }

    bool IopRpcBridge::dispatchSifManImport(uint16_t ordinal, IopCpuState &cpu)
    {
        const auto setV0 = [&](uint32_t value)
        {
            cpu.gpr[2] = value;
        };
        switch (ordinal)
        {
        case 4: // sceSifDma2Init
        case 5: // sceSifInit
            m_sifInitialized = true;
            setV0(0u);
            return true;
        case 7: // sceSifSetDma
        {
            uint32_t bytes = 0u;
            setV0(setDma(cpu.gpr[4], cpu.gpr[5], bytes));
            return true;
        }
        case 8: // sceSifDmaStat
            setV0(0xFFFFFFFFu);
            return true;
        case 29: // sceSifCheckInit
            setV0(m_sifInitialized ? 1u : 0u);
            return true;
        case 32:
        {
            uint32_t bytes = 0u;
            const uint32_t dmaId = setDma(cpu.gpr[4], cpu.gpr[5], bytes);
            if (dmaId != 0u && cpu.gpr[6] != 0u)
                m_dmaCallback = DmaCallback{cpu.gpr[6], cpu.gpr[7], cpu.gpr[28], bytes};
            setV0(dmaId);
            return true;
        }
        default:
            setV0(0u);
            return true;
        }
    }

    bool IopRpcBridge::dispatchSifCmdImport(uint16_t ordinal, IopCpuState &cpu)
    {
        const auto setV0 = [&](uint32_t value)
        {
            cpu.gpr[2] = value;
        };
        switch (ordinal)
        {
        case 4: // InitCmd
        case 5:
        case 6:
        case 7:
        case 8:
        case 9:
        case 10:
        case 11:
        case 14: // InitRpc
            setV0(0);
            return true;
        case 15: // sceSifBindRpc(cd, sid, mode): bind an IOP client to a server the EE registered
        {
            // Sony MCMAN mcman export 62 (0x42F0, called by MCSERV for its init fno 0xFE) binds cd 0x169D8 to the EE
            // libmc server 0x80000400; its fast paths then call it (0x433C fno 2, 0x4CB4 fno 1). Until now this was a
            // stub that left cd->server 0. SifRpcClientData_t: hdr 0x00-0x0F, command 0x10, buff 0x14, cbuff 0x18,
            // end_function 0x1C, end_param 0x20, server 0x24. A missing server leaves cd->server 0 and returns 0 (the
            // SDK's "not yet registered" answer; callers retry).
            const uint32_t cd = cpu.gpr[4];
            const uint32_t sid = cpu.gpr[5];
            uint32_t serverData = 0u, buffer = 0u, callbackBuffer = 0u;
            const bool found = m_host.bindEeRpcServer(sid, serverData, buffer, callbackBuffer);
            const bool writable = cd != 0u && m_memory.ownsRamRange(cd, 0x28u);
            if (writable)
            {
                m_memory.write32(cd + 0x10u, 0u);
                m_memory.write32(cd + 0x14u, found ? buffer : 0u);
                m_memory.write32(cd + 0x18u, found ? callbackBuffer : 0u);
                m_memory.write32(cd + 0x24u, found ? serverData : 0u);
            }
            char line[224];
            std::snprintf(line, sizeof(line), "[IOP] sceSifBindRpc cd=0x%06x sid=0x%08x -> EE server 0x%08x buf 0x%08x%s%s", cd, sid,
                          serverData, buffer, found ? "" : " (no EE server registered)",
                          writable ? "" : " (cd outside owned IOP RAM: not written)");
            m_host.log(found && writable ? LogLevel::Info : LogLevel::Warning, line);
            setV0(0);
            return true;
        }
        case 16: // sceSifCallRpc(cd, fno, mode, send, ssize, recv, rsize, end_func, end_param) to an EE server
        {
            const uint32_t cd = cpu.gpr[4];
            const uint32_t function = cpu.gpr[5];
            const uint32_t mode = cpu.gpr[6];
            const uint32_t send = cpu.gpr[7];
            const uint32_t stackPointer = cpu.gpr[29];
            const uint32_t sendSize = m_memory.read32(stackPointer + 16u);
            const uint32_t receive = m_memory.read32(stackPointer + 20u);
            const uint32_t receiveSize = m_memory.read32(stackPointer + 24u);
            const uint32_t endFunction = m_memory.read32(stackPointer + 28u);
            const uint32_t serverData = (cd != 0u && m_memory.ownsRamRange(cd, 0x28u)) ? m_memory.read32(cd + 0x24u) : 0u;
            const bool wait = (mode & 0x01u) == 0u;
            const auto fail = [&](const char *why)
            {
                char line[224];
                std::snprintf(line, sizeof(line), "[IOP] sceSifCallRpc cd=0x%06x fno=%u to EE server 0x%08x failed: %s", cd, function,
                              serverData, why);
                m_host.log(LogLevel::Warning, line);
                setV0(static_cast<uint32_t>(-1));
                return true;
            };
            if (serverData == 0u)
                return fail("client not bound");
            if (sendSize > 0x10000u || receiveSize > 0x10000u)
                return fail("size out of range");
            std::vector<uint8_t> payload(sendSize);
            if (sendSize != 0u && !m_memory.readRam(send, payload.data(), payload.size()))
                return fail("send buffer outside IOP RAM");
            int threadId = 0;
            if (wait)
            {
                const IopThread *current = m_kernel.currentThread();
                if (current == nullptr)
                    return fail("mode 0 call outside an IOP thread (cannot block)");
                threadId = current->id;
            }
            if (!wait && endFunction != 0u)
                m_host.log(LogLevel::Warning, "[IOP] sceSifCallRpc NOWAIT to an EE server: IOP end function not run");
            const bool queued = m_host.callEeRpcServer(
                serverData, function, std::move(payload), receiveSize,
                [this, threadId, receive](std::vector<uint8_t> data, bool ok)
                {
                    std::lock_guard<std::mutex> lock(m_eeRpcMutex);
                    m_eeRpcDone.push_back(EeRpcCompletion{threadId, receive, std::move(data), ok});
                });
            if (!queued)
                return fail("EE server function not callable");
            ++m_eeRpcInFlight; // serviceEeRpcCompletions takes it back off (every queued call pushes one completion)
            if (++m_eeRpcCalls <= 16u)
            {
                char line[224];
                std::snprintf(line, sizeof(line), "[IOP] sceSifCallRpc cd=0x%06x fno=%u mode=%u send=%u recv 0x%06x/%u -> EE server 0x%08x (call %u)",
                              cd, function, mode, sendSize, receive, receiveSize, serverData, m_eeRpcCalls);
                m_host.log(LogLevel::Info, line);
            }
            setV0(0);
            if (wait)
                (void)m_kernel.hostWaitCurrent(cpu);
            return true;
        }
        case 12: // sceSifSendCmd
        case 13: // isceSifSendCmd
        {
            constexpr uint32_t kHeaderSize = 16u;
            constexpr uint32_t kMaxPacketSize = 112u;
            const uint32_t commandId = cpu.gpr[4];
            const uint32_t packetAddress = cpu.gpr[5];
            const uint32_t packetSize = cpu.gpr[6];
            const uint32_t extraSource = cpu.gpr[7];
            const uint32_t stackPointer = cpu.gpr[29];
            const uint32_t extraDestination = m_memory.read32(stackPointer + 16u);
            const int32_t signedExtraSize = static_cast<int32_t>(m_memory.read32(stackPointer + 20u));

            if (packetAddress == 0u || packetSize < kHeaderSize || packetSize > kMaxPacketSize ||
                !m_memory.ownsRamRange(packetAddress, packetSize))
            {
                setV0(0u);
                return true;
            }

            std::array<uint8_t, kMaxPacketSize> packet{};
            if (!m_memory.readRam(packetAddress, packet.data(), packetSize))
            {
                setV0(0u);
                return true;
            }

            uint32_t extraSize = 0u;
            if (signedExtraSize > 0)
            {
                extraSize = static_cast<uint32_t>(signedExtraSize);
                if (extraSource == 0u || extraDestination == 0u ||
                    !m_memory.ownsRamRange(extraSource, extraSize) ||
                    !m_host.writeGuest(extraDestination, m_memory.ram().data() + IopMemory::physicalAddress(extraSource), extraSize))
                {
                    setV0(0u);
                    return true;
                }
            }

            const uint32_t sizeWord = packetSize | (extraSize << 8u);
            std::memcpy(packet.data() + 0u, &sizeWord, sizeof(sizeWord));
            std::memcpy(packet.data() + 4u, &extraDestination, sizeof(extraDestination));
            std::memcpy(packet.data() + 8u, &commandId, sizeof(commandId));

            if (!m_host.sendSifCommand(commandId, packet.data(), packetSize))
            {
                // A command without an EE handler is still a completed DMA on  real hardware. Only malformed packets fail above.
            }

            const uint32_t dmaId = m_nextDmaId++;
            if (m_nextDmaId == 0u || m_nextDmaId > static_cast<uint32_t>(std::numeric_limits<int32_t>::max()))
                m_nextDmaId = 1u;
            setV0(dmaId);
            return true;
        }
        case 17: // sceSifRegisterRpc
        {
            RpcServer server;
            server.serverData = cpu.gpr[4];
            server.sid = cpu.gpr[5];
            server.function = cpu.gpr[6];
            server.gp = cpu.gpr[28];
            server.buffer = cpu.gpr[7];
            const uint32_t stackPointer = cpu.gpr[29];
            server.callback = m_memory.read32(stackPointer + 16u);
            server.callbackBuffer = m_memory.read32(stackPointer + 20u);
            server.queue = m_memory.read32(stackPointer + 24u);
            m_servers[server.sid] = server;
            if (server.serverData != 0u)
            {
                m_memory.write32(server.serverData + 0x20u, server.sid);
                m_memory.write32(server.serverData + 0x28u, server.function);
                m_memory.write32(server.serverData + 0x2Cu, server.buffer);
            }
            setV0(server.serverData);
            return true;
        }
        case 18:
            setV0(0);
            return true;
        case 19: // SetRpcQueue
            if (cpu.gpr[4] != 0u && m_memory.ownsRamRange(cpu.gpr[4], 4u))
                m_memory.write32(cpu.gpr[4], cpu.gpr[5]);
            setV0(cpu.gpr[4]);
            return true;
        case 20:
        case 21:
            setV0(0);
            return true;
        case 22: // RpcLoop
            m_kernel.sleepCurrent(cpu);
            setV0(0);
            return true;
        case 23: // sceSifGetOtherData(rd, eeSource, iopDest, size, mode): the IOP pulls data from EE memory
        {
            // MCSERV gets the data of a memory card write this way (libmc sends only the unaligned head and tail in the
            // RPC). This was a stub: every save got the stale IOP buffer, zeros or the icon.sys written just before,
            // and the game reported its saves as corrupted.
            const uint32_t source = cpu.gpr[5];
            const uint32_t dest = cpu.gpr[6];
            const uint32_t size = cpu.gpr[7];
            int32_t result = 0;
            if (size != 0u)
            {
                std::vector<uint8_t> data(size);
                if (!m_host.readGuest(source, data.data(), data.size()) || !m_memory.writeRam(dest, data.data(), data.size()))
                    result = -1;
            }
            setV0(static_cast<uint32_t>(result));
            return true;
        }
        case 24: // RemoveRpc
        {
            const uint32_t serverData = cpu.gpr[4];
            for (auto server = m_servers.begin(); server != m_servers.end(); ++server)
            {
                if (server->second.serverData == serverData)
                {
                    m_servers.erase(server);
                    break;
                }
            }
            setV0(0);
            return true;
        }
        case 25:
        case 26:
        case 27:
        case 28:
        case 29:
            setV0(0);
            return true;
        default:
            return false;
        }
    }

    RpcResult IopRpcBridge::handleRpc(const RpcRequest &request, IopGuestExecutor &executor)
    {
        RpcResult result{};
        const auto serverIt = m_servers.find(request.sid);
        if (serverIt == m_servers.end() || serverIt->second.function == 0u)
            return result;

        RpcServer &server = serverIt->second;
        if (request.send.size != 0u && server.buffer != 0u)
        {
            const uint32_t copySize = std::min<uint32_t>(request.send.size, IopMemory::RamSize - std::min(server.buffer, IopMemory::RamSize));
            if (copySize != 0u)
            {
                std::vector<uint8_t> payload(copySize);
                if (m_host.readGuest(request.send.address, payload.data(), payload.size()))
                    (void)m_memory.writeRam(server.buffer, payload.data(), payload.size());
            }
        }

        uint32_t priority = kDefaultServerPriority;
        if (server.queue != 0u && m_memory.ownsRamRange(server.queue, 4u))
            priority = m_kernel.threadPriority(static_cast<int>(m_memory.read32(server.queue)), kDefaultServerPriority);
        const uint32_t serverBuffer = server.buffer;
        auto copyReceive = [this, receive = request.receive, serverBuffer](uint32_t returnPointer)
        {
            if (returnPointer == 0u)
                returnPointer = serverBuffer;
            if (receive.address == 0u || receive.size == 0u || returnPointer == 0u)
                return;
            const uint32_t physical = IopMemory::physicalAddress(returnPointer);
            if (physical >= IopMemory::RamSize)
                return;
            const uint32_t copySize = std::min<uint32_t>(receive.size, IopMemory::RamSize - physical);
            (void)m_host.writeGuest(receive.address, m_memory.ram().data() + physical, copySize);
            if (copySize < receive.size)
                (void)m_host.zeroGuest(receive.address + copySize, receive.size - copySize);
        };

        // NOWAIT calls (libmc: every memory card operation) may outlive the request: a card write or format takes
        // seconds of IOP time. Running them to the end here froze the EE meanwhile; the IOP then ran ahead of it,
        // the music stream starved and came back distorted and too loud, and input stopped (PROGRESS §55).
        if ((request.mode & 0x01u) != 0u && request.callToken != 0u)
        {
            uint32_t returnPointer = 0u;
            const uint64_t token = request.callToken;
            // PS2X_IOP_NOWAIT_DEFER (default on): the EE does not wait for a NOWAIT call, so the server need not run
            // inside it. On the PS2 the IOP serves it in parallel. Urban Reign's per-frame sound call (sid 0x81000201,
            // ~1 ms of interpreted IOP each) ran on the game thread: 6.3% of it with 8 fighters (PROGRESS §66).
            static const bool defer = [] { const char *v = std::getenv("PS2X_IOP_NOWAIT_DEFER"); return !(v && *v == '0'); }();
            if (defer && executor.startServerFunctionAsync(server.function, request.function, server.buffer, request.send.size,
                                                           0u, server.gp, priority,
                                                           [this, copyReceive, token](uint32_t later)
                                                           {
                                                               copyReceive(later);
                                                               m_host.completeRpc(token);
                                                           }))
            {
                result.handled = true;
                result.pending = true;
                result.resultAddress = request.receive.address;
                result.serverDispatchPolicy = ServerDispatchPolicy::Suppress;
                return result;
            }
            const bool finished = executor.executeServerFunctionAsync(
                server.function, request.function, server.buffer, request.send.size, 0u, server.gp, priority,
                [this, copyReceive, token](uint32_t later)
                {
                    copyReceive(later);
                    m_host.completeRpc(token);
                },
                returnPointer);
            if (!finished)
            {
                result.handled = true;
                result.pending = true;
                result.resultAddress = request.receive.address;
                result.serverDispatchPolicy = ServerDispatchPolicy::Suppress;
                return result;
            }
            copyReceive(returnPointer);
        }
        else
        {
            copyReceive(executor.executeServerFunction(server.function,
                                                       request.function,
                                                       server.buffer,
                                                       request.send.size,
                                                       0u,
                                                       server.gp,
                                                       priority));
        }

        result.handled = true;
        result.resultAddress = request.receive.address;
        result.serverDispatchPolicy = ServerDispatchPolicy::Suppress;
        result.signalNowaitCompletion = true;
        result.signalCompletion = true;
        return result;
    }

    void IopRpcBridge::onSifTransfer(const SifTransfer &transfer)
    {
        // The EE SIF transport owns the actual directional memory movement.
        // Services still receive both phases through IopSubsystem, but mirroring
        // IOP bytes through an equal-numbered EE address would alias two distinct
        // PS2 address spaces and can overwrite live game data.
        (void)transfer;
    }

    void IopRpcBridge::removeServersInRange(uint32_t base, uint32_t size)
    {
        for (auto server = m_servers.begin(); server != m_servers.end();)
        {
            const uint32_t function = IopMemory::physicalAddress(server->second.function);
            if (function >= base && function < base + size)
                server = m_servers.erase(server);
            else
                ++server;
        }
    }

    bool IopRpcBridge::hasEeRpcCompletions() noexcept
    {
        std::lock_guard<std::mutex> lock(m_eeRpcMutex);
        return !m_eeRpcDone.empty();
    }

    void IopRpcBridge::serviceEeRpcCompletions()
    {
        std::vector<EeRpcCompletion> done;
        {
            std::lock_guard<std::mutex> lock(m_eeRpcMutex);
            done.swap(m_eeRpcDone);
        }
        for (EeRpcCompletion &completion : done)
        {
            // The EE copied rsize bytes from the server's return pointer (SIF DMA into the IOP receive buffer).
            if (completion.ok && completion.receive != 0u && !completion.data.empty())
                (void)m_memory.writeRam(completion.receive, completion.data.data(), completion.data.size());
            if (!completion.ok)
                m_host.log(LogLevel::Warning, "[IOP] sceSifCallRpc to an EE server: the EE call did not run; receive buffer unchanged");
            if (completion.threadId != 0)
                m_kernel.hostResume(completion.threadId);
            for (uint32_t n = m_eeRpcInFlight.load(); n != 0u && !m_eeRpcInFlight.compare_exchange_weak(n, n - 1u);)
            {
            }
        }
    }

    bool IopRpcBridge::hasServer(uint32_t sid) const noexcept
    {
        const auto server = m_servers.find(sid);
        return server != m_servers.end() && server->second.function != 0u;
    }
}
