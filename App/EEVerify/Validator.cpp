#include "Validator.h"
#include "nlohmann/json.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <vector>

using json = nlohmann::json;

// 校验单个 DPI 字段（dpiX / dpiY）——定义见文件尾部 Helpers 区（validateUnitField
// 附近），此处为前置声明（validateCanvas 中先于定义使用）。
static void validateDpiField(const json        &j,
                             const std::string &name,
                             const std::string &prefix,
                             ValidationReport  &r);

// ============================================================================
//  ValidationReport::format
// ============================================================================

std::string ValidationReport::format(bool /*verbose*/) const
{
    const int   TOTAL_WIDTH = 64;
    const char *DIVIDER     = "========================================"
                              "========";
    const char *THIN        = "----------------------------------------"
                              "--------";

    std::ostringstream out;
    out << DIVIDER << "\n";
    out << "  EEVerify - ExportEngine JSON Validator\n";
    out << DIVIDER << "\n\n";

    if (!filename.empty()) {
        out << "File: " << filename << "\n";
        if (totalLines > 0)
            out << "Lines: " << totalLines << "\n";
        out << "\n";
    }

    if (issues.empty()) {
        out << "No issues found. The scene file looks good!\n\n";
        out << DIVIDER << "\n";
        out << "Result: PASSED\n";
        out << DIVIDER << "\n";
        return out.str();
    }

    // Print each issue in order
    for (size_t idx = 0; idx < issues.size(); ++idx) {
        const auto &isu = issues[idx];
        if (idx > 0)
            out << "\n";
        out << severityLabel(isu.severity) << "  [" << isu.path << "]\n";
        out << "        " << isu.message << "\n";
        if (!isu.suggestion.empty())
            out << "        \xe2\x86\x92 " << isu.suggestion << "\n";
    }

    out << "\n" << THIN << "\n";
    out << "  Summary: " << errorCount() << " error(s), " << warningCount() << " warning(s), "
        << hintCount() << " hint(s)\n";

    if (hasErrors())
        out << "  Overall: FAILED (errors found)\n";
    else
        out << "  Overall: PASSED\n";
    out << DIVIDER << "\n";
    return out.str();
}

// ============================================================================
//  Validator — public entry point
// ============================================================================

bool Validator::validate(const std::string &filePath, ValidationReport &report)
{
    report          = ValidationReport{};
    report.filename = filePath;

    // -- Read file -----------------------------------------------------------
    std::ifstream ifs(filePath);
    if (!ifs.is_open()) {
        addIssue(report, Severity::Error, "(file)", "Cannot open file: " + filePath,
                 "Check that the path exists and is readable.");
        return false;
    }
    std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());

    // Count lines for the report
    report.totalLines = 0;
    for (char ch : content)
        if (ch == '\n')
            ++report.totalLines;
    if (!content.empty() && content.back() != '\n')
        ++report.totalLines;

    // -- Parse JSON ----------------------------------------------------------
    json root;
    try {
        root = json::parse(content);
    } catch (const json::parse_error &e) {
        addIssue(report, Severity::Error, "(root)", std::string("JSON syntax error: ") + e.what(),
                 "Fix the JSON syntax before re-running. Check for missing "
                 "commas, trailing commas, or unmatched braces.");
        return false;
    }

    // -- Root must be an object ----------------------------------------------
    if (!root.is_object()) {
        addIssue(report, Severity::Error, "(root)",
                 "JSON root must be an object ({...}), got " + std::string(root.type_name()),
                 "Wrap the scene description in a JSON object with a "
                 "\"canvas\" key.");
        return false;
    }

    // Check for unknown top-level keys (useful to catch typos like "canvass")
    {
        static const std::set<std::string> sTopKeys = { "canvas" };
        checkUnknownFields(root, sTopKeys, "(root)", report);
    }

    if (!root.contains("canvas")) {
        addIssue(report, Severity::Error, "(root)", "Missing required key \"canvas\"",
                 "The top-level object must contain a \"canvas\" key.");
        return false;
    }

    const auto &canvasObj = root["canvas"];
    if (!canvasObj.is_object()) {
        addIssue(report, Severity::Error, "canvas",
                 "\"canvas\" must be an object, got " + std::string(canvasObj.type_name()),
                 "canvas should be an object with width, height, etc.");
        return false;
    }

    // -- Canvas-level validation ---------------------------------------------
    validateCanvas(canvasObj, report, "canvas");

    return !report.hasErrors();
}

// ============================================================================
//  Canvas validation
// ============================================================================

void Validator::validateCanvas(const json &j, ValidationReport &r, const std::string &prefix)
{
    // Known keys on canvas object
    static const std::set<std::string> sCanvasKeys = {
        "width",   "height",    "dpi",          "dpiX",  "dpiY",  "background", "rects",
        "circles", "freeLines", "bezierCurves", "lines", "texts", "images",     "unit",
    };
    checkUnknownFields(j, sCanvasKeys, prefix, r);

    // -- width ---------------------------------------------------------------
    if (j.contains("width")) {
        const auto &w = j["width"];
        if (!w.is_number()) {
            addIssue(r, Severity::Error, prefix + ".width",
                     "\"width\" must be a number, got " + std::string(w.type_name()),
                     "Set width to an integer pixel value, e.g. 800.");
        } else {
            int v = w.get<int>();
            if (v <= 0)
                addIssue(r, Severity::Error, prefix + ".width",
                         "width must be positive, got " + std::to_string(v),
                         "Set width to a positive integer, e.g. 800.");
        }
    } else {
        addIssue(r, Severity::Hint, prefix, "\"width\" not specified, defaults to 800",
                 "Add \"width\" for explicit canvas sizing.");
    }

    // -- height --------------------------------------------------------------
    if (j.contains("height")) {
        const auto &h = j["height"];
        if (!h.is_number()) {
            addIssue(r, Severity::Error, prefix + ".height",
                     "\"height\" must be a number, got " + std::string(h.type_name()),
                     "Set height to an integer pixel value, e.g. 600.");
        } else {
            int v = h.get<int>();
            if (v <= 0)
                addIssue(r, Severity::Error, prefix + ".height",
                         "height must be positive, got " + std::to_string(v),
                         "Set height to a positive integer, e.g. 600.");
        }
    } else {
        addIssue(r, Severity::Hint, prefix, "\"height\" not specified, defaults to 600",
                 "Add \"height\" for explicit canvas sizing.");
    }

    // -- dpiX / dpiY（旧 dpi 已废弃） --------------------------------------
    if (j.contains("dpi")) {
        addIssue(r, Severity::Error, prefix + ".dpi",
                 "\"dpi\" is deprecated, use \"dpiX\" and \"dpiY\"",
                 "Rename \"dpi\" to \"dpiX\" and \"dpiY\" (each defaults to 300).");
    }
    validateDpiField(j, "dpiX", prefix, r);
    validateDpiField(j, "dpiY", prefix, r);

    // -- unit -----------------------------------------------------------------
    validateUnitField(j, prefix, r);

    // -- background ----------------------------------------------------------
    if (j.contains("background")) {
        const auto &bg = j["background"];
        validateColor(bg, prefix + ".background", r);
    } else {
        addIssue(r, Severity::Hint, prefix,
                 "No \"background\" specified — canvas will be transparent black",
                 "Add a background color for opaque output, "
                 "e.g. "
                 "{\"c\":0,\"m\":0,\"y\":0,\"k\":0,\"a\":1.0}.");
    }

    // -- Primitive arrays ----------------------------------------------------
    auto validateArray = [&](const std::string &key,
                             void (Validator::*fn)(const json &, ValidationReport &)) {
        if (!j.contains(key))
            return;
        const auto &arr = j[key];
        if (!arr.is_array()) {
            addIssue(r, Severity::Error, prefix + "." + key,
                     "\"" + key + "\" must be an array, got " + std::string(arr.type_name()),
                     "Use [] even for a single element, e.g. \"" + key + "\": [{ ... }].");
            return;
        }
        (this->*fn)(arr, r);
    };

    validateArray("rects", &Validator::validateRects);
    validateArray("circles", &Validator::validateCircles);
    validateArray("freeLines", &Validator::validateFreeLines);
    validateArray("bezierCurves", &Validator::validateBezierCurves);
    validateArray("lines", &Validator::validateLines);
    validateArray("texts", &Validator::validateTexts);
    validateArray("images", &Validator::validateImages);
}

