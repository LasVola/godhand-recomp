#pragma once

// Port log: "[  12.3456][BOOT] message" lines, Info to stdout, Warn/Error to stderr.
// GENERIC: adapted from the Urban Reign port's src/urban/log.h (namespace, macro prefix and env name renamed, the Dll
// category added).

#include <cstdint>
#include <sstream>
#include <string>
#include <string_view>

namespace godhand::log
{
    enum class Category : uint32_t
    {
        Boot = 1u << 0,
        Loader = 1u << 1,
        File = 1u << 2,
        Ee = 1u << 3,
        Iop = 1u << 4,
        Gs = 1u << 5,
        Vu = 1u << 6,
        Spu2 = 1u << 7,
        Input = 1u << 8,
        Game = 1u << 9,
        Hook = 1u << 10,
        Dll = 1u << 11, // SNR2 DLL loads (function_hooks.cpp, installDllLoadLogger)
    };

    enum class Level
    {
        Error,
        Warn,
        Info,
        Trace,
    };

    // GODHAND_TRACE=BOOT,LOADER,FILE,EE,IOP,GS,VU,SPU2,INPUT,GAME,HOOK,DLL|ALL: categories whose Trace lines print.
    void configureFromEnvironment();
    void setTraceMask(uint32_t mask);
    bool traceEnabled(Category category);
    void write(Category category, Level level, std::string_view message);
    // Seconds since the log started: the time stamp of every log line ("[  12.3456]").
    double elapsedSeconds();
    std::string_view name(Category category);
}

#define GH_LOG(cat, lvl, expr)                                                      \
    do                                                                             \
    {                                                                              \
        if ((lvl) != ::godhand::log::Level::Trace || ::godhand::log::traceEnabled(cat)) \
        {                                                                          \
            std::ostringstream ghLogStream_;                                       \
            ghLogStream_ << expr;                                                  \
            ::godhand::log::write((cat), (lvl), ghLogStream_.str());               \
        }                                                                          \
    } while (0)

#define GH_INFO(cat, expr) GH_LOG(::godhand::log::Category::cat, ::godhand::log::Level::Info, expr)
#define GH_WARN(cat, expr) GH_LOG(::godhand::log::Category::cat, ::godhand::log::Level::Warn, expr)
#define GH_ERROR(cat, expr) GH_LOG(::godhand::log::Category::cat, ::godhand::log::Level::Error, expr)
#define GH_TRACE(cat, expr) GH_LOG(::godhand::log::Category::cat, ::godhand::log::Level::Trace, expr)

// Aliases for code written against the Urban-style names (port_prep/hle-bindings uses GODHAND_*).
#define GODHAND_INFO(cat, expr) GH_INFO(cat, expr)
#define GODHAND_WARN(cat, expr) GH_WARN(cat, expr)
#define GODHAND_ERROR(cat, expr) GH_ERROR(cat, expr)
#define GODHAND_TRACE(cat, expr) GH_TRACE(cat, expr)
