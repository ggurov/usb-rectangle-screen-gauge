/*
 * vocore_proto.c - encoding and decoding for the VoCore screen protocol.
 *
 * Kept free of USB and of any OS so the whole conversation can be asserted
 * byte for byte in the host tests.  See vocore_proto.h for the provenance of
 * every constant.
 */
#include "vocore_proto.h"

#include <stdio.h>
#include <string.h>

/* -------------------------------------------------------------------------- */
/* command builders                                                           */
/* -------------------------------------------------------------------------- */

static void put_le16(uint8_t *p, int v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
}

static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

int vocore_cmd_full_frame(vocore_mode_t mode, uint32_t bytes, uint8_t *out)
{
    out[0] = (uint8_t)mode;
    out[1] = VOCORE_CMD_WRITE_MEM;
    out[2] = (uint8_t)(bytes & 0xFF);
    out[3] = (uint8_t)((bytes >> 8) & 0xFF);
    out[4] = (uint8_t)((bytes >> 16) & 0xFF);
    out[5] = 0x00;
    return 6;
}

int vocore_cmd_partial_frame(vocore_mode_t mode, uint32_t bytes,
                             int x, int y, int w, int h, uint8_t *out)
{
    out[0] = (uint8_t)mode;
    out[1] = VOCORE_CMD_WRITE_MEM;
    put_le32(&out[2], bytes);
    put_le16(&out[6], x);
    put_le16(&out[8], y);
    put_le16(&out[10], w);
    put_le16(&out[12], h);

    /*
     * Raw RGB565 goes out as 12 bytes: the panel derives the height from the
     * byte count and the width.  The SDK's LZ4 path sends all 14 because a
     * compressed size says nothing about rows.
     */
    return (mode == VOCORE_MODE_RGB565) ? 12 : 14;
}

int vocore_cmd_wake(uint8_t *out)
{
    out[0] = 0x00;
    out[1] = VOCORE_CMD_SLEEP_OUT;
    out[2] = 0x00;
    out[3] = 0x00;
    out[4] = 0x00;
    out[5] = 0x00;
    return 6;
}

int vocore_cmd_brightness(int percent, uint8_t *out)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    out[0] = 0x00;
    out[1] = VOCORE_CMD_BRIGHTNESS;
    out[2] = 0x02;
    out[3] = 0x00;
    out[4] = 0x00;
    out[5] = 0x00;
    out[6] = (uint8_t)percent;
    out[7] = 0x00;
    return 8;
}

int vocore_cmd_flip(int mode, uint8_t *out)
{
    out[0] = 0x00;
    out[1] = VOCORE_CMD_FLIP;
    out[2] = 0x02;
    out[3] = 0x00;
    out[4] = 0x00;
    out[5] = 0x00;
    out[6] = (uint8_t)(mode & 0xFF);
    out[7] = (uint8_t)(mode & 0xFF);
    return 8;
}

int vocore_cmd_reg_addr(uint8_t tag, int payload_len, uint8_t *out)
{
    out[0] = 0x51;
    out[1] = 0x02;
    out[2] = (uint8_t)payload_len;
    out[3] = 0x1F;
    out[4] = tag;
    return 5;
}

int vocore_reg_reply_len(int payload_len)
{
    return 1 + payload_len;
}

uint32_t vocore_reg_value(const uint8_t *reply, int len)
{
    if (!reply || len < 5) {
        return VOCORE_ID_UNKNOWN;
    }
    return (uint32_t)reply[1]
         | ((uint32_t)reply[2] << 8)
         | ((uint32_t)reply[3] << 16)
         | ((uint32_t)reply[4] << 24);
}

/* -------------------------------------------------------------------------- */
/* identification                                                             */
/* -------------------------------------------------------------------------- */

/*
 * The model table is the SDK's own, including the panel revisions that only
 * differ in the glass part number.  `margin` is the 320-byte prefix the
 * SLM5.0 5-inch panel wants; every other panel wants none.
 *
 * The fallback for firmware that reports nothing is 480x800, not the SDK's
 * 480x854: the screen this project was ported to is a 4-inch/4.3-inch era
 * panel whose registers read back all ones, and it was measured at 480x800
 * (see docs/usb-screen.md).  `--size` overrides either way.
 */