// ============================================================================
//  Color validation
// ============================================================================

void Validator::validateColor(const json &j, const std::string &path, ValidationReport &r)
{
    if (!j.is_object()) {
        addIssue(r, Severity::Error, path,
                 "Color must be an object, got " + std::string(j.type_name()),
                 "Use {\"c\":0,\"m\":0,\"y\":0,\"k\":0,\"a\":1.0}.");
        return;
    }

    // Known fields for color
    static const std::set<std::string> sColorKeys = {
        "space", "c", "m", "y", "k", "r", "g", "b", "a",
    };
    checkUnknownFields(j, sColorKeys, path, r);

    // CMYK-only：space 字段已移除
    if (j.contains("space")) {
        addIssue(r, Severity::Error, path + ".space",
                 "Color field \"space\" is removed — colors are CMYK-only now.",
                 "Remove \"space\" and write c/m/y/k directly, e.g. "
                 "{\"c\":0,\"m\":100,\"y\":100,\"k\":0,\"a\":1.0}.");
    }
    // RGB 颜色已移除
    for (const char *key : { "r", "g", "b" }) {
        if (j.contains(key)) {
            addIssue(r, Severity::Error, path + "." + key,
                     "RGB colors are removed — use CMYK (c/m/y/k).",
                     "Convert to CMYK, e.g. {\"c\":0,\"m\":100,\"y\":100,\"k\":0,\"a\":1.0}.");
        }
    }

    // CMYK components are 0-100 in JSON
    auto checkCMYK = [&](const std::string &key, const std::string &label) {
        if (!j.contains(key))
            return;
        const auto &v = j[key];
        if (!v.is_number()) {
            addIssue(r, Severity::Error, path + "." + key,
                     "CMYK." + key + " must be a number, got " + std::string(v.type_name()),
                     "Set " + key + " to a percentage value between 0 and 100.");
        } else {
            double d = v.get<double>();
            if (d < 0.0 || d > 100.0)
                addIssue(r, Severity::Warning, path + "." + key,
                         "CMYK." + key + " = " + std::to_string(d)
                             + " is outside the normal range [0, 100]",
                         "Clamp " + key + " to 0-100 for physically meaningful color.");
        }
    };
    checkCMYK("c", "cyan");
    checkCMYK("m", "magenta");
    checkCMYK("y", "yellow");
    checkCMYK("k", "black");

    // 缺少全部 c/m/y/k 时渲染为纸白（全 0）——仅提示，不报错
    if (!j.contains("c") && !j.contains("m") && !j.contains("y") && !j.contains("k")) {
        addIssue(r, Severity::Hint, path,
                 "Color object has none of c/m/y/k — it renders as paper white (all 0)",
                 "Write the c/m/y/k components explicitly, e.g. "
                 "{\"c\":0,\"m\":0,\"y\":0,\"k\":0,\"a\":1.0}.");
    }

    // Alpha (CMYK colors)
    if (j.contains("a")) {
        const auto &a = j["a"];
        if (!a.is_number()) {
            addIssue(r, Severity::Error, path + ".a",
                     "Alpha must be a number, got " + std::string(a.type_name()),
                     "Set alpha to a value between 0.0 (transparent) and 1.0 "
                     "(opaque).");
        } else {
            double d = a.get<double>();
            if (d < 0.0 || d > 1.0)
                addIssue(r, Severity::Error, path + ".a",
                         "Alpha = " + std::to_string(d) + " is outside the valid range [0.0, 1.0]",
                         "Use 0.0 for fully transparent, 1.0 for fully opaque.");
            if (d == 0.0)
                addIssue(r, Severity::Hint, path + ".a",
                         "Alpha is 0.0 — this color/object will be completely "
                         "invisible",
                         "If invisibility is intentional, no change needed.");
        }
    }

    // Gradient must not be nested inside a color object (old format).
    // It is now a peer key alongside "fillColor" on Rect/Circle primitives.
    if (j.contains("gradient")) {
        addIssue(r, Severity::Error, path + ".gradient",
                 "Gradient must not be nested inside a color object. "
                 "Use \"gradient\" as a peer key alongside \"fillColor\" on the primitive.",
                 "Move the \"gradient\" object out of the color object to be "
                 "a sibling of \"fillColor\" (or replace \"fillColor\" with \"gradient\").");
    }
}

// ============================================================================
//  Gradient validation
// ============================================================================

