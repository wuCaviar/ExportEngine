#pragma once

#include "ExportEngine.h"
#include "EEError.h"
#include "SceneData.h"

#include <cstdint>
#include <mutex>
#include <system_error>
#include <vector>

namespace ATHC::EE {

// ============================================================================
//  IRenderSink — 可插拔的渲染输出接收器
// ============================================================================
//
//  把 json2tiff() 流水线最后一步“渲染 → 写 TIFF”抽象成策略接口：渲染器产出的
//  最终 CMYK 像素（RenderContext::cmykBuf）流入一个接收器，接收器可以是
//  TiffSink（写 TIFF，默认）、MemorySink（回传内存）、CallbackSink（转发宿主）
//  或任意自定义实现。
//
//  并发：
//    - granularity()/preferredRowsPerStrip()/requiresOrderedInput() 在 begin()
//      之前由编排线程调用，单线程。
//    - begin()/end() 各调用一次，单线程（编排线程）；write 失败后 end() 仍会被
//      调用，实现必须安全清理。
//    - writeStrip()/writeRow()/writeFrame() 可能被多个渲染 worker **并发、乱序**
//      调用 —— 实现必须线程安全。
//    - 错误语义 first-error-wins，错误码从 errorCode() 读取。
// ============================================================================

// 接收器期望的数据颗粒度，1:1 映射到渲染器方法：
//   Frame  → SceneRenderer::render()      整帧一次（内存重，最简单）
//   Strip  → SceneRenderer::renderStrip() 全宽横条（内存友好，默认）
//   Row    → SceneRenderer::renderRow()   全宽单行（内存最小，按行消费）
enum class SinkGranularity : uint8_t
{
    Frame = 0,
    Strip = 1,
    Row = 2,
};

// begin() 前一次性下发的输出元数据。接收器据此构造输出（文件头 / 缓冲 / 编码器）。
struct SinkDescriptor
{
    int width = 0;
    int height = 0;
    Dpi dpi{};
    int samplesPerPixel = 4; // 像素步长（4 = 标准 CMYK，>4 = 多通道/专色）
    std::vector<uint16_t> sampleInfo; // EXTRASAMPLES（多通道时非空，CMYK 时为空）
};

class EE_API IRenderSink
{
public:
    virtual ~IRenderSink() = default;

    // 期望粒度；编排层据此选择 render/renderStrip/renderRow。
    virtual SinkGranularity granularity() const noexcept = 0;

    // 可选覆盖：期望的条带高度，返回 0 = 用引擎默认
    //（TiffWriter::defaultRowsPerStrip 约 8MB/strip）。仅 Strip 粒度使用。
    virtual int preferredRowsPerStrip(int width, int height) const
    {
        (void)width;
        (void)height;
        return 0;
    }

    // 可选覆盖：需要按行序到达时返回 true（编排层对 Strip 采用单 worker 串行渲染）。
    virtual bool requiresOrderedInput() const { return false; }

    // 生命周期。begin/end 各调用一次，且都在编排线程（单线程）。
    virtual bool begin(const SinkDescriptor &desc) = 0;
    virtual bool end() = 0;

    // 数据入口。只实现 granularity() 声明的那一个；其余默认置
    // EEError::sink_unsupported_granularity 并返回 false。
    // 缓冲按值传递（move 语义）：渲染器把 strip/row 局部缓冲 move 进来，
    // 接收器可零拷贝入队/落盘或继续 move 给宿主。
    virtual bool writeStrip(int startRow, int rows, std::vector<uint8_t> cmyk);
    virtual bool writeRow(int row, std::vector<uint8_t> cmyk);
    virtual bool writeFrame(std::vector<uint8_t> cmyk);

    // 首个错误胜出，线程安全。
    virtual std::error_code errorCode() const;

protected:
    // 供子类复用的线程安全错误状态（与 TiffWriter::setError 同语义）。
    void setError(std::error_code ec);
    void clearError();

private:
    mutable std::mutex m_mtx;
    std::error_code m_error;
};

// ── 默认实现：未实现的粒度入口 ──────────────────────────────────────────
inline bool IRenderSink::writeStrip(int, int, std::vector<uint8_t>)
{
    setError(EEError::sink_unsupported_granularity);
    return false;
}

inline bool IRenderSink::writeRow(int, std::vector<uint8_t>)
{
    setError(EEError::sink_unsupported_granularity);
    return false;
}

inline bool IRenderSink::writeFrame(std::vector<uint8_t>)
{
    setError(EEError::sink_unsupported_granularity);
    return false;
}

inline std::error_code IRenderSink::errorCode() const
{
    std::lock_guard<std::mutex> lock(m_mtx);
    return m_error;
}

inline void IRenderSink::setError(std::error_code ec)
{
    std::lock_guard<std::mutex> lock(m_mtx);
    if (!m_error)
        m_error = ec;
}

inline void IRenderSink::clearError()
{
    std::lock_guard<std::mutex> lock(m_mtx);
    m_error.clear();
}

} // namespace ATHC::EE
