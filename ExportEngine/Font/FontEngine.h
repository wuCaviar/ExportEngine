#pragma once

#include "EEError.h"
#include <memory>
#include <string>
#include <unordered_map>
#include <cstdint>

// Forward declarations — Cairo C types
typedef struct _cairo_scaled_font cairo_scaled_font_t;
typedef struct _cairo_font_face cairo_font_face_t;
// FreeType C types
typedef struct FT_LibraryRec_ *FT_Library;
typedef struct FT_FaceRec_ *FT_Face;

namespace ATHC::EE {

// Font cache key for text measurement and font loading
struct FontCacheKey {
    std::string family;
    double size;
    bool bold;
    bool italic;
    bool operator==(const FontCacheKey &other) const
    {
        return family == other.family && size == other.size && bold == other.bold
               && italic == other.italic;
    }
};

// Hash support so FontCacheKey can be a std::unordered_map key.
struct FontCacheKeyHash {
    size_t operator()(const FontCacheKey &k) const;
};

// Platform-independent FreeType font engine backed by Cairo.
class FontEngine
{
public:
    FontEngine();
    ~FontEngine();
    FontEngine(const FontEngine &) = delete;
    FontEngine &operator=(const FontEngine &) = delete;

    std::error_code init();
    void cleanup();

    std::error_code loadFont(const std::string &fontFamily, double fontSize, bool bold, bool italic);

    double measureTextWidth(const std::string &text, const std::string &fontFamily,
        double fontSize, bool bold, bool italic);

    void resetKerning();

    // Drop cached font file lookups so subsequent loads observe newly installed fonts.
    void clearCache();

    // Create a lightweight, thread-independent copy.
    std::unique_ptr<FontEngine> cloneForThread() const;

    // Cairo font accessors for use by TextRenderer
    cairo_scaled_font_t *scaledFont() const { return m_scaledFont; }
    cairo_font_face_t *fontFace() const { return m_fontFace; }

    // Does the currently loaded font cover this codepoint? Returns true when
    // the face cannot be inspected (no charmap) so callers keep current
    // behaviour instead of falling back spuriously.
    bool hasGlyph(unsigned int codepoint) const;

    // Resolve the first installed fallback font family that covers a
    // codepoint the current font lacks (for CJK / wide-coverage text).
    // Returns "" when no candidate covers it. Results are cached per
    // codepoint — the answer is global (candidate list is static), not
    // dependent on the current font.
    std::string resolveFallbackFamily(unsigned int codepoint);

private:
    std::string lookupFontFile(const std::string &familyName, bool bold, bool italic);

    // A loaded font. Loaded fonts are kept alive for the engine's lifetime:
    // destroying an FT_Face lets FreeType reuse its address, and Cairo's
    // global unscaled-font map keys on FT face addresses — a reused address
    // then resolves to a stale unscaled font and glyph rasterization fails
    // silently with CAIRO_STATUS_FREETYPE_ERROR. Keeping faces alive avoids
    // the reuse entirely (and makes font switching cheap).
    struct CachedFont
    {
        cairo_scaled_font_t *scaledFont = nullptr;
        cairo_font_face_t *fontFace = nullptr;
        FT_Face ftFace = nullptr;
        // Style synthesis (faux bold / faux oblique) is baked into the
        // cairo font face via cairo_ft_font_face_set_synthesize, so the
        // cache needs no per-style flag.
    };

    // Owned Cairo / FreeType objects — must be destroyed in correct order:
    //   per-font: scaled_font → font_face → FT_Face, then FT_Library
    cairo_scaled_font_t *m_scaledFont = nullptr;
    cairo_font_face_t *m_fontFace = nullptr;
    FT_Face m_ftFace = nullptr;
    FT_Library m_ftLibrary = nullptr;

    FontCacheKey m_currentFont;
    std::unordered_map<std::string, std::string> m_fontPathCache;
    std::unordered_map<unsigned int, std::string> m_fallbackCache;
    std::unordered_map<FontCacheKey, CachedFont, FontCacheKeyHash> m_fontCache;
};

// Decode the next UTF-8 codepoint from a string.
// Returns bytes consumed (1-4), or 0 at end of string.
// On any malformed input (invalid lead byte, bad continuation byte,
// overlong encoding, UTF-16 surrogate, or out-of-range codepoint) the
// function emits U+FFFD and returns 1, so callers advance and resync.
inline int utf8Decode(const char *s, int len, unsigned int &codepoint)
{
    if (len <= 0)
        return 0;
    const uint8_t *p = reinterpret_cast<const uint8_t *>(s);
    uint8_t b0 = p[0];

    // ASCII fast path.
    if (b0 < 0x80) {
        codepoint = b0;
        return 1;
    }

    // Reject invalid lead bytes: 0x80-0xBF (continuation), 0xC0-0xC1 (overlong 2-byte),
    // 0xF5-0xFF (would decode to > U+10FFFF).
    if (b0 < 0xC2 || b0 > 0xF4) {
        codepoint = 0xFFFD;
        return 1;
    }

    int seqLen = 0;
    unsigned int minCp = 0;
    if ((b0 & 0xE0) == 0xC0) {
        seqLen = 2;
        minCp = 0x80;
    } else if ((b0 & 0xF0) == 0xE0) {
        seqLen = 3;
        minCp = 0x800;
    } else if ((b0 & 0xF8) == 0xF0) {
        seqLen = 4;
        minCp = 0x10000;
    } else {
        codepoint = 0xFFFD;
        return 1;
    }

    // Truncated sequence: emit U+FFFD and consume all remaining bytes.
    if (len < seqLen) {
        codepoint = 0xFFFD;
        return len;
    }

    // Validate continuation bytes.
    for (int i = 1; i < seqLen; ++i) {
        if ((p[i] & 0xC0) != 0x80) {
            codepoint = 0xFFFD;
            return 1;
        }
    }

    unsigned int cp = 0;
    switch (seqLen) {
    case 2:
        cp = ((b0 & 0x1F) << 6) | (p[1] & 0x3F);
        break;
    case 3:
        cp = ((b0 & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
        break;
    case 4:
        cp = ((b0 & 0x07) << 18) | ((p[1] & 0x3F) << 12)
           | ((p[2] & 0x3F) << 6) | (p[3] & 0x3F);
        break;
    }

    // Reject overlong encodings.
    if (cp < minCp) {
        codepoint = 0xFFFD;
        return 1;
    }
    // Reject UTF-16 surrogate halves.
    if (cp >= 0xD800 && cp <= 0xDFFF) {
        codepoint = 0xFFFD;
        return 1;
    }
    // Reject out-of-range (b0 <= 0xF4 already caps this, but double-check).
    if (cp > 0x10FFFF) {
        codepoint = 0xFFFD;
        return 1;
    }

    codepoint = cp;
    return seqLen;
}

} // namespace ATHC::EE
