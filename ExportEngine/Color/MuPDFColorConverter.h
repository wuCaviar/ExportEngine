#pragma once

#include "EEError.h"
#include "IColorConverter.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace ATHC::EE {

class MuPDFColorConverter;

// ============================================================================
//  Source-to-CMYK MuPDF transform builders
// ============================================================================
//  与 lcms2 版本相同的语义：
//    - 每个源图构建一次 transform；
//    - transform 非线程安全；
//    - RGBA / GRAYA 的 alpha 不参与颜色转换；
//    - CMYK 源图直通，由调用方拷贝；
//    - 优先使用 embedded ICC，其次使用工程 RGB/Gray ICC，最后 fallback。
//
//  头文件中不出现 MuPDF 类型，所有 MuPDF 句柄用 void* 保存。
// ============================================================================

class SourceToCmykMuPdf : public IColorTransform
{
public:
    SourceToCmykMuPdf() = default;
    ~SourceToCmykMuPdf() override;

    // Move-only.
    SourceToCmykMuPdf(const SourceToCmykMuPdf &) = delete;
    SourceToCmykMuPdf &operator=(const SourceToCmykMuPdf &) = delete;

    SourceToCmykMuPdf(SourceToCmykMuPdf &&o) noexcept;
    SourceToCmykMuPdf &operator=(SourceToCmykMuPdf &&o) noexcept;

    bool ok = false;

    void convert(const uint8_t *src, uint8_t *dst, int pixels) const override;

private:
    friend SourceToCmykMuPdf makeRgbToCmykMuPdf(
        const std::vector<uint8_t> &srcIcc, MuPDFColorConverter *cv, int srcBpp);

    friend SourceToCmykMuPdf makeGrayToCmykMuPdf(
        const std::vector<uint8_t> &srcIcc, MuPDFColorConverter *cv, int srcBpp);

    void release() noexcept;

    void *m_ctx = nullptr; // fz_context*
    void *m_srcCS = nullptr; // fz_colorspace*
    void *m_dstCS = nullptr; // fz_colorspace*
    int m_srcBpp = 0; // 3/4 for RGB, 1/2 for Gray
};

/// Convert a band-interleaved source buffer to CMYK in row chunks.
bool convertChunked(const SourceToCmykMuPdf &xform, const uint8_t *src, int w, int h, int srcBpp,
    std::vector<uint8_t> &cmykOut, int rowsPerChunk = 256);

/// Build RGB(A) -> CMYK transform.
SourceToCmykMuPdf makeRgbToCmykMuPdf(
    const std::vector<uint8_t> &srcIcc, MuPDFColorConverter *cv, int srcBpp);

/// Build Gray(A) -> CMYK transform.
SourceToCmykMuPdf makeGrayToCmykMuPdf(
    const std::vector<uint8_t> &srcIcc, MuPDFColorConverter *cv, int srcBpp);

///////////////////////////////////////////////////////////////////////////////

// 默认颜色转换引擎（MuPDF），实现 IColorConverter 接口。
class EE_API MuPDFColorConverter : public IColorConverter
{
public:
    MuPDFColorConverter();
    ~MuPDFColorConverter() override;

    // 创建并加载默认 RGB/CMYK/Gray profile 的引擎（<moduleDir>/ICC Profile/）。
    static std::shared_ptr<MuPDFColorConverter> createDefault();

    // ── IColorConverter 接口 ──
    std::unique_ptr<IColorTransform> makeToCmyk(
        PixelColorSpace src, const std::vector<uint8_t> &embeddedIcc) override;

    std::unique_ptr<IColorConverter> cloneForThread() const override;

    // 加载 RGB + CMYK ICC profile
    [[nodiscard]] std::error_code loadProfile(
        const std::string &rgbProfilePath, const std::string &cmykProfilePath);

    // 加载 RGB + CMYK + Gray ICC profile
    [[nodiscard]] std::error_code loadProfile(const std::string &rgbProfilePath,
        const std::string &cmykProfilePath, const std::string &grayProfilePath);

    // 获取已加载的 CMYK ICC profile 的原始字节（用于嵌入 TIFF）
    [[nodiscard]] bool getProfileBytes(std::vector<uint8_t> &out);

    // 获取已加载的 RGB 源 profile 的原始字节
    [[nodiscard]] bool getRgbProfileBytes(std::vector<uint8_t> &out);

    // 获取已加载的 Gray 源 profile 的原始字节
    [[nodiscard]] bool getGrayProfileBytes(std::vector<uint8_t> &out);

    bool isLoaded() const { return m_cmykCS != nullptr; }

    std::error_code errorCode() const { return m_errorCode; }

    // Tag type for clone constructor — skips default context/profile creation.
    struct CloneTag
    { };
    MuPDFColorConverter(CloneTag);

    // ── Internal use only: for MuPDF transform builders ──
    // 这两个方法不属于 IColorConverter 接口，只是为了 MuPDF 实现内部使用。
    void *internalContext() const { return m_ctx; }

    void clearErrorCode() { m_errorCode = {}; }

private:
    bool ensureContext();
    void cleanupProfiles();
    void cleanup();
    void loadDefaultProfiles();

    // MuPDF 句柄，全部用 void* 保存，避免头文件依赖 mupdf。
    void *m_ctx = nullptr; // fz_context*
    void *m_srgbCS = nullptr; // fz_colorspace*，device RGB fallback
    void *m_rgbCS = nullptr; // fz_colorspace*
    void *m_cmykCS = nullptr; // fz_colorspace*
    void *m_grayCS = nullptr; // fz_colorspace*

    // 保留原始 ICC 字节，便于嵌入 TIFF、cloneForThread、fallback。
    std::vector<uint8_t> m_rgbBytes;
    std::vector<uint8_t> m_cmykBytes;
    std::vector<uint8_t> m_grayBytes;

    std::error_code m_errorCode;
};

} // namespace ATHC::EE