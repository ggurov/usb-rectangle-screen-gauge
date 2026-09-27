# The USB screen

The display this port targets is a **VoCore USB2.0 Screen**.  It is not a
DisplayLink adapter, not an HDMI capture device and not a HID screen: it is a
vendor-specific USB device with its own controller, its own frame memory and a
protocol you speak in bulk transfers.  This is what it is, how it was found
out, and the two things about it that cost the most time.

## What the device is

| | |
|---|---|
| ID | `0xc872:0x1004`, manufacturer `VoCore`, product `USB2.0 Screen` |
| USB | full-speed? no — high speed, vendor class `ff/ff/ff`, one interface |
| Endpoints | `0x02` bulk OUT (512-byte packets) for frames, `0x81` interrupt IN (64-byte packets) for touch |
| Windows driver | WinUSB, installed by the VoCore "USB2.0 Screen driver" package (libwdi/wdi-simple) |
| Panel | 480 x 800 portrait, RGB565, behind a glossy cover in a case |
| Firmware | reports no model: both identification registers read `0xffffffff` |

The VoCore screens come in several sizes and two driver-board generations
(`V7B` and `MPRO`); the same VID/PID is used for all of them, so the model has
to come from the panel's registers, from a measurement, or from the label on
the back.  This one answers nothing, which is why everything below was measured
rather than read.

## The protocol

All of it is vendor control requests on endpoint 0 plus bulk data.  The
authoritative sources are the `v2scrctl` SDK (`source/screen_test.c` and
`source/partest.c`) and the in-kernel `fbusb` driver from the VoCore2 tree;
they agree byte for byte.  `src/vocore_proto.c` is the C translation and
`tests/unit/test_vocore_proto.c` pins every byte.

| Action | Transfer |
|---|---|
| Send a command | control OUT, request `0xb0` |
| Read a register | `0xb5` with `{0x51,0x02,len,0x1f,tag}`, then `0xb6` IN (1 byte ack), then `0xb7` IN (`len+1` bytes; the value is little-endian at offset 1) |
| Firmware info string | control IN `0xa0` (fall back to `0xa1`), 64 bytes, starts with `v` |
| Whole frame | `0xb0` with `{mode, 0x2c, size[3], 0}` then the frame on bulk OUT |
| Partial rectangle | `0xb0` with `{mode, 0x2c, size[4], x, y, w, h}` (12 bytes for raw RGB565 -- the height is derivable from size/w and is omitted; 14 for compressed) |
| Wake | `0xb0` with `{0x00, 0x29, 0, 0, 0, 0}` -- the panel boots asleep and shows nothing until this |
| Backlight | `0xb0` with `{0x00, 0x51, 0x02, 0, 0, 0, percent, 0}` |
| Flip/mirror | `0xb0` with `{0x00, 0x36, 0x02, 0, 0, 0, mode, mode}` |
| Touch | read the interrupt endpoint; two points, 12-bit coordinates, flag 0=press 1=hover 2=drag |

Frame modes are RGB565 (`0x00`), NV12 (`0x01`), baseline JPEG (`0x02`) and LZ4
(`0x04`, partial rectangles only).  This port uses RGB565 and nothing else: the
framebuffer is already RGB565, so a frame is one bulk transfer of raw pixels,
little-endian, top row first.

Register tags that were tried for identification: `0x1f fc` (screen),
`0x1f f8` (version), `0x1f f0` (board UID).  All three reply
`01 ff ff ff ff`: an acknowledge with an all-ones value.  There is no error to
recover from; the firmware simply has no model table.

## Quirk 1: the geometry had to be measured

The SDK's fallback for unknown firmware is the 5-inch glass, 480x854, and the
official `screen_test.exe` prints exactly that for this device.  It is wrong
for this panel, and a wrong height puts the dial off centre and cuts the bottom
rows.

The measurement, in order:

1. **Markers.**  A frame with 60 px blocks at known framebuffer positions, and
   a camera pointed at the screen.  The blocks landed where a 1:1 mapping of a
   480-wide framebuffer predicts, which ruled out `fbusb`'s 320-byte "margin"
   and the 854-row fallback (the bottom of an 854-row frame would have wrapped).
2. **Bands.**  Eight 100-row colour bands, each a different colour, and four
   120-column strips.  On the glass they appear in order and complete: eight
   bands, four strips, no doubling.  A 480x854 or 480x400 panel would have
   wrapped somewhere.
3. **Rainbow.**  A hue ramp along the long axis with a white crosshair.  The
   hue is constant down the short axis and monotonic along the long one, which
   is the strongest statement that the mapping is 1:1 in both directions.

Conclusion: **480 x 800, RGB565, no rotation, no margin**.  The register
fallback in `vocore_proto.c` is 480x800 for that reason, and `--size` overrides
it either way.

The visible glass is a little smaller than the memory: the panel sits in a
case, and the frame's outer pixels are behind the bezel.  Nothing the gauge
draws comes near them.

## Quirk 2: partial writes move the frame pointer

This one produced the most confusing symptom of the whole port: the dial
arriving as **two half-dials on opposite sides of the screen**, or a complete
dial with a slice of itself wrapped around to the other edge.

