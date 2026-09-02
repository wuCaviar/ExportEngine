#pragma once
#include <cmath>
#include <algorithm>
#include <cstdint>

#ifndef M_PI
#    define M_PI 3.14159265358979323846264338327950288
#endif
#ifndef M_PI_2
#    define M_PI_2 1.57079632679489661923132169163975144
#endif

// Stateless math utilities for SDF-based rendering.

namespace ATHC::EE {

namespace RenderMath {

inline double smoothstep(double edge0, double edge1, double x)
{
    double t = std::max(0.0, std::min(1.0, (x - edge0) / (edge1 - edge0)));
    return t * t * (3.0 - 2.0 * t);
}

// Euclidean modulo — result is always in [0, m).
inline double floorMod(double x, double m)
{
    double r = std::fmod(x, m);
    return r < 0 ? r + m : r;
}

// Rounded rectangle SDF: returns positive inside, negative outside.
// (px, py): point in local coords centered at rect center, unrotated.
// halfW, halfH: rect half-width/half-height.
// r: corner radius (clamped internally).
inline double roundedRectSDF(double px, double py, double halfW, double halfH, double r)
{
    r = std::min(r, std::min(halfW, halfH));
    if (r < 0)
        r = 0;
    double qx      = std::abs(px) - halfW + r;
    double qy      = std::abs(py) - halfH + r;
    double outside = std::hypot(std::max(qx, 0.0), std::max(qy, 0.0));
    double inside  = std::min(std::max(qx, qy), 0.0);
    return -(outside + inside - r);
}

// Integer alpha blending: dst = (src*a + dst*(255-a) + 127) / 255
// Uses uint16 intermediate; ~3-5x faster than double-precision.
inline uint8_t blendChannel(uint8_t dst, uint8_t src, uint8_t alpha)
{
    if (alpha == 0)
        return dst;
    if (alpha == 255)
        return src;
    return static_cast<uint8_t>(
        (static_cast<uint16_t>(src) * alpha + static_cast<uint16_t>(dst) * (255 - alpha) + 127)
        / 255);
}

inline void blendCmykRow(uint8_t *dst, const uint8_t *src, uint8_t alpha, int pixelCount)
{
    for (int i = 0; i < pixelCount; ++i) {
        dst[0] = blendChannel(dst[0], src[0], alpha);
        dst[1] = blendChannel(dst[1], src[1], alpha);
        dst[2] = blendChannel(dst[2], src[2], alpha);
        dst[3] = blendChannel(dst[3], src[3], alpha);
        dst += 4;
        src += 4;
    }
}
} // namespace RenderMath

} // namespace ATHC::EE
