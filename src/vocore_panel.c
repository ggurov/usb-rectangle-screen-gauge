/*
 * vocore_panel.c - one VoCore USB2.0 Screen over a transport.
 *
 * The state machine is deliberately small: identify, wake, then push frames.
 * Frame data goes out exactly as the SDK sends it, so a wrong byte here is a
 * wrong pixel on the panel; tests/unit/test_vocore_panel.c pins the whole
 * conversation against a fake transport.
 */
#include "vocore_panel.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CTRL_TIMEOUT_MS  1000
#define BULK_TIMEOUT_MS  2000
#define TOUCH_REPORT_LEN 128   /* the SDK reads 128 bytes for two points */

struct vocore_panel {
    const vocore_transport_t *t;
    const char *kind;

    vocore_id_t id;
    int brightness;
    int flip;
    int xshift;

    bool partial;
    bool awake;

    /* scratch the size of one whole frame, plus any panel prefix */
    uint8_t *frame_buf;
    int frame_buf_len;
    uint8_t *roll_buf;      /* only when the panel needs a column offset */

    uint32_t frames;
    uint64_t bytes;   /* 64-bit: a five-minute session is already ~5 GB */
    uint32_t errors;

    uint8_t touch_raw[TOUCH_REPORT_LEN];
};

/* -------------------------------------------------------------------------- */
/* small helpers                                                              */
/* -------------------------------------------------------------------------- */

static int ctrl_out(vocore_panel_t *p, uint8_t request, const uint8_t *data, int len)
{
    return p->t->control_out(p->t->ctx, request, data, len, CTRL_TIMEOUT_MS);
}

static int ctrl_in(vocore_panel_t *p, uint8_t request, uint8_t *data, int len)
{
    return p->t->control_in(p->t->ctx, request, data, len, CTRL_TIMEOUT_MS);
}

/*
 * A register read is three transfers: the address written with 0xb5, a
 * one-byte acknowledge on 0xb6, then the payload on 0xb7.  Firmwares that do
 * not implement a register answer with all ones.
 */
static uint32_t read_reg(vocore_panel_t *p, uint8_t tag, int payload_len,
                         bool *ok)
{
    uint8_t cmd[8];
    uint8_t reply[16];

    if (ok) *ok = false;
    if (payload_len < 1 || vocore_reg_reply_len(payload_len) > (int)sizeof(reply)) {
        return VOCORE_ID_UNKNOWN;
    }

    const int clen = vocore_cmd_reg_addr(tag, payload_len, cmd);
    if (ctrl_out(p, VOCORE_REQ_ADDR, cmd, clen) != clen) {
        return VOCORE_ID_UNKNOWN;
    }
    if (ctrl_in(p, VOCORE_REQ_ACK, reply, 1) != 1) {
        return VOCORE_ID_UNKNOWN;
    }
    const int rlen = vocore_reg_reply_len(payload_len);
    if (ctrl_in(p, VOCORE_REQ_DATA, reply, rlen) != rlen) {
        return VOCORE_ID_UNKNOWN;
    }
    if (ok) *ok = true;
    return vocore_reg_value(reply, rlen);
}

static void read_info(vocore_panel_t *p, char *out, int outlen)
{
    uint8_t buf[65];
    out[0] = '\0';

    for (int attempt = 0; attempt < 2; attempt++) {
        const uint8_t request = attempt == 0 ? VOCORE_REQ_INFO : VOCORE_REQ_INFO2;
        const int n = ctrl_in(p, request, buf, 64);
        if (n <= 0) {
            continue;
        }
        buf[n < 64 ? n : 64] = '\0';
        if (buf[0] == 'v') {
            const int copy = (n < outlen - 1) ? n : outlen - 1;
            memcpy(out, buf, (size_t)copy);
            out[copy] = '\0';
            return;
        }
    }
}

static bool send_cmd(vocore_panel_t *p, const uint8_t *cmd, int len)
{
    if (ctrl_out(p, VOCORE_REQ_CMD, cmd, len) != len) {
        p->errors++;
        return false;
    }
    return true;
}

