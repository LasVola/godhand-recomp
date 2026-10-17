#include "ps2x/iop/iop_subsystem.h"

#include "iop_service.h"
#include "iop_module_manager.h"
#include "emulator/iop_emulator.h"
#include "module_factories.h"
#include "ps2x/iop/ps2_path.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#pragma comment(lib, "Synchronization.lib") // WaitOnAddress / WakeByAddressSingle
#endif

#if defined(_M_X64) || defined(__x86_64__)
#include <immintrin.h>
#define PS2X_IOP_SPIN_PAUSE() _mm_pause()
#else
#define PS2X_IOP_SPIN_PAUSE() std::this_thread::yield()
#endif

namespace ps2x::iop
{
    namespace
    {
        // True on the IOP worker thread while it runs a batch (PS2X_IOP_THREAD).
        thread_local bool t_onIopWorker = false;

        uint32_t envMicros(const char *name, uint32_t fallback)
        {
            const char *v = std::getenv(name);
            return v && *v ? static_cast<uint32_t>(std::strtoul(v, nullptr, 10)) : fallback;
        }

        // How the EE and the IOP worker wait for each other. Default (Windows): spin a few microseconds with pause,
        // then sleep in WaitOnAddress; the other side calls WakeByAddressSingle only when it sees a sleeper.
        // PS2X_IOP_SPIN=1: the old behaviour (the worker spins up to 2 ms between batches, join spins forever).
        struct IopWaitPolicy
        {
            bool spinOnly = true;
            uint32_t workerSpinUs = 0u; // PS2X_IOP_WORKER_SPIN_US: idle worker spin before sleeping
            uint32_t joinSpinUs = 0u;   // PS2X_IOP_JOIN_SPIN_US: EE spin in join() before sleeping
        };

        const IopWaitPolicy &iopWaitPolicy()
        {
            static const IopWaitPolicy policy = [] {
                IopWaitPolicy p;
#if defined(_WIN32)
                const char *spin = std::getenv("PS2X_IOP_SPIN");
                p.spinOnly = spin && *spin == '1';
#endif
                // 5 us (was 20): at ~17k batches/s the 20 us spin was ~6-8 points of a core with no FPS effect (PROGRESS §75).
                p.workerSpinUs = envMicros("PS2X_IOP_WORKER_SPIN_US", 5u);
                p.joinSpinUs = envMicros("PS2X_IOP_JOIN_SPIN_US", 200u);
                std::fprintf(stderr, "[iop-thread] wait policy: %s (worker spin %u us, join spin %u us)\n",
                             p.spinOnly ? "spin (PS2X_IOP_SPIN=1)" : "spin then WaitOnAddress", p.workerSpinUs, p.joinSpinUs);
                return p;
            }();
            return policy;
        }

        // Host proxy for the threaded IOP. While a batch runs on the worker, the effects that land on the EE side -
        // writes into EE RAM (SIF DMA) and SIF command packets (they run EE handlers) - are queued and replayed in order
        // on the EE thread when it joins the batch. The EE therefore sees them at batch boundaries, as when the IOP
        // ran inline. Everything else (audio, pad, host files, EE RAM reads) is thread-safe and passes through.
        class DeferringHost final : public IopHost
        {
        public:
            explicit DeferringHost(IopHost &inner) : m_inner(inner) {}

            void drain()
            {
                for (Deferred &item : m_queue)
                {
                    switch (item.kind)
                    {
                    case Deferred::Kind::Write:
                        (void)m_inner.writeGuest(item.address, item.data.data(), item.data.size());
                        break;
                    case Deferred::Kind::Zero:
                        (void)m_inner.zeroGuest(item.address, item.size);
                        break;
                    case Deferred::Kind::SifCommand:
                        (void)m_inner.sendSifCommand(item.address, item.data.data(), item.data.size());
                        break;
                    case Deferred::Kind::RpcComplete:
                        m_inner.completeRpc(static_cast<uint64_t>(item.size));
                        break;
                    case Deferred::Kind::Action:
                        if (item.action)
                            item.action();
                        break;
                    }
                }
                m_queue.clear();
            }

