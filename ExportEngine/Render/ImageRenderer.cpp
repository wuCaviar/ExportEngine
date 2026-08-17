#include "ImageRenderer.h"
#include "IColorConverter.h"
#include "IResampler.h"
#include "NearestResampler.h"
#include "Log.h"
#include "TiffHelper.h"
#include "VipsUtil.h" // extractVipsImageDpi

#include <tiffio.h>
#include <vips/vips.h>

#include <cstring>
#include <cstdint>
#include <algorithm>
#include <cmath>
#include <memory>
#include <mutex>
#include <unordered_set>

using namespace ATHC::EE;

namespace {

// Capped set for warning throttling — clears when exceeding capacity
// to bound memory in long-running processes.
struct CappedFileSet
{
    static constexpr size_t kCap = 1024;
    std::unordered_set<std::string> files;
    bool insert(const std::string &f)
    {
        if (files.size() >= kCap)
            files.clear();
        return files.insert(f).second;
    }
    bool contains(const std::string &f) const { return files.count(f) > 0; }
};

// Single mutex for all warning-throttling CappedFileSets.
// Held briefly for set lookups — contention is negligible.
std::mutex g_warnMutex;

// Known-bad image files — shared by draw() and preDecode() so a failure in
// the pre-decode phase also silences the render pass (and vice versa).
CappedFileSet g_badFiles;

// ── Image color space classification ────────────────────────────────

enum class ImgType : uint8_t
{
    CMYK,
    RGB,
    Gray,
};

// ── One-time VIPS warning suppressor ──────────────────────────────────
// GLib warnings from vips (e.g. OOM) pollute console output — swallow
// them; real errors still surface via return codes + vips_error_buffer().
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

// ── VIPS helpers ─────────────────────────────────────────────────────

static ImgType classifyVipsImage(VipsImage *img)
{
    auto interp = vips_image_get_interpretation(img);
    if (interp == VIPS_INTERPRETATION_CMYK)
        return ImgType::CMYK;
    if (interp == VIPS_INTERPRETATION_B_W || interp == VIPS_INTERPRETATION_GREY16)
        return ImgType::Gray;
    // sRGB, RGB, scRGB, XYZ, LAB, and fallback
    return ImgType::RGB;
}

// 把 (ImgType, srcBpp) 映射到 PixelColorSpace（EE 只描述源空间，转换交给引擎）。
static PixelColorSpace toPixelSpace(ImgType t, int srcBpp)
{
    if (t == ImgType::RGB)
        return srcBpp == 4 ? PixelColorSpace::Rgba : PixelColorSpace::Rgb;
    if (t == ImgType::Gray)
        return srcBpp == 2 ? PixelColorSpace::GrayA : PixelColorSpace::Gray;
    return PixelColorSpace::Cmyk; // 直通
}

/// Check file extension — case-insensitive.
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

/// Read TIFF metadata via libtiff (header tags only, zero pixel decode).
/// When the TIFF embeds an ICC profile (RGB source), its bytes are copied
/// into outIcc for use in the colourspace conversion.
static bool readTiffMeta(const std::string &path, uint32_t &outW, uint32_t &outH, ImgType &outType,
    int &outBpp, double &outDpiX, double &outDpiY, std::vector<uint8_t> &outIcc)
{
    auto prevErr = TIFFSetErrorHandler(nullptr);
    auto prevWarn = TIFFSetWarningHandler(nullptr);
    TIFF *tif = TiffHelper::openTiff(path, "r");
    TIFFSetErrorHandler(prevErr);
    TIFFSetWarningHandler(prevWarn);
    if (!tif)
        return false;

    uint32_t w = 0, h = 0;
    TIFFGetField(tif, TIFFTAG_IMAGEWIDTH, &w);
    TIFFGetField(tif, TIFFTAG_IMAGELENGTH, &h);
    outW = w;
    outH = h;

    // Embedded ICC profile (may be absent)
    uint32_t iccLen = 0;
    void *iccData = nullptr;
    outIcc.clear();
    if (TIFFGetField(tif, TIFFTAG_ICCPROFILE, &iccLen, &iccData) && iccLen > 0 && iccData)
        outIcc.assign(
            static_cast<const uint8_t *>(iccData), static_cast<const uint8_t *>(iccData) + iccLen);

    uint16_t photo = 0, spp = 4;
    TIFFGetFieldDefaulted(tif, TIFFTAG_PHOTOMETRIC, &photo);
    TIFFGetFieldDefaulted(tif, TIFFTAG_SAMPLESPERPIXEL, &spp);

    // outBpp must be the ACTUAL samples-per-pixel: it drives buffer strides
    // for libtiff strip reads and the band count handed to lcms2. Hardcoding
    // 4 for RGB broke 3-band TIFFs (data misread as RGBA → wrong-band
    // CMYK output).
    if (photo == PHOTOMETRIC_SEPARATED) {
        outType = ImgType::CMYK;
        outBpp = spp;
    } else if (photo == PHOTOMETRIC_MINISBLACK || photo == PHOTOMETRIC_MINISWHITE) {
        outType = ImgType::Gray;
        outBpp = spp; // usually 1; 2 = gray + alpha
    } else {
        outType = ImgType::RGB;
        outBpp = spp; // 3 = RGB, 4 = RGBA
    }

    float xres = 72.0f, yres = 72.0f;
    uint16_t resUnit = RESUNIT_INCH;
    TIFFGetFieldDefaulted(tif, TIFFTAG_XRESOLUTION, &xres);
    TIFFGetFieldDefaulted(tif, TIFFTAG_YRESOLUTION, &yres);
    TIFFGetFieldDefaulted(tif, TIFFTAG_RESOLUTIONUNIT, &resUnit);
    if (resUnit == RESUNIT_CENTIMETER) {
        xres *= 2.54f;
        yres *= 2.54f;
    }
    outDpiX = (static_cast<double>(xres) >= 1.0) ? static_cast<double>(xres) : 72.0;
    outDpiY = (static_cast<double>(yres) >= 1.0) ? static_cast<double>(yres) : 72.0;

    TIFFClose(tif);
    return true;
}

/// ── 最近邻按需缩放（readRows 驱动：读源 band → 转 CMYK → 水平缩放 → 垂直拷入）──
/// 既不物化整幅源帧，也不物化整幅渲染帧：每次 readRows 只读覆盖请求输出行的
/// 源行范围（TIFF 逐 strip、非 TIFF 按 vips region），水平缩放（vips 最近邻，
/// 整宽，与整幅缩放的水平分量字节一致）后按垂直最近邻拷入输出。峰值内存 =
/// 一个源 band + 请求的输出行。

// 垂直最近邻映射：输出行 oy → 源行（与 vips KERNEL_NEAREST 一致：floor(oy·srcH/dstH)）。
static inline int verticalNN(int oy, int srcH, int dstH)
{
    const int sy = static_cast<int>(std::floor(oy * static_cast<double>(srcH) / dstH));
    return std::max(0, std::min(srcH - 1, sy));
}

/// Blend source rows into the render buffer. srcSpp = source channels per
/// pixel (≥4): the first four are CMYK; extra channels pass through when
/// present (multi-channel TIFF pass-through), otherwise zero-padded.
/// srcRow0 = source row number of the first row in src (chunk-relative
/// blending — src may hold only a row range, not the whole frame).
static void blendCmykRows(RenderContext &ctx, const uint8_t *src, int srcW, int srcSpp, int outX0,
    int outX1, int outY0, int outY1, int imgX, int imgY, int srcRow0)
{
    for (int py = outY0; py < outY1; ++py) {
        int srcRow = py - imgY - srcRow0;
        const uint8_t *rowBuf = src + static_cast<size_t>(srcRow) * srcW * srcSpp;
        for (int px = outX0; px < outX1; ++px) {
            int srcPx = px - imgX;
            const uint8_t *sp = rowBuf + static_cast<size_t>(srcPx) * srcSpp;
            ctx.blendCmyk(px, py, sp[0], sp[1], sp[2], sp[3], 255);
            // Extra channels: write if available, else zero-pad
            if (ctx.samplesPerPixel > 4) {
                int lx = (ctx.tileW > 0) ? (px - ctx.tileX) : px;
                int ly = (ctx.tileW > 0) ? (py - ctx.tileY) : py;
                size_t dstIdx =
                    (static_cast<size_t>(ly) * ctx.effectiveWidth() + static_cast<size_t>(lx))
                    * ctx.samplesPerPixel;
                if (srcSpp > 4) {
                    for (int ch = 4; ch < srcSpp && ch < ctx.samplesPerPixel; ++ch)
                        ctx.cmykBuf[dstIdx + ch] = sp[ch];
                    for (int ch = srcSpp; ch < ctx.samplesPerPixel; ++ch)
                        ctx.cmykBuf[dstIdx + ch] = 0;
                } else {
                    for (int ch = 4; ch < ctx.samplesPerPixel; ++ch)
                        ctx.cmykBuf[dstIdx + ch] = 0;
                }
            }
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════
//  Raster source abstraction
// ═══════════════════════════════════════════════════════════════════════
//  The render layer's single consumer interface for image primitives.
//  Each implementation owns decoding, colour conversion, resizing and
//  caching internally, and exposes CMYK (+ extra channels) rows — the
//  render layer never sees the source format.
struct RasterSource
{
    virtual ~RasterSource() = default;

    virtual int width() const = 0; // render target size (post-scale)
    virtual int height() const = 0;
    virtual int samplesPerPixel() const = 0; // ≥4; first four channels are CMYK
    // Read rows [y0, y0+n) into out (out size = n * width() * samplesPerPixel()).
    // Returns false on failure (partial output invalid).
    virtual bool readRows(int y0, int n, std::vector<uint8_t> &out) = 0;

    // True when construction succeeded (metadata/transform setup). Streaming
    // sources validate lazily in readRows and default to true.
    virtual bool ok() const { return true; }
};

// ── CMYK TIFF pass-through (no resize) ──────────────────────────────────
// 流式：逐 strip 读（带单 strip 缓存），单 strip 与多 strip 统一路径——
// 单 strip 时 rowsPerStrip == h，读一次整条后按行取，峰值内存 ≈ 一条 strip，
// 不物化也不跨图元持久化整帧。专色通道（>4 spp）原样保留。
class TiffCmykSource : public RasterSource
{
public:
    TiffCmykSource(std::string path, uint32_t srcW, uint32_t srcH, int srcBpp)
        : m_path(std::move(path)), m_w(srcW), m_h(srcH), m_spp(srcBpp)
    { }

    int width() const override { return static_cast<int>(m_w); }
    int height() const override { return static_cast<int>(m_h); }
    int samplesPerPixel() const override { return m_spp; }

    bool readRows(int y0, int n, std::vector<uint8_t> &out) override
    {
        if (!ensureOpen())
            return false;

        out.resize(static_cast<size_t>(n) * m_w * m_spp);
        // 逐 strip 读（strip 级缓存），按行组装
        for (int r = 0; r < n; ++r) {
            uint32_t srcRow = static_cast<uint32_t>(y0 + r);
            tstrip_t si = static_cast<tstrip_t>(srcRow) / m_rowsPerStrip;
            if (static_cast<int>(si) != m_cachedStrip) {
                uint32_t sRows = m_rowsPerStrip;
                if (si == m_nStrips - 1)
                    sRows = m_h - si * m_rowsPerStrip;
                m_stripStartRow = si * m_rowsPerStrip;
                m_stripBuf.resize(static_cast<size_t>(m_w) * sRows * m_spp);
                if (TIFFReadEncodedStrip(
                        m_tif, si, m_stripBuf.data(), static_cast<tsize_t>(m_stripBuf.size()))
                    < 0)
                    return false;
                m_cachedStrip = static_cast<int>(si);
            }
            std::memcpy(out.data() + static_cast<size_t>(r) * m_w * m_spp,
                m_stripBuf.data() + static_cast<size_t>(srcRow - m_stripStartRow) * m_w * m_spp,
                static_cast<size_t>(m_w) * m_spp);
        }
        return true;
    }

    ~TiffCmykSource()
    {
        if (m_tif)
            TIFFClose(m_tif);
    }

private:
    bool ensureOpen()
    {
        if (m_tif)
            return true;

        auto prevErr = TIFFSetErrorHandler(nullptr);
        auto prevWarn = TIFFSetWarningHandler(nullptr);
        m_tif = TiffHelper::openTiff(m_path, "r");
        TIFFSetErrorHandler(prevErr);
        TIFFSetWarningHandler(prevWarn);
        if (!m_tif)
            return false;

        TIFFGetFieldDefaulted(m_tif, TIFFTAG_ROWSPERSTRIP, &m_rowsPerStrip);
        if (m_rowsPerStrip == 0 || m_rowsPerStrip > m_h)
            m_rowsPerStrip = m_h;
        m_nStrips = TIFFNumberOfStrips(m_tif);
        return true;
    }

    std::string m_path;
    uint32_t m_w = 0, m_h = 0;
    int m_spp = 4;
    TIFF *m_tif = nullptr;
    uint32_t m_rowsPerStrip = 0;
    tstrip_t m_nStrips = 0;
    int m_cachedStrip = -1;
    uint32_t m_stripStartRow = 0;
    std::vector<uint8_t> m_stripBuf;
};

// ── RGB/Gray TIFF colourspace-only (no resize) ─────────────────────────
// Reads one strip at a time via libtiff, converts each strip to CMYK via
// lcms2 (embedded ICC → project source profile → project CMYK profile),
// and assembles rows. Peak memory ≈ one strip.
class TiffConvertSource : public RasterSource
{
public:
    TiffConvertSource(std::string path, uint32_t srcW, uint32_t srcH, int srcBpp, ImgType imgType,
        IColorConverter *cv, const std::vector<uint8_t> &srcIcc)
        : m_path(std::move(path)), m_w(srcW), m_h(srcH), m_srcBpp(srcBpp)
    {
        m_lcms = cv ? cv->makeToCmyk(toPixelSpace(imgType, srcBpp), srcIcc) : nullptr;
    }

    int width() const override { return static_cast<int>(m_w); }
    int height() const override { return static_cast<int>(m_h); }
    int samplesPerPixel() const override { return 4; }

    bool readRows(int y0, int n, std::vector<uint8_t> &out) override
    {
        if (!m_lcms || !ensureOpen())
            return false;

        out.resize(static_cast<size_t>(n) * m_w * 4);
        for (int r = 0; r < n; ++r) {
            uint32_t srcRow = static_cast<uint32_t>(y0 + r);
            tstrip_t si = static_cast<tstrip_t>(srcRow) / m_rowsPerStrip;
            if (static_cast<int>(si) != m_cachedStrip) {
                uint32_t sRows = m_rowsPerStrip;
                if (si == m_nStrips - 1)
                    sRows = m_h - si * m_rowsPerStrip;
                m_stripStartRow = si * m_rowsPerStrip;

                // Read raw strip via libtiff
                m_rawBuf.resize(static_cast<size_t>(m_w) * sRows * m_srcBpp);
                if (TIFFReadEncodedStrip(
                        m_tif, si, m_rawBuf.data(), static_cast<tsize_t>(m_rawBuf.size()))
                    < 0)
                    return false;

                // Convert strip to CMYK (per-strip memory)
                m_cmykBuf.resize(static_cast<size_t>(m_w) * sRows * 4);
                m_lcms->convert(m_rawBuf.data(), m_cmykBuf.data(), m_w * sRows);
                m_cachedStrip = static_cast<int>(si);
            }
            std::memcpy(out.data() + static_cast<size_t>(r) * m_w * 4,
                m_cmykBuf.data() + static_cast<size_t>(srcRow - m_stripStartRow) * m_w * 4,
                static_cast<size_t>(m_w) * 4);
        }
        return true;
    }

    ~TiffConvertSource()
    {
        if (m_tif)
            TIFFClose(m_tif);
    }

private:
    bool ensureOpen()
    {
        if (m_tif)
            return true;

        auto prevErr = TIFFSetErrorHandler(nullptr);
        auto prevWarn = TIFFSetWarningHandler(nullptr);
        m_tif = TiffHelper::openTiff(m_path, "r");
        TIFFSetErrorHandler(prevErr);
        TIFFSetWarningHandler(prevWarn);
        if (!m_tif)
            return false;

        TIFFGetFieldDefaulted(m_tif, TIFFTAG_ROWSPERSTRIP, &m_rowsPerStrip);
        if (m_rowsPerStrip == 0 || m_rowsPerStrip > m_h)
            m_rowsPerStrip = m_h;
        m_nStrips = TIFFNumberOfStrips(m_tif);
        return true;
    }

    std::string m_path;
    uint32_t m_w = 0, m_h = 0;
    int m_srcBpp = 4;
    TIFF *m_tif = nullptr;
    uint32_t m_rowsPerStrip = 0;
    tstrip_t m_nStrips = 0;
    std::unique_ptr<IColorTransform> m_lcms;
    int m_cachedStrip = -1;
    uint32_t m_stripStartRow = 0;
    std::vector<uint8_t> m_rawBuf;
    std::vector<uint8_t> m_cmykBuf;
};

// ── Decoded path (needs resize, or non-TIFF) ───────────────────────────
// 惰性流式：不物化整幅源帧、也不物化整幅渲染帧。readRows 按需只读覆盖请求
// 输出行的源行范围（TIFF 逐 strip、非 TIFF 按 vips region），转换 + 水平缩放 +
// 垂直最近邻拷入输出。峰值内存 = 一个源 band + 请求的输出行。
class DecodedSource : public RasterSource
{
public:
    DecodedSource(std::string path, uint32_t srcW, uint32_t srcH, int srcBpp, ImgType imgType,
        int renderW, int renderH, IColorConverter *cv, IResampler *resampler,
        const std::vector<uint8_t> &srcIcc, VipsImage *vipsImg)
        : m_path(std::move(path)), m_srcW(srcW), m_srcH(srcH), m_srcBpp(srcBpp),
          m_imgType(imgType), m_w(renderW), m_h(renderH), m_resampler(resampler),
          m_vipsImg(vipsImg)
    {
        // 非 TIFF：先 cast/校验/灰抽（都是惰性 vips 节点，不解码），得到实际 band 数。
        if (m_vipsImg)
            m_ok = prepareVips();

        if (m_ok && imgType != ImgType::CMYK) {
            // TIFF 用 srcBpp；非 TIFF 用准备后的实际 band 数（RGB 3/4、Gray 1）。
            const int bandBpp = m_vipsImg ? ((imgType == ImgType::RGB) ? m_vipsBands : 1) : srcBpp;
            m_lcms = cv ? cv->makeToCmyk(toPixelSpace(imgType, bandBpp), srcIcc) : nullptr;
            m_ok = m_lcms != nullptr;
            if (!m_ok)
                EELog::warn("Cannot build source→CMYK transform for: {}", m_path);
        }
    }

    ~DecodedSource() override
    {
        if (m_tif)
            TIFFClose(m_tif);
        if (m_vipsImg)
            g_object_unref(m_vipsImg);
    }

    int width() const override { return m_w; }
    int height() const override { return m_h; }
    int samplesPerPixel() const override { return 4; }
    bool ok() const override { return m_ok; }

    bool readRows(int y0, int n, std::vector<uint8_t> &out) override
    {
        if (!m_ok || n <= 0)
            return false;

        // 覆盖输出行 [y0, y0+n) 的源行范围（含端点）
        const int srcH = static_cast<int>(m_srcH);
        const int sy0 = verticalNN(y0, srcH, m_h);
        const int sy1 = verticalNN(y0 + n - 1, srcH, m_h);
        const int R = sy1 - sy0 + 1;

        // 读源 band → CMYK（m_srcW × R × 4）
        m_cmyk.resize(static_cast<size_t>(m_srcW) * R * 4);
        if (m_vipsImg) {
            if (!readVipsBand(sy0, R, m_cmyk))
                return false;
        } else {
            if (!readTiffBand(sy0, R, m_cmyk))
                return false;
        }

        // 水平缩放 band → m_hBand（m_w × R × 4），vscale=1.0（逐行最近邻）
        m_hBand.resize(static_cast<size_t>(m_w) * R * 4);
        m_resampler->resize(m_cmyk.data(), static_cast<int>(m_srcW), R, m_hBand.data(), m_w, R, 4);

        // 垂直最近邻拷入输出
        out.resize(static_cast<size_t>(n) * m_w * 4);
        for (int r = 0; r < n; ++r) {
            const int sy = verticalNN(y0 + r, srcH, m_h) - sy0;
            std::memcpy(out.data() + static_cast<size_t>(r) * m_w * 4,
                m_hBand.data() + static_cast<size_t>(sy) * m_w * 4,
                static_cast<size_t>(m_w) * 4);
        }
        return true;
    }

private:
    // 非 TIFF：cast 到 UCHAR + 校验 RGB band/色彩空间 + Gray 2band 抽单带。
    // 均为惰性 vips 节点（不触发解码）；成功后 m_vipsImg 为可随机读取的源。
    bool prepareVips()
    {
        VipsImage *in = m_vipsImg;
        m_vipsImg = nullptr;

        if (vips_image_get_format(in) != VIPS_FORMAT_UCHAR) {
            VipsImage *cast = nullptr;
            if (vips_cast(in, &cast, VIPS_FORMAT_UCHAR, nullptr)) {
                vips_error_clear();
                g_object_unref(in);
                return false;
            }
            g_object_unref(in);
            in = cast;
        }

        int bands = vips_image_get_bands(in);
        if (m_imgType == ImgType::RGB) {
            const auto interp = vips_image_get_interpretation(in);
            if (interp != VIPS_INTERPRETATION_sRGB && interp != VIPS_INTERPRETATION_RGB) {
                EELog::warn("Unsupported source colourspace ({})", static_cast<int>(interp));
                g_object_unref(in);
                return false;
            }
            if (bands != 3 && bands != 4) {
                EELog::warn("Unexpected RGB band count ({})", bands);
                g_object_unref(in);
                return false;
            }
        } else if (m_imgType == ImgType::Gray && bands == 2) {
            VipsImage *gray = nullptr;
            if (vips_extract_band(in, &gray, 0, 1, nullptr)) {
                vips_error_clear();
                g_object_unref(in);
                return false;
            }
            g_object_unref(in);
            in = gray;
            bands = 1;
        }

        m_vipsImg = in;
        m_vipsBands = bands;
        return true;
    }

    // TIFF：读源行 [y0, y0+rows) → CMYK，逐 strip（带 strip 缓存，避免重复读条）。
    bool readTiffBand(int y0, int rows, std::vector<uint8_t> &cmykOut)
    {
        if (!ensureOpenTiff())
            return false;
        cmykOut.resize(static_cast<size_t>(m_srcW) * rows * 4);
        for (int r = 0; r < rows; ++r) {
            const uint32_t srcRow = static_cast<uint32_t>(y0 + r);
            const tstrip_t si = static_cast<tstrip_t>(srcRow) / m_rowsPerStrip;
            if (static_cast<int>(si) != m_cachedStrip) {
                uint32_t sRows = m_rowsPerStrip;
                if (si == m_nStrips - 1)
                    sRows = m_srcH - si * m_rowsPerStrip;
                m_stripStartRow = si * m_rowsPerStrip;
                m_rawBuf.resize(static_cast<size_t>(m_srcW) * sRows * m_srcBpp);
                if (TIFFReadEncodedStrip(m_tif, si, m_rawBuf.data(),
                        static_cast<tsize_t>(m_rawBuf.size())) < 0)
                    return false;
                m_stripCmyk.resize(static_cast<size_t>(m_srcW) * sRows * 4);
                convertToCmyk(m_rawBuf.data(), m_stripCmyk.data(),
                    static_cast<int>(m_srcW * sRows));
                m_cachedStrip = static_cast<int>(si);
            }
            std::memcpy(cmykOut.data() + static_cast<size_t>(r) * m_srcW * 4,
                m_stripCmyk.data() + static_cast<size_t>(srcRow - m_stripStartRow) * m_srcW * 4,
                static_cast<size_t>(m_srcW) * 4);
        }
        return true;
    }

    // 非 TIFF：读源行 [y0, y0+rows) → CMYK，按 vips region（含 stride 归一）。
    bool readVipsBand(int y0, int rows, std::vector<uint8_t> &cmykOut)
    {
        const int w = static_cast<int>(m_srcW);
        cmykOut.resize(static_cast<size_t>(w) * rows * 4);
        VipsRegion *region = vips_region_new(m_vipsImg);
        VipsRect rect = { 0, y0, w, rows };
        if (vips_region_prepare(region, &rect)) {
            EELog::warn("Region read failed at row {}: {}", y0, vips_error_buffer());
            vips_error_clear();
            g_object_unref(region);
            return false;
        }
        const uint8_t *band = reinterpret_cast<const uint8_t *>(VIPS_REGION_ADDR(region, 0, y0));
        const size_t stride = VIPS_REGION_LSKIP(region);
        if (m_imgType == ImgType::CMYK) {
            // 直通：取前 4 通道（可能带 stride）
            for (int r = 0; r < rows; ++r)
                for (int x = 0; x < w; ++x)
                    for (int c = 0; c < 4; ++c)
                        cmykOut[(static_cast<size_t>(r) * w + x) * 4 + c] =
                            band[static_cast<size_t>(r) * stride
                                + static_cast<size_t>(x) * m_vipsBands + c];
        } else if (stride == static_cast<size_t>(w) * m_vipsBands) {
            m_lcms->convert(band, cmykOut.data(), w * rows);
        } else {
            m_rawBuf.resize(static_cast<size_t>(w) * rows * m_vipsBands);
            for (int r = 0; r < rows; ++r)
                std::memcpy(m_rawBuf.data() + static_cast<size_t>(r) * w * m_vipsBands,
                    band + static_cast<size_t>(r) * stride,
                    static_cast<size_t>(w) * m_vipsBands);
            m_lcms->convert(m_rawBuf.data(), cmykOut.data(), w * rows);
        }
        g_object_unref(region);
        return true;
    }

    bool ensureOpenTiff()
    {
        if (m_tif)
            return true;
        auto prevErr = TIFFSetErrorHandler(nullptr);
        auto prevWarn = TIFFSetWarningHandler(nullptr);
        m_tif = TiffHelper::openTiff(m_path, "r");
        TIFFSetErrorHandler(prevErr);
        TIFFSetWarningHandler(prevWarn);
        if (!m_tif)
            return false;
        TIFFGetFieldDefaulted(m_tif, TIFFTAG_ROWSPERSTRIP, &m_rowsPerStrip);
        if (m_rowsPerStrip == 0 || m_rowsPerStrip > m_srcH)
            m_rowsPerStrip = m_srcH;
        m_nStrips = TIFFNumberOfStrips(m_tif);
        return true;
    }

    // 源缓冲 → CMYK。CMYK 直通取前 4 通道；RGB/Gray 走 lcms。
    void convertToCmyk(const uint8_t *src, uint8_t *dst, int nPixels)
    {
        if (m_imgType == ImgType::CMYK) {
            for (int i = 0; i < nPixels; ++i)
                for (int c = 0; c < 4; ++c)
                    dst[static_cast<size_t>(i) * 4 + c] =
                        src[static_cast<size_t>(i) * m_srcBpp + c];
        } else {
            m_lcms->convert(src, dst, nPixels);
        }
    }

    std::string m_path;
    uint32_t m_srcW = 0, m_srcH = 0;
    int m_srcBpp = 4;
    ImgType m_imgType = ImgType::RGB;
    int m_w = 0, m_h = 0;
    IResampler *m_resampler = nullptr;
    std::unique_ptr<IColorTransform> m_lcms;
    bool m_ok = true;

    // TIFF 懒加载状态
    TIFF *m_tif = nullptr;
    uint32_t m_rowsPerStrip = 0;
    tstrip_t m_nStrips = 0;
    int m_cachedStrip = -1;
    uint32_t m_stripStartRow = 0;
    std::vector<uint8_t> m_rawBuf;
    std::vector<uint8_t> m_stripCmyk;

    // 非 TIFF 状态（cast/校验后的源）
    VipsImage *m_vipsImg = nullptr;
    int m_vipsBands = 0;

    // 读取暂存
    std::vector<uint8_t> m_cmyk;
    std::vector<uint8_t> m_hBand;
};

// ═══════════════════════════════════════════════════════════════════════
//  Factory — open an image primitive as a render-ready RasterSource
// ═══════════════════════════════════════════════════════════════════════
//  Reads metadata (libtiff tags for TIFFs, vips header for others — zero
//  pixel decode), computes the render size, and picks the cheapest source
//  implementation. Returns nullptr on failure (with an error logged).
static std::unique_ptr<RasterSource> openRasterSource(
    const ImageItem &img, const Dpi &dpi, IColorConverter *cv, IResampler *resampler)
{
    // resampler 为空时回退到默认最近邻（直接调用 ImageRenderer::draw/preDecode
    // 的宿主未注入服务时仍可用）。
    static NearestResampler s_defaultResampler;
    if (!resampler)
        resampler = &s_defaultResampler;

    // 1. Detect TIFF by extension
    bool isTiff = hasExtension(img.filePath, ".tif") || hasExtension(img.filePath, ".tiff");

    // 2. Read metadata
    uint32_t srcW = 0, srcH = 0;
    ImgType imgType = ImgType::RGB;
    int srcBpp = 4;
    double imgDpiX = 72.0, imgDpiY = 72.0;
    VipsImage *vipsImg = nullptr; // only used for non-TIFF or full-image paths
    std::vector<uint8_t> srcIcc; // embedded ICC of the source (RGB/Gray)

    if (isTiff) {
        if (!readTiffMeta(img.filePath, srcW, srcH, imgType, srcBpp, imgDpiX, imgDpiY, srcIcc)) {
            static CappedFileSet badFiles;
            std::lock_guard<std::mutex> lock(g_warnMutex);
            if (badFiles.insert(img.filePath))
                EELog::warn("Cannot open image: {}", img.filePath);
            return nullptr;
        }
    } else {
        // RANDOM：DecodedSource 现在惰性按需读行（渲染 strip 可能乱序/多线程），
        // SEQUENTIAL 会在每个 strip 重新从顶部解码到请求行，代价 O(n²)。
        vipsImg = vips_image_new_from_file(
            img.filePath.c_str(), "access", VIPS_ACCESS_RANDOM, nullptr);
        if (!vipsImg) {
            static CappedFileSet badFiles;
            std::lock_guard<std::mutex> lock(g_warnMutex);
            if (badFiles.insert(img.filePath))
                EELog::warn("Cannot open image: {} — {}", img.filePath, vips_error_buffer());
            vips_error_clear();
            return nullptr;
        }

        int sw = vips_image_get_width(vipsImg);
        int sh = vips_image_get_height(vipsImg);
        if (sw <= 0 || sh <= 0) {
            static CappedFileSet dimFiles;
            std::lock_guard<std::mutex> lock(g_warnMutex);
            if (dimFiles.insert(img.filePath))
                EELog::warn("Invalid dimensions ({}x{}): {}", sw, sh, img.filePath);
            g_object_unref(vipsImg);
            return nullptr;
        }
        srcW = static_cast<uint32_t>(sw);
        srcH = static_cast<uint32_t>(sh);

        imgType = classifyVipsImage(vipsImg);
        const int vipsBands = vips_image_get_bands(vipsImg);
        if (imgType == ImgType::CMYK)
            srcBpp = vipsBands;
        else if (imgType == ImgType::Gray)
            srcBpp = 1; // gray(+alpha) → 抽 band 0 → 1 通道
        else
            srcBpp = vipsBands; // RGB：实际 3 或 4 通道

        auto [dpiX, dpiY] = extractVipsImageDpi(vipsImg);
        imgDpiX = dpiX;
        imgDpiY = dpiY;

        // Embedded ICC (JPEG/PNG/…) — preferred source profile for lcms2.
        // vips exposes it as a header blob; copy it now (borrowed pointer).
        {
            const void *iccData = nullptr;
            size_t iccLen = 0;
            if (vips_image_get_blob(vipsImg, VIPS_META_ICC_NAME, &iccData, &iccLen) == 0 && iccData
                && iccLen > 0) {
                srcIcc.assign(static_cast<const uint8_t *>(iccData),
                    static_cast<const uint8_t *>(iccData) + iccLen);
            }
        }
    }

    // 3. Size guard
    if (srcW == 0 || srcH == 0 || srcW > 65535 || srcH > 65535) {
        static CappedFileSet dimFiles;
        std::lock_guard<std::mutex> lock(g_warnMutex);
        if (dimFiles.insert(img.filePath))
            EELog::warn("Invalid dimensions ({}x{}): {}", srcW, srcH, img.filePath);
        if (vipsImg)
            g_object_unref(vipsImg);
        return nullptr;
    }

    // 4. Render dimensions (preserve physical size at canvas DPI, per-axis)
    double dpiScaleX = static_cast<double>(dpi.x) / imgDpiX;
    double dpiScaleY = static_cast<double>(dpi.y) / imgDpiY;

    int renderW, renderH;
    if (img.width > 0 && img.height > 0) {
        renderW = static_cast<int>(img.width + 0.5);
        renderH = static_cast<int>(img.height + 0.5);
    } else if (img.width > 0) {
        renderW = static_cast<int>(img.width + 0.5);
        double aspect = static_cast<double>(srcH) / static_cast<double>(srcW);
        renderH = static_cast<int>(renderW * aspect + 0.5);
    } else if (img.height > 0) {
        renderH = static_cast<int>(img.height + 0.5);
        double aspect = static_cast<double>(srcW) / static_cast<double>(srcH);
        renderW = static_cast<int>(renderH * aspect + 0.5);
    } else {
        renderW = static_cast<int>(srcW * dpiScaleX + 0.5);
        renderH = static_cast<int>(srcH * dpiScaleY + 0.5);
    }
    if (renderW <= 0 || renderH <= 0) {
        if (vipsImg)
            g_object_unref(vipsImg);
        return nullptr;
    }

    bool needsResize = (renderW != static_cast<int>(srcW) || renderH != static_cast<int>(srcH));

    // 5. Log once per source
    {
        static CappedFileSet logged;
        std::lock_guard<std::mutex> lock(g_warnMutex);
        if (logged.insert(img.filePath)) {
            const char *typeLabel = imgType == ImgType::CMYK  ? "CMYK"
                                    : imgType == ImgType::RGB ? "RGB"
                                                              : "Gray";
            EELog::info("  [{}] {}×{} @ ~{:.0f}×{:.0f} DPI -> {}×{} px{}, pos ({},{})", typeLabel,
                srcW, srcH, imgDpiX, imgDpiY, renderW, renderH, needsResize ? " [SCALE]" : "",
                static_cast<int>(img.x + 0.5), static_cast<int>(img.y + 0.5));
        }
    }

    // 6. Cheapest path for the source
    if (isTiff && !needsResize && imgType == ImgType::CMYK)
        return std::make_unique<TiffCmykSource>(img.filePath, srcW, srcH, srcBpp);
    if (isTiff && !needsResize) // RGB/Gray TIFF: strip streaming + lcms2
        return std::make_unique<TiffConvertSource>(
            img.filePath, srcW, srcH, srcBpp, imgType, cv, srcIcc);
    auto decoded = std::make_unique<DecodedSource>(img.filePath, srcW, srcH, srcBpp, imgType,
        renderW, renderH, cv, resampler, srcIcc, vipsImg);
    if (!decoded->ok())
        return nullptr; // 转换句柄构建失败（构造器已记录具体原因）
    return decoded;
}

} // anonymous namespace

// ═══════════════════════════════════════════════════════════════════════
//  draw()
// ═══════════════════════════════════════════════════════════════════════

void ImageRenderer::draw(RenderContext &ctx, const ImageItem &img, const Dpi &dpi)
{
    if (img.filePath.empty())
        return;

    suppressVipsWarnings();

    {
        std::lock_guard<std::mutex> lock(g_warnMutex);
        if (g_badFiles.contains(img.filePath))
            return;
    }

    auto src = openRasterSource(img, dpi, ctx.converter, ctx.resampler);
    if (!src) {
        // Factory already logged the specific failure
        std::lock_guard<std::mutex> lock(g_warnMutex);
        g_badFiles.insert(img.filePath);
        return;
    }

    // Clip against the tile/canvas region
    int renderW = src->width();
    int renderH = src->height();
    int imgX = static_cast<int>(img.x + 0.5);
    int imgY = static_cast<int>(img.y + 0.5);
    int canvasX0 = (ctx.tileW > 0) ? ctx.tileX : 0;
    int canvasY0 = (ctx.tileW > 0) ? ctx.tileY : 0;
    int canvasX1 = (ctx.tileW > 0) ? ctx.tileX + ctx.tileW : ctx.canvasWidth;
    int canvasY1 = (ctx.tileW > 0) ? ctx.tileY + ctx.tileH : ctx.canvasHeight;

    if (imgX + renderW <= canvasX0 || imgY + renderH <= canvasY0 || imgX >= canvasX1
        || imgY >= canvasY1)
        return;

    int outX0 = std::max(canvasX0, imgX);
    int outY0 = std::max(canvasY0, imgY);
    int outX1 = std::min(canvasX1, imgX + renderW);
    int outY1 = std::min(canvasY1, imgY + renderH);

    const int spp = src->samplesPerPixel();

    // Read + blend in row chunks; sources stream internally (strip-level)
    constexpr int kRowChunk = 128;
    std::vector<uint8_t> rows;
    for (int y0 = outY0; y0 < outY1; y0 += kRowChunk) {
        int n = std::min(kRowChunk, outY1 - y0);
        int srcY = y0 - imgY;
        if (!src->readRows(srcY, n, rows)) {
            std::lock_guard<std::mutex> lock(g_warnMutex);
            if (g_badFiles.insert(img.filePath))
                EELog::info("  Skipping image (read failed): {}", img.filePath);
            return;
        }
        blendCmykRows(ctx, rows.data(), renderW, spp, outX0, outX1, y0, y0 + n, imgX, imgY, srcY);
    }
}

// ═══════════════════════════════════════════════════════════════════════
//  preDecode() — cheap early validation of image primitives
// ═══════════════════════════════════════════════════════════════════════
//  Sources decode lazily at render time (readRows streams per band), so there
//  is nothing to warm and no full frame to force-read. preDecode only probes
//  the header/transform so unreadable or unconvertible files are marked bad
//  once, up front (shared g_badFiles) — the render pass then skips them.
bool ImageRenderer::preDecode(
    const ImageItem &img, const Dpi &dpi, IColorConverter *cv, IResampler *resampler)
{
    if (img.filePath.empty())
        return true;

    suppressVipsWarnings();

    {
        std::lock_guard<std::mutex> lock(g_warnMutex);
        if (g_badFiles.contains(img.filePath))
            return false;
    }

    auto src = openRasterSource(img, dpi, cv, resampler);
    if (!src) {
        // Factory already logged the specific failure
        std::lock_guard<std::mutex> lock(g_warnMutex);
        if (g_badFiles.insert(img.filePath))
            EELog::info("  Skipping image (pre-decode failed): {}", img.filePath);
        return false;
    }
    return true;
}
