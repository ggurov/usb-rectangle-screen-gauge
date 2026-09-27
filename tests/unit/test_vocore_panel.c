/*
 * test_vocore_panel.c - the whole USB conversation, against a fake transport.
 *
 * The fake records control and bulk transfers and scripts the replies, so the
 * order of the identification reads, the wake, the frame headers and the
 * partial-rectangle fallback are all asserted without a screen attached.
 */
#include <stdlib.h>
#include <string.h>

#include "test_framework.h"
#include "vocore_panel.h"

/* -------------------------------------------------------------------------- */
/* fake transport                                                             */
/* -------------------------------------------------------------------------- */

#define MAX_CMDS 16
#define FAKE_BULK_MAX (1024 * 1024)

typedef struct {
    /* scripted register values */
    uint32_t ver;
    uint32_t code;
    uint8_t  info[64];
    int      info_len;

    /* last register address written */
    uint8_t addr[8];
    int     addr_len;

    /* recorded commands */
    uint8_t cmd[MAX_CMDS][16];
    int     cmd_len[MAX_CMDS];
    int     cmd_count;

    /* recorded transfers */
    uint8_t *bulk;
    int      bulk_len;
    int      bulk_count;
    int      fail_bulk_at;         /* call index to fail, -1 = never */

    uint8_t touch[64];
    int     touch_len;
    int     touch_count;
} fake_t;

static int fake_control_out(void *ctx, uint8_t request, const uint8_t *data,
                            int len, int timeout_ms)
{
    (void)timeout_ms;
    fake_t *f = ctx;

    if (request == VOCORE_REQ_ADDR) {
        memcpy(f->addr, data, (size_t)len);
        f->addr_len = len;
        return len;
    }
    if (request == VOCORE_REQ_CMD) {
        if (f->cmd_count >= MAX_CMDS) {
            TF_CHECK_MSG(false, "too many commands for the fake");
            return -1;
        }
        memcpy(f->cmd[f->cmd_count], data, (size_t)len);
        f->cmd_len[f->cmd_count] = len;
        f->cmd_count++;
        return len;
    }
    TF_CHECK_MSG(false, "unexpected control OUT request 0x%02x", request);
    return -1;
}

static int fake_control_in(void *ctx, uint8_t request, uint8_t *data,
                           int len, int timeout_ms)
{
    (void)timeout_ms;
    fake_t *f = ctx;

    if (request == VOCORE_REQ_ACK) {
        data[0] = 0x01;
        return 1;
    }
    if (request == VOCORE_REQ_DATA) {
        uint32_t value = VOCORE_ID_UNKNOWN;
        if (f->addr_len == 5) {
            if (f->addr[4] == VOCORE_REG_SCREEN)  value = f->ver;
            if (f->addr[4] == VOCORE_REG_VERSION) value = f->code;
        }
        data[0] = 0x01;
        data[1] = (uint8_t)(value & 0xFF);
        data[2] = (uint8_t)((value >> 8) & 0xFF);
        data[3] = (uint8_t)((value >> 16) & 0xFF);
        data[4] = (uint8_t)((value >> 24) & 0xFF);
        return len;
    }
    if (request == VOCORE_REQ_INFO) {
        if (f->info_len <= 0) {
            return -1;   /* firmware without the info string stalls */
        }
        memcpy(data, f->info, (size_t)f->info_len);
        return f->info_len;
    }
    if (request == VOCORE_REQ_INFO2) {
        return -1;
    }
    TF_CHECK_MSG(false, "unexpected control IN request 0x%02x", request);
    return -1;
}

static int fake_bulk_out(void *ctx, const uint8_t *data, int len, int timeout_ms)
{
    (void)timeout_ms;
    fake_t *f = ctx;

    if (f->fail_bulk_at == f->bulk_count) {
        f->bulk_count++;
        return -1;
    }
    if (f->bulk && len <= FAKE_BULK_MAX) {
        memcpy(f->bulk, data, (size_t)len);
    }
    f->bulk_count++;
    f->bulk_len = len;
    return len;
}

static int fake_interrupt_in(void *ctx, uint8_t *data, int len, int timeout_ms)
{
    (void)timeout_ms;
    fake_t *f = ctx;
    if (f->touch_len <= 0) {
        return 0;   /* timeout */
    }
    f->touch_count++;
    const int n = f->touch_len < len ? f->touch_len : len;
    memcpy(data, f->touch, (size_t)n);
    return n;
}

static void fake_init(fake_t *f)
{
    memset(f, 0, sizeof(*f));
    f->ver = VOCORE_ID_UNKNOWN;
    f->code = VOCORE_ID_UNKNOWN;
    f->fail_bulk_at = -1;
}

