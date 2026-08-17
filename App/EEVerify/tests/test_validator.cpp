// EEVerify comprehensive unit tests
// ============================================================================
//
// Covers all validation categories:
//   . File & JSON parsing       . Rect primitives
//   . Canvas validation         . Circle primitives
//   . Color validation          . FreeLine primitives
//   . Gradient validation       . BezierCurve primitives
//   . Unit validation           . Line primitives
//   . Edit-distance algorithm   . Text primitives
//   . Typo detection            . ImageItem primitives
//   . Integration / edge cases
//
// CTest-compatible -- returns 0 on all pass, non-zero on any failure.
// Compiles Validator.cpp directly -- no ExportEngine.dll dependency.
// ============================================================================

#include "Validator.h"
#include "test_harness.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

// Validator-specific expectation macro (kept here; common harness is in test_harness.h)
#define EXPECT_GT(a, b)                                                                         \
    do {                                                                                        \
        auto _va = (a);                                                                         \
        auto _vb = (b);                                                                         \
        if (!(_va > _vb)) {                                                                     \
            std::cerr << "  FAIL " << __FILE__ << ":" << __LINE__ << "  " << #a << " <= " << #b \
                      << "  (" << _va << " <= " << _vb << ")\n";                                \
            ++gFailed;                                                                          \
        } else {                                                                                \
            ++gPassed;                                                                          \
        }                                                                                       \
    } while (0)

// RAII temporary JSON file
struct TempFile
{
    std::string path;
    explicit TempFile(const std::string &content)
    {
        auto tmpDir = std::filesystem::temp_directory_path();
        auto stem = "eeverify_test_" + std::to_string(std::rand());
        path = (tmpDir / (stem + ".json")).string();
        std::ofstream out(path);
        out << content;
    }
    ~TempFile() { std::remove(path.c_str()); }
};

// Helper: convert backslashes to forward slashes for JSON embedding on Windows
static std::string jsonPath(const std::filesystem::path &p)
{
    std::string s = p.string();
    std::replace(s.begin(), s.end(), '\\', '/');
    return s;
}

static ValidationReport validateJson(const std::string &json)
{
    TempFile tf(json);
    Validator v;
    ValidationReport report;
    v.validate(tf.path, report);
    return report;
}

static bool hasIssue(const ValidationReport &r, Severity s, const std::string &pathSubstr)
{
    for (const auto &i : r.issues)
        if (i.severity == s && i.path.find(pathSubstr) != std::string::npos)
            return true;
    return false;
}

static bool hasIssueMsg(const ValidationReport &r, Severity s, const std::string &pathSubstr,
    const std::string &msgSubstr)
{
    for (const auto &i : r.issues)
        if (i.severity == s && i.path.find(pathSubstr) != std::string::npos
            && (i.message.find(msgSubstr) != std::string::npos
                || i.suggestion.find(msgSubstr) != std::string::npos))
            return true;
    return false;
}

static int countIssues(const ValidationReport &r, Severity s)
{
    int n = 0;
    for (const auto &i : r.issues)
        if (i.severity == s)
            ++n;
    return n;
}

// Assertion helpers for validation reports
#define EXPECT_ISSUE(report, sev, pathSubstr)                                                  \
    do {                                                                                       \
        if (!hasIssue(report, sev, pathSubstr)) {                                              \
            std::cerr << "  FAIL " << __FILE__ << ":" << __LINE__ << "  expected "             \
                      << severityLabel(sev) << " at path containing '" << pathSubstr << "'\n"; \
            for (auto &_i : (report).issues)                                                   \
                std::cerr << "    actual: " << severityLabel(_i.severity) << " [" << _i.path   \
                          << "] " << _i.message << "\n";                                       \
            ++gFailed;                                                                         \
        } else {                                                                               \
            ++gPassed;                                                                         \
        }                                                                                      \
    } while (0)

#define EXPECT_NO_ERRORS(report)                                                                  \
    do {                                                                                          \
        int _n = countIssues(report, Severity::Error);                                            \
        if (_n > 0) {                                                                             \
            std::cerr << "  FAIL " << __FILE__ << ":" << __LINE__ << "  expected no errors, got " \
                      << _n << "\n";                                                              \
            for (auto &_i : (report).issues)                                                      \
                if (_i.severity == Severity::Error)                                               \
                    std::cerr << "    ERROR [" << _i.path << "] " << _i.message << "\n";          \
            ++gFailed;                                                                            \
        } else {                                                                                  \
            ++gPassed;                                                                            \
        }                                                                                         \
    } while (0)

#define EXPECT_ISSUE_MSG(report, sev, pathSubstr, msgSubstr)                       \
    do {                                                                           \
        if (!hasIssueMsg(report, sev, pathSubstr, msgSubstr)) {                    \
            std::cerr << "  FAIL " << __FILE__ << ":" << __LINE__ << "  expected " \
                      << severityLabel(sev) << " at '" << pathSubstr               \
                      << "' with message containing '" << msgSubstr << "'\n";      \
            ++gFailed;                                                             \
        } else {                                                                   \
            ++gPassed;                                                             \
        }                                                                          \
    } while (0)

// ============================================================================
// 2. File & JSON parsing tests
// ============================================================================

TEST(file_missing)
{
    Validator v;
    ValidationReport report;
    bool ok = v.validate("/nonexistent/path/to/file.json", report);
    EXPECT_FALSE(ok);
    EXPECT_TRUE(hasIssue(report, Severity::Error, "(file)"));
}

TEST(file_json_syntax_error)
{
    std::string json = "{ this is not valid JSON !!!";
    auto r = validateJson(json);
    EXPECT_TRUE(r.hasErrors());
    EXPECT_ISSUE_MSG(r, Severity::Error, "(root)", "JSON syntax error");
}

TEST(file_root_not_object)
{
    auto r = validateJson("[1, 2, 3]");
    EXPECT_TRUE(r.hasErrors());
    EXPECT_ISSUE_MSG(r, Severity::Error, "(root)", "object");
}

TEST(file_missing_canvas_key)
{
    auto r = validateJson("{\"notCanvas\": {}}");
    EXPECT_TRUE(r.hasErrors());
    EXPECT_ISSUE_MSG(r, Severity::Error, "(root)", "Missing required key \"canvas\"");
}

TEST(file_canvas_not_object)
{
    auto r = validateJson("{\"canvas\": 42}");
    EXPECT_TRUE(r.hasErrors());
    EXPECT_ISSUE_MSG(r, Severity::Error, "canvas", "must be an object");
}

TEST(file_unknown_top_level_key_typo)
{
    auto r = validateJson("{\"canvass\": {\"width\": 800, \"height\": 600}}");
    EXPECT_TRUE(r.hasErrors());
    EXPECT_ISSUE(r, Severity::Hint, "canvass");
}

TEST(file_valid_minimal_passes)
{
    auto r = validateJson("{\"canvas\": {\"width\": 800, \"height\": 600, \"dpiX\": 300, \"dpiY\": 300}}");
    EXPECT_NO_ERRORS(r);
}

// ============================================================================
// 3. Canvas validation tests
// ============================================================================

TEST(canvas_width_not_number)
{
    auto r = validateJson("{\"canvas\": {\"width\": \"big\", \"height\": 600, \"dpiX\": 300, \"dpiY\": 300}}");
    EXPECT_ISSUE_MSG(r, Severity::Error, "canvas.width", "must be a number");
}

TEST(canvas_width_zero)
{
    auto r = validateJson("{\"canvas\": {\"width\": 0, \"height\": 600, \"dpiX\": 300, \"dpiY\": 300}}");
    EXPECT_ISSUE_MSG(r, Severity::Error, "canvas.width", "must be positive");
}

TEST(canvas_width_negative)
{
    auto r = validateJson("{\"canvas\": {\"width\": -100, \"height\": 600, \"dpiX\": 300, \"dpiY\": 300}}");
    EXPECT_ISSUE_MSG(r, Severity::Error, "canvas.width", "must be positive");
}

TEST(canvas_height_not_number)
{
    auto r = validateJson("{\"canvas\": {\"width\": 800, \"height\": true, \"dpiX\": 300, \"dpiY\": 300}}");
    EXPECT_ISSUE_MSG(r, Severity::Error, "canvas.height", "must be a number");
}

TEST(canvas_height_negative)
{
    auto r = validateJson("{\"canvas\": {\"width\": 800, \"height\": -50, \"dpiX\": 300, \"dpiY\": 300}}");
    EXPECT_ISSUE_MSG(r, Severity::Error, "canvas.height", "must be positive");
}

