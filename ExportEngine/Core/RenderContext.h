#pragma once

#include "SceneData.h"

#include <vector>
#include <cstdint>

namespace ATHC::EE {

class IColorConverter;
class IResampler;

// 带图块窗口支持的双缓冲渲染上下文。
// 所有像素写入均在图块局部坐标系中 (0..effectiveWidth()-1, 0..effectiveHeight()-1)。
// 当 tileW==0 时，窗口跨越整个画布维度。
class RenderContext
{
public:
    int canvasWidth = 0;
    int canvasHeight = 0;

    int extrasamples = 0;
    std::vector<uint16_t> sampleInfo;
    int samplesPerPixel = 4; // 每像素步长（cmykBuf 中，4 = 标准 CMYK）
    void setSamplesPerPixel(int spp); // spp 变化时重新分配 cmykBuf

    // 逻辑画布内的图块窗口（0 表示全画布）
    int tileX = 0;
    int tileY = 0;
    int tileW = 0;
    int tileH = 0;

    // 输出缓冲区（按图块尺寸分配）
    std::vector<uint8_t> cmykBuf;

    // 非拥有观察者；由所属 SceneRenderer 在渲染前设置。
    IColorConverter *converter = nullptr; // 颜色转换引擎（图片/纹理用）
    IResampler *resampler = nullptr;      // 像素缩放（图片用）

    // -- 初始化 --
    void initFullCanvas(int w, int h);
    void initTile(int cw, int ch, int tx, int ty, int tw, int th);
    void fillBackground(const Color &bg);

    // -- 尺寸 --
    int effectiveWidth() const { return tileW > 0 ? tileW : canvasWidth; }
    int effectiveHeight() const { return tileH > 0 ? tileH : canvasHeight; }

    // -- 像素混合（图块局部坐标）--
    void blendCmyk(int x, int y, uint8_t c, uint8_t m, uint8_t y_, uint8_t k, uint8_t a);

    // 统一像素写入：仅 CMYK（内联，热路径）
    void paintPixel(int x, int y, uint8_t c1, uint8_t c2, uint8_t c3, uint8_t c4, uint8_t a)
    {
        blendCmyk(x, y, c1, c2, c3, c4, a);
    }

    // -- 颜色管线（内联，在渲染循环中每像素调用）--
    void prepareColor(
        const Color &color, uint8_t &c1, uint8_t &c2, uint8_t &c3, uint8_t &c4, uint8_t &a)
    {
        a = static_cast<uint8_t>(color.alpha * 255 + 0.5);
        c1 = static_cast<uint8_t>(color.ch[0] + 0.5);
        c2 = static_cast<uint8_t>(color.ch[1] + 0.5);
        c3 = static_cast<uint8_t>(color.ch[2] + 0.5);
        c4 = static_cast<uint8_t>(color.ch[3] + 0.5);
    }
    // 在归一化坐标 (gx, gy ∈ [0,1]) 处计算渐变色。
    // 渐变必须有效（type != NONE，至少 2 个 stops）。
    // CMYK-only：CMYK 空间内插值，输出 4 通道。
    bool evalGradient(const Gradient &g, double gx, double gy, uint8_t &c1, uint8_t &c2,
        uint8_t &c3, uint8_t &c4, uint8_t &a);

    // 在矩形局部像素坐标 (px, py) 处计算网格填充（从矩形左上角测量，未旋转）。
    // 网格线位于 cellWidth/cellHeight 的整数倍处（解析为固定 1mm 格子，由画布 dpi 换算）；
    // 线宽固定为 1px——覆盖度采用最近像素（半线宽内为 1，外为 0），无 smoothstep 抗锯齿，
    // 因此每条线精确墨染一个设备像素。颜色在网格色的 CMYK 空间中混合（类似 evalGradient 的
    // 朴素通道插值）。线覆盖度由 alpha 通道承载；在透明背景情况下通道保持不缩放，以便
    // 直连 alpha over 混合仅应用一次覆盖度。
    // 网格参数无效时返回 false。
    bool evalGridFill(const GridFill &g, double px, double py, double rectW, double rectH,
        uint8_t &c1, uint8_t &c2, uint8_t &c3, uint8_t &c4, uint8_t &a);
};

} // namespace ATHC::EE
