#include "ColorTransform.h"

#include <algorithm>

#include "ColorConverter.h"
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
    o.ok = false;
}

SourceToCmykLcms &SourceToCmykLcms::operator=(SourceToCmykLcms &&o) noexcept
{
    if (this != &o) {
        if (m_xform)
            cmsDeleteTransform(static_cast<cmsHTRANSFORM>(m_xform));
        m_xform = o.m_xform;
        ok = o.ok;
        o.m_xform = nullptr;
        o.ok = false;
    }
    return *this;
}

void SourceToCmykLcms::convert(const uint8_t *src, uint8_t *dst, int pixels) const
{
    if (m_xform)
        cmsDoTransform(
            static_cast<cmsHTRANSFORM>(m_xform), src, dst, static_cast<cmsUInt32Number>(pixels));
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

SourceToCmykLcms makeRgbToCmykLcms(
    const std::vector<uint8_t> &srcIcc, ColorConverter *cv, int srcBpp)
{
    SourceToCmykLcms t;
    if (!cv || !cv->isLoaded())
        return t;

    std::vector<uint8_t> fallbackRgb, cmykBytes;
    if (!cv->getRgbProfileBytes(fallbackRgb) || !cv->getProfileBytes(cmykBytes))
        return t;

    cmsHPROFILE src = openProfileFromMem(srcIcc);
    // Embedded profile unparsable / absent → project default RGB profile
    if (!src)
        src = openProfileFromMem(fallbackRgb);
    cmsHPROFILE dst = openProfileFromMem(cmykBytes);

    if (src && dst) {
        cmsUInt32Number inFmt = (srcBpp == 4) ? TYPE_RGBA_8 : TYPE_RGB_8;
        t.m_xform = cmsCreateTransform(src, inFmt, dst, TYPE_CMYK_8, INTENT_PERCEPTUAL,
            cmsFLAGS_BLACKPOINTCOMPENSATION | cmsFLAGS_HIGHRESPRECALC);
    }
    if (src)
        cmsCloseProfile(src);
    if (dst)
        cmsCloseProfile(dst);
    t.ok = t.m_xform != nullptr;
    return t;
}

SourceToCmykLcms makeGrayToCmykLcms(
    const std::vector<uint8_t> &srcIcc, ColorConverter *cv, int srcBpp)
{
    SourceToCmykLcms t;
    if (!cv || !cv->isLoaded())
        return t;

    std::vector<uint8_t> grayBytes, cmykBytes;
    if (!cv->getProfileBytes(cmykBytes))
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
        t.m_xform = cmsCreateTransform(src, inFmt, dst, TYPE_CMYK_8, INTENT_PERCEPTUAL,
            cmsFLAGS_BLACKPOINTCOMPENSATION | cmsFLAGS_HIGHRESPRECALC);
    }
    if (src)
        cmsCloseProfile(src);
    if (dst)
        cmsCloseProfile(dst);
    t.ok = t.m_xform != nullptr;
    return t;
}

bool convertChunked(const SourceToCmykLcms &lcms, const uint8_t *src, int w, int h, int srcBpp,
    std::vector<uint8_t> &cmykOut, int rowsPerChunk)
{
    if (!lcms.ok || !src || w <= 0 || h <= 0 || srcBpp <= 0 || rowsPerChunk <= 0)
        return false;
    cmykOut.resize(static_cast<size_t>(w) * h * 4);
    const uint8_t *s = src;
    uint8_t *d = cmykOut.data();
    for (int y = 0; y < h; y += rowsPerChunk) {
        const int rows = std::min(rowsPerChunk, h - y);
        lcms.convert(s, d, w * rows);
        s += static_cast<size_t>(w) * rows * srcBpp;
        d += static_cast<size_t>(w) * rows * 4;
    }
    return true;
}

} // namespace ATHC::EE
