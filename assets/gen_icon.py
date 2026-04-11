#!/usr/bin/env python3
"""
gen_icon.py — Generates the CanDo icon: a dark hex-bevelled cube covered in
green PCB traces with an octagonal red button on top.

Requires Pillow:   pip install Pillow
Run:               python3 assets/gen_icon.py

Outputs:
  assets/icon.png  (256x256 master)
  assets/icon.ico  (multi-res: 16, 32, 48, 256)
"""

import math
import struct
import io

try:
    from PIL import Image, ImageDraw, ImageFilter
    HAS_PILLOW = True
except ImportError:
    HAS_PILLOW = False
    print("Pillow not found — falling back to stdlib-only minimal ICO writer.")


# ---------------------------------------------------------------------------
# Colour palette
# ---------------------------------------------------------------------------
BG          = (0, 0, 0, 0)           # transparent
CUBE_DARK   = (28, 32, 38, 255)      # dark steel body
CUBE_MID    = (40, 46, 54, 255)      # mid face
CUBE_LIGHT  = (55, 62, 72, 255)      # light face
EDGE_COLOR  = (18, 22, 28, 255)      # outline / bevel edges
TRACE_COLOR = (0, 200, 80, 255)      # bright green PCB traces
PAD_COLOR   = (0, 160, 60, 255)      # chip pad colour
CHIP_BODY   = (45, 50, 60, 255)      # IC body
CHIP_PINS   = (80, 200, 120, 255)    # IC pins
BTN_RED     = (190, 30, 30, 255)     # button top
BTN_DARK    = (120, 15, 15, 255)     # button bevel
BTN_BEVEL   = (230, 60, 60, 255)     # button highlight


