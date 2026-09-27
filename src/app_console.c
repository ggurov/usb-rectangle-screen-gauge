/*
 * app_console.c - the interactive console.
 *
 * The one command worth explaining is `connect`: the app starts even when
 * the screen is missing, and asks for it again when it appears.
 */
#include "app_console.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bsp.h"
#include "gauge_presets.h"
#include "gfx.h"
#include "app_gauge.h"
#include "app_tests.h"
#include "app_time.h"
#include "usb_libusb.h"

#ifdef _WIN32
#include <conio.h>
#include <io.h>
#else
#include <termios.h>
#include <unistd.h>
#include <fcntl.h>
#endif

#define LINE_MAX 128

static char s_line[LINE_MAX];
static int  s_len;
static bool s_quit;
static bool s_eof;

/* -------------------------------------------------------------------------- */
/* input                                                                      */
/* -------------------------------------------------------------------------- */

/*
 * A character from the keyboard, or from a pipe.  The keyboard path is
 * _kbhit/_getch so the REPL never blocks the frame loop; when stdin is
 * redirected (scripts, tests) the pipe is read instead and end of input means
 * "quit", so `echo fps | build\gauge.exe` does the obvious thing.
 */
static bool read_char(char *out)
{
#ifdef _WIN32
    if (_kbhit()) {
        *out = (char)_getch();
        return true;
    }
    if (!_isatty(_fileno(stdin))) {
        const int c = getchar();
        if (c == EOF) {
            s_eof = true;
            return false;
        }
        *out = (char)c;
        return true;
    }
    return false;
#else
    static bool raw_done;
    if (!raw_done) {
        struct termios t;
        tcgetattr(STDIN_FILENO, &t);
        t.c_lflag &= (tcflag_t)~ICANON;
        t.c_lflag &= (tcflag_t)~ECHO;
        tcsetattr(STDIN_FILENO, TCSANOW, &t);
        fcntl(STDIN_FILENO, F_SETFL, fcntl(STDIN_FILENO, F_GETFL) | O_NONBLOCK);
        raw_done = true;
    }
    const ssize_t n = read(STDIN_FILENO, out, 1);
    if (n == 1) {
        return true;
    }
    if (n == 0) {
        s_eof = true;
    }
    return false;
#endif
}

/* -------------------------------------------------------------------------- */
/* commands                                                                   */
/* -------------------------------------------------------------------------- */

static int cmd_help(void);
static int cmd_connect(void);

static int cmd_gauge(int argc, char **argv)
{
    if (argc < 2) {
        const gauge_preset_t *cur = app_gauge_current();
        printf("Available gauges:\n");
        for (const gauge_preset_t *p = gauge_presets_all(); p->id; p++) {
            printf("  %-6s %s%s\n", p->id, p->name,
                   (cur && strcmp(cur->id, p->id) == 0) ? "   <- active" : "");
        }
        printf("Usage: gauge <id>\n");
        return 0;
    }
    const gauge_preset_t *p = gauge_preset_find(argv[1]);
    if (!p) {
        printf("Unknown gauge '%s'. Run `gauge` for the list.\n", argv[1]);
        return 1;
    }
    app_gauge_select(p);
    app_gauge_show_stats(app_gauge_stats_shown());
    printf("Gauge -> %s\n", p->name);
    return 0;
}

static int cmd_demo(int argc, char **argv)
{
    if (argc < 2) {
        printf("Simulator is %s\n", app_gauge_is_demo() ? "ON" : "OFF");
        return 0;
    }
    if (strcmp(argv[1], "on") == 0) {
        app_gauge_set_demo(true);
        printf("Simulator on\n");
    } else if (strcmp(argv[1], "off") == 0) {
        app_gauge_set_demo(false);
        printf("Simulator off - use `value <n>`\n");
    } else if (strcmp(argv[1], "sweep") == 0) {
        app_gauge_sweep();
        printf("Self-test sweep\n");
    } else {
        printf("Usage: demo [on|off|sweep]\n");
        return 1;
    }
    return 0;
}

static int cmd_value(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: value <number>\n");
        return 1;
    }
    const float v = strtof(argv[1], NULL);
    app_gauge_set_value(v);
    printf("Value -> %.2f (simulator off)\n", (double)v);
    return 0;
}

