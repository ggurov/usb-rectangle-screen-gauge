/*
 * usb_libusb.c - libusb-1.0 loaded at runtime, on Windows.
 *
 * Why LoadLibrary instead of a normal import library:
 *
 *   - the repo needs no vendored .lib/.dll, so a plain `gcc *.c` build works;
 *   - the same DLL the VoCore driver and tools already rely on is reused
 *     (libwdi's WinUSB binding is what makes the device reachable at all);
 *   - if libusb is missing the app reports it instead of failing to start.
 *
 * Only the handful of entry points the panel needs are resolved.  The
 * structures they exchange are plain data and stable across libusb 1.x; the
 * declarations live here rather than in <libusb-1.0/libusb.h> so the build
 * does not need the SDK installed.
 */
#include "usb_libusb.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#error "the host app's runtime loader is Windows-only; link libusb normally elsewhere"
#endif

#include "vocore_proto.h"

/* -------------------------------------------------------------------------- */
/* the small corner of libusb's ABI this app uses                             */
/* -------------------------------------------------------------------------- */

typedef struct libusb_context libusb_context;
typedef struct libusb_device_handle libusb_device_handle;

struct libusb_device_descriptor {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t bcdUSB;
    uint8_t  bDeviceClass;
    uint8_t  bDeviceSubClass;
    uint8_t  bDeviceProtocol;
    uint8_t  bMaxPacketSize0;
    uint16_t idVendor;
    uint16_t idProduct;
    uint16_t bcdDevice;
    uint8_t  iManufacturer;
    uint8_t  iProduct;
    uint8_t  iSerialNumber;
    uint8_t  bNumConfigurations;
};

#define LIBUSB_SUCCESS             0
#define LIBUSB_ERROR_TIMEOUT      -7
#define LIBUSB_ERROR_NO_DEVICE    -4
#define LIBUSB_ERROR_ACCESS       -3
#define LIBUSB_ERROR_NOT_FOUND    -5
#define LIBUSB_ERROR_IO           -1

typedef int  (*fn_init)(libusb_context **ctx);
typedef void (*fn_exit)(libusb_context *ctx);
typedef libusb_device_handle *(*fn_open_vid_pid)(libusb_context *ctx, uint16_t vid, uint16_t pid);
typedef int  (*fn_claim_interface)(libusb_device_handle *dev, int interface_number);
typedef int  (*fn_release_interface)(libusb_device_handle *dev, int interface_number);
typedef void (*fn_close)(libusb_device_handle *dev);
typedef int  (*fn_control_transfer)(libusb_device_handle *dev, uint8_t request_type,
                                    uint8_t request, uint16_t value, uint16_t index,
                                    unsigned char *data, uint16_t length,
                                    unsigned int timeout);
typedef int  (*fn_bulk_transfer)(libusb_device_handle *dev, unsigned char endpoint,
                                 unsigned char *data, int length, int *transferred,
                                 unsigned int timeout);
typedef int  (*fn_interrupt_transfer)(libusb_device_handle *dev, unsigned char endpoint,
                                      unsigned char *data, int length, int *transferred,
                                      unsigned int timeout);
typedef const char *(*fn_error_name)(int errcode);
typedef int  (*fn_get_device_descriptor)(void *dev, struct libusb_device_descriptor *desc);
typedef uint8_t (*fn_get_bus_number)(void *dev);
typedef uint8_t (*fn_get_device_address)(void *dev);

/* -------------------------------------------------------------------------- */
/* loaded state                                                               */
/* -------------------------------------------------------------------------- */

typedef struct {
    libusb_context    *ctx;
    libusb_device_handle *handle;

    fn_init                init;
    fn_exit                exit;
    fn_open_vid_pid        open_vid_pid;
    fn_claim_interface     claim_interface;
    fn_release_interface   release_interface;
    fn_close               close;
    fn_control_transfer    control_transfer;
    fn_bulk_transfer       bulk_transfer;
    fn_interrupt_transfer  interrupt_transfer;
    fn_error_name          error_name;
    fn_get_device_descriptor get_descriptor;
    fn_get_bus_number      get_bus_number;
    fn_get_device_address  get_device_address;
} libusb_api_t;