void Validator::validateGradient(const json &j, const std::string &path, ValidationReport &r)
{
    static const std::set<std::string> sGradientKeys = {
        "type", "stops", "x1", "y1", "x2", "y2", "cx", "cy", "r", "startAngle",
    };
    checkUnknownFields(j, sGradientKeys, path, r);

    // -- type ----------------------------------------------------------------
    static const std::set<std::string> sValidTypes = {
        "linear",
        "radial",
        "conic",
    };
    std::string type;
    if (j.contains("type")) {
        if (!j["type"].is_string()) {
            addIssue(r, Severity::Error, path + ".type",
                     "\"type\" must be a string, got " + std::string(j["type"].type_name()),
                     "Set type to \"linear\", \"radial\", or \"conic\".");
            return; // can't proceed without valid type
        }
        type = j["type"].get<std::string>();
        if (sValidTypes.find(type) == sValidTypes.end()) {
            addIssue(r, Severity::Error, path + ".type",
                     "Unknown gradient type \"" + type
                         + "\". Expected "
                           "\"linear\", \"radial\", or \"conic\".",
                     "Use one of: \"linear\", \"radial\", \"conic\".");
            return;
        }
    } else {
        addIssue(r, Severity::Error, path, "Gradient missing required field \"type\"",
                 "Add \"type\": \"linear\" (or \"radial\"/\"conic\").");
        return;
    }

    // -- type-specific parameters --------------------------------------------
    if (type == "linear") {
        auto checkLinearParam = [&](const std::string &key) {
            if (j.contains(key) && !j[key].is_number())
                addIssue(r, Severity::Warning, path + "." + key,
                         "Linear gradient \"" + key + "\" should be a number, got "
                             + std::string(j[key].type_name()),
                         "Use a number (typically in 0.0–1.0 normalized range).");
        };
        checkLinearParam("x1");
        checkLinearParam("y1");
        checkLinearParam("x2");
        checkLinearParam("y2");
    }
    if (type == "radial") {
        auto checkRadialParam = [&](const std::string &key) {
            if (j.contains(key) && !j[key].is_number())
                addIssue(r, Severity::Warning, path + "." + key,
                         "Radial gradient \"" + key + "\" should be a number, got "
                             + std::string(j[key].type_name()),
                         "Use a number (typically in 0.0–1.0 normalized range).");
        };
        checkRadialParam("cx");
        checkRadialParam("cy");
        checkRadialParam("r");
        if (j.contains("r")) {
            double rv = j.value("r", 0.5);
            if (rv <= 0.0)
                addIssue(r, Severity::Warning, path + ".r",
                         "Radial gradient radius must be positive, got " + std::to_string(rv),
                         "Set r to a positive value, e.g. 0.5.");
        }
    }
    if (type == "conic") {
        auto checkConicParam = [&](const std::string &key) {
            if (j.contains(key) && !j[key].is_number())
                addIssue(r, Severity::Warning, path + "." + key,
                         "Conic gradient \"" + key + "\" should be a number, got "
                             + std::string(j[key].type_name()),
                         "Use a number.");
        };
        checkConicParam("cx");
        checkConicParam("cy");
        checkConicParam("startAngle");
        if (j.contains("startAngle")) {
            double sa = j.value("startAngle", 0.0);
            if (sa < 0.0 || sa > 360.0)
                addIssue(r, Severity::Hint, path + ".startAngle",
                         "startAngle = " + std::to_string(sa)
                             + " is outside the typical range [0, 360]",
                         "Angles outside [0, 360] are wrapped, but "
                             + std::to_string(std::fmod(sa, 360.0)) + " is equivalent.");
        }
    }

    // -- stops ---------------------------------------------------------------
    if (!j.contains("stops")) {
        addIssue(r, Severity::Error, path, "Gradient missing required field \"stops\"",
                 "Add at least 2 color stops, e.g. \"stops\": [{...}, {...}].");
        return;
    }
    const auto &stops = j["stops"];
    if (!stops.is_array()) {
        addIssue(r, Severity::Error, path + ".stops",
                 "\"stops\" must be an array, got " + std::string(stops.type_name()),
                 "Define stops as an array of stop objects with \"offset\" and "
                 "\"color\".");
        return;
    }
    if (stops.size() < 2) {
        addIssue(r, Severity::Error, path + ".stops",
                 "Gradient needs at least 2 stops, got " + std::to_string(stops.size()),
                 "Add at least 2 stops to define the gradient range.");
        return;
    }

    // Validate each stop
    double prevOffset = -1.0;
    bool   orderOk    = true;
    for (size_t i = 0; i < stops.size(); ++i) {
        const auto &stop     = stops[i];
        std::string stopPath = path + ".stops[" + std::to_string(i) + "]";

        if (!stop.is_object()) {
            addIssue(r, Severity::Error, stopPath,
                     "Stop must be an object, got " + std::string(stop.type_name()),
                     "Use {\"offset\": 0.0, \"color\": {...}}.");
            continue;
        }

        static const std::set<std::string> sStopKeys = { "offset", "color" };
        checkUnknownFields(stop, sStopKeys, stopPath, r);

        // offset
        if (!stop.contains("offset")) {
            addIssue(r, Severity::Error, stopPath, "Stop missing required field \"offset\"",
                     "Add an offset value between 0.0 and 1.0.");
        } else {
            const auto &off = stop["offset"];
            if (!off.is_number()) {
                addIssue(r, Severity::Error, stopPath + ".offset",
                         "\"offset\" must be a number, got " + std::string(off.type_name()),
                         "Set offset to a value between 0.0 and 1.0.");
            } else {
                double d = off.get<double>();
                if (d < 0.0 || d > 1.0) {
                    addIssue(r, Severity::Error, stopPath + ".offset",
                             "offset = " + std::to_string(d) + " is out of range [0.0, 1.0]",
                             "Clamp offset to the [0.0, 1.0] range.");
                }
                // Check ascending order
                if (i > 0 && d < prevOffset) {
                    orderOk = false;
                }
                prevOffset = d;
            }
        }

        // color
        if (!stop.contains("color")) {
            addIssue(r, Severity::Error, stopPath, "Stop missing required field \"color\"",
                     "Each stop needs a \"color\" object.");
        } else {
            validateColor(stop["color"], stopPath + ".color", r);
        }
    }

    if (!orderOk) {
        addIssue(r, Severity::Warning, path + ".stops",
                 "Gradient stops are not sorted by offset in ascending order",
                 "Reorder stops so offset values increase from 0.0 to 1.0 for "
                 "correct gradient rendering.");
    }
}

// ============================================================================
//  Fill validation (fillColor / gradient / gridFill / textureFill
//  mutual exclusion)
// ============================================================================

void Validator::validateFill(const json &prim, const std::string &path, ValidationReport &r)
{
    static const char       *kFillKeys[] = { "fillColor", "gradient", "gridFill", "textureFill" };
    std::vector<std::string> present;
    for (const char *k : kFillKeys)
        if (prim.contains(k))
            present.push_back(k);

    if (present.size() > 1) {
        std::string joined;
        for (size_t i = 0; i < present.size(); ++i) {
            if (i)
                joined += ", ";
            joined += "\"" + present[i] + "\"";
        }
        addIssue(
            r, Severity::Error, path,
            "Fill keys are mutually exclusive — " + joined + " present; use only one.",
            "Remove all but one of: \"fillColor\", \"gradient\", \"gridFill\", \"textureFill\".");
    }

    if (prim.contains("fillColor")) {
        const auto &fc = prim["fillColor"];
        validateColor(fc, path + ".fillColor", r);
    }
    if (prim.contains("gradient")) {
        const auto &g = prim["gradient"];
        if (g.is_object())
            validateGradient(g, path + ".gradient", r);
        else
            addIssue(r, Severity::Error, path + ".gradient",
                     "\"gradient\" must be an object, got " + std::string(g.type_name()),
                     "Define a gradient object with \"type\" and \"stops\".");
    }
    if (prim.contains("gridFill")) {
        const auto &g = prim["gridFill"];
        if (g.is_object())
            validateGridFill(g, path + ".gridFill", r);
        else
            addIssue(r, Severity::Error, path + ".gridFill",
                     "\"gridFill\" must be an object, got " + std::string(g.type_name()),
                     "Define a gridFill object with \"gridColor\" and \"backgroundColor\".");
    }
    if (prim.contains("textureFill")) {
        const auto &t = prim["textureFill"];
        if (t.is_object())
            validateTextureFill(t, path + ".textureFill", r);
        else
            addIssue(r, Severity::Error, path + ".textureFill",
                     "\"textureFill\" must be an object, got " + std::string(t.type_name()),
                     "Define a textureFill object with \"filePath\" and size fields.");
    }
}

