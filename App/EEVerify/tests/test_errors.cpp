// Error-code unification tests
// ============================================================================
// Tests for the EEError domain and the error_code migration of core
// components (JsonSceneParser, ColorConverter, TiffWriter, FontEngine,
// json2tiff). Links ExportEngine.dll (unlike test_validator).
// ============================================================================

#include "EEError.h"
#include "Json2Tiff.h"
#include "JsonSceneParser.h"
#include "PredecodeScheduler.h"
#include "RenderContext.h"
#include "ImageRenderer.h"
#include "ColorConverter.h"
#include "ColorTransform.h"
#include "NearestResampler.h"
#include "SceneData.h"
#include "SceneRenderer.h"
#include "TiffWriter.h"
#include "MemorySink.h"
#include "FontEngine.h"
#include "TextureSource.h"
#include "test_harness.h"

#include <vips/vips.h>
#include <tiffio.h>

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <system_error>
#include <thread>

namespace fs = std::filesystem;
using namespace ATHC::EE;

// Every enum value, in declaration order — used to verify message()/recoverable()
static const EEError kAllCodes[] = {
    EEError::io_failed,
    EEError::parse_cannot_open_file,
    EEError::parse_invalid_json,
    EEError::parse_root_not_object,
    EEError::parse_invalid_canvas_unit,
    EEError::parse_dimensions_not_positive,
    EEError::parse_dpi_not_positive,
    EEError::parse_converted_dimensions_not_positive,
    EEError::parse_dpi_deprecated,
    EEError::parse_invalid_color,
    EEError::parse_rgba_color_removed,
    EEError::parse_color_space_field_removed,
    EEError::icc_open_failed,
    EEError::icc_parse_failed,
    EEError::icc_not_initialized,
    EEError::tiff_open_failed,
    EEError::tiff_no_session,
    EEError::tiff_write_strip_failed,
    EEError::font_library_init_failed,
    EEError::font_invalid_family_name,
    EEError::font_not_found,
    EEError::font_face_load_failed,
    EEError::font_cairo_face_failed,
    EEError::font_scaled_font_failed,
    EEError::image_decode_failed,
    EEError::image_unsupported_colourspace,
    EEError::image_band_count_mismatch,
    EEError::composite_failed,
    EEError::sink_unsupported_granularity,
    EEError::sink_not_initialized,
    EEError::sink_write_failed,
};

TEST(eeerror_message_nonempty)
{
    for (EEError code : kAllCodes) {
        std::error_code ec(code);
        EXPECT_TRUE(!ec.message().empty());
    }
}

TEST(eeerror_category_name)
{
    EXPECT_EQ(std::string(eeErrorCategory().name()), std::string("EEError"));
}

TEST(eeerror_equality_with_enum)
{
    std::error_code ec = EEError::font_not_found;
    EXPECT_TRUE(ec == EEError::font_not_found);
    EXPECT_TRUE(ec != EEError::font_invalid_family_name);
    EXPECT_TRUE(ec.category() == eeErrorCategory());
}

TEST(eeerror_recoverable_table)
{
    // 可恢复（降级渲染）
    EXPECT_TRUE(isRecoverable(std::error_code(EEError::font_not_found)));
    EXPECT_TRUE(isRecoverable(std::error_code(EEError::font_invalid_family_name)));
    EXPECT_TRUE(isRecoverable(std::error_code(EEError::font_face_load_failed)));
    EXPECT_TRUE(isRecoverable(std::error_code(EEError::image_decode_failed)));
    EXPECT_TRUE(isRecoverable(std::error_code(EEError::image_unsupported_colourspace)));
    // 不可恢复
    EXPECT_FALSE(isRecoverable(std::error_code(EEError::parse_invalid_json)));
    EXPECT_FALSE(isRecoverable(std::error_code(EEError::icc_open_failed)));
    EXPECT_FALSE(isRecoverable(std::error_code(EEError::tiff_open_failed)));
    EXPECT_FALSE(isRecoverable(std::error_code(EEError::font_library_init_failed)));
    EXPECT_FALSE(isRecoverable(std::error_code(EEError::composite_failed)));
}

TEST(eeerror_other_categories_not_recoverable)
{
    // system/generic 域错误一律不可恢复
    EXPECT_FALSE(isRecoverable(std::error_code(2, std::generic_category())));
    EXPECT_FALSE(isRecoverable(std::make_error_code(std::errc::no_such_file_or_directory)));
}

// ── JsonSceneParser ────────────────────────────────────────────────────────

TEST(parser_missing_file)
{
    JsonSceneParser p;
    Canvas c;
    auto ec = p.parse("no/such/file.json", c);
    EXPECT_TRUE(ec == EEError::parse_cannot_open_file);
}

TEST(parser_invalid_json)
{
    JsonSceneParser p;
    Canvas c;
    auto ec = p.parseFromJson("{ not json", c);
    EXPECT_TRUE(ec == EEError::parse_invalid_json);
}

TEST(parser_root_not_object)
{
    JsonSceneParser p;
    Canvas c;
    auto ec = p.parseFromJson("[1, 2, 3]", c);
    EXPECT_TRUE(ec == EEError::parse_root_not_object);
}

TEST(parser_invalid_unit)
{
    JsonSceneParser p;
    Canvas c;
    auto ec = p.parseFromJson(R"({"canvas":{"width":100,"height":100,"unit":"cm"}})", c);
    EXPECT_TRUE(ec == EEError::parse_invalid_canvas_unit);
}

TEST(parser_dimensions_nonpositive)
{
    JsonSceneParser p;
    Canvas c;
    auto ec = p.parseFromJson(R"({"canvas":{"width":0,"height":100}})", c);
    EXPECT_TRUE(ec == EEError::parse_dimensions_not_positive);
}

TEST(parser_dpi_nonpositive)
{
    // dpiX <= 0 → 不可恢复解析错误
    JsonSceneParser p;
    Canvas c;
    auto ec = p.parseFromJson(R"({"canvas":{"width":100,"height":100,"dpiX":0}})", c);
    EXPECT_TRUE(ec == EEError::parse_dpi_not_positive);
}

TEST(parser_dpi_y_nonpositive)
{
    JsonSceneParser p;
    Canvas c;
    auto ec = p.parseFromJson(R"({"canvas":{"width":100,"height":100,"dpiX":300,"dpiY":0}})", c);
    EXPECT_TRUE(ec == EEError::parse_dpi_not_positive);
}

TEST(parser_dpi_deprecated)
{
    // 旧 dpi 字段存在且 dpiX/dpiY 均缺失 → 报废弃错误，不静默回退 300
    JsonSceneParser p;
    Canvas c;
    auto ec = p.parseFromJson(R"({"canvas":{"width":100,"height":100,"dpi":300}})", c);
    EXPECT_TRUE(ec == EEError::parse_dpi_deprecated);
}

TEST(parser_dpi_priority_xy_over_deprecated)
{
    // 旧 dpi 与 dpiX/dpiY 同时出现 → dpi 被忽略，dpiX/dpiY 生效（不报错）
    JsonSceneParser p;
    Canvas c;
    auto ec = p.parseFromJson(
        R"({"canvas":{"width":100,"height":100,"dpi":100,"dpiX":600,"dpiY":600}})", c);
    EXPECT_TRUE(!ec);
    EXPECT_EQ(c.dpi.x, 600);
    EXPECT_EQ(c.dpi.y, 600);
}

TEST(parser_color_space_field_removed)
{
    JsonSceneParser p;
    Canvas c;
    auto ec = p.parseFromJson(R"({"canvas":{"width":100,"height":100,"background":{"space":"cmyk","c":0,"m":0,"y":0,"k":0}}})", c);
    EXPECT_TRUE(ec == EEError::parse_color_space_field_removed);
}

