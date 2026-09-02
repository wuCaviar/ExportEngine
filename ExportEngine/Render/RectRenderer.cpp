#include "RectRenderer.h"
#include "RenderMath.h"
#include "CairoStroke.h"
#include "TextureSource.h"

#include <cmath>
#include <algorithm>
#include <memory>

using namespace ATHC::EE;

void RectRenderer::draw(RenderContext &ctx, const Rect &rect, const Dpi &canvasDpi)
{
    if (rect.width <= 0 || rect.height <= 0)
        return;

    // Texture fill source (created once per draw — decoded frames are
    // shared through TextureSource's static cache)
    bool                           hasTexture = rect.textureFill.has_value();
    std::unique_ptr<TextureSource> texSrc;
    double                         tileW = 0.0, tileH = 0.0;
    if (hasTexture) {
        const TextureFill &tf = *rect.textureFill;
        if (tf.filePath.empty()) {
            hasTexture = false;
        } else {
            int overrideW = tf.useOriginalSize ? 0 : static_cast<int>(tf.customWidth + 0.5);
            int overrideH = tf.useOriginalSize ? 0 : static_cast<int>(tf.customHeight + 0.5);
            texSrc        = std::make_unique<TextureSource>(tf.filePath, canvasDpi, ctx.converter,
                                                            overrideW, overrideH);
            if (!texSrc->ok()) {
                hasTexture = false;
                texSrc.reset();
            } else {
                tileW = static_cast<double>(texSrc->tileWidth());
                tileH = static_cast<double>(texSrc->tileHeight());
            }
        }
    }

    double cx    = rect.x + rect.width / 2.0;
    double cy    = rect.y + rect.height / 2.0;
    double halfW = rect.width / 2.0;
    double halfH = rect.height / 2.0;

    bool hasGradient  = rect.gradient.has_value();
    bool hasGrid      = rect.gridFill.has_value();
    bool hasSolidFill = !hasGradient && !hasGrid && !hasTexture && rect.fillColor.alpha > 0.0;
    bool hasFill      = hasGradient || hasGrid || hasSolidFill || hasTexture;

    uint8_t fc1 = 0, fc2 = 0, fc3 = 0, fc4 = 0, fa = 0;
    if (hasSolidFill)
        ctx.prepareColor(rect.fillColor, fc1, fc2, fc3, fc4, fa);

    double r          = rect.cornerRadius;
    bool   useRounded = r > 0.0;

    // Compute bounding box（无旋转，轴对齐）
    double bMinX = rect.x, bMaxX = rect.x + rect.width;
    double bMinY = rect.y, bMaxY = rect.y + rect.height;

    bool hasStroke =
        rect.strokeWidth > 0.0 && rect.strokeColor.alpha > 0.0 && rect.lineStyle != LineStyle::None;
    uint8_t sc1 = 0, sc2 = 0, sc3 = 0, sc4 = 0, sa = 0;
    if (hasStroke)
        ctx.prepareColor(rect.strokeColor, sc1, sc2, sc3, sc4, sa);

    bool   useSdfStroke = hasStroke && rect.lineStyle == LineStyle::Solid;
    double halfSW       = rect.strokeWidth * 0.5;

    // Clip bounds in canvas coordinates (tileW>0 means tiled rendering)
    int clipX0 = (ctx.tileW > 0) ? ctx.tileX : 0;
    int clipY0 = (ctx.tileW > 0) ? ctx.tileY : 0;
    int clipX1 = (ctx.tileW > 0) ? ctx.tileX + ctx.tileW - 1 : ctx.canvasWidth - 1;
    int clipY1 = (ctx.tileW > 0) ? ctx.tileY + ctx.tileH - 1 : ctx.canvasHeight - 1;
    int x0     = std::max(clipX0, static_cast<int>(std::floor(bMinX - halfSW)));
    int x1     = std::min(clipX1, static_cast<int>(std::ceil(bMaxX + halfSW)));
    int y0     = std::max(clipY0, static_cast<int>(std::floor(bMinY - halfSW)));
    int y1     = std::min(clipY1, static_cast<int>(std::ceil(bMaxY + halfSW)));

    // Per-pixel loop (fill + SDF stroke)
    if (hasFill || useSdfStroke) {
        for (int py = y0; py <= y1; ++py) {
            for (int px = x0; px <= x1; ++px) {
                double dpx = px + 0.5, dpy = py + 0.5;
                double localX = dpx - cx; // 无旋转：局部坐标 == 画布偏移
                double localY = dpy - cy;

                double distFill;
                if (useRounded)
                    distFill = RenderMath::roundedRectSDF(localX, localY, halfW, halfH, r);
                else
                    distFill = std::min(halfW - std::abs(localX), halfH - std::abs(localY));

                if (hasFill) {
                    uint8_t gfc1 = fc1, gfc2 = fc2, gfc3 = fc3, gfc4 = fc4;
                    uint8_t gfa = fa;
                    if (hasGradient) {
                        double gx = (localX + halfW) / rect.width;
                        double gy = (localY + halfH) / rect.height;
                        ctx.evalGradient(*rect.gradient, gx, gy, gfc1, gfc2, gfc3, gfc4, gfa);
                    } else if (hasGrid) {
                        if (!ctx.evalGridFill(*rect.gridFill, localX + halfW, localY + halfH,
                                              rect.width, rect.height, gfc1, gfc2, gfc3, gfc4, gfa))
                            gfa = 0; // invalid grid params — skip fill (parse already warned)
                    } else if (hasTexture) {
                        // Tile coordinate in the unrotated rect-local space,
                        // wrapped by the pattern cell (floorMod → [0, tile)).
                        const TextureFill &tf = *rect.textureFill;
                        double             rx = localX + halfW + tf.offsetX;
                        double             ry = localY + halfH + tf.offsetY;
                        double             u  = RenderMath::floorMod(rx, tileW) / tileW;
                        double             v  = RenderMath::floorMod(ry, tileH) / tileH;
                        if (!texSrc->sample(u, v, gfc1, gfc2, gfc3, gfc4, gfa))
                            gfa = 0; // sample failure — skip this pixel
                    }
                    uint8_t alpha = static_cast<uint8_t>(
                        RenderMath::smoothstep(-0.5, 0.5, distFill) * gfa + 0.5);
                    if (alpha > 0)
                        ctx.paintPixel(px, py, gfc1, gfc2, gfc3, gfc4, alpha);
                }

                if (useSdfStroke) {
                    double sAlpha =
                        RenderMath::smoothstep(-0.5, 0.5, halfSW - std::abs(distFill)) * sa;
                    if (sAlpha > 0) {
                        uint8_t aVal = static_cast<uint8_t>(sAlpha + 0.5);
                        ctx.paintPixel(px, py, sc1, sc2, sc3, sc4, aVal);
                    }
                }
            }
        }
    }

    // Cairo stroke for dashed / dotted lines (non-solid styles)
    if (hasStroke && !useSdfStroke) {
        double clampedR = useRounded ? std::min(r, std::min(halfW, halfH)) : 0.0;
        double pad      = rect.strokeWidth * 0.5 + 2.0;

        CairoStroke::PathBuilder buildPath = [&](cairo_t *cr) {
            // Translate to rect centre, then build the local-coord path.
            // Cairo applies transforms in reverse order: the last transform added
            // is applied first to user-space coordinates.
            // Result: translate(-surfX,-surfY) · translate(cx,cy) · local_pts
            cairo_translate(cr, cx, cy);

            if (clampedR > 0.0) {
                double x0 = -halfW, y0 = -halfH;
                double x1 = halfW, y1 = halfH;
                double rr = clampedR;

                cairo_new_sub_path(cr);
                cairo_arc(cr, x1 - rr, y0 + rr, rr, -M_PI_2, 0.0);
                cairo_arc(cr, x1 - rr, y1 - rr, rr, 0.0, M_PI_2);
                cairo_arc(cr, x0 + rr, y1 - rr, rr, M_PI_2, M_PI);
                cairo_arc(cr, x0 + rr, y0 + rr, rr, M_PI, 3.0 * M_PI_2);
                cairo_close_path(cr);
            } else {
                cairo_move_to(cr, -halfW, -halfH);
                cairo_line_to(cr, halfW, -halfH);
                cairo_line_to(cr, halfW, halfH);
                cairo_line_to(cr, -halfW, halfH);
                cairo_close_path(cr);
            }
        };

        CairoStroke::render(ctx, buildPath, rect.strokeWidth, rect.lineStyle, rect.strokeColor, sc1,
                            sc2, sc3, sc4, sa, bMinX - pad, bMinY - pad, bMaxX + pad, bMaxY + pad);
    }
}
