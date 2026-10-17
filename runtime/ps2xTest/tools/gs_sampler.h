#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <map>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")

class GSSampler
{
public:
    void Start()
    {
        HANDLE process = GetCurrentProcess();
        SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
        SymInitialize(process, nullptr, TRUE);
        DuplicateHandle(process, GetCurrentThread(), process, &m_target, THREAD_ALL_ACCESS, FALSE, 0);
        m_running = true;
        m_thread = std::thread([this]
                               {
            while (m_running.load())
            {
                if (SuspendThread(m_target) != static_cast<DWORD>(-1))
                {
                    CONTEXT context{};
                    context.ContextFlags = CONTEXT_CONTROL;
                    if (GetThreadContext(m_target, &context))
                        m_samples.push_back(context.Rip);
                    ResumeThread(m_target);
                }
                Sleep(0);
                std::this_thread::sleep_for(std::chrono::microseconds(200));
            } });
    }

    void StopAndReport(size_t top)
    {
        m_running = false;
        m_thread.join();
        HANDLE process = GetCurrentProcess();
        std::map<std::string, size_t> byLine;
        std::map<std::string, size_t> byFunction;
        for (uint64_t address : m_samples)
        {
            char buffer[sizeof(SYMBOL_INFO) + 512]{};
            auto *symbol = reinterpret_cast<SYMBOL_INFO *>(buffer);
            symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
            symbol->MaxNameLen = 511;
            DWORD64 displacement = 0;
            std::string function = SymFromAddr(process, address, &displacement, symbol) ? symbol->Name : "?";
            IMAGEHLP_LINE64 line{};
            line.SizeOfStruct = sizeof(line);
            DWORD lineDisplacement = 0;
            std::string where = function;
            if (SymGetLineFromAddr64(process, address, &lineDisplacement, &line))
            {
                std::string file = line.FileName;
                const size_t slash = file.find_last_of("\\/");
                if (slash != std::string::npos)
                    file = file.substr(slash + 1);
                where = file + ":" + std::to_string(line.LineNumber);
            }
            ++byLine[where];
            ++byFunction[function];
        }
        auto print = [&](const std::map<std::string, size_t> &table, const char *title)
        {
            std::vector<std::pair<size_t, std::string>> sorted;
            for (const auto &kv : table)
                sorted.push_back({kv.second, kv.first});
            std::sort(sorted.rbegin(), sorted.rend());
            std::printf("== %s (%zu samples)\n", title, m_samples.size());
            for (size_t i = 0; i < sorted.size() && i < top; ++i)
                std::printf("%6.2f%%  %s\n", 100.0 * static_cast<double>(sorted[i].first) / static_cast<double>(m_samples.size()),
                            sorted[i].second.c_str());
        };
        print(byFunction, "functions");
        print(byLine, "lines");
        CloseHandle(m_target);
    }

private:
    HANDLE m_target = nullptr;
    std::atomic<bool> m_running{false};
    std::thread m_thread;
    std::vector<uint64_t> m_samples;
};
#endif
