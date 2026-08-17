#pragma once

#include "IRenderSink.h"

#include <string>

namespace ATHC::EE {

// ============================================================================
//  TiffSink — 把 IRenderSink 适配到现有 TiffWriter，保持写 TIFF 的默认行为。
// ============================================================================
//
//  默认接收器：json2tiff() 内部即构造 TiffSink 再走统一流水线，输出与旧实现
//  逐字节一致（复用同一 TiffWriter 生产-消费管道）。
//
//  粒度固定为 SinkGranularity::Strip（条带存储，LZW 友好）。
// ============================================================================
class TiffWriter;
class EE_API TiffSink : public IRenderSink
{
public:
    explicit TiffSink(std::string path);
    virtual ~TiffSink();

    SinkGranularity granularity() const noexcept override { return SinkGranularity::Strip; }

    int preferredRowsPerStrip(int width, int height) const override;

    bool begin(const SinkDescriptor &desc) override;
    bool writeStrip(int startRow, int rows, std::vector<uint8_t> cmyk) override;
    bool end() override;

private:
    TiffWriter *m_writer;
    std::string m_path;
    SinkDescriptor m_desc;
};

} // namespace ATHC::EE
