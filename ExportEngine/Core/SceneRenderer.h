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

// Axis-aligned integer bounding box for draw-call culling
struct DrawBBox
{
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0; // inclusive
    bool valid = false;
    bool intersects(int tx, int ty, int tw, int th) const
    {
        if (!valid)
            return true; // unbounded path — always render
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

    // Early validation + texture warming before rendering. Image sources
    // decode lazily at render time, so preDecodeImages only probes their
    // header/transform to mark bad files once, and warms TextureSource's
    // frame cache. Unique file paths are deduplicated and bbox-culled
    // (matches renderTile).
    //
    // progress is invoked from worker threads for each completed unique path
    // (done/total counts attempts — failed images are skipped at render time)
    // and must be thread-safe. Not called at all when there is nothing to
    // pre-decode.
    using DecodeProgress = std::function<void(int done, int total, const std::string &filePath)>;
    void preDecodeImages(const Canvas &canvas, DecodeProgress progress);

    // Full-canvas render (compatible with existing API)
    RenderResult render(const Canvas &canvas);

    // Row rendering with per-row callback (called from worker threads).
    // Each unit is one full-width row (width×1 pixels); rows may arrive
    // out-of-order by concurrent workers — the callback must be thread-safe.
    //
    // Peak memory: N_workers × width × 4.
    // Best for: sinks that consume a single row at a time (minimal memory).
    using RowCallback = std::function<void(int row, std::vector<uint8_t> cmykRow)>;
    void renderRow(const Canvas &canvas, RowCallback callback);

    // Strip rendering with per-strip callback (called from worker threads).
    // rowsPerStrip=0 renders the full canvas in one pass.
    //
    // Each strip spans the full canvas width and `rowsPerStrip` rows tall.
    // The callback receives strip-local buffers (width × rows), NOT
    // full-frame buffers — this avoids the 2 × W × H × 4 peak memory
    // spike that occurs when assembling tiles into a full frame.
    //
    // Strips may be rendered out-of-order by concurrent workers; the
    // callback is invoked from worker threads and must be thread-safe.
    //
    // Peak memory: N_workers × rowsPerStrip × width × 4.
    // Best for: striped TIFF output where the writer consumes strips
    // directly (e.g. TiffWriter::writeStrip), with no full-frame
    // intermediate buffer.
    //
    // Example (combined with TiffWriter streaming):
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
