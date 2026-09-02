#include "TiffWriter.h"
#include "FileUtil.h"

#include <tiffio.h>
#include <algorithm>
#include <cstring>
#include <vector>

using namespace ATHC::EE;

// ============================================================
//  Bounded blocking queue — producer/consumer streaming primitive
// ============================================================
//  Producers (caller threads) push strips; a single I/O consumer thread pops
//  and writes them to libtiff. The bounded capacity provides natural
//  backpressure: when the queue is full, producers block, capping peak
//  in-flight memory.
//
//  Lifecycle:
//    - push() returns false once close() has been called → producer stops.
//    - pop()  returns false when closed AND drained → consumer exits.
//    - close() wakes all blocked threads.

bool TiffWriter::BoundedQueue::push(WriteTask &&task)
{
    std::unique_lock<std::mutex> lock(mtx);
    cvNotFull.wait(lock, [this] { return q.size() < capacity || closed; });
    if (closed)
        return false;
    q.push(std::move(task));
    cvNotEmpty.notify_one();
    return true;
}

bool TiffWriter::BoundedQueue::pop(WriteTask &out)
{
    std::unique_lock<std::mutex> lock(mtx);
    cvNotEmpty.wait(lock, [this] { return !q.empty() || closed; });
    if (q.empty())
        return false; // closed and drained
    out = std::move(q.front());
    q.pop();
    cvNotFull.notify_one();
    return true;
}

void TiffWriter::BoundedQueue::close()
{
    std::lock_guard<std::mutex> lock(mtx);
    closed = true;
    cvNotEmpty.notify_all();
    cvNotFull.notify_all();
}

// ============================================================
//  Error handling
// ============================================================

void TiffWriter::setError(std::error_code ec)
{
    std::lock_guard<std::mutex> lock(m_errorMutex);
    if (!m_error) // first error wins
        m_error = ec;
}

void TiffWriter::teardownSession()
{
    // Called after the TIFF handle is closed. Resets per-session state so
    // a subsequent begin*() call starts fresh.
    m_queue.reset();
    m_mode         = SessionMode::None;
    m_rowsPerStrip = 0;
}

// ============================================================
//  Destructor
// ============================================================

TiffWriter::~TiffWriter()
{
    // Defensive cleanup: if the caller forgot endStripWrite(), tear down the
    // I/O thread before closing the TIFF handle.
    if (m_queue) {
        m_queue->close();
    }
    if (m_ioThread.joinable()) {
        m_ioThread.join();
    }
    if (m_tif) {
        TIFFClose(m_tif);
        m_tif = nullptr;
    }
}

// ============================================================
//  TIFF field setup
// ============================================================

void TiffWriter::setupTiffCommon(TIFF                        *tif,
                                 int                          width,
                                 int                          height,
                                 Dpi                          dpi,
                                 int                          samplesPerPixel,
                                 const std::vector<uint16_t> &sampleInfo)
{
    TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, static_cast<uint32_t>(width));
    TIFFSetField(tif, TIFFTAG_IMAGELENGTH, static_cast<uint32_t>(height));
    TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, static_cast<uint16_t>(samplesPerPixel));
    TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, 8);
    TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_SEPARATED);
    TIFFSetField(tif, TIFFTAG_INKSET, INKSET_CMYK);
    TIFFSetField(tif, TIFFTAG_COMPRESSION, COMPRESSION_LZW); // 固定 LZW
    TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
    TIFFSetField(tif, TIFFTAG_XRESOLUTION, static_cast<float>(dpi.x));
    TIFFSetField(tif, TIFFTAG_YRESOLUTION, static_cast<float>(dpi.y));
    TIFFSetField(tif, TIFFTAG_RESOLUTIONUNIT, RESUNIT_INCH);
    if (samplesPerPixel > 4 && !sampleInfo.empty()) {
        TIFFSetField(tif, TIFFTAG_EXTRASAMPLES, static_cast<uint16_t>(samplesPerPixel - 4),
                     sampleInfo.data());
    }
}

int TiffWriter::defaultRowsPerStrip(int width, int height)
{
    constexpr int kTargetStripBytes = 8 * 1024 * 1024; // 8 MB
    if (width <= 0)
        return 1;
    int rows = kTargetStripBytes / (width * 4);
    if (rows < 1)
        rows = 1;
    if (height > 0 && rows > height)
        rows = height;
    return rows;
}

// ============================================================
//  Strip streaming (producer-consumer)
// ============================================================
//
//  Architecture:
//
//    Caller thread(s)                          I/O thread (single)
//    ──────────────────                       ─────────────────────
//    writeStrip()                             ioWorker()
//      ├─ move strip verbatim ──┐             loop:
//      └─ push strip ───────────┼──►queue──► TIFFWriteEncodedStrip()
//                               │           (libtiff single-writer)
//                               │
//                  backpressure:│
//                  queue full → block producer