static void identify(uint32_t ver, uint32_t code, vocore_id_t *out)
{
    out->ver = ver;
    out->code = code;
    out->known = (ver != VOCORE_ID_UNKNOWN);
    out->margin = 0;
    out->orientation = VOCORE_VERTICAL;
    out->width = 480;
    out->height = 800;

    switch (ver) {
    case 0x00000005:   /* 5 inch, 480x854 */
        out->width = 480;
        out->height = 854;
        out->margin = (code == 0x00000003) ? 0 : 320;
        snprintf(out->model, sizeof(out->model), "5inch 480x854 %s",
                 code == VOCORE_ID_UNKNOWN ? "SLM5.0-81FPC-A"
                 : code == 0x00000000  ? "D500FPC9373-C"
                 : code == 0x00000003  ? "D500FPC931A-A"
                                       : "unknown glass");
        break;

    case 0x00001005:   /* 5 inch OLED */
        out->width = 720;
        out->height = 1280;
        snprintf(out->model, sizeof(out->model), "5inch OLED 720x1280");
        break;

    case 0x00000304:   /* 4.3 inch */
        out->width = 480;
        out->height = 800;
        snprintf(out->model, sizeof(out->model), "4.3inch 480x800 D430FPC9316-A");
        break;

    case 0x00000004:   /* 4 inch */
        out->width = 480;
        out->height = 800;
        snprintf(out->model, sizeof(out->model), "4inch 480x800 %s",
                 code == 0x00000002 ? "VOCORE-4NDNV7B"
                 : code == 0x00000007 ? "VOCORE-4INRGB"
                                      : "TOSHIBA-2122");
        break;

    case 0x00000b04:   /* 4 inch, D397 glass */
        out->width = 480;
        out->height = 800;
        snprintf(out->model, sizeof(out->model), "4inch 480x800 D397FPC9367-B");
        break;

    case 0x00000104:   /* 4 inch, several glasses */
        out->width = 480;
        out->height = 800;
        snprintf(out->model, sizeof(out->model), "4inch 480x800 %s",
                 code == VOCORE_ID_UNKNOWN ? "DJN-1922"
                 : code == 0x00000001  ? "SLM4.0-33FPC-A"
                 : code == 0x00000002  ? "VOCORE-4NDNV10B"
                                       : "unknown glass");
        break;

    case 0x00000007:   /* 6.8 inch, mounted horizontally */
        out->width = 800;
        out->height = 480;
        out->orientation = VOCORE_HORIZONTAL;
        snprintf(out->model, sizeof(out->model), "6.8inch 800x480 D680FPC930G-A");
        break;

    case 0x00000403:   /* 3.4 inch round */
        out->width = 800;
        out->height = 800;
        out->orientation = VOCORE_ROUND;
        snprintf(out->model, sizeof(out->model), "3.4inch round 800x800 MPRO");
        break;

    case 0x0000000a:   /* 10 inch */
        out->width = 1024;
        out->height = 600;
        out->orientation = VOCORE_HORIZONTAL;
        snprintf(out->model, sizeof(out->model), "10inch 1024x600 MPRO");
        break;

    case 0x00000807:   /* 7.85 inch long strip */
        out->width = 1280;
        out->height = 400;
        out->orientation = VOCORE_HORIZONTAL;
        snprintf(out->model, sizeof(out->model), "7.85inch 1280x400 MPRO");
        break;

    default:
        out->known = false;
        snprintf(out->model, sizeof(out->model), "unknown panel (ver %08x code %08x)",
                 (unsigned)ver, (unsigned)code);
        break;
    }
}

void vocore_id_from_regs(uint32_t ver, uint32_t code, const char *info,
                         vocore_id_t *out)
{
    if (!out) {
        return;
    }
    identify(ver, code, out);

    if (info && info[0] == 'v') {
        snprintf(out->model, sizeof(out->model), "%s", info);
    }
}

/* -------------------------------------------------------------------------- */
/* touch                                                                      */
/* -------------------------------------------------------------------------- */

/* Point records start at offset 3, six bytes each:
 *   [0] x high nibble | u:2 | flag:2      [1] x low byte
 *   [2] y high nibble | id:4              [3] y low byte
 *   [4] weight                            [5] misc
 */
static void decode_point(const uint8_t *p, vocore_point_t *out)
{
    out->x = ((p[0] & 0x0F) << 8) | p[1];
    out->y = ((p[2] & 0x0F) << 8) | p[3];
    out->flag = (p[0] >> 6) & 0x03;
    out->id = (p[2] >> 4) & 0x0F;
    out->weight = p[4];
}

bool vocore_touch_decode(const uint8_t *buf, int len, vocore_touch_t *out)
{
    if (!buf || !out || len < 3) {
        return false;
    }
    memset(out, 0, sizeof(*out));

    const int count = buf[2];
    for (int i = 0; i < 2; i++) {
        const int offset = 3 + i * 6;
        if (offset + 6 > len) {
            break;
        }
        decode_point(&buf[offset], &out->point[i]);
        out->count = i + 1;
    }
    if (count > 0 && count < out->count) {
        out->count = count;
    }
    if (count == 0) {
        out->count = 0;
    }
    return out->count > 0;
}
