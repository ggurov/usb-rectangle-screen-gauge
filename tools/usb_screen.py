#!/usr/bin/env python3
"""Bring-up tool for the VoCore USB2.0 Screen.

The screen is the vendor-specific USB device 0xc872:0x1004: one interface, a
bulk OUT endpoint that takes whole frames, and an interrupt IN endpoint that
reports touch.  Everything is driven by vendor control requests; the protocol
is the one in the v2scrctl SDK (source/screen_test.c) and in the kernel fbusb
driver, and is mirrored in C by src/vocore_proto.c.

Needs pyusb and a libusb DLL; libusb-package supplies the latter:

    python -m pip install -r tools/requirements.txt
    python tools/usb_screen.py info
    python tools/usb_screen.py test            # wake, paint a pattern, watch touch
    python tools/usb_screen.py show quad --size 480x854
    python tools/usb_screen.py brightness 80
    python tools/usb_screen.py touch 30

The panel boots asleep: nothing is visible until `info` or `test` has sent
sleep-out at least once (the panel keeps the frame, the wake does not stick
across a power cycle).
"""
from __future__ import annotations

import argparse
import struct
import sys
import time

try:
    import usb.core
    import usb.util
except ImportError:  # pragma: no cover
    sys.exit("pyusb is missing: python -m pip install pyusb libusb-package")

try:
    import libusb_package
except ImportError:  # pragma: no cover
    libusb_package = None

VID, PID = 0xC872, 0x1004
EP_OUT, EP_IN = 0x02, 0x81

REQ_CMD, REQ_ADDR, REQ_ACK, REQ_DATA = 0xB0, 0xB5, 0xB6, 0xB7
REQ_INFO, REQ_INFO2 = 0xA0, 0xA1

REG_SCREEN, REG_VERSION, REG_UID = 0xFC, 0xF8, 0xF0

MODE_RGB565, MODE_NV12, MODE_MJPEG = 0x00, 0x01, 0x02

CMD_WRITE_MEM, CMD_SLEEP_OUT = 0x2C, 0x29
CMD_BRIGHTNESS, CMD_FLIP = 0x51, 0x36

DEFAULT_SIZE = (480, 800)   # measured; the SDK's 480x854 is the 5-inch glass

# Panel model table from screen_test.c: ver -> (w, h, margin, name).
MODELS = {
    0x00000005: (480, 854, 320, "5inch 480x854"),
    0x00001005: (720, 1280, 0, "5inch OLED 720x1280"),
    0x00000304: (480, 800, 0, "4.3inch 480x800"),
    0x00000004: (480, 800, 0, "4inch 480x800"),
    0x00000B04: (480, 800, 0, "4inch 480x800"),
    0x00000104: (480, 800, 0, "4inch 480x800"),
    0x00000007: (800, 480, 0, "6.8inch 800x480"),
    0x00000403: (800, 800, 0, "3.4inch round 800x800"),
    0x0000000A: (1024, 600, 0, "10inch 1024x600"),
    0x00000807: (1280, 400, 0, "7.85inch 1280x400"),
}


# ---------------------------------------------------------------------------
# the device
# ---------------------------------------------------------------------------

def open_screen():
    backend = libusb_package.get_libusb1_backend() if libusb_package else None
    dev = usb.core.find(idVendor=VID, idProduct=PID, backend=backend)
    if dev is None:
        raise SystemExit("no VoCore screen (0xc872:0x1004) found")
    try:
        dev.set_configuration()
    except usb.core.USBError:
        pass
    usb.util.claim_interface(dev, 0)
    return dev


def send_cmd(dev, payload: bytes) -> None:
    dev.ctrl_transfer(0x40, REQ_CMD, 0, 0, payload, timeout=2000)


def read_reg(dev, tag: int, length: int) -> int | None:
    try:
        dev.ctrl_transfer(0x40, REQ_ADDR, 0, 0,
                          bytes([0x51, 0x02, length, 0x1F, tag]), timeout=1000)
        dev.ctrl_transfer(0xC0, REQ_ACK, 0, 0, 1, timeout=1000)
        reply = bytes(dev.ctrl_transfer(0xC0, REQ_DATA, 0, 0, length + 1, timeout=1000))
    except usb.core.USBError:
        return None
    if len(reply) < 5:
        return None
    return struct.unpack_from("<i", reply, 1)[0] & 0xFFFFFFFF