TEST(canvas_dpi_deprecated)
{
    auto r = validateJson("{\"canvas\": {\"width\": 800, \"height\": 600, \"dpi\": 300}}");
    EXPECT_ISSUE_MSG(r, Severity::Error, "canvas.dpi", "deprecated");
}

TEST(canvas_dpiX_not_number)
{
    auto r = validateJson("{\"canvas\": {\"width\": 800, \"height\": 600, \"dpiX\": \"high\"}}");
    EXPECT_ISSUE_MSG(r, Severity::Error, "canvas.dpiX", "must be a number");
}

TEST(canvas_dpiX_zero)
{
    auto r = validateJson("{\"canvas\": {\"width\": 800, \"height\": 600, \"dpiX\": 0}}");
    EXPECT_ISSUE_MSG(r, Severity::Error, "canvas.dpiX", "must be positive");
}

TEST(canvas_dpiX_too_low)
{
    auto r = validateJson("{\"canvas\": {\"width\": 800, \"height\": 600, \"dpiX\": 50}}");
    EXPECT_ISSUE_MSG(r, Severity::Warning, "canvas.dpiX", "very low");
}

TEST(canvas_dpiY_too_high)
{
    auto r = validateJson("{\"canvas\": {\"width\": 800, \"height\": 600, \"dpiY\": 3000}}");
    EXPECT_ISSUE_MSG(r, Severity::Warning, "canvas.dpiY", "very high");
}

TEST(canvas_dpi_xy_both_valid_no_issue)
{
    // test_harness.h 没有 EXPECT_ISSUE_COUNT，按现有断言风格检查：
    // 两个字段均不应产生 Error / Hint issue（包括 Unknown field 提示）。
    auto r = validateJson(
        "{\"canvas\": {\"width\": 800, \"height\": 600, \"dpiX\": 300, \"dpiY\": 600}}");
    EXPECT_FALSE(hasIssue(r, Severity::Error, "canvas.dpiX"));
    EXPECT_FALSE(hasIssue(r, Severity::Error, "canvas.dpiY"));
    EXPECT_FALSE(hasIssue(r, Severity::Hint, "canvas.dpiX"));
    EXPECT_FALSE(hasIssue(r, Severity::Hint, "canvas.dpiY"));
}

TEST(canvas_missing_background_is_hint)
{
    auto r = validateJson("{\"canvas\": {\"width\": 800, \"height\": 600, \"dpiX\": 300, \"dpiY\": 300}}");
    EXPECT_ISSUE_MSG(r, Severity::Hint, "canvas", "No \"background\" specified");
}

TEST(canvas_background_wrong_type)
{
    auto r = validateJson(
        "{\"canvas\": {\"width\": 800, \"height\": 600, \"dpiX\": 300, \"dpiY\": 300, \"background\": \"red\"}}");
    EXPECT_ISSUE_MSG(r, Severity::Error, "canvas.background", "must be an object");
}

// ============================================================================
// 4. Color validation tests
// ============================================================================

TEST(color_valid_cmyk_passes)
{
    auto r = validateJson("{\"canvas\": {\"width\": 800, \"height\": 600, \"dpiX\": 300, \"dpiY\": 300,"
                          "  \"background\": {\"c\": 0, \"m\": 50, \"y\": 100, \"k\": 0, \"a\": 1.0}}}");
    EXPECT_NO_ERRORS(r);
}

TEST(color_space_field_removed)
{
    auto r = validateJson("{\"canvas\": {\"width\": 800, \"height\": 600, \"dpiX\": 300, \"dpiY\": 300,"
                          "  \"background\": {\"space\": \"cmyk\", \"c\": 0, \"m\": 0, \"y\": 0, \"k\": 0}}}");
    EXPECT_ISSUE_MSG(r, Severity::Error, "background.space", "removed");
}

TEST(color_rgb_removed)
{
    auto r = validateJson("{\"canvas\": {\"width\": 800, \"height\": 600, \"dpiX\": 300, \"dpiY\": 300,"
                          "  \"background\": {\"r\": 255, \"g\": 0, \"b\": 0}}}");
    EXPECT_ISSUE_MSG(r, Severity::Error, "background.r", "RGB");
}

TEST(color_cmyk_c_below_zero)
{
    auto r = validateJson(
        "{\"canvas\": {\"width\": 800, \"height\": 600, \"dpiX\": 300, \"dpiY\": 300,"
        "  \"background\": {\"c\": -5, \"m\": 0, \"y\": 0, \"k\": 0}}}");
    EXPECT_ISSUE_MSG(r, Severity::Warning, "background.c", "outside the normal range");
}

TEST(color_cmyk_m_above_100)
{
    auto r = validateJson(
        "{\"canvas\": {\"width\": 800, \"height\": 600, \"dpiX\": 300, \"dpiY\": 300,"
        "  \"background\": {\"c\": 0, \"m\": 150, \"y\": 0, \"k\": 0}}}");
    EXPECT_ISSUE_MSG(r, Severity::Warning, "background.m", "outside the normal range");
}

TEST(color_alpha_above_1)
{
    auto r = validateJson(
        "{\"canvas\": {\"width\": 800, \"height\": 600, \"dpiX\": 300, \"dpiY\": 300,"
        "  \"background\": {\"c\": 0, \"m\": 0, \"y\": 0, \"k\": 100, \"a\": 1.5}}}");
    EXPECT_ISSUE_MSG(r, Severity::Error, "background.a", "outside the valid range");
}

TEST(color_alpha_below_0)
{
    auto r = validateJson(
        "{\"canvas\": {\"width\": 800, \"height\": 600, \"dpiX\": 300, \"dpiY\": 300,"
        "  \"background\": {\"c\": 0, \"m\": 0, \"y\": 0, \"k\": 100, \"a\": -0.5}}}");
    EXPECT_ISSUE_MSG(r, Severity::Error, "background.a", "outside the valid range");
}

TEST(color_alpha_zero_hint)
{
    auto r = validateJson(
        "{\"canvas\": {\"width\": 800, \"height\": 600, \"dpiX\": 300, \"dpiY\": 300,"
        "  \"background\": {\"c\": 0, \"m\": 0, \"y\": 0, \"k\": 100, \"a\": 0.0}}}");
    EXPECT_ISSUE_MSG(r, Severity::Hint, "background.a", "completely invisible");
}

TEST(color_not_an_object)
{
    auto r = validateJson("{\"canvas\": {\"width\": 800, \"height\": 600, \"dpiX\": 300, \"dpiY\": 300,"
                          "  \"background\": [0, 0, 0]}}");
    EXPECT_ISSUE_MSG(r, Severity::Error, "canvas.background", "must be an object");
}

TEST(color_missing_cmyk_paper_white_hint)
{
    // 颜色对象缺少全部 c/m/y/k（如 {} 或仅含 a）时渲染为纸白（全 0）：
    // 仅给 Hint 提示，不产生 Error。
    auto r1 = validateJson(
        "{\"canvas\": {\"width\": 800, \"height\": 600, \"dpiX\": 300, \"dpiY\": 300,"
        "  \"background\": {\"a\": 0.5}}}");
    EXPECT_ISSUE_MSG(r1, Severity::Hint, "background", "paper white");
    EXPECT_NO_ERRORS(r1);

    auto r2 = validateJson(
        "{\"canvas\": {\"width\": 800, \"height\": 600, \"dpiX\": 300, \"dpiY\": 300,"
        "  \"background\": {}}}");
    EXPECT_ISSUE_MSG(r2, Severity::Hint, "background", "none of c/m/y/k");
    EXPECT_NO_ERRORS(r2);
}

TEST(color_unknown_field_gets_hint)
{
    auto r = validateJson(
        "{\"canvas\": {\"width\": 800, \"height\": 600, \"dpiX\": 300, \"dpiY\": 300,"
        "  \"background\": {\"c\": 0, \"m\": 0, \"y\": 0, \"k\": 0,"
        "    \"opacity\": 0.5}}}");
    EXPECT_ISSUE(r, Severity::Hint, "background.opacity");
}

// ============================================================================
// 5. Gradient validation tests
// ============================================================================

TEST(gradient_valid_linear_passes)
{
    auto r = validateJson(
        "{\"canvas\": {\"width\": 800, \"height\": 600, \"dpiX\": 300, \"dpiY\": 300,"
        "  \"rects\": [{\"x\": 0, \"y\": 0, \"width\": 100, \"height\": 100,"
        "    \"gradient\": {\"type\": \"linear\", \"x1\": 0, \"y1\": 0, \"x2\": 1, \"y2\": 0,"
        "        \"stops\": ["
        "          {\"offset\": 0.0, \"color\": {\"c\": 0, \"m\": 100, \"y\": 100, \"k\": 0}},"
        "          {\"offset\": 1.0, \"color\": {\"c\": 100, \"m\": 100, \"y\": 0, \"k\": 0}}"
        "        ]}}]}}");
    EXPECT_NO_ERRORS(r);
}

