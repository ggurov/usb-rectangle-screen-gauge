#!/usr/bin/env python3
"""
Contract checks - the things the C unit tests cannot see.

Guards the assumptions the project makes about its own generated artefacts and
about the host-side design previews:

  1. tools/gen_font.py must agree with the generated gfx_font_data.c, otherwise
     the fonts silently drift from the images the preview shows.
  2. Every character the gauge draws must be inside the font's generated range
     (0x20..0x7E).  A stray degree sign or en-dash renders as nothing at all.
  3. tools/render_preview.py must offer the same presets and the same geometry
     as the renderer, so the previews stay trustworthy.
  4. No graphics library may creep back in - it was dropped deliberately.
  5. Every relative link in the docs must resolve to a tracked file.

Run:  python tests/contracts/test_contracts.py
"""

from __future__ import annotations

import os
import re
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

SRC_DIR = os.path.join(REPO, "src")
INCLUDE_DIR = os.path.join(REPO, "include")
FONT_C = os.path.join(SRC_DIR, "gfx_font_data.c")

FAILURES: list[str] = []
CHECKS = 0


def check(name: str, condition: bool, detail: str = "") -> None:
    global CHECKS
    CHECKS += 1
    if condition:
        print(f"    PASS  {name}")
    else:
        print(f"    FAIL  {name}" + (f"\n          {detail}" if detail else ""))
        FAILURES.append(name)


def read(path: str) -> str:
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        return fh.read()


def const_int(text: str, name: str) -> int | None:
    m = re.search(rf"^{re.escape(name)}\s*=\s*(-?\d+)\s*,?\s*$", text, re.M)
    return int(m.group(1)) if m else None


# ---------------------------------------------------------------------------
# 1. font generator vs generated data
# ---------------------------------------------------------------------------
def test_font_generator() -> None:
    print("\n  [contract/fonts]")
    gen = read(os.path.join(REPO, "tools", "gen_font.py"))

    if not os.path.exists(FONT_C):
        check("gfx_font_data.c exists", False,
              "run: python tools/gen_font.py")
        return
    data = read(FONT_C)

    # every font the generator emits must be defined in the generated file
    names = re.findall(r'\(\s*"(gfx_font_\w+)"\s*,\s*(\d+)\s*,', gen)
    check("gen_font.py declares fonts", len(names) > 0)

    for name, size in names:
        check(f"{name} present in gfx_font_data.c",
              re.search(rf"^const gfx_font_t {name} =", data, re.M) is not None)
        # the generated blob records the pixel size in its comment
        check(f"{name} generated at {size}px",
              re.search(rf"/\* {name}: {size}px", data) is not None,
              "regenerate with: python tools/gen_font.py")

    # the charset must be a contiguous range, since the device indexes with
    # (c - first) and has no lookup table
    m = re.search(r'^CHARSET = .*range\((0x[0-9A-Fa-f]+), (0x[0-9A-Fa-f]+)\)', gen, re.M)
    check("charset is built from a contiguous range", m is not None)
    if m:
        first, last = int(m.group(1), 16), int(m.group(2), 16)
        check(f"charset starts at 0x{first:02X}",
              re.search(rf"\.first = 0x{first:02X}", data) is not None)
        check(f"0x20..0x{last - 1:02X} covered",
              re.search(rf"\.count = {last - first},", data) is not None)


# ---------------------------------------------------------------------------
# 2. every character drawn is inside the font range
# ---------------------------------------------------------------------------
def test_all_drawn_text_is_renderable() -> None:
    print("\n  [contract/text]")

    first, last = 0x20, 0x7E

    # string literals assigned to the gauge's text fields, plus anything passed
    # to gfx_text*
    sources = [
        os.path.join(SRC_DIR, "gauge_presets.c"),
        os.path.join(SRC_DIR, "gauge_render.c"),
    ]

    literals: list[str] = []
    for path in sources:
        text = read(path)
        literals += re.findall(r'"([^"\\]*)"', text)

    offenders = set()
    for lit in literals:
        for ch in lit:
            if not (first <= ord(ch) <= last):
                offenders.add(ch)

    check("every literal drawn as text is inside the font range",
          not offenders,
          f"these characters have no glyph and would render blank: "
          f"{sorted(hex(ord(c)) for c in offenders)} - use ASCII, e.g. 'DEG C' "
          f"instead of a degree sign")

    # the theme's font enum must match the three generated fonts
    theme = read(os.path.join(SRC_DIR, "gauge_theme.c"))
    for field in ("font_label", "font_caption", "font_value",
                  "font_unit", "font_wordmark", "font_tagline"):
        check(f"theme sets {field}",
              re.search(rf"\.{field}\s*=", theme) is not None)


