/*
 * app_gauge.h - the gauge on the desktop: one gauge_render_t, driven.
 *
 * A self-test sweep on start, an engine simulator script, a manual value and
 * a frame-rate read-out drawn on the dial.  The layout sizes the dial to the
 * 480x800 panel, and whole frames are pushed because this unit's firmware
 * ignores partial writes (see docs/usb-screen.md).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "gauge_presets.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Creates the default gauge.  False when out of memory. */
bool app_gauge_start(void);

void app_gauge_select(const gauge_preset_t *preset);
void app_gauge_next(void);
const gauge_preset_t *app_gauge_current(void);

void app_gauge_set_demo(bool on);
bool app_gauge_is_demo(void);
void app_gauge_set_value(float value);

/* Jumps the needle there without waiting for the slew (offline renders). */
void app_gauge_set_immediate(float value);
void app_gauge_sweep(void);

void app_gauge_show_stats(bool on);
bool app_gauge_stats_shown(void);
float app_gauge_fps(void);
void app_gauge_timing(uint32_t *render_us, uint32_t *flush_us);

/* Draws and pushes one frame.  dt is the measured frame interval, seconds. */
void app_gauge_frame(float dt);

/* Stops the gauge touching the framebuffer; the test screens take over. */
void app_gauge_set_visible(bool on);
bool app_gauge_visible(void);

/* Draws the current value into the framebuffer without touching the panel. */
void app_gauge_draw_now(void);

/* The rectangle the dial lives in; every frame after the first pushes only
 * this.  Valid once a gauge exists. */
void app_gauge_rect(int *x0, int *y0, int *x1, int *y1);

#ifdef __cplusplus
}
#endif
