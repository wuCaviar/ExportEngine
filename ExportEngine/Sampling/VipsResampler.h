#pragma once

#include "IResampler.h"

namespace ATHC::EE {

// 最近邻重采样（vips KERNEL_NEAREST + 精确尺寸修正）。const 方法无共享可变
// 状态，多线程可安全共用同一实例。
class EE_API VipsResampler : public IResampler
{
public:
    void resize(const uint8_t *src,
                int            srcW,
                int            srcH,
                uint8_t       *dst,
                int            dstW,
                int            dstH,
                int            channels) const override;
};

} // namespace ATHC::EE