            bool readGuest(uint32_t address, void *destination, size_t size) const override { return m_inner.readGuest(address, destination, size); }
            bool writeGuest(uint32_t address, const void *source, size_t size) override
            {
                if (!t_onIopWorker)
                    return m_inner.writeGuest(address, source, size);
                Deferred item{Deferred::Kind::Write, address, size, {}};
                item.data.assign(static_cast<const uint8_t *>(source), static_cast<const uint8_t *>(source) + size);
                m_queue.push_back(std::move(item));
                return true;
            }
            bool zeroGuest(uint32_t address, size_t size) override
            {
                if (!t_onIopWorker)
                    return m_inner.zeroGuest(address, size);
                m_queue.push_back({Deferred::Kind::Zero, address, size, {}});
                return true;
            }
            bool normalizeGuestAddress(uint32_t address, uint32_t &normalized) const override { return m_inner.normalizeGuestAddress(address, normalized); }
            bool readIopMemory(uint32_t address, void *destination, size_t size) const override { return m_inner.readIopMemory(address, destination, size); }
            bool writeIopMemory(uint32_t address, const void *source, size_t size) override { return m_inner.writeIopMemory(address, source, size); }
            bool zeroIopMemory(uint32_t address, size_t size) override { return m_inner.zeroIopMemory(address, size); }
            bool normalizeIopAddress(uint32_t address, uint32_t &normalized) const override { return m_inner.normalizeIopAddress(address, normalized); }
            uint32_t allocateIopHandle(IopHandleKind kind) override { return m_inner.allocateIopHandle(kind); }
            uint32_t allocateGuest(uint32_t size, uint32_t alignment) override { return m_inner.allocateGuest(size, alignment); }
            void freeGuest(uint32_t address) override { m_inner.freeGuest(address); }
            void audioCommand(uint32_t sid, uint32_t function, GuestBuffer send, GuestBuffer receive) override { m_inner.audioCommand(sid, function, send, receive); }
            void submitAudio(std::span<const int16_t> samples) override { m_inner.submitAudio(samples); }
            void resetAudio() override { m_inner.resetAudio(); }
            std::string hostPath(HostPathKind kind) const override { return m_inner.hostPath(kind); }
            std::string translateGuestPath(std::string_view path) const override { return m_inner.translateGuestPath(path); }
            uint64_t openHostFile(std::string_view path) override { return m_inner.openHostFile(path); }
            bool hostFileSize(uint64_t handle, uint64_t &size) const override { return m_inner.hostFileSize(handle, size); }
            bool readHostFile(uint64_t handle, uint64_t offset, void *destination, size_t size, size_t &bytesRead) override
            {
                return m_inner.readHostFile(handle, offset, destination, size, bytesRead);
            }
            void closeHostFile(uint64_t handle) override { m_inner.closeHostFile(handle); }
            int32_t memoryCard(const MemoryCardRequest &request) override { return m_inner.memoryCard(request); }
            bool hasGuestFunction(uint32_t address) const override { return m_inner.hasGuestFunction(address); }
            bool invokeGuestFunction(uint64_t callToken, uint32_t address, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3,
                                     uint32_t *resultAddress) override
            {
                return m_inner.invokeGuestFunction(callToken, address, a0, a1, a2, a3, resultAddress);
            }
            bool sendSifCommand(uint32_t commandId, const void *packet, size_t packetSize) override
            {
                if (!t_onIopWorker)
                    return m_inner.sendSifCommand(commandId, packet, packetSize);
                Deferred item{Deferred::Kind::SifCommand, commandId, packetSize, {}};
                item.data.assign(static_cast<const uint8_t *>(packet), static_cast<const uint8_t *>(packet) + packetSize);
                m_queue.push_back(std::move(item));
                return true;
            }
            void completeRpc(uint64_t callToken) override
            {
                if (!t_onIopWorker)
                    return m_inner.completeRpc(callToken);
                m_queue.push_back(Deferred{Deferred::Kind::RpcComplete, 0u, static_cast<size_t>(callToken), {}});
            }
            bool readPad(uint32_t port, PadInputState &state) override { return m_inner.readPad(port, state); }
            void padVibration(uint32_t port, uint8_t smallMotor, uint8_t largeMotor) override { m_inner.padVibration(port, smallMotor, largeMotor); }
            bool readPadSlot(uint32_t port, uint32_t slot, PadInputState &state) override { return m_inner.readPadSlot(port, slot, state); }
            void padVibrationSlot(uint32_t port, uint32_t slot, uint8_t smallMotor, uint8_t largeMotor) override
            {
                m_inner.padVibrationSlot(port, slot, smallMotor, largeMotor);
            }
            bool bindEeRpcServer(uint32_t sid, uint32_t &serverData, uint32_t &buffer, uint32_t &callbackBuffer) override
            {
                return m_inner.bindEeRpcServer(sid, serverData, buffer, callbackBuffer); // reads only
            }
            // An IOP -> EE call starts EE code: like a SIF command it is queued on the worker and started on the EE thread
            // when it joins the batch.
            bool callEeRpcServer(uint32_t serverData, uint32_t function, std::vector<uint8_t> send, uint32_t receiveSize,
                                 std::function<void(std::vector<uint8_t>, bool)> done) override
            {
                if (!t_onIopWorker)
                    return m_inner.callEeRpcServer(serverData, function, std::move(send), receiveSize, std::move(done));
                Deferred item{Deferred::Kind::Action, 0u, 0u, {}};
                item.action = [this, serverData, function, send = std::move(send), receiveSize, done = std::move(done)]() mutable
                {
                    if (!m_inner.callEeRpcServer(serverData, function, std::move(send), receiveSize, done))
                        done({}, false);
                };
                m_queue.push_back(std::move(item));
                return true;
            }
            void log(LogLevel level, std::string_view message) override { m_inner.log(level, message); }

