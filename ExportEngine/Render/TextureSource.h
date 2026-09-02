#pragma once
#include "SceneData.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ATHC::EE {

class IColorConverter;

// Texture pattern source for TextureFill (rect image-pattern fill).
// Decodes an image file (TIFF via libtiff, other formats via vips),
// converts to CMYK + alpha, and serves nearest-neighbour samples at
// normalized (u, v) ∈ [0,1) coordinates.
//
// Frames are shared through a static per-file cache (preDecode() warms it;
// entries carry the decoded source dimensions so stale frames are not
// reused when the file changes); a TextureSource instance holds a
// shared_ptr to the frame. Frames larger than kMaxCachedBytes are never
// cached.
class TextureSource
{
public:
    // overrideW/overrideH > 0 → tile size (canvas px, from customWidth/
    // customHeight); otherwise tile size = round(src × canvasDpi / imgDpi).
    TextureSource(const std::string &filePath,
                  const Dpi         &canvasDpi,
                  IColorConverter   *cv,
                  int                overrideW = 0,
                  int                overrideH = 0);
    ~TextureSource();

    // Warm the shared frame cache (keyed by file path only; tile size is
    // resolved per instance). Failure marks the file bad — later instances
    // skip it without retry.
    static void preDecode(const std::string &filePath,
                          const Dpi         &canvasDpi,
                          IColorConverter   *cv,
                          int                overrideW = 0,
                          int                overrideH = 0);

    bool ok() const { return m_frame != nullptr; }
    int  tileWidth() const { return m_tileW; }
    int  tileHeight() const { return m_tileH; }

    // Nearest-neighbour sample at normalized (u, v) ∈ [0, 1). Outputs CMYK
    // channels + alpha (0-255). Returns false if the source failed to decode.
    bool sample(double u, double v, uint8_t &c1, uint8_t &c2, uint8_t &c3, uint8_t &c4, uint8_t &a);

    static constexpr size_t kMaxCachedBytes = 256u * 1024u * 1024u; // 256 MB

private:
    // Log `msg` once per filePath (throttled), then return true.
    static bool warnOnce(const std::string &key, const std::string &msg);

    bool open(); // metadata + decode (or cache hit)

    std::string                                 m_path;
    Dpi                                         m_canvasDpi{ 300, 300 };
    IColorConverter                            *m_cv        = nullptr;
    int                                         m_overrideW = 0;
    int                                         m_overrideH = 0;
    int                                         m_srcW      = 0; // source pixel dims
    int                                         m_srcH      = 0;
    double                                      m_imgDpiX   = 72.0;
    double                                      m_imgDpiY   = 72.0;
    int                                         m_tileW     = 0; // tile size in canvas px
    int                                         m_tileH     = 0;
    std::shared_ptr<const std::vector<uint8_t>> m_frame;         // srcW×srcH×5 CMYKA（源图像素）

    // 缓存条目：解码帧 + 帧对应的源尺寸（open() 用重新读取的元数据
    // 校验缓存帧尺寸一致，防止文件被替换后旧帧被新尺寸索引 → OOB）。
    struct CachedFrame
    {
        std::shared_ptr<const std::vector<uint8_t>> frame;
        int                                         srcW = 0;
        int                                         srcH = 0;
    };

    static std::mutex                                   s_mtx;
    static std::unordered_map<std::string, CachedFrame> s_cache;
    static std::unordered_set<std::string>              s_badFiles;
    static std::unordered_set<std::string>              s_warned;
};

} // namespace ATHC::EE
