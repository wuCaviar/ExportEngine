#ifndef TIFFHELPER_H
#define TIFFHELPER_H

/// Cross-platform UTF-8 TIFF file opening (Enhanced for Windows).
///
/// On Windows, always uses TIFFOpenW() after converting input to UTF-16.
/// The conversion attempts UTF-8 first; if that fails, falls back to system
/// default ANSI (CP_ACP) to handle paths that are already in local encoding.
/// This ensures Chinese/Unicode paths open correctly regardless of system locale.
///
/// On Unix, passes the path directly to TIFFOpen().

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

/// Open a TIFF file with a UTF-8 encoded path.
/// @param utf8Path  File path (UTF-8 on all platforms)
/// @param mode      "r" for read, "w" for write
/// @return TIFF handle on success, nullptr on failure
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
    // Unix: UTF-8 native
    return TIFFOpen(utf8Path.c_str(), mode);
#endif
}

} // namespace TiffHelper
} // namespace ATHC::EE

#endif // TIFFHELPER_H
