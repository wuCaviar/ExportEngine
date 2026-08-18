#pragma once
#include "RenderContext.h"
#include "SceneData.h"

// 仅描边的路径图元（FreeLine、BezierCurve、Line）。
// 三者均使用 Cairo 的路径构建 + CairoStroke 管线。

namespace ATHC::EE {

namespace PathRenderer {

void drawFreeLine(RenderContext &ctx, const FreeLine &line);
void drawBezier(RenderContext &ctx, const BezierCurve &curve);
void drawLine(RenderContext &ctx, const Line &line);

} // namespace PathRenderer

} // namespace ATHC::EE
