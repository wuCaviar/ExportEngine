#include "FontEngine.h"
#include "Log.h"
#include <cairo.h>
#include <cairo-ft.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <cstring>
#include <functional>
#include <mutex>

using namespace ATHC::EE;

namespace {
// Font creation (registry lookup + FT_New_Face + cairo font face/scaled font)
// is not safe to run concurrently: cairo's global unscaled-font machinery
// and FreeType streams can leave one clone's first font with a broken
// scaled font (glyph rasterization fails with CAIRO_STATUS_FREETYPE_ERROR).
// Font loads are rare (cached per key), so a process-wide lock is cheap.
std::mutex g_fontLoadMutex;

// Fallback width estimate based on Unicode codepoint count.
double fallbackWidth(const std::string &text, double fontSize)
{
    int charCount = 0;
    int pos = 0;
    int totalLen = static_cast<int>(text.size());
    while (pos < totalLen) {
        unsigned int cp = 0;
        int bytes = utf8Decode(text.c_str() + pos, totalLen - pos, cp);
        if (bytes <= 0)
            break;
        pos += bytes;
        ++charCount;
    }
    return fontSize * 0.6 * static_cast<double>(charCount);
}

// Fallback font candidates: wide-coverage (CJK) families probed in order
// when the requested font lacks a glyph. lookupFontFile() is platform-
// specific, so a cross-platform list works — families that are not
// installed simply resolve to "" and are skipped.
const char *kFallbackCandidates[] = {
    "Microsoft YaHei",     // Windows CJK
    "SimSun",              // Windows CJK (legacy)
    "DengXian",            // Windows CJK (modern, Office)
    "PingFang SC",         // macOS CJK
    "Heiti SC",            // macOS CJK (legacy)
    "STHeiti",             // macOS CJK (legacy)
    "Noto Sans CJK SC",    // Linux
    "Noto Sans SC",        // Linux (variable font)
    "WenQuanYi Micro Hei", // Linux
    "Arial Unicode MS",    // general coverage
};

// RAII helpers for Cairo C types
struct CairoFontFaceGuard {
    cairo_font_face_t *ptr = nullptr;
    ~CairoFontFaceGuard() { if (ptr) cairo_font_face_destroy(ptr); }
};
struct CairoScaledFontGuard {
    cairo_scaled_font_t *ptr = nullptr;
    ~CairoScaledFontGuard() { if (ptr) cairo_scaled_font_destroy(ptr); }
};

// Tiny RAII for FT_Face
struct FtFaceGuard {
    FT_Face ptr = nullptr;
    FT_Library lib = nullptr; // needed for FT_Done_Face
    ~FtFaceGuard() { if (ptr) FT_Done_Face(ptr); ptr = nullptr; }
};

} // namespace

size_t FontCacheKeyHash::operator()(const FontCacheKey &k) const
{
    size_t h = std::hash<std::string>{}(k.family);
    uint64_t sizeBits = 0;
    std::memcpy(&sizeBits, &k.size, sizeof(sizeBits));
    h ^= std::hash<uint64_t>{}(sizeBits) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
    h ^= std::hash<bool>{}(k.bold) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
    h ^= std::hash<bool>{}(k.italic) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
    return h;
}

FontEngine::FontEngine() { }
FontEngine::~FontEngine()
{
    cleanup();
}

std::error_code FontEngine::init()
{
    cleanup();

    FT_Error err = FT_Init_FreeType(&m_ftLibrary);
    if (err) {
        // 一次性初始化，低频 —— 保留源码处日志（日志策略例外）
        EELog::error("FontEngine: FT_Init_FreeType failed (error {})", static_cast<int>(err));
        m_ftLibrary = nullptr;
        return EEError::font_library_init_failed;
    }
    return {};
}

void FontEngine::cleanup()
{
    // Destroy in dependency order: per-font scaled_font → font_face →
    // FT_Face, then FT_Library. The current-font pointers reference cache
    // entries, so clearing the cache destroys everything.
    for (auto &kv : m_fontCache) {
        auto &f = kv.second;
        if (f.scaledFont)
            cairo_scaled_font_destroy(f.scaledFont);
        if (f.fontFace)
            cairo_font_face_destroy(f.fontFace);
        if (f.ftFace)
            FT_Done_Face(f.ftFace);
    }
    m_fontCache.clear();
    m_scaledFont = nullptr;
    m_fontFace = nullptr;
    m_ftFace = nullptr;
    if (m_ftLibrary) {
        FT_Done_FreeType(m_ftLibrary);
        m_ftLibrary = nullptr;
    }
    m_fontPathCache.clear();
    m_fallbackCache.clear();
    m_currentFont = {};
}

void FontEngine::resetKerning()
{
    // Cairo handles kerning automatically via cairo_show_text() / glyph API.
    // No state to reset.
}

