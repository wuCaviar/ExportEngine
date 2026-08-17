#pragma once
#include "RenderContext.h"
#include "SceneData.h"

namespace ATHC::EE {

class FontEngine;

namespace TextRenderer {
void draw(RenderContext &ctx, const Text &text, FontEngine *fontEngine, const Dpi &dpi);
}

} // namespace ATHC::EE