TEST(gradient_valid_radial_passes)
{
    auto r = validateJson(
        "{\"canvas\": {\"width\": 800, \"height\": 600, \"dpiX\": 300, \"dpiY\": 300,"
        "  \"rects\": [{\"x\": 0, \"y\": 0, \"width\": 100, \"height\": 100,"
        "    \"gradient\": {\"type\": \"radial\", \"cx\": 0.5, \"cy\": 0.5, \"r\": 0.5,"
        "        \"stops\": ["
        "          {\"offset\": 0.0, \"color\": {\"c\": 0, \"m\": 0, \"y\": 0, \"k\": 0}},"
        "          {\"offset\": 1.0, \"color\": {\"c\": 0, \"m\": 0, \"y\": 0, \"k\": 100}}"
        "        ]}}]}}");
    EXPECT_NO_ERRORS(r);
}

TEST(gradient_missing_type)
{
    auto r = validateJson(
        "{\"canvas\": {\"width\": 800, \"height\": 600, \"dpiX\": 300, \"dpiY\": 300,"
        "  \"rects\": [{\"x\": 0, \"y\": 0, \"width\": 100, \"height\": 100,"
        "    \"gradient\": {\"stops\": ["
        "        {\"offset\": 0.0, \"color\": {\"c\": 0, \"m\": 100, \"y\": 100, \"k\": 0}}"
        "      ]}}]}}");
    EXPECT_ISSUE_MSG(r, Severity::Error, ".gradient", "missing required field \"type\"");
}

TEST(gradient_invalid_type)
{
    auto r = validateJson(
        "{\"canvas\": {\"width\": 800, \"height\": 600, \"dpiX\": 300, \"dpiY\": 300,"
        "  \"rects\": [{\"x\": 0, \"y\": 0, \"width\": 100, \"height\": 100,"
        "    \"gradient\": {\"type\": \"circular\","
        "        \"stops\": ["
        "          {\"offset\": 0.0, \"color\": {\"c\": 0, \"m\": 0, \"y\": 0, \"k\": 100}}"
        "        ]}}]}}");
    EXPECT_ISSUE_MSG(r, Severity::Error, ".gradient.type", "Unknown gradient type");
}

TEST(gradient_missing_stops)
{
    auto r = validateJson("{\"canvas\": {\"width\": 800, \"height\": 600, \"dpiX\": 300, \"dpiY\": 300,"
                          "  \"rects\": [{\"x\": 0, \"y\": 0, \"width\": 100, \"height\": 100,"
                          "    \"gradient\": {\"type\": \"linear\"}}]}}");
    EXPECT_ISSUE_MSG(r, Severity::Error, ".gradient", "missing required field \"stops\"");
}

TEST(gradient_stops_not_array)
{
    auto r =
        validateJson("{\"canvas\": {\"width\": 800, \"height\": 600, \"dpiX\": 300, \"dpiY\": 300,"
                     "  \"rects\": [{\"x\": 0, \"y\": 0, \"width\": 100, \"height\": 100,"
                     "    \"gradient\": {\"type\": \"linear\", \"stops\": \"not-an-array\"}}]}}");
    EXPECT_ISSUE_MSG(r, Severity::Error, ".stops", "must be an array");
}

TEST(gradient_too_few_stops)
{
    auto r = validateJson(
        "{\"canvas\": {\"width\": 800, \"height\": 600, \"dpiX\": 300, \"dpiY\": 300,"
        "  \"rects\": [{\"x\": 0, \"y\": 0, \"width\": 100, \"height\": 100,"
        "    \"gradient\": {\"type\": \"linear\", \"stops\": ["
        "        {\"offset\": 0.0, \"color\": {\"c\": 0, \"m\": 100, \"y\": 100, \"k\": 0}}"
        "      ]}}]}}");
    EXPECT_ISSUE_MSG(r, Severity::Error, ".stops", "at least 2 stops");
}

TEST(gradient_offset_out_of_range)
{
    auto r = validateJson(
        "{\"canvas\": {\"width\": 800, \"height\": 600, \"dpiX\": 300, \"dpiY\": 300,"
        "  \"rects\": [{\"x\": 0, \"y\": 0, \"width\": 100, \"height\": 100,"
        "    \"gradient\": {\"type\": \"linear\","
        "        \"stops\": ["
        "          {\"offset\": -0.1, \"color\": {\"c\": 0, \"m\": 100, \"y\": 100, \"k\": 0}},"
        "          {\"offset\": 1.5, \"color\": {\"c\": 100, \"m\": 100, \"y\": 0, \"k\": 0}}"
        "        ]}}]}}");
    EXPECT_ISSUE_MSG(r, Severity::Error, "offset", "out of range");
}

TEST(gradient_stops_not_sorted)
{
    auto r = validateJson(
        "{\"canvas\": {\"width\": 800, \"height\": 600, \"dpiX\": 300, \"dpiY\": 300,"
        "  \"rects\": [{\"x\": 0, \"y\": 0, \"width\": 100, \"height\": 100,"
        "    \"gradient\": {\"type\": \"linear\","
        "        \"stops\": ["
        "          {\"offset\": 0.8, \"color\": {\"c\": 0, \"m\": 100, \"y\": 100, \"k\": 0}},"
        "          {\"offset\": 0.2, \"color\": {\"c\": 100, \"m\": 100, \"y\": 0, \"k\": 0}}"
        "        ]}}]}}");
    EXPECT_ISSUE_MSG(r, Severity::Warning, ".stops", "not sorted");
}

TEST(gradient_radial_r_not_positive)
{
    auto r = validateJson(
        "{\"canvas\": {\"width\": 800, \"height\": 600, \"dpiX\": 300, \"dpiY\": 300,"
        "  \"rects\": [{\"x\": 0, \"y\": 0, \"width\": 100, \"height\": 100,"
        "    \"gradient\": {\"type\": \"radial\", \"r\": 0,"
        "        \"stops\": ["
        "          {\"offset\": 0.0, \"color\": {\"c\": 0, \"m\": 100, \"y\": 100, \"k\": 0}},"
        "          {\"offset\": 1.0, \"color\": {\"c\": 100, \"m\": 100, \"y\": 0, \"k\": 0}}"
        "        ]}}]}}");
    EXPECT_ISSUE_MSG(r, Severity::Warning, ".gradient.r", "must be positive");
}

// ============================================================================
// 5b. Fill validation tests (fillColor vs gradient mutual exclusion)
// ============================================================================

TEST(fill_solid_only_passes)
{
    auto r = validateJson(
        "{\"canvas\": {\"width\": 800, \"height\": 600, \"dpiX\": 300, \"dpiY\": 300,"
        "  \"rects\": [{\"x\": 0, \"y\": 0, \"width\": 100, \"height\": 50,"
        "    \"fillColor\": {\"c\": 0, \"m\": 0, \"y\": 0, \"k\": 100}}]}}");
    EXPECT_NO_ERRORS(r);
}

TEST(fill_gradient_only_passes)
{
    auto r = validateJson(
        "{\"canvas\": {\"width\": 800, \"height\": 600, \"dpiX\": 300, \"dpiY\": 300,"
        "  \"rects\": [{\"x\": 0, \"y\": 0, \"width\": 100, \"height\": 50,"
        "    \"gradient\": {\"type\": \"linear\", \"x1\": 0, \"y1\": 0, \"x2\": 1, \"y2\": 0,"
        "      \"stops\": ["
        "        {\"offset\": 0.0, \"color\": {\"c\": 0, \"m\": 100, \"y\": 100, \"k\": 0}},"
        "        {\"offset\": 1.0, \"color\": {\"c\": 100, \"m\": 100, \"y\": 0, \"k\": 0}}"
        "      ]}}]}}");
    EXPECT_NO_ERRORS(r);
}

TEST(fill_neither_passes)
{
    auto r = validateJson("{\"canvas\": {\"width\": 800, \"height\": 600, \"dpiX\": 300, \"dpiY\": 300,"
                          "  \"rects\": [{\"x\": 0, \"y\": 0, \"width\": 100, \"height\": 50}]}}");
    EXPECT_NO_ERRORS(r);
}

