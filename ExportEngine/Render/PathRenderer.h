#pragma once
#include "RenderContext.h"
#include "SceneData.h"

// Stroke-only path primitives (FreeLine, BezierCurve, Line).
// All three use Cairo's path building + CairoStroke pipeline.

namespace ATHC::EE {

namespace PathRenderer {

void drawFreeLine(RenderContext &ctx, const FreeLine &line);
void drawBezier(RenderContext &ctx, const BezierCurve &curve);
void drawLine(RenderContext &ctx, const Line &line);

} // namespace PathRenderer

} // namespace ATHC::EE
