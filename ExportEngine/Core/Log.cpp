#include "Log.h"
#include <spdlog/sinks/stdout_color_sinks.h>

#ifdef _WIN32
#    include <windows.h>
#endif

namespace ATHC::EE {
namespace EELog {

spdlog::logger &get()
{
    static auto logger = spdlog::stdout_color_mt("ExportEngine");
    return *logger;
}

void initLogger()
{
    get().set_level(spdlog::level::info);
    get().set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v");

#ifdef _WIN32
    // Windows console defaults to GBK (cp936), switch to UTF-8 (cp65001)
    // so Chinese characters in log output render correctly.
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif
}

void setLevel(spdlog::level::level_enum lv)
{
    get().set_level(lv);
}

} // namespace EELog
} // namespace ATHC::EE