TEST(fill_both_present_is_error)
{
    auto r = validateJson(
        "{\"canvas\": {\"width\": 800, \"height\": 600, \"dpiX\": 300, \"dpiY\": 300,"
        "  \"rects\": [{\"x\": 0, \"y\": 0, \"width\": 100, \"height\": 50,"
        "    \"fillColor\": {\"c\": 0, \"m\": 0, \"y\": 0, \"k\": 100},"
        "    \"gradient\": {\"type\": \"linear\","
        "      \"stops\": ["
        "        {\"offset\": 0.0, \"color\": {\"c\": 0, \"m\": 100, \"y\": 100, \"k\": 0}},"
        "        {\"offset\": 1.0, \"color\": {\"c\": 100, \"m\": 100, \"y\": 0, \"k\": 0}}"
        "      ]}}]}}");
    EXPECT_ISSUE_MSG(r, Severity::Error, "rects[0]", "mutually exclusive");
}

TEST(fill_gradient_nested_in_color_fails)
{
    auto r = validateJson(
        "{\"canvas\": {\"width\": 800, \"height\": 600, \"dpiX\": 300, \"dpiY\": 300,"
        "  \"rects\": [{\"x\": 0, \"y\": 0, \"width\": 100, \"height\": 50,"
        "    \"fillColor\": {\"c\": 0, \"m\": 0, \"y\": 0, \"k\": 100,"
        "      \"gradient\": {\"type\": \"linear\","
        "        \"stops\": ["
        "          {\"offset\": 0.0, \"color\": {\"c\": 0, \"m\": 100, \"y\": 100, \"k\": 0}},"
        "          {\"offset\": 1.0, \"color\": {\"c\": 100, \"m\": 100, \"y\": 0, \"k\": 0}}"
        "        ]}}}]}}");
    EXPECT_ISSUE_MSG(r, Severity::Error, "fillColor.gradient", "must not be nested");
}

TEST(fill_circle_variants)
{
    // Circle with solid fill only
    auto r1 = validateJson(
        "{\"canvas\": {\"width\": 800, \"height\": 600, \"dpiX\": 300, \"dpiY\": 300,"
        "  \"circles\": [{\"cx\": 100, \"cy\": 100, \"radiusX\": 50,"
        "    \"fillColor\": {\"c\": 0, \"m\": 0, \"y\": 0, \"k\": 100}}]}}");
    EXPECT_NO_ERRORS(r1);

    // Circle with gradient only
    auto r2 = validateJson(
        "{\"canvas\": {\"width\": 800, \"height\": 600, \"dpiX\": 300, \"dpiY\": 300,"
        "  \"circles\": [{\"cx\": 100, \"cy\": 100, \"radiusX\": 50,"
        "    \"gradient\": {\"type\": \"radial\","
        "      \"stops\": ["
        "        {\"offset\": 0.0, \"color\": {\"c\": 0, \"m\": 100, \"y\": 100, \"k\": 0}},"
        "        {\"offset\": 1.0, \"color\": {\"c\": 100, \"m\": 100, \"y\": 0, \"k\": 0}}"
        "      ]}}]}}");
    EXPECT_NO_ERRORS(r2);

    // Circle with both → error
    auto r3 = validateJson(
        "{\"canvas\": {\"width\": 800, \"height\": 600, \"dpiX\": 300, \"dpiY\": 300,"
        "  \"circles\": [{\"cx\": 100, \"cy\": 100, \"radiusX\": 50,"
        "    \"fillColor\": {\"c\": 0, \"m\": 0, \"y\": 0, \"k\": 100},"
        "    \"gradient\": {\"type\": \"radial\","
        "      \"stops\": ["
        "        {\"offset\": 0.0, \"color\": {\"c\": 0, \"m\": 100, \"y\": 100, \"k\": 0}},"
        "        {\"offset\": 1.0, \"color\": {\"c\": 100, \"m\": 100, \"y\": 0, \"k\": 0}}"
        "      ]}}]}}");
    EXPECT_ISSUE_MSG(r3, Severity::Error, "circles[0]", "mutually exclusive");
}

TEST(fill_color_wrong_type_is_error)
{
    auto r = validateJson(
        "{\"canvas\": {\"width\": 800, \"height\": 600, \"dpiX\": 300, \"dpiY\": 300,"
        "  \"rects\": [{\"x\": 0, \"y\": 0, \"width\": 100, \"height\": 50,"
        "    \"fillColor\": \"red\"}]}}");
    EXPECT_ISSUE_MSG(r, Severity::Error, "rects[0].fillColor", "must be an object");
}

// ============================================================================
// 6. Rect validation tests
// ============================================================================

TEST(rect_valid_passes)
{
    auto r = validateJson(
        "{\"canvas\": {\"width\": 800, \"height\": 600, \"dpiX\": 300, \"dpiY\": 300,"
        "  \"rects\": [{\"x\": 10, \"y\": 20, \"width\": 100, \"height\": 50,"
        "    \"fillColor\": {\"c\": 0, \"m\": 100, \"y\": 100, \"k\": 0}}]}}");
    EXPECT_NO_ERRORS(r);
}

TEST(rect_zero_width_warns)
{
    auto r = validateJson("{\"canvas\": {\"width\": 800, \"height\": 600, \"dpiX\": 300, \"dpiY\": 300,"
                          "  \"rects\": [{\"x\": 0, \"y\": 0, \"width\": 0, \"height\": 50}]}}");
    EXPECT_ISSUE_MSG(r, Severity::Warning, "rects[0].width", "zero or negative");
}

TEST(rect_negative_height_warns)
{
    auto r = validateJson("{\"canvas\": {\"width\": 800, \"height\": 600, \"dpiX\": 300, \"dpiY\": 300,"
                          "  \"rects\": [{\"x\": 0, \"y\": 0, \"width\": 100, \"height\": -10}]}}");
    EXPECT_ISSUE_MSG(r, Severity::Warning, "rects[0].height", "zero or negative");
}

TEST(rect_missing_width_warns)
{
    auto r = validateJson("{\"canvas\": {\"width\": 800, \"height\": 600, \"dpiX\": 300, \"dpiY\": 300,"
                          "  \"rects\": [{\"x\": 0, \"y\": 0, \"height\": 50}]}}");
    EXPECT_ISSUE_MSG(r, Severity::Warning, "rects[0]", "missing \"width\"");
}

TEST(rect_negative_stroke_width)
{
    auto r = validateJson("{\"canvas\": {\"width\": 800, \"height\": 600, \"dpiX\": 300, \"dpiY\": 300,"
                          "  \"rects\": [{\"x\": 0, \"y\": 0, \"width\": 100, \"height\": 50, "
                          "\"strokeWidth\": -2}]}}");
    EXPECT_ISSUE_MSG(r, Severity::Warning, "rects[0].strokeWidth", "negative");
}

TEST(rect_invalid_line_style)
{
    auto r = validateJson("{\"canvas\": {\"width\": 800, \"height\": 600, \"dpiX\": 300, \"dpiY\": 300,"
                          "  \"rects\": [{\"x\": 0, \"y\": 0, \"width\": 100, \"height\": 50,"
                          "    \"lineStyle\": \"double\"}]}}");
    EXPECT_ISSUE_MSG(r, Severity::Error, "rects[0].lineStyle", "Unknown line style");
}

TEST(rect_corner_radius_too_large_hint)
{
    auto r = validateJson("{\"canvas\": {\"width\": 800, \"height\": 600, \"dpiX\": 300, \"dpiY\": 300,"
                          "  \"rects\": [{\"x\": 0, \"y\": 0, \"width\": 100, \"height\": 50,"
                          "    \"cornerRadius\": 40}]}}");
    EXPECT_ISSUE_MSG(r, Severity::Hint, "rects[0].cornerRadius", "exceeds half");
}

TEST(rect_rotation_unknown_field)
{
    auto r = validateJson("{\"canvas\": {\"width\": 800, \"height\": 600, \"dpiX\": 300, \"dpiY\": 300,"
                          "  \"rects\": [{\"x\": 0, \"y\": 0, \"width\": 100, \"height\": 50,"
                          "    \"rotation\": 400}]}}");
    EXPECT_ISSUE_MSG(r, Severity::Hint, "rects[0].rotation", "Unknown field");
}

TEST(rect_unknown_field_typo)
{
    auto r = validateJson(
        "{\"canvas\": {\"width\": 800, \"height\": 600, \"dpiX\": 300, \"dpiY\": 300,"
        "  \"rects\": [{\"x\": 0, \"y\": 0, \"width\": 100, \"height\": 50,"
        "    \"storkeColor\": {\"c\": 0, \"m\": 0, \"y\": 0, \"k\": 100}}]}}");
    EXPECT_ISSUE_MSG(r, Severity::Hint, "rects[0].storkeColor", "Did you mean");
}

// ============================================================================
// 7. Circle validation tests
// ============================================================================

