#pragma once
#include "RenderContext.h"
#include "SceneData.h"

namespace ATHC::EE {

namespace TextRenderer {
void draw(RenderContext &ctx, const Text &text, const Dpi &dpi);
}

} // namespace ATHC::EE