TEST(parser_color_rgba_removed)
{
    JsonSceneParser p;
    Canvas c;
    auto ec = p.parseFromJson(R"({"canvas":{"width":100,"height":100,"background":{"r":255,"g":0,"b":0}}})", c);
    EXPECT_TRUE(ec == EEError::parse_rgba_color_removed);
}

TEST(parser_color_not_object)
{
    JsonSceneParser p;
    Canvas c;
    auto ec = p.parseFromJson(R"({"canvas":{"width":100,"height":100,"background":"red"}})", c);
    EXPECT_TRUE(ec == EEError::parse_invalid_color);
}

TEST(parser_color_cmyk_ok)
{
    JsonSceneParser p;
    Canvas c;
    auto ec = p.parseFromJson(R"({"canvas":{"width":100,"height":100,"background":{"c":0,"m":100,"y":100,"k":0,"a":1.0}}})", c);
    EXPECT_TRUE(!ec);
    EXPECT_EQ(c.background.ch[0], 0.0);
    EXPECT_EQ(c.background.ch[1], 255.0); // m 100% → 255
}

TEST(parser_dpi_xy_parsed)
{
    JsonSceneParser p;
    Canvas c;
    auto ec = p.parseFromJson(
        R"({"canvas":{"width":100,"height":100,"dpiX":300,"dpiY":600}})", c);
    EXPECT_TRUE(!ec);
    EXPECT_EQ(c.dpi.x, 300);
    EXPECT_EQ(c.dpi.y, 600);
}

TEST(parser_dpi_defaults)
{
    JsonSceneParser p;
    Canvas c;
    auto ec = p.parseFromJson(R"({"canvas":{"width":100,"height":100}})", c);
    EXPECT_TRUE(!ec);
    EXPECT_EQ(c.dpi.x, 300);
    EXPECT_EQ(c.dpi.y, 300);
}

TEST(parser_converted_dimensions_nonpositive)
{
    // dpiX=1 + unit=mm + width=1 → 1/25.4 ≈ 0.04 → round 后为 0 → 单位换算后非正
    JsonSceneParser p;
    Canvas c;
    auto ec = p.parseFromJson(
        R"({"canvas":{"width":1,"height":1,"unit":"mm","dpiX":1,"dpiY":1}})", c);
    EXPECT_TRUE(ec == EEError::parse_converted_dimensions_not_positive);
}

TEST(parser_field_type_mismatch)
{
    // 类型错误（如 "width":"abc"）会抛 nlohmann type_error——必须被捕获并
    // 映射为 parse_invalid_json，不得以异常形式跨 DLL 边界逃逸
    JsonSceneParser p;
    Canvas c;
    auto ec = p.parseFromJson(R"({"canvas":{"width":"abc","height":100}})", c);
    EXPECT_TRUE(ec == EEError::parse_invalid_json);
}

TEST(parser_success)
{
    JsonSceneParser p;
    Canvas c;
    auto ec = p.parseFromJson(R"({"canvas":{"width":100,"height":100}})", c);
    EXPECT_TRUE(!ec);
    EXPECT_EQ(c.width, 100);
    EXPECT_EQ(c.height, 100);
}

TEST(parser_anisotropic_mm_conversion)
{
    // 各向异性换算：X 用 dpiX=300、Y 用 dpiY=600
    //   10mm → round(10×300/25.4)=118；round(10×600/25.4)=236
    //   1mm   → 1×300/25.4=11.81…；1×600/25.4=23.62…（矩形坐标不取整）
    JsonSceneParser p;
    Canvas c;
    auto ec = p.parseFromJson(R"({"canvas":{"width":10,"height":10,"unit":"mm","dpiX":300,"dpiY":600,"rects":[{"x":1,"y":1,"width":1,"height":1,"unit":"mm"}]}})", c);
    EXPECT_TRUE(!ec);
    EXPECT_EQ(c.dpi.x, 300);
    EXPECT_EQ(c.dpi.y, 600);
    EXPECT_EQ(c.width, 118);
    EXPECT_EQ(c.height, 236);
    EXPECT_TRUE(c.rects.size() == 1u);
    EXPECT_TRUE(std::abs(c.rects[0].x - 300.0 / 25.4) < 1e-9);
    EXPECT_TRUE(std::abs(c.rects[0].y - 600.0 / 25.4) < 1e-9);
    EXPECT_TRUE(std::abs(c.rects[0].width - 300.0 / 25.4) < 1e-9);
    EXPECT_TRUE(std::abs(c.rects[0].height - 600.0 / 25.4) < 1e-9);
}

// ── 矩形网格/纹理填充解析 ────────────────────────────────────────────────

TEST(parse_rect_grid_fill)
{
    JsonSceneParser p;
    Canvas c;
    auto ec = p.parseFromJson(R"({
        "canvas": { "width": 800, "height": 600, "dpiX": 300, "dpiY": 300,
            "background": { "c": 0, "m": 0, "y": 0, "k": 0, "a": 1 },
            "rects": [{
                "x": 0, "y": 0, "width": 100, "height": 80,
                "gridFill": {
                    "gridColor": { "c": 0, "m": 0, "y": 0, "k": 100, "a": 1 },
                    "backgroundColor": { "c": 0, "m": 0, "y": 0, "k": 0, "a": 1 },
                    "transparentBackground": true
                }
            }]
        }
    })", c);
    EXPECT_TRUE(!ec);
    EXPECT_TRUE(c.rects.size() == 1u);
    const auto &r = c.rects[0];
    EXPECT_TRUE(r.gridFill.has_value());
    EXPECT_TRUE(!r.textureFill.has_value());
    const auto &g = *r.gridFill;
    EXPECT_TRUE(g.transparentBackground);
    // 网格尺寸固定：300dpi 下 1mm = 300 / 25.4 px，线宽固定 1px
    const double mmPx = 300.0 / 25.4;
    EXPECT_TRUE(std::abs(g.cellWidth - mmPx) < 1e-9);
    EXPECT_TRUE(std::abs(g.cellHeight - mmPx) < 1e-9);
    EXPECT_EQ(g.lineWidth, 1.0);
    EXPECT_EQ(g.gridColor.ch[0], 0.0);
    EXPECT_EQ(g.backgroundColor.ch[0], 0.0); // 纸白 = 全 0
}

TEST(parse_rect_texture_fill)
{
    JsonSceneParser p;
    Canvas c;
    auto ec = p.parseFromJson(R"({
        "canvas": { "width": 800, "height": 600, "dpiX": 300, "dpiY": 300,
            "background": { "c": 0, "m": 0, "y": 0, "k": 0, "a": 1 },
            "rects": [{
                "x": 0, "y": 0, "width": 200, "height": 150,
                "textureFill": {
                    "filePath": "D:/images/pattern.png",
                    "offsetX": 5, "offsetY": 7,
                    "useOriginalSize": false,
                    "customWidth": 40, "customHeight": 30
                }
            }]
        }
    })", c);
    EXPECT_TRUE(!ec);
    EXPECT_TRUE(c.rects.size() == 1u);
    const auto &r = c.rects[0];
    EXPECT_TRUE(r.textureFill.has_value());
    const auto &t = *r.textureFill;
    EXPECT_EQ(t.filePath, "D:/images/pattern.png");
    EXPECT_EQ(t.offsetX, 5.0);
    EXPECT_EQ(t.offsetY, 7.0);
    EXPECT_TRUE(!t.useOriginalSize);
    EXPECT_EQ(t.customWidth, 40.0);
    EXPECT_EQ(t.customHeight, 30.0);
    EXPECT_TRUE(!r.gridFill.has_value());
}

