/*
 * main.c - the gauge on a USB screen: console first, then the panel.
 *
 * The console comes up before the screen is touched, so a dead panel can
 * never lock you out; if the screen is missing, `connect` looks again.
 *
 *   gauge.exe [options]
 *     --size WxH        override the panel geometry (default: its registers)
 *     --brightness N    0-100, default 60
 *     --gauge ID        start on a preset (rpm, temp, boost, volts)
 *     --value N         start with a fixed reading, simulator off
 *     --test NAME       start on a bring-up screen
 *     --partial off     whole frames only
 *     --flip N          flip/mirror 0-3
 *     --xshift N        panel column offset (default 240: this glass)
 *     --run SECONDS     leave after N seconds (for scripted screenshots)
 *     --render FILE     draw one frame to a raw RGB565 file and exit
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#endif

#include "bsp.h"
#include "gauge_presets.h"
#include "gfx.h"
#include "app_console.h"
#include "app_gauge.h"
#include "app_tests.h"
#include "app_time.h"

#define MIN_FRAME_MS 2

/*
 * Ctrl+C and console close must release the device cleanly.  Force-killing
 * this process mid-transfer left the panel's firmware answering nothing at
 * all, on any endpoint, until it was unplugged - worth avoiding.
 */
static volatile int s_signalled;

#ifdef _WIN32
static BOOL WINAPI on_console_event(DWORD type)
{
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT) {
        s_signalled = 1;
        return TRUE;
    }
    return FALSE;
}
#endif

static void install_console_handler(void)
{
#ifdef _WIN32
    SetConsoleCtrlHandler(on_console_event, TRUE);
#endif
}

static void usage(void)
{
    printf("usage: gauge.exe [--size WxH] [--brightness N] [--gauge ID]\n"
           "                   [--value N] [--test NAME] [--partial on|off] [--flip N]\n"
           "                   [--xshift N] [--run SECONDS] [--render FILE]\n");
    printf("ids: ");
    for (const gauge_preset_t *p = gauge_presets_all(); p->id; p++) {
        printf("%s ", p->id);
    }
    printf("\n");
}