static vocore_transport_t make_transport(fake_t *f)
{
    vocore_transport_t t = {
        .ctx = f,
        .control_out = fake_control_out,
        .control_in = fake_control_in,
        .bulk_out = fake_bulk_out,
        .interrupt_in = fake_interrupt_in,
    };
    return t;
}

static vocore_panel_t *bring_up(fake_t *f, vocore_transport_t *t)
{
    fake_init(f);
    *t = make_transport(f);
    vocore_panel_t *p = vocore_panel_open(t, "fake");
    if (!p || !vocore_panel_bring_up(p, 0, 0, NULL, 0)) {
        TF_CHECK_MSG(false, "the fake panel did not come up");
        return NULL;
    }
    return p;
}

/* 480x800x2: the geometry the app falls back to when the panel reports none */
#define FAKE_FRAME (480 * 800 * 2)
static uint16_t g_fb[480 * 854];  /* bigger than any panel tested here */

/* -------------------------------------------------------------------------- */

TF_TEST(vocore_panel, bring_up_reads_registers_then_wakes_and_sets_brightness)
{
    fake_t f;
    vocore_transport_t t;
    vocore_panel_t *p = bring_up(&f, &t);
    TF_REQUIRE(p != NULL);

    const vocore_id_t *id = vocore_panel_id(p);
    TF_CHECK(!id->known);
    TF_EQ_INT(id->width, 480);
    TF_EQ_INT(id->height, 800);
    TF_EQ_INT(id->margin, 0);

    TF_EQ_INT(f.cmd_count, 2);
    TF_EQ_INT(f.cmd_len[0], 6);
    TF_EQ_INT(f.cmd[0][1], VOCORE_CMD_SLEEP_OUT);
    TF_EQ_INT(f.cmd_len[1], 8);
    TF_EQ_INT(f.cmd[1][1], VOCORE_CMD_BRIGHTNESS);
    TF_EQ_INT(f.cmd[1][6], 60);
    TF_EQ_INT(vocore_panel_brightness(p), 60);

    vocore_panel_close(p);
}

TF_TEST(vocore_panel, identified_five_inch_panel_gets_its_margin)
{
    fake_t f;
    vocore_transport_t t;
    fake_init(&f);
    f.ver = 0x00000005;
    t = make_transport(&f);

    vocore_panel_t *p = vocore_panel_open(&t, "fake");
    TF_REQUIRE(p != NULL);
    TF_CHECK(vocore_panel_bring_up(p, 0, 0, NULL, 0));
    TF_EQ_INT(vocore_panel_width(p), 480);
    TF_EQ_INT(vocore_panel_height(p), 854);
    TF_EQ_INT(vocore_panel_id(p)->margin, 320);

    f.bulk = malloc(FAKE_BULK_MAX);
    TF_REQUIRE(f.bulk != NULL);
    TF_CHECK(vocore_panel_show(p, g_fb));

    /* the count and the transfer both include the panel's 320-byte prefix */
    const uint8_t *hdr = f.cmd[f.cmd_count - 1];
    const uint32_t total = 480u * 854u * 2u + 320u;   /* 820160 = 0x0c83c0 */
    TF_EQ_INT(hdr[2], 0xc0);
    TF_EQ_INT(hdr[3], 0x83);
    TF_EQ_INT(hdr[4], 0x0c);
    TF_EQ_INT(f.bulk_len, (int)total);

    free(f.bulk);
    vocore_panel_close(p);
}

TF_TEST(vocore_panel, forced_size_overrides_the_registers)
{
    fake_t f;
    vocore_transport_t t;
    fake_init(&f);
    t = make_transport(&f);

    vocore_panel_t *p = vocore_panel_open(&t, "fake");
    TF_REQUIRE(p != NULL);
    TF_CHECK(vocore_panel_bring_up(p, 480, 800, NULL, 0));
    TF_EQ_INT(vocore_panel_width(p), 480);
    TF_EQ_INT(vocore_panel_height(p), 800);
    TF_CHECK(vocore_panel_id(p)->known);
    vocore_panel_close(p);
}