def make_icon(size=256):
    img = Image.new("RGBA", (size, size), BG)
    d   = ImageDraw.Draw(img)
    s   = size
    cx  = s // 2

    # -----------------------------------------------------------------------
    # Isometric cube faces
    # -----------------------------------------------------------------------
    # We draw three visible faces of a cube in isometric-ish perspective.
    # Vertices (as fractions of s):
    #   top-centre     T  = (0.50, 0.12)
    #   left           L  = (0.10, 0.38)
    #   right          R  = (0.90, 0.38)
    #   bottom-left    BL = (0.10, 0.76)
    #   bottom-right   BR = (0.90, 0.76)
    #   bottom-centre  BC = (0.50, 0.92)

    def pt(fx, fy):
        return (int(fx * s), int(fy * s))

    T  = pt(0.50, 0.12)
    L  = pt(0.10, 0.38)
    R  = pt(0.90, 0.38)
    BL = pt(0.10, 0.76)
    BR = pt(0.90, 0.76)
    BC = pt(0.50, 0.92)
    MC = pt(0.50, 0.54)  # middle centre (where 3 faces meet)

    # Top face  (T, R, MC, L)
    d.polygon([T, R, MC, L], fill=CUBE_LIGHT)
    # Left face (L, MC, BC, BL)
    d.polygon([L, MC, BC, BL], fill=CUBE_MID)
    # Right face (MC, R, BR, BC)
    d.polygon([MC, R, BR, BC], fill=CUBE_DARK)

    # Edges
    for poly in [[T, R, MC, L], [L, MC, BC, BL], [MC, R, BR, BC]]:
        d.polygon(poly, outline=EDGE_COLOR)
    # Central vertical ridge
    d.line([MC, BC], fill=EDGE_COLOR, width=max(1, s // 80))
    # Top ridges
    d.line([T, MC], fill=EDGE_COLOR, width=max(1, s // 80))

    # -----------------------------------------------------------------------
    # PCB trace helper
    # -----------------------------------------------------------------------
    tw = max(1, s // 90)   # trace width

    def trace(points, color=TRACE_COLOR):
        if len(points) >= 2:
            d.line(points, fill=color, width=tw)

    def pad(cx_, cy_, r=None, color=PAD_COLOR):
        r = r or max(2, s // 60)
        d.ellipse([cx_ - r, cy_ - r, cx_ + r, cy_ + r], fill=color)

    def chip(x, y, w, h, pins=3):
        """Draw a small IC chip rectangle with pins on left/right."""
        d.rectangle([x, y, x + w, y + h], fill=CHIP_BODY, outline=TRACE_COLOR)
        ph = h // (pins + 1)
        for i in range(1, pins + 1):
            py_ = y + ph * i
            # left pins
            d.line([x - tw * 3, py_, x, py_], fill=CHIP_PINS, width=tw)
            # right pins
            d.line([x + w, py_, x + w + tw * 3, py_], fill=CHIP_PINS, width=tw)

    # -----------------------------------------------------------------------
    # LEFT face traces
    # -----------------------------------------------------------------------
    # Left face occupies roughly x in [0.10s, 0.50s], y in [0.38s, 0.92s]
    # We'll place traces and chips in this parallelogram (approximate as rect)
    lx0, ly0 = int(0.13 * s), int(0.42 * s)
    lx1, ly1 = int(0.46 * s), int(0.88 * s)

    # Horizontal bus lines
    for frac in [0.30, 0.50, 0.70]:
        y_ = int(ly0 + frac * (ly1 - ly0))
        trace([(lx0, y_), (lx1, y_)])

    # Vertical stubs
    mx_ = (lx0 + lx1) // 2
    trace([(mx_, ly0 + 4), (mx_, ly1 - 4)])
    trace([(lx0 + 8, ly0 + 8), (lx0 + 8, ly1 - 8)])

    # Pads
    for fy in [0.25, 0.50, 0.75]:
        pad(lx0 + 8, int(ly0 + fy * (ly1 - ly0)))
        pad(mx_,     int(ly0 + fy * (ly1 - ly0)))

    # Chip on left face
    cw, ch = int(0.12 * s), int(0.14 * s)
    chip(lx0 + int(0.18 * (lx1 - lx0)),
         int(ly0 + 0.32 * (ly1 - ly0)), cw, ch, pins=3)

    # -----------------------------------------------------------------------
    # RIGHT face traces
    # -----------------------------------------------------------------------
    rx0, ry0 = int(0.54 * s), int(0.42 * s)
    rx1, ry1 = int(0.87 * s), int(0.88 * s)

    for frac in [0.25, 0.55, 0.80]:
        y_ = int(ry0 + frac * (ry1 - ry0))
        trace([(rx0, y_), (rx1, y_)])

    my_ = (ry0 + ry1) // 2
    trace([(rx0 + 4, ry0 + 4), (rx0 + 4, ry1 - 4)])
    trace([(rx1 - 8, ry0 + 8), (rx1 - 8, my_)])

    for fy in [0.20, 0.55, 0.85]:
        pad(rx1 - 8, int(ry0 + fy * (ry1 - ry0)))

    cw2, ch2 = int(0.13 * s), int(0.16 * s)
    chip(rx0 + int(0.38 * (rx1 - rx0)),
         int(ry0 + 0.20 * (ry1 - ry0)), cw2, ch2, pins=3)
    chip(rx0 + int(0.10 * (rx1 - rx0)),
         int(ry0 + 0.55 * (ry1 - ry0)), int(cw2 * 0.7), int(ch2 * 0.7), pins=2)

    # -----------------------------------------------------------------------
    # TOP face traces
    # -----------------------------------------------------------------------
    # Top face is the diamond T-R-MC-L; we add a few traces around the centre
    tcx, tcy = int(0.50 * s), int(0.33 * s)
    tr = int(0.13 * s)

    for angle_deg in [30, 90, 150, 210, 270, 330]:
        a = math.radians(angle_deg)
        x0_ = int(tcx + tr * 0.35 * math.cos(a))
        y0_ = int(tcy + tr * 0.35 * math.sin(a))
        x1_ = int(tcx + tr * 0.80 * math.cos(a))
        y1_ = int(tcy + tr * 0.80 * math.sin(a))
        trace([(x0_, y0_), (x1_, y1_)])
        pad(x1_, y1_, r=max(2, s // 70))

    # Small chip marks on top
    pad(tcx - int(0.06 * s), tcy - int(0.03 * s), r=max(2, s // 55), color=CHIP_BODY)
    pad(tcx + int(0.06 * s), tcy - int(0.03 * s), r=max(2, s // 55), color=CHIP_BODY)

    # -----------------------------------------------------------------------
    # Red octagonal button on top (centred around T shifted slightly down)
    # -----------------------------------------------------------------------
    btn_cx = int(0.50 * s)
    btn_cy = int(0.22 * s)
    br     = int(0.085 * s)   # button radius

    # Octagon vertices
    def octagon(cx_, cy_, r, fill, outline=None):
        pts = []
        for i in range(8):
            a = math.radians(i * 45 - 22.5)
            pts.append((cx_ + r * math.cos(a), cy_ + r * math.sin(a)))
        d.polygon(pts, fill=fill, outline=outline)

    # Shadow / dark base
    octagon(btn_cx, btn_cy + max(1, s // 60), int(br * 1.15), BTN_DARK)
    # Main button
    octagon(btn_cx, btn_cy, br, BTN_RED, outline=BTN_DARK)
    # Highlight (top-left glint)
    octagon(btn_cx - max(1, s // 80), btn_cy - max(1, s // 80),
            int(br * 0.55), BTN_BEVEL)
    # Centre dot
    cr = max(2, s // 40)
    d.ellipse([btn_cx - cr, btn_cy - cr, btn_cx + cr, btn_cy + cr],
              fill=BTN_DARK)

    # -----------------------------------------------------------------------
    # Subtle edge highlight on bevel
    # -----------------------------------------------------------------------
    ew = max(1, s // 100)
    d.line([T, L],  fill=(80, 90, 100, 180), width=ew)
    d.line([T, R],  fill=(80, 90, 100, 180), width=ew)
    d.line([L, BL], fill=(20, 24, 30, 200),  width=ew)
    d.line([R, BR], fill=(20, 24, 30, 200),  width=ew)
    d.line([BL, BC], fill=(20, 24, 30, 200), width=ew)
    d.line([BR, BC], fill=(20, 24, 30, 200), width=ew)

    return img


def write_ico_from_pil(images, path):
    """Write a Windows .ico file containing all supplied PIL images."""
    sizes = []
    bitmaps = []
    for img in images:
        buf = io.BytesIO()
        img.save(buf, format="PNG")
        bitmaps.append(buf.getvalue())
        sizes.append(img.size[0])

    num = len(images)
    header = struct.pack("<HHH", 0, 1, num)
    dir_size = 16 * num
    offset = 6 + dir_size
    directory = b""
    for i, (sz, data) in enumerate(zip(sizes, bitmaps)):
        w = sz if sz < 256 else 0
        h = sz if sz < 256 else 0
        directory += struct.pack("<BBBBHHII",
                                 w, h, 0, 0,   # width, height, colour count, reserved
                                 1, 32,         # planes, bit count
                                 len(data),     # size of image data
                                 offset)        # offset of image data
        offset += len(data)

    with open(path, "wb") as f:
        f.write(header)
        f.write(directory)
        for data in bitmaps:
            f.write(data)


def main():
    import os
    here = os.path.dirname(os.path.abspath(__file__))
    png_path = os.path.join(here, "icon.png")
    ico_path = os.path.join(here, "icon.ico")

    if not HAS_PILLOW:
        print("ERROR: Pillow is required.  Install with: pip install Pillow")
        raise SystemExit(1)

    print("Generating icon.png (256×256)…")
    master = make_icon(256)
    master.save(png_path)
    print(f"  Saved: {png_path}")

    print("Generating icon.ico (16, 32, 48, 256)…")
    ico_images = []
    for sz in [16, 32, 48, 256]:
        if sz == 256:
            ico_images.append(master.copy())
        else:
            ico_images.append(master.resize((sz, sz), Image.LANCZOS))
    write_ico_from_pil(ico_images, ico_path)
    print(f"  Saved: {ico_path}")
    print("Done.")


if __name__ == "__main__":
    main()
