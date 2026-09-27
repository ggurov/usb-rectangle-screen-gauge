/*
 * test_vocore_proto.c - the USB screen protocol, asserted byte for byte.
 *
 * These are the exact sequences the v2scrctl SDK sends, so a regression here
 * means the panel would receive something it has never been tested with.
 */
#include <string.h>

#include "test_framework.h"
#include "vocore_proto.h"

static void expect_bytes(const uint8_t *got, const uint8_t *want, int n, const char *what)
{
    for (int i = 0; i < n; i++) {
        TF_CHECK_MSG(got[i] == want[i], "%s: byte %d is 0x%02x, want 0x%02x",
                     what, i, got[i], want[i]);
    }
}

TF_TEST(vocore_proto, full_frame_header)
{
    uint8_t out[16];

    /* 480 x 854 x 2 = 819840 = 0x0c8280 */
    const int n = vocore_cmd_full_frame(VOCORE_MODE_RGB565, 819840u, out);
    TF_EQ_INT(n, 6);
    const uint8_t want[] = {0x00, 0x2c, 0x80, 0x82, 0x0c, 0x00};
    expect_bytes(out, want, 6, "full RGB565 480x854");
}

TF_TEST(vocore_proto, partial_frame_raw_omits_the_height)
{
    uint8_t out[16];

    /* a 480 x 100 rectangle: 96000 = 0x017700 */
    const int n = vocore_cmd_partial_frame(VOCORE_MODE_RGB565, 96000u, 0, 100, 480, 100, out);
    TF_EQ_INT(n, 12);
    const uint8_t want[] = {
        0x00, 0x2c,
        0x00, 0x77, 0x01, 0x00,
        0x00, 0x00,         /* x = 0    */
        0x64, 0x00,         /* y = 100  */
        0xe0, 0x01,         /* w = 480  */
    };
    expect_bytes(out, want, 12, "partial RGB565");

    /* the same rectangle compressed carries the height explicitly */
    const int m = vocore_cmd_partial_frame(VOCORE_MODE_LZ4, 96000u, 0, 100, 480, 100, out);
    TF_EQ_INT(m, 14);
    TF_EQ_INT(out[12], 0x64);
    TF_EQ_INT(out[13], 0x00);
}

TF_TEST(vocore_proto, wake_sleep_out)
{
    uint8_t out[16];
    const int n = vocore_cmd_wake(out);
    TF_EQ_INT(n, 6);
    const uint8_t want[] = {0x00, 0x29, 0x00, 0x00, 0x00, 0x00};
    expect_bytes(out, want, 6, "wake");
}

TF_TEST(vocore_proto, brightness_is_clamped_and_lands_in_byte_6)
{
    uint8_t out[16];

    TF_EQ_INT(vocore_cmd_brightness(60, out), 8);
    const uint8_t want[] = {0x00, 0x51, 0x02, 0x00, 0x00, 0x00, 60, 0x00};
    expect_bytes(out, want, 8, "brightness 60");

    vocore_cmd_brightness(-10, out);
    TF_EQ_INT(out[6], 0);
    vocore_cmd_brightness(1000, out);
    TF_EQ_INT(out[6], 100);
}

TF_TEST(vocore_proto, flip_writes_both_axis_bytes)
{
    uint8_t out[16];
    TF_EQ_INT(vocore_cmd_flip(2, out), 8);
    TF_EQ_INT(out[1], 0x36);
    TF_EQ_INT(out[6], 2);
    TF_EQ_INT(out[7], 2);
}

TF_TEST(vocore_proto, register_read_sequence)
{
    uint8_t out[16];

    TF_EQ_INT(vocore_cmd_reg_addr(VOCORE_REG_SCREEN, 4, out), 5);
    const uint8_t want[] = {0x51, 0x02, 0x04, 0x1f, 0xfc};
    expect_bytes(out, want, 5, "screen register address");

    TF_EQ_INT(vocore_reg_reply_len(4), 5);
    TF_EQ_INT(vocore_reg_reply_len(8), 9);

    const uint8_t reply[] = {0x01, 0xff, 0xff, 0xff, 0xff};
    TF_EQ_INT(vocore_reg_value(reply, 5), VOCORE_ID_UNKNOWN);

    const uint8_t reply2[] = {0x01, 0x04, 0x00, 0x00, 0x00};
    TF_EQ_INT(vocore_reg_value(reply2, 5), 4);
}

