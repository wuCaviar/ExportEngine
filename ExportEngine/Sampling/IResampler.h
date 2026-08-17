#pragma once

#include "ExportEngine.h"

#include <cstdint>

namespace ATHC::EE {

// ============================================================================
//  IResampler — 像素重采样（缩放）策略
// ============================================================================
//  EE 只持指针、只调用本接口，不关心算法（最近邻 / 双线性 / 双三次 / lanczos）。
//  默认实现 NearestResampler（vips KERNEL_NEAREST + 精确尺寸修正）与现有行为
//  字节级一致；宿主可注入其它实现替换缩放后端。
// ============================================================================
class EE_API IResampler
{
public:
    virtual ~IResampler() = default;

    // srcW×srcH → dstW×dstH，channels 字节/像素，行主序 band-interleaved。
    // dst 已预分配 dstW×dstH×channels；src 与 dst 可指向同一缓冲（就地）。
    // srcW==dstW 且 srcH==dstH 时必须是恒等操作。
    virtual void resize(const uint8_t *src, int srcW, int srcH, uint8_t *dst, int dstW, int dstH,
        int channels) const = 0;
};

} // namespace ATHC::EE
