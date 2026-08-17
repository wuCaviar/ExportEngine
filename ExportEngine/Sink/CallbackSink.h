#pragma once

#include "IRenderSink.h"

#include <functional>
#include <utility>

namespace ATHC::EE {

// ============================================================================
//  CallbackSink — 把 begin/writeStrip/end 转发给宿主注入的回调，让宿主无需
//  继承 IRenderSink 即可接条带流（预览、上传、跨进程管道等）。
// ============================================================================
class CallbackSink : public IRenderSink
{
public:
    using BeginFn = std::function<bool(const SinkDescriptor &)>;
    using StripFn = std::function<bool(int startRow, int rows, const std::vector<uint8_t> &)>;
    using EndFn = std::function<bool()>;

    CallbackSink(BeginFn onBegin, StripFn onStrip, EndFn onEnd)
        : m_onBegin(std::move(onBegin)), m_onStrip(std::move(onStrip)), m_onEnd(std::move(onEnd))
    { }

    SinkGranularity granularity() const noexcept override { return SinkGranularity::Strip; }

    bool begin(const SinkDescriptor &desc) override
    {
        clearError();
        if (m_onBegin && !m_onBegin(desc)) {
            setError(EEError::sink_write_failed);
            return false;
        }
        return true;
    }

    bool writeStrip(int startRow, int rows, std::vector<uint8_t> cmyk) override
    {
        // 转发给宿主回调（const& 只读，move 进来的缓冲在回调期间有效）。
        if (!m_onStrip || !m_onStrip(startRow, rows, cmyk)) {
            setError(EEError::sink_write_failed);
            return false;
        }
        return true;
    }

    bool end() override
    {
        if (m_onEnd && !m_onEnd()) {
            setError(EEError::sink_write_failed);
            return false;
        }
        return true;
    }

private:
    BeginFn m_onBegin;
    StripFn m_onStrip;
    EndFn m_onEnd;
};

} // namespace ATHC::EE
