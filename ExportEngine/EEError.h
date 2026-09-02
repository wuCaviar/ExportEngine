#pragma once

#include "ExportEngine.h" // EE_API (dllexport / dllimport)
#include <system_error>

namespace ATHC::EE {

// 单一错误域。值唯一、按域分组命名；每个码标注可恢复性
// （可恢复 = 渲染可优雅降级；不可恢复 = 操作中止）。
enum class EEError : int
{
    // ── io ──
    io_failed = 1, // 不可恢复
    // ── parse (JsonSceneParser) ──
    parse_cannot_open_file,                  // 不可恢复
    parse_invalid_json,                      // 不可恢复
    parse_root_not_object,                   // 不可恢复
    parse_invalid_canvas_unit,               // 不可恢复
    parse_dimensions_not_positive,           // 不可恢复
    parse_dpi_not_positive,                  // 不可恢复
    parse_converted_dimensions_not_positive, // 不可恢复
    parse_dpi_deprecated,                    // 不可恢复：旧字段 dpi 已废弃，改用 dpiX/dpiY
    parse_invalid_color,                     // 不可恢复：颜色必须是对象
    parse_rgba_color_removed,                // 不可恢复：RGB 颜色已移除，改用 c/m/y/k
    parse_color_space_field_removed,         // 不可恢复：space 字段已移除，颜色直接写 c/m/y/k
    // ── icc (LcmsColorConverter) ──
    icc_open_failed,     // 不可恢复
    icc_parse_failed,    // 不可恢复
    icc_not_initialized, // 不可恢复
    // ── tiff (TiffWriter) ──
    tiff_open_failed,        // 不可恢复
    tiff_no_session,         // 不可恢复（防御性）
    tiff_write_strip_failed, // 不可恢复
    // ── font（已退役：文字渲染改用 Pango 后不再产生；保留值以维持 ABI）──
    font_library_init_failed, // 已退役
    font_invalid_family_name, // 已退役
    font_not_found,           // 已退役
    font_face_load_failed,    // 已退役
    font_cairo_face_failed,   // 已退役
    font_scaled_font_failed,  // 已退役
    // ── render (ImageRenderer) ──
    image_decode_failed,           // 可恢复 → 跳过图片（warn）
    image_unsupported_colourspace, // 可恢复 → 跳过图片（warn）
    image_band_count_mismatch,     // 可恢复 → 跳过图片（warn）
    // ── composite (TiffWriter←ColorConverter 路径) ──
    composite_failed, // 不可恢复
    // ── sink (IRenderSink) ──
    sink_unsupported_granularity, // 不可恢复：接收器未实现其声明的粒度入口
    sink_not_initialized,         // 不可恢复：write 发生在 begin 前 / 尺寸不匹配
    sink_write_failed,            // 不可恢复：接收器未能消费渲染数据（自定义回调返回 false 等）
};

class EEErrorCategory final : public std::error_category
{
public:
    const char *name() const noexcept override { return "EEError"; }

    std::string message(int ev) const override
    {
        switch (static_cast<EEError>(ev)) {
        case EEError::io_failed:
            return "generic I/O failure";
        case EEError::parse_cannot_open_file:
            return "cannot open scene JSON file";
        case EEError::parse_invalid_json:
            return "invalid JSON in scene file";
        case EEError::parse_root_not_object:
            return "JSON root must be an object";
        case EEError::parse_invalid_canvas_unit:
            return "invalid canvas unit (valid: px, mm)";
        case EEError::parse_dimensions_not_positive:
            return "canvas dimensions must be positive";
        case EEError::parse_dpi_not_positive:
            return "canvas DPI must be positive";
        case EEError::parse_converted_dimensions_not_positive:
            return "canvas dimensions after unit conversion must be positive";
        case EEError::parse_dpi_deprecated:
            return "canvas field 'dpi' is deprecated, use 'dpiX' and 'dpiY'";
        case EEError::parse_invalid_color:
            return "color must be an object";
        case EEError::parse_rgba_color_removed:
            return "RGB colors are removed, use CMYK (c/m/y/k)";
        case EEError::parse_color_space_field_removed:
            return "color field 'space' is removed, write c/m/y/k directly";
        case EEError::icc_open_failed:
            return "failed to open ICC profile file";
        case EEError::icc_parse_failed:
            return "failed to parse ICC profile";
        case EEError::icc_not_initialized:
            return "colour converter not initialized";
        case EEError::tiff_open_failed:
            return "failed to open TIFF file for writing";
        case EEError::tiff_no_session:
            return "TIFF write operation without an active session";
        case EEError::tiff_write_strip_failed:
            return "libtiff failed to write a strip";
        case EEError::font_library_init_failed:
            return "FreeType library initialization failed";
        case EEError::font_invalid_family_name:
            return "invalid font family name";
        case EEError::font_not_found:
            return "font family not found on this system";
        case EEError::font_face_load_failed:
            return "failed to load font file";
        case EEError::font_cairo_face_failed:
            return "failed to create Cairo font face";
        case EEError::font_scaled_font_failed:
            return "failed to create scaled font";
        case EEError::image_decode_failed:
            return "failed to decode image";
        case EEError::image_unsupported_colourspace:
            return "image uses an unsupported colourspace";
        case EEError::image_band_count_mismatch:
            return "image band count mismatch";
        case EEError::composite_failed:
            return "tile/strip composite failed";
        case EEError::sink_unsupported_granularity:
            return "sink does not support the requested granularity";
        case EEError::sink_not_initialized:
            return "sink write called before begin or size mismatch";
        case EEError::sink_write_failed:
            return "sink failed to consume rendered data";
        }
        return "unknown EEError";
    }

    bool recoverable(int ev) const noexcept
    {
        switch (static_cast<EEError>(ev)) {
        case EEError::font_invalid_family_name:
        case EEError::font_not_found:
        case EEError::font_face_load_failed:
        case EEError::font_cairo_face_failed:
        case EEError::font_scaled_font_failed:
        case EEError::image_decode_failed:
        case EEError::image_unsupported_colourspace:
        case EEError::image_band_count_mismatch:
            return true;
        default:
            return false;
        }
    }
};

// EE_API: exported from ExportEngine.dll so EXEs and the DLL share ONE
// category instance. std::error_code equality compares category objects by
// pointer — a per-module static would make error_codes created inside the
// DLL never compare equal to enums converted in test/consumer code.
inline EE_API const std::error_category &eeErrorCategory() noexcept
{
    static const EEErrorCategory s_category;
    return s_category;
}

inline std::error_code make_error_code(EEError e) noexcept
{
    return { static_cast<int>(e), eeErrorCategory() };
}

// 统一可恢复性查询：ec 属于本域时查表，其它域（system 等）一律 false
inline bool isRecoverable(const std::error_code &ec) noexcept
{
    if (ec.category() != eeErrorCategory())
        return false;
    return static_cast<const EEErrorCategory &>(ec.category()).recoverable(ec.value());
}

} // namespace ATHC::EE

template<>
struct std::is_error_code_enum<ATHC::EE::EEError> : std::true_type
{ };
