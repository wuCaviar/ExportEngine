#pragma once
#include "RenderContext.h"
#include "SceneData.h"

namespace ATHC::EE {

namespace RectRenderer {
void draw(RenderContext &ctx, const Rect &rect, const Dpi &canvasDpi);
}

} // namespace ATHC::EE