TEST(parse_rect_fill_mutual_exclusion_grid_wins)
{
    // gradient + gridFill 同时给出 → 按优先级 gridFill 胜出
    JsonSceneParser p;
    Canvas c;
    auto ec = p.parseFromJson(R"({
        "canvas": { "width": 800, "height": 600, "dpiX": 300, "dpiY": 300,
            "background": { "c": 0, "m": 0, "y": 0, "k": 0, "a": 1 },
            "rects": [{
                "x": 0, "y": 0, "width": 100, "height": 80,
                "fillColor": { "c": 0, "m": 0, "y": 0, "k": 96.1, "a": 1 },
                "gradient": { "type": "linear", "stops": [
                    { "offset": 0, "color": { "c": 0, "m": 0, "y": 0, "k": 100, "a": 1 } },
                    { "offset": 1, "color": { "c": 0, "m": 0, "y": 0, "k": 0, "a": 1 } } ] },
                "gridFill": {
                    "gridColor": { "c": 0, "m": 0, "y": 0, "k": 100, "a": 1 },
                    "backgroundColor": { "c": 0, "m": 0, "y": 0, "k": 0, "a": 1 }
                }
            }]
        }
    })", c);
    EXPECT_TRUE(!ec);
    EXPECT_TRUE(c.rects.size() == 1u);
    const auto &r = c.rects[0];
    EXPECT_TRUE(r.gridFill.has_value());
    EXPECT_TRUE(!r.gradient.has_value());
    EXPECT_EQ(r.fillColor.ch[0], 0.0); // 默认色，未设置
}

TEST(parse_rect_fill_mutual_exclusion_texture_wins)
{
    // gridFill + textureFill → textureFill 胜出
    JsonSceneParser p;
    Canvas c;
    auto ec = p.parseFromJson(R"({
        "canvas": { "width": 800, "height": 600, "dpiX": 300, "dpiY": 300,
            "background": { "c": 0, "m": 0, "y": 0, "k": 0, "a": 1 },
            "rects": [{
                "x": 0, "y": 0, "width": 100, "height": 80,
                "gridFill": { "gridColor": { "c": 0, "m": 0, "y": 0, "k": 100, "a": 1 },
                    "backgroundColor": { "c": 0, "m": 0, "y": 0, "k": 0, "a": 1 } },
                "textureFill": { "filePath": "D:/images/p.png",
                    "customWidth": 40, "customHeight": 30 }
            }]
        }
    })", c);
    EXPECT_TRUE(!ec);
    EXPECT_TRUE(c.rects.size() == 1u);
    const auto &r = c.rects[0];
    EXPECT_TRUE(r.textureFill.has_value());
    EXPECT_TRUE(!r.gridFill.has_value());
}

// ── ColorConverter ─────────────────────────────────────────────────────────

TEST(converter_open_missing)
{
    ColorConverter cv;
    auto ec = cv.loadProfile("no/such/rgb.icc", "no/such/cmyk.icc");
    EXPECT_TRUE(ec == EEError::icc_open_failed);
}

TEST(converter_parse_garbage)
{
    // 存在但内容损坏的 profile → 解析失败
    auto dir = fs::temp_directory_path();
    auto rgb = dir / "ee_test_garbage_rgb.icc";
    auto cmyk = dir / "ee_test_garbage_cmyk.icc";
    // 预清理：避免残留的同名目录/文件导致 ofstream 写入失败
    fs::remove(rgb);
    fs::remove(cmyk);
    { std::ofstream f(rgb); f << "this is not an icc profile"; }
    { std::ofstream f(cmyk); f << "this is not an icc profile either"; }

    ColorConverter cv;
    auto ec = cv.loadProfile(rgb.string(), cmyk.string());

    fs::remove(rgb);
    fs::remove(cmyk);

    EXPECT_TRUE(ec == EEError::icc_parse_failed);
}

TEST(converter_not_initialized)
{
    ColorConverter cv;
    std::vector<uint8_t> out;
    EXPECT_FALSE(cv.getProfileBytes(out));
    EXPECT_TRUE(cv.errorCode() == EEError::icc_not_initialized);
}

// ── TiffWriter ─────────────────────────────────────────────────────────────

TEST(tiff_open_failed)
{
    TiffWriter w;
    auto badDir = fs::temp_directory_path() / "ee_no_such_dir_xyz" / "out.tif";
    EXPECT_FALSE(w.beginStripWrite(badDir.string(), 64, 64, Dpi{ 300, 300 }, 16, {}));
    EXPECT_TRUE(w.errorCode() == EEError::tiff_open_failed);
    // errorCode() 在 end* 调用后仍保留（teardownSession 不清错误）
    (void)w.endStripWrite();
    EXPECT_TRUE(w.errorCode() == EEError::tiff_open_failed);
}

TEST(tiff_no_session)
{
    TiffWriter w;
    std::vector<uint8_t> cmyk;
    EXPECT_FALSE(w.writeStrip(0, 0, cmyk));
    EXPECT_TRUE(w.errorCode() == EEError::tiff_no_session);
}

TEST(tiff_first_error_wins)
{
    TiffWriter w;
    auto badDir = fs::temp_directory_path() / "ee_no_such_dir_xyz" / "out.tif";
    EXPECT_FALSE(w.beginStripWrite(badDir.string(), 64, 64, Dpi{ 300, 300 }, 16, {})); // → tiff_open_failed
    // 后续不同失败不得覆盖首个错误
    std::vector<uint8_t> cmyk;
    EXPECT_FALSE(w.writeStrip(0, 0, cmyk)); // 本应 → tiff_no_session
    EXPECT_TRUE(w.errorCode() == EEError::tiff_open_failed);
}

namespace { void ensureVipsInit(); } // 定义见文件下方纹理区块——TiffWriter 测试需先 vips_init

TEST(tiff_resolution_tags_anisotropic)
{
    // 各向异性 DPI → TIFF 头 X/YResolution 分开写。
    // 注：vips 的 xres/yres 元数据键会按像素/毫米换算（300dpi→11.81），
    // 不返回写入的原始 tag 值——故用 libtiff 直读 TIFFTAG_XRESOLUTION/YRESOLUTION。
    TiffWriter w;
    auto path = (fs::temp_directory_path() / "ee_aniso_res.tif").string();
    EXPECT_TRUE(w.beginStripWrite(path, 16, 16, Dpi{ 300, 600 }, 16, {}));
    std::vector<uint8_t> cmyk(16 * 16 * 4, 0);
    EXPECT_TRUE(w.writeStrip(0, 16, cmyk));
    EXPECT_TRUE(w.endStripWrite());

    TIFF *tif = TIFFOpen(path.c_str(), "r");
    EXPECT_TRUE(tif != nullptr);
    if (!tif)
        return;
    float xres = 0.0f, yres = 0.0f;
    TIFFGetField(tif, TIFFTAG_XRESOLUTION, &xres);
    TIFFGetField(tif, TIFFTAG_YRESOLUTION, &yres);
    TIFFClose(tif);
    EXPECT_EQ(static_cast<int>(xres + 0.5f), 300);
    EXPECT_EQ(static_cast<int>(yres + 0.5f), 600);
}

// ── FontEngine ─────────────────────────────────────────────────────────────

TEST(font_init_success)
{
    FontEngine fe;
    EXPECT_TRUE(!fe.init());
}

TEST(font_invalid_name_empty)
{
    FontEngine fe;
    fe.init();
    auto ec = fe.loadFont("", 12.0, false, false);
    EXPECT_TRUE(ec == EEError::font_invalid_family_name);
}

