#pragma once

#include "IRenderSink.h"

namespace ATHC::EE {

// ============================================================================
//  NullSink — 丢弃所有渲染输出，仅用于基准测试渲染吞吐。
// ============================================================================
class NullSink : public IRenderSink
{
public:
    SinkGranularity granularity() const noexcept override { return SinkGranularity::Strip; }

    bool begin(const SinkDescriptor &) override
    {
        clearError();
        return true;
    }
    bool writeStrip(int, int, std::vector<uint8_t>) override { return true; }
    bool end() override { return true; }
};

} // namespace ATHC::EE
