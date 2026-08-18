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

// TextureFill（矩形图像图案填充）的纹理图案源。
// 解码图像文件（TIFF 通过 libtiff，其他格式通过 vips），
// 转换为 CMYK + alpha，并在归一化坐标 (u, v) ∈ [0,1) 处提供最近邻采样。
//
// 帧通过静态每文件缓存共享（preDecode() 预热缓存；
// 条目携带解码后的源尺寸，因此当文件变化时不会重用旧帧）；
// TextureSource 实例持有帧的 shared_ptr。大于 kMaxCachedBytes 的帧不会被缓存。
class TextureSource
{
public:
    // overrideW/overrideH > 0 → 瓦片尺寸（画布像素，来自 customWidth/customHeight）；
    // 否则瓦片尺寸 = round(源尺寸 × canvasDpi / imgDpi)。
    TextureSource(const std::string &filePath, const Dpi &canvasDpi, IColorConverter *cv,
        int overrideW = 0, int overrideH = 0);
    ~TextureSource();

    // 预热共享帧缓存（仅按文件路径键控；瓦片尺寸按每个实例解析）。\n    // 失败会将文件标记为坏文件——后续实例跳过它而不重试。
    static void preDecode(const std::string &filePath, const Dpi &canvasDpi, IColorConverter *cv,
        int overrideW = 0, int overrideH = 0);

    bool ok() const { return m_frame != nullptr; }
    int tileWidth() const { return m_tileW; }
    int tileHeight() const { return m_tileH; }

    // 在归一化坐标 (u, v) ∈ [0, 1) 处进行最近邻采样。输出 CMYK 通道 + alpha（0-255）。\n    // 如果源解码失败则返回 false。
    bool sample(double u, double v, uint8_t &c1, uint8_t &c2, uint8_t &c3, uint8_t &c4, uint8_t &a);

    static constexpr size_t kMaxCachedBytes = 256u * 1024u * 1024u; // 256 MB（256 兆字节）

private:
    // 对每个 filePath 记录 `msg` 一次（限流），然后返回 true。
    static bool warnOnce(const std::string &key, const std::string &msg);

    bool open(); // 元数据 + 解码（或缓存命中）

    std::string m_path;
    Dpi m_canvasDpi{ 300, 300 };
    IColorConverter *m_cv = nullptr;
    int m_overrideW = 0;
    int m_overrideH = 0;
    int m_srcW = 0; // 源图像素宽度
    int m_srcH = 0; // 源图像素高度
    double m_imgDpiX = 72.0;
    double m_imgDpiY = 72.0;
    int m_tileW = 0; // 画布像素的瓦片尺寸
    int m_tileH = 0;
    std::shared_ptr<const std::vector<uint8_t>> m_frame; // srcW×srcH×5 CMYKA（源图像素）

    // 缓存条目：解码帧 + 帧对应的源尺寸（open() 用重新读取的元数据
    // 校验缓存帧尺寸一致，防止文件被替换后旧帧被新尺寸索引 → OOB）。
    struct CachedFrame
    {
        std::shared_ptr<const std::vector<uint8_t>> frame;
        int srcW = 0;
        int srcH = 0;
    };

    static std::mutex s_mtx;
    static std::unordered_map<std::string, CachedFrame> s_cache;
    static std::unordered_set<std::string> s_badFiles;
    static std::unordered_set<std::string> s_warned;
};

} // namespace ATHC::EE