        private:
            struct Deferred
            {
                enum class Kind : uint8_t
                {
                    Write,
                    Zero,
                    SifCommand,
                    RpcComplete, // size = call token
                    Action       // run action on the EE thread
                } kind;
                uint32_t address; // EE address, or the command id
                size_t size;
                std::vector<uint8_t> data;
                std::function<void()> action{};
            };

            IopHost &m_inner;
            std::vector<Deferred> m_queue; // worker appends during a batch; the EE drains after joining it
        };

        // On by default (PS2X_IOP_THREAD=0 runs the IOP inline on the EE thread again). Urban Reign fight: 22 -> 25
        // game frames/s; the IOP may lag the EE by up to kMaxAsyncLagEeCycles, but every interaction catches it up.
        bool iopThreadEnabled()
        {
            static const bool enabled = [] { const char *v = std::getenv("PS2X_IOP_THREAD"); return !v || *v != '0'; }();
            return enabled;
        }
    }

    class IopSubsystem::Impl
    {
    public:
        explicit Impl(IopHost &hostRef)
            : host(hostRef),
              deferringHost(hostRef),
              emulator(iopThreadEnabled() ? static_cast<IopHost &>(deferringHost) : hostRef)
        {
            // PS2X_IOP_HLE_MCSERV=0: let the game's own MCSERV/MCMAN IRX (loaded on the emulated IOP) serve memory card
            // RPCs over the emulated SIO2 card instead of the HLE service. The HLE implements the newer XMCSERV
            // protocol; libmc 2.x games such as Urban Reign talk to the old MCSERV.
            if (const char *mcservHle = std::getenv("PS2X_IOP_HLE_MCSERV"); !mcservHle || *mcservHle != '0')
                coreServices.emplace_back(detail::createMcservService(host));
            coreServices.emplace_back(detail::createDbcmanService(host));
            // PS2X_IOP_HLE_LIBSD=0: let the game's own SDRDRV.IRX serve the sceSdRemote RPC (sid 0x80000701) on the
            // emulated IOP and real LIBSD.IRX, instead of the host-audio HLE that swallows those commands.
            if (const char *libsdHle = std::getenv("PS2X_IOP_HLE_LIBSD"); !libsdHle || *libsdHle != '0')
                coreServices.emplace_back(detail::createLibSdService(host));
            refreshServiceModuleKeys();
            rebuildRoutes();
        }

        bool serviceActive(const detail::IopService &service) const
        {
            return moduleManager.isLoaded(service.moduleAliases());
        }

