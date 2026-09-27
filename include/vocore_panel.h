/*
 * vocore_panel.h - one VoCore USB2.0 Screen, brought up and drawn to.
 *
 * It owns the USB device, learns the panel geometry, wakes it, pushes frames
 * and polls the touch endpoint.  It knows nothing about the gauge.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "vocore_proto.h"
#include "vocore_transport.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct vocore_panel vocore_panel_t;

/*
 * "kind" is a short transport name for logs ("libusb", "fake").  The panel
 * does not take ownership of `transport`, but keeps the pointer.
 */
vocore_panel_t *vocore_panel_open(const vocore_transport_t *transport, const char *kind);
void vocore_panel_close(vocore_panel_t *panel);

/*
 * Reads the identification registers, applies the caller's size override if
 * one is given (0 = use whatever the registers say), sends sleep-out and
 * applies the default brightness.  Returns false when the screen does not
 * answer; `err` receives a short reason.
 */
bool vocore_panel_bring_up(vocore_panel_t *panel, int force_width, int force_height,
                           char *err, int errlen);

/* Geometry the panel was brought up with. */
const vocore_id_t *vocore_panel_id(const vocore_panel_t *panel);
int vocore_panel_width(const vocore_panel_t *panel);
int vocore_panel_height(const vocore_panel_t *panel);

/* 0..100. */
bool vocore_panel_set_brightness(vocore_panel_t *panel, int percent);
int  vocore_panel_brightness(const vocore_panel_t *panel);

/* Flip/mirror 0..3, panel dependent. */
bool vocore_panel_set_flip(vocore_panel_t *panel, int mode);

/*
 * The panel's column origin, in pixels.  0 is the obvious value, but the
 * 4-inch panel this project was ported to shows column 0 in the middle of the
 * glass: it rolls every frame by exactly half its width.  Setting `xshift`
 * pre-rolls the frames instead, which is what makes the picture land straight.
 * See docs/usb-screen.md for how it was measured.
 */
void vocore_panel_set_xshift(vocore_panel_t *panel, int pixels);
int  vocore_panel_xshift(const vocore_panel_t *panel);

/*
 * Pushes the whole framebuffer.  `pixels` is RGB565, row major, little
 * endian, exactly width*height entries.
 */
bool vocore_panel_show(vocore_panel_t *panel, const uint16_t *pixels);

/*
 * Pushes one rectangle of RGB565 pixels, x1/y1 exclusive.  `pixels` is
 * tightly packed: (x1-x0) entries per row, exactly the buffer gfx hands to
 * bsp_lcd_draw_bitmap().  Falls back to a whole frame when partial writes
 * are disabled or the panel rejects them.
 */
bool vocore_panel_show_rect(vocore_panel_t *panel, const uint16_t *pixels,
                            int x0, int y0, int x1, int y1);

/* Enable/disable the partial-write fast path (default on). */
void vocore_panel_set_partial(vocore_panel_t *panel, bool on);
bool vocore_panel_partial(const vocore_panel_t *panel);

/*
 * Polls the touch interrupt endpoint with the given timeout.  Returns true
 * when a report with at least one point arrived; a timeout is not an error.
 */
bool vocore_panel_poll_touch(vocore_panel_t *panel, vocore_touch_t *touch, int timeout_ms);

/* Counters for the console. */
void vocore_panel_stats(const vocore_panel_t *panel, uint32_t *frames,
                        uint32_t *bytes, uint32_t *errors);

#ifdef __cplusplus
}
#endif