/* -------------------------------------------------------------------------- */
/* lifecycle                                                                  */
/* -------------------------------------------------------------------------- */

vocore_panel_t *vocore_panel_open(const vocore_transport_t *transport, const char *kind)
{
    if (!transport || !transport->control_out || !transport->control_in ||
        !transport->bulk_out || !transport->interrupt_in) {
        return NULL;
    }
    vocore_panel_t *p = calloc(1, sizeof(*p));
    if (!p) {
        return NULL;
    }
    p->t = transport;
    p->kind = kind ? kind : "?";
    p->brightness = 60;
    p->partial = true;
    return p;
}

void vocore_panel_close(vocore_panel_t *p)
{
    if (!p) {
        return;
    }
    free(p->frame_buf);
    free(p->roll_buf);
    free(p);
}

bool vocore_panel_bring_up(vocore_panel_t *p, int force_width, int force_height,
                           char *err, int errlen)
{
    if (!p) {
        return false;
    }

    bool ok = false;
    uint32_t ver = VOCORE_ID_UNKNOWN;
    uint32_t code = VOCORE_ID_UNKNOWN;
    char info[64];

    read_info(p, info, sizeof(info));
    ver = read_reg(p, VOCORE_REG_SCREEN, 4, &ok);
    if (ok) {
        code = read_reg(p, VOCORE_REG_VERSION, 4, NULL);
    }

    vocore_id_from_regs(ver, code, info[0] ? info : NULL, &p->id);
    if (force_width > 0 && force_height > 0) {
        p->id.width = force_width;
        p->id.height = force_height;
        p->id.known = true;
        snprintf(p->id.model, sizeof(p->id.model), "forced %dx%d", force_width, force_height);
    }

    if (p->id.width <= 0 || p->id.height <= 0) {
        if (err && errlen) snprintf(err, errlen, "no geometry");
        return false;
    }

    const int need = p->id.width * p->id.height * 2 + p->id.margin;
    free(p->frame_buf);
    p->frame_buf = malloc((size_t)need);
    if (!p->frame_buf) {
        if (err && errlen) snprintf(err, errlen, "out of memory for %d-byte frame", need);
        return false;
    }
    p->frame_buf_len = need;
    memset(p->frame_buf, 0, (size_t)need);

    uint8_t cmd[16];
    p->awake = send_cmd(p, cmd, vocore_cmd_wake(cmd));
    p->flip = -1;
    vocore_panel_set_brightness(p, p->brightness);

    if (err && errlen && !p->awake) {
        snprintf(err, errlen, "sleep-out was not acknowledged");
    }
    return true;
}

const vocore_id_t *vocore_panel_id(const vocore_panel_t *p)
{
    return p ? &p->id : NULL;
}

int vocore_panel_width(const vocore_panel_t *p)
{
    return p ? p->id.width : 0;
}

int vocore_panel_height(const vocore_panel_t *p)
{
    return p ? p->id.height : 0;
}

bool vocore_panel_set_brightness(vocore_panel_t *p, int percent)
{
    uint8_t cmd[8];
    if (!p) {
        return false;
    }
    const int n = vocore_cmd_brightness(percent, cmd);
    if (!send_cmd(p, cmd, n)) {
        return false;
    }
    p->brightness = cmd[6];
    return true;
}

int vocore_panel_brightness(const vocore_panel_t *p)
{
    return p ? p->brightness : 0;
}

bool vocore_panel_set_flip(vocore_panel_t *p, int mode)
{
    uint8_t cmd[8];
    if (!p) {
        return false;
    }
    const int n = vocore_cmd_flip(mode, cmd);
    if (!send_cmd(p, cmd, n)) {
        return false;
    }
    p->flip = mode;
    return true;
}

/* -------------------------------------------------------------------------- */
/* frame transfer                                                             */
/* -------------------------------------------------------------------------- */

