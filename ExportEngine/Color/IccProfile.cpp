#include "IccProfile.h"
#include "FileUtil.h" // Utf8ToWide (Windows)

#include <filesystem>
#include <fstream>
#include <system_error>

#ifdef _WIN32
#    ifndef WIN32_LEAN_AND_MEAN
#        define WIN32_LEAN_AND_MEAN
#    endif
#    include <Windows.h>
#elif defined(__APPLE__) || defined(__linux__)
#    ifndef _GNU_SOURCE
#        define _GNU_SOURCE
#    endif
#    include <dlfcn.h>
#    include <limits.h>
#endif

namespace fs = std::filesystem;

namespace ATHC::EE {

// ── Module directory (for ICC profile lookup relative to the library) ────

static std::string getModuleDir()
{
#ifdef _WIN32
    HMODULE    hModule = nullptr;
    static int dummy   = 0;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                               | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(&dummy), &hModule)
        && hModule) {
        char buf[MAX_PATH] = { 0 };
        GetModuleFileNameA(hModule, buf, MAX_PATH);
        std::string path(buf);
        auto        pos = path.find_last_of("\\/");
        return (pos != std::string::npos) ? path.substr(0, pos) : ".";
    }
    return ".";

#elif defined(__APPLE__) || defined(__linux__)
    Dl_info info;
    if (dladdr(reinterpret_cast<const void *>(&getModuleDir), &info) && info.dli_fname) {
        std::string path(info.dli_fname);
        auto        pos = path.find_last_of('/');
        return (pos != std::string::npos) ? path.substr(0, pos) : ".";
    }
    return ".";

#else
    return ".";
#endif
}

namespace IccProfile {

std::string resolve(const std::string &profile)
{
    const std::string moduleDir = getModuleDir();
    const fs::path    root      = fs::path(moduleDir) / "ICC Profile";
#ifdef _WIN32
    const fs::path raw = root / FileUtil::Utf8ToWide(profile);
#else
    const fs::path raw = root / profile;
#endif

    std::error_code ec;
    const auto      resolved = fs::weakly_canonical(raw, ec);
    if (ec)
        return {};

    const auto allowed = fs::weakly_canonical(root, ec);
    if (ec)
        return {};

    const auto resolvedStr = resolved.u8string();
    const auto allowedStr  = allowed.u8string();
    if (resolvedStr.size() < allowedStr.size()
        || resolvedStr.compare(0, allowedStr.size(), allowedStr) != 0)
        return {};

    return resolvedStr;
}

std::vector<uint8_t> readBytes(const std::string &profile)
{
    const std::string path = resolve(profile);
    if (path.empty())
        return {};

    std::error_code ec;
    const auto      p  = fs::u8path(path);
    const auto      sz = fs::file_size(p, ec);
    if (ec)
        return {};

    std::ifstream ifs(p, std::ios::binary);
    if (!ifs)
        return {};

    std::vector<uint8_t> buf(static_cast<size_t>(sz));
    ifs.read(reinterpret_cast<char *>(buf.data()), static_cast<std::streamsize>(sz));
    if (!ifs)
        return {};

    return buf;
}

} // namespace IccProfile
} // namespace ATHC::EE