TF_TEST(vocore_proto, known_panels_map_to_their_geometry)
{
    vocore_id_t id;

    /* the 5 inch 480x854 glass wants the 320-byte prefix */
    vocore_id_from_regs(0x00000005, VOCORE_ID_UNKNOWN, NULL, &id);
    TF_CHECK(id.known);
    TF_EQ_INT(id.width, 480);
    TF_EQ_INT(id.height, 854);
    TF_EQ_INT(id.margin, 320);
    TF_EQ_INT(id.orientation, VOCORE_VERTICAL);

    /* ...except the D500FPC931A-A revision */
    vocore_id_from_regs(0x00000005, 0x00000003, NULL, &id);
    TF_EQ_INT(id.margin, 0);

    /* the 6.8 inch is mounted sideways */
    vocore_id_from_regs(0x00000007, 0, NULL, &id);
    TF_CHECK(id.known);
    TF_EQ_INT(id.width, 800);
    TF_EQ_INT(id.height, 480);
    TF_EQ_INT(id.orientation, VOCORE_HORIZONTAL);

    /* the 10 inch and the round 3.4 inch */
    vocore_id_from_regs(0x0000000a, 0, NULL, &id);
    TF_EQ_INT(id.width, 1024);
    TF_EQ_INT(id.height, 600);
    vocore_id_from_regs(0x00000403, 0, NULL, &id);
    TF_EQ_INT(id.orientation, VOCORE_ROUND);
}

TF_TEST(vocore_proto, unknown_firmware_falls_back_to_the_measured_panel)
{
    vocore_id_t id;
    vocore_id_from_regs(VOCORE_ID_UNKNOWN, VOCORE_ID_UNKNOWN, NULL, &id);
    TF_CHECK(!id.known);
    TF_EQ_INT(id.width, 480);
    TF_EQ_INT(id.height, 800);
    TF_EQ_INT(id.margin, 0);
    TF_NOT_NULL(strstr(id.model, "unknown"));
}

TF_TEST(vocore_proto, firmware_info_string_wins_over_the_table)
{
    vocore_id_t id;
    vocore_id_from_regs(0x00000005, VOCORE_ID_UNKNOWN, "v0.25 5inch", &id);
    TF_STR_EQ(id.model, "v0.25 5inch");
    TF_EQ_INT(id.width, 480);
    TF_EQ_INT(id.height, 854);
}

/* -------------------------------------------------------------------------- */
/* touch                                                                      */
/* -------------------------------------------------------------------------- */

static void put_point(uint8_t *buf, int slot, int flag, int id, int x, int y, int weight)
{
    uint8_t *p = &buf[3 + slot * 6];
    p[0] = (uint8_t)(((flag & 3) << 6) | ((x >> 8) & 0x0F));
    p[1] = (uint8_t)(x & 0xFF);
    p[2] = (uint8_t)(((id & 0x0F) << 4) | ((y >> 8) & 0x0F));
    p[3] = (uint8_t)(y & 0xFF);
    p[4] = (uint8_t)weight;
    p[5] = 0;
}

TF_TEST(vocore_proto, touch_report_decodes_both_points)
{
    uint8_t buf[64] = {0};
    buf[2] = 2;
    put_point(buf, 0, 2, 0, 123, 456, 77);
    put_point(buf, 1, 0, 1, 479, 853, 12);

    vocore_touch_t t;
    TF_CHECK(vocore_touch_decode(buf, sizeof(buf), &t));
    TF_EQ_INT(t.count, 2);
    TF_EQ_INT(t.point[0].x, 123);
    TF_EQ_INT(t.point[0].y, 456);
    TF_EQ_INT(t.point[0].flag, 2);
    TF_EQ_INT(t.point[0].id, 0);
    TF_EQ_INT(t.point[0].weight, 77);
    TF_EQ_INT(t.point[1].x, 479);
    TF_EQ_INT(t.point[1].y, 853);
    TF_EQ_INT(t.point[1].id, 1);
}

TF_TEST(vocore_proto, touch_report_with_no_points_is_rejected)
{
    uint8_t buf[64] = {0};
    vocore_touch_t t;

    TF_CHECK(!vocore_touch_decode(buf, sizeof(buf), &t));
    TF_CHECK(!vocore_touch_decode(buf, 2, &t));
    TF_CHECK(!vocore_touch_decode(NULL, 64, &t));
}
