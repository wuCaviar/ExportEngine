#include "PathRenderer.h"
#include "CairoStroke.h"

#include <cmath>
#include <vector>

using namespace ATHC::EE;

namespace {

struct Vec2
{
    double x, y;
};

// ── Catmull-Rom → cubic Bézier (chordal parameterisation) ──────────
static void
addCatmullRomSegment(cairo_t *cr, const Vec2 &p0, const Vec2 &p1, const Vec2 &p2, const Vec2 &p3)
{
    double t01 = std::hypot(p1.x - p0.x, p1.y - p0.y);
    double t12 = std::hypot(p2.x - p1.x, p2.y - p1.y);
    double t23 = std::hypot(p3.x - p2.x, p3.y - p2.y);

    double m1x = (p2.x - p1.x
                  + t12
                        * ((p1.x - p0.x) / std::max(t01, 1e-10)
                           - (p2.x - p0.x) / std::max(t01 + t12, 1e-10)));
    double m1y = (p2.y - p1.y
                  + t12
                        * ((p1.y - p0.y) / std::max(t01, 1e-10)
                           - (p2.y - p0.y) / std::max(t01 + t12, 1e-10)));
    double m2x = (p2.x - p1.x
                  + t12
                        * ((p3.x - p2.x) / std::max(t23, 1e-10)
                           - (p3.x - p1.x) / std::max(t12 + t23, 1e-10)));
    double m2y = (p2.y - p1.y
                  + t12
                        * ((p3.y - p2.y) / std::max(t23, 1e-10)
                           - (p3.y - p1.y) / std::max(t12 + t23, 1e-10)));

    const double cp1x = p1.x + m1x / 3.0;
    const double cp1y = p1.y + m1y / 3.0;
    const double cp2x = p2.x - m2x / 3.0;
    const double cp2y = p2.y - m2y / 3.0;

    cairo_curve_to(cr, cp1x, cp1y, cp2x, cp2y, p2.x, p2.y);
}

static Vec2 mirrorPoint(const Vec2 &p, const Vec2 &r)
{
    return { 2.0 * p.x - r.x, 2.0 * p.y - r.y };
}

// Quadratic Bézier → cubic Bézier helper
static void
cairoQuadTo(cairo_t *cr, double x0, double y0, double x1, double y1, double x2, double y2)
{
    double c1x = x0 + 2.0 / 3.0 * (x1 - x0);
    double c1y = y0 + 2.0 / 3.0 * (y1 - y0);
    double c2x = x2 + 2.0 / 3.0 * (x1 - x2);
    double c2y = y2 + 2.0 / 3.0 * (y1 - y2);
    cairo_curve_to(cr, c1x, c1y, c2x, c2y, x2, y2);
}

// Compute bounding box of path points, expanded by strokeWidth
static void pathBBox(const std::vector<Vec2> &pts,
                     double                   strokeWidth,
                     double                  &x0,
                     double                  &y0,
                     double                  &x1,
                     double                  &y1)
{
    x0 = y0 = 1e15;
    x1 = y1 = -1e15;
    for (auto &p : pts) {
        x0 = std::min(x0, p.x);
        y0 = std::min(y0, p.y);
        x1 = std::max(x1, p.x);
        y1 = std::max(y1, p.y);
    }
    double pad = strokeWidth * 0.5 + 1.0;
    x0 -= pad;
    y0 -= pad;
    x1 += pad;
    y1 += pad;
}

} // anonymous namespace

// ── FreeLine (Catmull-Rom → cubic Bézier chain) ───────────────────────
void PathRenderer::drawFreeLine(RenderContext &ctx, const FreeLine &line)
{
    if (line.points.size() < 2)
        return;
    if (line.strokeColor.alpha <= 0.0 || line.strokeWidth <= 0.0)
        return;
    if (line.lineStyle == LineStyle::None)
        return;

    uint8_t sc1 = 0, sc2 = 0, sc3 = 0, sc4 = 0, sa = 0;
    ctx.prepareColor(line.strokeColor, sc1, sc2, sc3, sc4, sa);

    const size_t n = line.points.size();

    // Convert to Vec2 for easier maths
    std::vector<Vec2> pts(n);
    for (size_t i = 0; i < n; ++i)
        pts[i] = { line.points[i].first, line.points[i].second };

    // Mirrored endpoints for smooth tangent computation
    Vec2 first = mirrorPoint(pts[0], pts[1]);
    Vec2 last  = mirrorPoint(pts[n - 1], pts[n - 2]);

    // Bounding box
    double bboxX0, bboxY0, bboxX1, bboxY1;
    pathBBox(pts, line.strokeWidth, bboxX0, bboxY0, bboxX1, bboxY1);

    CairoStroke::PathBuilder buildPath = [&](cairo_t *cr) {
        cairo_move_to(cr, pts[0].x, pts[0].y);

        if (n >= 3) {
            addCatmullRomSegment(cr, first, pts[0], pts[1], pts[2]);
        } else {
            cairo_line_to(cr, pts[1].x, pts[1].y);
        }

        for (size_t i = 1; i + 2 < n; ++i)
            addCatmullRomSegment(cr, pts[i - 1], pts[i], pts[i + 1], pts[i + 2]);

        if (n >= 3)
            addCatmullRomSegment(cr, pts[n - 3], pts[n - 2], pts[n - 1], last);
    };

    CairoStroke::render(ctx, buildPath, line.strokeWidth, line.lineStyle, line.strokeColor, sc1,
                        sc2, sc3, sc4, sa, bboxX0, bboxY0, bboxX1, bboxY1);
}

