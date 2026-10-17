// Host display timing for EeScheduler's vblank lock (PS2X_VSYNC_LOCK, docs/PROGRESS.md §67). Kept out of
// EeScheduler.cpp so <windows.h> stays out of it.
//
// A small thread waits for the vertical blank of the monitor the game window is on (D3DKMTWaitForVerticalBlankEvent,
// per display source) and records when it happened and the measured refresh period. DwmGetCompositionTimingInfo is no
// use with monitors of different rates: on the user's 60 Hz + 75 Hz setup it reports the 75 Hz one.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace
{
    std::atomic<void *> g_window{nullptr};
    std::atomic<int64_t> g_lastVblankNs{0};   // steady_clock time since epoch
    std::atomic<int64_t> g_periodNs{0};       // measured refresh period (smoothed)
    std::atomic<bool> g_threadStarted{false};

    int64_t steadyNs()
    {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

#if defined(_WIN32)
    // d3dkmthk.h (user-mode subset), resolved from gdi32 at run time.
    using D3DKMT_HANDLE = UINT;
    struct D3DKMT_OPENADAPTERFROMHDC
    {
        HDC hDc;
        D3DKMT_HANDLE hAdapter;
        LUID AdapterLuid;
        UINT VidPnSourceId;
    };
    struct D3DKMT_WAITFORVERTICALBLANKEVENT
    {
        D3DKMT_HANDLE hAdapter;
        D3DKMT_HANDLE hDevice;
        UINT VidPnSourceId;
    };
    struct D3DKMT_CLOSEADAPTER
    {
        D3DKMT_HANDLE hAdapter;
    };
    using OpenAdapterFromHdcFn = LONG(APIENTRY *)(D3DKMT_OPENADAPTERFROMHDC *);
    using WaitForVerticalBlankEventFn = LONG(APIENTRY *)(const D3DKMT_WAITFORVERTICALBLANKEVENT *);
    using CloseAdapterFn = LONG(APIENTRY *)(const D3DKMT_CLOSEADAPTER *);

    void vblankThread()
    {
        SetThreadDescription(GetCurrentThread(), L"VsyncThread");
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
        HMODULE gdi = GetModuleHandleW(L"gdi32.dll");
        auto openAdapter = gdi ? reinterpret_cast<OpenAdapterFromHdcFn>(GetProcAddress(gdi, "D3DKMTOpenAdapterFromHdc")) : nullptr;
        auto waitVblank = gdi ? reinterpret_cast<WaitForVerticalBlankEventFn>(GetProcAddress(gdi, "D3DKMTWaitForVerticalBlankEvent")) : nullptr;
        auto closeAdapter = gdi ? reinterpret_cast<CloseAdapterFn>(GetProcAddress(gdi, "D3DKMTCloseAdapter")) : nullptr;
        if (!openAdapter || !waitVblank || !closeAdapter)
        {
            std::fprintf(stderr, "[vsync-lock] D3DKMT vblank API unavailable; not locked\n");
            return;
        }
        HMONITOR monitor = nullptr;
        D3DKMT_OPENADAPTERFROMHDC adapter{};
        bool open = false;
        int64_t previous = 0;
        uint32_t sinceCheck = 0u;
        for (;;)
        {
            // Follow the window to whichever monitor it is on (checked about once a second).
            if (!open || ++sinceCheck >= 60u)
            {
                sinceCheck = 0u;
                HWND window = static_cast<HWND>(g_window.load(std::memory_order_acquire));
                HMONITOR current = window ? MonitorFromWindow(window, MONITOR_DEFAULTTOPRIMARY)
                                          : MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
                if (current != monitor || !open)
                {
                    if (open)
                    {
                        const D3DKMT_CLOSEADAPTER close{adapter.hAdapter};
                        closeAdapter(&close);
                        open = false;
                    }
                    MONITORINFOEXW info{};
                    info.cbSize = sizeof(info);
                    if (GetMonitorInfoW(current, &info))
                    {
                        adapter = {};
                        adapter.hDc = CreateDCW(info.szDevice, nullptr, nullptr, nullptr);
                        if (adapter.hDc)
                        {
                            open = openAdapter(&adapter) >= 0;
                            DeleteDC(adapter.hDc);
                        }
                    }
                    monitor = current;
                    previous = 0;
                    g_periodNs.store(0, std::memory_order_release);
                }
            }
            if (!open)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }
            const D3DKMT_WAITFORVERTICALBLANKEVENT wait{adapter.hAdapter, 0u, adapter.VidPnSourceId};
            if (waitVblank(&wait) < 0)
            {
                const D3DKMT_CLOSEADAPTER close{adapter.hAdapter};
                closeAdapter(&close);
                open = false;
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }
            const int64_t now = steadyNs();
            g_lastVblankNs.store(now, std::memory_order_release);
            if (previous != 0)
            {
                const int64_t interval = now - previous;
                const int64_t period = g_periodNs.load(std::memory_order_relaxed);
                // Smoothed period; intervals off by more than 25% (a missed wakeup) are ignored.
                if (period == 0)
                    g_periodNs.store(interval, std::memory_order_release);
                else if (interval > period * 3 / 4 && interval < period * 5 / 4)
                    g_periodNs.store(period + (interval - period) / 32, std::memory_order_release);
            }
            previous = now;
        }
    }
#endif
}

