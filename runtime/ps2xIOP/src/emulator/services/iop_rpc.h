#pragma once

#include <functional>

#include "ps2x/iop/iop_types.h"
#include "ps2x/state_io.h"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

namespace ps2x::iop
{
    class IopHost;
}

namespace ps2x::iop::detail
{
    struct IopCpuState;
    class IopKernel;
    class IopMemory;

    class IopGuestExecutor
    {
    public:
        virtual ~IopGuestExecutor() = default;

        [[nodiscard]] virtual uint32_t executeGuestFunction(uint32_t address,
                                                            uint32_t a0,
                                                            uint32_t a1,
                                                            uint32_t a2,
                                                            uint32_t a3,
                                                            uint32_t gp) = 0;
        [[nodiscard]] virtual uint32_t executeGuestFunctionWithBudget(uint32_t address,
                                                                      uint32_t a0,
                                                                      uint32_t a1,
                                                                      uint32_t a2,
                                                                      uint32_t a3,
                                                                      uint32_t gp,
                                                                      uint32_t instructionBudget)
        {
            return executeGuestFunction(address, a0, a1, a2, a3, gp);
        }
        [[nodiscard]] virtual uint32_t executeServerFunction(uint32_t address,
                                                             uint32_t a0,
                                                             uint32_t a1,
                                                             uint32_t a2,
                                                             uint32_t a3,
                                                             uint32_t gp,
                                                             uint32_t priority)
        {
            (void)priority;
            return executeGuestFunction(address, a0, a1, a2, a3, gp);
        }
        // Like executeServerFunction, for a NOWAIT call: when the server blocks, returns false and leaves it running
        // as an IOP thread; later(result) runs when it returns. Returns true with result set when it finished now.
        [[nodiscard]] virtual bool executeServerFunctionAsync(uint32_t address,
                                                              uint32_t a0,
                                                              uint32_t a1,
                                                              uint32_t a2,
                                                              uint32_t a3,
                                                              uint32_t gp,
                                                              uint32_t priority,
                                                              std::function<void(uint32_t)> later,
                                                              uint32_t &result)
        {
            (void)later;
            result = executeServerFunction(address, a0, a1, a2, a3, gp, priority);
            return true;
        }
        // PS2X_IOP_NOWAIT_DEFER: start a NOWAIT server call as a ready IOP thread without running any of it here; the
        // IOP scheduler runs it with the rest of the IOP (on the IOP worker thread) and later(result) runs when it
        // returns. Returns false when it cannot (no free slot, nested call): the caller then runs it as before.
        [[nodiscard]] virtual bool startServerFunctionAsync(uint32_t address,
                                                            uint32_t a0,
                                                            uint32_t a1,
                                                            uint32_t a2,
                                                            uint32_t a3,
                                                            uint32_t gp,
                                                            uint32_t priority,
                                                            std::function<void(uint32_t)> later)
        {
            (void)address, (void)a0, (void)a1, (void)a2, (void)a3, (void)gp, (void)priority, (void)later;
            return false;
        }
    };

    class IopRpcBridge
    {
    public:
        struct DmaCallback
        {
            uint32_t function = 0;
            uint32_t argument = 0;
            uint32_t gp = 0;
            uint32_t bytes = 0;
        };

        IopRpcBridge(IopHost &host, IopMemory &memory, IopKernel &kernel) noexcept;

        void reset();
        [[nodiscard]] bool dispatchSifManImport(uint16_t ordinal, IopCpuState &cpu);
        [[nodiscard]] bool dispatchSifCmdImport(uint16_t ordinal, IopCpuState &cpu);
        [[nodiscard]] RpcResult handleRpc(const RpcRequest &request, IopGuestExecutor &executor);
        void onSifTransfer(const SifTransfer &transfer);
        void removeServersInRange(uint32_t base, uint32_t size);

        [[nodiscard]] bool hasServer(uint32_t sid) const noexcept;
        [[nodiscard]] size_t serverCount() const noexcept { return m_servers.size(); }
        [[nodiscard]] std::optional<DmaCallback> takeDmaCallback() noexcept;
        // IOP thread side of an IOP -> EE call: copies finished EE replies into IOP RAM and readies the waiting threads.
        void serviceEeRpcCompletions();
        [[nodiscard]] bool hasEeRpcCompletions() noexcept;

        template <typename Ar>
        void serialize(Ar &ar)
        {
            ps2x::state::fields(ar, m_servers, m_nextDmaId, m_dmaCallback);
        }

    private:
        [[nodiscard]] uint32_t setDma(uint32_t descriptorAddress, uint32_t descriptorCount, uint32_t &bytes);

        struct RpcServer
        {
            uint32_t sid = 0;
            uint32_t serverData = 0;
            uint32_t function = 0;
            uint32_t gp = 0;
            uint32_t buffer = 0;
            uint32_t callback = 0;
            uint32_t callbackBuffer = 0;
            uint32_t queue = 0;
        };

        IopHost &m_host;
        IopMemory &m_memory;
        IopKernel &m_kernel;
        std::unordered_map<uint32_t, RpcServer> m_servers;
        uint32_t m_nextDmaId = 1u;
        std::optional<DmaCallback> m_dmaCallback;
        bool m_sifInitialized = false;

        // IOP -> EE RPC replies (filled on the EE thread, drained on the IOP thread). Not in savestates: a state is
        // only saved at a safe point, never inside a memory card fast read/write.
        struct EeRpcCompletion
        {
            int threadId = 0;      // waiting IOP thread (0: NOWAIT call)
            uint32_t receive = 0u; // IOP receive buffer
            std::vector<uint8_t> data;
            bool ok = false;
        };
        std::mutex m_eeRpcMutex;
        std::vector<EeRpcCompletion> m_eeRpcDone;
        uint32_t m_eeRpcCalls = 0u;
    };
}
