#include "RenderContext.h"
#include "RenderMath.h"

#include <cmath>
#include <algorithm>

using namespace ATHC::EE;

// ============================================================
//  Initialization
// ============================================================

void RenderContext::initFullCanvas(int w, int h)
{
    canvasWidth  = w;
    canvasHeight = h;
    tileX        = 0;
    tileY        = 0;
    tileW        = 0;
    tileH        = 0;
    cmykBuf.resize(static_cast<size_t>(w) * static_cast<size_t>(h) * samplesPerPixel, 0);
}

void RenderContext::initTile(int cw, int ch, int tx, int ty, int tw, int th)
{
    canvasWidth  = cw;
    canvasHeight = ch;
    tileX        = tx;
    tileY        = ty;
    tileW        = tw;
    tileH        = th;
    cmykBuf.resize(static_cast<size_t>(tw) * static_cast<size_t>(th) * samplesPerPixel, 0);
}

void RenderContext::setSamplesPerPixel(int spp)
{
    if (spp == samplesPerPixel)
        return;
    int                  ew = effectiveWidth(), eh = effectiveHeight();
    int                  oldSpp = samplesPerPixel;
    std::vector<uint8_t> newBuf(static_cast<size_t>(ew) * static_cast<size_t>(eh) * spp, 0);
    const uint8_t       *src = cmykBuf.data();
    uint8_t             *dst = newBuf.data();
    // Copy min(oldSpp, spp) channels per pixel; extra channels are zeroed
    int copyCh = oldSpp < spp ? oldSpp : spp;
    for (int i = 0, n = ew * eh; i < n; ++i, src += oldSpp, dst += spp)
        std::memcpy(dst, src, copyCh);
    cmykBuf         = std::move(newBuf);
    extrasamples    = spp - 4;
    samplesPerPixel = spp;
}

void RenderContext::fillBackground(const Color &bg)
{
    // CMYK-only：背景色直接按 C/M/Y/K 4 通道写入 cmykBuf。
    uint8_t c  = static_cast<uint8_t>(bg.ch[0] + 0.5);
    uint8_t m  = static_cast<uint8_t>(bg.ch[1] + 0.5);
    uint8_t y  = static_cast<uint8_t>(bg.ch[2] + 0.5);
    uint8_t k  = static_cast<uint8_t>(bg.ch[3] + 0.5);
    uint8_t a  = static_cast<uint8_t>(bg.alpha * 255 + 0.5);
    int     ew = effectiveWidth(), eh = effectiveHeight();
    if (a == 255) {
        // Solid fill — buffer is zeroed, write CMYK channels only.
        // Extra channels (index 4+) remain 0 (from init resize).
        size_t   count = static_cast<size_t>(ew) * eh;
        uint8_t *p     = cmykBuf.data();
        for (size_t i = 0; i < count; ++i) {
            p[0] = c;
            p[1] = m;
            p[2] = y;
            p[3] = k;
            p += samplesPerPixel;
        }
    } else if (a > 0) {
        // Use canvas coordinates so blendCmyk can translate to tile-local
        int x0 = (tileW > 0) ? tileX : 0;
        int y0 = (tileW > 0) ? tileY : 0;
        for (int py = 0; py < eh; ++py)
            for (int px = 0; px < ew; ++px)
                blendCmyk(x0 + px, y0 + py, c, m, y, k, a);
    }
}

// ============================================================
//  Pixel blending
// ============================================================

void RenderContext::blendCmyk(int x, int y, uint8_t c, uint8_t m, uint8_t y_, uint8_t k, uint8_t a)
{
    if (tileW > 0) {
        x -= tileX;
        y -= tileY;
        if (x < 0 || x >= tileW || y < 0 || y >= tileH)
            return;
    } else if (x < 0 || x >= canvasWidth || y < 0 || y >= canvasHeight) {
        return;
    }
    size_t idx =
        (static_cast<size_t>(y) * static_cast<size_t>(effectiveWidth()) + static_cast<size_t>(x))
        * samplesPerPixel;
    if (a == 255) {
        cmykBuf[idx + 0] = c;
        cmykBuf[idx + 1] = m;
        cmykBuf[idx + 2] = y_;
        cmykBuf[idx + 3] = k;
        // Opaque pixel: zero extra channels (spot colors do not bleed
        // through fully-opaque vector elements drawn on top).
        for (int ch = 4; ch < samplesPerPixel; ++ch)
            cmykBuf[idx + ch] = 0;
    } else if (a > 0) {
        cmykBuf[idx + 0] = RenderMath::blendChannel(cmykBuf[idx + 0], c, a);
        cmykBuf[idx + 1] = RenderMath::blendChannel(cmykBuf[idx + 1], m, a);
        cmykBuf[idx + 2] = RenderMath::blendChannel(cmykBuf[idx + 2], y_, a);
        cmykBuf[idx + 3] = RenderMath::blendChannel(cmykBuf[idx + 3], k, a);
    }
}

// ============================================================
//  Gradient evaluation
// ============================================================

