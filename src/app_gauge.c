/*
 * app_gauge.c - drives the gauge and pushes frames to the USB screen.
 *
 * The value follows a demo script with a little smoothed noise, a self-test
 * sweep runs on start and on every gauge change, and the loop measures its
 * own frame interval so the read-out shows what the panel actually got.
 *
 * The layout differences live here:
 *
 *   - the dial is min(GFX_W, GFX_H) minus a margin, centred;
 *   - the theme's pixel lengths are doubled (geometry_scale 2) and the
 *     generated fonts are drawn at scale 2, so the 240 px design keeps its
 *     proportions on a 480 px dial;
 *   - after the first full frame only the dial's bounding square is pushed
 *     (about 430 KB instead of 768 KB, which is the difference between 27 and
 *     44 fps on the wire).
 */
#include "app_gauge.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bsp.h"
#include "gauge_render.h"
#include "gfx.h"
#include "app_time.h"

#define MIN_FRAME_MS 2

#define SELFTEST_TOP_MS 1150
#define SELFTEST_END_MS 1750

/* margin around the dial, in framebuffer pixels */
#define DIAL_MARGIN 16

typedef struct {
    uint16_t ms;
    float    frac;
} demo_step_t;

static const demo_step_t k_script[] = {
    { 1400, 0.11f }, {  600, 0.40f }, {  900, 0.375f }, {  500, 0.22f },
    {  700, 0.52f }, { 1000, 0.65f }, {  600, 0.575f }, {  900, 0.82f },
    {  700, 1.00f }, {  500, 0.65f }, {  800, 0.475f }, {  600, 0.30f },
    { 1200, 0.105f }, { 1000, 0.70f }, {  700, 0.375f }, { 1500, 0.115f },
};
#define SCRIPT_LEN (sizeof(k_script) / sizeof(k_script[0]))

static gauge_render_t       *s_gauge;
static const gauge_preset_t *s_preset;
static bool                  s_demo = true;
static bool                  s_visible = true;
static bool                  s_show_stats = true;
static bool                  s_need_full_flush = true;

static uint32_t s_elapsed_ms;
static size_t   s_step;
static uint32_t s_step_ms;
static float    s_jitter;
static bool     s_selftest;
static float    s_manual;

static uint64_t s_last_frame_us;
static float    s_fps;
static uint32_t s_render_us;
static uint32_t s_flush_us;

/* -------------------------------------------------------------------------- */

static void restart_selftest(void)
{
    s_elapsed_ms = 0;
    s_selftest = true;
    s_step = 0;
    s_step_ms = 0;
    s_need_full_flush = true;
}

/*
 * The presets describe the instrument, not the panel: take a copy and size
 * the dial to the framebuffer.  Labels, colours and slew rates stay
 * untouched.
 */
static void apply_layout(gauge_config_t *cfg)
{
    const int dial = (GFX_W < GFX_H ? GFX_W : GFX_H) - DIAL_MARGIN * 2;

    cfg->center_x = GFX_W / 2;
    cfg->center_y = GFX_H / 2;
    cfg->dial_diameter = dial;
    cfg->geometry_scale = (float)dial / 240.0f;
    cfg->text_scale = (dial + 120) / 240;   /* 2 for a 448 px dial */
    if (cfg->text_scale < 1) {
        cfg->text_scale = 1;
    }
}

static gauge_render_t *create(const gauge_preset_t *preset)
{
    gauge_config_t cfg;
    if (!preset || !preset->cfg) {
        return NULL;
    }
    cfg = *preset->cfg;
    apply_layout(&cfg);
    return gauge_render_create(&cfg);
}

bool app_gauge_start(void)
{
    if (!s_preset) {
        s_preset = gauge_preset_find("rpm");
    }
    if (!s_gauge) {
        s_gauge = create(s_preset);
        if (!s_gauge) {
            return false;
        }
    }
    s_visible = true;
    restart_selftest();
    return true;
}

void app_gauge_select(const gauge_preset_t *preset)
{
    if (!preset || !preset->cfg) {
        return;
    }
    gauge_render_t *next = create(preset);
    if (!next) {
        return;
    }
    gauge_render_destroy(s_gauge);
    s_gauge = next;
    s_preset = preset;
    s_visible = true;
    restart_selftest();
}

void app_gauge_next(void)
{
    const int count = gauge_presets_count();
    if (count < 1) {
        return;
    }
    int index = 0;
    for (int i = 0; i < count; i++) {
        if (gauge_preset_at(i) == s_preset) {
            index = i;
            break;
        }
    }
    app_gauge_select(gauge_preset_at((index + 1) % count));
}

const gauge_preset_t *app_gauge_current(void)
{
    return s_preset;
}

void app_gauge_set_demo(bool on)
{
    s_demo = on;
    if (on) {
        restart_selftest();
    }
}

bool app_gauge_is_demo(void)
{
    return s_demo;
}

