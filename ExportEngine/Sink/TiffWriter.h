#pragma once

#include "EEError.h"
#include "SceneData.h"

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

// Forward declare libtiff opaque type
using TIFF = struct tiff;

namespace ATHC::EE {

// ============================================================================
//  TiffWriter
// ============================================================================
//
//  CMYK TIFF writer with strip-streaming, built on a producer-consumer
//  pipeline:
//
//    Strip-streaming — beginStripWrite / writeStrip / endStripWrite
//      For renderStrip output: the renderer delivers strip-local CMYK
//      buffers; the writer moves each into a queue-owned buffer and enqueues
//      it verbatim (zero compositing, zero copy). TIFF storage is striped.
//
//  Shared:
//    - A bounded blocking queue (kQueueCapacity entries) that provides
//      natural backpressure when the I/O thread falls behind.
//    - A single background I/O thread that owns all libtiff writes
//      (libtiff is single-writer per TIFF handle).
//    - First-error-wins error propagation via m_error / m_ioError.
//
//  Peak memory: kQueueCapacity × stripBytes + N_producers × stripBytes
//  where N_producers = number of caller threads concurrently in writeStrip
//  (capped implicitly by the bounded queue).
//
// ============================================================================
//  Usage examples
// ============================================================================
//
//  ----------------------------------------------------------------------------
//  Example 1 — Strip streaming, single producer (renderStrip)
//  ----------------------------------------------------------------------------
//  TiffWriter w;
//
//  if (!w.beginStripWrite("out.tif", canvasW, canvasH, Dpi{300, 300}, rps, iccBytes))
//      handle_error(w.errorCode().message());
//
//  // Caller chooses strip granularity. TiffWriter auto-computes a sensible
//  // default (defaultRowsPerStrip helper) targeting ~8 MB per strip; you may
//  // pass your own rowsPerStrip to beginStripWrite if needed.
//  const int rps = TiffWriter::defaultRowsPerStrip(canvasW, canvasH);
//  for (int y = 0; y < canvasH; y += rps) {
//      int rows = std::min(rps, canvasH - y);
//      // ... render this strip into cmykBuf (rows×canvasW×4 bytes) ...
//      if (!w.writeStrip(y, rows, std::move(cmykBuf))) {
//          handle_error(w.errorCode().message());
//          break;
//      }
//  }
//
//  if (!w.endStripWrite())
//      handle_error(w.errorCode().message());
//
//  ----------------------------------------------------------------------------
//  Example 2 — Strip streaming, parallel producers (manual thread pool)
//  ----------------------------------------------------------------------------
//  TiffWriter w;
//  w.beginStripWrite("out.tif", W, H, Dpi{300, 300}, rps, iccBytes);
//
//  std::atomic<int> nextStrip{0};
//  std::atomic<bool> failed{false};
//
//  auto worker = [&]() {
//      while (true) {
//          if (failed.load()) return;
//          int s = nextStrip.fetch_add(1);
//          if (s >= stripCount) return;
//          int y = s * rps;
//          int rows = std::min(rps, H - y);
//          if (!w.writeStrip(y, rows, std::move(cmykBuf))) {
//              failed.store(true);
//              return;
//          }
//      }
//  };
//
//  std::vector<std::thread> pool;
//  for (unsigned i = 0; i < std::thread::hardware_concurrency(); ++i)
//      pool.emplace_back(worker);
//  for (auto &t : pool) t.join();
//
//  w.endStripWrite();
//  // Notes:
//  //   - writeStrip() is safe to call concurrently from multiple threads.
//  //   - The bounded I/O queue applies backpressure: if the I/O thread is
//  //     slower than the producers, writeStrip() blocks instead of growing
//  //     memory unbounded.
//  //   - Strips may be written out-of-order — libtiff accepts arbitrary
//  //     strip indices and finalizes the directory at close time.
//
// ============================================================================
class TiffWriter
{
public:
    TiffWriter() = default;
    ~TiffWriter();