bool RenderContext::evalGradient(const Gradient &g,
                                 double          gx,
                                 double          gy,
                                 uint8_t        &c1,
                                 uint8_t        &c2,
                                 uint8_t        &c3,
                                 uint8_t        &c4,
                                 uint8_t        &a)
{
    double t = 0.0;

    if (g.type == Gradient::LINEAR) {
        double dx = g.x2 - g.x1, dy = g.y2 - g.y1;
        double lenSq = dx * dx + dy * dy;
        t            = (lenSq > 1e-12) ? ((gx - g.x1) * dx + (gy - g.y1) * dy) / lenSq : 0.0;
    } else if (g.type == Gradient::RADIAL) {
        double dx = gx - g.cx, dy = gy - g.cy;
        t = std::hypot(dx, dy) / std::max(1e-10, g.r);
    } else if (g.type == Gradient::CONIC) {
        // CSS conic-gradient convention: 0° at 12 o'clock, clockwise.
        // atan2(y, x) gives angle from 3 o'clock counterclockwise.
        // Swap axes + negate y to align: atan2(dx, -dy) measures from top clockwise.
        double angle = std::atan2(gx - g.cx, -(gy - g.cy)) * 180.0 / M_PI;
        t            = std::fmod(angle - g.startAngle + 360.0, 360.0) / 360.0;
    }

    t = std::max(0.0, std::min(1.0, t));

    const auto &stops = g.stops;
    if (stops.size() < 2)
        return false;

    // Find the segment [stops[i], stops[i+1]] that brackets t.
    // Precondition: stops are sorted by offset ascending (enforced by parser).
    size_t i = 0;
    while (i + 1 < stops.size() && stops[i + 1].offset < t)
        ++i;
    if (i + 1 >= stops.size())
        i = stops.size() - 2;

    double t0 = stops[i].offset, t1 = stops[i + 1].offset;
    double localT = (t1 - t0 > 1e-10) ? (t - t0) / (t1 - t0) : 0.0;
    localT        = std::max(0.0, std::min(1.0, localT));

    const Color &c0  = stops[i].color;
    const Color &c1c = stops[i + 1].color;

    double invT = 1.0 - localT;
    c1          = static_cast<uint8_t>(c0.ch[0] * invT + c1c.ch[0] * localT + 0.5);
    c2          = static_cast<uint8_t>(c0.ch[1] * invT + c1c.ch[1] * localT + 0.5);
    c3          = static_cast<uint8_t>(c0.ch[2] * invT + c1c.ch[2] * localT + 0.5);
    c4          = static_cast<uint8_t>(c0.ch[3] * invT + c1c.ch[3] * localT + 0.5);
    a           = static_cast<uint8_t>((c0.alpha * invT + c1c.alpha * localT) * 255 + 0.5);

    return true;
}

bool RenderContext::evalGridFill(const GridFill &g,
                                 double          px,
                                 double          py,
                                 double          rectW,
                                 double          rectH,
                                 uint8_t        &c1,
                                 uint8_t        &c2,
                                 uint8_t        &c3,
                                 uint8_t        &c4,
                                 uint8_t        &a)
{
    if (g.cellWidth <= 0 || g.cellHeight <= 0 || g.lineWidth <= 0)
        return false;

    // Distance to the nearest vertical/horizontal grid line centre.
    double dx   = px - std::round(px / g.cellWidth) * g.cellWidth;
    double dy   = py - std::round(py / g.cellHeight) * g.cellHeight;
    double dist = std::min(std::abs(dx), std::abs(dy));

    // 固定 1px 网格线：线宽固定为 1 像素（解析器保证 lineWidth == 1.0），
    // 恰好覆盖像素中心落在半线宽内的最近像素。1px 线没有做 smoothstep
    // 抗锯齿的空间——smoothstep 会把墨迹摊到相邻像素（实测产生 ~2px
    // 软边线），改用最近像素硬切保证每条线严格 1 像素。
    double lineFactor = (dist <= g.lineWidth * 0.5) ? 1.0 : 0.0;

    const Color &grid = g.gridColor;
    const Color &bg   = g.backgroundColor;
    double       lf   = lineFactor;
    // Transparent background: background contributes nothing. Coverage goes
    // into alpha only; channels are unscaled so the straight-alpha blend
    // (blendChannel) gets pure grid colour with linear coverage — scaling
    // both channels and alpha would square the coverage at AA edges.
    double gridFactor = g.transparentBackground ? 1.0 : lineFactor;
    double bf         = g.transparentBackground ? 0.0 : (1.0 - lineFactor);
    double bgAlpha    = g.transparentBackground ? 0.0 : bg.alpha;

    c1 = static_cast<uint8_t>(grid.ch[0] * gridFactor + bg.ch[0] * bf + 0.5);
    c2 = static_cast<uint8_t>(grid.ch[1] * gridFactor + bg.ch[1] * bf + 0.5);
    c3 = static_cast<uint8_t>(grid.ch[2] * gridFactor + bg.ch[2] * bf + 0.5);
    c4 = static_cast<uint8_t>(grid.ch[3] * gridFactor + bg.ch[3] * bf + 0.5);
    a  = static_cast<uint8_t>((grid.alpha * lf + bgAlpha * bf) * 255 + 0.5);
    return true;
}