/*
 * The panel consumes RGB565 little-endian straight from the wire, which is
 * exactly how the framebuffer is laid out on x86.  A big-endian host would
 * have to swap here.  `pixels` is a whole frame: width*height entries.
 */
bool vocore_panel_show(vocore_panel_t *p, const uint16_t *pixels)
{
    if (!p || !pixels || !p->frame_buf) {
        return false;
    }

    const int image = p->id.width * p->id.height * 2;
    const uint8_t *data = (const uint8_t *)pixels;
    int count = image;

    /* The panel's column origin: roll each row so that framebuffer column x
     * lands on glass column (x + xshift) mod width.  The roll goes into its
     * own buffer so a gathered frame can be rolled too. */
    if (p->xshift) {
        if (!p->roll_buf) {
            p->roll_buf = malloc((size_t)image);
            if (!p->roll_buf) {
                p->errors++;
                return false;
            }
        }
        const int w = p->id.width;
        const int shift = ((p->xshift % w) + w) % w;
        for (int y = 0; y < p->id.height; y++) {
            const uint16_t *src = pixels + (size_t)y * w;
            uint16_t *dst = (uint16_t *)(p->roll_buf + (size_t)y * w * 2);
            memcpy(dst + shift, src, (size_t)(w - shift) * 2);
            memcpy(dst, src + (w - shift), (size_t)shift * 2);
        }
        data = p->roll_buf;
    } else if (p->id.margin > 0 && data != p->frame_buf) {
        /*
         * fbusb, the in-kernel driver the 5-inch SLM glass is actually used
         * with, sends the image first and the panel's extra 320 bytes after
         * it, with the command count including them.  screen_test.c shifts
         * the image by 320 instead and sends a count that excludes them; the
         * two disagree, and the driver that real users run wins.
         */
        memcpy(p->frame_buf, pixels, (size_t)image);
        data = p->frame_buf;
    }
    if (p->id.margin > 0) {
        count = p->frame_buf_len;
    }

    uint8_t cmd[8];
    const int clen = vocore_cmd_full_frame(VOCORE_MODE_RGB565, (uint32_t)count, cmd);
    if (!send_cmd(p, cmd, clen)) {
        return false;
    }
    if (p->t->bulk_out(p->t->ctx, data, count, BULK_TIMEOUT_MS) != count) {
        p->errors++;
        return false;
    }

    p->frames++;
    p->bytes += (uint64_t)count;
    return true;
}

/* Gather a packed rectangle into the frame scratch buffer, whole-frame style. */
static bool show_gathered(vocore_panel_t *p, const uint16_t *packed,
                          int x0, int y0, int x1, int y1)
{
    if (!p->frame_buf) {
        return false;
    }
    const int w = x1 - x0;
    for (int row = y0; row < y1; row++) {
        memcpy(p->frame_buf + (size_t)row * p->id.width * 2 + (size_t)x0 * 2,
               packed + (size_t)(row - y0) * w,
               (size_t)w * 2);
    }
    return vocore_panel_show(p, (const uint16_t *)p->frame_buf);
}

/* One rectangle straight down the wire, in the panel's own columns. */
static bool send_partial(vocore_panel_t *p, const void *data, int x, int y, int w, int h)
{
    const int bytes = w * h * 2;
    uint8_t cmd[16];
    const int clen = vocore_cmd_partial_frame(VOCORE_MODE_RGB565, (uint32_t)bytes,
                                              x, y, w, h, cmd);
    if (!send_cmd(p, cmd, clen) ||
        p->t->bulk_out(p->t->ctx, (const uint8_t *)data, bytes, BULK_TIMEOUT_MS) != bytes) {
        /*
         * Firmware older than v0.15 does not know the partial command.  Fall
         * back to whole frames for the rest of the session rather than
         * halving the frame rate with a failed transfer every frame.
         */
        p->partial = false;
        return false;
    }
    p->bytes += (uint64_t)bytes;
    return true;
}

/*
 * A rectangle whose column order differs from the panel's: gather each row
 * into the scratch buffer and send that.  Only the rolled path needs this.
 */
