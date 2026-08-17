#include "TextureSource.h"
#include "IColorConverter.h"
#include "Log.h"
#include "VipsUtil.h" // extractVipsImageDpi
#include "TiffHelper.h"

#include <tiffio.h>
#include <vips/vips.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>

using namespace ATHC::EE;

std::mutex TextureSource::s_mtx;
std::unordered_map<std::string, TextureSource::CachedFrame> TextureSource::s_cache;
std::unordered_set<std::string> TextureSource::s_badFiles;
std::unordered_set<std::string> TextureSource::s_warned;

namespace {

// warnOnce 限频集合容量上限（仿 ImageRenderer.cpp 的 CappedFileSet）。
constexpr size_t kMaxWarned = 1024;

// 帧每像素通道数：C、M、Y、K、A 共 5 通道。
// （brief 注释里的 "×4" 与 sample() 的 5 分量 CMYK+A 输出矛盾——
// 唯一自洽的布局是 5 字节/像素，sample 步长为 5。）
constexpr int kChannelsPerPixel = 5;
constexpr size_t kChannelsPerPixelS = 5;

// 一次性抑制 vips/GLib 的 stderr 警告（仿 ImageRenderer.cpp）。
static void suppressVipsWarnings()
{
    static std::once_flag s_once;
    std::call_once(s_once, [] {
        g_log_set_handler(
            "VIPS", G_LOG_LEVEL_WARNING,
            [](const gchar *, GLogLevelFlags, const gchar *, gpointer) {}, nullptr);
        g_log_set_handler(
            "VIPS-VIPS", G_LOG_LEVEL_WARNING,
            [](const gchar *, GLogLevelFlags, const gchar *, gpointer) {}, nullptr);
    });
}

/// 文件扩展名是否匹配（大小写不敏感）。
static bool hasExtension(const std::string &path, const char *extCmp)
{
    auto dotPos = path.rfind('.');
    if (dotPos == std::string::npos)
        return false;
    std::string ext = path.substr(dotPos);
    std::transform(ext.begin(), ext.end(), ext.begin(),
        [](unsigned char c) { return static_cast<char>(::tolower(c)); });
    return ext == extCmp;
}

static bool isTiffPath(const std::string &path)
{
    return hasExtension(path, ".tif") || hasExtension(path, ".tiff");
}

// ── TIFF：逐 strip 读取全部像素（仿 ImageRenderer 的 TiffCmykSource /
//    DecodedSource libtiff 读取方式）→ 5 通道 CMYKA 帧。RGB/灰源经
//    lcms2 转换（对齐 ImageRenderer），lcms 不可用即失败（不降级）。
//    返回 nullptr 表示失败。
std::shared_ptr<std::vector<uint8_t>> decodeTiffFrame(
    const std::string &path, IColorConverter *cv, uint32_t w, uint32_t h)
{
    auto prevErr = TIFFSetErrorHandler(nullptr);
    auto prevWarn = TIFFSetWarningHandler(nullptr);
    TIFF *tif = TiffHelper::openTiff(path, "r");
    TIFFSetErrorHandler(prevErr);
    TIFFSetWarningHandler(prevWarn);
    if (!tif)
        return nullptr;

    uint16_t photo = PHOTOMETRIC_MINISBLACK, spp = 1;
    TIFFGetFieldDefaulted(tif, TIFFTAG_PHOTOMETRIC, &photo);
    TIFFGetFieldDefaulted(tif, TIFFTAG_SAMPLESPERPIXEL, &spp);

    // 嵌入 ICC（借用的指针需在 TIFFClose 前拷贝；与 ImageRenderer 的
    // readTiffMeta 相同读取方式）
    std::vector<uint8_t> iccBytes;
    {
        uint32_t iccLen = 0;
        void *iccData = nullptr;
        if (TIFFGetField(tif, TIFFTAG_ICCPROFILE, &iccLen, &iccData) && iccLen > 0 && iccData)
            iccBytes.assign(static_cast<const uint8_t *>(iccData),
                static_cast<const uint8_t *>(iccData) + iccLen);
    }

    // EXTRASAMPLES 首项为 alpha 类型（ASSOCALPHA/UNASSALPHA）时保留 alpha 通道。
    // 只凭 extrasamples 标记判定——绝不只凭通道数推断 alpha；各 photometric
    // 分支再按自身通道布局决定 alpha 字节偏移（RGB spp=4/5 → 偏移 3；灰 spp=2
    // → 偏移 1；CMYK spp=5 → 偏移 4），因此 hasAlpha 在各分支内按 spp 收紧。
    uint16_t extraCount = 0;
    uint16_t *extraTypes = nullptr;
    TIFFGetFieldDefaulted(tif, TIFFTAG_EXTRASAMPLES, &extraCount, &extraTypes);
    const bool extrasAlpha = (extraCount >= 1 && extraTypes
                              && (extraTypes[0] == EXTRASAMPLE_ASSOCALPHA
                                  || extraTypes[0] == EXTRASAMPLE_UNASSALPHA));

    uint32_t rowsPerStrip = 0;
    TIFFGetFieldDefaulted(tif, TIFFTAG_ROWSPERSTRIP, &rowsPerStrip);
    if (rowsPerStrip == 0 || rowsPerStrip > h)
        rowsPerStrip = h;
    tstrip_t nStrips = TIFFNumberOfStrips(tif);

    auto raw = std::make_shared<std::vector<uint8_t>>(static_cast<size_t>(w) * h * spp);
    bool readOk = true;
    for (tstrip_t si = 0; si < nStrips; ++si) {
        uint32_t sRows = (si == nStrips - 1) ? (h - si * rowsPerStrip) : rowsPerStrip;
        size_t stripBytes = static_cast<size_t>(w) * sRows * spp;
        size_t offset = static_cast<size_t>(w) * si * rowsPerStrip * spp;
        if (TIFFReadEncodedStrip(tif, si, raw->data() + offset, static_cast<tsize_t>(stripBytes))
            < 0) {
            readOk = false;
            break;
        }
    }
    TIFFClose(tif);
    if (!readOk)
        return nullptr;

    const size_t nPixels = static_cast<size_t>(w) * h;
    auto frame = std::make_shared<std::vector<uint8_t>>(nPixels * kChannelsPerPixelS);

    switch (photo) {
    case PHOTOMETRIC_SEPARATED: { // CMYK（spp=4）或 CMYKA（spp=5）
        if (spp < 4)
            return nullptr;
        const bool hasAlpha = extrasAlpha && spp == 5; // alpha 在偏移 4
        for (size_t i = 0; i < nPixels; ++i) {
            const uint8_t *s = raw->data() + i * spp;
            uint8_t *d = frame->data() + i * kChannelsPerPixelS;
            d[0] = s[0];
            d[1] = s[1];
            d[2] = s[2];
            d[3] = s[3];
            d[4] = hasAlpha ? s[4] : 255;
        }
        break;
    }
    case PHOTOMETRIC_RGB: { // RGB（spp=3）或 RGBA（spp=4/5）
        if (spp < 3)
            return nullptr;
        const bool hasAlpha = extrasAlpha && (spp == 4 || spp == 5); // alpha 在偏移 3
        // 对齐 ImageRenderer：源 ICC 优先走 lcms2（RGB 提取为 3 通道 →
        // TYPE_RGB_8，alpha 不经转换）；构建失败视为解码失败（不降级）
        std::vector<uint8_t> rgb(nPixels * 3);
        for (size_t i = 0; i < nPixels; ++i)
            for (int c = 0; c < 3; ++c)
                rgb[i * 3 + c] = raw->data()[i * spp + c];
        std::unique_ptr<IColorTransform> lcms =
            cv ? cv->makeToCmyk(PixelColorSpace::Rgb, iccBytes) : nullptr;
        if (!lcms)
            return nullptr;
        std::vector<uint8_t> cmyk(nPixels * 4);
        lcms->convert(rgb.data(), cmyk.data(), static_cast<int>(nPixels));
        for (size_t i = 0; i < nPixels; ++i) {
            uint8_t *d = frame->data() + i * kChannelsPerPixelS;
            d[0] = cmyk[i * 4];
            d[1] = cmyk[i * 4 + 1];
            d[2] = cmyk[i * 4 + 2];
            d[3] = cmyk[i * 4 + 3];
            d[4] = hasAlpha ? raw->data()[i * spp + 3] : 255;
        }
        break;
    }
    case PHOTOMETRIC_MINISBLACK: { // 灰（spp=1）或 灰+alpha（spp=2，alpha 在偏移 1）
        if (spp < 1)
            return nullptr;
        const bool hasAlpha = extrasAlpha && (spp == 2 || spp == 4 || spp == 5);
        // 对齐 ImageRenderer：灰源优先走 Gray profile lcms2（提取 1 通道
        // → TYPE_GRAY_8，alpha 不经转换）；构建失败视为解码失败（不降级）
        std::vector<uint8_t> gray(nPixels);
        for (size_t i = 0; i < nPixels; ++i)
            gray[i] = raw->data()[i * spp];
        std::unique_ptr<IColorTransform> lcms =
            cv ? cv->makeToCmyk(PixelColorSpace::Gray, iccBytes) : nullptr;
        if (!lcms)
            return nullptr;
        std::vector<uint8_t> cmyk(nPixels * 4);
        lcms->convert(gray.data(), cmyk.data(), static_cast<int>(nPixels));
        for (size_t i = 0; i < nPixels; ++i) {
            uint8_t *d = frame->data() + i * kChannelsPerPixelS;
            d[0] = cmyk[i * 4];
            d[1] = cmyk[i * 4 + 1];
            d[2] = cmyk[i * 4 + 2];
            d[3] = cmyk[i * 4 + 3];
            d[4] = hasAlpha ? raw->data()[i * spp + 1] : 255;
        }
        break;
    }
    default:
        return nullptr; // 不支持的 photometric
    }
    return frame;
}

// ── vips（其余格式）：对齐 ImageRenderer（classifyVipsImage +
//    makeToCmyk），颜色转换仅 lcms2，vips 只做解码与几何：
//    - CMYK（bands 4/5）→ 直通（像素即 CMYK，不转换，alpha 直拷）
//    - 灰（B_W/GREY16，bands 1/2）→ 提取单通道 → Gray profile lcms2
//    - sRGB/RGB（bands 3/4）→ RGB profile lcms2（嵌入 ICC 优先）
//    - 其余解释（LAB/XYZ/scRGB…）或异常 band 数 → 解码失败
//    （不再有 vips_colourspace→sRGB 兜底，与图片图元一致：失败即失败）。
//    返回 nullptr 表示失败。
std::shared_ptr<std::vector<uint8_t>> decodeVipsFrame(
    const std::string &path, IColorConverter *cv, uint32_t w, uint32_t h)
{
    suppressVipsWarnings();

    VipsImage *img = vips_image_new_from_file(path.c_str(), "access", VIPS_ACCESS_RANDOM, nullptr);
    if (!img) {
        vips_error_clear();
        return nullptr;
    }

    // 嵌入 ICC（VIPS_META_ICC_NAME，borrowed 指针立即拷贝；与
    // ImageRenderer::openRasterSource 相同读取方式）
    std::vector<uint8_t> iccBytes;
    {
        const void *iccData = nullptr;
        size_t iccLen = 0;
        if (vips_image_get_blob(img, VIPS_META_ICC_NAME, &iccData, &iccLen) == 0 && iccData
            && iccLen > 0)
            iccBytes.assign(static_cast<const uint8_t *>(iccData),
                static_cast<const uint8_t *>(iccData) + iccLen);
    }

    // 8-bit 要求：16-bit 源先降到 UCHAR（与图片图元 DecodedSource 的 cast
    // 步骤一致；cast 不改 interpretation/bands）
    if (vips_image_get_format(img) != VIPS_FORMAT_UCHAR) {
        VipsImage *cast = nullptr;
        if (vips_cast(img, &cast, VIPS_FORMAT_UCHAR, nullptr)) {
            vips_error_clear();
            g_object_unref(img);
            return nullptr;
        }
        g_object_unref(img);
        img = cast;
    }

    // 对齐 ImageRenderer（classifyVipsImage + makeToCmyk）：按解释
    // 分类处理，颜色转换仅 lcms2；不支持的解释直接失败，不再有
    // vips_colourspace→sRGB 兜底。
    const auto interp = vips_image_get_interpretation(img);
    const int bands = vips_image_get_bands(img);

    // CMYK（spp=4）或 CMYKA（spp=5）→ 直通（像素即 CMYK，alpha 直拷）
    if (interp == VIPS_INTERPRETATION_CMYK) {
        if (bands != 4 && bands != 5) {
            EELog::warn("Unexpected CMYK band count ({})", bands);
            g_object_unref(img);
            return nullptr;
        }
        const bool hasAlpha = bands == 5; // alpha 在偏移 4
        size_t rawSize = 0;
        void *raw = vips_image_write_to_memory(img, &rawSize);
        g_object_unref(img);
        if (!raw) {
            vips_error_clear();
            return nullptr;
        }
        const size_t expected = static_cast<size_t>(w) * h * bands;
        if (rawSize != expected) {
            EELog::warn("Decode size mismatch: expected {}, got {}", expected, rawSize);
            g_free(raw);
            return nullptr;
        }
        const uint8_t *px = static_cast<const uint8_t *>(raw);
        const size_t nPixels = static_cast<size_t>(w) * h;
        auto frame = std::make_shared<std::vector<uint8_t>>(nPixels * kChannelsPerPixelS);
        for (size_t i = 0; i < nPixels; ++i) {
            const uint8_t *s = px + i * bands;
            uint8_t *d = frame->data() + i * kChannelsPerPixelS;
            d[0] = s[0];
            d[1] = s[1];
            d[2] = s[2];
            d[3] = s[3];
            d[4] = hasAlpha ? s[4] : 255;
        }
        g_free(raw);
        return frame;
    }

    // 灰（B_W/GREY16，spp=1 或灰+alpha spp=2，alpha 在偏移 1）→
    // Gray profile lcms2（嵌入 ICC 优先；alpha 不经转换直拷）
    if (interp == VIPS_INTERPRETATION_B_W || interp == VIPS_INTERPRETATION_GREY16) {
        if (bands != 1 && bands != 2) {
            EELog::warn("Unexpected Gray band count ({})", bands);
            g_object_unref(img);
            return nullptr;
        }
        const bool hasAlpha = bands == 2;
        size_t rawSize = 0;
        void *raw = vips_image_write_to_memory(img, &rawSize);
        g_object_unref(img);
        if (!raw) {
            vips_error_clear();
            return nullptr;
        }
        const size_t expected = static_cast<size_t>(w) * h * bands;
        if (rawSize != expected) {
            EELog::warn("Decode size mismatch: expected {}, got {}", expected, rawSize);
            g_free(raw);
            return nullptr;
        }
        const uint8_t *px = static_cast<const uint8_t *>(raw);
        const size_t nPixels = static_cast<size_t>(w) * h;
        std::vector<uint8_t> gray(nPixels);
        for (size_t i = 0; i < nPixels; ++i)
            gray[i] = px[i * bands];
        std::unique_ptr<IColorTransform> lcms =
            cv ? cv->makeToCmyk(PixelColorSpace::Gray, iccBytes) : nullptr;
        if (!lcms) {
            g_free(raw);
            return nullptr;
        }
        std::vector<uint8_t> cmyk(nPixels * 4);
        lcms->convert(gray.data(), cmyk.data(), static_cast<int>(nPixels));
        auto frame = std::make_shared<std::vector<uint8_t>>(nPixels * kChannelsPerPixelS);
        for (size_t i = 0; i < nPixels; ++i) {
            uint8_t *d = frame->data() + i * kChannelsPerPixelS;
            d[0] = cmyk[i * 4];
            d[1] = cmyk[i * 4 + 1];
            d[2] = cmyk[i * 4 + 2];
            d[3] = cmyk[i * 4 + 3];
            d[4] = hasAlpha ? px[i * bands + 1] : 255;
        }
        g_free(raw);
        return frame;
    }

    // sRGB/RGB（spp=3 或 RGBA spp=4）→ RGB profile lcms2（嵌入 ICC 优先）
    if ((interp == VIPS_INTERPRETATION_sRGB || interp == VIPS_INTERPRETATION_RGB)
        && (bands == 3 || bands == 4)) {
        size_t rawSize = 0;
        void *raw = vips_image_write_to_memory(img, &rawSize);
        g_object_unref(img);
        if (!raw) {
            vips_error_clear();
            return nullptr;
        }
        const size_t expected = static_cast<size_t>(w) * h * bands;
        if (rawSize != expected) {
            EELog::warn("Decode size mismatch: expected {}, got {}", expected, rawSize);
            g_free(raw);
            return nullptr;
        }
        const uint8_t *px = static_cast<const uint8_t *>(raw);
        const size_t nPixels = static_cast<size_t>(w) * h;
        std::unique_ptr<IColorTransform> lcms = cv
            ? cv->makeToCmyk(bands == 4 ? PixelColorSpace::Rgba : PixelColorSpace::Rgb, iccBytes)
            : nullptr;
        if (!lcms) {
            g_free(raw);
            return nullptr; // 与图片图元一致：lcms 不可用 → 解码失败
        }
        std::vector<uint8_t> cmyk(nPixels * 4);
        lcms->convert(px, cmyk.data(), static_cast<int>(nPixels));
        auto frame = std::make_shared<std::vector<uint8_t>>(nPixels * kChannelsPerPixelS);
        for (size_t i = 0; i < nPixels; ++i) {
            uint8_t *d = frame->data() + i * kChannelsPerPixelS;
            d[0] = cmyk[i * 4];
            d[1] = cmyk[i * 4 + 1];
            d[2] = cmyk[i * 4 + 2];
            d[3] = cmyk[i * 4 + 3];
            d[4] = (bands == 4) ? px[i * bands + 3] : 255; // alpha 直拷 / 无 alpha
        }
        g_free(raw);
        return frame;
    }

    // 其余解释（LAB/XYZ/scRGB…）或异常 band 数 → 解码失败（对齐
    // ImageRenderer：非 sRGB/RGB 解释拒绝，不降级）
    EELog::warn("Unsupported source colourspace ({}) for texture: {}", static_cast<int>(interp),
        path);
    g_object_unref(img);
    return nullptr;
}

} // anonymous namespace