static libusb_api_t s_api;
static vocore_transport_t s_transport;
static char s_dll_path[MAX_PATH];
static char s_device[64];
static bool s_open;

static const char *err_name(int code)
{
    if (s_api.error_name) {
        const char *n = s_api.error_name(code);
        if (n) {
            return n;
        }
    }
    return "error";
}

/* -------------------------------------------------------------------------- */
/* DLL loading                                                                */
/* -------------------------------------------------------------------------- */

static void copy_path(char *dst, size_t dstlen, const char *src)
{
    size_t n = strlen(src);
    if (n >= dstlen) {
        n = dstlen - 1;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static void join_path(char *out, size_t outlen, const char *dir, const char *file)
{
    const size_t filelen = strlen(file);
    size_t dirlen = strlen(dir);
    const size_t maxdir = (outlen > filelen + 2) ? outlen - filelen - 2 : 0;
    if (dirlen > maxdir) {
        dirlen = maxdir;
    }
    memcpy(out, dir, dirlen);
    out[dirlen] = '\\';
    memcpy(out + dirlen + 1, file, filelen + 1);
}

static HMODULE try_load(const char *path)
{
    if (!path || !*path) {
        return NULL;
    }
    HMODULE mod = LoadLibraryA(path);
    if (mod) {
        copy_path(s_dll_path, sizeof(s_dll_path), path);
    }
    return mod;
}

static HMODULE load_libusb(void)
{
    char exe_dir[MAX_PATH];
    char candidate[MAX_PATH];
    HMODULE mod;

    /* 1. an explicit override wins */
    if ((mod = try_load(getenv("GAUGE_LIBUSB_DLL"))) != NULL) {
        return mod;
    }

    /* 2. next to the executable, where tools/build.ps1 puts it */
    if (GetModuleFileNameA(NULL, exe_dir, sizeof(exe_dir))) {
        char *slash = strrchr(exe_dir, '\\');
        if (slash) {
            *slash = '\0';
        }
        join_path(candidate, sizeof(candidate), exe_dir, "libusb-1.0.dll");
        if ((mod = try_load(candidate)) != NULL) {
            return mod;
        }
    }

    /* 3. the normal DLL search order (current directory, PATH, system) */
    if ((mod = try_load("libusb-1.0.dll")) != NULL) {
        return mod;
    }

    /* 4. the usual MSYS2 install */
    return try_load("C:\\msys64\\mingw64\\bin\\libusb-1.0.dll");
}

static bool resolve(void *mod, const char *name, void *out, size_t out_size)
{
    FARPROC fn = GetProcAddress((HMODULE)mod, name);
    if (!fn) {
        return false;
    }
    if (out_size != sizeof(fn)) {
        return false;
    }
    memcpy(out, &fn, sizeof(fn));
    return true;
}

/* -------------------------------------------------------------------------- */
/* the transport                                                              */
/* -------------------------------------------------------------------------- */

static int ctrl_out(void *ctx, uint8_t request, const uint8_t *data, int len, int timeout_ms)
{
    (void)ctx;
    const int r = s_api.control_transfer(s_api.handle, VOCORE_REQ_OUT, request, 0, 0,
                                         (unsigned char *)data, (uint16_t)len,
                                         (unsigned int)timeout_ms);
    return (r >= 0) ? r : -1;
}

static int ctrl_in(void *ctx, uint8_t request, uint8_t *data, int len, int timeout_ms)
{
    (void)ctx;
    const int r = s_api.control_transfer(s_api.handle, VOCORE_REQ_IN, request, 0, 0,
                                         data, (uint16_t)len, (unsigned int)timeout_ms);
    return (r >= 0) ? r : -1;
}

static int bulk_out(void *ctx, const uint8_t *data, int len, int timeout_ms)
{
    (void)ctx;
    int transferred = 0;
    const int r = s_api.bulk_transfer(s_api.handle, VOCORE_EP_OUT,
                                      (unsigned char *)data, len, &transferred,
                                      (unsigned int)timeout_ms);
    if (r == LIBUSB_SUCCESS) {
        return transferred;
    }
    if (r == LIBUSB_ERROR_TIMEOUT && transferred > 0) {
        return transferred;
    }
    if (r == LIBUSB_ERROR_NO_DEVICE) {
        fprintf(stderr, "usb: the screen was unplugged\n");
    } else {
        fprintf(stderr, "usb: bulk transfer failed: %s (%d)\n", err_name(r), r);
    }
    return -1;
}

static int interrupt_in(void *ctx, uint8_t *data, int len, int timeout_ms)
{
    (void)ctx;
    int transferred = 0;
    const int r = s_api.interrupt_transfer(s_api.handle, VOCORE_EP_IN, data, len,
                                           &transferred, (unsigned int)timeout_ms);
    if (r == LIBUSB_SUCCESS) {
        return transferred;
    }
    if (r == LIBUSB_ERROR_TIMEOUT) {
        return 0;   /* idle panel: not an error */
    }
    return -1;
}

const vocore_transport_t *usb_libusb_open(char *err, int errlen)
{
    void *mod = load_libusb();
    if (!mod) {
        if (err && errlen) {
            snprintf(err, errlen, "libusb-1.0.dll not found (set GAUGE_LIBUSB_DLL)");
        }
        return NULL;
    }

    memset(&s_api, 0, sizeof(s_api));
    bool ok = resolve(mod, "libusb_init", &s_api.init, sizeof(s_api.init)) &&
              resolve(mod, "libusb_exit", &s_api.exit, sizeof(s_api.exit)) &&
              resolve(mod, "libusb_open_device_with_vid_pid", &s_api.open_vid_pid,
                      sizeof(s_api.open_vid_pid)) &&
              resolve(mod, "libusb_claim_interface", &s_api.claim_interface,
                      sizeof(s_api.claim_interface)) &&
              resolve(mod, "libusb_release_interface", &s_api.release_interface,
                      sizeof(s_api.release_interface)) &&
              resolve(mod, "libusb_close", &s_api.close, sizeof(s_api.close)) &&
              resolve(mod, "libusb_control_transfer", &s_api.control_transfer,
                      sizeof(s_api.control_transfer)) &&
              resolve(mod, "libusb_bulk_transfer", &s_api.bulk_transfer,
                      sizeof(s_api.bulk_transfer)) &&
              resolve(mod, "libusb_interrupt_transfer", &s_api.interrupt_transfer,
                      sizeof(s_api.interrupt_transfer));
    if (!ok) {
        if (err && errlen) {
            snprintf(err, errlen, "%s is not libusb-1.0 (missing entry points)", s_dll_path);
        }
        return NULL;
    }

    /* optional niceties */
    resolve(mod, "libusb_error_name", &s_api.error_name, sizeof(s_api.error_name));

    if (s_api.init(&s_api.ctx) != LIBUSB_SUCCESS) {
        if (err && errlen) {
            snprintf(err, errlen, "libusb_init failed");
        }
        return NULL;
    }

    s_api.handle = s_api.open_vid_pid(s_api.ctx, VOCORE_VID, VOCORE_PID);
    if (!s_api.handle) {
        if (err && errlen) {
            snprintf(err, errlen, "no VoCore screen (0xc872:0x1004) on the bus");
        }
        s_api.exit(s_api.ctx);
        return NULL;
    }

    const int r = s_api.claim_interface(s_api.handle, 0);
    if (r != LIBUSB_SUCCESS) {
        if (err && errlen) {
            snprintf(err, errlen, "claiming interface 0 failed: %s (%d)", err_name(r), r);
        }
        s_api.close(s_api.handle);
        s_api.exit(s_api.ctx);
        return NULL;
    }

    snprintf(s_device, sizeof(s_device), "0xc872:0x1004");
    s_transport.ctx = NULL;
    s_transport.control_out = ctrl_out;
    s_transport.control_in = ctrl_in;
    s_transport.bulk_out = bulk_out;
    s_transport.interrupt_in = interrupt_in;
    s_open = true;
    return &s_transport;
}

void usb_libusb_close(void)
{
    if (!s_open) {
        return;
    }
    s_api.release_interface(s_api.handle, 0);
    s_api.close(s_api.handle);
    s_api.exit(s_api.ctx);
    memset(&s_api, 0, sizeof(s_api));
    s_open = false;
}

const char *usb_libusb_dll_path(void)
{
    return s_dll_path[0] ? s_dll_path : "(not loaded)";
}

const char *usb_libusb_device_path(void)
{
    return s_device;
}
