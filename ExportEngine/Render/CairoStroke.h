#pragma once
#include "RenderContext.h"
#include "SceneData.h"

#include <cairo.h>
#include <functional>
#include <algorithm>
#include <cmath>

// Cairo-based stroke rendering replaces the AGG pipeline.
// AggStroke.h can be deleted once all call sites are migrated.

namespace ATHC::EE {

namespace CairoStroke {

using PathBuilder = std::function<void(cairo_t *)>;

// Configure cairo_t dash / cap / join for the given line style.
// Dash lengths are scaled by stroke width to match AGG behaviour.
inline void setupLineStyle(cairo_t *cr, LineStyle lineStyle, double strokeWidth)
{
    double sw = std::max(strokeWidth, 0.1);

    switch (lineStyle) {
    case LineStyle::Solid:
        cairo_set_line_cap(cr, CAIRO_LINE_CAP_BUTT);
        cairo_set_line_join(cr, CAIRO_LINE_JOIN_MITER);
        return;
    case LineStyle::Dashed: {
        double d[] = { 3.0 * sw, 2.0 * sw };
        cairo_set_dash(cr, d, 2, 0);
        break;
    }
    case LineStyle::Dotted: {
        double d[] = { 1.0 * sw, 2.0 * sw };
        cairo_set_dash(cr, d, 2, 0);
        break;
    }
    case LineStyle::DashDot: {
        double d[] = { 3.0 * sw, 1.5 * sw, 1.0 * sw, 1.5 * sw };
        cairo_set_dash(cr, d, 4, 0);
        break;
    }
    case LineStyle::DoubleDotDash: {
        double d[] = { 3.0 * sw, 1.5 * sw, 1.0 * sw, 1.5 * sw, 1.0 * sw, 1.5 * sw };
        cairo_set_dash(cr, d, 6, 0);
        break;
    }
    default:
        return;
    }

    // Dashed / dotted lines: round caps so short segments render as filled dots.
    // Matches AGG round_cap / round_join behaviour.
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
}

// Stroke a path into an A8 surface and composite the coverage values into
// the RenderContext.  buildPath receives a cairo_t that has already been
// translated so that bbox (x0,y0) maps to surface origin.
//
// bbox must be tight enough to bound the stroked outline (strokeWidth
// already added by caller).  Call sites that don't have a ready bbox can
// compute one from the un-stroked path points.
inline void render(RenderContext &ctx, PathBuilder buildPath, double strokeWidth,
    LineStyle lineStyle, const Color &strokeColor, uint8_t c1, uint8_t c2, uint8_t c3, uint8_t c4,
    uint8_t a, double bboxX0, double bboxY0, double bboxX1, double bboxY1)
{
    if (strokeWidth <= 0.0 || lineStyle == LineStyle::None)
        return;
    if (strokeColor.alpha <= 0.0)
        return;

    // Intersect the stroke bbox with the current render window (plus a 1 px
    // AA fringe) so the A8 surface stays window-sized even for full-canvas
    // strokes — a 120000 px tall line used to build a ~119500 px tall
    // surface and was then silently dropped by the size guard below.
    int clipX0 = (ctx.tileW > 0) ? ctx.tileX : 0;
    int clipX1 = (ctx.tileW > 0) ? ctx.tileX + ctx.tileW : ctx.canvasWidth;
    int clipY0 = (ctx.tileW > 0) ? ctx.tileY : 0;
    int clipY1 = (ctx.tileW > 0) ? ctx.tileY + ctx.tileH : ctx.canvasHeight;

    double sX0 = std::max(bboxX0, static_cast<double>(clipX0)) - 1.0;
    double sY0 = std::max(bboxY0, static_cast<double>(clipY0)) - 1.0;
    double sX1 = std::min(bboxX1, static_cast<double>(clipX1)) + 1.0;
    double sY1 = std::min(bboxY1, static_cast<double>(clipY1)) + 1.0;
    if (sX1 <= sX0 || sY1 <= sY0)
        return; // bbox does not overlap the render window

    // Expand by 1 px for AA fringe, then clamp
    int surfX = std::max(0, static_cast<int>(std::floor(sX0)));
    int surfY = std::max(0, static_cast<int>(std::floor(sY0)));
    int surfW = static_cast<int>(std::ceil(sX1)) - surfX + 2;
    int surfH = static_cast<int>(std::ceil(sY1)) - surfY + 2;

    // Guard against degenerate / out-of-range surfaces
    if (surfW <= 0 || surfH <= 0 || surfW > 32768 || surfH > 32768)
        return;

    // ── Render into A8 surface ──────────────────────────────────────
    cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_A8, surfW, surfH);
    if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surface);
        return;
    }

    cairo_t *cr = cairo_create(surface);
    cairo_translate(cr, -surfX, -surfY);

    buildPath(cr);

    cairo_set_line_width(cr, strokeWidth);
    setupLineStyle(cr, lineStyle, strokeWidth);
    cairo_stroke(cr);

    // ── Read back coverage and blend into output ────────────────────
    cairo_surface_flush(surface);
    const unsigned char *data = cairo_image_surface_get_data(surface);
    int stride = cairo_image_surface_get_stride(surface);

    for (int y = 0; y < surfH; ++y) {
        int cy = surfY + y;
        if (cy < clipY0 || cy >= clipY1)
            continue;
        const unsigned char *row = data + y * stride;
        for (int x = 0; x < surfW; ++x) {
            int cx = surfX + x;
            if (cx < clipX0 || cx >= clipX1)
                continue;
            uint8_t cov = row[x];
            if (cov == 0)
                continue;
            uint8_t effA = static_cast<uint8_t>((cov * a + 127) / 255);
            if (effA > 0)
                ctx.paintPixel(cx, cy, c1, c2, c3, c4, effA);
        }
    }

    cairo_destroy(cr);
    cairo_surface_destroy(surface);
}

} // namespace CairoStroke

} // namespace ATHC::EE
