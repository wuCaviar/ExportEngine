#include "JsonSceneParser.h"
#include "Log.h"

#include <nlohmann/json.hpp>

#include <fstream>
#include <cmath>
#include <algorithm>
#include <filesystem>

using json = nlohmann::json;
using namespace ATHC::EE;

namespace {
// 内部异常：颜色解析错误向上抛，由 parseFromJson 统一收敛为错误码。
// 与 json::exception → parse_invalid_json 的模式一致，parseRect 等
// 中间层无需改动签名。
struct ColorParseError
{
    std::error_code ec;
};
} // namespace

// -- unit helpers ----------------------------------------------------------

// Read optional "unit" field, falling back to canvas unit
static std::string resolveUnit(const json &j, const std::string &canvasUnit)
{
    return j.value("unit", canvasUnit);
}

// Convert a length value from its declared unit to pixels
//   px = identity
//   mm = value * dpi / 25.4
static double toPixels(double value, const std::string &unit, int dpi)
{
    if (unit == "mm")
        return value * static_cast<double>(dpi) / 25.4;
    // "px" or unknown — pass through unchanged
    return value;
}

// 方向性换算：X 方向尺寸用 dpi.x，Y 方向用 dpi.y
static double toPixelsX(double value, const std::string &unit, const Dpi &dpi)
{
    return toPixels(value, unit, dpi.x);
}

static double toPixelsY(double value, const std::string &unit, const Dpi &dpi)
{
    return toPixels(value, unit, dpi.y);
}

// Validate unit value; returns true for recognised units
static bool isValidUnit(const std::string &unit)
{
    return unit == "px" || unit == "mm";
}

std::error_code JsonSceneParser::parse(const std::string &filePath, Canvas &canvas)
{
    // UTF-8 path → wide via u8path, so MSVC's CRT (ANSI code page) can't
    // mangle Chinese characters in the JSON file path.
    std::ifstream ifs(std::filesystem::u8path(filePath), std::ios::binary);
    if (!ifs.is_open())
        return EEError::parse_cannot_open_file;
    std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    return parseFromJson(content, canvas);
}