TEST(font_invalid_name_path_traversal)
{
    FontEngine fe;
    fe.init();
    auto ec = fe.loadFont("..", 12.0, false, false);
    EXPECT_TRUE(ec == EEError::font_invalid_family_name);
}

TEST(font_not_found)
{
    FontEngine fe;
    fe.init();
    auto ec = fe.loadFont("NoSuchFontFamilyXyz_12345", 12.0, false, false);
    EXPECT_TRUE(ec == EEError::font_not_found);
}

// ── json2tiff 边界 ─────────────────────────────────────────────────────────

TEST(json2tiff_missing_json)
{
    std::error_code got;
    bool ok = json2tiff("no/such/file.json", "out.tif", nullptr,
        [&](std::error_code ec) { got = ec; });
    EXPECT_FALSE(ok);
    EXPECT_TRUE(got == EEError::parse_cannot_open_file);
}

TEST(json2tiff_invalid_json)
{
    auto p = fs::temp_directory_path() / "ee_test_bad.json";
    // 预清理：避免残留的同名目录/文件导致 ofstream 写入失败
    fs::remove(p);
    { std::ofstream f(p); f << "{ nope"; }

    std::error_code got;
    bool ok = json2tiff(p.string(), "out.tif", nullptr,
        [&](std::error_code ec) { got = ec; });

    fs::remove(p);

    EXPECT_FALSE(ok);
    EXPECT_TRUE(got == EEError::parse_invalid_json);
}

TEST(json2tiff_blocked_output_dir)
{
    // tiffPath 的父级是一个已存在的文件 → create_directories 必失败
    auto block = fs::temp_directory_path() / "ee_test_blocker";
    auto validJson = fs::temp_directory_path() / "ee_test_ok.json";
    // 预清理：避免残留的同名目录/文件导致 ofstream 写入失败
    fs::remove(block);
    fs::remove(validJson);
    { std::ofstream f(block); f << "x"; }
    { std::ofstream f(validJson); f << R"({"canvas":{"width":100,"height":100}})"; }

    std::error_code got;
    bool ok = json2tiff(validJson.string(), (block.string() + "/out.tif"), nullptr,
        [&](std::error_code ec) { got = ec; });

    fs::remove(validJson);
    fs::remove(block);

    EXPECT_FALSE(ok);
    EXPECT_TRUE(got);                          // 一些 system/filesystem 错误
    EXPECT_TRUE(got.category() != eeErrorCategory());
}

// ── json2sink（可插拔接收器）─────────────────────────────────────────────

TEST(json2sink_memory_frame)
{
    auto validJson = fs::temp_directory_path() / "ee_test_sink.json";
    fs::remove(validJson);
    { std::ofstream f(validJson); f << R"({"canvas":{"width":40,"height":30}})"; }

    auto sink = std::make_shared<MemorySink>();
    RenderServices svc;
    svc.sink = sink;

    std::error_code got;
    bool ok = json2sink(validJson.string(), svc, nullptr,
        [&](std::error_code ec) { got = ec; });

    fs::remove(validJson);

    EXPECT_TRUE(ok);
    EXPECT_FALSE(got);
    EXPECT_EQ(sink->descriptor().width, 40);
    EXPECT_EQ(sink->descriptor().height, 30);
    EXPECT_EQ(sink->descriptor().samplesPerPixel, 4);
    EXPECT_EQ(sink->frame().size(), (size_t)40 * 30 * 4);
}

TEST(json2sink_row_matches_strip)
{
    // Row 粒度（renderRow 逐行产出）与 Strip 粒度（renderStrip 条带）输出必须
    // 逐字节一致——渲染内容相同，只是交付颗粒不同。
    auto validJson = fs::temp_directory_path() / "ee_test_row.json";
    fs::remove(validJson);
    { std::ofstream f(validJson); f << R"({"canvas":{"width":40,"height":30}})"; }

    // Strip 基准：MemorySink 按 startRow 拼整帧
    auto strip = std::make_shared<MemorySink>();
    RenderServices svcStrip;
    svcStrip.sink = strip;
    std::error_code ec1;
    EXPECT_TRUE(json2sink(validJson.string(), svcStrip, nullptr,
        [&](std::error_code ec) { ec1 = ec; }));
    EXPECT_FALSE(ec1);

    // Row 粒度：逐行收集（乱序安全，按 row 索引写入）
    struct RowSink : IRenderSink
    {
        SinkGranularity granularity() const noexcept override { return SinkGranularity::Row; }
        bool begin(const SinkDescriptor &desc) override
        {
            clearError();
            m_desc = desc;
            m_frame.assign(static_cast<size_t>(desc.width) * desc.height * desc.samplesPerPixel, 0);
            return true;
        }
        bool writeRow(int row, std::vector<uint8_t> cmyk) override
        {
            const size_t rowBytes = static_cast<size_t>(m_desc.width) * m_desc.samplesPerPixel;
            if (cmyk.size() < rowBytes
                || static_cast<size_t>(row + 1) * rowBytes > m_frame.size()) {
                setError(EEError::sink_not_initialized);
                return false;
            }
            std::memcpy(m_frame.data() + static_cast<size_t>(row) * rowBytes, cmyk.data(), rowBytes);
            return true;
        }
        bool end() override { return true; }
        SinkDescriptor m_desc;
        std::vector<uint8_t> m_frame;
    };
    auto row = std::make_shared<RowSink>();
    RenderServices svcRow;
    svcRow.sink = row;
    std::error_code ec2;
    EXPECT_TRUE(json2sink(validJson.string(), svcRow, nullptr,
        [&](std::error_code ec) { ec2 = ec; }));
    EXPECT_FALSE(ec2);

    fs::remove(validJson);

    EXPECT_EQ(row->m_frame.size(), strip->frame().size());
    EXPECT_TRUE(row->m_frame == strip->frame());
}

TEST(json2sink_unsupported_granularity)
{
    // 接收器声明 Strip 粒度但未实现 writeStrip → 渲染阶段应报 sink_unsupported_granularity
    auto validJson = fs::temp_directory_path() / "ee_test_badgran.json";
    fs::remove(validJson);
    { std::ofstream f(validJson); f << R"({"canvas":{"width":40,"height":30}})"; }

    struct BadSink : IRenderSink
    {
        SinkGranularity granularity() const noexcept override { return SinkGranularity::Strip; }
        bool begin(const SinkDescriptor &) override { return true; }
        bool end() override { return true; }
    };
    auto sink = std::make_shared<BadSink>();
    RenderServices svc;
    svc.sink = sink;

    std::error_code got;
    bool ok = json2sink(validJson.string(), svc, nullptr,
        [&](std::error_code ec) { got = ec; });

    fs::remove(validJson);

    EXPECT_FALSE(ok);
    EXPECT_TRUE(got == EEError::sink_unsupported_granularity);
}

// ── 网格填充求值 ──────────────────────────────────────────────────────────

TEST(eval_grid_fill_center_line_is_grid_color)
{
    RenderContext ctx;
    GridFill g;
    g.gridColor = Color::fromCMYK(0, 0, 0, 255);           // 黑（K=100%）
    g.backgroundColor = Color::fromCMYK(0, 0, 0, 0);       // 纸白（全 0）
    g.cellWidth = 20; g.cellHeight = 20; g.lineWidth = 2;
    uint8_t c1 = 0, c2 = 0, c3 = 0, c4 = 0, a = 0;
    // px=0（竖线中心）：纯网格色（CMYK 黑 → K 通道满值）
    EXPECT_TRUE(ctx.evalGridFill(g, 0.0, 10.0, 100.0, 100.0, c1, c2, c3, c4, a));
    EXPECT_EQ(c1, 0); EXPECT_EQ(c2, 0); EXPECT_EQ(c3, 0); EXPECT_EQ(c4, 255);
    EXPECT_EQ(a, 255);
    // px=10（两条线之间正中，距线 10px > 线宽半宽 1px）：纯背景色（纸白 = 全 0）
    EXPECT_TRUE(ctx.evalGridFill(g, 10.0, 10.0, 100.0, 100.0, c1, c2, c3, c4, a));
    EXPECT_EQ(c1, 0); EXPECT_EQ(c2, 0); EXPECT_EQ(c3, 0); EXPECT_EQ(c4, 0);
    EXPECT_EQ(a, 255);
}

