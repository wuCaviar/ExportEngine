#pragma once

/// Json2Tiff — ExportEngine 唯一对外接口
///
/// 将 JSON 场景文件渲染为 CMYK 像素，并流入可插拔的渲染输出接收器（IRenderSink）。
/// 颜色转换 / 像素缩放 / 输出接收器均由 RenderServices 注入，EE 只做数据整合。
///
/// 用法:
///   #include "Json2Tiff.h"
///   using namespace ATHC::EE;
///
///   // 写 TIFF（便捷入口）
///   bool ok = json2tiff("scene.json", "output.tif",
///       [](int cur, int total, const std::string &msg) {
///           printf("[%d/%d] %s\n", cur, total, msg.c_str());
///       },
///       [](std::error_code ec) {
///           fprintf(stderr, "Error: %s\n", ec.message().c_str());
///       });
///
///   // 任意输出接收器 + 可插拔服务
///   RenderServices services;
///   services.sink = std::make_shared<MemorySink>();
///   json2sink("scene.json", services);

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "ExportEngine.h"
#include "EEError.h"
#include "IColorConverter.h"
#include "IResampler.h"
#include "IRenderSink.h"

namespace ATHC::EE {

/// 进度回调: (当前进度, 总进度, 描述)
/// total 恒为 100（百分比语义），可在任意阶段多次回调、单调递增。
/// 消息示例: "Decoding images (12/85): a.jpg"、"Rendering: strip 120/4000"。
using ProgressCallback = std::function<void(int current, int total, const std::string &message)>;

/// 错误回调: (错误码)
using ErrorCallback = std::function<void(std::error_code error)>;

/// 一次导出过程共享的可插拔服务（策略）。
/// 成员为空 → 引擎用内置默认（LcmsColorConverter = LittleCMS2，VipsResampler = vips 最近邻）。
/// 传入自定义实现即可替换颜色转换 / 像素缩放 / 渲染输出接收器，EE 只做数据整合。
struct RenderServices
{
    std::shared_ptr<IColorConverter> color;     // 颜色转换引擎（源 → CMYK）
    std::shared_ptr<IResampler>      resampler; // 像素缩放
    std::shared_ptr<IRenderSink>     sink;      // 渲染输出接收器（必需）
};

/// JSON 场景文件 → TIFF（便捷入口）。
/// 内部使用默认颜色转换/缩放引擎 + TiffSink（strip 条带存储）；需要自定义服务时请用 json2sink。
/// @param jsonPath     输入 .json 场景文件路径 (UTF-8)
/// @param tiffPath     输出 .tif 文件路径 (UTF-8)
/// @param progress     进度回调 (nullptr 表示不需要进度通知)
/// @param onError      错误回调 (nullptr 表示不需要错误通知)。回调收到 std::error_code
///                    （EEError 域或 system/filesystem 域），宿主可用 ec == EEError::x 分类、
///                    ATHC::EE::isRecoverable(ec) 判断可恢复性
/// @return true 成功, false 失败
EE_API bool json2tiff(const std::string &jsonPath,
                      const std::string &tiffPath,
                      ProgressCallback   progress = nullptr,
                      ErrorCallback      onError  = nullptr);

/// JSON 场景文件 → 任意渲染输出接收器（services.sink）。
///
/// 与 json2tiff 共用同一条流水线（解析 → 预扫描 → ICC → 预解码 → 渲染），
/// 颜色转换/缩放/输出接收器全部由 services 注入，成员为空时使用引擎默认。
/// 内置 TiffSink/MemorySink/CallbackSink/NullSink，也可传入自定义实现。
///
/// @param jsonPath     输入 .json 场景文件路径 (UTF-8)
/// @param services     可插拔服务；services.sink 为输出接收器（必需，由调用方构造）
/// @param progress     进度回调 (nullptr 表示不需要)
/// @param onError      错误回调 (nullptr 表示不需要)
/// @return true 成功, false 失败
EE_API bool json2sink(const std::string    &jsonPath,
                      const RenderServices &services,
                      ProgressCallback      progress = nullptr,
                      ErrorCallback         onError  = nullptr);

} // namespace ATHC::EE
