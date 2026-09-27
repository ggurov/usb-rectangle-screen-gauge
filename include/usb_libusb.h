/*
 * usb_libusb.h - the real transport: libusb-1.0.dll, loaded at runtime.
 *
 * The DLL is not linked at build time.  It is opened with LoadLibrary, which
 * means the only thing this project needs to build is a C compiler, and the
 * only thing it needs to run is a libusb DLL anywhere on the search path -
 * see usb_libusb_open() for where it looks.
 */
#pragma once

#include <stdbool.h>

#include "vocore_transport.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Opens the first VoCore screen (0xc872:0x1004) and claims its interface.
 * Returns NULL and fills `err` when the DLL or the screen is missing.
 */
const vocore_transport_t *usb_libusb_open(char *err, int errlen);

void usb_libusb_close(void);

/* Loaded DLL path and device address, for the console.  Never NULL. */
const char *usb_libusb_dll_path(void);
const char *usb_libusb_device_path(void);

#ifdef __cplusplus
}
#endif