    // ========================================================================
    //  Strip-streaming API  (see Example 1 & 2 above)
    // ========================================================================
    //
    //  beginStripWrite opens the TIFF, writes header tags + strip geometry
    //  (rowsPerStrip rows per strip) + ICC, and starts the background I/O
    //  consumer thread.
    //
    //  writeStrip takes ownership of one strip (move, no compositing or
    //  conversion — the buffer is already final CMYK). Safe to call from
    //  multiple threads concurrently (out-of-order OK).
    //
    //  **IMPORTANT: strip-local buffers**
    //    cmykBuf:   strip-local CMYK data, moved in. Must be exactly
    //               `rows × width × samplesPerPixel` bytes, or empty to
    //               write a zeroed strip (undersized input is zero-padded).
    //    startRow:  absolute Y coordinate of the strip's top row in the
    //               canvas. Used to compute the libtiff strip index
    //               (= startRow / rowsPerStrip). Strips may arrive
    //               out-of-order — libtiff accepts arbitrary indices.
    //
    //  This signature matches the output of SceneRenderer::renderStrip(),
    //  so the two can be piped together directly with zero copy:
    //
    //    renderer.renderStrip(canvas, rps,
    //        [&](int y, int rows, std::vector<uint8_t> cmyk) {
    //            writer.writeStrip(y, rows, std::move(cmyk));
    //        });
    //
    //  endStripWrite drains + closes + tears down. Must be called exactly
    //  once per successful beginStripWrite.
    //
    //  iccBytes: ICC profile bytes to embed (empty for naive/no-ICC mode).
    [[nodiscard]] bool beginStripWrite(const std::string           &filePath,
                                       int                          width,
                                       int                          height,
                                       Dpi                          dpi,
                                       int                          rowsPerStrip,
                                       const std::vector<uint8_t>  &iccBytes,
                                       int                          samplesPerPixel = 4,
                                       const std::vector<uint16_t> &sampleInfo      = {});
    [[nodiscard]] bool writeStrip(int startRow, int rows, std::vector<uint8_t> cmykBuf);
    [[nodiscard]] bool endStripWrite();

    // Last error, first-error-wins (thread-safe; set by producers and the
    // I/O worker). Preserved until the next begin*() call.
    std::error_code errorCode() const
    {
        std::lock_guard<std::mutex> lock(m_errorMutex);
        return m_error;
    }

    // Compute a sensible default rowsPerStrip targeting ~8 MB per strip
    // (good LZW compression vs. memory trade-off). Always returns ≥1 and
    // ≤height. Use this when you don't have a specific strip size in mind.
    static int defaultRowsPerStrip(int width, int height);

private:
    // ------------------------------------------------------------------
    //  One unit of work ready for libtiff.
    //  Declared before BoundedQueue so the queue member sees a complete type.
    // ------------------------------------------------------------------
    struct WriteTask
    {
        uint32_t             index = 0; // tile or strip index
        std::vector<uint8_t> buffer;    // padded pixel data
    };

    // ------------------------------------------------------------------
    //  Bounded blocking queue for producer-consumer streaming.
    //  Caps in-flight work to limit peak memory. Producers block when full
    //  (natural backpressure); consumer blocks when empty. close() wakes
    //  everyone and rejects further pushes.
    // ------------------------------------------------------------------
    struct BoundedQueue
    {
        explicit BoundedQueue(size_t cap) : capacity(cap) { }

        // Returns true if pushed; false if queue is closed (caller should stop).
        bool push(WriteTask &&task);
        // Returns true if popped; false if closed AND empty.
        bool pop(WriteTask &out);
        void close();

        const size_t            capacity;
        std::queue<WriteTask>   q;
        std::mutex              mtx;
        std::condition_variable cvNotEmpty;
        std::condition_variable cvNotFull;
        bool                    closed = false;
    };

    // Session mode — distinguishes an active session from none.
    enum class SessionMode : uint8_t
    {
        None,
        Strip,
    };

    // ------------------------------------------------------------------
    //  Internal helpers
    // ------------------------------------------------------------------

    // Set tags common to the TIFF write path
    static void setupTiffCommon(TIFF                        *tif,
                                int                          width,
                                int                          height,
                                Dpi                          dpi,
                                int                          samplesPerPixel,
                                const std::vector<uint16_t> &sampleInfo);

    // Background I/O worker for the strip streaming path.
    // Single-threaded consumer of m_queue; calls TIFFWriteEncodedStrip().
    void ioWorker();

    // Thread-safe error setter: first error wins, subsequent calls are no-ops.
    void setError(std::error_code ec);

    // Reset all per-session state (queue, I/O thread).
    // Called by endStripWrite after the TIFF is closed.
    void teardownSession();

    std::error_code m_error;
    // mutable: errorCode() 是 const 查询，但仍需加锁读取 m_error
    mutable std::mutex m_errorMutex;

    // === Streaming state (strip mode) ===
    TIFF                 *m_tif             = nullptr;
    int                   m_width           = 0;
    int                   m_height          = 0;
    int                   m_rowsPerStrip    = 0;
    int                   m_samplesPerPixel = 4; // output spp (4=std CMYK, >4=multi-channel)
    std::vector<uint16_t> m_sampleInfo;          // TIFFTAG_EXTRASAMPLES data
    SessionMode           m_mode = SessionMode::None;

    // Bounded producer-consumer queue. Capacity caps peak in-flight memory:
    //   Strip peak ≈ kQueueCapacity × rowsPerStrip × width × 4 bytes
    static constexpr size_t       kQueueCapacity = 4;
    std::unique_ptr<BoundedQueue> m_queue;
    std::thread                   m_ioThread;
    std::atomic<bool>             m_ioError{ false };
};

} // namespace ATHC::EE
