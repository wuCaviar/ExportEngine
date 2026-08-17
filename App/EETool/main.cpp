// ============================================================================
//  EETool — JSON → TIFF converter (CLI frontend for json2tiff)
// ============================================================================

#include "Json2Tiff.h"
#include "Version.h"
#include "Log.h"

#include <iostream>
#include <string>
#include <vector>
#include <filesystem>
#include <algorithm>
#include <cstring>

#ifdef _WIN32
#    ifndef WIN32_LEAN_AND_MEAN
#        define WIN32_LEAN_AND_MEAN
#    endif
#    include <Windows.h>
#endif

using namespace ATHC::EE;
namespace fs = std::filesystem;

#ifdef _WIN32
// Convert a command-line argument (ANSI/GBK per MSVC CRT main()) to UTF-8,
// so the library can treat all paths uniformly as UTF-8.
// With the "Beta: UTF-8 worldwide" locale option argv is already UTF-8
// (GetACP() == 65001) — pass through unchanged.
static std::string ansiToUtf8(const std::string &s)
{
    if (s.empty() || ::GetACP() == 65001)
        return s;
    int wlen = MultiByteToWideChar(CP_ACP, 0, s.c_str(), -1, nullptr, 0);
    if (wlen <= 0)
        return s;
    std::wstring w(static_cast<size_t>(wlen), L'\0');
    MultiByteToWideChar(CP_ACP, 0, s.c_str(), -1, &w[0], wlen);
    if (!w.empty() && w.back() == 0)
        w.pop_back();
    int ulen = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (ulen <= 0)
        return s;
    std::string u(static_cast<size_t>(ulen), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &u[0], ulen, nullptr, nullptr);
    if (!u.empty() && u.back() == 0)
        u.pop_back();
    return u;
}

// 将 std::string 转为 wstring。
// 优先按 UTF-8（JSON 内容、库 API 契约的路径），失败再按系统本地编码
// （GBK，兼容命令行 argv 等旧来源），两种都无法解码时返回空。
std::wstring Utf8ToWide(const std::string &utf8)
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

static void printUsage(const char *prog)
{
    std::cout << "EETool — JSON → TIFF converter  v" << Version::semantic() << "\n\n";
    std::cout << "Usage:\n";
    std::cout << "  " << prog << " <input.json>  -o <output.tiff>       Single file\n";
    std::cout << "  " << prog << " <input_dir/>  -o <output_dir/>       Batch folder\n\n";
    std::cout << "Options:\n";
    std::cout << "  -o <path>            Output path (required)\n";
    std::cout << "  -h, --help           Show this help\n";
    std::cout << "  -v, --version        Show version\n";
}

int main(int argc, char *argv[])
{
    EELog::initLogger();
    EELog::info("ExportEngine {}", Version::full());

    // Parse arguments
    std::string inputPath;
    std::string outputPath;

    for (int i = 1; i < argc; ++i) {
        std::string arg(argv[i]);
        if (arg == "-h" || arg == "--help") {
            printUsage(argv[0]);
            return 0;
        }
        if (arg == "-v" || arg == "--version") {
            std::cout << Version::full() << "/n";
            return 0;
        }
        if (arg == "-o") {
            if (i + 1 < argc)
                outputPath = argv[++i];
            else {
                std::cerr << "Error: -o requires a path argument\n";
                return 1;
            }
        } else if (arg[0] != '-') {
            if (inputPath.empty())
                inputPath = arg;
            else {
                std::cerr << "Error: unexpected extra argument: " << arg << "\n";
                return 1;
            }
        } else {
            std::cerr << "Error: unknown option: " << arg << "\n";
            printUsage(argv[0]);
            return 1;
        }
    }

    if (inputPath.empty()) {
        std::cerr << "Error: no input path specified\n\n";
        printUsage(argv[0]);
        return 1;
    }
    if (outputPath.empty()) {
        std::cerr << "Error: -o <output> is required\n\n";
        printUsage(argv[0]);
        return 1;
    }

#ifdef _WIN32
    // argv is ANSI/GBK on Windows — normalize to UTF-8 before entering the lib
    inputPath = ansiToUtf8(inputPath);
    outputPath = ansiToUtf8(outputPath);
#endif

    EELog::info("Write mode: Strip");

    std::error_code ec;
    bool inputIsDir = fs::is_directory(Utf8ToWide(inputPath), ec);
    if (ec)
        inputIsDir = false;

    if (!inputIsDir) {
        // ── Single file mode ───────────────────────────────────────────
        std::wstring wpath = Utf8ToWide(inputPath);
        if (!fs::exists(wpath)) {
            std::cerr << "Error: input file not found: " << inputPath << "\n";
            return 1;
        }

        bool ok = json2tiff(
            inputPath, outputPath,
            [](int cur, int total, const std::string &msg) {
                // std::cout << "  [" << cur << "/" << total << "] " << msg << "\n";
            },
            [](std::error_code ec) { std::cerr << "  Error: " << ec.message() << "\n"; });

        if (!ok)
            return 1;
        std::cout << "Done: " << outputPath << "\n";
    } else {
        // ── Folder mode ────────────────────────────────────────────────
        std::vector<fs::path> jsonFiles;
        for (auto &entry : fs::directory_iterator(Utf8ToWide(inputPath), ec)) {
            if (ec)
                break;
            if (!entry.is_regular_file())
                continue;
            auto ext = entry.path().extension().string();
            std::string lowerExt;
            lowerExt.reserve(ext.size());
            for (char c : ext)
                lowerExt += static_cast<char>(::tolower(static_cast<unsigned char>(c)));
            if (lowerExt == ".json")
                jsonFiles.push_back(entry.path());
        }

        if (jsonFiles.empty()) {
            std::cerr << "Error: no .json files found in: " << inputPath << "\n";
            return 1;
        }

        std::sort(jsonFiles.begin(), jsonFiles.end());
        fs::create_directories(Utf8ToWide(outputPath), ec);
        if (ec) {
            std::cerr << "Error: cannot create output directory: " << ec.message() << "\n";
            return 1;
        }

        std::cout << "Batch: " << jsonFiles.size() << " file(s) → " << outputPath << "\n\n";

        int okCount = 0, failCount = 0;
        for (size_t i = 0; i < jsonFiles.size(); ++i) {
            auto &jp = jsonFiles[i];
            auto outFile = fs::path(Utf8ToWide(outputPath)) / (jp.stem().string() + ".tif");

            std::cout << "[" << (i + 1) << "/" << jsonFiles.size() << "] " << jp.filename().string()
                      << "\n";

            bool ok = json2tiff(
                jp.u8string(), outFile.u8string(),
                [](int cur, int total, const std::string &msg) {
                    std::cout << "    [" << cur << "/" << total << "] " << msg << "\n";
                },
                [](std::error_code ec) { std::cerr << "    Error: " << ec.message() << "\n"; });

            if (ok)
                ++okCount;
            else {
                ++failCount;
                std::cerr << "  FAILED\n";
            }
        }

        std::cout << "\n── Done: " << okCount << " ok";
        if (failCount > 0)
            std::cout << ", " << failCount << " failed";
        std::cout << " ──\n";
        if (failCount > 0)
            return 1;
    }

    return 0;
}