def read_info(dev) -> str | None:
    for request in (REQ_INFO, REQ_INFO2):
        try:
            buf = bytes(dev.ctrl_transfer(0xC0, request, 0, 0, 64, timeout=1000))
        except usb.core.USBError:
            continue
        if buf[:1] == b"v":
            return buf.split(b"\x00")[0].decode(errors="replace")
    return None


def read_uid(dev) -> str:
    try:
        dev.ctrl_transfer(0x40, REQ_ADDR, 0, 0, bytes([0x51, 0x02, 0x08, 0x1F, REG_UID]),
                          timeout=1000)
        dev.ctrl_transfer(0xC0, REQ_ACK, 0, 0, 1, timeout=1000)
        buf = bytes(dev.ctrl_transfer(0xC0, REQ_DATA, 0, 0, 9, timeout=1000))
    except usb.core.USBError:
        return "?"
    return buf[1:].hex("-", 2) if len(buf) >= 9 else "?"


def identify(dev) -> tuple[int, int, int, int, int, str]:
    """Returns (w, h, margin, ver, code, model)."""
    info = read_info(dev)
    ver = read_reg(dev, REG_SCREEN, 4)
    code = read_reg(dev, REG_VERSION, 4)
    if ver is None or ver == 0xFFFFFFFF or ver not in MODELS:
        w, h, margin, model = (*DEFAULT_SIZE, 0, "unknown (measured 480x800)")
    else:
        w, h, margin, model = MODELS[ver]
    if info:
        model = f"{model} [{info}]"
    return w, h, margin, ver if ver is not None else 0xFFFFFFFF, \
        code if code is not None else 0xFFFFFFFF, model


# ---------------------------------------------------------------------------
# frames
# ---------------------------------------------------------------------------