bool TiffWriter::beginStripWrite(const std::string           &filePath,
                                 int                          width,
                                 int                          height,
                                 Dpi                          dpi,
                                 int                          rowsPerStrip,
                                 const std::vector<uint8_t>  &iccBytes,
                                 int                          samplesPerPixel,
                                 const std::vector<uint16_t> &sampleInfo)
{
    // Reset error state at the very start of every begin*() call, so a
    // previous session's error can never leak into this one (first-error-wins).
    {
        std::lock_guard<std::mutex> lock(m_errorMutex);
        m_error.clear();
    }
    if (m_tif) {
        setError(EEError::tiff_no_session);
        return false;
    }
    if (rowsPerStrip < 1)
        rowsPerStrip = defaultRowsPerStrip(width, height);

    m_samplesPerPixel = samplesPerPixel;
    m_sampleInfo      = sampleInfo;
    m_tif             = FileUtil::openTiff(filePath, "w8");
    if (!m_tif) {
        setError(EEError::tiff_open_failed);
        return false;
    }

    setupTiffCommon(m_tif, width, height, dpi, samplesPerPixel, sampleInfo);
    TIFFSetField(m_tif, TIFFTAG_ROWSPERSTRIP, static_cast<uint32_t>(rowsPerStrip));
    TIFFSetField(m_tif, TIFFTAG_ORIENTATION, ORIENTATION_TOPLEFT);

    if (!iccBytes.empty()) {
        TIFFSetField(m_tif, TIFFTAG_ICCPROFILE, static_cast<uint32_t>(iccBytes.size()),
                     iccBytes.data());
    }

    m_width        = width;
    m_height       = height;
    m_rowsPerStrip = rowsPerStrip;
    m_mode         = SessionMode::Strip;

    m_ioError.store(false);
    m_queue    = std::make_unique<BoundedQueue>(kQueueCapacity);
    m_ioThread = std::thread(&TiffWriter::ioWorker, this);
    return true;
}

bool TiffWriter::writeStrip(int startRow, int rows, std::vector<uint8_t> cmykBuf)
{
    if (m_mode != SessionMode::Strip || !m_tif || !m_queue) {
        setError(EEError::tiff_no_session);
        return false;
    }
    if (m_ioError.load(std::memory_order_acquire)) {
        return false;
    }

    // 缓冲已 move 进来：满尺寸直接入队（零拷贝），空/欠尺寸补零到 stripBytes。
    const size_t stripBytes = static_cast<size_t>(rows) * m_width * m_samplesPerPixel;

    // libtiff strip index = startRow / rowsPerStrip.
    const uint32_t stripIndex = static_cast<uint32_t>(startRow / m_rowsPerStrip);

    WriteTask task;
    task.index = stripIndex;
    if (cmykBuf.size() >= stripBytes) {
        task.buffer = std::move(cmykBuf);
        if (task.buffer.size() > stripBytes)
            task.buffer.resize(stripBytes); // 防御：恰好 stripBytes
    } else {
        std::vector<uint8_t> buf(stripBytes, 0);
        if (!cmykBuf.empty())
            std::memcpy(buf.data(), cmykBuf.data(), cmykBuf.size());
        task.buffer = std::move(buf);
    }

    if (!m_queue->push(std::move(task))) {
        return false;
    }
    if (m_ioError.load(std::memory_order_acquire)) {
        return false;
    }
    return true;
}

// ============================================================
//  Shared I/O worker (single consumer thread)
// ============================================================
//
//  Drains m_queue and dispatches to TIFFWriteEncodedStrip(). Single-threaded
//  — libtiff is single-writer per TIFF handle.

void TiffWriter::ioWorker()
{
    WriteTask task;
    bool      failed = false;
    // Drain the queue until closed-and-empty. On error, set the failure
    // flag, close the queue (rejects further pushes), and keep draining
    // so blocked producers can unblock and observe m_ioError.
    while (m_queue->pop(task)) {
        if (failed)
            continue; // discard remaining items, just unblock producers

        const bool ok =
            TIFFWriteEncodedStrip(m_tif, static_cast<tstrip_t>(task.index), task.buffer.data(),
                                  static_cast<tsize_t>(task.buffer.size()))
            >= 0;
        if (!ok)
            setError(EEError::tiff_write_strip_failed);

        if (!ok) {
            m_ioError.store(true, std::memory_order_release);
            m_queue->close();
            failed = true;
        }
    }
}

bool TiffWriter::endStripWrite()
{
    if (m_mode != SessionMode::Strip || !m_tif)
        return true;

    if (m_queue)
        m_queue->close();
    if (m_ioThread.joinable())
        m_ioThread.join();

    const bool ok = !m_ioError.load(std::memory_order_acquire);

    TIFFClose(m_tif);
    m_tif = nullptr;
    teardownSession();
    return ok;
}
