#pragma once

#include "ExportEngine.h" // EE_API（dll 导出/导入）
#include <system_error>

namespace ATHC::EE {

// 单一错误域。值唯一、按域分组命名；每个码标注可恢复性
// （可恢复 = 渲染可优雅降级；不可恢复 = 操作中止）。
enum class EEError : int
{
    // ── io ──
    io_failed = 1, // 不可恢复
    // ── parse (JsonSceneParser) ──
    parse_cannot_open_file, // 不可恢复
    parse_invalid_json, // 不可恢复
    parse_root_not_object, // 不可恢复
    parse_invalid_canvas_unit, // 不可恢复
    parse_dimensions_not_positive, // 不可恢复
    parse_dpi_not_positive, // 不可恢复
    parse_converted_dimensions_not_positive, // 不可恢复
    parse_dpi_deprecated, // 不可恢复：旧字段 dpi 已废弃，改用 dpiX/dpiY
    parse_invalid_color, // 不可恢复：颜色必须是对象
    parse_rgba_color_removed, // 不可恢复：RGB 颜色已移除，改用 c/m/y/k
    parse_color_space_field_removed, // 不可恢复：space 字段已移除，颜色直接写 c/m/y/k
    // ── icc (ColorConverter) ──
    icc_open_failed, // 不可恢复
    icc_parse_failed, // 不可恢复
    icc_not_initialized, // 不可恢复
    // ── tiff (TiffWriter) ──
    tiff_open_failed, // 不可恢复
    tiff_no_session, // 不可恢复（防御性）
    tiff_write_strip_failed, // 不可恢复
    // ── font (FontEngine) ──
    font_library_init_failed, // 不可恢复（引擎不可用）
    font_invalid_family_name, // 可恢复 → 降级矩形
    font_not_found, // 可恢复 → 降级矩形
    font_face_load_failed, // 可恢复 → 降级矩形
    font_cairo_face_failed, // 可恢复 → 降级矩形
    font_scaled_font_failed, // 可恢复 → 降级矩形
    // ── render (ImageRenderer) ──
    image_decode_failed, // 可恢复 → 跳过图片（warn）
    image_unsupported_colourspace, // 可恢复 → 跳过图片（warn）
    image_band_count_mismatch, // 可恢复 → 跳过图片（warn）
    // ── composite (TiffWriter←ColorConverter 路径) ──
    composite_failed, // 不可恢复
    // ── sink (IRenderSink) ──
    sink_unsupported_granularity, // 不可恢复：接收器未实现其声明的粒度入口
    sink_not_initialized, // 不可恢复：write 发生在 begin 前 / 尺寸不匹配
    sink_write_failed, // 不可恢复：接收器未能消费渲染数据（自定义回调返回 false 等）
};

class EEErrorCategory final : public std::error_category
{
public:
    const char *name() const noexcept override { return "EEError"; }

    std::string message(int ev) const override
    {
        switch (static_cast<EEError>(ev)) {
        case EEError::io_failed:
            return "通用 I/O 失败";
        case EEError::parse_cannot_open_file:
            return "无法打开场景 JSON 文件";
        case EEError::parse_invalid_json:
            return "场景文件中 JSON 无效";
        case EEError::parse_root_not_object:
            return "JSON 根节点必须是对象";
        case EEError::parse_invalid_canvas_unit:
            return "无效的画布单位（有效值：px, mm）";
        case EEError::parse_dimensions_not_positive:
            return "画布尺寸必须为正数";
        case EEError::parse_dpi_not_positive:
            return "画布 DPI 必须为正数";
        case EEError::parse_converted_dimensions_not_positive:
            return "单位转换后的画布尺寸必须为正数";
        case EEError::parse_dpi_deprecated:
            return "画布字段 'dpi' 已废弃，请使用 'dpiX' 和 'dpiY'";
        case EEError::parse_invalid_color:
            return "颜色必须是对象";
        case EEError::parse_rgba_color_removed:
            return "RGB 颜色已移除，请使用 CMYK (c/m/y/k)";
        case EEError::parse_color_space_field_removed:
            return "颜色字段 'space' 已移除，直接写入 c/m/y/k";
        case EEError::icc_open_failed:
            return "无法打开 ICC 配置文件";
        case EEError::icc_parse_failed:
            return "解析 ICC 配置文件失败";
        case EEError::icc_not_initialized:
            return "颜色转换器未初始化";
        case EEError::tiff_open_failed:
            return "打开 TIFF 文件写入失败";
        case EEError::tiff_no_session:
            return "TIFF 写入操作没有活动会话";
        case EEError::tiff_write_strip_failed:
            return "libtiff 写入条带失败";
        case EEError::font_library_init_failed:
            return "FreeType 库初始化失败";
        case EEError::font_invalid_family_name:
            return "无效的字体族名称";
        case EEError::font_not_found:
            return "系统中未找到该字体族";
        case EEError::font_face_load_failed:
            return "加载字体文件失败";
        case EEError::font_cairo_face_failed:
            return "创建 Cairo 字体面失败";
        case EEError::font_scaled_font_failed:
            return "创建缩放字体失败";
        case EEError::image_decode_failed:
            return "解码图像失败";
        case EEError::image_unsupported_colourspace:
            return "图像使用了不支持的颜色空间";
        case EEError::image_band_count_mismatch:
            return "图像波段数量不匹配";
        case EEError::composite_failed:
            return "图块/条带合成失败";
        case EEError::sink_unsupported_granularity:
            return "接收器不支持请求的粒度";
        case EEError::sink_not_initialized:
            return "在 begin 之前调用 sink write 或尺寸不匹配";
        case EEError::sink_write_failed:
            return "接收器未能消费渲染数据";
        }
        return "未知 EEError";
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

// EE_API: 从 ExportEngine.dll 导出，以便 EXE 和 DLL 共享同一个
// 错误类别实例。std::error_code 通过指针比较类别对象——如果每个模块使用独立的静态变量，
// 则 DLL 内部创建的 error_code 将永远无法与测试/消费者代码中转换的枚举值相等。
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

template<> struct std::is_error_code_enum<ATHC::EE::EEError> : std::true_type
{ };
