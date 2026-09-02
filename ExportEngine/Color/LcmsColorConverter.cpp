#include "LcmsColorConverter.h"
#include "IccProfile.h"
#include "FileUtil.h"

#include <algorithm>

#include <lcms2.h>

namespace ATHC::EE {

SourceToCmykLcms::~SourceToCmykLcms()
{
    if (m_xform)
        cmsDeleteTransform(static_cast<cmsHTRANSFORM>(m_xform));
}

SourceToCmykLcms::SourceToCmykLcms(SourceToCmykLcms &&o) noexcept : m_xform(o.m_xform), ok(o.ok)
{
    o.m_xform = nullptr;
    o.ok      = false;
}

SourceToCmykLcms &SourceToCmykLcms::operator=(SourceToCmykLcms &&o) noexcept
{
    if (this != &o) {
        if (m_xform)
            cmsDeleteTransform(static_cast<cmsHTRANSFORM>(m_xform));
        m_xform   = o.m_xform;
        ok        = o.ok;
        o.m_xform = nullptr;
        o.ok      = false;
    }
    return *this;
}

void SourceToCmykLcms::convert(const uint8_t *src, uint8_t *dst, int pixels) const
{
    if (m_xform)
        cmsDoTransform(static_cast<cmsHTRANSFORM>(m_xform), src, dst,
                       static_cast<cmsUInt32Number>(pixels));
}

namespace {

// Empty input → nullptr (cmsOpenProfileFromMem(nullptr, 0) would parse an
// empty buffer as garbage instead of failing cleanly).
cmsHPROFILE openProfileFromMem(const std::vector<uint8_t> &bytes)
{
    if (bytes.empty())
        return nullptr;
    return cmsOpenProfileFromMem(bytes.data(), static_cast<cmsUInt32Number>(bytes.size()));
}

} // anonymous namespace

SourceToCmykLcms
makeRgbToCmykLcms(const std::vector<uint8_t> &srcIcc, LcmsColorConverter *cv, int srcBpp)
{
    SourceToCmykLcms t;
    if (!cv || !cv->isLoaded())
        return t;

    std::vector<uint8_t> fallbackRgb, cmykBytes;
    if (!cv->getRgbProfileBytes(fallbackRgb) || !cv->getCMYKProfileBytes(cmykBytes))
        return t;

    cmsHPROFILE src = openProfileFromMem(srcIcc);
    // Embedded profile unparsable / absent → project default RGB profile
    if (!src)
        src = openProfileFromMem(fallbackRgb);
    cmsHPROFILE dst = openProfileFromMem(cmykBytes);

    if (src && dst) {
        cmsUInt32Number inFmt = (srcBpp == 4) ? TYPE_RGBA_8 : TYPE_RGB_8;
        t.m_xform             = cmsCreateTransform(src, inFmt, dst, TYPE_CMYK_8, INTENT_PERCEPTUAL,
                                                   cmsFLAGS_BLACKPOINTCOMPENSATION | cmsFLAGS_HIGHRESPRECALC);
    }
    if (src)
        cmsCloseProfile(src);
    if (dst)
        cmsCloseProfile(dst);
    t.ok = t.m_xform != nullptr;
    return t;
}

SourceToCmykLcms
makeGrayToCmykLcms(const std::vector<uint8_t> &srcIcc, LcmsColorConverter *cv, int srcBpp)
{
    SourceToCmykLcms t;
    if (!cv || !cv->isLoaded())
        return t;

    std::vector<uint8_t> grayBytes, cmykBytes;
    if (!cv->getCMYKProfileBytes(cmykBytes))
        return t;
    // Gray profile absent (2-profile API) → RGB profile's gray ramp
    // (failure leaves grayBytes empty — the transform build fails below)
    if (!cv->getGrayProfileBytes(grayBytes))
        (void)cv->getRgbProfileBytes(grayBytes);

    cmsHPROFILE src = openProfileFromMem(srcIcc);
    if (!src)
        src = openProfileFromMem(grayBytes);
    cmsHPROFILE dst = openProfileFromMem(cmykBytes);

    if (src && dst) {
        cmsUInt32Number inFmt = (srcBpp == 2) ? TYPE_GRAYA_8 : TYPE_GRAY_8;
        t.m_xform             = cmsCreateTransform(src, inFmt, dst, TYPE_CMYK_8, INTENT_PERCEPTUAL,
                                                   cmsFLAGS_BLACKPOINTCOMPENSATION | cmsFLAGS_HIGHRESPRECALC);
    }
    if (src)
        cmsCloseProfile(src);
    if (dst)
        cmsCloseProfile(dst);
    t.ok = t.m_xform != nullptr;
    return t;
}

bool convertChunked(const SourceToCmykLcms &lcms,
                    const uint8_t          *src,
                    int                     w,
                    int                     h,
                    int                     srcBpp,
                    std::vector<uint8_t>   &cmykOut,
                    int                     rowsPerChunk)
{
    if (!lcms.ok || !src || w <= 0 || h <= 0 || srcBpp <= 0 || rowsPerChunk <= 0)
        return false;
    cmykOut.resize(static_cast<size_t>(w) * h * 4);
    const uint8_t *s = src;
    uint8_t       *d = cmykOut.data();
    for (int y = 0; y < h; y += rowsPerChunk) {
        const int rows = std::min(rowsPerChunk, h - y);
        lcms.convert(s, d, w * rows);
        s += static_cast<size_t>(w) * rows * srcBpp;
        d += static_cast<size_t>(w) * rows * 4;
    }
    return true;
}

LcmsColorConverter::LcmsColorConverter()
{
    m_srgbProfile = cmsCreate_sRGBProfile();
}

LcmsColorConverter::LcmsColorConverter(CloneTag)
{
    // Clone constructor — profiles are shared from source.
    m_ownsProfiles = false;
}

LcmsColorConverter::~LcmsColorConverter()
{
    cleanup();
}

void LcmsColorConverter::cleanup()
{
    if (!m_ownsProfiles)
        return;
    if (m_rgbProfile) {
        cmsCloseProfile(static_cast<cmsHPROFILE>(m_rgbProfile));
        m_rgbProfile = nullptr;
    }
    if (m_cmykProfile) {
        cmsCloseProfile(static_cast<cmsHPROFILE>(m_cmykProfile));
        m_cmykProfile = nullptr;
    }
    if (m_grayProfile) {
        cmsCloseProfile(static_cast<cmsHPROFILE>(m_grayProfile));
        m_grayProfile = nullptr;
    }
    if (m_srgbProfile) {
        cmsCloseProfile(static_cast<cmsHPROFILE>(m_srgbProfile));
        m_srgbProfile = nullptr;
    }
}

std::error_code LcmsColorConverter::loadProfile(const std::string &rgbProfilePath,
                                                const std::string &cmykProfilePath)
{
    m_errorCode = {};
    cleanup();

    // cleanup 已释放 sRGB profile，此处重新创建
    m_srgbProfile = cmsCreate_sRGBProfile();

    // 加载 RGB 源 profile（UTF-8 路径 → 内存 → cmsOpenProfileFromMem，
    // 避免 cmsOpenProfileFromFile → fopen() 在 Windows 上将 UTF-8 路径
    // 错误解释为 ANSI 代码页导致的文件找不到问题）
    {
        std::string err;
        auto        data = FileUtil::readFileUtf8(rgbProfilePath, err);
        if (data.empty()) {
            cleanup();
            return EEError::icc_open_failed;
        }
        m_rgbProfile =
            cmsOpenProfileFromMem(data.data(), static_cast<cmsUInt32Number>(data.size()));
        if (!m_rgbProfile) {
            cleanup();
            return EEError::icc_parse_failed;
        }
    }

    // 加载 CMYK 目标 profile（同上，UTF-8 路径 → 内存 → cmsOpenProfileFromMem）
    {
        std::string err;
        auto        data = FileUtil::readFileUtf8(cmykProfilePath, err);
        if (data.empty()) {
            cleanup();
            return EEError::icc_open_failed;
        }
        m_cmykProfile =
            cmsOpenProfileFromMem(data.data(), static_cast<cmsUInt32Number>(data.size()));
        if (!m_cmykProfile) {
            cleanup();
            return EEError::icc_parse_failed;
        }
    }

    return {};
}

std::error_code LcmsColorConverter::loadProfile(const std::string &rgbProfilePath,
                                                const std::string &cmykProfilePath,
                                                const std::string &grayProfilePath)
{
    m_errorCode = {};
    if (auto ec = loadProfile(rgbProfilePath, cmykProfilePath))
        return ec;

    // 加载 Gray profile（UTF-8 路径 → 内存 → cmsOpenProfileFromMem，
    // 避免 cmsOpenProfileFromFile → fopen() 在 Windows 上将 UTF-8 路径
    // 错误解释为 ANSI 代码页导致的文件找不到问题）。
    // 灰度图→CMYK 的 transform 由 ImageRenderer 按源图构建，此处仅加载
    // profile 供 getGrayProfileBytes() 使用。
    {
        std::string err;
        auto        data = FileUtil::readFileUtf8(grayProfilePath, err);
        if (data.empty()) {
            return EEError::icc_open_failed;
        }
        m_grayProfile =
            cmsOpenProfileFromMem(data.data(), static_cast<cmsUInt32Number>(data.size()));
        if (!m_grayProfile) {
            return EEError::icc_parse_failed;
        }
    }

    return {};
}

bool LcmsColorConverter::getCMYKProfileBytes(std::vector<uint8_t> &out)
{
    m_errorCode = {};
    if (!m_cmykProfile) {
        m_errorCode = EEError::icc_not_initialized;
        return false;
    }

    cmsUInt32Number bytesNeeded = 0;
    cmsSaveProfileToMem(static_cast<cmsHPROFILE>(m_cmykProfile), nullptr, &bytesNeeded);
    if (bytesNeeded == 0) {
        m_errorCode = EEError::icc_not_initialized;
        return false;
    }

    out.resize(bytesNeeded);
    cmsSaveProfileToMem(static_cast<cmsHPROFILE>(m_cmykProfile), out.data(), &bytesNeeded);
    return true;
}

bool LcmsColorConverter::getRgbProfileBytes(std::vector<uint8_t> &out)
{
    m_errorCode = {};
    // 加载的 RGB 源 profile（如 SRGB IEC61966-2.1.icc）优先，
    // 未加载时回退到内置 sRGB profile。
    cmsHPROFILE prof = static_cast<cmsHPROFILE>(m_rgbProfile ? m_rgbProfile : m_srgbProfile);
    if (!prof) {
        m_errorCode = EEError::icc_not_initialized;
        return false;
    }

    cmsUInt32Number bytesNeeded = 0;
    cmsSaveProfileToMem(prof, nullptr, &bytesNeeded);
    if (bytesNeeded == 0) {
        m_errorCode = EEError::icc_not_initialized;
        return false;
    }

    out.resize(bytesNeeded);
    cmsSaveProfileToMem(prof, out.data(), &bytesNeeded);
    return true;
}

bool LcmsColorConverter::getGrayProfileBytes(std::vector<uint8_t> &out)
{
    m_errorCode = {};
    if (!m_grayProfile) {
        m_errorCode = EEError::icc_not_initialized;
        return false;
    }

    cmsUInt32Number bytesNeeded = 0;
    cmsSaveProfileToMem(static_cast<cmsHPROFILE>(m_grayProfile), nullptr, &bytesNeeded);
    if (bytesNeeded == 0) {
        m_errorCode = EEError::icc_not_initialized;
        return false;
    }

    out.resize(bytesNeeded);
    cmsSaveProfileToMem(static_cast<cmsHPROFILE>(m_grayProfile), out.data(), &bytesNeeded);
    return true;
}

std::unique_ptr<IColorConverter> LcmsColorConverter::cloneForThread() const
{
    if (!m_cmykProfile)
        return nullptr;

    auto clone = std::make_unique<LcmsColorConverter>(CloneTag{});

    // Share the read-only profiles from the source.
    clone->m_srgbProfile = m_srgbProfile;
    clone->m_rgbProfile  = m_rgbProfile;
    clone->m_cmykProfile = m_cmykProfile;
    clone->m_grayProfile = m_grayProfile;

    return clone;
}

std::shared_ptr<LcmsColorConverter> LcmsColorConverter::createDefault()
{
    auto cv = std::make_shared<LcmsColorConverter>();
    cv->loadDefaultProfiles();
    return cv;
}

void LcmsColorConverter::loadDefaultProfiles()
{
    // 失败由 isLoaded()/getProfileBytes() 后续判断；此处显式丢弃 [[nodiscard]] 返回值。
    (void)loadProfile(IccProfile::resolve(IccProfile::kDefaultRgb),
                      IccProfile::resolve(IccProfile::kDefaultCmyk),
                      IccProfile::resolve(IccProfile::kDefaultGray));
}

std::unique_ptr<IColorTransform>
LcmsColorConverter::makeToCmyk(PixelColorSpace src, const std::vector<uint8_t> &embeddedIcc)
{
    switch (src) {
    case PixelColorSpace::Rgb:
        return std::make_unique<SourceToCmykLcms>(makeRgbToCmykLcms(embeddedIcc, this, 3));
    case PixelColorSpace::Rgba:
        return std::make_unique<SourceToCmykLcms>(makeRgbToCmykLcms(embeddedIcc, this, 4));
    case PixelColorSpace::Gray:
        return std::make_unique<SourceToCmykLcms>(makeGrayToCmykLcms(embeddedIcc, this, 1));
    case PixelColorSpace::GrayA:
        return std::make_unique<SourceToCmykLcms>(makeGrayToCmykLcms(embeddedIcc, this, 2));
    case PixelColorSpace::Cmyk:
        return nullptr; // 直通（调用方原样拷贝，不转换）
    }
    return nullptr;
}
} // namespace ATHC::EE
