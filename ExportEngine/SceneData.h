#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <optional>
#include <utility>

namespace ATHC::EE {

enum class LineStyle : uint8_t
{
    Solid = 0,
    Dashed = 1,
    Dotted = 2,
    DashDot = 3,
    DoubleDotDash = 4,
    None = 5,
};

struct Color
{
    // ch[0..3] = C/M/Y/K，内部值 0-255（JSON 层按 0-100% 换算）。
    // 默认构造 = CMYK 黑 (0,0,0,100%)
    double ch[4] = { 0, 0, 0, 255 };
    double alpha = 1.0;

    static Color fromCMYK(double c, double m, double y, double k, double a = 1.0)
    {
        Color col;
        col.ch[0] = c;
        col.ch[1] = m;
        col.ch[2] = y;
        col.ch[3] = k;
        col.alpha = a;
        return col;
    }
};

struct Gradient
{
    enum Type : uint8_t
    {
        NONE = 0,
        LINEAR = 1,
        RADIAL = 2,
        CONIC = 3
    };
    Type type = NONE;

    struct Stop
    {
        double offset = 0.0;
        Color color;
    };
    std::vector<Stop> stops;

    double x1 = 0.0, y1 = 0.0;
    double x2 = 1.0, y2 = 0.0;
    double cx = 0.5, cy = 0.5;
    double r = 0.5;
    double startAngle = 0.0;
};

struct GridFill
{
    Color gridColor; // 网格线颜色
    Color backgroundColor; // 背景色（transparentBackground 时不可用）
    bool transparentBackground = false;
    double cellWidth = 10.0; // 格子宽（固定 1mm，按画布 dpi 换算为像素）
    double cellHeight = 10.0; // 格子高（固定 1mm，按画布 dpi 换算为像素）
    double lineWidth = 1.0; // 网格线宽（固定 1px）
};

struct TextureFill
{
    std::string filePath;
    double offsetX = 0.0; // 水平偏移（画布单位，解析后为像素）
    double offsetY = 0.0; // 垂直偏移（画布单位，解析后为像素）
    bool useOriginalSize = false; // 使用原图大小
    double customWidth = 0.0; // 自定义宽（useOriginalSize 时不可用）
    double customHeight = 0.0; // 自定义高（useOriginalSize 时不可用）
};

struct Rect
{
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;
    double z = 0.0;
    double strokeWidth = 0.0;
    Color strokeColor;
    Color fillColor;
    std::optional<Gradient> gradient;
    std::optional<GridFill> gridFill;
    std::optional<TextureFill> textureFill;
    double cornerRadius = 0.0;
    LineStyle lineStyle = LineStyle::Solid;
};

struct Circle
{
    double cx = 0.0;
    double cy = 0.0;
    double radiusX = 0.0;
    double radiusY = 0.0;
    double z = 0.0;
    double strokeWidth = 0.0;
    Color strokeColor;
    Color fillColor;
    std::optional<Gradient> gradient;
    LineStyle lineStyle = LineStyle::Solid;
};

struct FreeLine
{
    std::vector<std::pair<double, double>> points;
    double z = 0.0;
    double strokeWidth = 1.0;
    Color strokeColor;
    LineStyle lineStyle = LineStyle::Solid;
};

struct BezierCurve
{
    std::vector<std::pair<double, double>> controlPoints;
    double z = 0.0;
    double strokeWidth = 1.0;
    Color strokeColor;
    LineStyle lineStyle = LineStyle::Solid;
};

struct Line
{
    double x1 = 0.0;
    double y1 = 0.0;
    double x2 = 0.0;
    double y2 = 0.0;
    double z = 0.0;
    double strokeWidth = 1.0;
    Color strokeColor;
    LineStyle lineStyle = LineStyle::Solid;
};

struct ImageItem
{
    std::string filePath;
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;
    double z = 0.0;
};

struct Text
{
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
    std::string content;
    std::string fontFamily;
    double fontSize = 16.0;
    bool bold = false;
    bool italic = false;
    Color textColor;
};

// 画布分辨率。x = 水平（X 方向）DPI，y = 垂直（Y 方向）DPI。
// 各向异性（x ≠ y）时像素网格为非正方形，mm→px 换算按方向。
struct Dpi
{
    int x = 300;
    int y = 300;
};

struct Canvas
{
    int width = 0;
    int height = 0;
    Dpi dpi;
    Color background;
    std::vector<Rect> rects;
    std::vector<Circle> circles;
    std::vector<FreeLine> freeLines;
    std::vector<BezierCurve> bezierCurves;
    std::vector<Line> lines;
    std::vector<Text> texts;
    std::vector<ImageItem> images;

    // Multi-channel output: discovered by pre-scanning image TIFFs.
    // Default 4 = standard CMYK; >4 adds extra samples (spot colors, etc.)
    int samplesPerPixel = 4;
    std::vector<uint16_t> sampleInfo;
};

} // namespace ATHC::EE