TEST(eval_grid_fill_one_pixel_line)
{
    // 固定 1px 网格线：最近像素整像素着墨，不做 smoothstep 摊开
    RenderContext ctx;
    GridFill g;
    g.gridColor = Color::fromCMYK(0, 0, 0, 255);           // 黑（K=100%）
    g.backgroundColor = Color::fromCMYK(0, 0, 0, 0);       // 纸白（全 0）
    g.cellWidth = 20; g.cellHeight = 20; g.lineWidth = 1;  // 解析器固定 1px
    uint8_t c1 = 0, c2 = 0, c3 = 0, c4 = 0, a = 0;
    // 线中心：纯网格色
    EXPECT_TRUE(ctx.evalGridFill(g, 0.0, 10.0, 100.0, 100.0, c1, c2, c3, c4, a));
    EXPECT_EQ(c1, 0); EXPECT_EQ(a, 255);
    // 距线 0.4px（同一像素内）：仍整像素着墨，无部分覆盖
    EXPECT_TRUE(ctx.evalGridFill(g, 0.4, 10.0, 100.0, 100.0, c1, c2, c3, c4, a));
    EXPECT_EQ(c1, 0); EXPECT_EQ(a, 255);
    // 距线 1px（相邻像素）：纯背景色（纸白全 0），无淡墨边
    EXPECT_TRUE(ctx.evalGridFill(g, 1.0, 10.0, 100.0, 100.0, c1, c2, c3, c4, a));
    EXPECT_EQ(c1, 0); EXPECT_EQ(c4, 0); EXPECT_EQ(a, 255);
}

TEST(eval_grid_fill_transparent_background)
{
    RenderContext ctx;
    GridFill g;
    g.gridColor = Color::fromCMYK(0, 0, 0, 255);           // 黑
    g.backgroundColor = Color::fromCMYK(0, 100, 100, 0);   // 红（应被忽略）
    g.transparentBackground = true;
    g.cellWidth = 20; g.cellHeight = 20; g.lineWidth = 2;
    uint8_t c1 = 0, c2 = 0, c3 = 0, c4 = 0, a = 0;
    // 线外：alpha 为 0（透出画布）
    EXPECT_TRUE(ctx.evalGridFill(g, 10.0, 10.0, 100.0, 100.0, c1, c2, c3, c4, a));
    EXPECT_EQ(a, 0);
    // 线上：纯网格色
    EXPECT_TRUE(ctx.evalGridFill(g, 0.0, 10.0, 100.0, 100.0, c1, c2, c3, c4, a));
    EXPECT_EQ(c1, 0); EXPECT_EQ(a, 255);
}

TEST(eval_grid_fill_horizontal_line)
{
    RenderContext ctx;
    GridFill g;
    g.gridColor = Color::fromCMYK(0, 0, 0, 255);           // 黑
    g.backgroundColor = Color::fromCMYK(0, 0, 0, 0);       // 纸白
    g.cellWidth = 20; g.cellHeight = 20; g.lineWidth = 2;
    uint8_t c1 = 0, c2 = 0, c3 = 0, c4 = 0, a = 0;
    // py=0（横线中心）而 px 在线外：仍是网格色
    EXPECT_TRUE(ctx.evalGridFill(g, 10.0, 0.0, 100.0, 100.0, c1, c2, c3, c4, a));
    EXPECT_EQ(c1, 0);
}

TEST(eval_grid_fill_invalid_params_returns_false)
{
    RenderContext ctx;
    GridFill g;
    g.cellWidth = 0; // 无效
    uint8_t c1 = 0, c2 = 0, c3 = 0, c4 = 0, a = 0;
    EXPECT_FALSE(ctx.evalGridFill(g, 0.0, 0.0, 100.0, 100.0, c1, c2, c3, c4, a));
}

// ── 纹理填充采样 ──────────────────────────────────────────────────────────

namespace {
// vips 可能未初始化（Json2Tiff 的 call_once 只在 json2tiff() 时触发）——
// 测试内自带 call_once + vips_init（与 DLL 侧重复调用也安全）。
void ensureVipsInit()
{
    static std::once_flag once;
    std::call_once(once, [] { vips_init("EEVerifyTests"); });
}

// 生成 2×2（4 像素）RGBA PNG 测试图：第 0 行 = 红(不透明) + 绿(不透明)，
// 第 1 行 = 蓝(不透明) + 透明。
// 采样约定（与 TextureSource 实现一致）：xF = u×srcW, yF = v×srcH，
// 最近邻采样（floor 归入像素格）——u ∈ [0, 0.5) → 红像素列，
// u ∈ [0.5, 1) → 绿像素列，v ∈ [0, 0.5) → 第 0 行，v ∈ [0.5, 1) → 第 1 行。
bool makeFixturePng(const std::string &path)
{
    ensureVipsInit();
    const std::vector<uint8_t> px = {
        255, 0, 0, 255, 0, 255, 0, 255,
        0, 0, 255, 255, 0, 0, 0, 0,
    };
    VipsImage *img = vips_image_new_from_memory(
        px.data(), px.size(), 2, 2, 4, VIPS_FORMAT_UCHAR);
    vips_image_set_int(VIPS_IMAGE(img), "interpretation", VIPS_INTERPRETATION_sRGB);
    int r = vips_pngsave(img, path.c_str(), nullptr);
    g_object_unref(img);
    return r == 0;
}

// 生成 2×2 RGBA TIFF 测试图：vips_tiffsave 写出 PHOTOMETRIC_RGB + spp=4 +
// EXTRASAMPLES[UNASSALPHA]（已实测验证）。与 PNG fixture 像素布局相同，
// 走 TextureSource 的 libtiff 解码路径，验证 spp=4 RGBA TIFF 的 alpha 直通。
bool makeRgbaTiff(const std::string &path)
{
    ensureVipsInit();
    const std::vector<uint8_t> px = {
        255, 0, 0, 255, 0, 255, 0, 255,
        0, 0, 255, 255, 0, 0, 0, 0,
    };
    VipsImage *img = vips_image_new_from_memory(
        px.data(), px.size(), 2, 2, 4, VIPS_FORMAT_UCHAR);
    vips_image_set_int(VIPS_IMAGE(img), "interpretation", VIPS_INTERPRETATION_sRGB);
    int r = vips_tiffsave(img, path.c_str(), nullptr);
    g_object_unref(img);
    return r == 0;
}

// 已加载真实 ICC profile 的 ColorConverter（TextureSource 测试专用）。
// 纹理解码必须经 lcms2 转 CMYK（与图片图元一致：lcms 不可用即解码失败，
// 无朴素公式回退），故测试需传已加载的 cv，不能用 nullptr。
// profile 文件来自项目 ICC Profile/ 目录（TEST_SOURCE_DIR = 仓库根）。
ColorConverter &loadedTextureConverter()
{
    static ColorConverter cv;
    static bool loaded = false;
    if (!loaded) {
        loaded = true;
        auto ec = cv.loadProfile(
            std::string(TEST_SOURCE_DIR) + "/ICC Profile/RGB/SRGB IEC61966-2.1.icc",
            std::string(TEST_SOURCE_DIR) + "/ICC Profile/CMYK/JapanColor2001Coated.icc");
        EXPECT_TRUE(!ec); // fixture 前提：profile 必须可加载
    }
    return cv;
}
} // namespace