void Validator::validateGridFill(const json &j, const std::string &path, ValidationReport &r)
{
    static const std::set<std::string> sGridKeys = {
        "gridColor", "backgroundColor", "transparentBackground",
        "cellWidth", "cellHeight",      "lineWidth",
    };
    checkUnknownFields(j, sGridKeys, path, r);

    if (j.contains("gridColor"))
        validateColor(j["gridColor"], path + ".gridColor", r);
    if (j.contains("backgroundColor"))
        validateColor(j["backgroundColor"], path + ".backgroundColor", r);

    if (j.contains("transparentBackground") && !j["transparentBackground"].is_boolean())
        addIssue(r, Severity::Error, path + ".transparentBackground",
                 "must be a boolean, got " + std::string(j["transparentBackground"].type_name()),
                 "Set true or false.");

    // cellWidth/cellHeight/lineWidth 已废弃：网格固定 1mm、线宽固定 1px，
    // 均按画布 dpi 计算，JSON 中的旧配置不再生效。
    for (const char *k : { "cellWidth", "cellHeight", "lineWidth" }) {
        if (!j.contains(k))
            continue;
        addIssue(r, Severity::Warning, path,
                 "\"cellWidth\"/\"cellHeight\"/\"lineWidth\" are deprecated and ignored — "
                 "the grid cell is fixed at 1mm and the line width at 1px, "
                 "computed from the canvas dpi",
                 "Remove these fields; the grid size is derived from the canvas dpi.");
        break;
    }
}

void Validator::validateTextureFill(const json &j, const std::string &path, ValidationReport &r)
{
    static const std::set<std::string> sTextureKeys = {
        "filePath", "offsetX", "offsetY", "useOriginalSize", "customWidth", "customHeight",
    };
    checkUnknownFields(j, sTextureKeys, path, r);

    if (!j.contains("filePath") || !j["filePath"].is_string()
        || j["filePath"].get<std::string>().empty()) {
        addIssue(r, Severity::Error, path, "missing or empty \"filePath\"",
                 "Point to an image file (PNG/TIFF/JPEG...).");
    }

    for (const char *k : { "offsetX", "offsetY" })
        if (j.contains(k) && !j[k].is_number())
            addIssue(r, Severity::Error, path + "." + k,
                     "must be a number, got " + std::string(j[k].type_name()),
                     "Set a number, e.g. 0.");

    if (j.contains("useOriginalSize") && !j["useOriginalSize"].is_boolean())
        addIssue(r, Severity::Error, path + ".useOriginalSize",
                 "must be a boolean, got " + std::string(j["useOriginalSize"].type_name()),
                 "Set true or false.");

    bool useOriginal = false;
    if (j.contains("useOriginalSize") && j["useOriginalSize"].is_boolean())
        useOriginal = j["useOriginalSize"].get<bool>();
    if (!useOriginal) {
        for (const char *k : { "customWidth", "customHeight" }) {
            if (!j.contains(k) || !j[k].is_number()) {
                addIssue(r, Severity::Error, path + "." + k, "required when useOriginalSize=false",
                         "Set a positive number, e.g. 100.");
            } else if (j[k].get<double>() <= 0.0) {
                addIssue(r, Severity::Error, path + "." + k,
                         "must be positive when useOriginalSize=false",
                         "Set a positive number, e.g. 100.");
            }
        }
    } else if (j.contains("customWidth") || j.contains("customHeight")) {
        addIssue(r, Severity::Warning, path,
                 "\"customWidth\"/\"customHeight\" are ignored when useOriginalSize=true",
                 "Remove them, or set useOriginalSize to false.");
    }
}

// ============================================================================
//  Line-style validation
// ============================================================================

void Validator::validateLineStyleValue(const json        &j,
                                       const std::string &path,
                                       const std::string &key,
                                       ValidationReport  &r)
{
    static const std::set<std::string> sValid = {
        "solid", "dashed", "dotted", "dashDot", "doubleDotDash", "none",
    };
    if (!j.contains(key))
        return;
    const auto &v = j[key];
    if (!v.is_string()) {
        addIssue(r, Severity::Error, path + "." + key,
                 "\"" + key + "\" must be a string, got " + std::string(v.type_name()),
                 "Use one of: solid, dashed, dotted, dashDot, doubleDotDash, none.");
        return;
    }
    std::string s = v.get<std::string>();
    if (sValid.find(s) == sValid.end()) {
        addIssue(r, Severity::Error, path + "." + key, "Unknown line style \"" + s + "\"",
                 "Valid styles: solid, dashed, dotted, dashDot, doubleDotDash, "
                 "none.");
    }
}

// ============================================================================
//  Rect validation
// ============================================================================

void Validator::validateRects(const json &arr, ValidationReport &r)
{
    for (size_t i = 0; i < arr.size(); ++i) {
        const auto &item = arr[i];
        std::string p    = "canvas.rects[" + std::to_string(i) + "]";

        if (!item.is_object()) {
            addIssue(r, Severity::Error, p,
                     "Rect must be an object, got " + std::string(item.type_name()),
                     "Define a rect with x, y, width, height, etc.");
            continue;
        }

        // "comment" is a documentation-only field, ignored by the renderer.
        static const std::set<std::string> sKeys = {
            "x",         "y",        "width",       "height",      "z",           "fillColor",
            "gradient",  "gridFill", "textureFill", "strokeColor", "strokeWidth", "cornerRadius",
            "lineStyle", "comment",  "unit",
        };
        checkUnknownFields(item, sKeys, p, r);
        validateUnitField(item, p, r);

        // width / height
        auto checkPositive = [&](const std::string &key) {
            if (!item.contains(key)) {
                addIssue(r, Severity::Warning, p,
                         "Rect missing \"" + key + "\" — will default to 0 (may be invisible)",
                         "Set " + key + " to a positive value.");
                return;
            }
            const auto &v = item[key];
            if (!v.is_number()) {
                addIssue(r, Severity::Error, p + "." + key,
                         "Rect \"" + key + "\" must be a number, got " + std::string(v.type_name()),
                         "Set " + key + " to a positive number, e.g. 100.");
            } else if (v.get<double>() <= 0.0) {
                addIssue(r, Severity::Warning, p + "." + key,
                         "Rect \"" + key
                             + "\" is zero or negative — rect will "
                               "have no area",
                         "Set " + key + " to a positive value.");
            }
        };
        checkPositive("width");
        checkPositive("height");

        // cornerRadius
        if (item.contains("cornerRadius")) {
            const auto &cr = item["cornerRadius"];
            if (cr.is_number() && cr.get<double>() < 0.0)
                addIssue(r, Severity::Warning, p + ".cornerRadius",
                         "cornerRadius is negative: " + std::to_string(cr.get<double>()),
                         "Set cornerRadius to 0 for square corners or a "
                         "positive value for rounded corners.");
            // Check that cornerRadius <= min(width,height)/2
            if (item.contains("width") && item.contains("height") && item["width"].is_number()
                && item["height"].is_number() && cr.is_number()) {
                double maxR =
                    std::min(item["width"].get<double>(), item["height"].get<double>()) / 2.0;
                if (cr.get<double>() > maxR && cr.get<double>() > 0.0)
                    addIssue(r, Severity::Hint, p + ".cornerRadius",
                             "cornerRadius " + std::to_string(cr.get<double>())
                                 + " exceeds half the smaller dimension (" + std::to_string(maxR)
                                 + ") — will be clamped internally",
                             "Reduce cornerRadius to " + std::to_string(maxR) + " or less.");
            }
        }

        // Common primitives checks
        if (item.contains("strokeWidth")) {
            const auto &sw = item["strokeWidth"];
            if (sw.is_number() && sw.get<double>() < 0.0)
                addIssue(r, Severity::Warning, p + ".strokeWidth",
                         "strokeWidth is negative: " + std::to_string(sw.get<double>()),
                         "Set strokeWidth to 0 (no stroke) or a positive width.");
        }

        // Colors & lineStyle
        validateFill(item, p, r);
        if (item.contains("strokeColor"))
            validateColor(item["strokeColor"], p + ".strokeColor", r);
        validateLineStyleValue(item, p, "lineStyle", r);
    }
}