int main(int argc, char **argv)
{
    int force_w = 0, force_h = 0;
    int brightness = 60;
    int flip = -1;
    /*
     * This 4-inch glass ignores partial writes and every frame lands straight
     * after a power cycle: both of these are the honest defaults, and both
     * have a flag because the panel's state can change (see docs/usb-screen.md).
     */
    int xshift = 0;
    bool partial = false;
    const char *gauge_id = NULL;
    const char *test = NULL;
    bool have_value = false;
    float value = 0.0f;
    double run_seconds = 0.0;
    const char *render_path = NULL;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
            usage();
            return 0;
        } else if (strcmp(a, "--size") == 0 && i + 1 < argc) {
            if (sscanf(argv[++i], "%dx%d", &force_w, &force_h) != 2) {
                fprintf(stderr, "--size wants WxH, e.g. 480x800\n");
                return 2;
            }
        } else if (strcmp(a, "--brightness") == 0 && i + 1 < argc) {
            brightness = atoi(argv[++i]);
        } else if (strcmp(a, "--gauge") == 0 && i + 1 < argc) {
            gauge_id = argv[++i];
        } else if (strcmp(a, "--value") == 0 && i + 1 < argc) {
            value = strtof(argv[++i], NULL);
            have_value = true;
        } else if (strcmp(a, "--test") == 0 && i + 1 < argc) {
            test = argv[++i];
        } else if (strcmp(a, "--partial") == 0 && i + 1 < argc) {
            partial = strcmp(argv[++i], "off") != 0;
        } else if (strcmp(a, "--flip") == 0 && i + 1 < argc) {
            flip = atoi(argv[++i]);
        } else if (strcmp(a, "--xshift") == 0 && i + 1 < argc) {
            xshift = atoi(argv[++i]);
        } else if (strcmp(a, "--run") == 0 && i + 1 < argc) {
            run_seconds = strtod(argv[++i], NULL);
        } else if (strcmp(a, "--render") == 0 && i + 1 < argc) {
            render_path = argv[++i];
        } else {
            fprintf(stderr, "unknown option '%s'\n", a);
            usage();
            return 2;
        }
    }

    app_time_init();
    install_console_handler();
    gfx_init();

    printf("usb-rectangle-screen-gauge: host gauge for the VoCore USB2.0 screen\n");
    printf("framebuffer %dx%d RGB565 (%u KB), no graphics library\n",
           GFX_W, GFX_H, (unsigned)(GFX_W * GFX_H * 2 / 1024));

    char err[128] = {0};
    if (bsp_display_init(force_w, force_h, err, sizeof(err))) {
        printf("screen: %s\n", bsp_display_model());
        printf("        %dx%d, %s\n", bsp_display_width(), bsp_display_height(),
               bsp_display_transport());
        bsp_display_set_partial(partial);
        bsp_display_set_xshift(xshift);
        bsp_backlight_set(brightness);
        if (flip >= 0) {
            bsp_display_flip(flip);
        }
    } else {
        printf("screen: not connected (%s)\n", err);
        printf("        the console still works; `connect` tries again\n");
    }

    if (!app_gauge_start()) {
        fprintf(stderr, "could not create the gauge\n");
        return 1;
    }
    if (gauge_id) {
        const gauge_preset_t *p = gauge_preset_find(gauge_id);
        if (p) {
            app_gauge_select(p);
        } else {
            fprintf(stderr, "unknown gauge '%s'\n", gauge_id);
        }
    }
    if (have_value) {
        app_gauge_set_value(value);
    }
    if (test) {
        app_tests_show(test);
    }

    if (render_path) {
        /* offline: draw one frame and write the raw RGB565 buffer, no panel */
        if (have_value) {
            app_gauge_set_immediate(value);
        }
        app_gauge_draw_now();
        FILE *fp = fopen(render_path, "wb");
        if (!fp) {
            fprintf(stderr, "cannot write %s\n", render_path);
            return 1;
        }
        const size_t n = (size_t)GFX_W * GFX_H * 2;
        if (fwrite(gfx_framebuffer(), 1, n, fp) != n) {
            fprintf(stderr, "short write to %s\n", render_path);
            fclose(fp);
            return 1;
        }
        fclose(fp);
        printf("wrote %s (%zu bytes, %dx%d RGB565)\n", render_path, n, GFX_W, GFX_H);
        return 0;
    }

    app_console_start();

    const uint64_t start = app_now_us();
    uint64_t last = start;
    uint64_t deadline = 0;
    if (run_seconds > 0) {
        deadline = start + (uint64_t)(run_seconds * 1e6);
    }

    while (!app_console_wants_quit() && !s_signalled) {
        app_console_poll();

        const uint64_t now = app_now_us();
        const float dt = (float)(now - last) / 1000000.0f;
        if (dt >= (float)MIN_FRAME_MS / 1000.0f) {
            last = now;

            /* a tap on the screen cycles the instruments.
             * Polling per frame rather than per loop keeps a silent touch
             * endpoint from adding its timeout to every pass. */
            if (bsp_touch_poll()) {
                int x, y;
                bool down;
                bsp_touch_last(&x, &y, &down);
                if (!down) {
                    app_gauge_next();
                    printf("\ntouch: gauge -> %s\n",
                           app_gauge_current() ? app_gauge_current()->name : "?");
                    fflush(stdout);
                }
            }

            app_gauge_frame(dt);
        }
        if (deadline && now >= deadline) {
            break;
        }
        app_sleep_ms(1);
    }

    uint32_t frames = 0, errors = 0;
    uint64_t bytes = 0;
    bsp_lcd_flush_stats(&frames, &bytes, &errors);
    const double seconds = (double)(app_now_us() - start) / 1e6;
    if (seconds > 0.1) {
        printf("summary: %.1f s, %u transfers, %.1f MB/s, %.1f fps, %u errors\n",
               seconds, (unsigned)frames,
               (double)bytes / seconds / 1e6,
               (double)frames / seconds,
               (unsigned)errors);
    }
    if (errors) {
        printf("the screen stopped accepting transfers: if it stays silent on\n"
               "every command, unplug it for a second (see docs/usb-screen.md)\n");
    }

    bsp_display_shutdown();
    printf("bye\n");
    return 0;
}