// 分块转换结果必须与整幅单次转换逐字节一致（lcms 逐像素无状态）。
TEST(color_transform_chunked_matches_whole)
{
    auto &cv = loadedTextureConverter();
    const int w = 100, h = 53;
    std::vector<uint8_t> src(static_cast<size_t>(w) * h * 3);
    for (size_t i = 0; i < src.size(); ++i)
        src[i] = static_cast<uint8_t>((i * 31) & 0xFF);

    auto lcms = makeRgbToCmykLcms({}, &cv, 3);
    EXPECT_TRUE(lcms.ok);

    std::vector<uint8_t> whole(static_cast<size_t>(w) * h * 4);
    lcms.convert(src.data(), whole.data(), w * h);

    std::vector<uint8_t> chunked;
    // 7 行/块 → 53 = 7×7+4，末块非整，覆盖不均匀切分
    EXPECT_TRUE(convertChunked(lcms, src.data(), w, h, 3, chunked, 7));
    EXPECT_TRUE(chunked == whole);
}

TEST(color_transform_chunked_rejects_bad_transform)
{
    SourceToCmykLcms notOk; // 默认构造：ok=false
    std::vector<uint8_t> src(3 * 4 * 3, 0), out;
    EXPECT_FALSE(convertChunked(notOk, src.data(), 3, 4, 3, out, 2));
    EXPECT_TRUE(out.empty());
}

// PNG（非 TIFF）走 DecodedSource → makeToCmyk；不缩放时输出应等于
// 源缓冲直接分块转换的结果（回归锚定：分块 region 读取与整幅读取逐字节一致）。
TEST(image_png_decode_chunked_conversion_matches_reference)
{
    ensureVipsInit();
    auto &cv = loadedTextureConverter();

    const int w = 37, h = 23; // 奇数尺寸
    std::vector<uint8_t> src(static_cast<size_t>(w) * h * 3);
    for (size_t i = 0; i < src.size(); ++i)
        src[i] = static_cast<uint8_t>((i * 31) & 0xFF);

    std::string pngPath = (fs::temp_directory_path()
        / ("ee_chunk_png_" + std::to_string(std::rand()) + ".png")).string();
    {
        VipsImage *img = vips_image_new_from_memory(
            src.data(), src.size(), w, h, 3, VIPS_FORMAT_UCHAR);
        vips_image_set_int(VIPS_IMAGE(img), "interpretation", VIPS_INTERPRETATION_sRGB);
        EXPECT_FALSE(vips_pngsave(img, pngPath.c_str(), nullptr));
        g_object_unref(img);
    }

    ImageItem img;
    img.filePath = pngPath;
    img.width = w;   // 与源同尺寸：无缩放，但非 TIFF 仍走 DecodedSource
    img.height = h;
    img.x = img.y = img.z = 0;

    RenderContext ctx;
    ctx.converter = &cv;
    ctx.initTile(w, h, 0, 0, w, h);
    ImageRenderer::draw(ctx, img, Dpi{ 300, 300 });

    std::vector<uint8_t> expected;
    auto lcms = makeRgbToCmykLcms({}, &cv, 3);
    EXPECT_TRUE(lcms.ok);
    EXPECT_TRUE(convertChunked(lcms, src.data(), w, h, 3, expected, 7));

    EXPECT_TRUE(ctx.cmykBuf == expected);
    fs::remove(pngPath);
}

// RGB TIFF + 2× 放大走 DecodedSource 的 libtiff 整幅读取 + lcms 转换 + resizeExact。
// 参考模型：源像素分块转换 + 整数倍 NN 放大，逐像素比对（回归锚定）。
TEST(image_tiff_resize_chunked_conversion_matches_reference)
{
    ensureVipsInit();
    auto &cv = loadedTextureConverter();

    const uint32_t w = 37, h = 23;
    std::vector<uint8_t> src(static_cast<size_t>(w) * h * 3);
    for (size_t i = 0; i < src.size(); ++i)
        src[i] = static_cast<uint8_t>((i * 31) & 0xFF);

    std::string tifPath = (fs::temp_directory_path()
        / ("ee_chunk_tif_" + std::to_string(std::rand()) + ".tif")).string();
    {
        TIFF *tif = TIFFOpen(tifPath.c_str(), "w");
        TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, w);
        TIFFSetField(tif, TIFFTAG_IMAGELENGTH, h);
        TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, 3);
        TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, 8);
        TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB);
        TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
        TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP, 7); // 多 strip + 末条非整
        tstrip_t nStrips = TIFFNumberOfStrips(tif);
        for (tstrip_t si = 0; si < nStrips; ++si) {
            uint32_t rows = (si == nStrips - 1) ? (h - si * 7) : 7;
            size_t off = static_cast<size_t>(si) * 7 * w * 3;
            TIFFWriteEncodedStrip(tif, si, src.data() + off, static_cast<tsize_t>(rows * w * 3));
        }
        TIFFClose(tif);
    }

    ImageItem img;
    img.filePath = tifPath;
    img.width = w * 2; // 2× 放大 → needsResize → DecodedSource
    img.height = h * 2;
    img.x = img.y = img.z = 0;

    RenderContext ctx;
    ctx.converter = &cv;
    ctx.initTile(w * 2, h * 2, 0, 0, w * 2, h * 2);
    ImageRenderer::draw(ctx, img, Dpi{ 300, 300 });

    std::vector<uint8_t> cmykSrc;
    auto lcms = makeRgbToCmykLcms({}, &cv, 3);
    EXPECT_TRUE(lcms.ok);
    EXPECT_TRUE(convertChunked(lcms, src.data(), w, h, 3, cmykSrc, 7));

    std::vector<uint8_t> expected(static_cast<size_t>(w) * 2 * h * 2 * 4);
    for (int y = 0; y < static_cast<int>(h) * 2; ++y) {
        for (int x = 0; x < static_cast<int>(w) * 2; ++x) {
            const uint8_t *sp = cmykSrc.data() + (static_cast<size_t>(y / 2) * w + x / 2) * 4;
            uint8_t *dp = expected.data() + (static_cast<size_t>(y) * w * 2 + x) * 4;
            std::memcpy(dp, sp, 4);
        }
    }
    EXPECT_TRUE(ctx.cmykBuf == expected);
    fs::remove(tifPath);
}