TEST(circle_valid_passes)
{
    auto r = validateJson(
        "{\"canvas\": {\"width\": 800, \"height\": 600, \"dpiX\": 300, \"dpiY\": 300,"
        "  \"circles\": [{\"cx\": 100, \"cy\": 100, \"radiusX\": 50,"
        "    \"fillColor\": {\"c\": 0, \"m\": 100, \"y\": 100, \"k\": 0}}]}}");
    EXPECT_NO_ERRORS(r);
}

TEST(circle_missing_radius_x_is_error)
{
    auto r = validateJson("{\"canvas\": {\"width\": 800, \"height\": 600, \"dpiX\": 300, \"dpiY\": 300,"
                          "  \"circles\": [{\"cx\": 100, \"cy\": 100}]}}");
    EXPECT_ISSUE_MSG(r, Severity::Error, "circles[0]", "missing required field \"radiusX\"");
}

TEST(circle_radius_x_zero)
{
    auto r = validateJson("{\"canvas\": {\"width\": 800, \"height\": 600, \"dpiX\": 300, \"dpiY\": 300,"
                          "  \"circles\": [{\"cx\": 100, \"cy\": 100, \"radiusX\": 0}]}}");
    EXPECT_ISSUE_MSG(r, Severity::Error, "circles[0].radiusX", "must be positive");
}

TEST(circle_radius_y_negative)
{
    auto r = validateJson(
        "{\"canvas\": {\"width\": 800, \"height\": 600, \"dpiX\": 300, \"dpiY\": 300,"
        "  \"circles\": [{\"cx\": 100, \"cy\": 100, \"radiusX\": 50, \"radiusY\": -5}]}}");
    EXPECT_ISSUE_MSG(r, Severity::Warning, "circles[0].radiusY", "must be positive");
}

TEST(circle_negative_stroke_width)
{
    auto r = validateJson(
        "{\"canvas\": {\"width\": 800, \"height\": 600, \"dpiX\": 300, \"dpiY\": 300,"
        "  \"circles\": [{\"cx\": 100, \"cy\": 100, \"radiusX\": 50, \"strokeWidth\": -1}]}}");
    EXPECT_ISSUE_MSG(r, Severity::Warning, "circles[0].strokeWidth", "negative");
}

// ============================================================================
// 8. FreeLine validation tests
// ============================================================================

TEST(freeline_valid_passes)
{
    auto r = validateJson(
        "{\"canvas\": {\"width\": 800, \"height\": 600, \"dpiX\": 300, \"dpiY\": 300,"
        "  \"freeLines\": [{\"points\": [{\"x\": 0, \"y\": 0}, {\"x\": 100, \"y\": 100},"
        "    {\"x\": 200, \"y\": 50}],"
        "    \"strokeColor\": {\"c\": 0, \"m\": 0, \"y\": 0, \"k\": 100}}]}}");
    EXPECT_NO_ERRORS(r);
}

TEST(freeline_missing_points)
{
    auto r = validateJson("{\"canvas\": {\"width\": 800, \"height\": 600, \"dpiX\": 300, \"dpiY\": 300,"
                          "  \"freeLines\": [{\"strokeColor\": {\"c\": 0, \"m\": 0, \"y\": 0, \"k\": 100}}]}}");
    EXPECT_ISSUE_MSG(r, Severity::Error, "freeLines[0]", "missing required field \"points\"");
}

TEST(freeline_points_not_array)
{
    auto r = validateJson("{\"canvas\": {\"width\": 800, \"height\": 600, \"dpiX\": 300, \"dpiY\": 300,"
                          "  \"freeLines\": [{\"points\": \"not-an-array\"}]}}");
    EXPECT_ISSUE_MSG(r, Severity::Error, "freeLines[0].points", "must be an array");
}

TEST(freeline_too_few_points)
{
    auto r = validateJson("{\"canvas\": {\"width\": 800, \"height\": 600, \"dpiX\": 300, \"dpiY\": 300,"
                          "  \"freeLines\": [{\"points\": [{\"x\": 0, \"y\": 0}]}]}}");
    EXPECT_ISSUE_MSG(r, Severity::Error, "freeLines[0].points", "at least 2 points");
}

TEST(freeline_point_missing_x)
{
    auto r = validateJson("{\"canvas\": {\"width\": 800, \"height\": 600, \"dpiX\": 300, \"dpiY\": 300,"
                          "  \"freeLines\": [{\"points\": [{\"x\": 0, \"y\": 0}, {\"y\": 50}]}]}}");
    EXPECT_ISSUE_MSG(r, Severity::Error, "points[1]", "missing required field \"x\"");
}

TEST(freeline_point_x_not_number)
{
    auto r = validateJson(
        "{\"canvas\": {\"width\": 800, \"height\": 600, \"dpiX\": 300, \"dpiY\": 300,"
        "  \"freeLines\": [{\"points\": [{\"x\": 0, \"y\": 0}, {\"x\": \"ten\", \"y\": 50}]}]}}");
    EXPECT_ISSUE_MSG(r, Severity::Error, "points[1].x", "must be a number");
}

// ============================================================================
// 9. BezierCurve validation tests
// ============================================================================

TEST(bezier_valid_passes)
{
    auto r = validateJson(
        "{\"canvas\": {\"width\": 800, \"height\": 600, \"dpiX\": 300, \"dpiY\": 300,"
        "  \"bezierCurves\": [{\"controlPoints\": ["
        "    {\"x\": 0, \"y\": 0}, {\"x\": 50, \"y\": 100},"
        "    {\"x\": 100, \"y\": 100}, {\"x\": 150, \"y\": 0}],"
        "    \"strokeColor\": {\"c\": 0, \"m\": 0, \"y\": 0, \"k\": 100}}]}}");
    EXPECT_NO_ERRORS(r);
}

TEST(bezier_missing_control_points)
{
    auto r = validateJson("{\"canvas\": {\"width\": 800, \"height\": 600, \"dpiX\": 300, \"dpiY\": 300,"
                          "  \"bezierCurves\": [{\"strokeColor\": {\"c\": 0, \"m\": 0, \"y\": 0, \"k\": 100}}]}}");
    EXPECT_ISSUE_MSG(
        r, Severity::Error, "bezierCurves[0]", "missing required field \"controlPoints\"");
}

TEST(bezier_nonstandard_point_count)
{
    // 5 points: (5-1)=4, 4%3=1 -- not standard (2, 3, 4, 7, 10, ...)
    auto r =
        validateJson("{\"canvas\": {\"width\": 800, \"height\": 600, \"dpiX\": 300, \"dpiY\": 300,"
                     "  \"bezierCurves\": [{\"controlPoints\": ["
                     "    {\"x\": 0, \"y\": 0}, {\"x\": 10, \"y\": 10}, {\"x\": 20, \"y\": 10},"
                     "    {\"x\": 30, \"y\": 0}, {\"x\": 40, \"y\": 0}]}]}}");
    EXPECT_ISSUE_MSG(r, Severity::Hint, "controlPoints", "not a standard count");
}

// ============================================================================
// 10. Line validation tests
// ============================================================================

TEST(line_valid_passes)
{
    auto r = validateJson(
        "{\"canvas\": {\"width\": 800, \"height\": 600, \"dpiX\": 300, \"dpiY\": 300,"
        "  \"lines\": [{\"x1\": 10, \"y1\": 20, \"x2\": 100, \"y2\": 200,"
        "    \"strokeColor\": {\"c\": 0, \"m\": 0, \"y\": 0, \"k\": 100}}]}}");
    EXPECT_NO_ERRORS(r);
}

TEST(line_degenerate_warns)
{
    auto r = validateJson(
        "{\"canvas\": {\"width\": 800, \"height\": 600, \"dpiX\": 300, \"dpiY\": 300,"
        "  \"lines\": [{\"x1\": 50, \"y1\": 50, \"x2\": 50, \"y2\": 50,"
        "    \"strokeColor\": {\"c\": 0, \"m\": 0, \"y\": 0, \"k\": 100}}]}}");
    EXPECT_ISSUE_MSG(r, Severity::Warning, "lines[0]", "degenerates to a point");
}

TEST(line_invalid_line_style)
{
    auto r = validateJson("{\"canvas\": {\"width\": 800, \"height\": 600, \"dpiX\": 300, \"dpiY\": 300,"
                          "  \"lines\": [{\"x1\": 0, \"y1\": 0, \"x2\": 100, \"y2\": 100,"
                          "    \"lineStyle\": \"wavy\"}]}}");
    EXPECT_ISSUE_MSG(r, Severity::Error, "lines[0].lineStyle", "Unknown line style");
}

// ============================================================================
// 11. Text validation tests
// ============================================================================