# ---------------------------------------------------------------------------
# 3. preview renderer vs the C renderer
# ---------------------------------------------------------------------------
def test_preview_matches_the_renderer() -> None:
    print("\n  [contract/preview]")
    preview = read(os.path.join(REPO, "tools", "render_preview.py"))
    presets_c = read(os.path.join(SRC_DIR, "gauge_presets.c"))

    preview_ids = set(re.findall(r'dict\(id="([a-z0-9_]+)"', preview))
    in_c = set(re.findall(r'^\s*\{\s*"([a-z0-9_]+)"\s*,', presets_c, re.M))

    check("render_preview.py defines presets", len(preview_ids) > 0)
    check("gauge_presets.c defines presets", len(in_c) > 0)
    check(f"preset ids match: {sorted(in_c)}",
          preview_ids == in_c,
          f"preview has {sorted(preview_ids)}, the renderer has {sorted(in_c)}")

    theme = read(os.path.join(SRC_DIR, "gauge_theme.c"))
    for name, c_name in [("BEZEL_W", "bezel_width"), ("BAND_GAP", "band_gap"),
                         ("BAND_W", "band_width"), ("TICK_MAJOR_LEN", "tick_major_len"),
                         ("TICK_MINOR_LEN", "tick_minor_len"),
                         ("HUB_R", "hub_radius")]:
        py = const_int(preview, name)
        c = re.search(rf"\.{c_name}\s*=\s*(\d+)\s*,", theme)
        c_val = int(c.group(1)) if c else None
        check(f"preview {name} ({py}) == theme .{c_name} ({c_val})",
              py is not None and py == c_val,
              "Keep tools/render_preview.py in step with gauge_theme.c")


# ---------------------------------------------------------------------------
# 4. no graphics library anywhere
# ---------------------------------------------------------------------------
def test_no_graphics_library() -> None:
    print("\n  [contract/no-lvgl]")

    offenders = []
    for root, dirs, files in os.walk(REPO):
        dirs[:] = [d for d in dirs if d not in (".git", "build", "__pycache__")]
        for fn in files:
            if fn.endswith((".c", ".h")):
                path = os.path.join(root, fn)
                if re.search(r'#\s*include\s*[<"]lvgl', read(path)):
                    offenders.append(os.path.relpath(path, REPO).replace("\\", "/"))

    check("no source includes lvgl", not offenders, f"{offenders}")

    # and the build does not link one either
    build = read(os.path.join(REPO, "tools", "build.ps1")).lower()
    check("the build script links no graphics library",
          "lvgl" not in build and "-llv" not in build)


# ---------------------------------------------------------------------------
# 5. documentation links resolve
# ---------------------------------------------------------------------------
def test_doc_links() -> None:
    print("\n  [contract/docs]")

    md_files = []
    for root, dirs, files in os.walk(REPO):
        dirs[:] = [d for d in dirs if d not in (".git", "build", "__pycache__")]
        for fn in files:
            if fn.endswith(".md"):
                md_files.append(os.path.join(root, fn))

    check("markdown files found", len(md_files) > 0)

    # collect every relative link/image target and where it came from
    targets: dict[str, list[str]] = {}
    for path in md_files:
        rel_md = os.path.relpath(path, REPO).replace("\\", "/")
        for match in re.finditer(r"!?\[[^\]]*\]\(([^)]+)\)", read(path)):
            target = match.group(1).strip()
            if target.startswith(("http://", "https://", "mailto:", "#")):
                continue
            target = target.split("#", 1)[0].strip()
            if not target:
                continue
            resolved = os.path.normpath(os.path.join(os.path.dirname(path), target))
            targets.setdefault(resolved, []).append(rel_md)

    check("markdown links a relative file", len(targets) > 0)

    missing = [p for p in targets if not os.path.exists(p)]
    check("every relative link points at a file that exists",
          not missing,
          "\n          ".join(f"{os.path.relpath(p, REPO).replace(chr(92), '/')} "
                             f"(linked from {', '.join(targets[p])})" for p in missing))

    # An ignored file is not on GitHub, so the link is broken there even though
    # it works locally.  This is exactly how the README's dial preview rotted.
    if missing:
        return

    rel_to_abs = {os.path.relpath(p, REPO).replace("\\", "/"): p for p in targets}
    try:
        proc = subprocess.run(["git", "check-ignore", "--stdin"], cwd=REPO,
                              input="\n".join(rel_to_abs), capture_output=True,
                              text=True)
    except OSError:
        return

    ignored = [ln.strip().replace("\\", "/")
               for ln in (proc.stdout or "").splitlines() if ln.strip()]
    detail = "\n          ".join(
        f"{rel} is gitignored, so it will 404 on GitHub "
        f"(linked from {', '.join(targets[rel_to_abs[rel]])})"
        for rel in ignored if rel in rel_to_abs)
    check("every linked file is tracked by git (not gitignored)",
          not ignored, detail)


def main() -> int:
    print("usb-rectangle-screen-gauge contract checks")
    test_font_generator()
    test_all_drawn_text_is_renderable()
    test_preview_matches_the_renderer()
    test_no_graphics_library()
    test_doc_links()

    print("\n-------------------- summary --------------------")
    print(f"  checks: {CHECKS} run, {len(FAILURES)} failed")
    print(f"  result: {'OK' if not FAILURES else 'FAILED'}")
    print("===================================================")
    return 0 if not FAILURES else 1


if __name__ == "__main__":
    raise SystemExit(main())