// ============================================================================
//  Circle validation
// ============================================================================

void Validator::validateCircles(const json &arr, ValidationReport &r)
{
    for (size_t i = 0; i < arr.size(); ++i) {
        const auto &item = arr[i];
        std::string p    = "canvas.circles[" + std::to_string(i) + "]";

        if (!item.is_object()) {
            addIssue(r, Severity::Error, p,
                     "Circle must be an object, got " + std::string(item.type_name()),
                     "Define a circle with cx, cy, radiusX, etc.");
            continue;
        }

        static const std::set<std::string> sKeys = {
            "cx",       "cy",          "radiusX",     "radiusY",   "z",       "fillColor",
            "gradient", "strokeColor", "strokeWidth", "lineStyle", "comment", "unit",
        };
        checkUnknownFields(item, sKeys, p, r);
        validateUnitField(item, p, r);

        // radiusX
        if (!item.contains("radiusX")) {
            addIssue(r, Severity::Error, p, "Circle missing required field \"radiusX\"",
                     "Set radiusX to a positive value.");
        } else {
            const auto &rx = item["radiusX"];
            if (!rx.is_number())
                addIssue(r, Severity::Error, p + ".radiusX",
                         "\"radiusX\" must be a number, got " + std::string(rx.type_name()),
                         "Set radiusX to a positive number.");
            else if (rx.get<double>() <= 0.0)
                addIssue(r, Severity::Error, p + ".radiusX",
                         "radiusX must be positive, got " + std::to_string(rx.get<double>()),
                         "Set radiusX to a positive value.");
        }

        // radiusY (optional, defaults to radiusX)
        if (item.contains("radiusY")) {
            const auto &ry = item["radiusY"];
            if (!ry.is_number())
                addIssue(r, Severity::Warning, p + ".radiusY",
                         "\"radiusY\" must be a number, got " + std::string(ry.type_name()),
                         "Set radiusY to a positive number, or omit for a circle.");
            else if (ry.get<double>() <= 0.0)
                addIssue(r, Severity::Warning, p + ".radiusY",
                         "radiusY must be positive, got " + std::to_string(ry.get<double>()),
                         "Set radiusY to a positive value.");
        }

        // strokeWidth
        if (item.contains("strokeWidth")) {
            const auto &sw = item["strokeWidth"];
            if (sw.is_number() && sw.get<double>() < 0.0)
                addIssue(r, Severity::Warning, p + ".strokeWidth",
                         "strokeWidth is negative: " + std::to_string(sw.get<double>()),
                         "Set strokeWidth to 0 (no stroke) or a positive width.");
        }

        // Colors & lineStyle
        validateFill(item, p, r);
        if (item.contains("strokeColor"))
            validateColor(item["strokeColor"], p + ".strokeColor", r);
        validateLineStyleValue(item, p, "lineStyle", r);
    }
}

// ============================================================================
//  FreeLine validation
// ============================================================================

void Validator::validateFreeLines(const json &arr, ValidationReport &r)
{
    for (size_t i = 0; i < arr.size(); ++i) {
        const auto &item = arr[i];
        std::string p    = "canvas.freeLines[" + std::to_string(i) + "]";

        if (!item.is_object()) {
            addIssue(r, Severity::Error, p,
                     "FreeLine must be an object, got " + std::string(item.type_name()),
                     "Define a freeLine with \"points\" array.");
            continue;
        }

        static const std::set<std::string> sKeys = {
            "points", "z", "strokeColor", "strokeWidth", "lineStyle", "comment", "unit",
        };
        checkUnknownFields(item, sKeys, p, r);
        validateUnitField(item, p, r);

        // points
        if (!item.contains("points")) {
            addIssue(r, Severity::Error, p, "FreeLine missing required field \"points\"",
                     "Add a points array with at least 2 coordinate pairs.");
        } else {
            const auto &pts = item["points"];
            if (!pts.is_array()) {
                addIssue(r, Severity::Error, p + ".points",
                         "\"points\" must be an array, got " + std::string(pts.type_name()),
                         "Use an array of {\"x\": ..., \"y\": ...} objects.");
            } else if (pts.size() < 2) {
                addIssue(r, Severity::Error, p + ".points",
                         "FreeLine needs at least 2 points, got " + std::to_string(pts.size()),
                         "Add at least 2 points to define a visible line.");
            } else {
                for (size_t j = 0; j < pts.size(); ++j) {
                    std::string pp = p + ".points[" + std::to_string(j) + "]";
                    const auto &pt = pts[j];
                    if (!pt.is_object()) {
                        addIssue(r, Severity::Error, pp,
                                 "Point must be an object with \"x\" and \"y\", got "
                                     + std::string(pt.type_name()),
                                 "Use {\"x\": number, \"y\": number}.");
                        continue;
                    }
                    static const std::set<std::string> sPointKeys = { "x", "y" };
                    checkUnknownFields(pt, sPointKeys, pp, r);

                    if (!pt.contains("x"))
                        addIssue(r, Severity::Error, pp, "Point missing required field \"x\"",
                                 "Add an x coordinate.");
                    else if (!pt["x"].is_number())
                        addIssue(r, Severity::Error, pp + ".x",
                                 "\"x\" must be a number, got " + std::string(pt["x"].type_name()),
                                 "Set x to a coordinate value.");

                    if (!pt.contains("y"))
                        addIssue(r, Severity::Error, pp, "Point missing required field \"y\"",
                                 "Add a y coordinate.");
                    else if (!pt["y"].is_number())
                        addIssue(r, Severity::Error, pp + ".y",
                                 "\"y\" must be a number, got " + std::string(pt["y"].type_name()),
                                 "Set y to a coordinate value.");
                }
            }
        }

        // strokeWidth
        if (item.contains("strokeWidth")) {
            const auto &sw = item["strokeWidth"];
            if (sw.is_number() && sw.get<double>() < 0.0)
                addIssue(r, Severity::Warning, p + ".strokeWidth",
                         "strokeWidth is negative: " + std::to_string(sw.get<double>()),
                         "Set strokeWidth to a positive width (default is 1).");
        }

        // strokeColor & lineStyle
        if (item.contains("strokeColor"))
            validateColor(item["strokeColor"], p + ".strokeColor", r);
        validateLineStyleValue(item, p, "lineStyle", r);
    }
}

// ============================================================================
//  BezierCurve validation
// ============================================================================