static bool send_partial_gathered(vocore_panel_t *p, const uint16_t *packed, int packed_w,
                                  int col_offset, int w, int h, int x, int y)
{
    if (!p->frame_buf || (size_t)w * h * 2 > (size_t)p->frame_buf_len) {
        return false;
    }
    for (int row = 0; row < h; row++) {
        memcpy(p->frame_buf + (size_t)row * w * 2,
               packed + (size_t)row * packed_w + col_offset,
               (size_t)w * 2);
    }
    return send_partial(p, p->frame_buf, x, y, w, h);
}

bool vocore_panel_show_rect(vocore_panel_t *p, const uint16_t *pixels,
                            int x0, int y0, int x1, int y1)
{
    if (!p || !pixels) {
        return false;
    }
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > p->id.width) x1 = p->id.width;
    if (y1 > p->id.height) y1 = p->id.height;
    if (x1 <= x0 || y1 <= y0) {
        return true;
    }

    const bool whole = (x0 == 0 && y0 == 0 && x1 == p->id.width && y1 == p->id.height);
    if (whole) {
        return vocore_panel_show(p, pixels);
    }
    if (!p->partial) {
        return show_gathered(p, pixels, x0, y0, x1, y1);
    }

    const int w = x1 - x0;
    const int h = y1 - y0;
    const int shift = p->xshift ? ((p->xshift % p->id.width) + p->id.width) % p->id.width : 0;

    if (shift == 0) {
        if (!send_partial(p, pixels, x0, y0, w, h)) {
            return show_gathered(p, pixels, x0, y0, x1, y1);
        }
        p->frames++;
        return true;
    }

    /*
     * Rolled panel: framebuffer column x is shown at column
     * (x + shift) mod width, so a rectangle that spans the wrap has to go out
     * as two rectangles in the panel's column order.
     */
    const int cut = p->id.width - shift;   /* first framebuffer column that wraps */
    bool ok = true;

    if (x0 < cut && ok) {
        const int end = (x1 < cut) ? x1 : cut;
        ok = send_partial_gathered(p, pixels, w, 0, end - x0, h, x0 + shift, y0);
    }
    if (x1 > cut && ok) {
        const int start = (x0 > cut) ? x0 : cut;
        ok = send_partial_gathered(p, pixels, w, start - x0, x1 - start, h,
                                   start + shift - p->id.width, y0);
    }
    if (!ok && p->partial) {
        /* failed for another reason: let the whole-frame path try */
        return show_gathered(p, pixels, x0, y0, x1, y1);
    }
    if (ok) {
        p->frames++;
    }
    return ok;
}

void vocore_panel_set_partial(vocore_panel_t *p, bool on)
{
    if (p) {
        p->partial = on;
    }
}

void vocore_panel_set_xshift(vocore_panel_t *p, int pixels)
{
    if (p) {
        p->xshift = pixels;
    }
}

int vocore_panel_xshift(const vocore_panel_t *p)
{
    return p ? p->xshift : 0;
}

bool vocore_panel_partial(const vocore_panel_t *p)
{
    return p && p->partial;
}

/* -------------------------------------------------------------------------- */
/* touch                                                                      */
/* -------------------------------------------------------------------------- */

bool vocore_panel_poll_touch(vocore_panel_t *p, vocore_touch_t *touch, int timeout_ms)
{
    if (!p || !touch) {
        return false;
    }
    const int n = p->t->interrupt_in(p->t->ctx, p->touch_raw,
                                     sizeof(p->touch_raw), timeout_ms);
    if (n <= 0) {
        return false;
    }
    return vocore_touch_decode(p->touch_raw, n, touch);
}

void vocore_panel_stats(const vocore_panel_t *p, uint32_t *frames,
                        uint64_t *bytes, uint32_t *errors)
{
    if (!p) {
        return;
    }
    if (frames) *frames = p->frames;
    if (bytes)  *bytes = p->bytes;
    if (errors) *errors = p->errors;
}
