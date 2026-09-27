# Development

## Toolchain

| | |
|---|---|
| Compiler | MSYS2 mingw-w64 gcc, `C:\msys64\mingw64\bin\gcc.exe` |
| Build | [`tools/build.ps1`](../tools/build.ps1) — one gcc invocation |
| Run | `build\gauge.exe` |
| Tests | [`tools/test.ps1`](../tools/test.ps1) — unit tests + contract checks |
| libusb | `libusb-1.0.dll`, loaded at runtime (never linked) |
| Python | 3.13 for the tooling (pyusb, libusb-package, opencv for previews) |

There is no cross toolchain, no CMake, no IDE project.  The build is a single
`gcc` call with `-Iinclude` and `-DGFX_W=480 -DGFX_H=800`, which is also the
whole story of how the panel's geometry reaches the drawing code.

The DLL is opened with `LoadLibrary`, so the build has no import library to
find; `tools/build.ps1` copies the first one it finds next to the executable:

1. `$env:GAUGE_LIBUSB_DLL`
2. `libusb-1.0.dll` next to the gcc (MSYS2: `pacman -S mingw-w64-x86_64-libusb`)
3. the Python wheel: `pip install libusb-package`
4. `C:\msys64\mingw64\bin\libusb-1.0.dll`

The screen must be bound to WinUSB.  The VoCore "USB2.0 Screen driver" package
(or Zadig) does that; Windows ships no driver for a vendor-class device.

## Build and run

```powershell
tools\build.ps1
build\gauge.exe                        # gauge + REPL
build\gauge.exe --run 10 --value 5200  # headless, for a scripted screenshot
```

The app has no flash step: it *is* the program, and it opens the screen at
startup.  `--size`, `--xshift`, `--brightness`, `--gauge`, `--value`, `--test`,
`--partial` and `--render` are the options worth knowing; the console can change
most of them at run time, and it also reads piped stdin
(`'fps','info','test quad','quit' | build\gauge.exe`).

## Testing

```powershell
tools\test.ps1                  # unit tests + contract checks (~2 s)
tools\test.ps1 -Filter render   # only suites whose name contains "render"
```

Run this before every commit.  It is fast, needs no hardware, and covers the
whole renderer and the screen's protocol.

The key enabler: **`gfx` talks to the panel through exactly one function**
(`bsp_lcd_draw_bitmap`).  Stubbing that lets `gfx.c`, `gfx_text.c` and
`gauge_render.c` be compiled and exercised against a real framebuffer.

The screen's protocol is testable for the same reason: `src/vocore_panel.c`
speaks through a `vocore_transport_t` — four function pointers — instead of
calling libusb, so the tests script the replies and assert the exact bytes.
The transport that talks to WinUSB is `src/usb_libusb.c`, and only that file
knows the DLL exists.

See [`tests/README.md`](../tests/README.md).

## Design notes

### Layering, and why it matters for tests

| File | Depends on | Tested by |
|---|---|---|
| `gauge_math.c` | libm only | unit tests |
| `gauge_theme.c` | nothing | unit tests |
| `gauge_presets.c` | string.h | unit tests |
| `gfx.c` | one panel call | unit tests, panel stubbed |
| `gfx_text.c` | `gfx.c` | unit tests |
| `gauge_render.c` | `gfx` | unit tests |
| `vocore_proto.c` | nothing | unit tests |
| `vocore_panel.c` | a transport vtable | unit tests, fake transport |
| `usb_libusb.c` | WinUSB via libusb | hardware only |
| `app_bsp.c` | `vocore_panel.c` | hardware only |

Resist moving arithmetic into `gauge_render.c` or `app_bsp.c`.

### Frame budget

A whole 480x800 RGB565 frame is 768,000 bytes.  Measured on this machine:

| | |
|---|---|
| render (CPU → framebuffer) | ~1 ms |
| panel transfer (USB, bulk) | ~30 ms at ~18 MB/s |
| delivered | **~24 fps** |

The transfer is the whole budget, which is why the app sends exactly one bulk
transfer per frame and nothing else.  The loop does not sleep a fixed period:
it measures the real frame interval and passes it to the slew filter, so the
link sets the rate.

Partial rectangles would cut the traffic in half, and the protocol supports
them — this unit's firmware does not, so they are off by default and
`docs/usb-screen.md` explains what happens if you turn them on.  Compressed
frames (LZ4 or baseline JPEG, both in the protocol) are the next lever.

### Why the fonts are generated

`tools/gen_font.py` rasterises a system TTF into 8-bit coverage masks, emitted
as `src/gfx_font_data.c`.  No font library runs in the app, the glyphs are
anti-aliased, and the whole set is 64 KB.

Two details that are easy to get wrong and are both covered by tests:

* **`bearing_y` is measured from the baseline**, not the ascender.  PIL places
  the text origin on the ascender line, so the ascent has to be subtracted
  again.  Getting this wrong draws every glyph one ascent too low.
* **The charset is a contiguous `0x20..0x7E`**, so the renderer can index
  glyphs with `(c - first)` and needs no lookup table.

Text is positioned on **cap height**, not line height: use
`gfx_text_cap_centered()`.  On the 480 px glass the fonts are drawn at
`text_scale` 2 — the masks are replicated as square blocks, which holds up
better than re-rasterising a second set.

### Why the needle slews

`gauge_render_set_value()` only stores a target.  The loop moves `displayed`
toward it with an exponential approach capped by a maximum slew rate, so the
needle accelerates off a stop and settles like a real moving-coil movement.
`slew_time` in the config is the time for a full-scale ramp — 0.30 s for a
tachometer, 2.0 s for a temperature gauge.

### Why the console comes first

`main()` starts the REPL *before* it opens the screen.  If the panel fails to
come up the app logs the error and carries on, leaving a working console — so a
flat battery, a missing driver or a wedged panel costs you a `connect`, not a
restart.  `connect` re-opens the device and the gauge carries on.

### Regenerating the fonts

```powershell
python tools\gen_font.py
```

Writes `src/gfx_font_data.c` and `include/gfx_font_data.h`.  A contract check
fails if the generated file drifts from the generator's declared sizes.
