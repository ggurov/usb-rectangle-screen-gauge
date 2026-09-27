/*
 * vocore_proto.h - the VoCore USB2.0 Screen's wire protocol, as pure data.
 *
 * The screen is a vendor-specific USB device (0xc872:0x1004): one interface,
 * a bulk OUT endpoint that takes whole frames, and an interrupt IN endpoint
 * that reports touch.  Everything is driven by vendor control requests.
 *
 * Nothing in here touches libusb, so the encoding and the decode are unit
 * tested on the host without a screen attached.  The protocol comes from the
 * v2scrctl SDK (source/screen_test.c and source/partest.c) and from the
 * in-kernel fbusb driver in the VoCore2 tree; both agree byte for byte.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* USB identity and endpoints. */
#define VOCORE_VID 0xC872u
#define VOCORE_PID 0x1004u

#define VOCORE_EP_OUT 0x02u   /* bulk OUT, 512-byte packets, frame data */
#define VOCORE_EP_IN  0x81u   /* interrupt IN, 64-byte packets, touch   */

/* bmRequestType for the vendor control requests. */
#define VOCORE_REQ_OUT 0x40u
#define VOCORE_REQ_IN  0xC0u

/* Vendor control requests. */
#define VOCORE_REQ_CMD   0xB0u  /* send a command / frame header           */
#define VOCORE_REQ_ADDR  0xB5u  /* register read: write the address        */
#define VOCORE_REQ_ACK   0xB6u  /* register read: one-byte acknowledge     */
#define VOCORE_REQ_DATA  0xB7u  /* register read: the payload              */
#define VOCORE_REQ_INFO  0xA0u  /* firmware info string (newer firmware)   */
#define VOCORE_REQ_INFO2 0xA1u  /* the same, for firmwares that stall 0xa0 */

/* Tags for the register-read address command, from screen_test.c. */
#define VOCORE_REG_SCREEN  0xFCu   /* panel model id      (4-byte reply)  */
#define VOCORE_REG_VERSION 0xF8u   /* panel revision      (4-byte reply)  */
#define VOCORE_REG_UID     0xF0u   /* board id            (8-byte reply)  */

/* Panel-controller commands carried inside a VOCORE_REQ_CMD transfer. */
#define VOCORE_CMD_WRITE_MEM 0x2Cu /* write frame memory                  */
#define VOCORE_CMD_SLEEP_OUT 0x29u /* wake the panel; it boots asleep     */
#define VOCORE_CMD_BRIGHTNESS 0x51u
#define VOCORE_CMD_FLIP      0x36u

/* Pixel formats accepted by VOCORE_CMD_WRITE_MEM. */
typedef enum {
    VOCORE_MODE_RGB565 = 0x00,
    VOCORE_MODE_NV12   = 0x01,
    VOCORE_MODE_MJPEG  = 0x02,
    VOCORE_MODE_LZ4    = 0x04,   /* compressed partial rectangles only */
} vocore_mode_t;

/* How the panel is mounted, from the model table. */
typedef enum {
    VOCORE_VERTICAL = 0,
    VOCORE_HORIZONTAL = 1,
    VOCORE_ROUND = 2,
} vocore_orientation_t;

/* Identification register values, or all-ones when the firmware has none. */
#define VOCORE_ID_UNKNOWN 0xFFFFFFFFu

typedef struct {
    uint32_t ver;                    /* VOCORE_REG_SCREEN                    */
    uint32_t code;                   /* VOCORE_REG_VERSION                   */
    int width;
    int height;
    int margin;                      /* leading bytes the panel expects      */
    vocore_orientation_t orientation;
    bool known;                      /* false for firmware that reports none */
    char model[64];                  /* human-readable, never empty          */
} vocore_id_t;

/*
 * Fills `out` from the two registers and the optional info string ('\0' or
 * a string starting with 'v' from VOCORE_REQ_INFO).  Unknown register values
 * fall back to the SDK's own default: a 480x854 vertical panel, no margin.
 */
void vocore_id_from_regs(uint32_t ver, uint32_t code, const char *info,
                         vocore_id_t *out);

/* -------------------------------------------------------------------------- */
/* command builders                                                           */
/* -------------------------------------------------------------------------- */

/*
 * Whole-frame header: {mode, 0x2c, bytes[3], 0x00}, 6 bytes.  The frame data
 * follows on the bulk endpoint, `bytes` of it.
 */
int vocore_cmd_full_frame(vocore_mode_t mode, uint32_t bytes, uint8_t *out);

/*
 * Partial rectangle: {mode, 0x2c, bytes[4], x, y, w, h}.  The SDK sends 12
 * bytes for raw RGB565 (the height is derivable from bytes/w and is left
 * off) and all 14 for compressed data, where it has to be explicit.
 */
int vocore_cmd_partial_frame(vocore_mode_t mode, uint32_t bytes,
                             int x, int y, int w, int h, uint8_t *out);

/* Sleep out, 6 bytes.  Nothing is visible until this has been sent once. */
int vocore_cmd_wake(uint8_t *out);

/* Backlight, 0..100 -> 8 bytes. */
int vocore_cmd_brightness(int percent, uint8_t *out);

/* Flip/mirror, 0..3, applied to most panels; 8 bytes. */
int vocore_cmd_flip(int mode, uint8_t *out);

/* Register-read address command, 5 bytes; tag is one of VOCORE_REG_*. */
int vocore_cmd_reg_addr(uint8_t tag, int payload_len, uint8_t *out);

/*
 * Register reply length: one acknowledge byte plus the payload.  The caller
 * issues VOCORE_REQ_DATA with this length and reads the value at offset 1.
 */
int vocore_reg_reply_len(int payload_len);

/* Little-endian 32-bit register value out of a VOCORE_REQ_DATA reply. */
uint32_t vocore_reg_value(const uint8_t *reply, int len);

/* -------------------------------------------------------------------------- */
/* touch                                                                      */
/* -------------------------------------------------------------------------- */

typedef struct {
    int id;
    int flag;      /* 0 press, 1 hover, 2 drag */
    int x;
    int y;
    int weight;
} vocore_point_t;

typedef struct {
    int count;                  /* points present in this report, 0..2 */
    vocore_point_t point[2];
} vocore_touch_t;

/*
 * Decodes one interrupt-IN report.  Returns false when the report is short or
 * carries no point, so an idle endpoint can be polled cheaply.
 */
bool vocore_touch_decode(const uint8_t *buf, int len, vocore_touch_t *out);

#ifdef __cplusplus
}
#endif