// 流式（strip 逐段解码+缩放）在非整数放大下必须与整幅 NearestResampler 逐字节一致。
TEST(image_tiff_resize_streaming_matches_whole_resize)
{
    ensureVipsInit();
    auto &cv = loadedTextureConverter();

    const uint32_t w = 37, h = 23;
    std::vector<uint8_t> src(static_cast<size_t>(w) * h * 3);
    for (size_t i = 0; i < src.size(); ++i)
        src[i] = static_cast<uint8_t>((i * 31) & 0xFF);

    std::string tifPath = (fs::temp_directory_path()
        / ("ee_stream_tif_" + std::to_string(std::rand()) + ".tif")).string();
    {
        TIFF *tif = TIFFOpen(tifPath.c_str(), "w");
        TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, w);
        TIFFSetField(tif, TIFFTAG_IMAGELENGTH, h);
        TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, 3);
        TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, 8);
        TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB);
        TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
        TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP, 7);
        tstrip_t nStrips = TIFFNumberOfStrips(tif);
        for (tstrip_t si = 0; si < nStrips; ++si) {
            uint32_t rows = (si == nStrips - 1) ? (h - si * 7) : 7;
            size_t off = static_cast<size_t>(si) * 7 * w * 3;
            TIFFWriteEncodedStrip(tif, si, src.data() + off, static_cast<tsize_t>(rows * w * 3));
        }
        TIFFClose(tif);
    }

    const int dw = 100, dh = 60; // 非整数放大
    ImageItem img;
    img.filePath = tifPath;
    img.width = dw;
    img.height = dh;
    img.x = img.y = img.z = 0;

    RenderContext ctx;
    ctx.converter = &cv;
    ctx.initTile(dw, dh, 0, 0, dw, dh);
    ImageRenderer::draw(ctx, img, Dpi{ 300, 300 });

    std::vector<uint8_t> cmykSrc;
    auto lcms = makeRgbToCmykLcms({}, &cv, 3);
    EXPECT_TRUE(lcms.ok);
    EXPECT_TRUE(convertChunked(lcms, src.data(), w, h, 3, cmykSrc, 7));

    std::vector<uint8_t> expected(static_cast<size_t>(dw) * dh * 4);
    NearestResampler().resize(cmykSrc.data(), w, h, expected.data(), dw, dh, 4);

    EXPECT_TRUE(ctx.cmykBuf == expected);
    fs::remove(tifPath);
}

TEST(texture_source_opaque_sample)
{
    const std::string dir = (std::filesystem::temp_directory_path() / "eetexture").string();
    std::filesystem::create_directories(dir);
    const std::string png = dir + "/fixture.png";
    EXPECT_TRUE(makeFixturePng(png));

    TextureSource src(png, Dpi{ 300, 300 }, &loadedTextureConverter(), 8, 4); // 覆盖尺寸 8×4
    EXPECT_TRUE(src.ok());
    EXPECT_EQ(src.tileWidth(), 8);
    EXPECT_EQ(src.tileHeight(), 4);

    uint8_t c1 = 0, c2 = 0, c3 = 0, c4 = 0, a = 0;
    // u=0, v=0 → 左上角红像素
    EXPECT_TRUE(src.sample(0.0, 0.0, c1, c2, c3, c4, a));
    EXPECT_EQ(a, 255);
    // u=0.75, v=0.25 → xF=1.5, yF=0.5：最近邻归入 (1,0) 绿像素（不透明）
    EXPECT_TRUE(src.sample(0.75, 0.25, c1, c2, c3, c4, a));
    EXPECT_EQ(a, 255);
}

TEST(texture_source_alpha_sample)
{
    const std::string dir = (std::filesystem::temp_directory_path() / "eetexture").string();
    std::filesystem::create_directories(dir);
    const std::string png = dir + "/fixture.png";
    EXPECT_TRUE(makeFixturePng(png));

    TextureSource src(png, Dpi{ 300, 300 }, &loadedTextureConverter(), 8, 4);
    EXPECT_TRUE(src.ok());

    uint8_t c1 = 0, c2 = 0, c3 = 0, c4 = 0, a = 0;
    // 右下角 (u≈1, v≈1) → xF=1.98, yF=1.98 → (1,1) 透明像素
    EXPECT_TRUE(src.sample(0.99, 0.99, c1, c2, c3, c4, a));
    EXPECT_EQ(a, 0);
    // 左下角 (u≈0, v≈1) → xF=0.02, yF=1.98 → 最近邻归入 (0,1) 蓝像素（不透明）
    EXPECT_TRUE(src.sample(0.01, 0.99, c1, c2, c3, c4, a));
    EXPECT_EQ(a, 255);
}

TEST(texture_source_nearest_sample)
{
    const std::string dir = (std::filesystem::temp_directory_path() / "eetexture").string();
    std::filesystem::create_directories(dir);
    const std::string png = dir + "/fixture.png";
    EXPECT_TRUE(makeFixturePng(png));

    TextureSource src(png, Dpi{ 300, 300 }, &loadedTextureConverter(), 8, 4); // 覆盖到 8×4 → 4px/cell 宽
    EXPECT_TRUE(src.ok());

    uint8_t c1 = 0, c2 = 0, c3 = 0, c4 = 0, a = 0;
    // u=0.25, v=0.25 → xF=0.5, yF=0.5：最近邻归入 (0,0) 红像素（不透明）
    // （1:1 半像素对齐：采样点落在像素格内即取该格，不做邻域混合）
    EXPECT_TRUE(src.sample(0.25, 0.25, c1, c2, c3, c4, a));
    EXPECT_EQ(a, 255);
}

TEST(texture_source_rgba_tiff_alpha)
{
    // spp=4 RGBA TIFF（EXTRASAMPLES 首项 UNASSALPHA）→ alpha 直通
    const std::string dir = (std::filesystem::temp_directory_path() / "eetexture").string();
    std::filesystem::create_directories(dir);
    const std::string tif = dir + "/rgba_fixture.tif";
    EXPECT_TRUE(makeRgbaTiff(tif));

    TextureSource src(tif, Dpi{ 300, 300 }, &loadedTextureConverter(), 8, 4);
    EXPECT_TRUE(src.ok());

    uint8_t c1 = 0, c2 = 0, c3 = 0, c4 = 0, a = 0;
    // 左上角红像素：不透明
    EXPECT_TRUE(src.sample(0.0, 0.0, c1, c2, c3, c4, a));
    EXPECT_EQ(a, 255);
    // 右下角透明像素
    EXPECT_TRUE(src.sample(0.99, 0.99, c1, c2, c3, c4, a));
    EXPECT_EQ(a, 0);
}

TEST(texture_source_missing_file_ok_false)
{
    ensureVipsInit(); // vips 路径：文件不存在 → 元数据失败 → ok()==false
    TextureSource src("D:/no/such/file_xyz.png", Dpi{ 300, 300 }, nullptr);
    EXPECT_TRUE(!src.ok());
}

TEST(texture_source_override_zero_dimension_falls_back)
{
    // 覆盖尺寸某一维 ≤ 0 → 该维回退 DPI 换算的原图尺寸（不得为 1px 或 0）
    // fixture：Test/images/win/pattern_alpha.png，8×8，无嵌入 DPI → imgDpi 72；
    // canvasDpi 300 → 原图缩放 tile = round(8×300/72) = 33。
    ensureVipsInit();
    const std::string png =
        std::string(TEST_SOURCE_DIR) + "/Test/images/win/pattern_alpha.png";
    TextureSource src(png, Dpi{ 300, 300 }, &loadedTextureConverter(), 40, 0); // overrideW=40, overrideH=0
    EXPECT_TRUE(src.ok());
    EXPECT_EQ(src.tileWidth(), 40);
    EXPECT_EQ(src.tileHeight(), 33);

    // 采样不得崩溃/越界，alpha 始终在 [0, 255]
    uint8_t c1 = 0, c2 = 0, c3 = 0, c4 = 0, a = 0;
    for (double u : { 0.0, 0.5, 0.99 })
        for (double v : { 0.0, 0.5, 0.99 }) {
            EXPECT_TRUE(src.sample(u, v, c1, c2, c3, c4, a));
            EXPECT_TRUE(a <= 255);
            EXPECT_TRUE(a >= 0);
        }
}

