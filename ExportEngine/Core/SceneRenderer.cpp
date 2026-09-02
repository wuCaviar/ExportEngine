#include "SceneRenderer.h"
#include "IColorConverter.h"
#include "IResampler.h"
#include "RenderContext.h"
#include "RectRenderer.h"
#include "CircleRenderer.h"
#include "PathRenderer.h"
#include "ImageRenderer.h"
#include "TextRenderer.h"
#include "TextureSource.h"
#include "RenderMath.h"
#include "PredecodeScheduler.h"
#include "FileUtil.h"

#include <algorithm>
#include <tuple>
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>
#include <unordered_set>
#include <cmath>
#include <cctype>
#include <filesystem>
#include <tiffio.h>
#include <vips/vips.h>

using namespace ATHC::EE;

// ============================================================
//  Constructor / Destructor
// ============================================================

SceneRenderer::SceneRenderer()  = default;
SceneRenderer::~SceneRenderer() = default;

void SceneRenderer::setConverter(IColorConverter *converter)
{
    m_converter = converter;
}

void SceneRenderer::setResampler(IResampler *resampler)
{
    m_resampler = resampler;
}

// ============================================================
//  BBox computation — conservative axis-aligned bounds for culling
// ============================================================

namespace {

int roundUp(double v)
{
    return static_cast<int>(std::ceil(v));
}
int roundDn(double v)
{
    return static_cast<int>(std::floor(v));
}

} // anonymous namespace

