#include "CircleRenderer.h"
#include "RenderMath.h"
#include "CairoStroke.h"

#include <cmath>
#include <algorithm>

using namespace ATHC::EE;

void CircleRenderer::draw(RenderContext &ctx, const Circle &circle)
{
    if (circle.radiusX <= 0 || circle.radiusY <= 0)
        return;

    double rx = circle.radiusX, ry = circle.radiusY;

    bool hasGradient = circle.gradient.has_value();
    bool hasSolidFill = !hasGradient && circle.fillColor.alpha > 0.0;
    bool hasFill = hasGradient || hasSolidFill;

    uint8_t fc1 = 0, fc2 = 0, fc3 = 0, fc4 = 0, fa = 0;
    if (hasSolidFill)
        ctx.prepareColor(circle.fillColor, fc1, fc2, fc3, fc4, fa);

    if (hasFill) {
        double maxR = std::max(rx, ry) + circle.strokeWidth;
        int clipX0 = (ctx.tileW > 0) ? ctx.tileX : 0;
        int clipY0 = (ctx.tileW > 0) ? ctx.tileY : 0;
        int clipX1 = (ctx.tileW > 0) ? ctx.tileX + ctx.tileW - 1 : ctx.canvasWidth - 1;
        int clipY1 = (ctx.tileW > 0) ? ctx.tileY + ctx.tileH - 1 : ctx.canvasHeight - 1;
        int x0 = std::max(clipX0, static_cast<int>(std::floor(circle.cx - maxR)));
        int x1 = std::min(clipX1, static_cast<int>(std::ceil(circle.cx + maxR)));
        int y0 = std::max(clipY0, static_cast<int>(std::floor(circle.cy - maxR)));
        int y1 = std::min(clipY1, static_cast<int>(std::ceil(circle.cy + maxR)));

        for (int py = y0; py <= y1; ++py) {
            for (int px = x0; px <= x1; ++px) {
                double localX = (px + 0.5) - circle.cx;
                double localY = (py + 0.5) - circle.cy;

                double distFill;
                bool inside;
                bool isCircle = std::abs(rx - ry) < 1e-6;
                if (isCircle) {
                    // True circle — fast path: simple distance calculation
                    double dist = std::hypot(localX, localY);
                    inside = dist < rx;
                    distFill = rx - dist; // positive inside, negative outside
                } else {
                    // Ellipse — Newton iteration for nearest point on ellipse
                    double theta;
                    if (std::abs(localX) < 1e-10 && std::abs(localY) < 1e-10) {
                        theta = M_PI / 2.0;
                    } else {
                        theta = std::atan2(localY, localX);
                    }
                    for (int iter = 0; iter < 16; ++iter) {
                        double ct = std::cos(theta), st = std::sin(theta);
                        double dex = -rx * st, dey = ry * ct;
                        double d2ex = -rx * ct, d2ey = -ry * st;
                        double dx_t = localX - rx * ct, dy_t = localY - ry * st;
                        double f = dx_t * dex + dy_t * dey;
                        double fp = dx_t * d2ex + dy_t * d2ey - dex * dex - dey * dey;
                        if (std::abs(fp) < 1e-20)
                            break;
                        double step = f / fp;
                        theta -= step;
                        if (std::abs(step) < 1e-10 || std::abs(f) < 1e-10)
                            break;
                    }
                    double nearX = rx * std::cos(theta);
                    double nearY = ry * std::sin(theta);
                    distFill = std::hypot(localX - nearX, localY - nearY);
                    inside = (localX * localX) / (rx * rx) + (localY * localY) / (ry * ry) < 1.0;
                    if (!inside)
                        distFill = -distFill;
                }

                uint8_t gfc1 = fc1, gfc2 = fc2, gfc3 = fc3, gfc4 = fc4;
                uint8_t gfa = fa;
                if (hasGradient) {
                    double bw = rx * 2.0, bh = ry * 2.0;
                    double gx = (localX + rx) / bw;
                    double gy = (localY + ry) / bh;
                    ctx.evalGradient(*circle.gradient, gx, gy, gfc1, gfc2, gfc3, gfc4, gfa);
                }
                uint8_t alpha =
                    static_cast<uint8_t>(RenderMath::smoothstep(-0.5, 0.5, distFill) * gfa + 0.5);
                if (alpha > 0)
                    ctx.paintPixel(px, py, gfc1, gfc2, gfc3, gfc4, alpha);
            }
        }
    }

    bool hasStroke = circle.strokeWidth > 0.0 && circle.strokeColor.alpha > 0.0
                     && circle.lineStyle != LineStyle::None;
    if (hasStroke) {
        uint8_t sc1 = 0, sc2 = 0, sc3 = 0, sc4 = 0, sa = 0;
        ctx.prepareColor(circle.strokeColor, sc1, sc2, sc3, sc4, sa);

        bool isCircle = std::abs(rx - ry) < 1e-6;
        double pad = circle.strokeWidth * 0.5 + 2.0;
        double maxR = std::max(rx, ry) + pad;

        CairoStroke::PathBuilder buildPath = [&](cairo_t *cr) {
            cairo_translate(cr, circle.cx, circle.cy);

            if (isCircle) {
                cairo_arc(cr, 0, 0, rx, 0, 2.0 * M_PI);
            } else {
                // Scale to turn ellipse into circle, then use cairo_arc,
                // then the inverse scale happens via the CTM.
                cairo_save(cr);
                cairo_scale(cr, 1.0, ry / rx);
                cairo_arc(cr, 0, 0, rx, 0, 2.0 * M_PI);
                cairo_restore(cr);
            }
        };

        CairoStroke::render(ctx, buildPath, circle.strokeWidth, circle.lineStyle,
            circle.strokeColor, sc1, sc2, sc3, sc4, sa, circle.cx - maxR, circle.cy - maxR,
            circle.cx + maxR, circle.cy + maxR);
    }
}