void Validator::validateBezierCurves(const json &arr, ValidationReport &r)
{
    for (size_t i = 0; i < arr.size(); ++i) {
        const auto &item = arr[i];
        std::string p    = "canvas.bezierCurves[" + std::to_string(i) + "]";

        if (!item.is_object()) {
            addIssue(r, Severity::Error, p,
                     "BezierCurve must be an object, got " + std::string(item.type_name()),
                     "Define a bezierCurve with \"controlPoints\" array.");
            continue;
        }

        static const std::set<std::string> sKeys = {
            "controlPoints", "z", "strokeColor", "strokeWidth", "lineStyle", "comment", "unit",
        };
        checkUnknownFields(item, sKeys, p, r);
        validateUnitField(item, p, r);

        // controlPoints
        if (!item.contains("controlPoints")) {
            addIssue(r, Severity::Error, p, "BezierCurve missing required field \"controlPoints\"",
                     "Add controlPoints: 2 pts=line, 3=quadratic, "
                     "4=cubic, 7=two cubics, etc.");
        } else {
            const auto &cpts = item["controlPoints"];
            if (!cpts.is_array()) {
                addIssue(r, Severity::Error, p + ".controlPoints",
                         "\"controlPoints\" must be an array, got " + std::string(cpts.type_name()),
                         "Use an array of {\"x\": ..., \"y\": ...} objects.");
            } else {
                int n = (int)cpts.size();
                if (n < 2) {
                    addIssue(r, Severity::Error, p + ".controlPoints",
                             "BezierCurve needs at least 2 control points, got "
                                 + std::to_string(n),
                             "Use at least 2 points.");
                } else if (n > 2 && (n - 1) % 3 != 0) {
                    // The engine chains cubic segments: 4, 7, 10, 13, ...
                    // 2 (line) and 3 (quadratic) are also valid.
                    if (n != 3)
                        addIssue(r, Severity::Hint, p + ".controlPoints",
                                 std::to_string(n)
                                     + " control points: not a "
                                       "standard count (2=line, 3=quadratic, "
                                       "4=cubic, 7=two cubics, ...)",
                                 "Consider using 2, 3, or 3k+1 points.");
                }

                for (size_t j = 0; j < cpts.size(); ++j) {
                    std::string pp = p + ".controlPoints[" + std::to_string(j) + "]";
                    const auto &pt = cpts[j];
                    if (!pt.is_object()) {
                        addIssue(r, Severity::Error, pp,
                                 "Control point must be an object with \"x\" "
                                 "and \"y\", got "
                                     + std::string(pt.type_name()),
                                 "Use {\"x\": number, \"y\": number}.");
                        continue;
                    }
                    static const std::set<std::string> sPtKeys = { "x", "y" };
                    checkUnknownFields(pt, sPtKeys, pp, r);

                    if (!pt.contains("x"))
                        addIssue(r, Severity::Error, pp,
                                 "Control point missing required field \"x\"",
                                 "Add an x coordinate.");
                    else if (!pt["x"].is_number())
                        addIssue(r, Severity::Error, pp + ".x",
                                 "\"x\" must be a number, got " + std::string(pt["x"].type_name()),
                                 "Set x to a coordinate value.");

                    if (!pt.contains("y"))
                        addIssue(r, Severity::Error, pp,
                                 "Control point missing required field \"y\"",
                                 "Add a y coordinate.");
                    else if (!pt["y"].is_number())
                        addIssue(r, Severity::Error, pp + ".y",
                                 "\"y\" must be a number, got " + std::string(pt["y"].type_name()),
                                 "Set y to a coordinate value.");
                }
            }
        }

        if (item.contains("strokeWidth")) {
            const auto &sw = item["strokeWidth"];
            if (sw.is_number() && sw.get<double>() < 0.0)
                addIssue(r, Severity::Warning, p + ".strokeWidth",
                         "strokeWidth is negative: " + std::to_string(sw.get<double>()),
                         "Set strokeWidth to a positive width (default is 1).");
        }

        if (item.contains("strokeColor"))
            validateColor(item["strokeColor"], p + ".strokeColor", r);
        validateLineStyleValue(item, p, "lineStyle", r);
    }
}

// ============================================================================
//  Line validation
// ============================================================================

void Validator::validateLines(const json &arr, ValidationReport &r)
{
    for (size_t i = 0; i < arr.size(); ++i) {
        const auto &item = arr[i];
        std::string p    = "canvas.lines[" + std::to_string(i) + "]";

        if (!item.is_object()) {
            addIssue(r, Severity::Error, p,
                     "Line must be an object, got " + std::string(item.type_name()),
                     "Define a line with x1, y1, x2, y2.");
            continue;
        }

        static const std::set<std::string> sKeys = {
            "x1",          "y1",          "x2",        "y2",      "z",
            "strokeColor", "strokeWidth", "lineStyle", "comment", "unit",
        };
        checkUnknownFields(item, sKeys, p, r);
        validateUnitField(item, p, r);

        auto checkCoord = [&](const std::string &key) {
            if (item.contains(key) && !item[key].is_number())
                addIssue(r, Severity::Error, p + "." + key,
                         "\"" + key + "\" must be a number, got "
                             + std::string(item[key].type_name()),
                         "Set " + key + " to a coordinate value.");
        };
        checkCoord("x1");
        checkCoord("y1");
        checkCoord("x2");
        checkCoord("y2");

        // Degenerate line check (start == end)
        if (item.contains("x1") && item.contains("y1") && item.contains("x2") && item.contains("y2")
            && item["x1"].is_number() && item["y1"].is_number() && item["x2"].is_number()
            && item["y2"].is_number()) {
            double dx = item["x2"].get<double>() - item["x1"].get<double>();
            double dy = item["y2"].get<double>() - item["y1"].get<double>();
            if (dx == 0.0 && dy == 0.0)
                addIssue(r, Severity::Warning, p,
                         "Line start and end points are identical ("
                             + std::to_string(item["x1"].get<double>()) + ","
                             + std::to_string(item["y1"].get<double>())
                             + ") — line degenerates to a point",
                         "Set different x2/y2 coordinates to draw a visible line.");
        }

        if (item.contains("strokeWidth")) {
            const auto &sw = item["strokeWidth"];
            if (sw.is_number() && sw.get<double>() < 0.0)
                addIssue(r, Severity::Warning, p + ".strokeWidth",
                         "strokeWidth is negative: " + std::to_string(sw.get<double>()),
                         "Set strokeWidth to a positive width.");
        }

        if (item.contains("strokeColor"))
            validateColor(item["strokeColor"], p + ".strokeColor", r);
        validateLineStyleValue(item, p, "lineStyle", r);
    }
}

// ============================================================================
//  Text validation
// ============================================================================