DrawBBox SceneRenderer::computeBBox(DrawType type, const void *prim, const Canvas *canvasHint)
{
    DrawBBox bb;
    double   xMin = 0, xMax = 0, yMin = 0, yMax = 0;

    switch (type) {
    case DrawType::RECT: {
        auto &r = *static_cast<const Rect *>(prim);
        if (r.width <= 0 || r.height <= 0)
            return bb;
        xMin      = r.x;
        xMax      = r.x + r.width;
        yMin      = r.y;
        yMax      = r.y + r.height;
        double sw = r.strokeWidth * 0.5;
        xMin -= sw;
        xMax += sw;
        yMin -= sw;
        yMax += sw;
        break;
    }
    case DrawType::CIRCLE: {
        auto  &c = *static_cast<const Circle *>(prim);
        double r = std::max(c.radiusX, c.radiusY) + c.strokeWidth;
        xMin     = c.cx - r;
        xMax     = c.cx + r;
        yMin     = c.cy - r;
        yMax     = c.cy + r;
        break;
    }
    case DrawType::LINE: {
        auto  &l   = *static_cast<const Line *>(prim);
        double sw2 = l.strokeWidth * 0.5 + 1.0;
        xMin       = std::min(l.x1, l.x2) - sw2;
        xMax       = std::max(l.x1, l.x2) + sw2;
        yMin       = std::min(l.y1, l.y2) - sw2;
        yMax       = std::max(l.y1, l.y2) + sw2;
        break;
    }
    case DrawType::FREELINE: {
        auto &fl = *static_cast<const FreeLine *>(prim);
        if (fl.points.empty())
            return bb;
        double sw2 = fl.strokeWidth * 0.5 + 1.0;
        xMin       = fl.points[0].first;
        xMax       = fl.points[0].first;
        yMin       = fl.points[0].second;
        yMax       = fl.points[0].second;
        for (auto &p : fl.points) {
            if (p.first < xMin)
                xMin = p.first;
            if (p.first > xMax)
                xMax = p.first;
            if (p.second < yMin)
                yMin = p.second;
            if (p.second > yMax)
                yMax = p.second;
        }
        xMin -= sw2;
        xMax += sw2;
        yMin -= sw2;
        yMax += sw2;
        break;
    }
    case DrawType::BEZIER: {
        auto &bc = *static_cast<const BezierCurve *>(prim);
        if (bc.controlPoints.empty())
            return bb;
        double sw2 = bc.strokeWidth * 0.5 + 1.0;
        xMin       = bc.controlPoints[0].first;
        xMax       = bc.controlPoints[0].first;
        yMin       = bc.controlPoints[0].second;
        yMax       = bc.controlPoints[0].second;
        for (auto &p : bc.controlPoints) {
            if (p.first < xMin)
                xMin = p.first;
            if (p.first > xMax)
                xMax = p.first;
            if (p.second < yMin)
                yMin = p.second;
            if (p.second > yMax)
                yMax = p.second;
        }
        xMin -= sw2;
        xMax += sw2;
        yMin -= sw2;
        yMax += sw2;
        break;
    }
    case DrawType::TEXT: {
        auto  &t       = *static_cast<const Text *>(prim);
        int    dpiX    = canvasHint ? canvasHint->dpi.x : 72;
        int    dpiY    = canvasHint ? canvasHint->dpi.y : 72;
        // TextRenderer lays out on a 96 DPI reference basis and scales the
        // result by the canvas DPI, so a pt of type sits ~dpi/72 device px
        // per axis (x/y independent for anisotropic DPI).
        double pxSizeX = t.fontSize * static_cast<double>(dpiX) / 72.0;
        double pxSizeY = t.fontSize * static_cast<double>(dpiY) / 72.0;
        // TextRenderer places the layout ink with its top-left at (t.x, t.y)
        // and the ink extends downward/rightward, so the culling box must
        // cover [t.y, t.y + inkH]. With width/height set, the ink fills the
        // scaled target box but may exceed it slightly (italic overhang,
        // descenders) — pad the far edges by ~0.3em. Without width/height,
        // estimate the natural size: ≤ ~0.75em per char of the longest
        // line, ≤ ~1.3em per explicit line.
        double pad = std::max(pxSizeX, pxSizeY) * 0.3;
        xMin       = t.x;
        yMin       = t.y;
        if (t.width > 0.0) {
            xMax = t.x + t.width + pad;
        } else {
            size_t maxLen = 0, cur = 0;
            for (char c : t.content) {
                if (c == '\n') {
                    maxLen = std::max(maxLen, cur);
                    cur    = 0;
                } else {
                    ++cur;
                }
            }
            maxLen = std::max(maxLen, cur);
            xMax   = t.x + pxSizeX * 0.75 * static_cast<double>(maxLen) + pad;
        }
        if (t.height > 0.0) {
            yMax = t.y + t.height + pad;
        } else {
            size_t lines = 1 + static_cast<size_t>(
                                  std::count(t.content.begin(), t.content.end(), '\n'));
            yMax = t.y + pxSizeY * 1.3 * static_cast<double>(lines) + pad;
        }
        break;
    }
    case DrawType::IMAGE: {
        auto &img = *static_cast<const ImageItem *>(prim);
        // width/height=0 means auto-calculated from source DPI — unbounded bbox
        if (img.width <= 0 || img.height <= 0)
            return bb; // valid=false → always render
        xMin = img.x;
        yMin = img.y;
        xMax = img.x + img.width;
        yMax = img.y + img.height;
        break;
    }
    }

    if (xMax - xMin < 0 || yMax - yMin < 0)
        return bb;

    bb.x0    = roundDn(xMin);
    bb.y0    = roundDn(yMin);
    bb.x1    = roundUp(xMax);
    bb.y1    = roundUp(yMax);
    bb.valid = true;
    return bb;
}

// ============================================================
//  Draw call collection and sorting
// ============================================================

