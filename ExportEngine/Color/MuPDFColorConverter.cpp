#include "MuPDFColorConverter.h"
#include "IccProfile.h"
#include "FileUtil.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

#include <mupdf/fitz.h>

namespace ATHC::EE {

namespace {

inline fz_context *asCtx(void *p)
{
    return static_cast<fz_context *>(p);
}

inline fz_colorspace *asColorspace(void *p)
{
    return static_cast<fz_colorspace *>(p);
}

// Some MuPDF versions use:
//   fz_new_icc_colorspace(ctx, type, name, buffer)
// while newer versions use:
//   fz_new_icc_colorspace(ctx, type, flags, name, buffer)
//
// This example uses the newer 5-argument form.
// If your MuPDF is older, remove the `0` flags argument.
fz_colorspace *openIccColorspace(fz_context *ctx, const std::vector<uint8_t> &bytes,
    enum fz_colorspace_type type, const char *name)
{
    if (!ctx || bytes.empty())
        return nullptr;

    fz_buffer *buf = nullptr;
    fz_colorspace *cs = nullptr;

    fz_try(ctx)
    {
        buf = fz_new_buffer_from_copied_data(ctx, bytes.data(), bytes.size());
        cs = fz_new_icc_colorspace(ctx, type, 0, name, buf);
    }
    fz_catch(ctx) { cs = nullptr; }

    if (buf)
        fz_drop_buffer(ctx, buf);

