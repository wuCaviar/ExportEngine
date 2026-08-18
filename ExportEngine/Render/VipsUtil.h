#pragma once

#include <vips/vips.h>

#include <utility>

namespace ATHC::EE {

// 提取 vips 加载的栅格图像的 DPI（每轴：x, y）。
//
// vips 加载器（PNG/JPEG/…）不直接暴露 "dpi" 元数据字段——
// 分辨率存储在图像头部的 xres/yres 中，单位为像素/毫米，
// 与源单位无关（VIPS_META_RESOLUTION_UNIT 仅记录原始单位，供保存器选择输出单位）。
// ImageRenderer 和 TextureSource 共享此函数（两者均用 vips 解码，但通过 lcms2 转换颜色）。
// 若无有效分辨率，每轴默认回退到 72.0。
inline std::pair<double, double> extractVipsImageDpi(VipsImage *img)
{
    double dpiX = vips_image_get_xres(img) * 25.4; // px/mm → px/inch（像素/毫米 → 像素/英寸）
    double dpiY = vips_image_get_yres(img) * 25.4;
    return { dpiX >= 1.0 ? dpiX : 72.0, dpiY >= 1.0 ? dpiY : 72.0 };
}

} // namespace ATHC::EE
