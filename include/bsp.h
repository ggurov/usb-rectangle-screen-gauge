/*
 * bsp.h - what the graphics layer asks of the panel, on the desktop.
 *
 * gfx.c talks to the panel through exactly one function, and this header is
 * that contract: the primitives draw into the framebuffer, and this pushes
 * rectangles of it to the VoCore USB screen.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Brings up the screen.  `force_width`/`force_height` override the geometry
 * the registers report (0,0 = trust the panel).  Returns false with a reason
 * in `err` rather than aborting: a dead screen must not cost you the console.
 */
bool bsp_display_init(int force_width, int force_height, char *err, int errlen);
void bsp_display_shutdown(void);

/* Pushes a packed RGB565 rectangle; x1/y1 are exclusive.  Blocking. */
int bsp_lcd_draw_bitmap(int x0, int y0, int x1, int y1, const uint16_t *pixels);

/* Completed transfers and bytes, for `flush`. */
uint32_t bsp_lcd_flush_count(void);
void bsp_lcd_flush_stats(uint32_t *frames, uint32_t *bytes, uint32_t *errors);

/* 0..100, returns false when the panel did not acknowledge. */
bool bsp_backlight_set(int percent);
int  bsp_backlight_get(void);

/* Flip/mirror 0..3 on the panels that support it. */
bool bsp_display_flip(int mode);

/* Whole frames or partial rectangles; the fast path is on by default. */
void bsp_display_set_partial(bool on);
bool bsp_display_partial(void);

/*
 * The panel's column origin in pixels.  This 4-inch glass shows framebuffer
 * column 0 in the middle, so the app pre-rolls every frame by half its width
 * (--xshift 240); 0 is the honest default for panels that do not need it.
 */
void bsp_display_set_xshift(int pixels);
int  bsp_display_xshift(void);

/* Panel model string, width and height, as brought up. */
const char *bsp_display_model(void);
int bsp_display_width(void);
int bsp_display_height(void);
const char *bsp_display_transport(void);

/* -------------------------------------------------------------------------- */
/* touch                                                                      */
/* -------------------------------------------------------------------------- */

/* Polls the interrupt endpoint; true when a report with a point arrived. */
bool bsp_touch_poll(void);

/* Last decoded report, in panel pixels. */
void bsp_touch_last(int *x, int *y, bool *pressed);
uint32_t bsp_touch_tap_count(void);

#ifdef __cplusplus
}
#endif