TF_TEST(vocore_panel, full_frame_goes_out_with_the_sdk_header)
{
    fake_t f;
    vocore_transport_t t;
    vocore_panel_t *p = bring_up(&f, &t);
    TF_REQUIRE(p != NULL);

    for (int i = 0; i < 480 * 854; i++) {
        g_fb[i] = (uint16_t)(i * 7);
    }

    TF_CHECK(vocore_panel_show(p, g_fb));

    const uint8_t *hdr = f.cmd[f.cmd_count - 1];
    TF_EQ_INT(f.cmd_len[f.cmd_count - 1], 6);
    TF_EQ_INT(hdr[0], VOCORE_MODE_RGB565);
    TF_EQ_INT(hdr[1], 0x2c);
    TF_EQ_INT(hdr[2], 0x00);
    TF_EQ_INT(hdr[3], 0xb8);      /* 768000 = 0x0bb800 */
    TF_EQ_INT(hdr[4], 0x0b);

    TF_EQ_INT(f.bulk_count, 1);
    TF_EQ_INT(f.bulk_len, FAKE_FRAME);

    uint32_t frames = 0, errors = 0;
    uint64_t bytes = 0;
    vocore_panel_stats(p, &frames, &bytes, &errors);
    TF_EQ_INT(frames, 1);
    TF_EQ_INT(bytes, FAKE_FRAME);
    TF_EQ_INT(errors, 0);

    vocore_panel_close(p);
}

TF_TEST(vocore_panel, partial_rectangle_goes_out_packed)
{
    fake_t f;
    vocore_transport_t t;
    vocore_panel_t *p = bring_up(&f, &t);
    TF_REQUIRE(p != NULL);

    for (int i = 0; i < 480 * 854; i++) {
        g_fb[i] = (uint16_t)(i * 3);
    }

    /* the caller hands over the packed rectangle, as gfx_flush_rect does */
    static uint16_t packed[20 * 20];
    for (int i = 0; i < 20 * 20; i++) {
        packed[i] = (uint16_t)(1000 + i);
    }
    f.bulk = malloc(FAKE_BULK_MAX);
    TF_REQUIRE(f.bulk != NULL);
    TF_CHECK(vocore_panel_show_rect(p, packed, 10, 20, 30, 40));

    const uint8_t *hdr = f.cmd[f.cmd_count - 1];
    TF_EQ_INT(f.cmd_len[f.cmd_count - 1], 12);
    TF_EQ_INT(hdr[0], VOCORE_MODE_RGB565);
    TF_EQ_INT(hdr[1], 0x2c);
    TF_EQ_INT(hdr[2], 0x20);          /* 800 bytes = 0x00000320 */
    TF_EQ_INT(hdr[3], 0x03);
    TF_EQ_INT(hdr[6], 10);            /* x */
    TF_EQ_INT(hdr[7], 0);
    TF_EQ_INT(hdr[8], 20);            /* y */
    TF_EQ_INT(hdr[10], 20);           /* w */

    TF_EQ_INT(f.bulk_len, 20 * 20 * 2);
    for (int i = 0; i < 20 * 20; i++) {
        const uint16_t got = ((const uint16_t *)f.bulk)[i];
        TF_CHECK_MSG(got == packed[i], "packed pixel %d is %u, want %u",
                     i, got, packed[i]);
    }

    free(f.bulk);
    vocore_panel_close(p);
}

TF_TEST(vocore_panel, a_failed_partial_falls_back_to_full_frames)
{
    fake_t f;
    vocore_transport_t t;
    vocore_panel_t *p = bring_up(&f, &t);
    TF_REQUIRE(p != NULL);

    f.bulk = malloc(FAKE_BULK_MAX);
    TF_REQUIRE(f.bulk != NULL);
    f.fail_bulk_at = 0;    /* the partial write fails, the fallback works */

    TF_CHECK(vocore_panel_show_rect(p, g_fb + 20 * 480, 0, 20, 100, 120));
    TF_CHECK(!vocore_panel_partial(p));
    TF_EQ_INT(f.bulk_len, FAKE_FRAME);

    /* the next rectangle goes out as a whole frame without another attempt */
    f.bulk_count = 0;
    f.bulk_len = 0;
    f.fail_bulk_at = -1;
    const int cmds_before = f.cmd_count;
    TF_CHECK(vocore_panel_show_rect(p, g_fb + 20 * 480, 0, 20, 100, 120));
    TF_EQ_INT(f.bulk_count, 1);
    TF_EQ_INT(f.bulk_len, FAKE_FRAME);
    TF_EQ_INT(f.cmd_count, cmds_before + 1);
    TF_EQ_INT(f.cmd_len[f.cmd_count - 1], 6);

    free(f.bulk);
    vocore_panel_close(p);
}

