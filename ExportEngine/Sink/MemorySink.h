#pragma once

#include "IRenderSink.h"

#include <cstring>

namespace ATHC::EE {

// ============================================================================
//  MemorySink — 把渲染结果回收为内存中的一整帧 CMYK 缓冲。
// ============================================================================
//
//  Strip 粒度，按 startRow 索引拼进整帧，天然乱序安全；内存峰值与 strip 流式
//  模式一致（不额外产生 2×W×H×4 尖峰）。end() 后通过 frame()/descriptor() 取
//  整帧与元数据，供宿主二次编码、送 GUI、上传等。
// ============================================================================
class MemorySink : public IRenderSink
{
public:
    SinkGranularity granularity() const noexcept override { return SinkGranularity::Strip; }

    bool begin(const SinkDescriptor &desc) override
    {
        clearError();
        m_desc = desc;
        m_frame.assign(static_cast<size_t>(desc.width) * static_cast<size_t>(desc.height)
                           * static_cast<size_t>(desc.samplesPerPixel),
            0);
        return true;
    }

    bool writeStrip(int startRow, int rows, std::vector<uint8_t> cmyk) override
    {
        const size_t rowBytes = static_cast<size_t>(m_desc.width) * m_desc.samplesPerPixel;
        const size_t off = static_cast<size_t>(startRow) * rowBytes;
        const size_t n = static_cast<size_t>(rows) * rowBytes;
        if (cmyk.size() < n || off + n > m_frame.size()) {
            setError(EEError::sink_not_initialized);
            return false;
        }
        std::memcpy(m_frame.data() + off, cmyk.data(), n);
        return true;
    }

    bool end() override { return true; }

    const std::vector<uint8_t> &frame() const { return m_frame; }
    const SinkDescriptor &descriptor() const { return m_desc; }

private:
    SinkDescriptor m_desc;
    std::vector<uint8_t> m_frame;
};

} // namespace ATHC::EE
