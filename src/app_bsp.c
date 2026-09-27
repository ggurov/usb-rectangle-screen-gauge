/*
 * app_bsp.c - the panel under gfx, on the desktop.
 *
 * Owns the single vocore_panel_t and exposes it through the narrow bsp_*
 * API that gfx and the app draw through.
 */
#include "bsp.h"

#include <stdio.h>
#include <string.h>

#include "usb_libusb.h"
#include "vocore_panel.h"

static vocore_panel_t *s_panel;
static bool s_touch_down;
static int s_touch_x, s_touch_y;
static uint32_t s_taps;
static bool s_last_down;

bool bsp_display_init(int force_width, int force_height, char *err, int errlen)
{
    if (s_panel) {
        return true;
    }

    const vocore_transport_t *transport = usb_libusb_open(err, errlen);
    if (!transport) {
        return false;
    }

    s_panel = vocore_panel_open(transport, "libusb");
    if (!s_panel) {
        usb_libusb_close();
        if (err && errlen) {
            snprintf(err, errlen, "out of memory");
        }
        return false;
    }

    if (!vocore_panel_bring_up(s_panel, force_width, force_height, err, errlen)) {
        vocore_panel_close(s_panel);
        s_panel = NULL;
        usb_libusb_close();
        return false;
    }

    s_touch_down = false;
    s_last_down = false;
    s_taps = 0;
    return true;
}

void bsp_display_shutdown(void)
{
    if (!s_panel) {
        return;
    }
    vocore_panel_close(s_panel);
    s_panel = NULL;
    usb_libusb_close();
}

int bsp_lcd_draw_bitmap(int x0, int y0, int x1, int y1, const uint16_t *pixels)
{
    if (!s_panel) {
        return -1;
    }
    return vocore_panel_show_rect(s_panel, pixels, x0, y0, x1, y1) ? 0 : -1;
}

uint32_t bsp_lcd_flush_count(void)
{
    uint32_t frames = 0, errors = 0;
    uint64_t bytes = 0;
    vocore_panel_stats(s_panel, &frames, &bytes, &errors);
    return frames;
}

void bsp_lcd_flush_stats(uint32_t *frames, uint64_t *bytes, uint32_t *errors)
{
    vocore_panel_stats(s_panel, frames, bytes, errors);
}

bool bsp_backlight_set(int percent)
{
    return s_panel && vocore_panel_set_brightness(s_panel, percent);
}

int bsp_backlight_get(void)
{
    return vocore_panel_brightness(s_panel);
}

bool bsp_display_flip(int mode)
{
    return s_panel && vocore_panel_set_flip(s_panel, mode);
}

void bsp_display_set_partial(bool on)
{
    vocore_panel_set_partial(s_panel, on);
}

bool bsp_display_partial(void)
{
    return vocore_panel_partial(s_panel);
}

void bsp_display_set_xshift(int pixels)
{
    vocore_panel_set_xshift(s_panel, pixels);
}

int bsp_display_xshift(void)
{
    return vocore_panel_xshift(s_panel);
}

const char *bsp_display_model(void)
{
    const vocore_id_t *id = vocore_panel_id(s_panel);
    return id ? id->model : "no panel";
}

int bsp_display_width(void)
{
    return vocore_panel_width(s_panel);
}

int bsp_display_height(void)
{
    return vocore_panel_height(s_panel);
}

const char *bsp_display_transport(void)
{
    return usb_libusb_dll_path();
}

/* -------------------------------------------------------------------------- */
/* touch                                                                      */
/* -------------------------------------------------------------------------- */

bool bsp_touch_poll(void)
{
    if (!s_panel) {
        return false;
    }
    vocore_touch_t touch;
    if (!vocore_panel_poll_touch(s_panel, &touch, 1) || touch.count < 1) {
        return false;
    }

    const vocore_point_t *p = &touch.point[0];
    /* flag 1 is hover; anything else with a position is a real touch */
    const bool down = (p->flag != 1);
    if (down) {
        s_touch_x = p->x;
        s_touch_y = p->y;
    }
    if (!down && s_last_down) {
        s_taps++;
    }
    s_last_down = down;
    s_touch_down = down;
    return true;
}

void bsp_touch_last(int *x, int *y, bool *pressed)
{
    if (x) *x = s_touch_x;
    if (y) *y = s_touch_y;
    if (pressed) *pressed = s_touch_down;
}

uint32_t bsp_touch_tap_count(void)
{
    return s_taps;
}