static int cmd_fps(int argc, char **argv)
{
    if (argc >= 2) {
        if (strcmp(argv[1], "on") == 0) {
            app_gauge_show_stats(true);
        } else if (strcmp(argv[1], "off") == 0) {
            app_gauge_show_stats(false);
        } else {
            printf("usage: fps [on|off]\n");
            return 1;
        }
    }
    printf("delivered frame rate: %.1f fps  (readout %s)\n",
           (double)app_gauge_fps(),
           app_gauge_stats_shown() ? "shown" : "hidden");
    uint32_t render_us = 0, flush_us = 0;
    app_gauge_timing(&render_us, &flush_us);
    printf("last frame: %.1f ms render + %.1f ms panel flush\n",
           (double)render_us / 1000.0, (double)flush_us / 1000.0);
    return 0;
}

static int cmd_backlight(int argc, char **argv)
{
    if (argc < 2) {
        printf("Backlight %d%%\n", bsp_backlight_get());
        return 0;
    }
    const int pct = atoi(argv[1]);
    if (!bsp_backlight_set(pct)) {
        printf("Backlight change failed (screen not connected?)\n");
        return 1;
    }
    printf("Backlight -> %d%%\n", bsp_backlight_get());
    return 0;
}

static int cmd_flip(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: flip <0-3>  (0 is the panel default)\n");
        return 1;
    }
    const int mode = atoi(argv[1]) & 3;
    if (!bsp_display_flip(mode)) {
        printf("flip failed (screen not connected?)\n");
        return 1;
    }
    printf("Flip/mirror -> %d\n", mode);
    return 0;
}

static int cmd_partial(int argc, char **argv)
{
    if (argc >= 2) {
        if (strcmp(argv[1], "on") == 0) {
            bsp_display_set_partial(true);
        } else if (strcmp(argv[1], "off") == 0) {
            bsp_display_set_partial(false);
        } else {
            printf("usage: partial [on|off]\n");
            return 1;
        }
    }
    printf("Partial writes are %s\n", bsp_display_partial() ? "ON" : "OFF");
    printf("(the panel disables them by itself if the firmware rejects one)\n");
    return 0;
}

static int cmd_xshift(int argc, char **argv)
{
    if (argc >= 2) {
        bsp_display_set_xshift(atoi(argv[1]));
    }
    printf("Column offset is %d px (the panel rolls its frames by this much)\n",
           bsp_display_xshift());
    return 0;
}

static int cmd_test(int argc, char **argv)
{
    if (argc < 2) {
        printf("Test screens: fill, bars, grid, circle, quad\n");
        printf("`next` cycles; a bare `test` redraws the current one (%s).\n",
               app_tests_current());
        return 0;
    }
    app_tests_show(argv[1]);
    printf("Test screen: %s\n", app_tests_current());
    return 0;
}

static int cmd_next(void)
{
    app_tests_next();
    printf("Test screen: %s\n", app_tests_current());
    return 0;
}

static int cmd_gauge_resume(void)
{
    app_gauge_set_visible(true);
    printf("Gauge -> %s\n", app_gauge_current() ? app_gauge_current()->name : "?");
    return 0;
}

static int cmd_flush(void)
{
    uint32_t frames = 0, pixels = 0;
    uint32_t panel_frames = 0, errors = 0;
    uint64_t bytes = 0;
    gfx_get_stats(&frames, &pixels);
    bsp_lcd_flush_stats(&panel_frames, &bytes, &errors);
    printf("full frames pushed : %u\n", (unsigned)frames);
    printf("panel transfers    : %u (%llu bytes, %.1f MB)\n", (unsigned)panel_frames,
           (unsigned long long)bytes, (double)bytes / 1e6);
    printf("errors             : %u\n", (unsigned)errors);
    printf("mode               : %s\n", bsp_display_partial() ? "partial" : "whole frames");
    printf("column offset      : %d px\n", bsp_display_xshift());
    return 0;
}

static int cmd_touch(void)
{
    int x, y;
    bool pressed;
    bsp_touch_last(&x, &y, &pressed);
    printf("last touch %d,%d (%s), %u taps\n", x, y, pressed ? "down" : "up",
           (unsigned)bsp_touch_tap_count());
    return 0;
}

static int cmd_info(void)
{
    printf("panel       : %s\n", bsp_display_model());
    printf("geometry    : %dx%d\n", bsp_display_width(), bsp_display_height());
    printf("usb         : libusb at %s, %s\n", bsp_display_transport(), usb_libusb_device_path());
    printf("framebuffer : %dx%d, %u KB\n", GFX_W, GFX_H,
           (unsigned)(GFX_W * GFX_H * 2 / 1024));
    return 0;
}

static int cmd_connect(void)
{
    char err[128] = {0};
    if (bsp_display_init(0, 0, err, sizeof(err))) {
        printf("screen up: %s\n", bsp_display_model());
        app_gauge_set_visible(true);
        return 0;
    }
    printf("no screen: %s\n", err);
    return 1;
}