        void refreshServiceModuleKeys()
        {
            std::vector<std::string> keys;
            for (const auto &service : coreServices)
            {
                for (std::string_view alias : service->moduleAliases())
                    keys.emplace_back(alias);
            }
            moduleManager.setServiceModuleKeys(std::move(keys));
        }

        void rebuildRoutes()
        {
            routes.clear();
            lastError.clear();
            for (const auto &service : coreServices)
            {
                if (!serviceActive(*service))
                    continue;
                for (const uint32_t sid : service->sids())
                {
                    if (!routes.emplace(sid, service.get()).second)
                    {
                        std::ostringstream out;
                        out << "duplicate IOP SID 0x" << std::hex << sid << " in core services";
                        lastError = out.str();
                        routes.clear();
                        return;
                    }
                }
            }
        }

        void recordLoadOutcome(std::string_view path, bool hle)
        {
            constexpr size_t maxOutcomes = 32u;
            if (loadOutcomes.size() >= maxOutcomes || !loggedLoadPaths.emplace(path).second)
                return;
            std::string message = hle ? "[IOP:HLE] fallback module='" : "[IOP:load-failed] module='";
            message.append(path);
            message += hle ? "' physical IRX unavailable; using registered HLE provider"
                           : "' no HLE provider accepted the module; physical IRX was not loaded";
            loadOutcomes.push_back(message);
            host.log(hle ? LogLevel::Info : LogLevel::Warning, message);
        }

        ~Impl()
        {
            if (worker.joinable())
            {
                join();
                {
                    std::lock_guard<std::mutex> lock(wakeMutex);
                    stopWorker.store(true, std::memory_order_release);
                }
                wakeCv.notify_one();
#if defined(_WIN32)
                WakeByAddressAll(const_cast<std::atomic<bool> *>(&busy));
#endif
                worker.join();
            }
        }

        void flushEeCycles() noexcept
        {
            join();
            if (pendingEeCycles != 0u)
            {
                const uint64_t cycles = pendingEeCycles;
                pendingEeCycles = 0u;
                emulator.runEeCycles(cycles);
            }
            eeCycleBudget = emulator.eeCyclesUntilBatch();
        }

        // PS2X_IOP_THREAD=1: whole IOP batches run on a worker thread while the EE keeps going. Every other entry
        // point joins the running batch first, so the EE never touches the IOP while it runs.
        void runEeCyclesAsync(uint64_t eeCycles) noexcept
        {
            pendingEeCycles += eeCycles;
            if (pendingEeCycles < kAsyncBatchEeCycles)
                return;
            // While the worker is still on the previous batch, keep running and let cycles pile up (the EE skipping
            // idle time produces several batches back to back), up to kMaxAsyncLagEeCycles; then wait as before.
            // Every IOP interaction (RPC, SIF DMA, IOP memory) still joins and catches up first.
            if (busy.load(std::memory_order_acquire) && pendingEeCycles < kMaxAsyncLagEeCycles)
                return;
            join();
            postedEeCycles = pendingEeCycles;
            pendingEeCycles = 0u;
            if (!worker.joinable())
                worker = std::thread([this] { workerLoop(); });
#if defined(_WIN32)
            if (!iopWaitPolicy().spinOnly)
            {
                // Store busy, then look for a sleeper (both seq_cst, paired with the worker's store of workerSleeping
                // and load of busy): either we see it asleep and wake it, or it sees busy and does not sleep.
                busy.store(true, std::memory_order_seq_cst);
                if (workerSleeping.load(std::memory_order_seq_cst))
                {
                    WakeByAddressSingle(&busy);
                    workerWakes.fetch_add(1u, std::memory_order_relaxed);
                }
                return;
            }
#endif
            busy.store(true, std::memory_order_release);
            if (workerSleeping.load(std::memory_order_acquire))
            {
                std::lock_guard<std::mutex> lock(wakeMutex);
                wakeCv.notify_one();
            }
        }

        // Waits for the running batch, then replays its EE-side effects on this (the EE) thread.
        void join() noexcept
        {
            if (!worker.joinable())
                return;
            if (busy.load(std::memory_order_acquire))
            {
                const auto waitStart = std::chrono::steady_clock::now();
#if defined(_WIN32)
                if (!iopWaitPolicy().spinOnly)
                    waitBatchDone(waitStart);
                else
#endif
                    while (busy.load(std::memory_order_acquire))
                        PS2X_IOP_SPIN_PAUSE();
                const double waited = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - waitStart).count();
                double expected = joinWaitUs.load(std::memory_order_relaxed);
                while (!joinWaitUs.compare_exchange_weak(expected, expected + waited, std::memory_order_relaxed))
                {
                }
            }
            deferringHost.drain();
        }

