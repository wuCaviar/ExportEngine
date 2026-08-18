#pragma once
#include "RenderContext.h"
#include "SceneData.h"

#include <cairo.h>
#include <functional>
#include <algorithm>
#include <cmath>

// 基于 Cairo 的描边渲染替代 AGG 管线。
// 一旦所有调用点迁移完成，AggStroke.h 即可删除。

namespace ATHC::EE {

namespace CairoStroke {

using PathBuilder = std::function<void(cairo_t *)>;

// 为给定的线样式配置 cairo_t 的虚线/端点/连接方式。
// 虚线长度按线宽缩放，以匹配 AGG 行为。
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

    // 虚线/点线：圆形端点，使短线段渲染为填充圆点。
    // 匹配 AGG 的 round_cap / round_join 行为。
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
}

// 将路径描边渲染到 A8 表面，并将覆盖值合成到 RenderContext 中。
// buildPath 接收一个已平移的 cairo_t，使得 bbox (x0,y0) 映射到表面原点。
//
// bbox 必须足够紧密以包围描边轮廓（调用者已添加 strokeWidth）。
// 没有现成 bbox 的调用点可以从未描边的路径点计算一个。
inline void render(RenderContext &ctx, PathBuilder buildPath, double strokeWidth,
    LineStyle lineStyle, const Color &strokeColor, uint8_t c1, uint8_t c2, uint8_t c3, uint8_t c4,
    uint8_t a, double bboxX0, double bboxY0, double bboxX1, double bboxY1)
{
    if (strokeWidth <= 0.0 || lineStyle == LineStyle::None)
        return;
    if (strokeColor.alpha <= 0.0)
        return;

    // 将描边 bbox 与当前渲染窗口相交（外加 1 像素 AA 边缘），
    // 因此即使对于全画布描边，A8 表面也保持窗口大小——
    // 一个 120000 像素高的线条曾经构建了一个约 119500 像素高的表面，
    // 然后被下面的尺寸保护静默丢弃。
    int clipX0 = (ctx.tileW > 0) ? ctx.tileX : 0;
    int clipX1 = (ctx.tileW > 0) ? ctx.tileX + ctx.tileW : ctx.canvasWidth;
    int clipY0 = (ctx.tileW > 0) ? ctx.tileY : 0;
    int clipY1 = (ctx.tileW > 0) ? ctx.tileY + ctx.tileH : ctx.canvasHeight;

    double sX0 = std::max(bboxX0, static_cast<double>(clipX0)) - 1.0;
    double sY0 = std::max(bboxY0, static_cast<double>(clipY0)) - 1.0;
    double sX1 = std::min(bboxX1, static_cast<double>(clipX1)) + 1.0;
    double sY1 = std::min(bboxY1, static_cast<double>(clipY1)) + 1.0;
    if (sX1 <= sX0 || sY1 <= sY0)
        return; // bbox 与渲染窗口不重叠

    // 扩展 1 像素用于 AA 边缘，然后钳制
    int surfX = std::max(0, static_cast<int>(std::floor(sX0)));
    int surfY = std::max(0, static_cast<int>(std::floor(sY0)));
    int surfW = static_cast<int>(std::ceil(sX1)) - surfX + 2;
    int surfH = static_cast<int>(std::ceil(sY1)) - surfY + 2;

    // 防止退化/超出范围的表面
    if (surfW <= 0 || surfH <= 0 || surfW > 32768 || surfH > 32768)
        return;

    // ── 渲染到 A8 表面 ──────────────────────────────────────
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

    // ── 读取覆盖值并合成到输出 ────────────────────
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