TEST(text_valid_passes)
{
    auto r = validateJson("{\"canvas\": {\"width\": 800, \"height\": 600, \"dpiX\": 300, \"dpiY\": 300,"
                          "  \"texts\": [{\"x\": 50, \"y\": 100, \"content\": \"Hello World\","
                          "    \"fontFamily\": \"SimSun\", \"fontSize\": 16}]}}");
    EXPECT_NO_ERRORS(r);
}

TEST(text_missing_content)
{
    auto r = validateJson("{\"canvas\": {\"width\": 800, \"height\": 600, \"dpiX\": 300, \"dpiY\": 300,"
                          "  \"texts\": [{\"x\": 50, \"y\": 100}]}}");
    EXPECT_ISSUE_MSG(r, Severity::Error, "texts[0]", "missing required field \"content\"");
}

TEST(text_empty_content)
{
    auto r = validateJson("{\"canvas\": {\"width\": 800, \"height\": 600, \"dpiX\": 300, \"dpiY\": 300,"
                          "  \"texts\": [{\"x\": 50, \"y\": 100, \"content\": \"\"}]}}");
    EXPECT_ISSUE_MSG(r, Severity::Warning, "texts[0].content", "empty string");
}

TEST(text_font_size_zero_or_negative)
{
    auto r = validateJson(
        "{\"canvas\": {\"width\": 800, \"height\": 600, \"dpiX\": 300, \"dpiY\": 300,"
        "  \"texts\": [{\"x\": 50, \"y\": 100, \"content\": \"Hi\", \"fontSize\": -5}]}}");
    EXPECT_ISSUE_MSG(r, Severity::Error, "texts[0].fontSize", "must be positive");
}

TEST(text_empty_font_family)
{
    auto r = validateJson(
        "{\"canvas\": {\"width\": 800, \"height\": 600, \"dpiX\": 300, \"dpiY\": 300,"
        "  \"texts\": [{\"x\": 50, \"y\": 100, \"content\": \"Hi\", \"fontFamily\": \"\"}]}}");
    EXPECT_ISSUE_MSG(r, Severity::Warning, "texts[0].fontFamily", "empty");
}

TEST(text_bold_wrong_type)
{
    auto r = validateJson(
        "{\"canvas\": {\"width\": 800, \"height\": 600, \"dpiX\": 300, \"dpiY\": 300,"
        "  \"texts\": [{\"x\": 50, \"y\": 100, \"content\": \"Hi\", \"bold\": \"yes\"}]}}");
    EXPECT_ISSUE_MSG(r, Severity::Warning, "texts[0].bold", "should be true or false");
}

TEST(text_rotation_unknown_field)
{
    // rotation is no longer supported for text — the field is unknown and
    // flagged as a hint (it is ignored by the renderer).
    auto r = validateJson(
        "{\"canvas\": {\"width\": 800, \"height\": 600, \"dpiX\": 300, \"dpiY\": 300,"
        "  \"texts\": [{\"x\": 50, \"y\": 100, \"content\": \"Hi\", \"rotation\": 30}]}}");
    EXPECT_ISSUE_MSG(r, Severity::Hint, "texts[0].rotation", "Unknown field");
}

// ============================================================================
// 12. ImageItem validation tests
// ============================================================================

TEST(image_valid_passes)
{
    auto tmpDir = std::filesystem::temp_directory_path();
    auto imgPath = tmpDir / "eeverify_test_valid.tiff";
    {
        std::ofstream out(imgPath);
        out << "fake tiff content";
    }
    std::string json = "{\"canvas\": {\"width\": 800, \"height\": 600, \"dpiX\": 300, \"dpiY\": 300,"
                       "  \"images\": [{\"x\": 0, \"y\": 0, \"filePath\": \""
                       + jsonPath(imgPath) + "\"}]}}";
    auto r = validateJson(json);
    std::remove(imgPath.string().c_str());
    EXPECT_NO_ERRORS(r);
}

TEST(image_missing_file_path)
{
    auto r = validateJson("{\"canvas\": {\"width\": 800, \"height\": 600, \"dpiX\": 300, \"dpiY\": 300,"
                          "  \"images\": [{\"x\": 0, \"y\": 0}]}}");
    EXPECT_ISSUE_MSG(r, Severity::Error, "images[0]", "missing required field \"filePath\"");
}

TEST(image_empty_file_path)
{
    auto r = validateJson("{\"canvas\": {\"width\": 800, \"height\": 600, \"dpiX\": 300, \"dpiY\": 300,"
                          "  \"images\": [{\"filePath\": \"\", \"x\": 0, \"y\": 0}]}}");
    EXPECT_ISSUE_MSG(r, Severity::Error, "images[0].filePath", "empty string");
}

TEST(image_nonexistent_file)
{
    auto r = validateJson(
        "{\"canvas\": {\"width\": 800, \"height\": 600, \"dpiX\": 300, \"dpiY\": 300,"
        "  \"images\": [{\"filePath\": \"/nonexistent/image_12345.tiff\", \"x\": 0, \"y\": 0}]}}");
    EXPECT_ISSUE_MSG(r, Severity::Error, "images[0].filePath", "does not exist");
}

TEST(image_non_tiff_extension_warns)
{
    auto tmpDir = std::filesystem::temp_directory_path();
    auto tmpPath = tmpDir / "eeverify_test_image.png";
    {
        std::ofstream out(tmpPath);
        out << "fake png content";
    }
    std::string json = "{\"canvas\": {\"width\": 800, \"height\": 600, \"dpiX\": 300, \"dpiY\": 300,"
                       "  \"images\": [{\"filePath\": \""
                       + jsonPath(tmpPath) + "\", \"x\": 0, \"y\": 0}]}}";
    auto r = validateJson(json);
    EXPECT_ISSUE_MSG(r, Severity::Warning, "images[0].filePath", "only TIFF files");
    std::remove(tmpPath.string().c_str());
}

TEST(image_negative_width)
{
    auto tmpDir = std::filesystem::temp_directory_path();
    auto imgPath = tmpDir / "eeverify_test_neg.tiff";
    {
        std::ofstream out(imgPath);
        out << "tiff";
    }
    std::string json = "{\"canvas\": {\"width\": 800, \"height\": 600, \"dpiX\": 300, \"dpiY\": 300,"
                       "  \"images\": [{\"filePath\": \""
                       + jsonPath(imgPath) + "\", \"x\": 0, \"y\": 0, \"width\": -100}]}}";
    auto r = validateJson(json);
    std::remove(imgPath.string().c_str());
    EXPECT_ISSUE_MSG(r, Severity::Warning, "images[0].width", "negative");
}

// ============================================================================
// 13. Unit validation tests
// ============================================================================

TEST(unit_canvas_valid_px)
{
    auto r = validateJson(
        "{\"canvas\": {\"unit\": \"px\", \"width\": 800, \"height\": 600, \"dpiX\": 300, \"dpiY\": 300}}");
    EXPECT_NO_ERRORS(r);
}

TEST(unit_canvas_valid_mm)
{
    auto r = validateJson(
        "{\"canvas\": {\"unit\": \"mm\", \"width\": 210, \"height\": 297, \"dpiX\": 300, \"dpiY\": 300}}");
    EXPECT_NO_ERRORS(r);
}

TEST(unit_canvas_invalid_value)
{
    auto r = validateJson(
        "{\"canvas\": {\"unit\": \"cm\", \"width\": 800, \"height\": 600, \"dpiX\": 300, \"dpiY\": 300}}");
    EXPECT_ISSUE_MSG(r, Severity::Error, "canvas.unit", "Unknown unit");
}

TEST(unit_canvas_not_string)
{
    auto r = validateJson(
        "{\"canvas\": {\"unit\": 123, \"width\": 800, \"height\": 600, \"dpiX\": 300, \"dpiY\": 300}}");
    EXPECT_ISSUE_MSG(r, Severity::Error, "canvas.unit", "must be a string");
}

TEST(unit_primitive_valid_mm)
{
    auto r = validateJson(
        "{\"canvas\": {\"width\": 800, \"height\": 600, \"dpiX\": 300, \"dpiY\": 300,"
        "  \"rects\": [{\"unit\": \"mm\", \"x\": 10, \"y\": 10, \"width\": 50, \"height\": 30}]}}");
    EXPECT_NO_ERRORS(r);
}

TEST(unit_primitive_invalid_value)
{
    auto r = validateJson("{\"canvas\": {\"width\": 800, \"height\": 600, \"dpiX\": 300, \"dpiY\": 300,"
                          "  \"rects\": [{\"unit\": \"inch\", \"x\": 10, \"y\": 10, \"width\": "
                          "100, \"height\": 50}]}}");
    EXPECT_ISSUE_MSG(r, Severity::Error, "rects[0].unit", "Unknown unit");
}

