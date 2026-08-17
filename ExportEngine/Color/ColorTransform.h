#pragma once

#include "IColorConverter.h"

#include <cstdint>
#include <vector>

namespace ATHC::EE {

class ColorConverter;

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
    SourceToCmykLcms(const SourceToCmykLcms &) = delete;
    SourceToCmykLcms &operator=(const SourceToCmykLcms &) = delete;
    SourceToCmykLcms(SourceToCmykLcms &&o) noexcept;
    SourceToCmykLcms &operator=(SourceToCmykLcms &&o) noexcept;

    bool ok = false;

    void convert(const uint8_t *src, uint8_t *dst, int pixels) const override;

private:
    friend SourceToCmykLcms makeRgbToCmykLcms(
        const std::vector<uint8_t> &srcIcc, ColorConverter *cv, int srcBpp);
    friend SourceToCmykLcms makeGrayToCmykLcms(
        const std::vector<uint8_t> &srcIcc, ColorConverter *cv, int srcBpp);

    void *m_xform = nullptr; // cmsHTRANSFORM (opaque, owned — see ColorTransform.cpp)
};

/// Convert a band-interleaved source buffer (w×h pixels, srcBpp bytes per
/// pixel) to CMYK in row chunks of `rowsPerChunk` rows. Byte-identical to a
/// single whole-image convert() call — cmsDoTransform is stateless per
/// pixel — while letting callers avoid holding a second full-size source
/// buffer. Returns false if the transform is not ok or the geometry is
/// inconsistent.
bool convertChunked(const SourceToCmykLcms &lcms, const uint8_t *src, int w, int h, int srcBpp,
    std::vector<uint8_t> &cmykOut, int rowsPerChunk = 256);

/// Build the RGB→CMYK transform. srcIcc = embedded source profile bytes
/// (empty = none); cv provides the fallback RGB and the CMYK profiles.
/// srcBpp selects the input format: 3 = TYPE_RGB_8, 4 = TYPE_RGBA_8
/// (alpha band is ignored by lcms). Rendering intent matches
/// ColorConverter (perceptual).
SourceToCmykLcms makeRgbToCmykLcms(
    const std::vector<uint8_t> &srcIcc, ColorConverter *cv, int srcBpp);

/// Build the Gray→CMYK transform. srcIcc = embedded source profile bytes
/// (empty = none); cv provides the project Gray profile (fallback: the RGB
/// profile used as a gray ramp when only two profiles are loaded) and the
/// CMYK profile. srcBpp selects the input format: 1 = TYPE_GRAY_8,
/// 2 = TYPE_GRAYA_8 (alpha band is ignored by lcms).
SourceToCmykLcms makeGrayToCmykLcms(
    const std::vector<uint8_t> &srcIcc, ColorConverter *cv, int srcBpp);

} // namespace ATHC::EE
