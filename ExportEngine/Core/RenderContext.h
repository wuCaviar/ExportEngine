#pragma once

#include "SceneData.h"

#include <vector>
#include <cstdint>

namespace ATHC::EE {

class IColorConverter;
class IResampler;

// Dual-buffer rendering context with tile window support.
// All pixel writes are in tile-local coordinates (0..effectiveWidth()-1, 0..effectiveHeight()-1).
// When tileW==0, the window spans the full canvas dimension.
class RenderContext
{
public:
    int canvasWidth = 0;
    int canvasHeight = 0;

    int extrasamples = 0;
    std::vector<uint16_t> sampleInfo;
    int samplesPerPixel = 4; // per-pixel stride in cmykBuf (4 = std CMYK)
    void setSamplesPerPixel(int spp); // reallocates cmykBuf when spp changes

    // Tile window within the logical canvas (0 means full-canvas)
    int tileX = 0;
    int tileY = 0;
    int tileW = 0;
    int tileH = 0;

    // Output buffer (sized to tile dimensions)
    std::vector<uint8_t> cmykBuf;

    // Non-owning observers; set before rendering by the owning SceneRenderer.
    IColorConverter *converter = nullptr; // 颜色转换引擎（图片/纹理用）
    IResampler *resampler = nullptr;      // 像素缩放（图片用）

    // -- Initialization --
    void initFullCanvas(int w, int h);
    void initTile(int cw, int ch, int tx, int ty, int tw, int th);
    void fillBackground(const Color &bg);

    // -- Dimensions --
    int effectiveWidth() const { return tileW > 0 ? tileW : canvasWidth; }
    int effectiveHeight() const { return tileH > 0 ? tileH : canvasHeight; }

    // -- Pixel blending (tile-local coordinates) --
    void blendCmyk(int x, int y, uint8_t c, uint8_t m, uint8_t y_, uint8_t k, uint8_t a);

    // Unified pixel write: CMYK-only (inline, hot path)
    void paintPixel(int x, int y, uint8_t c1, uint8_t c2, uint8_t c3, uint8_t c4, uint8_t a)
    {
        blendCmyk(x, y, c1, c2, c3, c4, a);
    }

    // -- Color pipeline (inline, called per-pixel in render loops) --
    void prepareColor(
        const Color &color, uint8_t &c1, uint8_t &c2, uint8_t &c3, uint8_t &c4, uint8_t &a)
    {
        a = static_cast<uint8_t>(color.alpha * 255 + 0.5);
        c1 = static_cast<uint8_t>(color.ch[0] + 0.5);
        c2 = static_cast<uint8_t>(color.ch[1] + 0.5);
        c3 = static_cast<uint8_t>(color.ch[2] + 0.5);
        c4 = static_cast<uint8_t>(color.ch[3] + 0.5);
    }
    // Evaluate gradient color at normalized coordinates (gx, gy in [0,1]).
    // Gradient must be valid (type != NONE, at least 2 stops).
    // CMYK-only：CMYK 空间内插值，输出 4 通道。
    bool evalGradient(const Gradient &g, double gx, double gy, uint8_t &c1, uint8_t &c2,
        uint8_t &c3, uint8_t &c4, uint8_t &a);

    // Evaluate grid fill at rect-local pixel coordinates (px, py measured
    // from the rect's top-left corner, unrotated). Grid lines sit at
    // integer multiples of cellWidth/cellHeight (parsed as a fixed 1mm cell
    // derived from the canvas dpi); line width is fixed at 1px — coverage is
    // nearest-pixel (1 inside half the line width, 0 outside), no smoothstep
    // antialiasing, so every line inks exactly one device pixel. Colors mix
    // in the grid color's CMYK space (naive channel interpolation, like
    // evalGradient). Line coverage is carried in the alpha channel; in the
    // transparent-background case channels are left unscaled so the
    // straight-alpha over blend applies coverage exactly once.
    // Returns false on invalid grid parameters.
    bool evalGridFill(const GridFill &g, double px, double py, double rectW, double rectH,
        uint8_t &c1, uint8_t &c2, uint8_t &c3, uint8_t &c4, uint8_t &a);
};

} // namespace ATHC::EE