def rgb565(r: int, g: int, b: int) -> int:
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def pattern(w: int, h: int, name: str) -> bytes:
    """A test picture, RGB565 little endian, row major."""
    buf = bytearray(w * h * 2)

    def px(x: int, y: int, c: int) -> None:
        if 0 <= x < w and 0 <= y < h:
            struct.pack_into("<H", buf, (y * w + x) * 2, c)

    def fill(x0: int, y0: int, x1: int, y1: int, c: int) -> None:
        for y in range(max(0, y0), min(h, y1)):
            off = (y * w + max(0, x0)) * 2
            buf[off:off + (min(w, x1) - max(0, x0)) * 2] = struct.pack("<H", c) * (min(w, x1) - max(0, x0))

    white, black, red, green, blue, yellow = (
        rgb565(255, 255, 255), rgb565(0, 0, 0), rgb565(255, 0, 0),
        rgb565(0, 255, 0), rgb565(0, 0, 255), rgb565(255, 255, 0))

    if name == "white":
        return struct.pack("<H", white) * (w * h)

    if name == "quad":
        fill(0, 0, w // 2, h // 2, red)
        fill(w // 2, 0, w, h // 2, green)
        fill(0, h // 2, w // 2, h, blue)
        fill(w // 2, h // 2, w, h, yellow)
    elif name == "bars":
        for i in range(8):
            fill(i * w // 8, 0, (i + 1) * w // 8, h,
                 (red, green, blue, white, yellow, rgb565(0, 255, 255),
                  rgb565(255, 0, 255), black)[i])
    elif name == "grid":
        for x in range(0, w, 16):
            for y in range(h):
                px(x, y, rgb565(0, 90, 0))
        for y in range(0, h, 16):
            for x in range(w):
                px(x, y, rgb565(0, 90, 0))
    elif name == "border":
        pass
    else:
        raise SystemExit(f"unknown pattern '{name}'")

    # Every pattern gets a frame inside the frame: 4 px in from each edge.
    # If the panel is shorter than the buffer the extra rows wrap, and the
    # second top border tells you so.
    for x in range(w):
        for d in range(4):
            px(x, d, white)
            px(x, h - 1 - d, white)
    for y in range(h):
        for d in range(4):
            px(d, y, white)
            px(w - 1 - d, y, white)

    # A ruler on the centre row and a 54 px marker above the bottom edge,
    # which is exactly the difference between the 800 and the 854 panels.
    for y in (h // 2, h - 55, h - 27):
        for x in range(0, w, 4):
            px(x, y, red)
    for x in range(0, w, 8):
        px(x, 4, blue)

    return bytes(buf)


def load_frame(path: str, w: int, h: int) -> bytes:
    """Accepts a raw RGB565 dump or the SDK's 138-byte-header .bmp files."""
    with open(path, "rb") as f:
        data = f.read()
    if data[:2] == b"BM":
        data = data[138:]
    want = w * h * 2
    if len(data) < want:
        raise SystemExit(f"{path} holds {len(data)} bytes, need {want} for {w}x{h}")
    return data[:want]


def push_frame(dev, buf: bytes) -> None:
    """Whole frames only.

    The panel's partial-rectangle command is deliberately not exposed here.
    This firmware accepts it, draws nothing, and advances its frame pointer by
    the rectangle's size -- so the *next* whole frame comes out shifted.  That
    is what produced the split dial during bring-up; see docs/usb-screen.md.
    """
    size = len(buf)
    cmd = bytes([MODE_RGB565, CMD_WRITE_MEM, size & 0xFF, (size >> 8) & 0xFF,
                 (size >> 16) & 0xFF, 0x00])
    send_cmd(dev, cmd)
    dev.write(EP_OUT, buf, timeout=5000)


# ---------------------------------------------------------------------------
# touch
# ---------------------------------------------------------------------------

def decode_touch(buf: bytes):
    points = []
    for i in range(2):
        o = 3 + i * 6
        if o + 6 > len(buf):
            break
        x = ((buf[o] & 0x0F) << 8) | buf[o + 1]
        y = ((buf[o + 2] & 0x0F) << 8) | buf[o + 3]
        points.append((i, (buf[o] >> 6) & 3, x, y, buf[o + 4]))
    return points


def watch_touch(dev, seconds: float) -> None:
    flags = ("PRESS", "HOVER", "DRAG")
    print(f"touch: watching for {seconds:.0f}s - drag a finger across the panel")
    lo = [9999, 9999]
    hi = [-1, -1]
    end = time.time() + seconds
    while time.time() < end:
        try:
            buf = bytes(dev.read(EP_IN, 128, timeout=1000))
        except usb.core.USBError as e:
            if getattr(e, "errno", None) in (110, 10060) or "timed out" in str(e).lower():
                continue
            print(f"touch read failed: {e}")
            return
        for i, flag, x, y, weight in decode_touch(buf):
            print(f"  id{i} {flags[flag]} ({x},{y}) w{weight}", flush=True)
            lo[0], hi[0] = min(lo[0], x), max(hi[0], x)
            lo[1], hi[1] = min(lo[1], y), max(hi[1], y)
    print(f"touch: raw range x {lo[0]}..{hi[0]}  y {lo[1]}..{hi[1]}")
    if hi[0] >= 0:
        print("touch: the panel reports pixel coordinates, so the maxima above")
        print("       are its own width and height (plus a little edge margin).")


# ---------------------------------------------------------------------------
# commands
# ---------------------------------------------------------------------------

def cmd_info(args, dev) -> int:
    w, h, margin, ver, code, model = identify(dev)
    print(f"model      : {model}")
    print(f"registers  : screen 0x{ver:08x}  version 0x{code:08x}")
    print(f"uid        : {read_uid(dev)}")
    print(f"geometry   : {w}x{h}  margin {margin}")
    print(f"usb        : bus {dev.bus} address {dev.address}  bcdDevice {dev.bcdDevice:04x}")
    return 0


def cmd_wake(args, dev) -> int:
    send_cmd(dev, bytes([0x00, CMD_SLEEP_OUT, 0, 0, 0, 0]))
    print("sleep-out sent")
    return 0


def cmd_brightness(args, dev) -> int:
    value = max(0, min(100, args.value))
    send_cmd(dev, bytes([0x00, CMD_BRIGHTNESS, 0x02, 0, 0, 0, value, 0]))
    print(f"brightness -> {value}")
    return 0


def cmd_flip(args, dev) -> int:
    """Kept off the CLI on purpose.

    On this unit the SDK's flip command does not merely mirror: it changes the
    display's addressing, and none of the four modes brings the power-on state
    back (a power cycle does).  Only use this if you are ready to re-measure
    the orientation; see docs/usb-screen.md.
    """
    value = args.value & 3
    send_cmd(dev, bytes([0x00, CMD_FLIP, 0x02, 0, 0, 0, value, value]))
    print(f"flip -> {value}")
    return 0


def cmd_show(args, dev) -> int:
    auto_w, auto_h, _margin, _ver, _code, model = identify(dev)
    if args.size:
        w, h = (int(v) for v in args.size.lower().split("x"))
    else:
        w, h = auto_w, auto_h
        print(f"using detected geometry {w}x{h} ({model})")

    send_cmd(dev, bytes([0x00, CMD_SLEEP_OUT, 0, 0, 0, 0]))
    if args.brightness is not None:
        cmd_brightness(argparse.Namespace(value=args.brightness), dev)

    frame = load_frame(args.file, w, h) if args.file else pattern(w, h, args.pattern)
    push_frame(dev, frame)
    print(f"sent {len(frame)} bytes ({w}x{h} RGB565)")

    if args.keep:
        watch_touch(dev, args.keep)
    return 0


def cmd_touch(args, dev) -> int:
    send_cmd(dev, bytes([0x00, CMD_SLEEP_OUT, 0, 0, 0, 0]))
    watch_touch(dev, args.seconds)
    return 0


def cmd_test(args, dev) -> int:
    w, h, _margin, _ver, _code, model = identify(dev)
    print(f"panel: {model} ({w}x{h})")
    send_cmd(dev, bytes([0x00, CMD_SLEEP_OUT, 0, 0, 0, 0]))
    send_cmd(dev, bytes([0x00, CMD_BRIGHTNESS, 0x02, 0, 0, 0, 80, 0]))
    for name in ("white", "bars", "quad"):
        push_frame(dev, pattern(w, h, name))
        print(f"painted {name}, 2 s")
        time.sleep(2)
    print("the last pattern stays up:")
    print("  four quadrants (red/green/blue/yellow), white border 4 px in from")
    print(f"  every edge, red ruler lines at rows {h // 2}, {h - 55} and {h - 27}.")
    print(f"  If the panel is really {w}x{h} the border meets on all four sides.")
    print("  If it is 54 rows shorter, the top border appears twice.")
    watch_touch(dev, args.seconds)
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)

    sub.add_parser("info", help="identify the panel").set_defaults(func=cmd_info)
    sub.add_parser("wake", help="send sleep-out").set_defaults(func=cmd_wake)

    p = sub.add_parser("brightness", help="set the backlight, 0-100")
    p.add_argument("value", type=int)
    p.set_defaults(func=cmd_brightness)

    # NB: there is deliberately no "flip" subcommand -- see cmd_flip().

    p = sub.add_parser("show", help="send a frame")
    p.add_argument("file", nargs="?", help="raw RGB565 or the SDK's .bmp")
    p.add_argument("--pattern", default="quad",
                   choices=("quad", "bars", "grid", "white", "border"))
    p.add_argument("--size", help="WxH override, e.g. 480x800")
    p.add_argument("--brightness", type=int)
    p.add_argument("--keep", type=float, default=0, metavar="SECONDS",
                   help="watch touch after sending")
    p.set_defaults(func=cmd_show)

    p = sub.add_parser("touch", help="watch the touch endpoint")
    p.add_argument("seconds", type=float, nargs="?", default=30)
    p.set_defaults(func=cmd_touch)

    p = sub.add_parser("test", help="wake, paint test patterns, watch touch")
    p.add_argument("--seconds", type=float, default=30)
    p.set_defaults(func=cmd_test)

    args = parser.parse_args()
    dev = open_screen()
    try:
        return args.func(args, dev)
    finally:
        usb.util.release_interface(dev, 0)


if __name__ == "__main__":
    sys.exit(main())
