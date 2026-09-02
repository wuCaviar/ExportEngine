#pragma once

#include <vips/vips.h>

#include <utility>

namespace ATHC::EE {

// Extract the DPI of a vips-loaded raster image, per axis (x, y).
//
// vips loaders (PNG/JPEG/…) do NOT expose a "dpi" metadata field —
// resolution lives in the image header's xres/yres, in pixels per
// millimetre regardless of the source unit (VIPS_META_RESOLUTION_UNIT
// only records the original unit, for savers to pick an output unit).
// Shared by ImageRenderer and TextureSource (both decode with vips but
// convert colour via lcms2). Each axis falls back to 72.0 when no valid
// resolution is present.
inline std::pair<double, double> extractVipsImageDpi(VipsImage *img)
{
    double dpiX = vips_image_get_xres(img) * 25.4; // px/mm → px/inch
    double dpiY = vips_image_get_yres(img) * 25.4;
    return { dpiX >= 1.0 ? dpiX : 72.0, dpiY >= 1.0 ? dpiY : 72.0 };
}

} // namespace ATHC::EE
