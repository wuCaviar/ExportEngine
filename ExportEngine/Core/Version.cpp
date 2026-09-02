#include "Version.h"

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

static const std::string s_version = ATHC::EE::Version::full();
static const std::string s_ts      = ATHC::EE::Version::buildTimestamp();

extern "C" {

const char *EE_getVersion()
{
    return s_version.c_str();
}

const char *EE_getBuildTimestamp()
{
    return s_ts.c_str();
}
}