void Validator::validateTexts(const json &arr, ValidationReport &r)
{
    for (size_t i = 0; i < arr.size(); ++i) {
        const auto &item = arr[i];
        std::string p    = "canvas.texts[" + std::to_string(i) + "]";

        if (!item.is_object()) {
            addIssue(r, Severity::Error, p,
                     "Text must be an object, got " + std::string(item.type_name()),
                     "Define a text object with \"content\" and \"fontFamily\".");
            continue;
        }

        // comment — user documentation (ignored by renderer)
        static const std::set<std::string> sKeys = {
            "x",      "y",         "z",     "content", "fontFamily", "fontSize", "bold",
            "italic", "textColor", "width", "height",  "underline",  "comment",  "unit",
        };
        checkUnknownFields(item, sKeys, p, r);
        validateUnitField(item, p, r);

        // content
        if (!item.contains("content")) {
            addIssue(r, Severity::Error, p, "Text missing required field \"content\"",
                     "Add the text string to render, e.g. \"Hello World\".");
        } else {
            const auto &c = item["content"];
            if (!c.is_string())
                addIssue(r, Severity::Error, p + ".content",
                         "\"content\" must be a string, got " + std::string(c.type_name()),
                         "Provide the text as a string value.");
            else if (c.get<std::string>().empty())
                addIssue(r, Severity::Warning, p + ".content",
                         "content is an empty string — no text will be rendered",
                         "Add text content or remove this text entry.");
        }

        // fontFamily
        if (item.contains("fontFamily")) {
            const auto &ff = item["fontFamily"];
            if (!ff.is_string())
                addIssue(r, Severity::Warning, p + ".fontFamily",
                         "\"fontFamily\" must be a string, got " + std::string(ff.type_name()),
                         "Use a font family name, e.g. \"SimSun\", \"Arial\".");
            else if (ff.get<std::string>().empty())
                addIssue(r, Severity::Warning, p + ".fontFamily",
                         "fontFamily is empty — will fall back to \"Arial\"",
                         "Specify a font family name for reliable rendering.");
        }

        // fontSize
        if (item.contains("fontSize")) {
            const auto &fs = item["fontSize"];
            if (!fs.is_number())
                addIssue(r, Severity::Error, p + ".fontSize",
                         "\"fontSize\" must be a number, got " + std::string(fs.type_name()),
                         "Set fontSize to a positive pt value.");
            else if (fs.get<double>() <= 0.0)
                addIssue(r, Severity::Error, p + ".fontSize",
                         "fontSize must be positive, got " + std::to_string(fs.get<double>()),
                         "Set fontSize to a positive pt value, e.g. 16.");
        }

        // bold / italic
        if (item.contains("bold") && !item["bold"].is_boolean() && !item["bold"].is_number())
            addIssue(r, Severity::Warning, p + ".bold",
                     "\"bold\" should be true or false, got "
                         + std::string(item["bold"].type_name()),
                     "Use true for bold text, false for normal weight.");
        if (item.contains("italic") && !item["italic"].is_boolean() && !item["italic"].is_number())
            addIssue(r, Severity::Warning, p + ".italic",
                     "\"italic\" should be true or false, got "
                         + std::string(item["italic"].type_name()),
                     "Use true for italic text, false for upright.");

        // textColor
        if (item.contains("textColor"))
            validateColor(item["textColor"], p + ".textColor", r);

        // width / height — target box; 0 or omitted = natural size (no scaling)
        if (item.contains("width")) {
            const auto &w = item["width"];
            if (!w.is_number())
                addIssue(r, Severity::Error, p + ".width",
                         "\"width\" must be a number, got " + std::string(w.type_name()),
                         "Set width to a positive number, or 0 for natural size.");
            else if (w.get<double>() < 0.0)
                addIssue(r, Severity::Warning, p + ".width",
                         "width is negative: " + std::to_string(w.get<double>()),
                         "Set width to 0 (natural size) or a positive value.");
        }
        if (item.contains("height")) {
            const auto &h = item["height"];
            if (!h.is_number())
                addIssue(r, Severity::Error, p + ".height",
                         "\"height\" must be a number, got " + std::string(h.type_name()),
                         "Set height to a positive number, or 0 for natural size.");
            else if (h.get<double>() < 0.0)
                addIssue(r, Severity::Warning, p + ".height",
                         "height is negative: " + std::to_string(h.get<double>()),
                         "Set height to 0 (natural size) or a positive value.");
        }

        // underline
        if (item.contains("underline") && !item["underline"].is_boolean()
            && !item["underline"].is_number())
            addIssue(r, Severity::Warning, p + ".underline",
                     "\"underline\" should be true or false, got "
                         + std::string(item["underline"].type_name()),
                     "Use true to draw an underline beneath the text.");
    }
}

void Validator::validateImages(const json &arr, ValidationReport &r)
{
    for (size_t i = 0; i < arr.size(); ++i) {
        const auto &item = arr[i];
        std::string p    = "canvas.images[" + std::to_string(i) + "]";

        if (!item.is_object()) {
            addIssue(r, Severity::Error, p,
                     "Image must be an object, got " + std::string(item.type_name()),
                     "Define an image object with \"filePath\".");
            continue;
        }

        static const std::set<std::string> sKeys = {
            "filePath", "x", "y", "width", "height", "z", "comment", "unit",
        };
        checkUnknownFields(item, sKeys, p, r);
        validateUnitField(item, p, r);

        // filePath
        if (!item.contains("filePath")) {
            addIssue(r, Severity::Error, p, "Image missing required field \"filePath\"",
                     "Add the absolute path to a CMYK TIFF file, e.g. "
                     "\"/path/to/image.tiff\".");
        } else {
            const auto &fp = item["filePath"];
            if (!fp.is_string())
                addIssue(r, Severity::Error, p + ".filePath",
                         "\"filePath\" must be a string, got " + std::string(fp.type_name()),
                         "Provide the absolute path to a TIFF file.");
            else {
                const std::string &path = fp.get<std::string>();
                if (path.empty())
                    addIssue(r, Severity::Error, p + ".filePath", "filePath is an empty string",
                             "Provide the absolute path to a TIFF file.");
                else {
                    // Check if file exists.
                    //
                    // IMPORTANT: nlohmann::json stores strings as UTF-8, but on
                    // Windows std::filesystem::path assumes the system ANSI code
                    // page (e.g. GBK on Chinese Windows). Use u8path() so that
                    // UTF-8 paths containing non-ASCII characters (Chinese, etc.)
                    // are converted correctly to the internal wide representation.
                    //
                    // Also use the noexcept overload (with std::error_code) to
                    // avoid std::filesystem::filesystem_error crashing on paths
                    // that cannot be accessed (permission, code-page, long paths).
                    std::error_code ec;
                    if (!std::filesystem::exists(std::filesystem::u8path(path), ec)) {
                        if (ec)
                            addIssue(r, Severity::Error, p + ".filePath",
                                     "Cannot access file: " + path + " (" + ec.message() + ")",
                                     "Check the file path and ensure it is valid and "
                                     "accessible.");
                        else
                            addIssue(r, Severity::Error, p + ".filePath",
                                     "File does not exist: " + path,
                                     "Check the file path and ensure the file is "
                                     "accessible.");
                    } else {
                        // Warn if file extension is not TIFF
                        auto dotPos = path.rfind('.');
                        if (dotPos != std::string::npos) {
                            std::string ext = path.substr(dotPos);
                            for (auto &c : ext)
                                c = static_cast<char>(std::tolower(c));
                            if (ext != ".tiff" && ext != ".tif")
                                addIssue(r, Severity::Warning, p + ".filePath",
                                         "File extension is \"" + ext
                                             + "\" — only TIFF files are supported",
                                         "Use a CMYK TIFF file for correct rendering.");
                        }
                    }
                }
            }
        }

        // x / y
        if (item.contains("x") && !item["x"].is_number())
            addIssue(r, Severity::Error, p + ".x",
                     "\"x\" must be a number, got " + std::string(item["x"].type_name()),
                     "Set x to a numeric pixel coordinate.");
        if (item.contains("y") && !item["y"].is_number())
            addIssue(r, Severity::Error, p + ".y",
                     "\"y\" must be a number, got " + std::string(item["y"].type_name()),
                     "Set y to a numeric pixel coordinate.");

        // width / height
        if (item.contains("width")) {
            const auto &w = item["width"];
            if (!w.is_number())
                addIssue(r, Severity::Error, p + ".width",
                         "\"width\" must be a number, got " + std::string(w.type_name()),
                         "Set width to a positive number, or 0 for auto.");
            else if (w.get<double>() < 0.0)
                addIssue(r, Severity::Warning, p + ".width",
                         "width is negative: " + std::to_string(w.get<double>()),
                         "Set width to 0 (auto) or a positive value.");
        }
        if (item.contains("height")) {
            const auto &h = item["height"];
            if (!h.is_number())
                addIssue(r, Severity::Error, p + ".height",
                         "\"height\" must be a number, got " + std::string(h.type_name()),
                         "Set height to a positive number, or 0 for auto.");
            else if (h.get<double>() < 0.0)
                addIssue(r, Severity::Warning, p + ".height",
                         "height is negative: " + std::to_string(h.get<double>()),
                         "Set height to 0 (auto) or a positive value.");
        }

        // z
        if (item.contains("z") && !item["z"].is_number())
            addIssue(r, Severity::Warning, p + ".z",
                     "\"z\" should be a number, got " + std::string(item["z"].type_name()),
                     "Use a numeric value for z-order.");
    }
}

