#pragma once

#include <spdlog/spdlog.h>

// 带控制台输出的全局日志单例。
// 在进程启动时调用 initLogger() 一次，在任何日志调用之前。
//
// 核心状态（日志实例 + 初始化）位于 ExportEngine DLL 的 Log.cpp 中——
// 每个链接该 DLL 的.exe 共享单个 spdlog::stdout_color_mt() 调用，
// 避免与 vcpkg 编译库版 spdlog 出现重复注册问题。

namespace ATHC::EE {

namespace EELog {

// 非内联——ExportEngine.dll 内的单一实例。
spdlog::logger &get();
void initLogger();
void setLevel(spdlog::level::level_enum lv);

// 内联便捷辅助函数是 get() 的薄包装，可保留为头文件形式而不会重复日志状态。
template<class... Args> void info(spdlog::format_string_t<Args...> fmt, Args &&...args)
{
    get().info(fmt, std::forward<Args>(args)...);
}

template<class... Args> void warn(spdlog::format_string_t<Args...> fmt, Args &&...args)
{
    get().warn(fmt, std::forward<Args>(args)...);
}

template<class... Args> void error(spdlog::format_string_t<Args...> fmt, Args &&...args)
{
    get().error(fmt, std::forward<Args>(args)...);
}

} // namespace EELog

} // namespace ATHC::EE