TEST(unit_circle_valid_mm)
{
    auto r = validateJson(
        "{\"canvas\": {\"width\": 800, \"height\": 600, \"dpiX\": 300, \"dpiY\": 300,"
        "  \"circles\": [{\"unit\": \"mm\", \"cx\": 50, \"cy\": 50, \"radiusX\": 25}]}}");
    EXPECT_NO_ERRORS(r);
}

TEST(unit_line_valid_px_explicit)
{
    auto r = validateJson(
        "{\"canvas\": {\"unit\": \"mm\", \"width\": 210, \"height\": 297, \"dpiX\": 300, \"dpiY\": 300,"
        "  \"lines\": [{\"unit\": \"px\", \"x1\": 100, \"y1\": 200, \"x2\": 300, \"y2\": 400}]}}");
    EXPECT_NO_ERRORS(r);
}

// ============================================================================
// 14. Edit-distance algorithm tests
// ============================================================================

TEST(ed_transposition_adjacent)
{
    EXPECT_EQ(Validator::editDistance("ab", "ba"), 1);
    EXPECT_EQ(Validator::editDistance("abc", "acb"), 1);
}

TEST(ed_transposition_stroke_typo)
{
    EXPECT_EQ(Validator::editDistance("storkeColor", "strokeColor"), 1);
}

TEST(ed_transposition_extra_letter)
{
    // "strokeColour" vs "strokeColor": delete 'u' -> distance 1
    EXPECT_EQ(Validator::editDistance("strokeColour", "strokeColor"), 1);
}

TEST(ed_substitution_single)
{
    EXPECT_EQ(Validator::editDistance("abc", "axc"), 1);
}

TEST(ed_substitution_all)
{
    EXPECT_EQ(Validator::editDistance("abc", "xyz"), 3);
}

TEST(ed_insert_one)
{
    EXPECT_EQ(Validator::editDistance("abc", "abcd"), 1);
}

TEST(ed_delete_one)
{
    EXPECT_EQ(Validator::editDistance("abcd", "abc"), 1);
    EXPECT_EQ(Validator::editDistance("strokeColor", "strokColor"), 1);
}

TEST(ed_empty_both)
{
    EXPECT_EQ(Validator::editDistance("", ""), 0);
}

TEST(ed_empty_vs_string)
{
    EXPECT_EQ(Validator::editDistance("abc", ""), 3);
    EXPECT_EQ(Validator::editDistance("", "abc"), 3);
}

TEST(ed_identical)
{
    EXPECT_EQ(Validator::editDistance("strokeColor", "strokeColor"), 0);
}

TEST(ed_early_termination)
{
    int d = Validator::editDistance("abcdef", "xyz123", 2);
    EXPECT_GT(d, 2);
}

TEST(ed_length_quick_reject)
{
    int d = Validator::editDistance("ab", "abcdefg", 2);
    EXPECT_GT(d, 2);
}

// closestKeys tests
TEST(ck_exact_match)
{
    auto result = Validator::closestKeys("strokeColor", { "strokeColor", "x", "y" });
    EXPECT_EQ(result.size(), 1u);
    EXPECT_EQ(result[0], std::string("strokeColor"));
}

TEST(ck_typo_transposition)
{
    auto result =
        Validator::closestKeys("storkeColor", { "strokeColor", "strokeWidth", "lineStyle" });
    EXPECT_EQ(result.size(), 1u);
    EXPECT_EQ(result[0], std::string("strokeColor"));
}

TEST(ck_multiple_matches)
{
    auto result = Validator::closestKeys("strokeColour", { "strokeColor", "strokeWidth" });
    EXPECT_EQ(result.size(), 1u);
    EXPECT_EQ(result[0], std::string("strokeColor"));
}

TEST(ck_no_match)
{
    auto result = Validator::closestKeys("completelyWrongKey", { "x", "y", "width" });
    EXPECT_TRUE(result.empty());
}

TEST(ck_case_insensitive)
{
    auto result = Validator::closestKeys("StrokeColor", { "strokeColor", "strokeWidth" });
    EXPECT_EQ(result.size(), 1u);
    EXPECT_EQ(result[0], std::string("strokeColor"));
}

TEST(ck_short_key)
{
    auto result = Validator::closestKeys("x", { "cx", "cy", "width" });
    EXPECT_EQ(result.size(), 2u);
    EXPECT_EQ(result[0], std::string("cx"));
    EXPECT_EQ(result[1], std::string("cy"));
}

TEST(ck_near_miss_multiple)
{
    auto result = Validator::closestKeys("radious", { "radiusX", "radiusY", "radial" });
    EXPECT_TRUE(result.size() >= 1u);
}

// ============================================================================
// 15. Integration tests -- full scenes
// ============================================================================

TEST(integration_full_scene_all_primitives)
{
    auto tmpDir = std::filesystem::temp_directory_path();
    auto imgPath = tmpDir / "eeverify_integration_test.tiff";
    {
        std::ofstream out(imgPath);
        out << "fake tiff";
    }
    std::string imgPathStr = jsonPath(imgPath);

    std::string json = std::string(
        "{\"canvas\": {"
        "  \"unit\": \"mm\", \"width\": 210, \"height\": 297, \"dpiX\": 300, \"dpiY\": 300,"
        "  \"background\": {\"c\": 0, \"m\": 0, \"y\": 0, \"k\": 0, \"a\": "
        "1.0},"
        "  \"rects\": ["
        "    {\"x\": 10, \"y\": 10, \"width\": 50, \"height\": 30, \"z\": 1,"
        "     \"fillColor\": {\"c\": 0, \"m\": 100, \"y\": 100, \"k\": 0},"
        "     \"strokeColor\": {\"c\": 0, \"m\": 0, \"y\": 0, \"k\": 100},"
        "     \"strokeWidth\": 0.5, \"lineStyle\": \"solid\"}"
        "  ],"
        "  \"circles\": ["
        "    {\"cx\": 100, \"cy\": 50, \"radiusX\": 20, \"z\": 2,"
        "     \"fillColor\": {\"c\": 100, \"m\": 0, \"y\": 0, \"k\": 0},"
        "     \"strokeWidth\": 0.3, \"lineStyle\": \"solid\"}"
        "  ],"
        "  \"lines\": ["
        "    {\"x1\": 5, \"y1\": 150, \"x2\": 200, \"y2\": 150, \"z\": 3,"
        "     \"strokeColor\": {\"c\": 0, \"m\": 0, \"y\": 0, \"k\": 50},"
        "     \"strokeWidth\": 0.5, \"lineStyle\": \"dashed\"}"
        "  ],"
        "  \"freeLines\": ["
        "    {\"points\": [{\"x\": 10, \"y\": 200}, {\"x\": 50, \"y\": 220}, {\"x\": 100, \"y\": "
        "190}],"
        "     \"strokeColor\": {\"c\": 0, \"m\": 100, \"y\": 100, \"k\": 0},"
        "     \"strokeWidth\": 1.0, \"lineStyle\": \"solid\"}"
        "  ],"
        "  \"bezierCurves\": ["
        "    {\"controlPoints\": [{\"x\": 10, \"y\": 250}, {\"x\": 60, \"y\": 280},"
        "                        {\"x\": 110, \"y\": 280}, {\"x\": 160, \"y\": 250}],"
        "     \"strokeColor\": {\"c\": 0, \"m\": 0, \"y\": 0, \"k\": 100},"
        "     \"strokeWidth\": 0.8, \"lineStyle\": \"solid\"}"
        "  ],"
        "  \"texts\": ["
        "    {\"x\": 10, \"y\": 30, \"content\": \"Test\", \"fontFamily\": \"SimSun\","
        "     \"fontSize\": 14, \"textColor\": {\"c\": 0, \"m\": 0, \"y\": 0, \"k\": 100}}"
        "  ],"
        "  \"images\": ["
        "    {\"filePath\": \""
        + imgPathStr
        + "\", \"x\": 10, \"y\": 100, \"width\": 100, \"height\": 80}"
          "  ]"
          "}}");
    auto r = validateJson(json);
    std::remove(imgPath.string().c_str());
    EXPECT_NO_ERRORS(r);
}

TEST(integration_multiple_errors_collected)
{
    std::string json =
        std::string("{\"canvas\": {"
                    "  \"width\": -100, \"height\": 0, \"dpiX\": -1, \"dpiY\": -1, \"unit\": \"cm\","
                    "  \"rects\": ["
                    "    {\"x\": \"left\", \"y\": 0, \"width\": -5, \"height\": -5,"
                    "     \"lineStyle\": \"invalid_style\","
                    "     \"fillColor\": {\"a\": 5.0}}"
                    "  ]"
                    "}}");
    auto r = validateJson(json);
    int errCount = countIssues(r, Severity::Error);
    EXPECT_TRUE(errCount >= 5);
}