// ═══════════════════════════════════════════════════════════════════════
//  TextureSource
// ═══════════════════════════════════════════════════════════════════════

TextureSource::TextureSource(const std::string &filePath, const Dpi &canvasDpi, IColorConverter *cv,
    int overrideW, int overrideH)
    : m_path(filePath)
    , m_canvasDpi(canvasDpi)
    , m_cv(cv)
    , m_overrideW(overrideW)
    , m_overrideH(overrideH)
{
    open();
}

TextureSource::~TextureSource() = default;

bool TextureSource::warnOnce(const std::string &key, const std::string &msg)
{
    std::lock_guard<std::mutex> lock(s_mtx);
    if (s_warned.size() >= kMaxWarned)
        s_warned.clear();
    if (!s_warned.insert(key).second)
        return false;
    EELog::warn("{}", msg);
    return true;
}

bool TextureSource::open()
{
    // ── 1. 元数据读取 ────────────────────────────────────────────────
    if (isTiffPath(m_path)) {
        auto prevErr = TIFFSetErrorHandler(nullptr);
        auto prevWarn = TIFFSetWarningHandler(nullptr);
        TIFF *tif = TiffHelper::openTiff(m_path, "r");
        TIFFSetErrorHandler(prevErr);
        TIFFSetWarningHandler(prevWarn);
        if (!tif) {
            warnOnce(m_path, "Cannot open image: " + m_path);
            std::lock_guard<std::mutex> lock(s_mtx);
            s_badFiles.insert(m_path);
            return false;
        }
        uint32_t w = 0, h = 0;
        TIFFGetField(tif, TIFFTAG_IMAGEWIDTH, &w);
        TIFFGetField(tif, TIFFTAG_IMAGELENGTH, &h);
        float xres = 72.0f, yres = 72.0f;
        uint16_t resUnit = RESUNIT_INCH;
        TIFFGetFieldDefaulted(tif, TIFFTAG_XRESOLUTION, &xres);
        TIFFGetFieldDefaulted(tif, TIFFTAG_YRESOLUTION, &yres);
        TIFFGetFieldDefaulted(tif, TIFFTAG_RESOLUTIONUNIT, &resUnit);
        TIFFClose(tif);
        if (w == 0 || h == 0) {
            warnOnce(m_path, "Invalid dimensions (0x0): " + m_path);
            std::lock_guard<std::mutex> lock(s_mtx);
            s_badFiles.insert(m_path);
            return false;
        }
        if (resUnit == RESUNIT_CENTIMETER) {
            xres *= 2.54f;
            yres *= 2.54f;
        }
        m_imgDpiX = (static_cast<double>(xres) >= 1.0) ? static_cast<double>(xres) : 72.0;
        m_imgDpiY = (static_cast<double>(yres) >= 1.0) ? static_cast<double>(yres) : 72.0;
        m_srcW = static_cast<int>(w);
        m_srcH = static_cast<int>(h);
    } else {
        suppressVipsWarnings();
        VipsImage *img =
            vips_image_new_from_file(m_path.c_str(), "access", VIPS_ACCESS_RANDOM, nullptr);
        if (!img) {
            warnOnce(m_path, "Cannot open image: " + m_path + " — " + vips_error_buffer());
            vips_error_clear();
            std::lock_guard<std::mutex> lock(s_mtx);
            s_badFiles.insert(m_path);
            return false;
        }
        int w = vips_image_get_width(img);
        int h = vips_image_get_height(img);
        auto [dpiX, dpiY] = extractVipsImageDpi(img);
        g_object_unref(img);
        if (w <= 0 || h <= 0) {
            warnOnce(m_path, "Invalid dimensions (" + std::to_string(w) + "x" + std::to_string(h)
                                 + "): " + m_path);
            std::lock_guard<std::mutex> lock(s_mtx);
            s_badFiles.insert(m_path);
            return false;
        }
        m_imgDpiX = dpiX;
        m_imgDpiY = dpiY;
        m_srcW = w;
        m_srcH = h;
    }

    // ── 2. tile 尺寸（逐维独立：override > 0 用 override，否则该维回退
    //    到逐轴 DPI 换算的原图尺寸；任何输入下 m_tileW/m_tileH 都 ≥ 1）───
    double dpiScaleX = static_cast<double>(m_canvasDpi.x) / m_imgDpiX;
    double dpiScaleY = static_cast<double>(m_canvasDpi.y) / m_imgDpiY;
    m_tileW =
        (m_overrideW > 0) ? m_overrideW : std::max(static_cast<int>(m_srcW * dpiScaleX + 0.5), 1);
    m_tileH =
        (m_overrideH > 0) ? m_overrideH : std::max(static_cast<int>(m_srcH * dpiScaleY + 0.5), 1);

    // ── 3. 缓存命中（帧 + 其源尺寸）/ 已知坏文件 ────────────────────
    {
        std::lock_guard<std::mutex> lock(s_mtx);
        auto it = s_cache.find(m_path); // 缓存键 = filePath（仅路径）
        if (it != s_cache.end() && it->second.srcW == m_srcW && it->second.srcH == m_srcH) {
            m_frame = it->second.frame;
            return true;
        }
    }
    {
        bool bad = false;
        {
            std::lock_guard<std::mutex> lock(s_mtx);
            bad = s_badFiles.count(m_path) > 0;
        }
        if (bad) {
            // warnOnce 内部会再锁 s_mtx —— 不能在持锁时调用
            warnOnce(m_path, "Skipping previously failed texture: " + m_path);
            return false;
        }
    }

    // ── 4. 解码 ──────────────────────────────────────────────────────
    std::shared_ptr<std::vector<uint8_t>> frame;
    if (isTiffPath(m_path))
        frame = decodeTiffFrame(
            m_path, m_cv, static_cast<uint32_t>(m_srcW), static_cast<uint32_t>(m_srcH));
    else
        frame = decodeVipsFrame(
            m_path, m_cv, static_cast<uint32_t>(m_srcW), static_cast<uint32_t>(m_srcH));
    if (!frame) {
        warnOnce(m_path, "Decode failed for texture: " + m_path);
        std::lock_guard<std::mutex> lock(s_mtx);
        s_badFiles.insert(m_path);
        return false;
    }

    // ── 5. 发布缓存（仅 ≤ kMaxCachedBytes 的帧入缓存）────────────────
    if (frame->size() <= kMaxCachedBytes) {
        std::lock_guard<std::mutex> lock(s_mtx);
        s_cache[m_path] = CachedFrame{ frame, m_srcW, m_srcH };
    }
    m_frame = frame;
    return true;
}

