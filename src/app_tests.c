/*
 * app_tests.c - bring-up test screens, drawn straight into the framebuffer.
 *
 * One screen per failure mode: a solid white `fill` separates a panel problem
 * from a drawing problem, `bars` shows colour order, `grid` shows missing
 * regions, `circle` shows geometry and `quad` shows orientation.
 */
#include "app_tests.h"

#include <string.h>

#include "gfx.h"
#include "app_gauge.h"

static const char *const k_tests[] = {"fill", "bars", "grid", "circle", "quad"};
#define TEST_COUNT (sizeof(k_tests) / sizeof(k_tests[0]))
static int s_current;

/* Every pixel white.  Anything dark here is the panel, not the drawing. */
static void test_fill(void)
{
    gfx_clear(gfx_rgb(255, 255, 255));
}

/* Colour bars in a known order, so the byte order and inversion are obvious. */
static void test_bars(void)
{
    static const uint32_t cols[] = {
        0xFF0000, 0x00FF00, 0x0000FF, 0xFFFFFF,
        0xFFFF00, 0x00FFFF, 0xFF00FF, 0x000000,
    };
    const int n = (int)(sizeof(cols) / sizeof(cols[0]));
    const int w = GFX_W / n;
    for (int i = 0; i < n; i++) {
        gfx_fill_rect(i * w, 0, (i + 1) * w - 1, GFX_H - 1, gfx_hex(cols[i]));
    }
    gfx_fill_rect(0, 0, GFX_W - 1, 3, gfx_rgb(255, 255, 255));
    gfx_fill_rect(0, GFX_H - 4, GFX_W - 1, GFX_H - 1, gfx_rgb(0, 0, 0));
}

/* A 1 px grid every 16 px; missing lines show up as gaps in a pattern. */
static void test_grid(void)
{
    gfx_clear(gfx_rgb(0, 0, 0));
    for (int x = 0; x < GFX_W; x += 16) {
        gfx_vline(0, GFX_H - 1, x, gfx_rgb(0, 90, 0));
    }
    for (int y = 0; y < GFX_H; y += 16) {
        gfx_hline(0, GFX_W - 1, y, gfx_rgb(0, 90, 0));
    }
    for (int x = 0; x < GFX_W; x += 64) {
        gfx_vline(0, GFX_H - 1, x, gfx_rgb(0, 255, 0));
    }
    for (int y = 0; y < GFX_H; y += 64) {
        gfx_hline(0, GFX_W - 1, y, gfx_rgb(0, 255, 0));
    }
    gfx_rect(0, 0, GFX_W - 1, GFX_H - 1, gfx_rgb(255, 255, 255));
}

/* Concentric circles and a crosshair: geometry, and the round bezel. */
static void test_circle(void)
{
    const int cx = GFX_W / 2;
    const int cy = GFX_H / 2;
    const int r = (GFX_W < GFX_H ? GFX_W : GFX_H) / 2;

    gfx_clear(gfx_rgb(0, 0, 0));
    gfx_circle(cx, cy, r - 1, gfx_rgb(255, 255, 255));
    gfx_circle(cx, cy, r / 2, gfx_rgb(0, 200, 60));
    gfx_circle(cx, cy, r / 4, gfx_rgb(0, 120, 40));
    gfx_hline(0, GFX_W - 1, cy, gfx_rgb(60, 60, 60));
    gfx_vline(0, GFX_H - 1, cx, gfx_rgb(60, 60, 60));
    gfx_disc(cx, cy, 8, gfx_rgb(255, 60, 0));
    /* four quadrant ticks, so rotation is unambiguous */
    gfx_fill_rect(cx - 3, 0, cx + 3, 30, gfx_rgb(255, 0, 0));
    gfx_fill_rect(GFX_W - 31, cy - 3, GFX_W - 1, cy + 3, gfx_rgb(0, 255, 0));
    gfx_fill_rect(cx - 3, GFX_H - 31, cx + 3, GFX_H - 1, gfx_rgb(0, 0, 255));
    gfx_fill_rect(0, cy - 3, 30, cy + 3, gfx_rgb(255, 255, 0));
}

/* Four flat quadrants: the quickest way to see which region is not driven. */
static void test_quad(void)
{
    const int cx = GFX_W / 2;
    const int cy = GFX_H / 2;
    gfx_fill_rect(0, 0, cx - 1, cy - 1, gfx_rgb(255, 0, 0));
    gfx_fill_rect(cx, 0, GFX_W - 1, cy - 1, gfx_rgb(0, 255, 0));
    gfx_fill_rect(0, cy, cx - 1, GFX_H - 1, gfx_rgb(0, 0, 255));
    gfx_fill_rect(cx, cy, GFX_W - 1, GFX_H - 1, gfx_rgb(255, 255, 0));
    gfx_rect(0, 0, GFX_W - 1, GFX_H - 1, gfx_rgb(255, 255, 255));
}

/* -------------------------------------------------------------------------- */

void app_tests_show(const char *name)
{
    /* a test screen owns the display; stop the gauge drawing over it */
    app_gauge_set_visible(false);

    if (!name || !*name) {
        name = k_tests[s_current];
    } else {
        for (int i = 0; i < (int)TEST_COUNT; i++) {
            if (strcmp(name, k_tests[i]) == 0) {
                s_current = i;
                break;
            }
        }
    }

    const char *which = k_tests[s_current];
    if (strcmp(which, "fill") == 0) {
        test_fill();
    } else if (strcmp(which, "bars") == 0) {
        test_bars();
    } else if (strcmp(which, "grid") == 0) {
        test_grid();
    } else if (strcmp(which, "circle") == 0) {
        test_circle();
    } else {
        test_quad();
    }

    /* these are whole-screen, and the previous frame may have been partial */
    gfx_flush();
}

void app_tests_next(void)
{
    s_current = (s_current + 1) % (int)TEST_COUNT;
    app_tests_show(NULL);
}

const char *app_tests_current(void)
{
    return k_tests[s_current];
}
