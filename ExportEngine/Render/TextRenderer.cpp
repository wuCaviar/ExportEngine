#include "TextRenderer.h"
#include "Log.h"

#include <cairo.h>
#include <pango/pango.h>
#include <pango/pangocairo.h>
#include <fontconfig/fontconfig.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <unordered_map>

using namespace ATHC::EE;

namespace {

// Fontconfig-based family-name resolver. Pango's default font map on
// Windows is the GDI (win32) backend, which only recognizes family names
// in the system UI language — on an English Windows, "宋体" fails to
// resolve (SimSun is the name GDI exposes). fontconfig reads every
// language variant from the font's name table, so querying it for the
// ASCII (English) alias of a localized family name gives the win32
// backend a name it can actually match. Results are cached; called with
// g_textMutex held (draw()).
std::string resolveFamilyName(const std::string &family)
{
    static std::unordered_map<std::string, std::string> s_cache;
    auto                                                it = s_cache.find(family);
    if (it != s_cache.end())
        return it->second;

    std::string resolved = family;
    FcPattern  *pat      = FcNameParse(reinterpret_cast<const FcChar8 *>(family.c_str()));
    if (pat) {
        FcConfigSubstitute(nullptr, pat, FcMatchPattern);
        FcDefaultSubstitute(pat);
        FcResult   res = FcResultNoMatch;
        FcPattern *m   = FcFontMatch(nullptr, pat, &res);
        if (m) {
            FcChar8 *val = nullptr;
            for (int i = 0; FcPatternGetString(m, FC_FAMILY, i, &val) == FcResultMatch; ++i) {
                const char          *s     = reinterpret_cast<const char *>(val);
                const unsigned char *p     = reinterpret_cast<const unsigned char *>(s);
                bool                 ascii = true;
                for (; *p; ++p) {
                    if (*p > 0x7F) {
                        ascii = false;
                        break;
                    }
                }
                if (ascii) {
                    resolved = s;
                    break;
                }
            }
            FcPatternDestroy(m);
        }
        FcPatternDestroy(pat);
    }
    s_cache.emplace(family, resolved);
    return resolved;
}

// Serializes ALL text operations across worker threads. The default
// pangocairo font map is process-global shared state, and Pango font
// loading behind it is not re-entrant. Text rendering is cheap relative
// to pixel work, so a single process-wide lock is fine.
static std::mutex g_textMutex;

// Fontconfig-based check: does this family have a real italic face?
// (i.e. a distinct font file from the regular face). The win32/GDI backend
// synthesizes a weak ~6° slant for missing italics but still reports the
// font style as italic, so Pango cannot tell real from synthetic — but
// fontconfig resolves the actual font files. Cached; called with
// g_textMutex held (draw()).
bool familyHasItalicFace(const std::string &family)
{
    static std::unordered_map<std::string, bool> s_cache;
    auto                                         it = s_cache.find(family);
    if (it != s_cache.end())
        return it->second;

    auto fileFor = [](const std::string &pat) {
        FcPattern *p = FcNameParse(reinterpret_cast<const FcChar8 *>(pat.c_str()));
        if (!p)
            return std::string();
        FcConfigSubstitute(nullptr, p, FcMatchPattern);
        FcDefaultSubstitute(p);
        FcResult    res = FcResultNoMatch;
        FcPattern  *m   = FcFontMatch(nullptr, p, &res);
        std::string file;
        if (m) {
            FcChar8 *v = nullptr;
            if (FcPatternGetString(m, FC_FILE, 0, &v) == FcResultMatch)
                file = reinterpret_cast<const char *>(v);
            FcPatternDestroy(m);
        }
        FcPatternDestroy(p);
        return file;
    };

    // slant=110 is italic (fontconfig constant); SimSun and other CJK
    // faces have no italic file, so the match falls back to the regular
    // file and the two paths are equal.
    std::string reg = fileFor(family);
    std::string itl = fileFor(family + ":slant=110");
    bool        has = !reg.empty() && !itl.empty() && reg != itl;
    s_cache.emplace(family, has);
    return has;
}

// Fallback rectangle when layout/rasterization cannot proceed
// (surface allocation failure, oversized buffer, ...).
static void drawFallback(RenderContext &ctx,
                         const Text    &text,
                         double         pixelSize,
                         uint8_t        tc1,
                         uint8_t        tc2,
                         uint8_t        tc3,
                         uint8_t        tc4,
                         uint8_t        ta)
{
    double textX = text.x;
    double textY = text.y;
    if (ta == 0)
        return;

    // Line count for the estimate: one line per explicit newline.
    size_t lineCount =
        1 + static_cast<size_t>(std::count(text.content.begin(), text.content.end(), '\n'));

    int    clipX0         = (ctx.tileW > 0) ? ctx.tileX : 0;
    int    clipY0         = (ctx.tileW > 0) ? ctx.tileY : 0;
    int    clipX1         = (ctx.tileW > 0) ? ctx.tileX + ctx.tileW : ctx.canvasWidth;
    int    clipY1         = (ctx.tileW > 0) ? ctx.tileY + ctx.tileH : ctx.canvasHeight;
    double fallbackWidth  = pixelSize * 0.6 * static_cast<double>(text.content.length());
    double fallbackHeight = pixelSize * static_cast<double>(lineCount);
    int    startX         = std::max(clipX0, static_cast<int>(textX));
    int    endX           = std::min(clipX1, static_cast<int>(textX + fallbackWidth));
    int    startY         = std::max(clipY0, static_cast<int>(textY));
    int    endY           = std::min(clipY1, static_cast<int>(textY + fallbackHeight));

    for (int py = startY; py < endY; ++py) {
        for (int px = startX; px < endX; ++px) {
            ctx.paintPixel(px, py, tc1, tc2, tc3, tc4, ta / 2);
        }
    }
}

// Helper to free a Cairo A8 surface + context and read back the buffer
struct A8Render
{
    cairo_surface_t *surf = nullptr;
    cairo_t         *cr   = nullptr;
    int              w = 0, h = 0;
    unsigned char   *data   = nullptr;
    int              stride = 0;