std::vector<SceneRenderer::DrawCall> SceneRenderer::buildDrawCalls(const Canvas &canvas)
{
    std::vector<DrawCall> calls;
    calls.reserve(canvas.rects.size() + canvas.circles.size() + canvas.freeLines.size()
                  + canvas.bezierCurves.size() + canvas.lines.size() + canvas.texts.size()
                  + canvas.images.size());

#define PUSH_CALL(type, vec, primPtr)                                   \
    for (size_t i = 0; i < (vec).size(); ++i) {                         \
        DrawCall call{ (vec)[i].z, DrawType::type, i,                   \
                       computeBBox(DrawType::type, primPtr, &canvas) }; \
        calls.push_back(call);                                          \
    }

    PUSH_CALL(RECT, canvas.rects, &canvas.rects[i]);
    PUSH_CALL(CIRCLE, canvas.circles, &canvas.circles[i]);
    PUSH_CALL(FREELINE, canvas.freeLines, &canvas.freeLines[i]);
    PUSH_CALL(BEZIER, canvas.bezierCurves, &canvas.bezierCurves[i]);
    PUSH_CALL(LINE, canvas.lines, &canvas.lines[i]);
    PUSH_CALL(TEXT, canvas.texts, &canvas.texts[i]);
    PUSH_CALL(IMAGE, canvas.images, &canvas.images[i]);
#undef PUSH_CALL

    std::sort(calls.begin(), calls.end(),
              [](const DrawCall &a, const DrawCall &b) { return a.z < b.z; });

    return calls;
}

// ============================================================
//  Dispatch to per-primitive renderers
// ============================================================

void SceneRenderer::dispatchDraw(
    DrawType type, size_t index, const Canvas &canvas, RenderContext &ctx)
{
    switch (type) {
    case DrawType::RECT:
        RectRenderer::draw(ctx, canvas.rects[index], canvas.dpi);
        break;
    case DrawType::CIRCLE:
        CircleRenderer::draw(ctx, canvas.circles[index]);
        break;
    case DrawType::FREELINE:
        PathRenderer::drawFreeLine(ctx, canvas.freeLines[index]);
        break;
    case DrawType::BEZIER:
        PathRenderer::drawBezier(ctx, canvas.bezierCurves[index]);
        break;
    case DrawType::LINE:
        PathRenderer::drawLine(ctx, canvas.lines[index]);
        break;
    case DrawType::TEXT:
        TextRenderer::draw(ctx, canvas.texts[index], canvas.dpi);
        break;
    case DrawType::IMAGE:
        ImageRenderer::draw(ctx, canvas.images[index], canvas.dpi);
        break;
    }
}

// ============================================================
//  Tile rendering — bbox-culled dispatch
// ============================================================

void SceneRenderer::renderTile(const std::vector<DrawCall> &calls,
                               const Canvas                &canvas,
                               RenderContext               &ctx)
{
    int tx = ctx.tileX, ty = ctx.tileY, tw = ctx.tileW, th = ctx.tileH;
    if (tw <= 0) {
        tw = ctx.canvasWidth;
        th = ctx.canvasHeight;
    }

    for (auto &call : calls) {
        if (!call.bbox.intersects(tx, ty, tw, th))
            continue;
        dispatchDraw(call.type, call.index, canvas, ctx);
    }
}

// ============================================================
//  Full-canvas render (sequential)
// ============================================================

RenderResult SceneRenderer::render(const Canvas &canvas)
{
    RenderResult result;
    result.width  = canvas.width;
    result.height = canvas.height;

    RenderContext ctx;
    ctx.converter       = m_converter;
    ctx.resampler       = m_resampler;
    ctx.samplesPerPixel = canvas.samplesPerPixel;
    ctx.sampleInfo      = canvas.sampleInfo;
    ctx.initFullCanvas(canvas.width, canvas.height);
    ctx.fillBackground(canvas.background);

    auto calls = buildDrawCalls(canvas);
    renderTile(calls, canvas, ctx);

    result.cmykBuf = std::move(ctx.cmykBuf);
    return result;
}

// ============================================================
//  Pre-decode — budget-constrained early validation + texture warming
//  Image sources are lazy now (DecodedSource streams per band at render
//  time), so pre-decode only probes their header/transform to mark bad files
//  once. Texture fills still warm TextureSource's frame cache. Work is
//  grouped into cost-budgeted waves (schedulePredecodeWaves) so parallel
//  peak memory stays bounded.
// ============================================================

