#include "TextRenderer.h"
#include "FontEngine.h"
#include "Log.h"

#include <cairo.h>
#include <cairo-ft.h>
#include <cmath>
#include <algorithm>
#include <mutex>
#include <vector>
#include <cstdint>

using namespace ATHC::EE;

namespace {

// ── Fallback rectangle when font engine is unavailable ──────────────
static void drawFallback(RenderContext &ctx, const Text &text, double pixelSize, uint8_t tc1,
    uint8_t tc2, uint8_t tc3, uint8_t tc4, uint8_t ta)
{
    double textX = text.x;
    double textY = text.y + pixelSize;
    if (ta == 0)
        return;

    int clipX0 = (ctx.tileW > 0) ? ctx.tileX : 0;
    int clipY0 = (ctx.tileW > 0) ? ctx.tileY : 0;
    int clipX1 = (ctx.tileW > 0) ? ctx.tileX + ctx.tileW : ctx.canvasWidth;
    int clipY1 = (ctx.tileW > 0) ? ctx.tileY + ctx.tileH : ctx.canvasHeight;
    double fallbackWidth = pixelSize * 0.6 * text.content.length();
    double fallbackHeight = pixelSize;
    int startX = std::max(clipX0, static_cast<int>(textX));
    int endX = std::min(clipX1, static_cast<int>(textX + fallbackWidth));
    int startY = std::max(clipY0, static_cast<int>(textY - fallbackHeight));
    int endY = std::min(clipY1, static_cast<int>(textY));

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
    cairo_t *cr = nullptr;
    int w = 0, h = 0;
    unsigned char *data = nullptr;
    int stride = 0;

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
            data = cairo_image_surface_get_data(surf);
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

// Serializes ALL font operations (font loading, FreeType face usage, Cairo
// glyph rasterization) across worker threads. Concurrent font use is unsafe
// here: each worker thread owns a separate FreeType library, and Cairo's
// global unscaled-font map + "zombie" face state can then end up sharing one
// unscaled font across threads (FT face addresses get reused), leaving a
// scaled font with a corrupted scale — glyph rasterization fails silently
// with CAIRO_STATUS_FREETYPE_ERROR. Text rendering is cheap relative to
// pixel work, so a single process-wide lock is fine.
static std::mutex g_textMutex;

// A run of text that shares one font: either the primary (requested)
// family or a resolved fallback family covering glyphs the primary lacks.
struct TextRun
{
    std::string text; // UTF-8 substring (codepoint-aligned)
    std::string family; // empty = primary font
};

// Split text into runs by per-codepoint glyph coverage. Codepoints the
// primary font lacks and for which no fallback family is installed stay in
// the primary run (current behaviour: glyph renders as .notdef / nothing).
void analyzeTextRuns(const std::string &content, FontEngine *fe, std::vector<TextRun> &runs)
{
    runs.clear();
    std::string cur;
    std::string curFamily; // "" = primary
    auto flush = [&]() {
        if (!cur.empty()) {
            runs.push_back({ cur, curFamily });
            cur.clear();
        }
    };
    const char *s = content.c_str();
    int len = static_cast<int>(content.size());
    int pos = 0;
    while (pos < len) {
        unsigned int cp = 0;
        int bytes = utf8Decode(s + pos, len - pos, cp);
        if (bytes <= 0)
            break;
        std::string family;
        if (!fe->hasGlyph(cp))
            family = fe->resolveFallbackFamily(cp); // "" = none → primary
        if (family != curFamily) {
            flush();
            curFamily = family;
        }
        cur.append(s + pos, static_cast<size_t>(bytes));
        pos += bytes;
    }
    flush();
}

// Estimated advance width (0.6em per codepoint) for a run whose font failed
// to load — keeps subsequent runs positioned consistently.
double estimatedRunWidth(const std::string &text, double pixelSize)
{
    int count = 0;
    int pos = 0;
    int len = static_cast<int>(text.size());
    while (pos < len) {
        unsigned int cp = 0;
        int bytes = utf8Decode(text.c_str() + pos, len - pos, cp);
        if (bytes <= 0)
            break;
        pos += bytes;
        ++count;
    }
    return pixelSize * 0.6 * static_cast<double>(count);
}

} // anonymous namespace

void TextRenderer::draw(
    RenderContext &ctx, const Text &text, FontEngine *fontEngine, const Dpi &dpi)
{
    if (text.content.empty())
        return;
    if (text.textColor.alpha <= 0.0)
        return;

    std::lock_guard<std::mutex> lock(g_textMutex);

    double pixelSize = text.fontSize * static_cast<double>(dpi.x) / 72.0;

    uint8_t tc1 = 0, tc2 = 0, tc3 = 0, tc4 = 0, ta = 0;
    ctx.prepareColor(text.textColor, tc1, tc2, tc3, tc4, ta);

    if (!fontEngine)
        return;

    auto ec = fontEngine->loadFont(text.fontFamily, pixelSize, text.bold, text.italic);
    if (ec) {
        if (isRecoverable(ec)) {
            EELog::warn("Text: font '{}' unavailable ({}), rendering fallback box", text.fontFamily,
                ec.message());
            drawFallback(ctx, text, pixelSize, tc1, tc2, tc3, tc4, ta);
        }
        return;
    }

    // loadFont() sets m_scaledFont on success — verify it's usable
    auto *sf = fontEngine->scaledFont();
    if (!sf)
        return;

    // ── Analyse glyph coverage → per-font runs ────────────────────────
    // Glyphs the primary font lacks are drawn with the first installed
    // fallback family that covers them (FontEngine::resolveFallbackFamily).
    std::vector<TextRun> runs;
    analyzeTextRuns(text.content, fontEngine, runs);

    // ── Measure text ──────────────────────────────────────────────────
    cairo_font_extents_t fontExt;
    cairo_scaled_font_extents(sf, &fontExt);

    // Fallback fonts may have taller ascenders/descenders than the primary —
    // the buffer must fit the tallest, with all runs sharing one baseline.
    double maxAscent = fontExt.ascent;
    double maxDescent = fontExt.descent;

    // The primary font must be current before measuring a primary run — a
    // preceding fallback run may have switched fonts.
    auto ensurePrimary = [&]() {
        fontEngine->loadFont(text.fontFamily, pixelSize, text.bold, text.italic);
    };

    double textWidth = 0.0;
    for (const auto &run : runs) {
        if (run.family.empty()) {
            ensurePrimary();
            textWidth += fontEngine->measureTextWidth(
                run.text, text.fontFamily, pixelSize, text.bold, text.italic);
            continue;
        }
        auto runEc = fontEngine->loadFont(run.family, pixelSize, text.bold, text.italic);
        if (runEc) {
            if (isRecoverable(runEc)) {
                EELog::warn("Text: fallback font '{}' unavailable ({}), estimating width",
                    run.family, runEc.message());
                textWidth += estimatedRunWidth(run.text, pixelSize);
            }
            continue;
        }
        textWidth +=
            fontEngine->measureTextWidth(run.text, run.family, pixelSize, text.bold, text.italic);
        cairo_font_extents_t ext;
        cairo_scaled_font_extents(fontEngine->scaledFont(), &ext);
        maxAscent = std::max(maxAscent, ext.ascent);
        maxDescent = std::max(maxDescent, ext.descent);
    }
    ensurePrimary(); // restore primary for the render pass below
    sf = fontEngine->scaledFont();

    double ascender = maxAscent;
    double descender = maxDescent;
    double textHeight = ascender + descender;

    double padX = std::ceil(pixelSize * 0.35);
    double padY = std::ceil(pixelSize * 0.35);

    int bufW = static_cast<int>(std::ceil(textWidth) + padX * 2);
    int bufH = static_cast<int>(std::ceil(textHeight) + padY * 2);
    if (bufW <= 0 || bufH <= 0) {
        drawFallback(ctx, text, pixelSize, tc1, tc2, tc3, tc4, ta);
        return;
    }
    bufW = std::min(bufW, 32768);
    bufH = std::min(bufH, 32768);

    // ── Render text into A8 surface ───────────────────────────────────
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

    // Draw run by run: primary font, then fallback fonts for glyphs the
    // primary lacks. All runs share one baseline at (padY + maxAscent).
    double cursorX = padX;
    for (const auto &run : runs) {
        if (run.family.empty()) {
            ensurePrimary();
        } else if (auto runEc =
                       fontEngine->loadFont(run.family, pixelSize, text.bold, text.italic)) {
            if (isRecoverable(runEc))
                cursorX += estimatedRunWidth(run.text, pixelSize);
            // warn 已在测量循环记录一次，此处不重复
            continue;
        }
        cairo_save(a8.cr);
        cairo_set_scaled_font(a8.cr, fontEngine->scaledFont());
        cairo_move_to(a8.cr, cursorX, padY + maxAscent);
        cairo_show_text(a8.cr, run.text.c_str());
        cairo_restore(a8.cr);
        cursorX += fontEngine->measureTextWidth(run.text,
            run.family.empty() ? text.fontFamily : run.family, pixelSize, text.bold, text.italic);
    }

    a8.flush();

    // Restore the primary font — the run loop above may have left a
    // fallback font current, invalidating sf.
    ensurePrimary();
    sf = fontEngine->scaledFont();

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
        int by = py - offY;
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
