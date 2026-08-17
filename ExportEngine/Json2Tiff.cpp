// ============================================================================
//  Json2Tiff — JSON → TIFF converter (library entry point)
// ============================================================================
//  Internal flow:
//    1. JsonSceneParser::parse()   → Canvas
//    2. ColorConverter::loadProfile() — ICC profiles
//    3. Render & write into a pluggable IRenderSink (Strip / Row / Frame)
//    4. Finalize (sink end)
//
//  Strip mode:
//    SceneRenderer::renderStrip() + sink.writeStrip()
//    Peak memory: N_workers × stripBytes + queue × stripBytes
//
//  Row mode:
//    SceneRenderer::renderRow() + sink.writeRow()
//    Peak memory: N_workers × width × 4
// ============================================================================

#include "Json2Tiff.h"

#include "SceneData.h"
#include "ColorConverter.h"
#include "RenderContext.h"
#include "JsonSceneParser.h"
#include "SceneRenderer.h"
#include "TiffWriter.h"
#include "TiffSink.h"
#include "TiffHelper.h"
#include "NearestResampler.h"
#include "ProgressManager.h"
#include "Version.h"
#include "Log.h"

#include <vips/vips.h>

#include <tiffio.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace ATHC::EE {

// ── Thread-safe, throttled progress reporter ────────────────────────────
// Strip/tile callbacks and the image pre-decode phase run on worker threads;
// this serializes progress invocation (consumers assume single-threaded
// callbacks) and collapses repeated calls into at most one per percent
// change, bounding log volume and GUI refresh.
class ProgressReporter
{
public:
    explicit ProgressReporter(ProgressCallback cb) : m_cb(std::move(cb)) { }

    // Report unless the whole-run percentage is unchanged (throttle).
    void report(int cur, int total, const std::string &msg)
    {
        int pct = (total > 0) ? (cur * 100 / total) : 0;
        std::lock_guard<std::mutex> lock(m_mtx);
        if (pct == m_lastPct)
            return;
        m_lastPct = pct;
        EELog::info("[{}/{}] {}", cur, total, msg);
        if (m_cb)
            m_cb(cur, total, msg);
    }

    // Always emit (final milestone) and let the next report through.
    void reportFinal(int cur, int total, const std::string &msg)
    {
        std::lock_guard<std::mutex> lock(m_mtx);
        m_lastPct = -1;
        EELog::info("[{}/{}] {}", cur, total, msg);
        if (m_cb)
            m_cb(cur, total, msg);
    }

private:
    ProgressCallback m_cb;
    std::mutex m_mtx;
    int m_lastPct = -1;
};

// =========================================================================
//  renderToSink — drive the renderer and stream output into a pluggable sink
// =========================================================================

static bool renderToSink(const Canvas &canvas, IColorConverter &color, IResampler &resampler,
    IRenderSink &sink, const std::function<void(int done, int total)> &unitProgress,
    const std::function<void(std::error_code, const std::string &)> &reportError)
{
    SinkDescriptor desc;
    desc.width = canvas.width;
    desc.height = canvas.height;
    desc.dpi = canvas.dpi;
    desc.samplesPerPixel = canvas.samplesPerPixel;
    desc.sampleInfo = canvas.sampleInfo;

    if (!sink.begin(desc)) {
        reportError(sink.errorCode(), "sink begin");
        return false;
    }

    std::atomic<bool> failed{ false };
    std::atomic<int> completed{ 0 };
    auto stop = [&] { return failed.load(std::memory_order_acquire); };

    {
        SceneRenderer renderer;
        renderer.setConverter(&color);
        renderer.setResampler(&resampler);

        switch (sink.granularity()) {
        case SinkGranularity::Frame: {
            auto result = renderer.render(canvas);
            if (!sink.writeFrame(std::move(result.cmykBuf)))
                failed.store(true, std::memory_order_release);
            break;
        }
        case SinkGranularity::Strip: {
            int rps = sink.preferredRowsPerStrip(canvas.width, canvas.height);
            if (rps <= 0)
                rps = TiffWriter::defaultRowsPerStrip(canvas.width, canvas.height);
            const int total = (canvas.height + rps - 1) / rps;
            renderer.renderStrip(
                canvas, rps,
                [&](int y, int rows, std::vector<uint8_t> buf) {
                    if (stop())
                        return;
                    if (!sink.writeStrip(y, rows, std::move(buf))) {
                        failed.store(true, std::memory_order_release);
                        return;
                    }
                    if (unitProgress)
                        unitProgress(++completed, total);
                },
                sink.requiresOrderedInput());
            break;
        }
        case SinkGranularity::Row: {
            const int total = canvas.height;
            renderer.renderRow(canvas, [&](int row, std::vector<uint8_t> buf) {
                if (stop())
                    return;
                if (!sink.writeRow(row, std::move(buf))) {
                    failed.store(true, std::memory_order_release);
                    return;
                }
                if (unitProgress)
                    unitProgress(++completed, total);
            });
            break;
        }
        }
    }

    if (stop() || !sink.end()) {
        reportError(sink.errorCode(), "sink write");
        return false;
    }
    return true;
}