// The game window, so the vblank thread follows the monitor it is on (ps2_runtime.cpp, after InitWindow).
void ps2xHostVsyncSetWindow(void *window) noexcept
{
    g_window.store(window, std::memory_order_release);
}

// The last vblank of the game window's monitor and its refresh period, on the steady_clock time base. Returns false
// while there is no recent measurement (thread starting, monitor switch, no D3DKMT).
bool ps2xHostVsync(std::chrono::steady_clock::time_point &lastVblank, std::chrono::nanoseconds &period) noexcept
{
#if defined(_WIN32)
    if (!g_threadStarted.exchange(true))
        std::thread(vblankThread).detach();
    const int64_t last = g_lastVblankNs.load(std::memory_order_acquire);
    const int64_t periodNs = g_periodNs.load(std::memory_order_acquire);
    if (last == 0 || periodNs == 0 || steadyNs() - last > 100000000)
        return false;
    lastVblank = std::chrono::steady_clock::time_point(std::chrono::nanoseconds(last));
    period = std::chrono::nanoseconds(periodNs);
    // PS2X_VSYNC_TEST_MULTIPLY=<n> (tests): report an n times faster display (60 Hz -> 120/240 Hz) with vblanks between
    // the real ones, to exercise the lock's multiple-of-60 path on a 60 Hz monitor.
    static const int64_t multiply = []
    {
        const char *value = std::getenv("PS2X_VSYNC_TEST_MULTIPLY");
        return value ? std::max<int64_t>(1, std::atoll(value)) : 1;
    }();
    if (multiply > 1)
    {
        period = std::chrono::nanoseconds(periodNs / multiply);
        lastVblank += period * ((steadyNs() - last) / period.count());
    }
    return true;
#else
    (void)lastVblank;
    (void)period;
    return false;
#endif
}

// Round 10 diagnostics (PS2X_PACING_STATS): the efficiency class of the logical processor the calling thread runs on
// right now (hybrid CPUs: higher = faster core; 0 on every core of a non-hybrid CPU), or -1 when unknown, and the
// calling thread's CPU time (user + kernel) in milliseconds. Kept here so <windows.h> stays out of EeScheduler.cpp.
int ps2xCurrentCoreClass() noexcept
{
#if defined(_WIN32)
    static const std::vector<int8_t> classes = []
    {
        std::vector<int8_t> table(64u * 4u, -1);
        DWORD length = 0u;
        GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &length);
        if (length == 0u)
            return table;
        std::vector<uint8_t> buffer(length);
        auto *info = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *>(buffer.data());
        if (!GetLogicalProcessorInformationEx(RelationProcessorCore, info, &length))
            return table;
        for (DWORD offset = 0u; offset < length;)
        {
            const auto *entry = reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *>(buffer.data() + offset);
            if (entry->Relationship == RelationProcessorCore)
                for (WORD g = 0u; g < entry->Processor.GroupCount; ++g)
                {
                    const GROUP_AFFINITY &mask = entry->Processor.GroupMask[g];
                    if (mask.Group >= 4u)
                        continue;
                    for (uint32_t lp = 0u; lp < 64u; ++lp)
                        if (mask.Mask & (static_cast<KAFFINITY>(1) << lp))
                            table[mask.Group * 64u + lp] = static_cast<int8_t>(entry->Processor.EfficiencyClass);
                }
            offset += entry->Size;
        }
        return table;
    }();
    PROCESSOR_NUMBER number{};
    GetCurrentProcessorNumberEx(&number);
    const size_t index = static_cast<size_t>(number.Group) * 64u + number.Number;
    return index < classes.size() ? classes[index] : -1;
#else
    return -1;
#endif
}

double ps2xCurrentThreadCpuMs() noexcept
{
#if defined(_WIN32)
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user))
        return 0.0;
    const auto ticks = [](const FILETIME &t)
    { return (static_cast<uint64_t>(t.dwHighDateTime) << 32) | t.dwLowDateTime; };
    return static_cast<double>(ticks(kernel) + ticks(user)) / 10000.0;
#else
    return 0.0;
#endif
}
