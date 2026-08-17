#include "FontEngine.h"
#include <windows.h>
#include <string>
#include <vector>
#include <algorithm>
#include <cctype>
#include <locale>

using namespace ATHC::EE;

namespace {
// Unit-separator character: cannot appear in a font family name, so it
// prevents cache-key collisions like ("Test", bold) vs ("Test|B", regular).
constexpr char kSep = '\x1F';

std::string toLower(const std::string &s)
{
    // Use the classic C locale so behaviour is independent of the process locale.
    const std::locale &classic = std::locale::classic();
    std::string r = s;
    for (auto &c : r)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c), classic));
    return r;
}

bool fileExists(const std::string &path)
{
    DWORD attr = GetFileAttributesA(path.c_str());
    // Reject directories — only regular files are valid font files.
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

std::string getFontsDirectory()
{
    char winDir[MAX_PATH] = { 0 };
    if (GetWindowsDirectoryA(winDir, MAX_PATH) > 0)
        return std::string(winDir) + "\\Fonts\\";
    return "C:\\Windows\\Fonts\\"; // last-resort fallback
}

std::string makeFullPath(const std::string &data)
{
    if (data.find(':') != std::string::npos
        || (data.size() >= 2 && data[0] == '\\' && data[1] == '\\'))
        return data; // absolute drive path or UNC path
    return getFontsDirectory() + data;
}

// Look up an exact (case-insensitive) font name in either HKLM or HKCU
// font registration. The registry stores values like "Arial (TrueType)" →
// "arial.ttf". We strip the trailing "(TrueType)"/"(OpenType)" marker and
// compare the remaining name to targetName, so "Arial" no longer matches
// "Arial Black" or "Arial Narrow".
//
// Many CJK fonts have compound registry names like
//   "Microsoft YaHei & Microsoft YaHei UI (TrueType)"  →  msyh.ttc
//   "SimSun & NSimSun (TrueType)"                       →  simsun.ttc
// so we also split on '&' and '(', trimming each part for comparison.
std::string lookupRegistryExact(HKEY root, const std::string &targetName)
{
    HKEY hKey;
    // Always request the 64-bit view so a 32-bit build sees all installed
    // fonts rather than the WOW6432Node redirection.
    REGSAM access = KEY_READ | KEY_WOW64_64KEY;
    if (RegOpenKeyExA(
            root, "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Fonts", 0, access, &hKey)
        != ERROR_SUCCESS)
        return {};

    std::string tl = toLower(targetName);
    std::string result;
    DWORD idx = 0;
    while (true) {
        char valueName[512];
        BYTE valueData[1024]; // expanded from 512 to fit long paths
        DWORD nameLen = sizeof(valueName);
        DWORD dataLen = sizeof(valueData);
        DWORD type;
        LONG ret =
            RegEnumValueA(hKey, idx++, valueName, &nameLen, nullptr, &type, valueData, &dataLen);
        if (ret == ERROR_NO_MORE_ITEMS)
            break;
        if (ret == ERROR_MORE_DATA)
            continue; // value larger than our buffer; skip (very rare for font paths)
        if (ret != ERROR_SUCCESS)
            continue;
        if (type != REG_SZ)
            continue;
        if (dataLen == 0) // guard against (dataLen - 1) underflow below
            continue;

        std::string name(valueName, nameLen);
        std::string nl = toLower(name);

        // Strip the trailing "(TrueType)" / "(OpenType)" marker if present.
        size_t paren = nl.find("(truetype)");
        if (paren == std::string::npos)
            paren = nl.find("(opentype)");
        if (paren != std::string::npos) {
            nl = nl.substr(0, paren);
            while (!nl.empty() && (nl.back() == ' ' || nl.back() == '\t'))
                nl.pop_back();
        }

        // Compare target against the full stripped name as well as each
        // '&' / '(' separated part (handles compound entries like
        // "Microsoft YaHei & Microsoft YaHei UI" → matches "Microsoft YaHei").
        bool matched = false;
        if (nl == tl) {
            matched = true;
        } else {
            size_t pos = 0;
            while (pos < nl.size()) {
                // Skip leading whitespace.
                while (pos < nl.size() && (nl[pos] == ' ' || nl[pos] == '\t'))
                    ++pos;
                if (pos >= nl.size())
                    break;
                size_t end = pos;
                while (end < nl.size() && nl[end] != '&' && nl[end] != '(')
                    ++end;
                // Trim trailing whitespace on this part.
                size_t partEnd = end;
                while (partEnd > pos && (nl[partEnd - 1] == ' ' || nl[partEnd - 1] == '\t'))
                    --partEnd;
                if (partEnd > pos && std::string(nl.c_str() + pos, partEnd - pos) == tl) {
                    matched = true;
                    break;
                }
                pos = end + 1; // skip past '&' or '('
            }
        }

        if (matched) {
            // dataLen includes the trailing NUL for REG_SZ; drop exactly one byte.
            std::string data(reinterpret_cast<char *>(valueData), dataLen - 1);
            std::string full = makeFullPath(data);
            if (fileExists(full)) {
                result = full;
                break;
            }
        }
    }
    RegCloseKey(hKey);
    return result;
}

std::string lookupFileVariants(const std::string &family, bool bold, bool italic)
{
    struct Candidate
    {
        std::string name;
        int priority;
    };
    std::vector<Candidate> cands;

    // Base family name — always included. When bold/italic is requested, prefer
    // explicit " Family Bold"/" Family Italic" variants first, but try the base
    // family BEFORE any ambiguous abbreviation-based suffix (b/bd/i/bi) since
    // single-letter suffixes match unrelated files (e.g. simsunb.ttf is SimSun-ExtB,
    // not SimSun Bold).
    cands.push_back({ family, bold || italic ? 80 : 100 });

    // Full-word style suffixes: these are standard font naming conventions.
    if (bold && italic) {
        cands.push_back({ family + " Bold Italic", 90 });
    }
    if (bold) {
        cands.push_back({ family + " Bold", 90 });
    }
    if (italic) {
        cands.push_back({ family + " Italic", 90 });
    }

    // Lowercase abbreviation variants: only useful when the font file literally
    // uses a short suffix (e.g. "timesbi.ttf" for Times New Roman Bold Italic).
    // Priority is LOWER than the base family name to avoid false matches with
    // files like simsunb.ttf (SimSun-ExtB) or similar non-style suffixes.
    std::string lf = toLower(family);
    if (bold && italic)
        cands.push_back({ lf + "bi", 60 });
    if (bold) {
        cands.push_back({ lf + "b", 60 });
        cands.push_back({ lf + "bd", 60 });
    }
    if (italic)
        cands.push_back({ lf + "i", 60 });

    std::sort(cands.begin(), cands.end(),
        [](const Candidate &a, const Candidate &b) { return a.priority > b.priority; });

    std::string fontsDir = getFontsDirectory();
    // Windows file system is case-insensitive, so checking each extension
    // once is enough; the previous .TTF/.TTC/.OTF variants were redundant.
    for (const auto &c : cands) {
        for (const char *ext : { ".ttf", ".ttc", ".otf" }) {
            std::string p = fontsDir + c.name + ext;
            if (fileExists(p))
                return p;
        }
    }
    return {};
}
} // namespace