TEST(integration_warnings_only_passes)
{
    auto r = validateJson("{\"canvas\": {\"width\": 800, \"height\": 600, \"dpiX\": 50, \"dpiY\": 50,"
                          "  \"rects\": [{\"x\": 0, \"y\": 0, \"width\": 0, \"height\": 50,"
                          "    \"cornerRadius\": 500}]}}");
    EXPECT_FALSE(r.hasErrors());
    EXPECT_TRUE(r.warningCount() + r.hintCount() > 0);
}

TEST(integration_comment_fields_ignored)
{
    auto r = validateJson("{\"canvas\": {\"width\": 800, \"height\": 600, \"dpiX\": 300, \"dpiY\": 300,"
                          "  \"_author\": \"test\", \"$schema\": \"v1\","
                          "  \"rects\": [{\"x\": 0, \"y\": 0, \"width\": 100, \"height\": 50,"
                          "    \"_note\": \"this should be ignored\"}]}}");
    EXPECT_FALSE(hasIssue(r, Severity::Hint, "_author"));
    EXPECT_FALSE(hasIssue(r, Severity::Hint, "$schema"));
    EXPECT_FALSE(hasIssue(r, Severity::Hint, "_note"));
    EXPECT_NO_ERRORS(r);
}

// ============================================================================
// 16. Edge cases
// ============================================================================

TEST(edge_rect_not_object)
{
    auto r = validateJson("{\"canvas\": {\"width\": 800, \"height\": 600, \"dpiX\": 300, \"dpiY\": 300,"
                          "  \"rects\": [\"not an object\"]}}");
    EXPECT_ISSUE_MSG(r, Severity::Error, "rects[0]", "must be an object");
}

TEST(edge_primitives_array_not_array)
{
    auto r = validateJson("{\"canvas\": {\"width\": 800, \"height\": 600, \"dpiX\": 300, \"dpiY\": 300,"
                          "  \"rects\": {\"not\": \"an array\"}}}");
    EXPECT_ISSUE_MSG(r, Severity::Error, "canvas.rects", "must be an array");
}

TEST(edge_line_style_on_rect_all_valid)
{
    const char *styles[] = { "solid", "dashed", "dotted", "dashDot", "doubleDotDash", "none" };
    for (auto style : styles) {
        std::string json = "{\"canvas\": {\"width\": 800, \"height\": 600, \"dpiX\": 300, \"dpiY\": 300,"
                           "  \"rects\": [{\"x\": 0, \"y\": 0, \"width\": 100, \"height\": 50,"
                           "    \"lineStyle\": \""
                           + std::string(style) + "\"}]}}";
        auto r = validateJson(json);
        EXPECT_FALSE(hasIssue(r, Severity::Error, "lineStyle"));
    }
}

// ============================================================================
// 17. Rect grid/texture fill validation tests
// ============================================================================

TEST(rect_grid_fill_valid_passes)
{
    auto r = validateJson(
        "{\"canvas\": {\"width\": 800, \"height\": 600, \"dpiX\": 300, \"dpiY\": 300,"
        "  \"rects\": [{\"x\": 0, \"y\": 0, \"width\": 100, \"height\": 50,"
        "    \"gridFill\": {"
        "      \"gridColor\": {\"c\": 0, \"m\": 0, \"y\": 0, \"k\": 100},"
        "      \"backgroundColor\": {\"c\": 0, \"m\": 0, \"y\": 0, \"k\": 0},"
        "      \"transparentBackground\": true"
        "    }}]}}");
    EXPECT_NO_ERRORS(r);
}

TEST(rect_grid_fill_deprecated_size_keys_warn)
{
    // cellWidth/cellHeight/lineWidth 已废弃：网格固定 1mm、线宽固定 1px，
    // 按画布 dpi 计算，JSON 中的旧配置不再生效，仅输出告警。
    auto r = validateJson("{\"canvas\": {\"width\": 800, \"height\": 600, \"dpiX\": 300, \"dpiY\": 300,"
                          "  \"rects\": [{\"x\": 0, \"y\": 0, \"width\": 100, \"height\": 50,"
                          "    \"gridFill\": {\"cellWidth\": 20}}]}}");
    EXPECT_ISSUE_MSG(r, Severity::Warning, "rects[0].gridFill", "deprecated");
}

TEST(rect_grid_fill_unknown_key_errors)
{
    auto r = validateJson("{\"canvas\": {\"width\": 800, \"height\": 600, \"dpiX\": 300, \"dpiY\": 300,"
                          "  \"rects\": [{\"x\": 0, \"y\": 0, \"width\": 100, \"height\": 50,"
                          "    \"gridFill\": {\"gridColor\": {\"c\": 0, \"m\": 0, \"y\": 0, \"k\": 100}, \"spacing\": 5}}]}}");
    EXPECT_ISSUE_MSG(r, Severity::Hint, "rects[0].gridFill.spacing", "Unknown field");
}

TEST(rect_texture_fill_missing_custom_size_errors)
{
    auto r = validateJson(
        "{\"canvas\": {\"width\": 800, \"height\": 600, \"dpiX\": 300, \"dpiY\": 300,"
        "  \"rects\": [{\"x\": 0, \"y\": 0, \"width\": 200, \"height\": 150,"
        "    \"textureFill\": {\"filePath\": \"D:/images/pattern.png\"}}]}}");
    EXPECT_ISSUE_MSG(r, Severity::Error, "rects[0].textureFill.customWidth",
        "required when useOriginalSize=false");
    EXPECT_ISSUE_MSG(r, Severity::Error, "rects[0].textureFill.customHeight",
        "required when useOriginalSize=false");
}

TEST(rect_texture_fill_original_size_ignores_custom_warns)
{
    auto r = validateJson(
        "{\"canvas\": {\"width\": 800, \"height\": 600, \"dpiX\": 300, \"dpiY\": 300,"
        "  \"rects\": [{\"x\": 0, \"y\": 0, \"width\": 200, \"height\": 150,"
        "    \"textureFill\": {\"filePath\": \"D:/images/pattern.png\","
        "      \"useOriginalSize\": true, \"customWidth\": 40}}]}}");
    EXPECT_ISSUE_MSG(r, Severity::Warning, "rects[0].textureFill",
        "ignored when useOriginalSize=true");
}

TEST(rect_fill_conflict_grid_and_gradient_errors)
{
    auto r = validateJson(
        "{\"canvas\": {\"width\": 800, \"height\": 600, \"dpiX\": 300, \"dpiY\": 300,"
        "  \"rects\": [{\"x\": 0, \"y\": 0, \"width\": 100, \"height\": 50,"
        "    \"gradient\": {\"type\": \"linear\","
        "      \"stops\": ["
        "        {\"offset\": 0.0, \"color\": {\"c\": 0, \"m\": 100, \"y\": 100, \"k\": 0}},"
        "        {\"offset\": 1.0, \"color\": {\"c\": 100, \"m\": 100, \"y\": 0, \"k\": 0}}"
        "      ]},"
        "    \"gridFill\": {"
        "      \"gridColor\": {\"c\": 0, \"m\": 0, \"y\": 0, \"k\": 100}}}]}}");
    EXPECT_ISSUE_MSG(r, Severity::Error, "rects[0]", "mutually exclusive");
}

TEST(rect_texture_fill_non_boolean_use_original_size_errors)
{
    // 非布尔 useOriginalSize：已报错且校验器不得抛异常（nlohmann value() 会
    // 抛 type_error.302，validate() 只捕 parse_error → 之前会直接崩溃）
    auto r = validateJson(
        "{\"canvas\": {\"width\": 800, \"height\": 600, \"dpiX\": 300, \"dpiY\": 300,"
        "  \"rects\": [{\"x\": 0, \"y\": 0, \"width\": 200, \"height\": 150,"
        "    \"textureFill\": {\"filePath\": \"D:/images/pattern.png\","
        "      \"useOriginalSize\": \"yes\"}}]}}");
    EXPECT_ISSUE_MSG(r, Severity::Error, "rects[0].textureFill.useOriginalSize",
        "must be a boolean");
}

TEST(rect_grid_fill_non_object_errors)
{
    // 非对象 gridFill：必须报错，不得静默通过
    auto r = validateJson("{\"canvas\": {\"width\": 800, \"height\": 600, \"dpiX\": 300, \"dpiY\": 300,"
                          "  \"rects\": [{\"x\": 0, \"y\": 0, \"width\": 100, \"height\": 50,"
                          "    \"gridFill\": \"grid\"}]}}");
    EXPECT_ISSUE_MSG(r, Severity::Error, "rects[0].gridFill", "must be an object");
}