TEST(texture_source_anisotropic_dpi_per_axis_tile)
{
    // 渲染层逐轴缩放（TextureSource m_tileW/m_tileH）：
    //   画布 Dpi{300,600} + 图片嵌入 X/YResolution = 300/600
    //   → tileW = 16×300/300 = 16、tileH = 16×600/600 = 16
    // 旧标量行为 max(imgDpiX, imgDpiY) = 600 对两轴同用
    //   → tileW = 16×300/600 = 8、tileH = 16×600/600 = 16
    //   与逐轴结果不同（8 ≠ 16），断言能区分各向异性与标量缩放。
    const std::string dir = (std::filesystem::temp_directory_path() / "eetexture").string();
    std::filesystem::create_directories(dir);
    const std::string tif = dir + "/aniso_dpi.tif";

    // libtiff 直写 16×16 RGBA 条纹 TIFF（单 strip），嵌入各向异性分辨率
    // （X=300 / Y=600，RESUNIT_INCH）——TIFF 是唯一能携带逐轴 DPI 的格式
    {
        TIFF *t = TIFFOpen(tif.c_str(), "w");
        EXPECT_TRUE(t != nullptr);
        if (!t)
            return;
        TIFFSetField(t, TIFFTAG_IMAGEWIDTH, 16);
        TIFFSetField(t, TIFFTAG_IMAGELENGTH, 16);
        TIFFSetField(t, TIFFTAG_SAMPLESPERPIXEL, 4);
        TIFFSetField(t, TIFFTAG_BITSPERSAMPLE, 8);
        TIFFSetField(t, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB);
        TIFFSetField(t, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
        TIFFSetField(t, TIFFTAG_RESOLUTIONUNIT, RESUNIT_INCH);
        TIFFSetField(t, TIFFTAG_XRESOLUTION, 300.0f);
        TIFFSetField(t, TIFFTAG_YRESOLUTION, 600.0f);
        std::vector<uint8_t> buf(16 * 16 * 4, 255);
        EXPECT_TRUE(TIFFWriteEncodedStrip(t, 0, buf.data(), static_cast<tsize_t>(buf.size()))
            >= 0);
        TIFFClose(t);
    }

    TextureSource src(tif, Dpi{ 300, 600 }, &loadedTextureConverter());
    EXPECT_TRUE(src.ok());
    EXPECT_EQ(src.tileWidth(), 16);  // 16 × dpiX(300) / imgDpiX(300)
    EXPECT_EQ(src.tileHeight(), 16); // 16 × dpiY(600) / imgDpiY(600)

    // 采样不越界；RGBA 无 EXTRASAMPLES → alpha 直通 255
    uint8_t c1 = 0, c2 = 0, c3 = 0, c4 = 0, a = 0;
    EXPECT_TRUE(src.sample(0.5, 0.5, c1, c2, c3, c4, a));
    EXPECT_EQ(a, 255);
}

// ── 预解码 wave 调度器 ─────────────────────────────────────────────────────

TEST(predecode_scheduler_all_fit_one_wave)
{
    auto waves = schedulePredecodeWaves({ 1, 2, 3 }, 10, 4);
    EXPECT_EQ(waves.size(), 1u);
    EXPECT_TRUE((waves[0] == std::vector<size_t>{ 0, 1, 2 }));
}

TEST(predecode_scheduler_budget_splits_waves)
{
    auto waves = schedulePredecodeWaves({ 5, 5, 5 }, 10, 4);
    EXPECT_EQ(waves.size(), 2u);
    EXPECT_TRUE((waves[0] == std::vector<size_t>{ 0, 1 }));
    EXPECT_TRUE((waves[1] == std::vector<size_t>{ 2 }));
}

TEST(predecode_scheduler_oversize_runs_alone)
{
    auto waves = schedulePredecodeWaves({ 12, 1, 1 }, 10, 4);
    EXPECT_EQ(waves.size(), 2u);
    EXPECT_TRUE((waves[0] == std::vector<size_t>{ 0 }));
    EXPECT_TRUE((waves[1] == std::vector<size_t>{ 1, 2 }));
}

TEST(predecode_scheduler_caps_parallelism)
{
    auto waves = schedulePredecodeWaves({ 1, 1, 1, 1 }, 100, 2);
    EXPECT_EQ(waves.size(), 2u);
    EXPECT_TRUE((waves[0] == std::vector<size_t>{ 0, 1 }));
    EXPECT_TRUE((waves[1] == std::vector<size_t>{ 2, 3 }));
}

TEST(predecode_scheduler_empty_and_zero_parallel)
{
    EXPECT_TRUE(schedulePredecodeWaves({}, 10, 4).empty());
    EXPECT_TRUE(schedulePredecodeWaves({ 1, 2 }, 10, 0).empty());
}

// 成本模型：图片源现在惰性解码（DecodedSource 按 band 流式，不物化渲染帧），
// 唯一瞬态是源分辨率解码（raw + CMYK，×1.5 安全系数）。超大源仍会超过默认
// 4 GB 预算从而独占串行，防止并行解码峰值失控。
TEST(predecode_cost_large_source_exceeds_default_budget)
{
    const uint64_t kDefaultBudget = 4ULL * 1024 * 1024 * 1024;
    uint64_t cost = estimateDecodeCost(30000, 30000, 7);
    EXPECT_TRUE(cost > kDefaultBudget);
}

TEST(predecode_cost_source_transient)
{
    // 只计源瞬态：srcW×srcH×chans×3/2
    uint64_t cost = estimateDecodeCost(37, 23, 7);
    EXPECT_EQ(cost, 37ULL * 23 * 7 * 3 / 2);
}

TEST(predecode_cost_zero_dims_floor)
{
    // 空/不可读 → cost 1（不会 0 导致漏调度）
    EXPECT_EQ(estimateDecodeCost(0, 0, 7), 1ULL);
}

// 预解码冒烟：3 张小 PNG 并行/串行都能完成，进度回调 done/total 语义正确。
TEST(scene_predecode_images_reports_all_items)
{
    ensureVipsInit();
    auto &cv = loadedTextureConverter();

    Canvas canvas;
    canvas.dpi = Dpi{ 300, 300 };
    // width/height 必须覆盖图元 bbox：preDecodeImages 的剔除逻辑镜像
    // renderTile（bbox 与 (0,0,W,H) 不相交即跳过），默认 0×0 会剔除全部图片
    canvas.width = 8;
    canvas.height = 8;
    for (int i = 0; i < 3; ++i) {
        std::vector<uint8_t> px(8 * 8 * 3);
        for (size_t k = 0; k < px.size(); ++k)
            px[k] = static_cast<uint8_t>((k + i * 40) & 0xFF);
        std::string p = (fs::temp_directory_path()
            / ("ee_pd_" + std::to_string(std::rand()) + ".png")).string();
        {
            VipsImage *v = vips_image_new_from_memory(
                px.data(), px.size(), 8, 8, 3, VIPS_FORMAT_UCHAR);
            vips_image_set_int(VIPS_IMAGE(v), "interpretation", VIPS_INTERPRETATION_sRGB);
            EXPECT_FALSE(vips_pngsave(v, p.c_str(), nullptr));
            g_object_unref(v);
        }
        ImageItem img;
        img.filePath = p;
        img.width = 8;
        img.height = 8;
        img.x = img.y = img.z = 0;
        canvas.images.push_back(img);
    }

    SceneRenderer renderer;
    renderer.setConverter(&cv);

    std::mutex m;
    std::vector<int> dones;
    renderer.preDecodeImages(canvas, [&](int done, int total, const std::string &) {
        std::lock_guard<std::mutex> lock(m);
        dones.push_back(done);
        EXPECT_EQ(total, 3);
    });

    EXPECT_EQ(dones.size(), 3u);
    for (int i = 1; i <= 3; ++i)
        EXPECT_TRUE(std::find(dones.begin(), dones.end(), i) != dones.end());
    for (const auto &img : canvas.images)
        fs::remove(img.filePath);
}
