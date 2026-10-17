#include "runtime/gs/gs_band_pool.h"
#include "ThreadNaming.h"

#include <algorithm>
#include <cstdlib>
#include <string>
#include <xmmintrin.h>

GSBandPool::GSBandPool(unsigned workers)
{
    m_workers.reserve(workers);
    for (unsigned i = 0; i < workers; ++i)
        m_workers.emplace_back([this, i]
                               {
            ThreadNaming::SetCurrentThreadName("GSBand" + std::to_string(i));
            WorkerLoop(); });
}

GSBandPool::~GSBandPool()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stop = true;
    }
    m_wake.notify_all();
    for (std::thread &worker : m_workers)
        worker.join();
}

unsigned GSBandPool::ConfiguredThreads()
{
    if (const char *value = std::getenv("PS2X_GS_BANDS"))
        return static_cast<unsigned>(std::max(1, std::atoi(value)));
    const unsigned hardware = std::max(1u, std::thread::hardware_concurrency());
    return std::clamp(hardware > 4u ? hardware - 4u : 1u, 1u, 8u);
}

void GSBandPool::Consume()
{
    for (;;)
    {
        const unsigned band = m_next.fetch_add(1u, std::memory_order_relaxed);
        if (band >= m_bands)
            return;
        (*m_work)(band);
        std::lock_guard<std::mutex> lock(m_mutex);
        if (++m_finished == m_bands)
            m_done.notify_all();
    }
}

void GSBandPool::WorkerLoop()
{
    uint64_t seen = 0u;
    for (;;)
    {
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_wake.wait(lock, [&]
                        { return m_stop || m_generation != seen; });
            if (m_stop)
                return;
            seen = m_generation;
            ++m_active;
        }
        _mm_setcsr(m_mxcsr);
        Consume();
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            --m_active;
        }
        m_done.notify_all();
    }
}

void GSBandPool::Run(unsigned bands, const std::function<void(unsigned)> &work)
{
    if (bands <= 1u || m_workers.empty())
    {
        for (unsigned band = 0; band < bands; ++band)
            work(band);
        return;
    }
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_done.wait(lock, [&]
                    { return m_active == 0u; });
        m_work = &work;
        m_bands = bands;
        m_mxcsr = _mm_getcsr();
        m_finished = 0u;
        m_next.store(0u, std::memory_order_relaxed);
        ++m_generation;
    }
    m_wake.notify_all();
    Consume();
    std::unique_lock<std::mutex> lock(m_mutex);
    m_done.wait(lock, [&]
                { return m_finished == m_bands && m_active == 0u; });
    m_work = nullptr;
}
