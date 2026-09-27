/*
 * vocore_transport.h - the four USB transfers the screen protocol needs.
 *
 * The panel code talks to a transport instead of libusb directly, which is
 * what lets tests/unit/test_vocore_panel.c hold the whole conversation against
 * a fake and assert the exact bytes.  usb_libusb.c is the real one.
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    void *ctx;

    /* Vendor control OUT, e.g. VOCORE_REQ_CMD.  Returns bytes sent or < 0. */
    int (*control_out)(void *ctx, uint8_t request, const uint8_t *data,
                       int len, int timeout_ms);

    /* Vendor control IN, e.g. VOCORE_REQ_DATA.  Returns bytes read or < 0. */
    int (*control_in)(void *ctx, uint8_t request, uint8_t *data,
                      int len, int timeout_ms);

    /* Bulk OUT on VOCORE_EP_OUT.  Returns bytes sent or < 0. */
    int (*bulk_out)(void *ctx, const uint8_t *data, int len, int timeout_ms);

    /*
     * Interrupt IN on VOCORE_EP_IN.  Returns bytes read or < 0; a timeout is
     * reported as 0 so polling an idle panel is not an error.
     */
    int (*interrupt_in)(void *ctx, uint8_t *data, int len, int timeout_ms);
} vocore_transport_t;

#ifdef __cplusplus
}
#endif
