#pragma once

#include "SceneData.h"

#include <memory>
#include <vector>
#include <functional>

namespace ATHC::EE {

class IColorConverter;
class IResampler;
class FontEngine;
class RenderContext;

struct RenderResult
{
    std::vector<uint8_t> cmykBuf;
    int width = 0;
    int height = 0;
};

// 用于绘制调用裁剪的轴对齐整数边界框
struct DrawBBox
{
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0; // 包含边界
    bool valid = false;
    bool intersects(int tx, int ty, int tw, int th) const
    {
        if (!valid)
            return true; // 无边界路径——始终渲染
        return !(tx + tw <= x0 || tx >= x1 || ty + th <= y0 || ty >= y1);
    }
};

class SceneRenderer
{
public:
    SceneRenderer();
    ~SceneRenderer();
    SceneRenderer(const SceneRenderer &) = delete;
    SceneRenderer &operator=(const SceneRenderer &) = delete;

    void setConverter(IColorConverter *converter);
    void setResampler(IResampler *resampler);

    // 渲染前早期验证 + 纹理预热。图片源在渲染时惰性解码，因此 preDecodeImages
    // 仅探测其头部/变换以一次性标记坏文件，并预热 TextureSource 的帧缓存。
    // 唯一文件路径会去重并进行 bbox 裁剪（与 renderTile 匹配）。
    //
    // progress 由工作线程为每个完成的唯一路径调用（done/total 统计尝试次数——
    // 失败的图片在渲染时跳过），必须是线程安全的。当没有内容可预解码时完全不调用。
    using DecodeProgress = std::function<void(int done, int total, const std::string &filePath)>;
    void preDecodeImages(const Canvas &canvas, DecodeProgress progress);

    // 全画布渲染（与现有 API 兼容）
    RenderResult render(const Canvas &canvas);

    // 逐行渲染并带每行回调（从工作线程调用）。
    // 每个单元是一整行（宽×1 像素）；行可能由并发工作线程乱序到达——回调必须是线程安全的。
    //
    // 峰值内存：N_workers × width × 4。
    // 适用场景：一次消费一行的接收器（内存最小化）。
    using RowCallback = std::function<void(int row, std::vector<uint8_t> cmykRow)>;
    void renderRow(const Canvas &canvas, RowCallback callback);

    // 逐条带渲染并带每条带回调（从工作线程调用）。
    // rowsPerStrip=0 时一次性渲染整个画布。
    //
    // 每个条带横跨整个画布宽度，高度为 `rowsPerStrip` 行。
    // 回调接收条带局部缓冲区（宽×行数），而非
    // 全帧缓冲区——这避免了组装图块到全帧时的 2 × W × H × 4 峰值内存
    // 激增。
    //
    // 条带可能由并发工作线程乱序渲染；
    // 回调从工作线程调用，必须是线程安全的。
    //
    // 峰值内存：N_workers × rowsPerStrip × width × 4。
    // 适用场景：条带式 TIFF 输出，写入器直接消费条带
    // （例如 TiffWriter::writeStrip），无需全帧
    // 中间缓冲区。
    //
    // 示例（结合 TiffWriter 流式写入）：
    //   writer.beginStripWrite(path, W, H, Dpi{300, 300}, rps, icc);
    //   renderer.renderStrip(canvas, rps,
    //       [&](int y, int rows, std::vector<uint8_t> cmyk) {
    //           writer.writeStrip(y, rows, std::move(cmyk));
    //       });
    //   writer.endStripWrite();
    using StripCallback =
        std::function<void(int startRow, int rows, std::vector<uint8_t> cmykStrip)>;
    // ordered = true 时单 worker 串行渲染（自上而下），供需要按行序到达的接收器使用。
    void renderStrip(
        const Canvas &canvas, int rowsPerStrip, StripCallback callback, bool ordered = false);

private:
    enum class DrawType : uint8_t
    {
        RECT,
        CIRCLE,
        FREELINE,
        BEZIER,
        LINE,
        TEXT,
        IMAGE
    };

    struct DrawCall
    {
        double z;
        DrawType type;
        size_t index;
        DrawBBox bbox;
    };

    void renderTile(const std::vector<DrawCall> &calls, const Canvas &canvas, RenderContext &ctx,
        FontEngine *fe);

    std::vector<DrawCall> buildDrawCalls(const Canvas &canvas);
    void dispatchDraw(
        DrawType type, size_t index, const Canvas &canvas, RenderContext &ctx, FontEngine *fe);

    static DrawBBox computeBBox(DrawType type, const void *prim, const Canvas *canvasHint);

    std::unique_ptr<FontEngine> m_fontEngine;
    IColorConverter *m_converter = nullptr;
    IResampler *m_resampler = nullptr;
};

} // namespace ATHC::EE
