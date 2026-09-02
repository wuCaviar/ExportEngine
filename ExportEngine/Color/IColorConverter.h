#pragma once

#include "EEError.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace ATHC::EE {

// ============================================================================
//  IColorConverter — 颜色转换引擎（策略）
// ============================================================================
//  EE 只持指针、只调用本接口，不关心底层是 lcms2 / vips / 查表 / 直通，也
//  不关心「源 → CMYK」的转换过程与 CMYK 目标 profile——引擎自己管理自己的
//  ICC profile。
//
//  并发契约：
//    - makeToCmyk() 返回的 IColorTransform 非线程安全，每线程需独立句柄
//      （用 cloneForThread() 为每个 worker 克隆引擎后再 makeToCmyk）。
// ============================================================================

// 源像素颜色空间（EE 探测得到，只做「描述」，不参与转换）。
enum class PixelColorSpace : uint8_t
{
    Rgb,   // 3 字节/像素（RGB_8）
    Rgba,  // 4 字节/像素（RGBA_8，alpha 不参与转换）
    Gray,  // 1 字节/像素（GRAY_8）
    GrayA, // 2 字节/像素（GRAYA_8，alpha 不参与转换）
    Cmyk,  // 直通（调用方原样拷贝 4 字节，含专色通道按需保留）
};

// 不透明转换句柄：源 → CMYK_8（4 字节/像素）。每线程独立、非线程安全。
class EE_API IColorTransform
{
public:
    virtual ~IColorTransform() = default;

    // pixels 个源像素 → CMYK；dst 至少 pixels×4 字节。
    virtual void convert(const uint8_t *src, uint8_t *dst, int pixels) const = 0;
};

class EE_API IColorConverter
{
public:
    virtual ~IColorConverter() = default;

    // 为「源空间 + 可选内嵌 ICC」构建 →CMYK 句柄。
    // embeddedIcc = 源像素内嵌的 ICC 字节（可为空 = 无内嵌，引擎用自身回退 profile）。
    // Cmyk 空间返回 nullptr = 直通（调用方原样拷贝，不转换）。
    // 引擎未就绪/源空间不支持时返回 nullptr（源图跳过）。
    virtual std::unique_ptr<IColorTransform>
    makeToCmyk(PixelColorSpace src, const std::vector<uint8_t> &embeddedIcc) = 0;

    // 每线程独立克隆（transform 句柄非线程安全）。
    virtual std::unique_ptr<IColorConverter> cloneForThread() const = 0;
};

} // namespace ATHC::EE