// ── BezierCurve ───────────────────────────────────────────────────────
void PathRenderer::drawBezier(RenderContext &ctx, const BezierCurve &curve)
{
    size_t n = curve.controlPoints.size();
    if (n < 2)
        return;
    if (curve.strokeColor.alpha <= 0.0 || curve.strokeWidth <= 0.0)
        return;
    if (curve.lineStyle == LineStyle::None)
        return;

    uint8_t sc1 = 0, sc2 = 0, sc3 = 0, sc4 = 0, sa = 0;
    ctx.prepareColor(curve.strokeColor, sc1, sc2, sc3, sc4, sa);

    // Collect points into Vec2 for bbox and path building
    std::vector<Vec2> cpts(n);
    for (size_t i = 0; i < n; ++i)
        cpts[i] = { curve.controlPoints[i].first, curve.controlPoints[i].second };

    double bboxX0, bboxY0, bboxX1, bboxY1;
    pathBBox(cpts, curve.strokeWidth, bboxX0, bboxY0, bboxX1, bboxY1);

    CairoStroke::PathBuilder buildPath = [&](cairo_t *cr) {
        cairo_move_to(cr, cpts[0].x, cpts[0].y);

        if (n == 2) {
            cairo_line_to(cr, cpts[1].x, cpts[1].y);
        } else if (n == 3) {
            // Quadratic → cubic
            cairoQuadTo(cr, cpts[0].x, cpts[0].y, cpts[1].x, cpts[1].y, cpts[2].x, cpts[2].y);
        } else {
            for (size_t i = 0; i + 3 < n; i += 3) {
                cairo_curve_to(cr, cpts[i + 1].x, cpts[i + 1].y, cpts[i + 2].x, cpts[i + 2].y,
                               cpts[i + 3].x, cpts[i + 3].y);
            }
        }
    };

    CairoStroke::render(ctx, buildPath, curve.strokeWidth, curve.lineStyle, curve.strokeColor, sc1,
                        sc2, sc3, sc4, sa, bboxX0, bboxY0, bboxX1, bboxY1);
}

// ── Line ──────────────────────────────────────────────────────────────
void PathRenderer::drawLine(RenderContext &ctx, const Line &line)
{
    if (line.strokeColor.alpha <= 0.0 || line.strokeWidth <= 0.0)
        return;
    if (line.lineStyle == LineStyle::None)
        return;
    if (std::hypot(line.x2 - line.x1, line.y2 - line.y1) < 1e-10)
        return;

    uint8_t sc1 = 0, sc2 = 0, sc3 = 0, sc4 = 0, sa = 0;
    ctx.prepareColor(line.strokeColor, sc1, sc2, sc3, sc4, sa);

    double pad    = line.strokeWidth * 0.5 + 1.0;
    double bboxX0 = std::min(line.x1, line.x2) - pad;
    double bboxY0 = std::min(line.y1, line.y2) - pad;
    double bboxX1 = std::max(line.x1, line.x2) + pad;
    double bboxY1 = std::max(line.y1, line.y2) + pad;

    CairoStroke::PathBuilder buildPath = [&](cairo_t *cr) {
        cairo_move_to(cr, line.x1, line.y1);
        cairo_line_to(cr, line.x2, line.y2);
    };

    CairoStroke::render(ctx, buildPath, line.strokeWidth, line.lineStyle, line.strokeColor, sc1,
                        sc2, sc3, sc4, sa, bboxX0, bboxY0, bboxX1, bboxY1);
}
