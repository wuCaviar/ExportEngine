#pragma once

#include <spdlog/spdlog.h>

// Global logger singleton with console output.
// Call initLogger() once at process start before any logging call.
//
// Core state (logger instance + initialisation) lives in the ExportEngine
// DLL via Log.cpp  — every .exe that links the DLL shares a single
// spdlog::stdout_color_mt() call, avoiding duplicate-registration issues
// with vcpkg's compiled-lib spdlog.

namespace ATHC::EE {

namespace EELog {

// Non-inline — single instance inside ExportEngine.dll.
spdlog::logger &get();
void            initLogger();
void            setLevel(spdlog::level::level_enum lv);

// Inline convenience helpers are thin wrappers around get() and can stay
// header-only without duplicating logger state.
template<class... Args>
void info(spdlog::format_string_t<Args...> fmt, Args &&...args)
{
    get().info(fmt, std::forward<Args>(args)...);
}

template<class... Args>
void warn(spdlog::format_string_t<Args...> fmt, Args &&...args)
{
    get().warn(fmt, std::forward<Args>(args)...);
}

template<class... Args>
void error(spdlog::format_string_t<Args...> fmt, Args &&...args)
{
    get().error(fmt, std::forward<Args>(args)...);
}

} // namespace EELog

} // namespace ATHC::EE