// ── Pre-decode scheduling helpers ─────────────────────────────────────

// Cheap header-only probe of an image's source size + channel count (mirrors
// the metadata phase of openRasterSource). Image sources decode lazily at
// render time (DecodedSource streams per band — no full frame), so the cost
// is just the source-resolution transient. Unreadable images get cost 1 —
// the probe fails fast and the file is marked bad.
static void probeDecodeCost(const std::string &filePath, const ImageItem *img, uint64_t &cost)
{
    uint64_t w = 0, h = 0, chans = 7;
    {
        auto  prevErr  = TIFFSetErrorHandler(nullptr);
        auto  prevWarn = TIFFSetWarningHandler(nullptr);
        TIFF *tif      = FileUtil::openTiff(filePath, "r");
        TIFFSetErrorHandler(prevErr);
        TIFFSetWarningHandler(prevWarn);
        if (tif) {
            uint32_t iw = 0, ih = 0;
            uint16_t photo = 0;
            TIFFGetField(tif, TIFFTAG_IMAGEWIDTH, &iw);
            TIFFGetField(tif, TIFFTAG_IMAGELENGTH, &ih);
            if (TIFFGetField(tif, TIFFTAG_PHOTOMETRIC, &photo)) {
                if (photo == PHOTOMETRIC_SEPARATED)
                    chans = 4;
                else if (photo == PHOTOMETRIC_MINISWHITE || photo == PHOTOMETRIC_MINISBLACK)
                    chans = 5;
            }
            TIFFClose(tif);
            w = iw;
            h = ih;
        }
    }
    if (w == 0 || h == 0) {
        VipsImage *vimg =
            vips_image_new_from_file(filePath.c_str(), "access", VIPS_ACCESS_SEQUENTIAL, nullptr);
        if (!vimg) {
            vips_error_clear();
            cost = 1;
            return;
        }
        w           = static_cast<uint64_t>(vips_image_get_width(vimg));
        h           = static_cast<uint64_t>(vips_image_get_height(vimg));
        auto interp = vips_image_get_interpretation(vimg);
        if (interp == VIPS_INTERPRETATION_CMYK)
            chans = 4;
        else if (interp == VIPS_INTERPRETATION_B_W || interp == VIPS_INTERPRETATION_GREY16)
            chans = 5;
        g_object_unref(vimg);
        if (w <= 0 || h <= 0) {
            cost = 1;
            return;
        }
    }

    // 纹理按源分辨率缓存 CMYKA 帧（≤256 MB）；图片惰性解码，只计源瞬态。
    cost = estimateDecodeCost(w, h, img ? chans : 5);
}

