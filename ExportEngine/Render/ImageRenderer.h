#pragma once
#include "RenderContext.h"
#include "SceneData.h"

// CMYK TIFF image rendering with strip/band streaming to bound memory usage.
// Sources decode + colour-convert + scale per band at render time (no full
// resampled frame is ever materialized). Peak memory ≈ one source band +
// the requested output rows.

namespace ATHC::EE {

class IColorConverter;
class IResampler;

namespace ImageRenderer {

void draw(RenderContext &ctx, const ImageItem &img, const Dpi &dpi);

/// Cheap early validation of an image primitive (header/transform probe) so
/// unreadable or unconvertible files are marked bad once, up front. Sources
/// decode lazily at render time, so there is no frame to warm. Returns false
/// on failure (already marked bad — render will skip).
bool preDecode(const ImageItem &img, const Dpi &dpi, IColorConverter *cv, IResampler *resampler);

} // namespace ImageRenderer

} // namespace ATHC::EE