    bool create(int width, int height)
    {
        w = width;
        h = height;
        if (w <= 0 || h <= 0 || w > 32768 || h > 32768)
            return false;
        surf = cairo_image_surface_create(CAIRO_FORMAT_A8, w, h);
        if (cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
            destroy();
            return false;
        }
        cr = cairo_create(surf);
        return true;
    }

    void flush()
    {
        if (surf) {
            cairo_surface_flush(surf);
            data   = cairo_image_surface_get_data(surf);
            stride = cairo_image_surface_get_stride(surf);
        }
    }

    void destroy()
    {
        if (cr) {
            cairo_destroy(cr);
            cr = nullptr;
        }
        if (surf) {
            cairo_surface_destroy(surf);
            surf = nullptr;
        }
        data = nullptr;
        w = h = stride = 0;
    }
};

// RAII guard for PangoLayout + PangoContext pair.
struct PangoLayoutGuard
{
    PangoLayout  *layout = nullptr;
    PangoContext *ctx    = nullptr;
    ~PangoLayoutGuard()
    {
        if (layout)
            g_object_unref(layout);
        if (ctx)
            g_object_unref(ctx);
    }
};

} // anonymous namespace

void TextRenderer::draw(RenderContext &ctx, const Text &text, const Dpi &dpi)
{
    if (text.content.empty())
        return;
    if (text.textColor.alpha <= 0.0)
        return;

    std::lock_guard<std::mutex> lock(g_textMutex);

    // Device-pixel font size — used only for padding estimates / fallback rect.
    double pixelSize = text.fontSize * static_cast<double>(dpi.x) / 72.0;

    uint8_t tc1 = 0, tc2 = 0, tc3 = 0, tc4 = 0, ta = 0;
    ctx.prepareColor(text.textColor, tc1, tc2, tc3, tc4, ta);

    // ── Pango reference layout (96 DPI): shaping, \n line breaks, font
    // fallback ────────────────────────────────────────────────────────
    // Metrics are measured once on a fixed 96 DPI basis so the bounding
    // rect matches the 96 DPI design metrics; the canvas DPI (or an
    // explicit width/height box) is applied as an overall scale at
    // raster time.
    // The default win32/GDI font map only sees family names in the system
    // UI language — resolve localized aliases (宋体 → SimSun, …) through
    // fontconfig first so they render regardless of OS language.
    std::string familyName = resolveFamilyName(text.fontFamily);

    PangoFontMap *fontMap = pango_cairo_font_map_get_default();
    if (!fontMap) {
        drawFallback(ctx, text, pixelSize, tc1, tc2, tc3, tc4, ta);
        return;
    }

    PangoLayoutGuard pg;
    pg.ctx    = pango_cairo_font_map_create_context(PANGO_CAIRO_FONT_MAP(fontMap));
    pg.layout = pango_layout_new(pg.ctx);
    if (!pg.ctx || !pg.layout) {
        drawFallback(ctx, text, pixelSize, tc1, tc2, tc3, tc4, ta);
        return;
    }

    // No wrapping: layout width stays -1, only explicit \n breaks lines.

    // Build the layout for a given font style (desc/underline/text stay the
    // same — only the style field changes between the two passes below).
    auto buildLayout = [&](PangoStyle style) {
        PangoFontDescription *desc = pango_font_description_new();
        pango_font_description_set_family(desc, familyName.c_str());
        // Absolute size = pixels on the 96 DPI reference basis (pt × 96/72) —
        // bypasses Pango's own DPI handling; target scale comes later.
        pango_font_description_set_absolute_size(desc, text.fontSize * (96.0 / 72.0) * PANGO_SCALE);
        pango_font_description_set_weight(desc,
                                          text.bold ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL);
        pango_font_description_set_style(desc, style);
        pango_layout_set_font_description(pg.layout, desc);
        pango_font_description_free(desc);

        if (text.underline) {
            PangoAttrList  *attrs = pango_attr_list_new();
            PangoAttribute *ua    = pango_attr_underline_new(PANGO_UNDERLINE_SINGLE);
            ua->start_index       = 0;
            ua->end_index         = static_cast<guint>(text.content.size());
            pango_attr_list_insert(attrs, ua);
            pango_layout_set_attributes(pg.layout, attrs);
            pango_attr_list_unref(attrs);
        }

        pango_layout_set_text(pg.layout, text.content.c_str(), -1);
    };

    // ── Italic handling ────────────────────────────────────────────────
    // When the resolved font has no real italic face (SimSun, ...), the
    // win32/GDI backend silently substitutes a weak ~6° synthetic slant —
    // visibly shallower than the standard oblique (~12°). Detect the
    // missing face via fontconfig and re-layout as upright, then apply a
    // proper shear at raster time.
    constexpr double kObliqueShear = 0.2; // ≈11.3°, matches cairo-ft synthesize
    bool             needShear     = false;
    if (text.italic) {
        if (familyHasItalicFace(familyName)) {
            buildLayout(PANGO_STYLE_ITALIC);
        } else {
            buildLayout(PANGO_STYLE_NORMAL); // drop the weak GDI slant
            needShear = true;
        }
    } else {
        buildLayout(PANGO_STYLE_NORMAL);
    }

    // ── Measure the 96 DPI reference box (logical) and ink extents ────
    int refW = 0, refH = 0;
    pango_layout_get_pixel_size(pg.layout, &refW, &refH);
    PangoRectangle ink;
    pango_layout_get_pixel_extents(pg.layout, &ink, nullptr);

    if (refW <= 0 || refH <= 0 || ink.width <= 0 || ink.height <= 0) {
        return; // whitespace-only text — no visible ink
    }

    // ── Overall scale from the 96 DPI reference to the target size ────
    // No box: canvas DPI relative to the reference (keeps the pt physical
    // size; x/y independent for anisotropic DPI). With a box: the logical
    // box (line advance box, stable across glyphs) is mapped onto
    // width/height — both given fills the box (X and Y independent, may
    // distort when the aspect differs), only one given scales uniformly
    // by that dimension so the other axis follows.
    double sx = static_cast<double>(dpi.x) / 96.0;
    double sy = static_cast<double>(dpi.y) / 96.0;
    if (text.width > 0.0 && text.height > 0.0) {
        sx = text.width / static_cast<double>(refW);
        sy = text.height / static_cast<double>(refH);
    } else if (text.width > 0.0) {
        sx = sy = text.width / static_cast<double>(refW);
    } else if (text.height > 0.0) {
        sx = sy = text.height / static_cast<double>(refH);
    }

    int pad = static_cast<int>(std::ceil(pixelSize * 0.35));

    // Synthetic oblique shear extends the ink horizontally (the top shifts
    // right, the bottom left by up to shear·inkH/2). Reserve that amount on
    // both sides so the sheared ink never reaches the buffer edge.
    int shearPad =
        needShear ? static_cast<int>(std::ceil(kObliqueShear * ink.height * sx / 2.0)) : 0;

    int bufW =
        static_cast<int>(std::ceil(static_cast<double>(ink.width) * sx)) + pad * 2 + shearPad * 2;
    int bufH = static_cast<int>(std::ceil(static_cast<double>(ink.height) * sy)) + pad * 2;
    if (bufW <= 0 || bufH <= 0) {
        drawFallback(ctx, text, pixelSize, tc1, tc2, tc3, tc4, ta);
        return;
    }
    bufW = std::min(bufW, 32768);
    bufH = std::min(bufH, 32768);

    // ── Rasterize into A8 coverage buffer at target scale ─────────────
    A8Render a8;
    if (!a8.create(bufW, bufH)) {
        drawFallback(ctx, text, pixelSize, tc1, tc2, tc3, tc4, ta);
        return;
    }

    // Clear to transparent (A8 surface memory is uninitialised after create)
    cairo_set_operator(a8.cr, CAIRO_OPERATOR_CLEAR);
    cairo_paint(a8.cr);
    cairo_set_operator(a8.cr, CAIRO_OPERATOR_OVER);

    // White-on-transparent source — glyph shapes = full coverage (255) in A8
    cairo_set_source_rgba(a8.cr, 1.0, 1.0, 1.0, 1.0);

    // Place the ink's top-left at buffer (pad + shearPad, pad): the
    // layout's logical origin sits at (ink.x, ink.y) relative to the ink
    // box, so after the scale transform it must land at
    // (pad + shearPad - ink.x*sx, pad - ink.y*sy).
    cairo_translate(a8.cr, static_cast<double>(pad) + shearPad - static_cast<double>(ink.x) * sx,
                    static_cast<double>(pad) - static_cast<double>(ink.y) * sy);
    cairo_scale(a8.cr, sx, sy);

    // Synthetic oblique: shear around the ink's vertical center so the top
    // leans right and the bottom leans left (x' = x + shear·(cy − y)).
    if (needShear) {
        double         cy = static_cast<double>(ink.y) + ink.height / 2.0;
        cairo_matrix_t m;
        cairo_matrix_init(&m, 1.0, 0.0, -kObliqueShear, 1.0, kObliqueShear * cy, 0.0);
        cairo_transform(a8.cr, &m);
    }
    pango_cairo_show_layout(a8.cr, pg.layout);

    a8.flush();

    // ── Find actual glyph bounds ──────────────────────────────────────
    int actualLeft = a8.w, actualRight = 0;
    int actualTop = a8.h, actualBottom = 0;
    for (int by = 0; by < a8.h; ++by) {
        const unsigned char *row = a8.data + by * a8.stride;
        for (int bx = 0; bx < a8.w; ++bx) {
            if (row[bx] > 0) {
                if (bx < actualLeft)
                    actualLeft = bx;
                if (bx > actualRight)
                    actualRight = bx;
                if (by < actualTop)
                    actualTop = by;
                if (by > actualBottom)
                    actualBottom = by;
            }
        }
    }

    if (actualTop >= a8.h) {
        a8.destroy();
        return;
    }

    // ── Composite alpha buffer to output ──────────────────────────────
    // Ink top-left lands at (text.x, text.y), ink extends downward/right.
    int clipX0 = (ctx.tileW > 0) ? ctx.tileX : 0;
    int clipY0 = (ctx.tileW > 0) ? ctx.tileY : 0;
    int clipX1 = (ctx.tileW > 0) ? ctx.tileX + ctx.tileW : ctx.canvasWidth;
    int clipY1 = (ctx.tileW > 0) ? ctx.tileY + ctx.tileH : ctx.canvasHeight;

    int offX = static_cast<int>(text.x - actualLeft);
    int offY = static_cast<int>(text.y - actualTop);

    int outX0 = std::max(clipX0, offX);
    int outY0 = std::max(clipY0, offY);
    int outX1 = std::min(clipX1, offX + a8.w);
    int outY1 = std::min(clipY1, offY + a8.h);

    for (int py = outY0; py < outY1; ++py) {
        int                  by  = py - offY;
        const unsigned char *row = a8.data + by * a8.stride;
        for (int px = outX0; px < outX1; ++px) {
            uint8_t cov = row[px - offX];
            if (cov > 0) {
                uint8_t effA = static_cast<uint8_t>((cov * ta + 127) / 255);
                if (effA > 0)
                    ctx.paintPixel(px, py, tc1, tc2, tc3, tc4, effA);
            }
        }
    }

    a8.destroy();
}