void SceneRenderer::preDecodeImages(const Canvas &canvas, DecodeProgress progress)
{
    // Dedupe by file path; skip images whose bbox cannot intersect the
    // canvas (auto-sized images have an unbounded bbox and are kept) —
    // mirrors renderTile's culling.
    std::vector<const ImageItem *> images;
    {
        std::unordered_set<std::string> seen;
        for (const auto &img : canvas.images) {
            if (img.filePath.empty() || !seen.insert(img.filePath).second)
                continue;
            DrawBBox bb = computeBBox(DrawType::IMAGE, &img, &canvas);
            if (bb.valid && !bb.intersects(0, 0, canvas.width, canvas.height))
                continue;
            images.push_back(&img);
        }
    }

    // Rect texture fills: warm TextureSource's shared frame cache too
    // (deduplicated by file path only).
    std::vector<std::tuple<std::string, int, int>> textures; // path, overrideW, overrideH
    {
        std::unordered_set<std::string> seen;
        for (const auto &rect : canvas.rects) {
            if (!rect.textureFill || rect.textureFill->filePath.empty())
                continue;
            const TextureFill &tf = *rect.textureFill;
            int                ow = tf.useOriginalSize ? 0 : static_cast<int>(tf.customWidth + 0.5);
            int         oh  = tf.useOriginalSize ? 0 : static_cast<int>(tf.customHeight + 0.5);
            std::string key = tf.filePath; // 缓存键仅按文件路径（见 Task 3 Step 4 要求 3）
            if (seen.insert(key).second)
                textures.emplace_back(tf.filePath, ow, oh);
        }
    }

    struct PredecodeItem
    {
        enum class Kind
        {
            Image,
            Texture
        };
        Kind             kind;
        const ImageItem *img = nullptr;  // Kind::Image
        std::string      path;           // Kind::Texture
        int              ow = 0, oh = 0; // 纹理 tile 尺寸覆盖
        uint64_t         cost = 0;
    };

    std::vector<PredecodeItem> items;
    {
        // 图片预校验顺序不影响结果（源惰性解码、无整帧缓存）；保留倒序遍历
        // 以保持与历史版本一致的进度/日志顺序。
        for (auto it = images.rbegin(); it != images.rend(); ++it) {
            uint64_t cost = 0;
            probeDecodeCost((*it)->filePath, *it, cost);
            items.push_back({ PredecodeItem::Kind::Image, *it, {}, 0, 0, cost });
        }
        for (const auto &[path, ow, oh] : textures) {
            uint64_t cost = 0;
            probeDecodeCost(path, nullptr, cost);
            items.push_back({ PredecodeItem::Kind::Texture, nullptr, path, ow, oh, cost });
        }
    }

    const int total = static_cast<int>(items.size());
    if (total == 0)
        return; // nothing to pre-decode — caller keeps the previous range

    // Decode budget: 4 GB transient decode buffers (image probes are cheap;
    // texture frames account separately in TextureSource's own cache).
    // Items costing more than the budget run alone (serial) — peak memory
    // never exceeds one decode.
    constexpr uint64_t kDefaultBudgetBytes = 4ULL * 1024 * 1024 * 1024;
    uint64_t           budget              = kDefaultBudgetBytes;

    unsigned nWorkers = std::thread::hardware_concurrency();
    if (nWorkers < 1)
        nWorkers = 1;

    std::vector<uint64_t> costs;
    costs.reserve(items.size());
    for (const auto &it : items)
        costs.push_back(it.cost);
    const auto waves = schedulePredecodeWaves(costs, budget, nWorkers);

    // Wave 内并行、wave 间串行：并行度与峰值内存都被预算约束；wave 顺序
    // 保持倒序语义（band 过渡时最多重解码一张图，与串行版一致）。
    std::atomic<int> done{ 0 };
    for (const auto &wave : waves) {
        std::vector<std::thread> threads;
        threads.reserve(wave.size());
        for (size_t idx : wave) {
            threads.emplace_back([&, idx] {
                const PredecodeItem &it = items[idx];
                if (it.kind == PredecodeItem::Kind::Image) {
                    // 失败在内部处理（标记 bad file，渲染期跳过）
                    ImageRenderer::preDecode(*it.img, canvas.dpi, m_converter, m_resampler);
                    if (progress)
                        progress(++done, total, it.img->filePath);
                } else {
                    TextureSource::preDecode(it.path, canvas.dpi, m_converter, it.ow, it.oh);
                    if (progress)
                        progress(++done, total, it.path);
                }
            });
        }
        for (auto &t : threads)
            t.join();
    }
}

// ============================================================
//  Row rendering — per-row worker pool
//  Each work unit is one full-width row. Reuses renderStrip with
//  rowsPerStrip=1 (band-serial scheduling, rows may arrive out-of-order).
//  Callback invoked from worker threads directly (no serialization).
// ============================================================

void SceneRenderer::renderRow(const Canvas &canvas, RowCallback callback)
{
    if (canvas.height <= 0)
        return;
    renderStrip(canvas, 1, [&](int startRow, int rows, std::vector<uint8_t> cmyk) {
        // rowsPerStrip=1 → rows 恒为 1
        (void)rows;
        callback(startRow, std::move(cmyk));
    });
}