    return cs;
}

inline uint8_t floatToByte(float v)
{
    int x = static_cast<int>(std::lround(v * 255.0f));
    if (x < 0)
        return 0;
    if (x > 255)
        return 255;
    return static_cast<uint8_t>(x);
}

} // anonymous namespace

// ============================================================================
// SourceToCmykMuPdf
// ============================================================================

SourceToCmykMuPdf::~SourceToCmykMuPdf()
{
    release();
}

SourceToCmykMuPdf::SourceToCmykMuPdf(SourceToCmykMuPdf &&o) noexcept
    : ok(o.ok), m_ctx(o.m_ctx), m_srcCS(o.m_srcCS), m_dstCS(o.m_dstCS), m_srcBpp(o.m_srcBpp)
{
    o.ok = false;
    o.m_ctx = nullptr;
    o.m_srcCS = nullptr;
    o.m_dstCS = nullptr;
    o.m_srcBpp = 0;
}

SourceToCmykMuPdf &SourceToCmykMuPdf::operator=(SourceToCmykMuPdf &&o) noexcept
{
    if (this != &o) {
        release();

        ok = o.ok;
        m_ctx = o.m_ctx;
        m_srcCS = o.m_srcCS;
        m_dstCS = o.m_dstCS;
        m_srcBpp = o.m_srcBpp;

        o.ok = false;
        o.m_ctx = nullptr;
        o.m_srcCS = nullptr;
        o.m_dstCS = nullptr;
        o.m_srcBpp = 0;
    }
    return *this;
}

void SourceToCmykMuPdf::release() noexcept
{
    if (!m_ctx)
        return;

    fz_context *ctx = asCtx(m_ctx);

    if (m_srcCS) {
        fz_drop_colorspace(ctx, asColorspace(m_srcCS));
        m_srcCS = nullptr;
    }

    if (m_dstCS) {
        fz_drop_colorspace(ctx, asColorspace(m_dstCS));
        m_dstCS = nullptr;
    }

    fz_drop_context(ctx);
    m_ctx = nullptr;
    m_srcBpp = 0;
    ok = false;
}

void SourceToCmykMuPdf::convert(const uint8_t *src, uint8_t *dst, int pixels) const
{
    if (!ok || !src || !dst || pixels <= 0)
        return;

    if (!m_ctx || !m_srcCS || !m_dstCS || m_srcBpp <= 0)
        return;

    fz_context *ctx = asCtx(m_ctx);
    fz_colorspace *srcCS = asColorspace(m_srcCS);
    fz_colorspace *dstCS = asColorspace(m_dstCS);

    int srcN = 0;
    int dstN = 0;

    fz_try(ctx)
    {
        srcN = fz_colorspace_n(ctx, srcCS);
        dstN = fz_colorspace_n(ctx, dstCS);
    }
    fz_catch(ctx) { return; }

    if (srcN <= 0 || dstN <= 0)
        return;

    // Safety: alpha is ignored, so source components cannot exceed srcBpp.
    if (srcN > m_srcBpp)
        srcN = m_srcBpp;

    fz_color_params params = fz_default_color_params;

#ifndef FZ_RI_PERCEPTUAL
#    define FZ_RI_PERCEPTUAL 0
#endif

    params.ri = FZ_RI_PERCEPTUAL;
    params.bp = 1; // black point compensation

    fz_try(ctx)
    {
        for (int i = 0; i < pixels; ++i) {
            const uint8_t *s = src + static_cast<size_t>(i) * m_srcBpp;
            uint8_t *d = dst + static_cast<size_t>(i) * 4;

            float in[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
            float out[4] = { 0.0f, 0.0f, 0.0f, 0.0f };

            // RGB(A): read RGB, skip alpha.
            // Gray(A): read Gray, skip alpha.
            for (int c = 0; c < srcN; ++c) {
                in[c] = static_cast<float>(s[c]) / 255.0f;
            }

            fz_convert_color(ctx, srcCS, in, dstCS, out,
                nullptr, // no proof colorspace
                params);

            const int writeN = std::min(dstN, 4);
            for (int c = 0; c < writeN; ++c) {
                d[c] = floatToByte(out[c]);
            }
            for (int c = writeN; c < 4; ++c) {
                d[c] = 0;
            }
        }
    }
    fz_catch(ctx)
    {
        // Leave already-written pixels as-is.
        // Callers should treat transform failure conservatively.
    }
}

// ============================================================================
// Builders
// ============================================================================

SourceToCmykMuPdf makeRgbToCmykMuPdf(
    const std::vector<uint8_t> &srcIcc, MuPDFColorConverter *cv, int srcBpp)
{
    SourceToCmykMuPdf t;

    if (!cv || !cv->isLoaded())
        return t;

    if (srcBpp != 3 && srcBpp != 4)
        return t;

    std::vector<uint8_t> cmykBytes;
    std::vector<uint8_t> rgbBytes;

    if (!cv->getProfileBytes(cmykBytes))
        return t;

    // Optional fallback RGB profile. Empty is acceptable: device RGB will be used.
    (void)cv->getRgbProfileBytes(rgbBytes);

    fz_context *baseCtx = asCtx(cv->internalContext());
    if (!baseCtx)
        return t;

    // Each transform owns its own context clone, making the transform object
    // independent of the converter lifetime and safe for single-thread use.
    fz_context *ctx = fz_clone_context(baseCtx);
    if (!ctx)
        return t;

    t.m_ctx = ctx;
    t.m_srcBpp = srcBpp;

    // Destination CMYK profile.
    t.m_dstCS = openIccColorspace(ctx, cmykBytes, FZ_COLORSPACE_CMYK, "CMYK");
    if (!t.m_dstCS)
        return t;

    // Source profile preference:
    //   embedded ICC -> project RGB ICC -> MuPDF device RGB fallback.
    fz_colorspace *srcCS = nullptr;

    if (!srcIcc.empty()) {
        srcCS = openIccColorspace(ctx, srcIcc, FZ_COLORSPACE_RGB, "EmbeddedRGB");
    }

    if (!srcCS && !rgbBytes.empty()) {
        srcCS = openIccColorspace(ctx, rgbBytes, FZ_COLORSPACE_RGB, "ProjectRGB");
    }

    if (!srcCS) {
        fz_try(ctx) { srcCS = fz_keep_colorspace(ctx, fz_device_rgb(ctx)); }
        fz_catch(ctx) { srcCS = nullptr; }
    }

    t.m_srcCS = srcCS;

    if (t.m_srcCS && t.m_dstCS) {
        t.ok = true;
        cv->clearErrorCode();
    }

    return t;
}

SourceToCmykMuPdf makeGrayToCmykMuPdf(
    const std::vector<uint8_t> &srcIcc, MuPDFColorConverter *cv, int srcBpp)
{
    SourceToCmykMuPdf t;

    if (!cv || !cv->isLoaded())
        return t;

    if (srcBpp != 1 && srcBpp != 2)
        return t;

    std::vector<uint8_t> cmykBytes;
    std::vector<uint8_t> grayBytes;

    if (!cv->getProfileBytes(cmykBytes))
        return t;

    // Optional Gray profile. If absent, fallback to MuPDF device Gray.
    // This differs slightly from the lcms2 implementation, which could use
    // the RGB profile's gray ramp. MuPDF requires a Gray colorspace for
    // Gray input here.
    (void)cv->getGrayProfileBytes(grayBytes);

    fz_context *baseCtx = asCtx(cv->internalContext());
    if (!baseCtx)
        return t;

    fz_context *ctx = fz_clone_context(baseCtx);
    if (!ctx)
        return t;

    t.m_ctx = ctx;
    t.m_srcBpp = srcBpp;

    // Destination CMYK profile.
    t.m_dstCS = openIccColorspace(ctx, cmykBytes, FZ_COLORSPACE_CMYK, "CMYK");
    if (!t.m_dstCS)
        return t;

    // Source profile preference:
    //   embedded ICC -> project Gray ICC -> MuPDF device Gray fallback.
    fz_colorspace *srcCS = nullptr;

    if (!srcIcc.empty()) {
        srcCS = openIccColorspace(ctx, srcIcc, FZ_COLORSPACE_GRAY, "EmbeddedGray");
    }

    if (!srcCS && !grayBytes.empty()) {
        srcCS = openIccColorspace(ctx, grayBytes, FZ_COLORSPACE_GRAY, "ProjectGray");
    }

    if (!srcCS) {
        fz_try(ctx) { srcCS = fz_keep_colorspace(ctx, fz_device_gray(ctx)); }
        fz_catch(ctx) { srcCS = nullptr; }
    }

    t.m_srcCS = srcCS;

    if (t.m_srcCS && t.m_dstCS) {
        t.ok = true;
        cv->clearErrorCode();
    }

    return t;
}

// ============================================================================
// Chunked conversion
// ============================================================================

bool convertChunked(const SourceToCmykMuPdf &xform, const uint8_t *src, int w, int h, int srcBpp,
    std::vector<uint8_t> &cmykOut, int rowsPerChunk)
{
    if (!xform.ok || !src || w <= 0 || h <= 0 || srcBpp <= 0 || rowsPerChunk <= 0)
        return false;

    cmykOut.resize(static_cast<size_t>(w) * h * 4);

    const uint8_t *s = src;
    uint8_t *d = cmykOut.data();

    for (int y = 0; y < h; y += rowsPerChunk) {
        const int rows = std::min(rowsPerChunk, h - y);
        xform.convert(s, d, w * rows);

        s += static_cast<size_t>(w) * rows * srcBpp;
        d += static_cast<size_t>(w) * rows * 4;
    }

    return true;
}

// ============================================================================
// Construction / destruction
// ============================================================================

MuPDFColorConverter::MuPDFColorConverter()
{
    ensureContext();
}

MuPDFColorConverter::MuPDFColorConverter(CloneTag)
{
    // Clone constructor — context and profiles are initialized by caller.
}

MuPDFColorConverter::~MuPDFColorConverter()
{
    cleanup();
}

bool MuPDFColorConverter::ensureContext()
{
    if (m_ctx)
        return true;

    // If your application already has a global MuPDF context with proper locks,
    // consider cloning that context here instead of creating a new one.
    m_ctx = fz_new_context(nullptr, nullptr, FZ_STORE_DEFAULT);
    if (!m_ctx) {
        m_errorCode = EEError::icc_not_initialized;
        return false;
    }

    fz_context *ctx = asCtx(m_ctx);

    fz_try(ctx) { m_srgbCS = fz_keep_colorspace(ctx, fz_device_rgb(ctx)); }
    fz_catch(ctx) { m_srgbCS = nullptr; }

    return true;
}

void MuPDFColorConverter::cleanupProfiles()
{
    if (!m_ctx)
        return;

    fz_context *ctx = asCtx(m_ctx);

    if (m_rgbCS) {
        fz_drop_colorspace(ctx, asColorspace(m_rgbCS));
        m_rgbCS = nullptr;
    }

    if (m_cmykCS) {
        fz_drop_colorspace(ctx, asColorspace(m_cmykCS));
        m_cmykCS = nullptr;
    }

    if (m_grayCS) {
        fz_drop_colorspace(ctx, asColorspace(m_grayCS));
        m_grayCS = nullptr;
    }

    m_rgbBytes.clear();
    m_cmykBytes.clear();
    m_grayBytes.clear();
}

void MuPDFColorConverter::cleanup()
{
    cleanupProfiles();

    if (!m_ctx)
        return;

    fz_context *ctx = asCtx(m_ctx);

    if (m_srgbCS) {
        fz_drop_colorspace(ctx, asColorspace(m_srgbCS));
        m_srgbCS = nullptr;
    }

    fz_drop_context(ctx);
    m_ctx = nullptr;
}

// ============================================================================
// Profile loading
// ============================================================================

std::error_code MuPDFColorConverter::loadProfile(
    const std::string &rgbProfilePath, const std::string &cmykProfilePath)
{
    m_errorCode = {};

    if (!ensureContext()) {
        m_errorCode = EEError::icc_not_initialized;
        return m_errorCode;
    }

    cleanupProfiles();

    fz_context *ctx = asCtx(m_ctx);

    std::string err;

    auto rgbData = FileUtil::readFileUtf8(rgbProfilePath, err);
    if (rgbData.empty()) {
        m_errorCode = EEError::icc_open_failed;
        return m_errorCode;
    }

    auto cmykData = FileUtil::readFileUtf8(cmykProfilePath, err);
    if (cmykData.empty()) {
        m_errorCode = EEError::icc_open_failed;
        return m_errorCode;
    }

    m_rgbCS = openIccColorspace(ctx, rgbData, FZ_COLORSPACE_RGB, "RGB");
    if (!m_rgbCS) {
        cleanupProfiles();
        m_errorCode = EEError::icc_parse_failed;
        return m_errorCode;
    }

    m_cmykCS = openIccColorspace(ctx, cmykData, FZ_COLORSPACE_CMYK, "CMYK");
    if (!m_cmykCS) {
        cleanupProfiles();
        m_errorCode = EEError::icc_parse_failed;
        return m_errorCode;
    }

    m_rgbBytes = std::move(rgbData);
    m_cmykBytes = std::move(cmykData);

    return {};
}

std::error_code MuPDFColorConverter::loadProfile(const std::string &rgbProfilePath,
    const std::string &cmykProfilePath, const std::string &grayProfilePath)
{
    m_errorCode = {};

    if (auto ec = loadProfile(rgbProfilePath, cmykProfilePath))
        return ec;

    fz_context *ctx = asCtx(m_ctx);

    std::string err;
    auto grayData = FileUtil::readFileUtf8(grayProfilePath, err);
    if (grayData.empty()) {
        m_errorCode = EEError::icc_open_failed;
        return m_errorCode;
    }

    m_grayCS = openIccColorspace(ctx, grayData, FZ_COLORSPACE_GRAY, "Gray");
    if (!m_grayCS) {
        m_errorCode = EEError::icc_parse_failed;
        return m_errorCode;
    }

    m_grayBytes = std::move(grayData);

    return {};
}

// ============================================================================
// Profile byte accessors
// ============================================================================

bool MuPDFColorConverter::getProfileBytes(std::vector<uint8_t> &out)
{
    m_errorCode = {};

    if (m_cmykBytes.empty()) {
        m_errorCode = EEError::icc_not_initialized;
        return false;
    }

    out = m_cmykBytes;
    return true;
}

bool MuPDFColorConverter::getRgbProfileBytes(std::vector<uint8_t> &out)
{
    m_errorCode = {};

    // MuPDF device RGB is only a conversion fallback.
    // It may not be exportable as an ICC profile byte stream.
    // If callers require RGB ICC bytes, an RGB ICC file must be loaded.
    if (!m_rgbBytes.empty()) {
        out = m_rgbBytes;
        return true;
    }

    m_errorCode = EEError::icc_not_initialized;
    return false;
}

bool MuPDFColorConverter::getGrayProfileBytes(std::vector<uint8_t> &out)
{
    m_errorCode = {};

    if (m_grayBytes.empty()) {
        m_errorCode = EEError::icc_not_initialized;
        return false;
    }

    out = m_grayBytes;
    return true;
}

// ============================================================================
// Thread cloning
// ============================================================================

std::unique_ptr<IColorConverter> MuPDFColorConverter::cloneForThread() const
{
    if (!m_ctx || !m_cmykCS || m_cmykBytes.empty())
        return nullptr;

    auto clone = std::make_unique<MuPDFColorConverter>(CloneTag{});

    fz_context *srcCtx = asCtx(m_ctx);

    // Prefer cloning the original context. If your MuPDF build does not
    // support fz_clone_context(), create a new context instead.
    fz_context *newCtx = fz_clone_context(srcCtx);
    if (!newCtx) {
        newCtx = fz_new_context(nullptr, nullptr, FZ_STORE_DEFAULT);
    }

    if (!newCtx)
        return nullptr;

    clone->m_ctx = newCtx;

    fz_try(newCtx) { clone->m_srgbCS = fz_keep_colorspace(newCtx, fz_device_rgb(newCtx)); }
    fz_catch(newCtx) { clone->m_srgbCS = nullptr; }

    // Rebuild profile colorspaces in the new context from original ICC bytes.
    // This avoids sharing mutable MuPDF state across threads.
    clone->m_cmykCS = openIccColorspace(newCtx, m_cmykBytes, FZ_COLORSPACE_CMYK, "CMYK");
    if (!clone->m_cmykCS) {
        return nullptr; // clone destructor cleans up
    }

    if (!m_rgbBytes.empty()) {
        clone->m_rgbCS = openIccColorspace(newCtx, m_rgbBytes, FZ_COLORSPACE_RGB, "RGB");
    }

    if (!m_grayBytes.empty()) {
        clone->m_grayCS = openIccColorspace(newCtx, m_grayBytes, FZ_COLORSPACE_GRAY, "Gray");
    }

    clone->m_rgbBytes = m_rgbBytes;
    clone->m_cmykBytes = m_cmykBytes;
    clone->m_grayBytes = m_grayBytes;

    return clone;
}

// ============================================================================
// Default construction helper
// ============================================================================

std::shared_ptr<MuPDFColorConverter> MuPDFColorConverter::createDefault()
{
    auto cv = std::make_shared<MuPDFColorConverter>();
    cv->loadDefaultProfiles();
    return cv;
}

void MuPDFColorConverter::loadDefaultProfiles()
{
    (void)loadProfile(IccProfile::resolve(IccProfile::kDefaultRgb),
        IccProfile::resolve(IccProfile::kDefaultCmyk),
        IccProfile::resolve(IccProfile::kDefaultGray));
}

// ============================================================================
// IColorConverter
// ============================================================================

std::unique_ptr<IColorTransform> MuPDFColorConverter::makeToCmyk(
    PixelColorSpace src, const std::vector<uint8_t> &embeddedIcc)
{
    switch (src) {
    case PixelColorSpace::Rgb: {
        auto t = makeRgbToCmykMuPdf(embeddedIcc, this, 3);
        if (!t.ok)
            return nullptr;
        return std::make_unique<SourceToCmykMuPdf>(std::move(t));
    }

    case PixelColorSpace::Rgba: {
        auto t = makeRgbToCmykMuPdf(embeddedIcc, this, 4);
        if (!t.ok)
            return nullptr;
        return std::make_unique<SourceToCmykMuPdf>(std::move(t));
    }

    case PixelColorSpace::Gray: {
        auto t = makeGrayToCmykMuPdf(embeddedIcc, this, 1);
        if (!t.ok)
            return nullptr;
        return std::make_unique<SourceToCmykMuPdf>(std::move(t));
    }

    case PixelColorSpace::GrayA: {
        auto t = makeGrayToCmykMuPdf(embeddedIcc, this, 2);
        if (!t.ok)
            return nullptr;
        return std::make_unique<SourceToCmykMuPdf>(std::move(t));
    }

    case PixelColorSpace::Cmyk:
        return nullptr; // pass-through
    }

    return nullptr;
}

} // namespace ATHC::EE
