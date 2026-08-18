#ifndef TIFFHELPER_H
#define TIFFHELPER_H

/// 跨平台 UTF-8 TIFF 文件打开（增强 Windows 支持）。
///
/// 在 Windows 上，始终使用 TIFFOpenW()，并在调用前将输入转换为 UTF-16。
/// 转换首先尝试 UTF-8；如果失败，则回退到系统默认 ANSI (CP_ACP) 以处理
/// 已经是本地编码的路径。这确保中文/Unicode 路径在任何系统区域设置下都能正确打开。
///
/// 在 Unix 上，直接将路径传递给 TIFFOpen()。

#include <string>
#include <tiffio.h>

#ifdef _WIN32
#    ifndef WIN32_LEAN_AND_MEAN
#        define WIN32_LEAN_AND_MEAN
#    endif
#    include <Windows.h>
#endif

namespace ATHC::EE {
namespace TiffHelper {

#ifdef _WIN32

// 将 std::string 转为 wstring。
// 优先按 UTF-8（JSON 内容、库 API 契约的路径），失败再按系统本地编码
// （GBK，兼容命令行 argv 等旧来源），两种都无法解码时返回空。
inline std::wstring Utf8ToWide(const std::string &utf8)
{
    if (utf8.empty())
        return {};
    const UINT cps[] = { CP_UTF8, CP_ACP };
    for (UINT cp : cps) {
        int len = MultiByteToWideChar(cp, MB_ERR_INVALID_CHARS, utf8.c_str(), -1, nullptr, 0);
        if (len <= 0)
            continue;
        std::wstring w(static_cast<size_t>(len), L'\0');
        MultiByteToWideChar(cp, MB_ERR_INVALID_CHARS, utf8.c_str(), -1, &w[0], len);
        if (!w.empty() && w.back() == 0)
            w.pop_back(); // 去掉结尾 null
        return w;
    }
    return {};
}
#endif

/// 使用 UTF-8 编码路径打开 TIFF 文件。
/// @param utf8Path  文件路径（所有平台上均为 UTF-8）
/// @param mode      "r" 表示读取，"w" 表示写入
/// @return 成功返回 TIFF 句柄，失败返回 nullptr
inline TIFF *openTiff(const std::string &utf8Path, const char *mode)
{
    if (utf8Path.empty())
        return nullptr;

#ifdef _WIN32
    std::wstring widePath = Utf8ToWide(utf8Path);
    if (!widePath.empty())
        return TIFFOpenW(widePath.c_str(), mode);

    return TIFFOpen(utf8Path.c_str(), mode);
#else
    // Unix：UTF-8 原生
    return TIFFOpen(utf8Path.c_str(), mode);
#endif
}

} // namespace TiffHelper
} // namespace ATHC::EE

#endif // TIFFHELPER_H