// ============================================================
//  Strip rendering — parallel worker pool
//  Callback invoked from worker threads directly (no serialization).
//
//  Each work unit is a full-width strip (rowsPerStrip rows tall).
//  The callback receives strip-local buffers (width × rows), so the
//  caller never needs to hold the full frame in memory.
// ============================================================

void SceneRenderer::renderStrip(const Canvas &canvas,
                                int           rowsPerStrip,
                                StripCallback callback,
                                bool          ordered)
{
    if (rowsPerStrip <= 0) {
        // Fall back to full-canvas render as a single strip.
        RenderResult result = render(canvas);
        callback(0, canvas.height, std::move(result.cmykBuf));
        return;
    }

    auto allCalls = buildDrawCalls(canvas);

    // Collect strip coordinates. Each strip spans the full canvas width
    // and at most `rowsPerStrip` rows (the last strip may be shorter).
    struct StripCoord
    {
        int startRow;
        int rows;
    };
    std::vector<StripCoord> strips;
    strips.reserve((canvas.height + rowsPerStrip - 1) / rowsPerStrip);
    for (int y = 0; y < canvas.height; y += rowsPerStrip) {
        int rows = std::min(rowsPerStrip, canvas.height - y);
        strips.push_back({ y, rows });
    }

    unsigned nWorkers = ordered ? 1u : std::thread::hardware_concurrency();
    if (nWorkers < 1)
        nWorkers = 1;

    // ── Band-serial scheduling ───────────────────────────────────────────
    // Strips are grouped into vertical bands of nWorkers consecutive strips;
    // bands run serially top-down while strips within a band run in parallel.
    // A strip only reads the images overlapping its rows, and each image
    // streams just the source rows it needs (DecodedSource is lazy), so the
    // active memory stays a narrow vertical window (~band height × row width)
    // regardless of how large the whole resampled image would be.
    const size_t nStrips = strips.size();
    size_t       band    = 0;
    while (band < nStrips) {
        const size_t   bandEnd = std::min(nStrips, band + nWorkers);
        const unsigned bandWorkers =
            static_cast<unsigned>(std::min(nWorkers, static_cast<unsigned>(bandEnd - band)));

        std::atomic<size_t> nextIdx{ band };

        auto worker = [&]() {
            // Per-thread lcms2 converter clone — cmsHTRANSFORM is not
            // thread-safe, so each worker gets its own transform.
            auto threadConverter = m_converter ? m_converter->cloneForThread() : nullptr;
            IColorConverter *cv  = threadConverter ? threadConverter.get() : m_converter;

            for (;;) {
                const size_t idx = nextIdx.fetch_add(1);
                if (idx >= bandEnd)
                    return;
                const auto &s = strips[idx];

                // Initialize a strip-windowed render context. The window is
                // (tileX=0, tileY=s.startRow, tileW=canvas.width, tileH=s.rows).
                // The private renderTile() helper does bbox-culled dispatch
                // over any rectangular window, so it works for strips too.
                RenderContext ctx;
                ctx.converter       = cv;
                ctx.resampler       = m_resampler;
                ctx.samplesPerPixel = canvas.samplesPerPixel;
                ctx.sampleInfo      = canvas.sampleInfo;
                ctx.initTile(canvas.width, canvas.height, 0, s.startRow, canvas.width, s.rows);
                ctx.fillBackground(canvas.background);

                renderTile(allCalls, canvas, ctx);

                // Callback invoked from worker thread — caller must be
                // thread-safe. ctx.cmykBuf is strip-local.
                callback(s.startRow, s.rows, std::move(ctx.cmykBuf));
            }
        };

        std::vector<std::thread> threads;
        for (unsigned i = 0; i < bandWorkers; ++i)
            threads.emplace_back(worker);
        for (auto &t : threads)
            t.join();

        band = bandEnd;
    }
}
