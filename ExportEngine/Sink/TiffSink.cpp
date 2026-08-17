#include "TiffSink.h"

#include "TiffWriter.h"
#include "TiffHelper.h"
#include "IccProfile.h"
#include "Log.h"

#include <filesystem>

using namespace ATHC::EE;
namespace fs = std::filesystem;

ATHC::EE::TiffSink::TiffSink(std::string path) : m_path(std::move(path))
{
    m_writer = new TiffWriter();
}

ATHC::EE::TiffSink::~TiffSink()
{
    delete m_writer;
    m_writer = nullptr;
}

int ATHC::EE::TiffSink::preferredRowsPerStrip(int width, int height) const
{
    return TiffWriter::defaultRowsPerStrip(width, height);
}

bool TiffSink::begin(const SinkDescriptor &desc)
{
    clearError();
    m_desc = desc;

    // 输出目录（与旧 json2tiff 行为一致：写前创建父目录）
    fs::path outPath(TiffHelper::Utf8ToWide(m_path));
    if (outPath.has_parent_path()) {
        std::error_code ec;
        fs::create_directories(outPath.parent_path(), ec);
        if (ec) {
            setError(ec);
            return false;
        }
    }

    // 写入 TIFF 的 sink 自己处理 ICC：内部加载默认 CMYK profile 内嵌。
    // EE 不提取、不传递 ICC 字节。
    const std::vector<uint8_t> iccBytes = IccProfile::readBytes(IccProfile::kDefaultCmyk);

    const int rps = preferredRowsPerStrip(desc.width, desc.height);
    const int stripCount = (desc.height + rps - 1) / rps;
    EELog::info("  Write mode: Strip");
    EELog::info("  Strips: {} ({} rows/strip)", stripCount, rps);
    const bool ok = m_writer->beginStripWrite(m_path, desc.width, desc.height, desc.dpi, rps,
        iccBytes, desc.samplesPerPixel, desc.sampleInfo);

    if (!ok)
        setError(m_writer->errorCode());
    return ok;
}

bool TiffSink::writeStrip(int startRow, int rows, std::vector<uint8_t> cmyk)
{
    if (m_writer->writeStrip(startRow, rows, std::move(cmyk)))
        return true;
    setError(m_writer->errorCode());
    return false;
}

bool TiffSink::end()
{
    const bool ok = m_writer->endStripWrite();
    if (ok) {
        const int rps = preferredRowsPerStrip(m_desc.width, m_desc.height);
        const int stripCount = (m_desc.height + rps - 1) / rps;
        EELog::info("  {} strips written", stripCount);
        return true;
    }
    setError(m_writer->errorCode());
    return false;
}