static int cmd_version(void)
{
    printf("usb-rectangle-screen-gauge  |  no graphics library\n");
    printf("no graphics library; framebuffer %dx%d RGB565\n", GFX_W, GFX_H);
#ifdef __DATE__
    printf("built: %s %s\n", __DATE__, __TIME__);
#endif
    return 0;
}

static int cmd_help(void)
{
    printf("Commands:\n");
    printf("  gauge [id]        list or select an instrument\n");
    printf("  demo [on|off|sweep]  engine simulator / self-test sweep\n");
    printf("  value <n>         drive the needle directly\n");
    printf("  fps [on|off]      delivered frame rate and the split\n");
    printf("  backlight [0-100] panel backlight\n");
    printf("  flip <0-3>        flip/mirror the panel\n");
    printf("  partial [on|off]  partial-rectangle writes\n");
    printf("  xshift [n]        panel column offset (240 on this glass)\n");
    printf("  test [name]       fill | bars | grid | circle | quad\n");
    printf("  next              next test screen\n");
    printf("  gauge             (bare) list instruments\n");
    printf("  resume            back to the gauge after a test screen\n");
    printf("  flush             panel transfer statistics\n");
    printf("  touch             last touch position\n");
    printf("  info              panel, usb and framebuffer\n");
    printf("  connect           (re)open the screen\n");
    printf("  version           build information\n");
    printf("  quit              exit\n");
    return 0;
}

/* -------------------------------------------------------------------------- */

static void run_command(char *line)
{
    char *argv[8];
    int argc = 0;
    for (char *tok = strtok(line, " \t"); tok && argc < 8; tok = strtok(NULL, " \t")) {
        argv[argc++] = tok;
    }
    if (argc == 0) {
        return;
    }

    const char *cmd = argv[0];
    if (strcmp(cmd, "help") == 0)          cmd_help();
    else if (strcmp(cmd, "gauge") == 0)    cmd_gauge(argc, argv);
    else if (strcmp(cmd, "demo") == 0)     cmd_demo(argc, argv);
    else if (strcmp(cmd, "value") == 0)    cmd_value(argc, argv);
    else if (strcmp(cmd, "fps") == 0)      cmd_fps(argc, argv);
    else if (strcmp(cmd, "backlight") == 0) cmd_backlight(argc, argv);
    else if (strcmp(cmd, "flip") == 0)     cmd_flip(argc, argv);
    else if (strcmp(cmd, "partial") == 0)  cmd_partial(argc, argv);
    else if (strcmp(cmd, "xshift") == 0)   cmd_xshift(argc, argv);
    else if (strcmp(cmd, "test") == 0)     cmd_test(argc, argv);
    else if (strcmp(cmd, "next") == 0)     cmd_next();
    else if (strcmp(cmd, "resume") == 0)   cmd_gauge_resume();
    else if (strcmp(cmd, "flush") == 0)    cmd_flush();
    else if (strcmp(cmd, "touch") == 0)    cmd_touch();
    else if (strcmp(cmd, "info") == 0)     cmd_info();
    else if (strcmp(cmd, "connect") == 0)  cmd_connect();
    else if (strcmp(cmd, "version") == 0)  cmd_version();
    else if (strcmp(cmd, "quit") == 0 || strcmp(cmd, "exit") == 0) {
        s_quit = true;
    } else {
        printf("Unknown command '%s'. Try `help`.\n", cmd);
    }
    fflush(stdout);
}

void app_console_start(void)
{
    printf("gauge> ");
    fflush(stdout);
}

void app_console_poll(void)
{
    char c;
    while (read_char(&c)) {
        if (c == '\r' || c == '\n') {
            putchar('\n');
            s_line[s_len] = '\0';
            run_command(s_line);
            s_len = 0;
            if (!s_quit) {
                printf("gauge> ");
                fflush(stdout);
            }
        } else if (c == 8 || c == 127) {   /* backspace */
            if (s_len > 0) {
                s_len--;
                printf("\b \b");
                fflush(stdout);
            }
        } else if (c == 27) {
            /* an escape sequence (arrow keys): drop the three bytes */
            char skip;
            read_char(&skip);
            read_char(&skip);
        } else if (c >= 32 && s_len < LINE_MAX - 1) {
            s_line[s_len++] = c;
            putchar(c);
            fflush(stdout);
        }
    }
}

bool app_console_wants_quit(void)
{
    if (s_eof && !s_quit) {
        s_quit = true;
        printf("\n(input ended; exiting)\n");
    }
    return s_quit;
}
