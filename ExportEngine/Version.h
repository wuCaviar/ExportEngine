#pragma once

#include "ExportEngine.h"
#include <cstdint>
#include <string>

// ---- 语义版本 (手动维护) ----
#define EE_VERSION_MAJOR 0
#define EE_VERSION_MINOR 1
#define EE_VERSION_PATCH 0

namespace ATHC::EE {

namespace Version {

constexpr uint32_t major = EE_VERSION_MAJOR;
constexpr uint32_t minor = EE_VERSION_MINOR;
constexpr uint32_t patch = EE_VERSION_PATCH;

// "0.1.0"
inline std::string semantic()
{
    return std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(patch);
}

// ---- 编译时间提取 ----
// __DATE__: "Jun  4 2026"  偏移: [0..2]=月, [4..5]=日, [7..10]=年
// __TIME__: "10:34:00"     偏移: [0..1]=时, [3..4]=分, [6..7]=秒
//
// BUILD_TIMESTAMP is set by CMake at configure time (format: "YYYY-MM-DD hh:mm:ss")
// Falls back to __DATE__ __TIME__ for non-CMake builds.
#ifndef BUILD_TIMESTAMP
#    define BUILD_TIMESTAMP __DATE__ " " __TIME__
#endif

inline std::string buildTimestamp()
{
    return BUILD_TIMESTAMP;
}

// "0.1.0 (2026-06-04 10:34:00)"
inline std::string full()
{
    return semantic() + " (" + buildTimestamp() + ")";
}

} // namespace Version

} // namespace ATHC::EE

// DLL 导出 C 接口 (C linkage, outside namespace)
extern "C" {
EE_API const char *EE_getVersion();
EE_API const char *EE_getBuildTimestamp();
}
