#pragma once

#include "SceneData.h"
#include "EEError.h"

#include <string>

namespace ATHC::EE {

class JsonSceneParser
{
public:
    [[nodiscard]] std::error_code parse(const std::string &filePath, Canvas &canvas);
    [[nodiscard]] std::error_code parseFromJson(const std::string &jsonStr, Canvas &canvas);

private:
    Color parseColor(const void *jsonObj); // const nlohmann::json*
    Gradient parseGradient(const void *jsonObj); // const nlohmann::json*
    Rect parseRect(const void *jsonObj, const std::string &canvasUnit, const Dpi &dpi);
    Circle parseCircle(const void *jsonObj, const std::string &canvasUnit, const Dpi &dpi);
    FreeLine parseFreeLine(const void *jsonObj, const std::string &canvasUnit, const Dpi &dpi);
    BezierCurve parseBezierCurve(
        const void *jsonObj, const std::string &canvasUnit, const Dpi &dpi);
    Line parseLine(const void *jsonObj, const std::string &canvasUnit, const Dpi &dpi);
    Text parseText(const void *jsonObj, const std::string &canvasUnit, const Dpi &dpi);
    ImageItem parseImage(const void *jsonObj, const std::string &canvasUnit, const Dpi &dpi);
    GridFill parseGridFill(const void *jsonObj, const Dpi &dpi);
    TextureFill parseTextureFill(const void *jsonObj, const std::string &unit, const Dpi &dpi);
    LineStyle parseLineStyle(
        const void *jsonObj, const std::string &key, LineStyle defaultVal = LineStyle::Solid);
};

} // namespace ATHC::EE