std::string FontEngine::lookupFontFile(const std::string &familyName, bool bold, bool italic)
{
    std::string cacheKey = familyName + kSep + (bold ? "B" : "") + (italic ? "I" : "");
    auto cacheIt = m_fontPathCache.find(cacheKey);
    if (cacheIt != m_fontPathCache.end()) {
        return cacheIt->second;
    }

    std::string result;

    // Per-user font registrations take precedence (matches Windows font picker).
    if (bold && italic) {
        result = lookupRegistryExact(HKEY_CURRENT_USER, familyName + " Bold Italic");
    }
    if (result.empty() && bold) {
        result = lookupRegistryExact(HKEY_CURRENT_USER, familyName + " Bold");
    }
    if (result.empty() && italic) {
        result = lookupRegistryExact(HKEY_CURRENT_USER, familyName + " Italic");
    }
    if (result.empty()) {
        result = lookupRegistryExact(HKEY_CURRENT_USER, familyName);
    }

    // Fall back to system-wide registrations.
    if (result.empty() && bold && italic) {
        result = lookupRegistryExact(HKEY_LOCAL_MACHINE, familyName + " Bold Italic");
    }
    if (result.empty() && bold) {
        result = lookupRegistryExact(HKEY_LOCAL_MACHINE, familyName + " Bold");
    }
    if (result.empty() && italic) {
        result = lookupRegistryExact(HKEY_LOCAL_MACHINE, familyName + " Italic");
    }
    if (result.empty()) {
        result = lookupRegistryExact(HKEY_LOCAL_MACHINE, familyName);
    }

    // Last resort: filesystem scan of the fonts directory.
    if (result.empty()) {
        result = lookupFileVariants(familyName, bold, italic);
    }

    m_fontPathCache[cacheKey] = result;
    return result;
}
