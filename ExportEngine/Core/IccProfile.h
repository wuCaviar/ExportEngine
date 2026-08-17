#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ATHC::EE {
namespace IccProfile {

// ============================================================================
//  IccProfile — ICC 配置文件定位与读取（EE 不关心，供策略内部使用）
// ============================================================================
//  ColorConverter（默认颜色引擎）与 TiffSink（写 TIFF）各自内部加载自己需要的
//  ICC profile，不经过 EE。文件位于 <ExportEngine.dll 所在目录>/ICC Profile/。
// ============================================================================

// 默认 ICC（相对 <moduleDir>/ICC Profile/）。
inline constexpr const char *kDefaultRgb = "RGB/SRGB IEC61966-2.1.icc";
inline constexpr const char *kDefaultCmyk = "CMYK/JapanColor2001Coated.icc";
inline constexpr const char *kDefaultGray = "Gray/Dot Gain 15%.icc";

// 解析 <moduleDir>/ICC Profile/<profile> 为规范路径（带沙箱检查）。
// 失败（路径越界/不可解析）返回空串。
std::string resolve(const std::string &profile);

// 读取 <moduleDir>/ICC Profile/<profile> 的字节。失败（文件不存在/不可读）返回空。
std::vector<uint8_t> readBytes(const std::string &profile);

} // namespace IccProfile
} // namespace ATHC::EE