std::error_code JsonSceneParser::parseFromJson(const std::string &jsonStr, Canvas &canvas)
{
    json root;
    try {
        root = json::parse(jsonStr);

        if (!root.is_object())
            return EEError::parse_root_not_object;

        auto canvasObj = root.value("canvas", json::object());

        // -- unit (canvas-level default, inherited by primitives that don't specify) --
        std::string canvasUnit = canvasObj.value("unit", "px");
        if (!isValidUnit(canvasUnit))
            return EEError::parse_invalid_canvas_unit;

        // -- canvas dimensions (convert to pixels before storing) --
        int rawWidth = canvasObj.value("width", 800);
        int rawHeight = canvasObj.value("height", 600);
        // -- dpi（X/Y 独立，缺省 300）--
        // 旧字段 dpi 已废弃：存在且新字段均缺失时报错，防止迁移时静默回退 300
        bool hasDpiX = canvasObj.contains("dpiX");
        bool hasDpiY = canvasObj.contains("dpiY");
        if (canvasObj.contains("dpi") && !hasDpiX && !hasDpiY)
            return EEError::parse_dpi_deprecated;
        canvas.dpi.x = canvasObj.value("dpiX", 300);
        canvas.dpi.y = canvasObj.value("dpiY", 300);

        if (rawWidth <= 0 || rawHeight <= 0)
            return EEError::parse_dimensions_not_positive;
        if (canvas.dpi.x <= 0 || canvas.dpi.y <= 0)
            return EEError::parse_dpi_not_positive;

        // convert canvas width/height from declared unit to pixels
        canvas.width = static_cast<int>(
            std::round(toPixelsX(static_cast<double>(rawWidth), canvasUnit, canvas.dpi)));
        canvas.height = static_cast<int>(
            std::round(toPixelsY(static_cast<double>(rawHeight), canvasUnit, canvas.dpi)));

        if (canvas.width <= 0 || canvas.height <= 0)
            return EEError::parse_converted_dimensions_not_positive;

        if (canvasObj.contains("background"))
            canvas.background = parseColor(&canvasObj["background"]);

        canvas.rects.clear();
        if (canvasObj.contains("rects"))
            for (auto &val : canvasObj["rects"])
                canvas.rects.push_back(parseRect(&val, canvasUnit, canvas.dpi));

        canvas.circles.clear();
        if (canvasObj.contains("circles"))
            for (auto &val : canvasObj["circles"])
                canvas.circles.push_back(parseCircle(&val, canvasUnit, canvas.dpi));

        canvas.freeLines.clear();
        if (canvasObj.contains("freeLines"))
            for (auto &val : canvasObj["freeLines"])
                canvas.freeLines.push_back(parseFreeLine(&val, canvasUnit, canvas.dpi));

        canvas.bezierCurves.clear();
        if (canvasObj.contains("bezierCurves"))
            for (auto &val : canvasObj["bezierCurves"])
                canvas.bezierCurves.push_back(parseBezierCurve(&val, canvasUnit, canvas.dpi));

        canvas.lines.clear();
        if (canvasObj.contains("lines"))
            for (auto &val : canvasObj["lines"])
                canvas.lines.push_back(parseLine(&val, canvasUnit, canvas.dpi));

        canvas.texts.clear();
        if (canvasObj.contains("texts"))
            for (auto &val : canvasObj["texts"])
                canvas.texts.push_back(parseText(&val, canvasUnit, canvas.dpi));

        canvas.images.clear();
        if (canvasObj.contains("images"))
            for (auto &val : canvasObj["images"])
                canvas.images.push_back(parseImage(&val, canvasUnit, canvas.dpi));

        return {};
    } catch (const ColorParseError &e) {
        return e.ec;
    } catch (const json::exception &) {
        // parse_error（语法）与 type_error（字段类型不匹配，如 "width":"abc"）等
        // nlohmann 异常统一视为解析失败——不得让异常跨 DLL 边界逃逸到调用方
        return EEError::parse_invalid_json;
    }
}

Color JsonSceneParser::parseColor(const void *obj)
{
    const auto &j = *static_cast<const json *>(obj);
    if (!j.is_object())
        throw ColorParseError{ EEError::parse_invalid_color };

    // CMYK-only：space 字段已移除，r/g/b 颜色已移除——显式报错防止
    // 旧格式静默解析出错误颜色。
    if (j.contains("space"))
        throw ColorParseError{ EEError::parse_color_space_field_removed };
    if (j.contains("r") || j.contains("g") || j.contains("b"))
        throw ColorParseError{ EEError::parse_rgba_color_removed };

    Color color = Color::fromCMYK(j.value("c", 0.0) * 255.0 / 100.0,
        j.value("m", 0.0) * 255.0 / 100.0, j.value("y", 0.0) * 255.0 / 100.0,
        j.value("k", 0.0) * 255.0 / 100.0, j.value("a", 1.0));

    // Gradient must not be nested inside a color object — it is a peer key.
    if (j.contains("gradient")) {
        EELog::warn("Gradient must not be nested inside a color object. "
                    "Use \"gradient\" as a peer key alongside \"fillColor\" on the primitive.");
        return Color{}; // 默认黑，行为同现状
    }

    return color;
}

Gradient JsonSceneParser::parseGradient(const void *obj)
{
    const auto &g = *static_cast<const json *>(obj);
    Gradient grad;

    std::string type = g.value("type", "");
    if (type == "linear")
        grad.type = Gradient::LINEAR;
    else if (type == "radial")
        grad.type = Gradient::RADIAL;
    else if (type == "conic")
        grad.type = Gradient::CONIC;

    if (grad.type != Gradient::NONE && g.contains("stops")) {
        for (auto &stop : g["stops"]) {
            double offset = stop.value("offset", 0.0);
            if (!stop.contains("color"))
                continue;
            grad.stops.push_back({ offset, parseColor(&stop["color"]) });
        }
        // Sort stops by offset ascending — evalGradient requires sorted input
        // for correct segment lookup via linear scan.
        if (grad.stops.size() > 1) {
            std::sort(grad.stops.begin(), grad.stops.end(),
                [](const Gradient::Stop &a, const Gradient::Stop &b) {
                    return a.offset < b.offset;
                });
        }
    }

    grad.x1 = g.value("x1", 0.0);
    grad.y1 = g.value("y1", 0.0);
    grad.x2 = g.value("x2", 1.0);
    grad.y2 = g.value("y2", 0.0);
    grad.cx = g.value("cx", 0.5);
    grad.cy = g.value("cy", 0.5);
    grad.r = g.value("r", 0.5);
    grad.startAngle = g.value("startAngle", 0.0);

    return grad;
}

