#pragma once

#include "EEError.h"
#include "IColorConverter.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace ATHC::EE {

class LcmsColorConverter;
// ============================================================================
//  Source-to-CMYK lcms2 transform builders
// ============================================================================
//  Shared by ImageRenderer (image primitives) and TextureSource (rect
//  texture fills): both decode a source image, then convert its pixels to
//  the project CMYK profile (the same one embedded in the output TIFF)
//  through lcms2.
//
//  Source profile preference: embedded source ICC → project source profile
//  (RGB or Gray) → project CMYK profile (JapanColor2001Coated.icc).
//  A transform is created once per source image and reused per strip/tile.
//  vips is NOT used for colourspace conversion — decoding and geometry only
//  — so every source shares one colour engine.
//
//  No lcms2 types appear in this header: the transform handle is stored as
//  an opaque void* and every lcms2 call lives in ColorTransform.cpp, so
//  consumers don't need the lcms2 headers to include this.
//
//  lcms2 cmsHTRANSFORM is not thread-safe; each thread that converts must
//  build its own transform (callers build one per source image, and stream
//  pixels through it band-by-band at render time).
// ============================================================================

/// One lcms2 transform handle (move-only). `ok` is false when no transform
/// could be built (e.g. converter not loaded) — callers treat the source
/// image as failed to decode.
class SourceToCmykLcms : public IColorTransform
{
public:
    SourceToCmykLcms() = default;
    ~SourceToCmykLcms() override;

    // Move-only: default copy/move would leave the source's transform alive,
    // causing a double-free when the temporary is destroyed after
    // move-assignment (`lcms = makeRgbToCmykLcms(...)`).
    SourceToCmykLcms(const SourceToCmykLcms &)            = delete;
    SourceToCmykLcms &operator=(const SourceToCmykLcms &) = delete;
    SourceToCmykLcms(SourceToCmykLcms &&o) noexcept;
    SourceToCmykLcms &operator=(SourceToCmykLcms &&o) noexcept;

    bool ok = false;

    void convert(const uint8_t *src, uint8_t *dst, int pixels) const override;

private:
    friend SourceToCmykLcms
    makeRgbToCmykLcms(const std::vector<uint8_t> &srcIcc, LcmsColorConverter *cv, int srcBpp);
    friend SourceToCmykLcms
    makeGrayToCmykLcms(const std::vector<uint8_t> &srcIcc, LcmsColorConverter *cv, int srcBpp);

    void *m_xform = nullptr; // cmsHTRANSFORM (opaque, owned — see ColorTransform.cpp)
};

/// Convert a band-interleaved source buffer (w×h pixels, srcBpp bytes per
/// pixel) to CMYK in row chunks of `rowsPerChunk` rows. Byte-identical to a
/// single whole-image convert() call — cmsDoTransform is stateless per
/// pixel — while letting callers avoid holding a second full-size source
/// buffer. Returns false if the transform is not ok or the geometry is
/// inconsistent.
bool convertChunked(const SourceToCmykLcms &lcms,
                    const uint8_t          *src,
                    int                     w,
                    int                     h,
                    int                     srcBpp,
                    std::vector<uint8_t>   &cmykOut,
                    int                     rowsPerChunk = 256);

/// Build the RGB→CMYK transform. srcIcc = embedded source profile bytes
/// (empty = none); cv provides the fallback RGB and the CMYK profiles.
/// srcBpp selects the input format: 3 = TYPE_RGB_8, 4 = TYPE_RGBA_8
/// (alpha band is ignored by lcms). Rendering intent matches
/// LcmsColorConverter (perceptual).
SourceToCmykLcms
makeRgbToCmykLcms(const std::vector<uint8_t> &srcIcc, LcmsColorConverter *cv, int srcBpp);

/// Build the Gray→CMYK transform. srcIcc = embedded source profile bytes
/// (empty = none); cv provides the project Gray profile (fallback: the RGB
/// profile used as a gray ramp when only two profiles are loaded) and the
/// CMYK profile. srcBpp selects the input format: 1 = TYPE_GRAY_8,
/// 2 = TYPE_GRAYA_8 (alpha band is ignored by lcms).
SourceToCmykLcms
makeGrayToCmykLcms(const std::vector<uint8_t> &srcIcc, LcmsColorConverter *cv, int srcBpp);

///////////////////////////////////////////////////////////////////////////////

// 默认颜色转换引擎（LittleCMS2），实现 IColorConverter 接口。
class EE_API LcmsColorConverter : public IColorConverter
{
public:
    LcmsColorConverter();
    ~LcmsColorConverter() override;

    // 创建并加载默认 RGB/CMYK/Gray profile 的引擎（<moduleDir>/ICC Profile/）。
    static std::shared_ptr<LcmsColorConverter> createDefault();

    // ── IColorConverter 接口 ──
    std::unique_ptr<IColorTransform> makeToCmyk(PixelColorSpace             src,
                                                const std::vector<uint8_t> &embeddedIcc) override;

    std::unique_ptr<IColorConverter> cloneForThread() const override;

    // 加载 RGB + CMYK ICC profile
    // rgbProfilePath: RGB 源 profile 路径 (用于 RGB->CMYK 转换)
    // cmykProfilePath: CMYK 目标 profile 路径 (用于转换 + 嵌入 TIFF)
    [[nodiscard]] std::error_code loadProfile(const std::string &rgbProfilePath,
                                              const std::string &cmykProfilePath);

    // 加载 RGB + CMYK + Gray ICC profile (三通道完整加载)
    [[nodiscard]] std::error_code loadProfile(const std::string &rgbProfilePath,
                                              const std::string &cmykProfilePath,
                                              const std::string &grayProfilePath);

    // 获取已加载的 CMYK ICC profile 的原始字节 (用于嵌入 TIFF)
    [[nodiscard]] bool getCMYKProfileBytes(std::vector<uint8_t> &out);

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
    LcmsColorConverter(CloneTag);

private:
    void           *m_srgbProfile = nullptr; // cmsHPROFILE (内置 sRGB，仅作 fallback)
    void           *m_rgbProfile  = nullptr; // cmsHPROFILE (从文件加载的 RGB profile)
    void           *m_cmykProfile = nullptr; // cmsHPROFILE
    void           *m_grayProfile = nullptr; // cmsHPROFILE (Gray profile，用于灰度图→CMYK)
    std::error_code m_errorCode;
    bool            m_ownsProfiles = true;

    void cleanup();
    void loadDefaultProfiles();
};

} // namespace ATHC::EE