void FontEngine::clearCache()
{
    m_fontPathCache.clear();
    m_fallbackCache.clear();
    m_currentFont = {};
}

std::error_code FontEngine::loadFont(const std::string &fontFamily, double fontSize, bool bold, bool italic)
{
    if (!m_ftLibrary)
        return EEError::font_library_init_failed;
    if (fontFamily.empty())
        return EEError::font_invalid_family_name;
    if (!(fontSize > 0.0))
        return EEError::font_invalid_family_name;

    std::lock_guard<std::mutex> lock(g_fontLoadMutex);

    // Sanitise font family name
    for (unsigned char ch : fontFamily) {
        if (ch < 0x20 || ch == 0x7F)
            return EEError::font_invalid_family_name;
        if (ch == '/' || ch == '\\' || ch == ':')
            return EEError::font_invalid_family_name;
    }
    if (fontFamily == ".." || fontFamily.find("/../") != std::string::npos
        || fontFamily.find("\\..\\") != std::string::npos
        || fontFamily.find("../") != std::string::npos
        || fontFamily.find("..\\") != std::string::npos
        || fontFamily.find("/..") != std::string::npos
        || fontFamily.find("\\..") != std::string::npos)
        return EEError::font_invalid_family_name;

    FontCacheKey key{ fontFamily, fontSize, bold, italic };
    if (m_currentFont == key)
        return {};

    // Cache hit: switch pointers only. The previous font stays alive in the
    // cache — destroying an FT_Face lets FreeType reuse its address, and
    // Cairo's unscaled-font map keys on face addresses (see CachedFont).
    auto cachedIt = m_fontCache.find(key);
    if (cachedIt != m_fontCache.end()) {
        m_scaledFont = cachedIt->second.scaledFont;
        m_fontFace = cachedIt->second.fontFace;
        m_ftFace = cachedIt->second.ftFace;
        m_currentFont = key;
        return {};
    }

    // ── Resolve font file ──────────────────────────────────────────────
    // Prefer the exact bold/italic variant file; when a requested style
    // has no variant file, fall back to the closest face and let Cairo
    // synthesize the missing style (cairo_ft_font_face_set_synthesize
    // below) instead of silently dropping it. lookupFontFile() falls back
    // to the base-family registration when a variant is missing, so a
    // style is only "real" when its resolved path differs from the
    // regular face's path.
    std::string regularPath = lookupFontFile(fontFamily, false, false);
    std::string boldPath    = lookupFontFile(fontFamily, true, false);
    std::string italicPath  = lookupFontFile(fontFamily, false, true);
    std::string biPath      = lookupFontFile(fontFamily, true, true);

    bool hasBold   = !boldPath.empty()   && boldPath   != regularPath;
    bool hasItalic = !italicPath.empty() && italicPath != regularPath;
    bool hasBI     = !biPath.empty() && biPath != regularPath
                     && biPath != boldPath && biPath != italicPath;

    std::string fontPath;
    unsigned synthFlags = 0;
    if (bold && italic) {
        if (hasBI)
            fontPath = biPath;
        else if (hasBold) {
            fontPath = boldPath;
            synthFlags = CAIRO_FT_SYNTHESIZE_OBLIQUE;
        } else if (hasItalic) {
            fontPath = italicPath;
            synthFlags = CAIRO_FT_SYNTHESIZE_BOLD;
        } else if (!regularPath.empty()) {
            fontPath = regularPath;
            synthFlags = CAIRO_FT_SYNTHESIZE_BOLD | CAIRO_FT_SYNTHESIZE_OBLIQUE;
        }
    } else if (bold) {
        if (hasBold)
            fontPath = boldPath;
        else if (!regularPath.empty()) {
            fontPath = regularPath;
            synthFlags = CAIRO_FT_SYNTHESIZE_BOLD;
        }
    } else if (italic) {
        if (hasItalic)
            fontPath = italicPath;
        else if (!regularPath.empty()) {
            fontPath = regularPath;
            synthFlags = CAIRO_FT_SYNTHESIZE_OBLIQUE;
        }
    } else {
        fontPath = regularPath;
    }

    if (fontPath.empty())
        return EEError::font_not_found;

    // ── Load face via FreeType ─────────────────────────────────────────
    FT_Face newFace = nullptr;
    FT_Error err = FT_New_Face(m_ftLibrary, fontPath.c_str(), 0, &newFace);
    if (err || !newFace)
        return EEError::font_face_load_failed;

    // Create Cairo font face from the FT_Face (Cairo takes ownership of
    // the reference, but we must keep the FT_Face alive).
    cairo_font_face_t *newFontFace = cairo_ft_font_face_create_for_ft_face(newFace, 0);
    if (!newFontFace || cairo_font_face_status(newFontFace) != CAIRO_STATUS_SUCCESS) {
        FT_Done_Face(newFace);
        return EEError::font_cairo_face_failed;
    }

    // Synthesize missing styles (faux bold / faux oblique). Set on the
    // face BEFORE the scaled font is created, so Cairo applies them when
    // loading glyphs — the scaled font then carries the style and the
    // renderer needs no extra transform.
    if (synthFlags != 0)
        cairo_ft_font_face_set_synthesize(newFontFace, synthFlags);

    // ── Create scaled font ─────────────────────────────────────────────
    cairo_matrix_t fontMatrix;
    cairo_matrix_init_scale(&fontMatrix, fontSize, fontSize);

    cairo_matrix_t ctm;
    cairo_matrix_init_identity(&ctm);

    cairo_font_options_t *options = cairo_font_options_create();
    cairo_font_options_set_hint_style(options, CAIRO_HINT_STYLE_FULL);
    cairo_font_options_set_hint_metrics(options, CAIRO_HINT_METRICS_ON);
    cairo_font_options_set_antialias(options, CAIRO_ANTIALIAS_GRAY);

    cairo_scaled_font_t *newScaledFont = cairo_scaled_font_create(
        newFontFace, &fontMatrix, &ctm, options);
    cairo_font_options_destroy(options);

    if (!newScaledFont
        || cairo_scaled_font_status(newScaledFont) != CAIRO_STATUS_SUCCESS) {
        cairo_font_face_destroy(newFontFace);
        FT_Done_Face(newFace);
        return EEError::font_scaled_font_failed;
    }

    // ── Store in cache and switch ──────────────────────────────────────
    // The previous font is NOT destroyed — it stays alive in the cache so
    // its FT face address is never reused (see CachedFont).
    m_fontCache[key] = CachedFont{ newScaledFont, newFontFace, newFace };
    m_scaledFont = newScaledFont;
    m_fontFace = newFontFace;
    m_ftFace = newFace;

    m_currentFont = key;
    return {};
}