        void workerLoop()
        {
            t_onIopWorker = true;
#if defined(_WIN32)
            SetThreadDescription(GetCurrentThread(), L"IOPThread"); // shows up in profilers (URBAN_NATIVE_PROFILE, F6)
#endif
#if defined(_WIN32)
            if (!iopWaitPolicy().spinOnly)
            {
                for (;;)
                {
                    if (!waitForBatch())
                        return;
                    runPostedBatch();
                    // Pairs with join(): either the EE sees busy == false, or we see it waiting and wake it.
                    busy.store(false, std::memory_order_seq_cst);
                    if (eeWaiting.load(std::memory_order_seq_cst))
                        WakeByAddressSingle(&busy);
                }
            }
#endif
            for (;;)
            {
                // Spin briefly (batches arrive every few tens of microseconds while the game runs), then sleep.
                const auto idleSince = std::chrono::steady_clock::now();
                uint32_t spins = 0u;
                while (!busy.load(std::memory_order_acquire))
                {
                    if (stopWorker.load(std::memory_order_acquire))
                        return;
                    // Pure spinning (no yield: a yielded worker can take a whole OS quantum to come back, and the EE then
                    // waits for it in join) for up to 2 ms, then sleep until the next batch.
                    PS2X_IOP_SPIN_PAUSE();
                    if (++spins < 1024u)
                        continue;
                    spins = 0u;
                    if (std::chrono::steady_clock::now() - idleSince < std::chrono::milliseconds(2))
                        continue;
                    std::unique_lock<std::mutex> lock(wakeMutex);
                    workerSleeping.store(true, std::memory_order_release);
                    wakeCv.wait_for(lock, std::chrono::milliseconds(50),
                                    [this] { return busy.load(std::memory_order_acquire) || stopWorker.load(std::memory_order_acquire); });
                    workerSleeping.store(false, std::memory_order_release);
                }
                runPostedBatch();
                busy.store(false, std::memory_order_release);
            }
        }

        void runPostedBatch()
        {
            const auto batchStart = std::chrono::steady_clock::now();
            emulator.runEeCycles(postedEeCycles);
            const auto batchEnd = std::chrono::steady_clock::now();
            // PS2X_IOP_THREAD_STATS=1: batches per second, worker busy time and EE join wait, every 5 s.
            static const bool stats = [] { const char *v = std::getenv("PS2X_IOP_THREAD_STATS"); return v && *v == '1'; }();
            if (stats)
            {
                static auto windowStart = batchStart;
                static double busyUs = 0.0;
                static uint64_t batches = 0u;
                busyUs += std::chrono::duration<double, std::micro>(batchEnd - batchStart).count();
                ++batches;
                const double windowS = std::chrono::duration<double>(batchEnd - windowStart).count();
                if (windowS >= 5.0)
                {
                    std::fprintf(stderr, "[iop-thread] %.0f batches/s, worker busy %.1f%% (%.1f us/batch), EE join wait %.1f%%, "
                                         "worker wakes %.0f/s, EE join sleeps %.0f/s\n",
                                 batches / windowS, busyUs / (windowS * 1e4), busyUs / static_cast<double>(batches),
                                 joinWaitUs.exchange(0.0) / (windowS * 1e4), workerWakes.exchange(0u) / windowS,
                                 joinSleeps.exchange(0u) / windowS);
                    windowStart = batchEnd;
                    busyUs = 0.0;
                    batches = 0u;
                }
            }
        }

#if defined(_WIN32)
        // Worker side: spin PS2X_IOP_WORKER_SPIN_US for the next batch, then sleep on busy until the EE posts one.
        // Returns false when the worker must stop.
        bool waitForBatch()
        {
            const auto spinUntil = std::chrono::steady_clock::now() + std::chrono::microseconds(iopWaitPolicy().workerSpinUs);
            uint32_t spins = 0u;
            while (!busy.load(std::memory_order_acquire))
            {
                if (stopWorker.load(std::memory_order_acquire))
                    return false;
                PS2X_IOP_SPIN_PAUSE();
                if ((++spins & 63u) != 0u || std::chrono::steady_clock::now() < spinUntil)
                    continue;
                workerSleeping.store(true, std::memory_order_seq_cst);
                bool notBusy = false;
                // WaitOnAddress re-checks the value, so a wake between this load and the call is not lost. The timeout
                // is only a safety net (shutdown also wakes us).
                if (!busy.load(std::memory_order_seq_cst) && !stopWorker.load(std::memory_order_acquire))
                    WaitOnAddress(&busy, &notBusy, sizeof(notBusy), 50u);
                workerSleeping.store(false, std::memory_order_relaxed);
            }
            return true;
        }