GridFill JsonSceneParser::parseGridFill(const void *obj, const Dpi &dpi)
{
    const auto &j = *static_cast<const json *>(obj);
    GridFill g;
    if (j.contains("gridColor"))
        g.gridColor = parseColor(&j["gridColor"]);
    if (j.contains("backgroundColor"))
        g.backgroundColor = parseColor(&j["backgroundColor"]);
    g.transparentBackground = j.value("transparentBackground", false);

    // 网格尺寸固定：格子 = 1mm（X 用 dpiX、Y 用 dpiY 换算为像素），线宽 = 1px
    g.cellWidth = static_cast<double>(dpi.x) / 25.4;
    g.cellHeight = static_cast<double>(dpi.y) / 25.4;
    g.lineWidth = 1.0;
    for (const char *k : { "cellWidth", "cellHeight", "lineWidth" })
        if (j.contains(k))
            EELog::warn("GridFill field '{}' is deprecated and ignored — grid cell "
                        "is fixed at 1mm and line width at 1px, computed from the "
                        "canvas dpi.",
                k);
    return g;
}

TextureFill JsonSceneParser::parseTextureFill(
    const void *obj, const std::string &unit, const Dpi &dpi)
{
    const auto &j = *static_cast<const json *>(obj);
    TextureFill t;
    t.filePath = j.value("filePath", "");
    t.offsetX = toPixelsX(j.value("offsetX", 0.0), unit, dpi);
    t.offsetY = toPixelsY(j.value("offsetY", 0.0), unit, dpi);
    t.useOriginalSize = j.value("useOriginalSize", false);
    t.customWidth = toPixelsX(j.value("customWidth", 0.0), unit, dpi);
    t.customHeight = toPixelsY(j.value("customHeight", 0.0), unit, dpi);
    if (t.filePath.empty())
        EELog::warn("TextureFill has empty filePath — fill skipped.");
    else if (!t.useOriginalSize && (t.customWidth <= 0 || t.customHeight <= 0))
        EELog::warn("TextureFill has non-positive customWidth/customHeight — "
                    "that dimension falls back to original-size scaling.");
    return t;
}

LineStyle JsonSceneParser::parseLineStyle(
    const void *obj, const std::string &key, LineStyle defaultVal)
{
    const auto &j = *static_cast<const json *>(obj);
    if (!j.contains(key))
        return defaultVal;
    std::string val = j.value(key, "");
    if (val == "solid")
        return LineStyle::Solid;
    if (val == "dashed")
        return LineStyle::Dashed;
    if (val == "dotted")
        return LineStyle::Dotted;
    if (val == "dashDot")
        return LineStyle::DashDot;
    if (val == "doubleDotDash")
        return LineStyle::DoubleDotDash;
    if (val == "none")
        return LineStyle::None;
    return defaultVal;
}

