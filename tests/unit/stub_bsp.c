/*
 * stub_bsp.c - the panel the unit tests run against.
 *
 * gfx.c talks to the panel through exactly one function, so replacing it gives
 * the tests a real framebuffer with no USB traffic: `gfx_flush*` records the
 * rectangle it was asked for and nothing else happens.  The rest of the bsp_*
 * surface exists so the same binary links without the real panel layer.
 */
#include "stub_bsp.h"

#include <string.h>

#include "bsp.h"
#include "gfx.h"

static int s_last_x0, s_last_y0, s_last_x1, s_last_y1;
static uint32_t s_calls;
static uint32_t s_fail_after = UINT32_MAX;
static int s_backlight;

int bsp_lcd_draw_bitmap(int x0, int y0, int x1, int y1, const uint16_t *pixels)
{
    (void)pixels;
    s_last_x0 = x0;
    s_last_y0 = y0;
    s_last_x1 = x1;
    s_last_y1 = y1;
    if (s_calls >= s_fail_after) {
        return -1;
    }
    s_calls++;
    return 0;
}

uint32_t bsp_lcd_flush_count(void)
{
    return s_calls;
}

void bsp_lcd_flush_stats(uint32_t *frames, uint64_t *bytes, uint32_t *errors)
{
    if (frames) *frames = s_calls;
    if (bytes)  *bytes = (uint64_t)s_calls * (uint64_t)GFX_W * (uint64_t)GFX_H * 2u;
    if (errors) *errors = 0;
}

bool bsp_display_init(int force_width, int force_height, char *err, int errlen)
{
    (void)force_width; (void)force_height; (void)err; (void)errlen;
    return true;
}

void bsp_display_shutdown(void) {}

bool bsp_backlight_set(int percent) { s_backlight = percent; return true; }
int  bsp_backlight_get(void)        { return s_backlight; }

bool bsp_display_flip(int mode) { (void)mode; return true; }
void bsp_display_set_partial(bool on) { (void)on; }
bool bsp_display_partial(void) { return false; }
void bsp_display_set_xshift(int pixels) { (void)pixels; }
int  bsp_display_xshift(void) { return 0; }

const char *bsp_display_model(void) { return "stub panel"; }
int  bsp_display_width(void)  { return GFX_W; }
int  bsp_display_height(void) { return GFX_H; }
const char *bsp_display_transport(void) { return "stub"; }

bool bsp_touch_poll(void) { return false; }
void bsp_touch_last(int *x, int *y, bool *pressed)
{
    if (x) *x = 0;
    if (y) *y = 0;
    if (pressed) *pressed = false;
}
uint32_t bsp_touch_tap_count(void) { return 0; }

/* --- test hooks ---------------------------------------------------------- */

uint32_t stub_bsp_calls(void) { return s_calls; }

void stub_bsp_reset(void)
{
    s_calls = 0;
    s_fail_after = UINT32_MAX;
    s_last_x0 = s_last_y0 = s_last_x1 = s_last_y1 = -1;
}

void stub_bsp_fail_after(uint32_t n) { s_fail_after = n; }

void stub_bsp_last_rect(int *x0, int *y0, int *x1, int *y1)
{
    if (x0) *x0 = s_last_x0;
    if (y0) *y0 = s_last_y0;
    if (x1) *x1 = s_last_x1;
    if (y1) *y1 = s_last_y1;
}