bool TextureSource::sample(
    double u, double v, uint8_t &c1, uint8_t &c2, uint8_t &c3, uint8_t &c4, uint8_t &a)
{
    if (!m_frame || m_srcW <= 0 || m_srcH <= 0)
        return false;

    // 最近邻采样（与 ImageRenderer 的 vips NEAREST 语义一致，输出像素值
    // 永远是源图真实存在的值）：xF = u×srcW ∈ [0, srcW)，floor 将采样点
    // 归入其所在像素格（像素 i 占据 [i, i+1)）。
    // 半像素对齐：目标像素中心（rx = px+0.5，见 RectRenderer）在 1:1
    // （tile 尺寸 == 源尺寸）时映射到 xF = px+0.5，floor → px —— 逐像素
    // 精确对应源图。最近邻的 floor 语义天然消除双线性时代"采样点落在
    // 两像素之间"的半像素错位，无需显式 -0.5 偏移。
    double xF = u * m_srcW;
    double yF = v * m_srcH;
    if (xF < 0.0)
        xF = 0.0;
    if (yF < 0.0)
        yF = 0.0;
    if (xF >= m_srcW)
        xF = std::nextafter(static_cast<double>(m_srcW), 0.0);
    if (yF >= m_srcH)
        yF = std::nextafter(static_cast<double>(m_srcH), 0.0);

    const int j = static_cast<int>(xF); // xF ≥ 0 → 截断 == floor；clamp 后 ≤ srcW-1
    const int i = static_cast<int>(yF);

    // 直拷 5 通道（C、M、Y、K、A）
    const uint8_t *p = m_frame->data() + (static_cast<size_t>(i) * m_srcW + j) * kChannelsPerPixelS;
    c1 = p[0];
    c2 = p[1];
    c3 = p[2];
    c4 = p[3];
    a = p[4];
    return true;
}

void TextureSource::preDecode(const std::string &filePath, const Dpi &canvasDpi, IColorConverter *cv,
    int overrideW, int overrideH)
{
    // 构造即触发解码 + 发布缓存；失败已在内部标记 s_badFiles，
    // render 期构造同一 key 的实例会立刻 ok()==false。
    TextureSource src(filePath, canvasDpi, cv, overrideW, overrideH);
    (void)src;
}