double FontEngine::measureTextWidth(
    const std::string &text, const std::string &fontFamily, double fontSize, bool bold, bool italic)
{
    if (text.empty())
        return 0.0;

    if (!m_scaledFont)
        return fallbackWidth(text, fontSize);

    FontCacheKey key{ fontFamily, fontSize, bold, italic };
    if (!(m_currentFont == key))
        return fallbackWidth(text, fontSize);

    // Use a recording surface for lightweight measurement (no pixel
    // allocation needed — Cairo only tracks the bounding boxes).
    cairo_surface_t *rec =
        cairo_recording_surface_create(CAIRO_CONTENT_ALPHA, nullptr);
    cairo_t *cr = cairo_create(rec);
    cairo_set_scaled_font(cr, m_scaledFont);

    cairo_text_extents_t extents;
    cairo_text_extents(cr, text.c_str(), &extents);

    double width = extents.x_advance;

    cairo_destroy(cr);
    cairo_surface_destroy(rec);

    return width;
}

bool FontEngine::hasGlyph(unsigned int codepoint) const
{
    if (!m_ftFace || !m_ftFace->charmap)
        return true; // face cannot be inspected — keep current behaviour
    return FT_Get_Char_Index(m_ftFace, codepoint) != 0;
}

std::string FontEngine::resolveFallbackFamily(unsigned int codepoint)
{
    auto it = m_fallbackCache.find(codepoint);
    if (it != m_fallbackCache.end())
        return it->second;

    std::string result;
    // Probe candidates with a temporary FT face only — never touch the
    // current font state (m_currentFont / m_scaledFont stay intact).
    for (const char *family : kFallbackCandidates) {
        std::string path = lookupFontFile(family, false, false);
        if (path.empty())
            continue;
        FT_Face face = nullptr;
        FT_Error err = FT_New_Face(m_ftLibrary, path.c_str(), 0, &face);
        if (err || !face)
            continue;
        bool covers = face->charmap && FT_Get_Char_Index(face, codepoint) != 0;
        FT_Done_Face(face);
        if (covers) {
            result = family;
            break;
        }
    }

    m_fallbackCache[codepoint] = result;
    return result;
}

std::unique_ptr<FontEngine> FontEngine::cloneForThread() const
{
    auto clone = std::make_unique<FontEngine>();
    if (clone->init())
        return nullptr;

    clone->m_fontPathCache = m_fontPathCache;
    clone->m_fallbackCache = m_fallbackCache;

    if (!m_currentFont.family.empty()) {
        if (clone->loadFont(m_currentFont.family, m_currentFont.size,
                m_currentFont.bold, m_currentFont.italic))
            return nullptr;
    }

    return clone;
}