The explanation: this firmware does **not** implement the SDK's partial-draw
command, but it does not reject it either.  The command is accepted, nothing is
drawn, and the panel's frame write pointer has moved on by the size of the
rectangle.  The panel then writes subsequent whole frames from wherever the
pointer is, wrapping at the end of its frame memory -- so the picture comes out
circularly shifted by exactly the number of pixels the ignored partial write
claimed.

That is why the symptom kept changing shape.  A 480x480 partial shifts the
frame by 230,400 pixels (480 rows exactly: two half dials, one above the
other).  A smaller one leaves a column offset (a slice of the dial wrapped
round).  Every whole frame is exactly one frame memory -- 480x800x2 bytes --
so on its own it never changes the alignment.

So:

* **the app never sends partial writes**; `--partial on` exists because the
  protocol does and other units support it, but on this glass it costs the
  alignment as well as the frame rate;
* `tools/usb_screen.py` never sends them either;
* after a power cycle the pointer is at zero and whole frames land perfectly;
* if a session does inherit a shifted panel, `--xshift N` (or `xshift N` in the
  console) pre-rolls whole frames by N columns, and a power cycle is the
  complete cure.

### The flip command changes the addressing

One more thing not to touch: the SDK's flip/mirror command (`0x36`) does more
than mirror on this unit -- sending it moved the display into a rotated
addressing mode, and none of the four mode values reproduced the power-on
state.  `--flip` is wired through for completeness, but `gauge.exe` never
sends it unless asked, and the bring-up tool does not expose it any more.
Recovery is a power cycle.

## Frame rate

Measured with a stopwatch around the transfer loop, 480x800 RGB565:

| | |
|---|---|
| Whole frames | 768,000 bytes, ~24 fps, ~18 MB/s |
| A dial rectangle (480x464) | would be ~42 fps -- if the firmware drew it |
| USB | high speed, one bulk transfer per frame, no compression |

That is the wire's limit, not the renderer's: rendering the dial takes about
1 ms on the desktop (the app's read-out splits render from flush; `fps` in the
console shows both).  Since this unit ignores partial writes, whole frames are
what it gets: about 24 fps, 18 MB/s, out of a high-speed link that measures
~20.5 MB/s with a raw writer.  The read-out on the dial shows the delivered
rate.

LZ4-compressed partial rectangles (`mode 0x04`) and baseline JPEG are supported
by the protocol and would cut the traffic further; neither is used yet.

## The touch endpoint

`0x81` is advertised as an interrupt IN endpoint and the SDK decodes
two-point reports from it.  On this unit it stays silent: no reports at all,
with or without the panel awake, over sixty-second polls.  The likely
explanation is that this is the non-touch version of the glass (VoCore sells
both), and no touch controller is fitted.

The app polls the endpoint anyway.  If a report ever arrives, a tap cycles
the instruments, and `tools/usb_screen.py touch 30` is the standalone probe
for it.

## Bring-up tool

`tools/usb_screen.py` is the Python-side tool used during the port; the C
protocol layer is its translation.  It needs pyusb and a libusb DLL
(`pip install pyusb libusb-package`).

```powershell
python tools\usb_screen.py info                 # identify, UID, geometry
python tools\usb_screen.py test                 # wake, paint patterns, watch touch
python tools\usb_screen.py show quad --size 480x800
python tools\usb_screen.py show frame.raw --size 480x800
python tools\usb_screen.py brightness 80
python tools\usb_screen.py touch 30
```

## Running it without a PC

The screen is a USB *device*: it has no SPI, RGB or MIPI input, only the vendor
bulk protocol, so something has to be a USB *host*.  Today that is the desktop
app.

A standalone build means a USB 2.0 high-speed host -- a Raspberry Pi, or the
VoCore2 this screen was designed for.  `vocore_proto.c` and `vocore_panel.c`
are portable C over a four-function transport vtable, so only a USB-host
transport would be new; the protocol, the panel logic and the whole renderer
are already unit-tested.  Settle for nothing slower than high speed: a
full-speed (12 Mbit/s) host would move a 768,000-byte frame in about a second.

## What would go wrong differently next time

* A frame that arrives split in half, or with a slice of itself wrapped round
  to the other edge, is the **frame pointer shifted by an ignored partial
  write**: `--xshift N` pre-rolls around it, a power cycle clears it, and
  `--partial on` is what causes it -- leave it off.
* A dial that is off centre vertically or cut off at the bottom is the **wrong
  height**: try `--size 480x800`.
* A screen that shows nothing at all after a power cycle has not been woken:
  sleep-out must be sent once (`info` or `test` does it).
* A dial that appears rotated or sheared after some experimenting means the
  **flip command** was sent: only a power cycle clears that one.
* A picture with the colours wrong (red and blue swapped) is a byte-order
  problem, not a panel one -- but note the SDK's screens are RGB565
  little-endian, which is what `gfx` produces on x86.
* **Killing the app while it holds the device can wedge the panel's
  firmware.**  Force-terminating `gauge.exe` mid-transfer left this unit
  answering nothing at all -- control requests included -- until the USB cable
  was unplugged and plugged back in.  USB-level resets did not help; only a
  power cycle did.  The app installs a console handler so Ctrl+C and closing
  the window release the interface cleanly, and `--run N` exits by itself;
  avoid `Stop-Process -Force` on it.
