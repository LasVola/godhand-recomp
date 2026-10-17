#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

class GSBandPool
{
public:
    explicit GSBandPool(unsigned workers);
    ~GSBandPool();

    GSBandPool(const GSBandPool &) = delete;
    GSBandPool &operator=(const GSBandPool &) = delete;

    unsigned Threads() const { return static_cast<unsigned>(m_workers.size()) + 1u; }

    void Run(unsigned bands, const std::function<void(unsigned)> &work);

    static unsigned ConfiguredThreads();

private:
    void WorkerLoop();
    void Consume();

    std::vector<std::thread> m_workers;
    std::mutex m_mutex;
    std::condition_variable m_wake;
    std::condition_variable m_done;
    const std::function<void(unsigned)> *m_work = nullptr;
    unsigned m_bands = 0u;
    uint32_t m_mxcsr = 0u;
    uint64_t m_generation = 0u;
    std::atomic<unsigned> m_next{0u};
    unsigned m_finished = 0u;
    unsigned m_active = 0u;
    bool m_stop = false;
};
