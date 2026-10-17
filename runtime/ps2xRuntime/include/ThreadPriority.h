#pragma once

#include <cstdlib>

#if defined(_WIN32)
#ifndef WINAPI
#define WINAPI __stdcall
#endif

extern "C"
{
    __declspec(dllimport) void *WINAPI GetCurrentThread(void);
    __declspec(dllimport) int WINAPI SetThreadPriority(void *hThread, int nPriority);
}
#endif

namespace ThreadPriority
{
    enum class Level
    {
        AboveNormal = 1,
        Highest = 2
    };

    inline bool Enabled()
    {
        static const bool enabled = []
        {
            const char *value = std::getenv("PS2X_THREAD_PRIORITY");
            return !value || !*value || *value != '0';
        }();
        return enabled;
    }

    inline void RaiseCurrentThread(Level level)
    {
        if (!Enabled())
            return;
#if defined(_WIN32)
        ::SetThreadPriority(::GetCurrentThread(), static_cast<int>(level));
#else
        (void)level;
#endif
    }
}