        // EE side: spin PS2X_IOP_JOIN_SPIN_US for the running batch (they take ~15 us), then sleep until it ends.
        void waitBatchDone(std::chrono::steady_clock::time_point waitStart)
        {
            const auto spinUntil = waitStart + std::chrono::microseconds(iopWaitPolicy().joinSpinUs);
            uint32_t spins = 0u;
            while (busy.load(std::memory_order_acquire))
            {
                PS2X_IOP_SPIN_PAUSE();
                if ((++spins & 63u) != 0u || std::chrono::steady_clock::now() < spinUntil)
                    continue;
                eeWaiting.store(true, std::memory_order_seq_cst);
                bool isBusy = true;
                if (busy.load(std::memory_order_seq_cst))
                {
                    joinSleeps.fetch_add(1u, std::memory_order_relaxed);
                    WaitOnAddress(&busy, &isBusy, sizeof(isBusy), 50u);
                }
                eeWaiting.store(false, std::memory_order_relaxed);
            }
        }
#endif

        static constexpr uint64_t kAsyncBatchEeCycles = 8192u;
        static constexpr uint64_t kMaxAsyncLagEeCycles = 512u * kAsyncBatchEeCycles;

        IopHost &host;
        DeferringHost deferringHost;
        uint64_t pendingEeCycles = 0u;
        uint64_t eeCycleBudget = 0u;
        uint64_t postedEeCycles = 0u;
        std::thread worker;
        std::atomic<bool> busy{false};
        std::atomic<bool> stopWorker{false};
        std::atomic<bool> workerSleeping{false};
        std::atomic<bool> eeWaiting{false};
        std::atomic<uint32_t> workerWakes{0u};
        std::atomic<uint32_t> joinSleeps{0u};
        std::atomic<double> joinWaitUs{0.0};
        std::mutex wakeMutex;
        std::condition_variable wakeCv;
        detail::ServiceList coreServices;
        std::unordered_map<uint32_t, detail::IopService *> routes;
        std::vector<std::string> loadOutcomes;
        std::unordered_set<std::string> loggedLoadPaths;
        std::string lastError;
        detail::IopModuleManager moduleManager;
        detail::IopEmulator emulator;
    };

    IopSubsystem::IopSubsystem(IopHost &host)
        : m_impl(std::make_unique<Impl>(host))
    {
    }

    IopSubsystem::~IopSubsystem() = default;
    IopSubsystem::IopSubsystem(IopSubsystem &&) noexcept = default;
    IopSubsystem &IopSubsystem::operator=(IopSubsystem &&) noexcept = default;

    void IopSubsystem::reset()
    {
        m_impl->join();
        m_impl->pendingEeCycles = 0u;
        m_impl->eeCycleBudget = 0u;
        m_impl->moduleManager.reset();
        m_impl->loadOutcomes.clear();
        m_impl->loggedLoadPaths.clear();
        for (auto &service : m_impl->coreServices)
        {
            if (service)
            {
                service->reset();
            }
        }
        m_impl->emulator.reset();
        m_impl->refreshServiceModuleKeys();
        m_impl->rebuildRoutes();
    }

    ModuleLoadResult IopSubsystem::loadModule(std::string_view path, const void *arguments, uint32_t argumentSize)
    {
        m_impl->flushEeCycles();
        const ParsedPs2Path parsed = parsePs2Path(path);
        if (!parsed)
            return {true, -1, -1};

        if (parsed.device != Ps2PathDevice::Rom0)
        {
            ModuleLoadResult physical = m_impl->emulator.loadModule(path, arguments, argumentSize);
            if (physical.moduleId > 0)
            {
                m_impl->moduleManager.observePhysicalLoad(physical.moduleId, path);
                m_impl->rebuildRoutes();
                return physical;
            }
        }

        ModuleLoadResult hle = m_impl->moduleManager.loadHle(path);
        if (hle.moduleId > 0)
        {
            m_impl->rebuildRoutes();
            if (parsed.device != Ps2PathDevice::Rom0)
                m_impl->recordLoadOutcome(path, true);
        }
        else
        {
            m_impl->recordLoadOutcome(path, false);
        }
        return hle;
    }

    ModuleLoadResult IopSubsystem::loadModuleBuffer(uint32_t guestAddress, const void *arguments, uint32_t argumentSize)
    {
        m_impl->flushEeCycles();
        return m_impl->emulator.loadModuleBuffer(guestAddress, arguments, argumentSize);
    }

    bool IopSubsystem::stopModule(int32_t moduleId, int32_t *result)
    {
        m_impl->flushEeCycles();
        if (m_impl->moduleManager.stopHle(moduleId, result))
        {
            m_impl->rebuildRoutes();
            return true;
        }
        if (!m_impl->emulator.stopModule(moduleId, result))
            return false;
        m_impl->moduleManager.observePhysicalStop(moduleId);
        m_impl->rebuildRoutes();
        return true;
    }

    void IopSubsystem::runEeCycles(uint64_t eeCycles) noexcept
    {
        if (iopThreadEnabled())
        {
            m_impl->runEeCyclesAsync(eeCycles);
            return;
        }
        m_impl->pendingEeCycles += eeCycles;
        if (m_impl->pendingEeCycles >= m_impl->eeCycleBudget)
            m_impl->flushEeCycles();
    }

    RpcAbi IopSubsystem::selectRpcAbi(const RpcAbiRequest &request) const
    {
        m_impl->join();
        for (const auto &service : m_impl->coreServices)
        {
            if (service && m_impl->serviceActive(*service))
            {
                const RpcAbi selected = service->selectRpcAbi(request);
                if (selected != RpcAbi::RuntimeDefault)
                {
                    return selected;
                }
            }
        }
        return RpcAbi::RuntimeDefault;
    }

    bool IopSubsystem::canBindRpc(uint32_t sid) const noexcept
    {
        m_impl->join();
        if (m_impl->routes.find(sid) != m_impl->routes.end())
        {
            return true;
        }
        return m_impl->emulator.hasRpcServer(sid);
    }

    RpcResult IopSubsystem::handleRpc(const RpcRequest &request)
    {
        // PS2X_IOP_RPC_STATS=1: per server (sid) and function, calls per second, wait/NOWAIT and the time the EE spends
        // in the call (catching the IOP up + running the server), every 5 s.
        static const bool rpcStats = [] { const char *v = std::getenv("PS2X_IOP_RPC_STATS"); return v && *v == '1'; }();
        const auto statsStart = rpcStats ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        m_impl->flushEeCycles();
        const auto flushed = rpcStats ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        const auto route = m_impl->routes.find(request.sid);
        detail::IopService *hle = route != m_impl->routes.end() ? route->second : nullptr;

        RpcResult emulated = m_impl->emulator.handleRpc(request);
        if (rpcStats)
        {
            struct Entry
            {
                uint64_t calls = 0u;
                double flushUs = 0.0, serverUs = 0.0;
                uint32_t mode = 0u;
            };
            static std::map<std::pair<uint32_t, uint32_t>, Entry> entries;
            static auto windowStart = statsStart;
            const auto done = std::chrono::steady_clock::now();
            Entry &e = entries[{request.sid, request.function}];
            ++e.calls;
            e.mode = request.mode;
            e.flushUs += std::chrono::duration<double, std::micro>(flushed - statsStart).count();
            e.serverUs += std::chrono::duration<double, std::micro>(done - flushed).count();
            const double window = std::chrono::duration<double>(done - windowStart).count();
            if (window >= 5.0)
            {
                for (const auto &[key, value] : entries)
                    std::fprintf(stderr, "[iop-rpc] sid 0x%08x fn %u mode %u: %.1f calls/s, catch-up %.1f us, server %.1f us per call (%.2f%% of the EE)\n",
                                 key.first, key.second, value.mode, value.calls / window, value.flushUs / value.calls,
                                 value.serverUs / value.calls, (value.flushUs + value.serverUs) / (window * 1e4));
                entries.clear();
                windowStart = done;
            }
        }
        if (emulated.handled || !hle)
        {
            return emulated;
        }
        return hle->handleRpc(request);
    }

    void IopSubsystem::onSifTransfer(const SifTransfer &transfer)
    {
        m_impl->flushEeCycles();
        for (auto &service : m_impl->coreServices)
        {
            if (service && m_impl->serviceActive(*service))
            {
                service->onSifTransfer(transfer);
            }
        }
        m_impl->emulator.onSifTransfer(transfer);
    }

    uint32_t IopSubsystem::allocateMemory(uint32_t size, uint32_t alignment)
    {
        m_impl->join();
        return m_impl->emulator.allocateMemory(size, alignment);
    }

    bool IopSubsystem::freeMemory(uint32_t address)
    {
        m_impl->join();
        return m_impl->emulator.freeMemory(address);
    }

    bool IopSubsystem::readMemory(uint32_t address, void *destination, size_t size) const
    {
        m_impl->join();
        return m_impl->emulator.readMemory(address, destination, size);
    }

    bool IopSubsystem::writeMemory(uint32_t address, const void *source, size_t size)
    {
        m_impl->join();
        return m_impl->emulator.writeMemory(address, source, size);
    }

    bool IopSubsystem::zeroMemory(uint32_t address, size_t size)
    {
        m_impl->join();
        return m_impl->emulator.zeroMemory(address, size);
    }

    bool IopSubsystem::isMemoryRange(uint32_t address, size_t size) const
    {
        m_impl->join();
        return m_impl->emulator.isMemoryRange(address, size);
    }

    DebugSnapshot IopSubsystem::debugSnapshot() const
    {
        m_impl->flushEeCycles();
        DebugSnapshot snapshot;
        snapshot.emulatorCycles = m_impl->emulator.cycles();
        snapshot.emulatorInstructions = m_impl->emulator.instructions();
        snapshot.emulatorLoadedModules = m_impl->emulator.loadedModuleCount();
        snapshot.emulatorThreads = m_impl->emulator.threadCount();
        snapshot.emulatorRpcServers = m_impl->emulator.rpcServerCount();
        snapshot.diagnostics = m_impl->loadOutcomes;
        for (std::string &line : m_impl->emulator.describeThreads())
            snapshot.diagnostics.push_back(std::move(line));
        if (!m_impl->lastError.empty())
        {
            snapshot.diagnostics.push_back(m_impl->lastError);
        }

        for (const auto &service : m_impl->coreServices)
        {
            DebugService row;
            row.name = service->name();
            row.sids.assign(service->sids().begin(), service->sids().end());
            row.active = m_impl->serviceActive(*service);
            service->appendDebugMetrics(row.metrics);
            snapshot.services.push_back(std::move(row));
        }
        return snapshot;
    }

    void IopSubsystem::flushPendingWork()
    {
        m_impl->flushEeCycles();
    }

    bool IopSubsystem::hasPendingRpc()
    {
        m_impl->join();
        return m_impl->emulator.hasPendingRpc();
    }

    void IopSubsystem::saveState(ps2x::state::Writer &out)
    {
        m_impl->flushEeCycles();
        out.section(ps2x::state::fourcc("IOPS"));
        m_impl->moduleManager.serialize(out);
        m_impl->emulator.saveState(out);
    }

    void IopSubsystem::loadState(ps2x::state::Reader &in)
    {
        m_impl->join();
        in.section(ps2x::state::fourcc("IOPS"));
        m_impl->moduleManager.serialize(in);
        m_impl->emulator.loadState(in);
        m_impl->pendingEeCycles = 0u;
        m_impl->eeCycleBudget = 0u;
        m_impl->rebuildRoutes();
    }
}
