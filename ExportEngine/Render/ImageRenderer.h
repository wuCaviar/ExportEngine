#pragma once
#include "RenderContext.h"
#include "SceneData.h"

// CMYK TIFF 图像渲染，采用条带/波段流式处理以限制内存使用。
// 源在渲染时按波段进行解码 + 颜色转换 + 缩放（永远不会生成完整的重采样帧）。
// 峰值内存 ≈ 一个源波段 + 请求的输出行。

namespace ATHC::EE {

class IColorConverter;
class IResampler;

namespace ImageRenderer {

void draw(RenderContext &ctx, const ImageItem &img, const Dpi &dpi);

/// 对图像图元进行廉价的早期验证（头部/变换探测），以便
/// 无法读取或无法转换的文件一次性被标记为坏文件。源在渲染时惰性解码，
/// 因此没有可预热的帧。失败时返回 false（已标记为坏——渲染将跳过）。
bool preDecode(
    const ImageItem &img, const Dpi &dpi, IColorConverter *cv, IResampler *resampler);

} // namespace ImageRenderer

} // namespace ATHC::EE