void app_gauge_set_value(float value)
{
    s_manual = value;
    s_demo = false;
}

void app_gauge_set_immediate(float value)
{
    s_manual = value;
    s_demo = false;
    if (s_gauge) {
        gauge_render_set_immediate(s_gauge, value);
    }
}

void app_gauge_sweep(void)
{
    s_demo = true;
    restart_selftest();
}

void app_gauge_show_stats(bool on)
{
    s_show_stats = on;
    if (!on && s_gauge) {
        gauge_render_set_status(s_gauge, NULL);
    }
}

bool app_gauge_stats_shown(void)
{
    return s_show_stats;
}

float app_gauge_fps(void)
{
    return s_fps;
}

void app_gauge_timing(uint32_t *render_us, uint32_t *flush_us)
{
    if (render_us) *render_us = s_render_us;
    if (flush_us) *flush_us = s_flush_us;
}

void app_gauge_set_visible(bool on)
{
    s_visible = on;
    if (on) {
        s_need_full_flush = true;
    }
}

bool app_gauge_visible(void)
{
    return s_visible;
}

void app_gauge_draw_now(void)
{
    if (s_gauge) {
        gauge_render_draw(s_gauge);
    }
}

void app_gauge_rect(int *x0, int *y0, int *x1, int *y1)
{
    if (!s_gauge) {
        if (x0) *x0 = 0;
        if (y0) *y0 = 0;
        if (x1) *x1 = GFX_W;
        if (y1) *y1 = GFX_H;
        return;
    }
    gauge_geometry_t geo;
    gauge_render_geometry(s_gauge, &geo);
    const gauge_config_t *cfg = gauge_render_config(s_gauge);
    const int r = geo.dial_radius + 2;
    if (x0) *x0 = cfg->center_x - r;
    if (y0) *y0 = cfg->center_y - r;
    if (x1) *x1 = cfg->center_x + r;
    if (y1) *y1 = cfg->center_y + r;
}

/* -------------------------------------------------------------------------- */

void app_gauge_frame(float dt)
{
    if (!s_gauge || !s_visible) {
        return;
    }

    const gauge_config_t *cfg = gauge_render_config(s_gauge);
    const uint32_t dt_ms = (uint32_t)(dt * 1000.0f);

    if (!s_demo) {
        gauge_render_set_value(s_gauge, s_manual);
    } else if (s_selftest) {
        s_elapsed_ms += dt_ms;
        if (s_elapsed_ms < 500) {
            gauge_render_set_value(s_gauge, cfg->min);
        } else if (s_elapsed_ms < SELFTEST_TOP_MS) {
            gauge_render_set_value(s_gauge, cfg->max);
        } else if (s_elapsed_ms < SELFTEST_END_MS) {
            gauge_render_set_value(s_gauge, cfg->min);
        } else {
            s_selftest = false;
            s_step = 0;
            s_step_ms = 0;
        }
    } else {
        s_step_ms += dt_ms;
        if (s_step_ms >= k_script[s_step].ms) {
            s_step_ms = 0;
            s_step = (s_step + 1) % SCRIPT_LEN;
        }
        const float range = cfg->max - cfg->min;
        float value = cfg->min + k_script[s_step].frac * range;

        /* a little smoothed noise so the needle never looks generated */
        const float r = ((float)(rand() % 2001) / 1000.0f) - 1.0f;
        s_jitter += (r - s_jitter) * 0.25f;
        value += s_jitter * 0.012f * range;

        gauge_render_set_value(s_gauge, value);
    }

    const uint64_t render_start = app_now_us();
    gauge_render_tick(s_gauge, dt);
    const uint64_t flush_start = app_now_us();

    /*
     * Whole frames unless the panel is known to do partial writes properly.
     * This unit's firmware accepts the partial command and ignores it - worse,
     * a partial command can leave the panel's column window half-set (see
     * docs/usb-screen.md), so it is off by default and `--partial on` is
     * opt-in.
     */
    if (s_need_full_flush || !bsp_display_partial()) {
        gfx_flush();
        s_need_full_flush = false;
    } else {
        int x0, y0, x1, y1;
        app_gauge_rect(&x0, &y0, &x1, &y1);
        gfx_flush_rect(x0, y0, x1, y1);
    }

    const uint64_t done = app_now_us();
    s_render_us = (uint32_t)(flush_start - render_start);
    s_flush_us = (uint32_t)(done - flush_start);

    if (s_last_frame_us) {
        const uint64_t delta = done - s_last_frame_us;
        if (delta > 0) {
            const float instant = 1000000.0f / (float)delta;
            s_fps = (s_fps <= 0.0f) ? instant : (s_fps + (instant - s_fps) * 0.15f);
        }
    }
    s_last_frame_us = done;

    if (s_show_stats) {
        char buf[24];
        snprintf(buf, sizeof(buf), "%.1f fps", (double)s_fps);
        gauge_render_set_status(s_gauge, buf);
    }
}
