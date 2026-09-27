/*
 * gfx_text.c - draws generated alpha-mask glyphs into the framebuffer.
 *
 * Coverage is used as a blend factor, so the edges of the generated type stay
 * smooth instead of turning into stair-steps.  A scale factor replicates each
 * mask pixel into a square block, which is how the same fonts serve a panel
 * twice the size they were rasterised for.
 */
#include "gfx_text.h"

#include <string.h>

#include "gfx_font_data.h"

static const gfx_glyph_t *glyph_for(const gfx_font_t *font, char c)
{
    const unsigned idx = (unsigned char)c - font->first;
    if (idx >= font->count) {
        return NULL;
    }
    return &font->glyphs[idx];
}

static int normalise_scale(int scale)
{
    return scale < 1 ? 1 : scale;
}

int gfx_text_width_scaled(const char *text, const gfx_font_t *font, int scale)
{
    if (!text || !font) {
        return 0;
    }
    scale = normalise_scale(scale);

    int pen = 0;
    for (const char *p = text; *p; p++) {
        const gfx_glyph_t *g = glyph_for(font, *p);
        pen += (g ? g->advance : font->line_height / 3) * scale;
    }
    return pen;
}

int gfx_text_width(const char *text, const gfx_font_t *font)
{
    return gfx_text_width_scaled(text, font, 1);
}

void gfx_text_scaled(int x, int baseline_y, const char *text, const gfx_font_t *font,
                     uint16_t colour, int scale)
{
    if (!text || !font) {
        return;
    }
    scale = normalise_scale(scale);

    int pen = x;

    for (const char *p = text; *p; p++) {
        const gfx_glyph_t *g = glyph_for(font, *p);
        if (!g) {
            pen += (font->line_height / 3) * scale;
            continue;
        }
        if (g->w == 0 || g->h == 0) {
            pen += g->advance * scale;
            continue;
        }

        const int gx = pen + g->bearing_x * scale;
        const int gy = baseline_y + g->bearing_y * scale;
        const uint8_t *src = &font->alpha[g->offset];

        for (int row = 0; row < g->h; row++) {
            for (int sy = 0; sy < scale; sy++) {
                const int py = gy + row * scale + sy;
                if ((unsigned)py >= GFX_H) {
                    continue;
                }
                uint16_t *dst = &gfx_framebuffer()[py * GFX_W];
                for (int col = 0; col < g->w; col++) {
                    const uint8_t a = src[col];
                    if (a == 0) {
                        continue;
                    }
                    for (int sx = 0; sx < scale; sx++) {
                        const int px = gx + col * scale + sx;
                        if ((unsigned)px >= GFX_W) {
                            continue;
                        }
                        dst[px] = (a >= 250) ? colour : gfx_blend(dst[px], colour, a);
                    }
                }
            }
            src += g->w;
        }

        pen += g->advance * scale;
    }
}

void gfx_text(int x, int baseline_y, const char *text, const gfx_font_t *font, uint16_t colour)
{
    gfx_text_scaled(x, baseline_y, text, font, colour, 1);
}

void gfx_text_centered(int cx, int baseline_y, const char *text, const gfx_font_t *font,
                       uint16_t colour)
{
    gfx_text(cx - gfx_text_width(text, font) / 2, baseline_y, text, font, colour);
}

void gfx_text_right(int x_right, int baseline_y, const char *text, const gfx_font_t *font,
                    uint16_t colour)
{
    gfx_text(x_right - gfx_text_width(text, font), baseline_y, text, font, colour);
}

void gfx_text_cap_centered_scaled(int cx, int cy, const char *text, const gfx_font_t *font,
                                  uint16_t colour, int scale)
{
    if (!font) {
        return;
    }
    scale = normalise_scale(scale);
    /* put the middle of the cap band on cy: baseline = cy + cap/2 */
    const int baseline = cy + (font->cap_height * scale) / 2;
    gfx_text_scaled(cx - gfx_text_width_scaled(text, font, scale) / 2, baseline,
                    text, font, colour, scale);
}

void gfx_text_cap_centered(int cx, int cy, const char *text, const gfx_font_t *font,
                           uint16_t colour)
{
    gfx_text_cap_centered_scaled(cx, cy, text, font, colour, 1);
}