Rect JsonSceneParser::parseRect(const void *obj, const std::string &canvasUnit, const Dpi &dpi)
{
    const auto &j = *static_cast<const json *>(obj);
    auto unit = resolveUnit(j, canvasUnit);

    Rect r;
    r.x = toPixelsX(j.value("x", 0.0), unit, dpi);
    r.y = toPixelsY(j.value("y", 0.0), unit, dpi);
    r.width = toPixelsX(j.value("width", 0.0), unit, dpi);
    r.height = toPixelsY(j.value("height", 0.0), unit, dpi);
    r.strokeWidth = toPixelsX(j.value("strokeWidth", 0.0), unit, dpi);
    r.z = j.value("z", 0.0);
    r.cornerRadius = toPixelsX(j.value("cornerRadius", 0.0), unit, dpi);
    r.lineStyle = parseLineStyle(&j, "lineStyle", LineStyle::Solid);

    // Fill modes are mutually exclusive — prefer in order:
    // textureFill > gridFill > gradient > fillColor
    bool hasFill = j.contains("fillColor");
    bool hasGrad = j.contains("gradient");
    bool hasGrid = j.contains("gridFill");
    bool hasTexture = j.contains("textureFill");
    int fillCount =
        (hasFill ? 1 : 0) + (hasGrad ? 1 : 0) + (hasGrid ? 1 : 0) + (hasTexture ? 1 : 0);
    if (fillCount > 1) {
        EELog::warn("Rect has {} fill keys (fillColor/gradient/gridFill/textureFill) — "
                    "only one is used; priority: textureFill > gridFill > gradient > fillColor.",
            fillCount);
    }
    if (hasFill && !hasGrad && !hasGrid && !hasTexture)
        r.fillColor = parseColor(&j["fillColor"]);
    if (hasGrad && !hasGrid && !hasTexture)
        r.gradient = parseGradient(&j["gradient"]);
    if (hasGrid && !hasTexture)
        r.gridFill = parseGridFill(&j["gridFill"], dpi);
    if (hasTexture)
        r.textureFill = parseTextureFill(&j["textureFill"], unit, dpi);
    if (j.contains("strokeColor"))
        r.strokeColor = parseColor(&j["strokeColor"]);

    return r;
}

Circle JsonSceneParser::parseCircle(const void *obj, const std::string &canvasUnit, const Dpi &dpi)
{
    const auto &j = *static_cast<const json *>(obj);
    auto unit = resolveUnit(j, canvasUnit);

    Circle c;
    c.cx = toPixelsX(j.value("cx", 0.0), unit, dpi);
    c.cy = toPixelsY(j.value("cy", 0.0), unit, dpi);
    c.radiusX = toPixelsX(j.value("radiusX", 0.0), unit, dpi);
    // radiusY 缺省时用 Y 轴换算同一 radiusX 的 JSON 原始值——各向异性下
    // 物理尺寸保持正圆（各向同性输入与旧的 c.radiusY = c.radiusX 等价）
    if (j.contains("radiusY"))
        c.radiusY = toPixelsY(j.value("radiusY", 0.0), unit, dpi);
    else
        c.radiusY = toPixelsY(j.value("radiusX", 0.0), unit, dpi);
    c.strokeWidth = toPixelsX(j.value("strokeWidth", 0.0), unit, dpi);
    c.z = j.value("z", 0.0);
    c.lineStyle = parseLineStyle(&j, "lineStyle", LineStyle::Solid);

    // fillColor and gradient are mutually exclusive — prefer gradient, warn
    bool hasFill = j.contains("fillColor");
    bool hasGrad = j.contains("gradient");
    if (hasFill && hasGrad) {
        EELog::warn("Circle has both \"fillColor\" and \"gradient\" — gradient takes precedence, "
                    "fillColor ignored.");
    }
    if (hasFill && !hasGrad)
        c.fillColor = parseColor(&j["fillColor"]);
    if (hasGrad)
        c.gradient = parseGradient(&j["gradient"]);

    if (j.contains("strokeColor"))
        c.strokeColor = parseColor(&j["strokeColor"]);

    return c;
}

FreeLine JsonSceneParser::parseFreeLine(
    const void *obj, const std::string &canvasUnit, const Dpi &dpi)
{
    const auto &j = *static_cast<const json *>(obj);
    auto unit = resolveUnit(j, canvasUnit);

    FreeLine fl;
    fl.strokeWidth = toPixelsX(j.value("strokeWidth", 1.0), unit, dpi);
    fl.z = j.value("z", 0.0);
    fl.lineStyle = parseLineStyle(&j, "lineStyle", LineStyle::Solid);

    if (j.contains("strokeColor"))
        fl.strokeColor = parseColor(&j["strokeColor"]);

    if (j.contains("points"))
        for (auto &pt : j["points"]) {
            auto p = std::make_pair(
                toPixelsX(pt.value("x", 0.0), unit, dpi), toPixelsY(pt.value("y", 0.0), unit, dpi));
            fl.points.push_back(p);
        }

    return fl;
}

