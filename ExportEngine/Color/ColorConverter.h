#pragma once

#include "EEError.h"
#include "IColorConverter.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace ATHC::EE {

// 默认颜色转换引擎（LittleCMS2），实现 IColorConverter 接口。
class EE_API ColorConverter : public IColorConverter
{
public:
    ColorConverter();
    ~ColorConverter() override;

    // 创建并加载默认 RGB/CMYK/Gray profile 的引擎（<moduleDir>/ICC Profile/）。
    static std::shared_ptr<ColorConverter> createDefault();

    // ── IColorConverter 接口 ──
    std::unique_ptr<IColorTransform> makeToCmyk(
        PixelColorSpace src, const std::vector<uint8_t> &embeddedIcc) override;
    std::unique_ptr<IColorConverter> cloneForThread() const override;

    // 加载 RGB + CMYK ICC profile
    // rgbProfilePath: RGB 源 profile 路径 (用于 RGB->CMYK 转换)
    // cmykProfilePath: CMYK 目标 profile 路径 (用于转换 + 嵌入 TIFF)
    [[nodiscard]] std::error_code loadProfile(
        const std::string &rgbProfilePath, const std::string &cmykProfilePath);

    // 加载 RGB + CMYK + Gray ICC profile (三通道完整加载)
    [[nodiscard]] std::error_code loadProfile(const std::string &rgbProfilePath,
        const std::string &cmykProfilePath, const std::string &grayProfilePath);

    // 获取已加载的 CMYK ICC profile 的原始字节 (用于嵌入 TIFF)
    [[nodiscard]] bool getProfileBytes(std::vector<uint8_t> &out);

    // 获取已加载的 RGB 源 profile 的原始字节 (用于图片 RGB→CMYK 转换)
    [[nodiscard]] bool getRgbProfileBytes(std::vector<uint8_t> &out);

    // 获取已加载的 Gray 源 profile 的原始字节 (用于灰度图片→CMYK 转换)
    [[nodiscard]] bool getGrayProfileBytes(std::vector<uint8_t> &out);

    bool isLoaded() const { return m_cmykProfile != nullptr; }
    // 转换类方法失败的码（当前唯一失败模式：icc_not_initialized）。
    // 每次公共方法调用入口处清零，失败时置码，成功路径保持为空；单线程使用。
    std::error_code errorCode() const { return m_errorCode; }

    // Tag type for clone constructor — skips default sRGB profile creation.
    struct CloneTag
    { };
    ColorConverter(CloneTag);

private:
    void *m_srgbProfile = nullptr; // cmsHPROFILE (内置 sRGB，仅作 fallback)
    void *m_rgbProfile = nullptr; // cmsHPROFILE (从文件加载的 RGB profile)
    void *m_cmykProfile = nullptr; // cmsHPROFILE
    void *m_grayProfile = nullptr; // cmsHPROFILE (Gray profile，用于灰度图→CMYK)
    std::error_code m_errorCode;
    bool m_ownsProfiles = true;

    void cleanup();
    void loadDefaultProfiles();
};

} // namespace ATHC::EE
