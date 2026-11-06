#!/usr/bin/env python3
"""Turn the application's own Finder icon into a Windows .ico.

    python tools/make_icon.py work/app/rsrc out.ico

Reads icl8/ICN# 1000 (32x32) and ics8/ics# 1000 (16x16) from the extracted
resources (your own copy of the game), colours them with the Macintosh's
standard 8-bit palette, and writes 16, 32, 48 and 256 pixel images (the large
ones scaled up by whole pixels, to stay crisp)."""
import io, struct, sys
from pathlib import Path
from PIL import Image

def mac_palette():
    """The standard Macintosh 8-bit colour table (System 7 'clut' 8)."""
    v = [0xFF, 0xCC, 0x99, 0x66, 0x33, 0x00]
    pal = [(r, g, b) for r in v for g in v for b in v][:215]   # the 6x6x6 cube minus black
    ramp = [0xEE, 0xDD, 0xBB, 0xAA, 0x88, 0x77, 0x55, 0x44, 0x22, 0x11]
    pal += [(x, 0, 0) for x in ramp] + [(0, x, 0) for x in ramp] + [(0, 0, x) for x in ramp]
    pal += [(x, x, x) for x in ramp] + [(0, 0, 0)]
    return pal

def icon(rsrc, kind8, mask_kind, size, rid=1000):
    px = (rsrc / f"{kind8}_{rid}.bin").read_bytes()
    mk = (rsrc / f"{mask_kind}_{rid}.bin").read_bytes()
    rb = size // 8
    mask = mk[size * rb: size * rb * 2]          # ICN#/ics#: icon bitmap, then the mask
    pal = mac_palette()
    im = Image.new("RGBA", (size, size))
    for y in range(size):
        for x in range(size):
            on = (mask[y * rb + x // 8] >> (7 - x % 8)) & 1
            r, g, b = pal[px[y * size + x]]
            im.putpixel((x, y), (r, g, b, 255 if on else 0))
    return im

def main():
    rsrc, out = Path(sys.argv[1]), sys.argv[2]
    big, small = icon(rsrc, "icl8", "ICN#", 32), icon(rsrc, "ics8", "ics#", 16)
    imgs = [small, big, big.resize((48, 48), Image.NEAREST), big.resize((256, 256), Image.NEAREST)]
    blobs = []
    for im in imgs:
        b = io.BytesIO(); im.save(b, "PNG"); blobs.append((im.size[0], b.getvalue()))
    with open(out, "wb") as f:
        f.write(struct.pack("<HHH", 0, 1, len(blobs)))
        off = 6 + 16 * len(blobs)
        for s, data in blobs:
            f.write(struct.pack("<BBBBHHII", s % 256, s % 256, 0, 0, 1, 32, len(data), off)); off += len(data)
        for _, data in blobs: f.write(data)
    imgs[3].save(Path(out).with_suffix(".png"))
    print(f"wrote {out}")

if __name__ == "__main__":
    main()