BezierCurve JsonSceneParser::parseBezierCurve(
    const void *obj, const std::string &canvasUnit, const Dpi &dpi)
{
    const auto &j = *static_cast<const json *>(obj);
    auto unit = resolveUnit(j, canvasUnit);

    BezierCurve bc;
    bc.strokeWidth = toPixelsX(j.value("strokeWidth", 1.0), unit, dpi);
    bc.z = j.value("z", 0.0);
    bc.lineStyle = parseLineStyle(&j, "lineStyle", LineStyle::Solid);

    if (j.contains("strokeColor"))
        bc.strokeColor = parseColor(&j["strokeColor"]);

    if (j.contains("controlPoints"))
        for (auto &pt : j["controlPoints"]) {
            auto p = std::make_pair(
                toPixelsX(pt.value("x", 0.0), unit, dpi), toPixelsY(pt.value("y", 0.0), unit, dpi));
            bc.controlPoints.push_back(p);
        }

    return bc;
}

Line JsonSceneParser::parseLine(const void *obj, const std::string &canvasUnit, const Dpi &dpi)
{
    const auto &j = *static_cast<const json *>(obj);
    auto unit = resolveUnit(j, canvasUnit);

    Line l;
    l.x1 = toPixelsX(j.value("x1", 0.0), unit, dpi);
    l.y1 = toPixelsY(j.value("y1", 0.0), unit, dpi);
    l.x2 = toPixelsX(j.value("x2", 0.0), unit, dpi);
    l.y2 = toPixelsY(j.value("y2", 0.0), unit, dpi);
    l.strokeWidth = toPixelsX(j.value("strokeWidth", 1.0), unit, dpi);
    l.z = j.value("z", 0.0);
    l.lineStyle = parseLineStyle(&j, "lineStyle", LineStyle::Solid);

    if (j.contains("strokeColor"))
        l.strokeColor = parseColor(&j["strokeColor"]);

    return l;
}

Text JsonSceneParser::parseText(const void *obj, const std::string &canvasUnit, const Dpi &dpi)
{
    const auto &j = *static_cast<const json *>(obj);
    auto unit = resolveUnit(j, canvasUnit);

    Text t;

    // 位置
    t.x = toPixelsX(j.value("x", 0.0), unit, dpi);
    t.y = toPixelsY(j.value("y", 0.0), unit, dpi);
    t.z = j.value("z", 0.0);

    // 文字属性 (fontSize is pt, not affected by unit)
    t.content = j.value("content", "");
    t.fontFamily = j.value("fontFamily", "Arial");
    t.fontSize = j.value("fontSize", 16.0);
    t.bold = j.value("bold", false);
    t.italic = j.value("italic", false);

    if (j.contains("textColor"))
        t.textColor = parseColor(&j["textColor"]);
    else
        t.textColor = Color{}; // 默认黑（CMYK 0,0,0,100%）

    return t;
}

ImageItem JsonSceneParser::parseImage(
    const void *obj, const std::string &canvasUnit, const Dpi &dpi)
{
    const auto &j = *static_cast<const json *>(obj);
    auto unit = resolveUnit(j, canvasUnit);

    ImageItem img;
    img.filePath = j.value("filePath", "");
    img.x = toPixelsX(j.value("x", 0.0), unit, dpi);
    img.y = toPixelsY(j.value("y", 0.0), unit, dpi);
    // width/height of 0 means "auto" — only convert when explicitly set
    double w = j.value("width", 0.0);
    double h = j.value("height", 0.0);
    img.width = (w > 0.0) ? toPixelsX(w, unit, dpi) : 0.0;
    img.height = (h > 0.0) ? toPixelsY(h, unit, dpi) : 0.0;
    img.z = j.value("z", 0.0);
    return img;
}
