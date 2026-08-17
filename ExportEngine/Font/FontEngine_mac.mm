#include "FontEngine.h"
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include <string>
#include <vector>
#include <cstdlib>
#include <cstring>
#include <cctype>

using namespace ATHC::EE;

namespace {
// Unit-separator character: cannot appear in a font family name, so it
// prevents cache-key collisions like ("Test", bold) vs ("Test|B", regular).
constexpr char kSep = '\x1F';

bool isRegularFile(const std::string &path)
{
    struct stat st;
    if (stat(path.c_str(), &st) != 0)
        return false;
    return S_ISREG(st.st_mode);
}

bool hasFontExtension(const std::string &name)
{
    auto endsWith = [](const std::string &s, const char *suffix) {
        size_t n = std::strlen(suffix);
        return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
    };
    std::string lower = name;
    for (auto &c : lower)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return endsWith(lower, ".ttf") || endsWith(lower, ".otf")
        || endsWith(lower, ".ttc") || endsWith(lower, ".dfont");
}

bool iequals(const std::string &a, const std::string &b)
{
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i]))
            != std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    }
    return true;
}

// Recursively scan a directory tree for a font file whose base name (without
// extension) matches targetBase case-insensitively. macOS file systems are
// case-insensitive by default but case-sensitive volumes exist, so we cannot
// rely on access() alone to find e.g. "arial" against "Arial.ttf".
bool scanDirRecursive(const std::string &dir, const std::string &targetBase,
                      std::string &outPath, int depth = 0)
{
    if (depth > 8)
        return false;
    DIR *d = opendir(dir.c_str());
    if (!d)
        return false;
    bool found = false;
    struct dirent *ent;
    while ((ent = readdir(d)) != nullptr) {
        std::string name = ent->d_name;
        if (name == "." || name == "..")
            continue;
        std::string full = dir;
        if (full.empty() || full.back() != '/')
            full += '/';
        full += name;
        struct stat st;
        if (stat(full.c_str(), &st) != 0)
            continue;
        if (S_ISDIR(st.st_mode)) {
            if (scanDirRecursive(full, targetBase, outPath, depth + 1)) {
                found = true;
                break;
            }
        } else if (S_ISREG(st.st_mode) && hasFontExtension(name)) {
            size_t dot = name.find_last_of('.');
            std::string base = (dot != std::string::npos) ? name.substr(0, dot) : name;
            if (iequals(base, targetBase)) {
                outPath = full;
                found = true;
                break;
            }
        }
    }
    closedir(d);
    return found;
}

std::string expandHome(const std::string &path)
{
    if (path.empty() || path[0] != '~')
        return path;
    const char *home = std::getenv("HOME");
    if (!home || home[0] == '\0')
        return path.substr(1);
    return std::string(home) + path.substr(1);
}
} // namespace

std::string FontEngine::lookupFontFile(const std::string &familyName, bool bold, bool italic)
{
    std::string cacheKey = familyName + kSep + (bold ? "B" : "") + (italic ? "I" : "");
    auto cacheIt = m_fontPathCache.find(cacheKey);
    if (cacheIt != m_fontPathCache.end())
        return cacheIt->second;

    std::vector<std::string> searchPaths = {
        "/System/Library/Fonts/",
        "/System/Library/Fonts/Supplemental/",
        "/Library/Fonts/",
        expandHome("~/Library/Fonts/"),
    };

    auto tryVariant = [&](const std::string &baseName) -> std::string {
        // 1. Direct path with various extensions.
        for (const auto &dir : searchPaths) {
            for (const char *ext : { ".ttf", ".otf", ".ttc", ".dfont" }) {
                std::string path = dir + baseName + ext;
                if (isRegularFile(path))
                    return path;
            }
        }
        // 2. Recursive scan for case-insensitive matching on case-sensitive volumes.
        for (const auto &dir : searchPaths) {
            std::string found;
            if (scanDirRecursive(dir, baseName, found))
                return found;
        }
        return {};
    };

    std::string result = tryVariant(familyName);

    if (result.empty() && (bold || italic)) {
        std::string suffix;
        if (bold && italic)
            suffix = " Bold Italic";
        else if (bold)
            suffix = " Bold";
        else
            suffix = " Italic";
        result = tryVariant(familyName + suffix);
    }

    m_fontPathCache[cacheKey] = result;
    return result;
}
