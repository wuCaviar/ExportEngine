#include "ColorConverter.h"
#include "ColorTransform.h" // SourceToCmykLcms / makeRgbToCmykLcms / makeGrayToCmykLcms
#include "IccProfile.h"

#include <lcms2.h>

#include <filesystem>
#include <fstream>
#include <vector>

using namespace ATHC::EE;

namespace {

/// Read a file whose path is UTF-8 into a byte vector.
///
/// On Windows, cmsOpenProfileFromFile() calls fopen() internally, which
/// interprets paths in the system ANSI code page (e.g. GBK on Chinese
/// Windows).  JSON scene files store paths as UTF-8, so Chinese characters
/// get misinterpreted and the file is not found.
///
/// This helper uses std::filesystem::u8path() to correctly convert the
/// UTF-8 path to the internal wide representation, then reads the file
/// through the std::ifstream(path) overload (which uses _wfopen on MSVC).
std::vector<uint8_t> readFileUtf8(const std::string &utf8Path, std::string &errMsg)
{
    std::error_code ec;
    auto p = std::filesystem::u8path(utf8Path);
    auto sz = std::filesystem::file_size(p, ec);
    if (ec) {
        errMsg = ec.message();
        return {};
    }
    std::ifstream ifs(p, std::ios::binary);
    if (!ifs) {
        errMsg = "Cannot open file";
        return {};
    }
    std::vector<uint8_t> buf(static_cast<size_t>(sz));
    ifs.read(reinterpret_cast<char *>(buf.data()), sz);
    if (!ifs) {
        errMsg = "Read error";
        return {};
    }
    return buf;
}

} // anonymous namespace

ColorConverter::ColorConverter()
{
    m_srgbProfile = cmsCreate_sRGBProfile();
}

ColorConverter::ColorConverter(CloneTag)
{
    // Clone constructor — profiles are shared from source.
    m_ownsProfiles = false;
}

ColorConverter::~ColorConverter()
{
    cleanup();
}

void ColorConverter::cleanup()
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

std::error_code ColorConverter::loadProfile(
    const std::string &rgbProfilePath, const std::string &cmykProfilePath)
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
        auto data = readFileUtf8(rgbProfilePath, err);
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
        auto data = readFileUtf8(cmykProfilePath, err);
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

std::error_code ColorConverter::loadProfile(const std::string &rgbProfilePath,
    const std::string &cmykProfilePath, const std::string &grayProfilePath)
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
        auto data = readFileUtf8(grayProfilePath, err);
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

bool ColorConverter::getProfileBytes(std::vector<uint8_t> &out)
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

bool ColorConverter::getRgbProfileBytes(std::vector<uint8_t> &out)
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

bool ColorConverter::getGrayProfileBytes(std::vector<uint8_t> &out)
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

std::unique_ptr<IColorConverter> ColorConverter::cloneForThread() const
{
    if (!m_cmykProfile)
        return nullptr;

    auto clone = std::make_unique<ColorConverter>(CloneTag{});

    // Share the read-only profiles from the source.
    clone->m_srgbProfile = m_srgbProfile;
    clone->m_rgbProfile = m_rgbProfile;
    clone->m_cmykProfile = m_cmykProfile;
    clone->m_grayProfile = m_grayProfile;

    return clone;
}

std::shared_ptr<ColorConverter> ColorConverter::createDefault()
{
    auto cv = std::make_shared<ColorConverter>();
    cv->loadDefaultProfiles();
    return cv;
}

void ColorConverter::loadDefaultProfiles()
{
    // 失败由 isLoaded()/getProfileBytes() 后续判断；此处显式丢弃 [[nodiscard]] 返回值。
    (void)loadProfile(IccProfile::resolve(IccProfile::kDefaultRgb),
        IccProfile::resolve(IccProfile::kDefaultCmyk),
        IccProfile::resolve(IccProfile::kDefaultGray));
}

std::unique_ptr<IColorTransform> ColorConverter::makeToCmyk(
    PixelColorSpace src, const std::vector<uint8_t> &embeddedIcc)
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