TF_TEST(vocore_panel, touch_reports_are_decoded_and_idle_polls_are_quiet)
{
    fake_t f;
    vocore_transport_t t;
    vocore_panel_t *p = bring_up(&f, &t);
    TF_REQUIRE(p != NULL);

    vocore_touch_t touch;
    TF_CHECK(!vocore_panel_poll_touch(p, &touch, 1));   /* nothing there yet */
    TF_EQ_INT(f.touch_count, 0);

    f.touch[2] = 1;
    f.touch[3] = 0x80 | 0x01;    /* flag drag, x high nibble 1 */
    f.touch[4] = 0x2c;           /* x = 0x12c = 300 */
    f.touch[5] = 0x01;           /* id 0, y high nibble 1 */
    f.touch[6] = 0x90;           /* y = 0x190 = 400 */
    f.touch[7] = 55;             /* weight */
    f.touch_len = 16;

    TF_CHECK(vocore_panel_poll_touch(p, &touch, 1));
    TF_EQ_INT(touch.count, 1);
    TF_EQ_INT(touch.point[0].x, 300);
    TF_EQ_INT(touch.point[0].y, 400);
    TF_EQ_INT(touch.point[0].flag, 2);
    TF_EQ_INT(touch.point[0].weight, 55);
    TF_EQ_INT(f.touch_count, 1);

    vocore_panel_close(p);
}

TF_TEST(vocore_panel, a_column_offset_rolls_whole_frames)
{
    fake_t f;
    vocore_transport_t t;
    vocore_panel_t *p = bring_up(&f, &t);
    TF_REQUIRE(p != NULL);

    /* the 4-inch glass shows column 0 in the middle: roll by half a width */
    vocore_panel_set_xshift(p, 240);
    TF_EQ_INT(vocore_panel_xshift(p), 240);

    for (int i = 0; i < 480 * 800; i++) {
        g_fb[i] = (uint16_t)(i % 4096);
    }
    f.bulk = malloc(FAKE_BULK_MAX);
    TF_REQUIRE(f.bulk != NULL);
    TF_CHECK(vocore_panel_show(p, g_fb));

    const uint16_t *sent = (const uint16_t *)f.bulk;
    for (int row = 0; row < 800; row += 97) {
        for (int x = 0; x < 480; x += 53) {
            const uint16_t want = g_fb[row * 480 + ((x - 240 + 480) % 480)];
            TF_CHECK_MSG(sent[row * 480 + x] == want,
                         "row %d col %d: got %u want %u",
                         row, x, sent[row * 480 + x], want);
        }
    }

    free(f.bulk);
    vocore_panel_close(p);
}

TF_TEST(vocore_panel, a_column_offset_splits_a_straddling_rectangle)
{
    fake_t f;
    vocore_transport_t t;
    vocore_panel_t *p = bring_up(&f, &t);
    TF_REQUIRE(p != NULL);

    vocore_panel_set_xshift(p, 240);
    static uint16_t packed[100 * 10];
    for (int i = 0; i < 100 * 10; i++) {
        packed[i] = (uint16_t)(500 + i);
    }

    /* framebuffer columns 200..299; the wrap is at 240 */
    f.bulk = malloc(FAKE_BULK_MAX);
    TF_REQUIRE(f.bulk != NULL);
    TF_CHECK(vocore_panel_show_rect(p, packed, 200, 0, 300, 10));

    /* two transfers: 200..239 to panel 440, then 240..299 to panel 0 */
    TF_CHECK(f.cmd_count >= 4);
    const uint8_t *hdr_a = f.cmd[f.cmd_count - 2];
    const uint8_t *hdr_b = f.cmd[f.cmd_count - 1];
    TF_EQ_INT(hdr_a[6] | (hdr_a[7] << 8), 440);
    TF_EQ_INT(hdr_a[10] | (hdr_a[11] << 8), 40);
    TF_EQ_INT(hdr_b[6] | (hdr_b[7] << 8), 0);
    TF_EQ_INT(hdr_b[10] | (hdr_b[11] << 8), 60);

    free(f.bulk);
    vocore_panel_close(p);
}

TF_TEST(vocore_panel, brightness_and_flip_reach_the_panel)
{
    fake_t f;
    vocore_transport_t t;
    vocore_panel_t *p = bring_up(&f, &t);
    TF_REQUIRE(p != NULL);

    const int base = f.cmd_count;
    TF_CHECK(vocore_panel_set_brightness(p, 100));
    TF_EQ_INT(f.cmd[base][1], VOCORE_CMD_BRIGHTNESS);
    TF_EQ_INT(f.cmd[base][6], 100);

    TF_CHECK(vocore_panel_set_flip(p, 3));
    TF_EQ_INT(f.cmd[base + 1][1], VOCORE_CMD_FLIP);
    TF_EQ_INT(f.cmd[base + 1][6], 3);

    vocore_panel_close(p);
}