// ============================================================================
//  Helpers
// ============================================================================

void Validator::addIssue(ValidationReport  &r,
                         Severity           s,
                         const std::string &path,
                         const std::string &msg,
                         const std::string &suggestion) const
{
    r.issues.emplace_back(s, path, msg, suggestion);
}

// -- DPI-field validation (dpiX / dpiY; legacy "dpi" is deprecated) ----------

// 校验单个 DPI 字段（dpiX / dpiY）。缺省 300 时不产生任何 issue。
//
// NOTE: 这是文件级 static 自由函数（brief 要求放在 validateUnitField 附近），
// 无法调用 Validator 的私有成员 addIssue，因此直接 emplace 到
// ValidationReport::issues —— 与 addIssue 的实现完全一致。
static void validateDpiField(const json        &j,
                             const std::string &name,
                             const std::string &prefix,
                             ValidationReport  &r)
{
    if (!j.contains(name))
        return;
    const auto &d = j[name];
    if (!d.is_number()) {
        r.issues.emplace_back(Severity::Error, prefix + "." + name,
                              "\"" + name + "\" must be a number, got "
                                  + std::string(d.type_name()),
                              "Set " + name + " to an integer, e.g. 300.");
        return;
    }
    int v = d.get<int>();
    if (v <= 0)
        r.issues.emplace_back(Severity::Error, prefix + "." + name,
                              name + " must be positive, got " + std::to_string(v),
                              "Set " + name + " to a positive value, typically 300 for print.");
    else if (v < 72)
        r.issues.emplace_back(Severity::Warning, prefix + "." + name,
                              name + " is very low: " + std::to_string(v)
                                  + " (72+ recommended for screen, 300 for print)",
                              "Increase " + name + " to 150 or 300 for acceptable quality.");
    else if (v > 2400)
        r.issues.emplace_back(Severity::Warning, prefix + "." + name,
                              name + " is very high: " + std::to_string(v)
                                  + " (may cause excessive memory usage)",
                              "Consider reducing " + name + " to 300-600 for most print jobs.");
}

// -- Unit-field validation (shared by canvas and primitives) -----------------

void Validator::validateUnitField(const json &j, const std::string &path, ValidationReport &r)
{
    if (!j.contains("unit"))
        return;
    const auto &u = j["unit"];
    if (!u.is_string()) {
        addIssue(r, Severity::Error, path + ".unit",
                 "\"unit\" must be a string, got " + std::string(u.type_name()),
                 "Set unit to \"px\" (pixels) or \"mm\" (millimeters).");
    } else {
        std::string val = u.get<std::string>();
        if (val != "px" && val != "mm") {
            addIssue(r, Severity::Error, path + ".unit",
                     "Unknown unit \"" + val + "\". Expected \"px\" or \"mm\".",
                     "Set unit to \"px\" for pixels or \"mm\" for millimeters.");
        }
    }
}

// -- Unknown field detection ------------------------------------------------

namespace {

std::string toLower(const std::string &s)
{
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s)
        out += static_cast<char>(std::tolower(c));
    return out;
}

int adaptiveMaxDist(const std::string &a, const std::string &b)
{
    int minLen    = static_cast<int>(std::min(a.size(), b.size()));
    int threshold = minLen / 3;
    return threshold > 2 ? threshold : 2;
}

} // namespace

int Validator::editDistance(const std::string &a, const std::string &b, int budget)
{
    int m = static_cast<int>(a.size());
    int n = static_cast<int>(b.size());

    // Quick exit: empty strings
    if (m == 0)
        return n;
    if (n == 0)
        return m;

    // Length-difference lower bound: if already > budget, bail
    int lenDiff = m > n ? m - n : n - m;
    if (lenDiff > budget)
        return budget + 1;

    std::vector<int> prev2(n + 1), prev(n + 1), cur(n + 1);
    for (int j = 0; j <= n; ++j)
        prev[j] = j;

    for (int i = 1; i <= m; ++i) {
        cur[0]     = i;
        int rowMin = i;
        for (int j = 1; j <= n; ++j) {
            int cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
            cur[j]   = std::min({ prev[j] + 1,           // deletion
                                  cur[j - 1] + 1,        // insertion
                                  prev[j - 1] + cost }); // substitution

            // Transposition: swap adjacent characters
            if (i >= 2 && j >= 2 && a[i - 1] == b[j - 2] && a[i - 2] == b[j - 1])
                cur[j] = std::min(cur[j], prev2[j - 2] + 1);

            if (cur[j] < rowMin)
                rowMin = cur[j];
        }
        if (rowMin > budget)
            return budget + 1; // early bailout

        std::swap(prev2, prev);
        std::swap(prev, cur);
    }
    return prev[n];
}

std::vector<std::string> Validator::closestKeys(const std::string           &input,
                                                const std::set<std::string> &candidates)
{
    std::string                              inputLower = toLower(input);
    std::vector<std::pair<int, std::string>> matches;

    int bestDist = std::numeric_limits<int>::max();
    for (const auto &k : candidates) {
        int budget = adaptiveMaxDist(input, k);
        int d      = editDistance(inputLower, toLower(k), budget);
        if (d <= budget) {
            matches.emplace_back(d, k);
            if (d < bestDist)
                bestDist = d;
        }
    }

    if (matches.empty())
        return {};

    // Collect all candidates within threshold, sorted by distance then alphabetically
    std::sort(matches.begin(), matches.end(), [](const auto &a, const auto &b) {
        return a.first != b.first ? a.first < b.first : a.second < b.second;
    });

    std::vector<std::string> result;
    for (auto &[dist, name] : matches)
        result.push_back(std::move(name));
    return result;
}

void Validator::checkUnknownFields(const json                  &j,
                                   const std::set<std::string> &knownKeys,
                                   const std::string           &path,
                                   ValidationReport            &r) const
{
    if (!j.is_object())
        return;
    for (auto it = j.begin(); it != j.end(); ++it) {
        const std::string &key = it.key();
        if (knownKeys.find(key) != knownKeys.end())
            continue;

        // Ignore keys starting with _ or $ (often used for comments / metadata)
        if (!key.empty() && (key[0] == '_' || key[0] == '$'))
            continue;

        std::string              hint;
        std::vector<std::string> suggestions = closestKeys(key, knownKeys);
        if (!suggestions.empty()) {
            if (suggestions.size() == 1) {
                hint = "Did you mean \"" + suggestions[0] + "\"?";
            } else {
                hint = "Did you mean one of: ";
                for (size_t i = 0; i < suggestions.size(); ++i) {
                    if (i > 0)
                        hint += ", ";
                    hint += "\"" + suggestions[i] + "\"";
                }
                hint += "?";
            }
        } else {
            hint = "This field will be ignored by the renderer.";
        }

        addIssue(r, Severity::Hint, path + "." + key, "Unknown field \"" + key + "\"", hint);
    }
}