// =========================================================================
//  runPipeline — shared pipeline: parse → pre-scan → ICC → pre-decode →
//  renderToSink. Both json2tiff and json2sink funnel through here.
// =========================================================================

static bool runPipeline(const std::string &jsonPath, const RenderServices &services,
    ProgressCallback progress, ErrorCallback onError, const std::string &finalMessage)
{
    static std::once_flag s_initVips;
    std::call_once(s_initVips, [] { vips_init("ExportEngine"); });

    // ── Helpers ──────────────────────────────────────────────────────
    // 错误只在边界记录一次：EELog::error + onError(ec)。
    // context 只进日志（如文件路径/名称），错误码是宿主编程式分类的载体。
    auto reportError = [&](std::error_code ec, const std::string &context = std::string()) {
        if (context.empty())
            EELog::error("json2tiff: {}", ec.message());
        else
            EELog::error("json2tiff: {} ({})", context, ec.message());
        if (onError)
            onError(ec);
        return false;
    };

    ProgressReporter reporter(progress);

    EELog::info("Json2Tiff  ExportEngine {}", Version::full());

    // ── Validate output sink ─────────────────────────────────────────
    if (!services.sink) {
        reportError(EEError::io_failed, "no output sink in RenderServices");
        return false;
    }
    IRenderSink &sink = *services.sink;

    // ── Validate input ───────────────────────────────────────────────
    if (!fs::exists(TiffHelper::Utf8ToWide(jsonPath))) {
        reportError(EEError::parse_cannot_open_file, "JSON file not found: " + jsonPath);
        return false;
    }

    // ── [0→4%] Parse JSON ────────────────────────────────────────────
    reporter.report(0, 100, "Parsing JSON...");

    Canvas canvas;
    JsonSceneParser parser;
    auto ec = parser.parse(jsonPath, canvas);
    if (ec) {
        reportError(ec, "Parse error");
        return false;
    }

    EELog::info("  {}×{} px @ {} DPI", canvas.width, canvas.height, canvas.dpi.x);
    EELog::info("  {} rects, {} circles, {} lines, {} freelines, {} beziers, {} texts, {} images",
        canvas.rects.size(), canvas.circles.size(), canvas.lines.size(), canvas.freeLines.size(),
        canvas.bezierCurves.size(), canvas.texts.size(), canvas.images.size());

    // Pre-scan images in z-order:
    //   First multi-channel image (lowest z) determines output samplesPerPixel.
    //   Uses libtiff for TIFF files (reads only header tags, zero pixel decode),
    //   falls back to VIPS for other formats.
    {
        // Build a z-sorted index array matching render-time draw order
        std::vector<size_t> sortedIdx(canvas.images.size());
        for (size_t i = 0; i < sortedIdx.size(); ++i)
            sortedIdx[i] = i;
        std::sort(sortedIdx.begin(), sortedIdx.end(),
            [&](size_t a, size_t b) { return canvas.images[a].z < canvas.images[b].z; });

        for (size_t idx : sortedIdx) {
            const auto &img = canvas.images[idx];
            if (img.filePath.empty())
                continue;

            if (canvas.samplesPerPixel != 4)
                continue; // already determined by a lower-z image

            // ── Try libtiff first (header-only, no pixel decode) ──────
            bool scanned = false;
            {
                auto prevErr = TIFFSetErrorHandler(nullptr);
                auto prevWarn = TIFFSetWarningHandler(nullptr);
                TIFF *tif = TiffHelper::openTiff(img.filePath, "r");
                TIFFSetErrorHandler(prevErr);
                TIFFSetWarningHandler(prevWarn);

                if (tif) {
                    uint16_t photo = 0;
                    uint16_t extrasCount = 0;
                    uint16_t *extraTypes = nullptr;
                    if (TIFFGetField(tif, TIFFTAG_PHOTOMETRIC, &photo)
                        && photo == PHOTOMETRIC_SEPARATED
                        && TIFFGetField(tif, TIFFTAG_EXTRASAMPLES, &extrasCount, &extraTypes)
                        && extrasCount > 0 && extraTypes) {
                        canvas.samplesPerPixel = 4 + extrasCount;
                        canvas.sampleInfo.assign(extraTypes, extraTypes + extrasCount);
                    }
                    TIFFClose(tif);
                    scanned = true;
                }
            }

            // ── Fallback: VIPS for non-TIFF formats ──────────────────
            if (!scanned) {
                VipsImage *vipsImg = vips_image_new_from_file(
                    img.filePath.c_str(), "access", VIPS_ACCESS_SEQUENTIAL, nullptr);
                if (vipsImg) {
                    if (vips_image_get_interpretation(vipsImg) == VIPS_INTERPRETATION_CMYK) {
                        int ch = vips_image_get_bands(vipsImg);
                        if (ch > 4) {
                            canvas.samplesPerPixel = ch;
                            uint16_t extras = static_cast<uint16_t>(ch - 4);
                            canvas.sampleInfo.assign(extras, EXTRASAMPLE_UNSPECIFIED);
                        }
                    }
                    g_object_unref(vipsImg);
                } else {
                    vips_error_clear(); // skip unreadable images
                }
            }
        }

        if (canvas.samplesPerPixel > 4)
            EELog::info("  Multi-channel output: {} samples/pixel", canvas.samplesPerPixel);
    }

    // Fallback DPI
    if (canvas.dpi.x <= 0 || canvas.dpi.y <= 0)
        canvas.dpi = Dpi{ 300, 300 };

    reporter.report(4, 100, "Parsing JSON...");

    // ── 颜色转换引擎（内部自管 ICC profile，EE 不干预）────────────────
    std::shared_ptr<IColorConverter> color = services.color;
    if (!color)
        color = ColorConverter::createDefault();

    std::shared_ptr<IResampler> resampler = services.resampler;
    if (!resampler)
        resampler = std::make_shared<NearestResampler>();

    // ── [8→75%] Pre-decode images ────────────────────────────────────
    // Decode+convert+resize every unique image needing it, in parallel,
    // before rendering — the dominant cost gets its own smooth progress
    // counter and the render pass never blocks on a first-touch decode.
    // 预解码并行后进度回调可从多个 worker 线程并发写——必须原子
    std::atomic<int> decodeTotal{ -1 }; // -1 = phase skipped (nothing to pre-decode)
    {
        SceneRenderer renderer;
        renderer.setConverter(color.get());
        renderer.setResampler(resampler.get());
        renderer.preDecodeImages(canvas, [&](int done, int total, const std::string &path) {
            decodeTotal = total;
            int cur = 8 + (done * 67) / std::max(total, 1);
            auto name = fs::path(TiffHelper::Utf8ToWide(path)).filename().u8string();
            if (name.empty())
                name = path;
            reporter.report(cur, 100,
                "Decoding images (" + std::to_string(done) + "/" + std::to_string(total)
                    + "): " + name);
        });
    }

    // ── [75→100%] Render & write ─────────────────────────────────────
    // No pre-decode phase → render spans the full remaining range.
    const int renderStart = (decodeTotal > 0) ? 75 : 8;
    const char *unitName = (sink.granularity() == SinkGranularity::Row) ? "row" : "strip";
    auto unitProgress = [&](int done, int total) {
        int cur = renderStart + (done * (100 - renderStart)) / std::max(total, 1);
        reporter.report(cur, 100,
            "Rendering: " + std::string(unitName) + " " + std::to_string(done) + "/"
                + std::to_string(total));
    };

    if (!renderToSink(canvas, *color, *resampler, sink, unitProgress, reportError))
        return false;

    // ── 100% Done ────────────────────────────────────────────────────
    reporter.reportFinal(100, 100, finalMessage);
    return true;
}

// =========================================================================
//  json2sink — public API
// =========================================================================

bool json2sink(const std::string &jsonPath, const RenderServices &services,
    ProgressCallback progress, ErrorCallback onError)
{
    return runPipeline(jsonPath, services, progress, onError, "Exported");
}

// =========================================================================
//  json2tiff — public API (thin wrapper: TiffSink + runPipeline)
// =========================================================================

bool json2tiff(const std::string &jsonPath, const std::string &tiffPath, ProgressCallback progress,
    ErrorCallback onError)
{
    RenderServices services;
    services.sink = std::make_shared<TiffSink>(tiffPath);
    if (!runPipeline(jsonPath, services, progress, onError, "Exported: " + tiffPath))
        return false;

    auto fileSize = fs::file_size(TiffHelper::Utf8ToWide(tiffPath));
    EELog::info("  Output: {} ({:.1f} MB)", tiffPath, fileSize / 1'048'576.0);
    return true;
}

} // namespace ATHC::EE
