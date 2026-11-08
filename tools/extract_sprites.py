#!/usr/bin/env python3
r"""
extract_sprites.py - pull every sprite and tile out of your own copy of Mac
Prince of Persia (1992) and save them as PNG, ready for upscaling.

    py tools\extract_sprites.py                      (reads work\files, writes work\sprites)
    py tools\extract_sprites.py --gfx LC             (the 512x384 "LC" art set instead)
    py tools\extract_sprites.py --files game\data\files --out C:\somewhere\sprites

Only the Python standard library is needed. Everything it writes is made from
the game's own art, so the output stays local (work/ is gitignored).

What it reads
-------------
The colour art lives in the files "Persia(COLOR)" (disk 2) and "Persia(LC)"
(disk 3), type 'PPgf'. Each is split into named sets (Prince, Guard, Torch,
Palace, ...). For a set with resource ID n:

  SHPD n   12 bytes: offset in the data fork, packed length, unpacked length
  SHPT n   one 4-byte offset per shape into the unpacked set, -1 = no shape
           (the game's shape number is the index in this table)

The packing is LZSS (CODE 2 0x2536 in the lifter's offsets): a flag byte read
LSB first, flag 1 = a 2-byte back-reference (distance = low 12 bits + 1,
length = top 4 bits + 3), flag 0 = one literal byte.

A shape (drawn by the 8-bit blitter, CODE 2 0x3dc4) is an 8-byte header

  width, height, x offset, y offset      (signed 16-bit, big-endian)

followed by a byte-code. In each byte, bit 7 = start the next row, the low 5
bits are a count n (31 = add the next byte), and bits 6-5 pick the action:

  00  n == 0: end of shape; else one colour byte, drawn n+1 times
  01  n+1 colour bytes follow, drawn as they are
  10  skip n+1 pixels (transparent)
  11  n > 0: start a block repeated n+1 times; n == 0: end of that block
      (or end of shape when no block is open)

The game draws a shape with its pixel (0,0) at (x - x offset, y - y offset),
mirrored about x for the other facing direction, so only one direction is
stored. Colour bytes are entries of the game's palette (clut 2000). Entries
0x69-0x75 are replaced per level by a guard uniform (clut 3000-3007), and
196-200 are cycled for the torch flames.

What it writes (in --out, default work/sprites/<COLOR|LC>/)
------------------------------------------------------------
  <id>_<set name>/<nnn>.png         RGBA: the sprite, transparent background
  <id>_<set name>/<nnn>_mask.png    greyscale: 255 = pixel drawn, 0 = not
  <id>_<set name>/<nnn>_idx.png     8-bit indexed with the game's palette
                                    (index 255 = transparent); keeps the
                                    original colour numbers for recolouring
  <id>_<set name>/_sheet.png        all shapes of the set side by side (x2)
  sprites.json                      every shape: set, number, size, offsets,
                                    files. This is what an HD layer needs to
                                    match "draw shape #37 of set 400" to a file
  palette.json                      the palette used, and the guard variants
"""
import argparse
import json
import os
import struct
import sys
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TRANSPARENT = 255                      # never used by any shape


# ---------- resource forks ----------

def read_resources(fork):
    """Return {(type, id): (name, bytes)} for a resource fork."""
    d_off, m_off, _, _ = struct.unpack(">IIII", fork[:16])
    m = fork[m_off:]
    tl, nl = struct.unpack(">HH", m[24:28])
    t = m[tl:]
    (ntypes,) = struct.unpack(">H", t[:2])
    out = {}
    for i in range(ntypes + 1):
        rtype, count, ref = struct.unpack(">4sHH", t[2 + i * 8:10 + i * 8])
        for j in range(count + 1):
            rid, name_off, attr_addr = struct.unpack(">hHI", t[ref + j * 12:ref + j * 12 + 8])
            a = d_off + (attr_addr & 0xFFFFFF)
            (ln,) = struct.unpack(">I", fork[a:a + 4])
            name = None
            if name_off != 0xFFFF:
                p = nl + name_off
                name = m[p + 1:p + 1 + m[p]].decode("mac_roman")
            out[(rtype.decode("mac_roman"), rid)] = (name, fork[a + 4:a + 4 + ln])
    return out


def read_manifest(files_dir):
    """files.txt (written by prepare.py) -> {name: (type, data path, rsrc path)}."""
    out = {}
    with open(files_dir / "files.txt", encoding="mac_roman") as f:
        for line in f:
            if line.startswith("#") or not line.strip():
                continue
            name, ftype, _creator, dpath, _dl, rpath, _rl = line.rstrip("\n").split("\t")
            fix = lambda p: None if p == "-" else files_dir / Path(p).relative_to("files")
            out[name] = (ftype, fix(dpath), fix(rpath))
    return out


# ---------- the game's formats ----------

def unpack_lzss(src, size):
    out = bytearray()
    i = 0
    while i < len(src) and len(out) < size:
        flags = src[i]
        i += 1
        for _ in range(8):
            if i >= len(src):
                break
            if flags & 1:
                v = (src[i] << 8) | src[i + 1]
                i += 2
                dist, n = (v & 0xFFF) + 1, (v >> 12) + 3
                for _ in range(n):
                    out.append(out[-dist])
            else:
                out.append(src[i])
                i += 1
            flags >>= 1
    if len(out) != size:
        raise ValueError(f"unpacked {len(out)} bytes, expected {size}")
    return bytes(out)


def decode_shape(buf, off):
    """Return (width, height, xoff, yoff, pixels) with pixels a list of rows
    holding a palette index or None (transparent)."""
    w, h, xo, yo = struct.unpack(">hhhh", buf[off:off + 8])
    rows = [[None] * w for _ in range(h)]
    p, x, y, blocks = off + 8, 0, 0, []

    def put(c):
        if 0 <= x < w and 0 <= y < h:
            rows[y][x] = c

    while True:
        b = buf[p]
        p += 1
        if b & 0x80:
            y, x = y + 1, 0
        n = b & 0x1F
        if n == 0x1F:
            n += buf[p]
            p += 1
        op = (b >> 5) & 3
        if op == 2:                                   # skip
            x += n + 1
        elif op == 1:                                 # literal run
            for _ in range(n + 1):
                put(buf[p])
                p += 1
                x += 1
        elif op == 0:                                 # fill / end
            if n == 0:
                break
            c = buf[p]
            p += 1
            for _ in range(n + 1):
                put(c)
                x += 1
        else:                                         # repeat block / end
            if n:
                blocks.append([p, n])
            elif not blocks:
                break
            else:
                blocks[-1][1] -= 1
                if blocks[-1][1] >= 0:
                    p = blocks[-1][0]
                else:
                    blocks.pop()
    return w, h, xo, yo, rows


def read_clut(res, rid):
    d = res[("clut", rid)][1]
    (n,) = struct.unpack(">H", d[6:8])
    out = {}
    for k in range(n + 1):
        v, r, g, b = struct.unpack(">HHHH", d[8 + k * 8:16 + k * 8])
        out[v] = (r >> 8, g >> 8, b >> 8)
    return out


# ---------- PNG (standard library only) ----------

def _png(path, w, h, color_type, rows, plte=None, trns=None):
    def chunk(tag, data):
        c = tag + data
        return struct.pack(">I", len(data)) + c + struct.pack(">I", zlib.crc32(c) & 0xFFFFFFFF)
    raw = b"".join(b"\0" + bytes(r) for r in rows)
    out = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, color_type, 0, 0, 0))
    if plte:
        out += chunk(b"PLTE", plte)
    if trns:
        out += chunk(b"tRNS", trns)
    out += chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")
    Path(path).write_bytes(out)


def write_rgba(path, rows, pal):
    out = []
    for r in rows:
        line = bytearray()
        for c in r:
            line += bytes((0, 0, 0, 0)) if c is None else bytes(pal[c]) + b"\xff"
        out.append(line)
    _png(path, len(rows[0]), len(rows), 6, out)


def write_mask(path, rows):
    _png(path, len(rows[0]), len(rows), 0, [bytes(0 if c is None else 255 for c in r) for r in rows])


def write_indexed(path, rows, pal):
    plte = b"".join(bytes(c) for c in pal)
    trns = bytes(255 if i != TRANSPARENT else 0 for i in range(256))
    _png(path, len(rows[0]), len(rows), 3,
         [bytes(TRANSPARENT if c is None else c for c in r) for r in rows], plte, trns)


def write_sheet(path, shapes, pal, scale=2, cols=12):
    """All shapes of a set on one dark sheet, bottoms aligned, for a quick look."""
    shapes = [s for s in shapes if s[0] > 0 and s[1] > 0]
    if not shapes:
        return
    cw = max(s[0] for s in shapes) + 4
    ch = max(s[1] for s in shapes) + 4
    cols = min(cols, len(shapes))
    nrows = (len(shapes) + cols - 1) // cols
    W, H = cw * cols, ch * nrows
    bg, cell = (20, 20, 30), (40, 40, 60)
    img = [[bg] * W for _ in range(H)]
    for k, (w, h, rows) in enumerate(shapes):
        ox = (k % cols) * cw + 2
        oy = (k // cols) * ch + ch - 2 - h
        for y in range(h):
            for x in range(w):
                c = rows[y][x]
                img[oy + y][ox + x] = cell if c is None else pal[c]
    out = []
    for r in img:
        line = bytearray()
        for c in r:
            line += bytes(c) * scale
        for _ in range(scale):
            out.append(line)
    _png(path, W * scale, H * scale, 2, out)


# ---------- main ----------

def safe(name):
    return "".join(ch if ch.isalnum() or ch in " -_" else "_" for ch in name).strip().replace(" ", "_")


def find_files_dir(given):
    if given:
        return Path(given)
    for p in (ROOT / "work" / "files", ROOT / "game" / "data" / "files"):
        if (p / "files.txt").exists():
            return p
    sys.exit("Can't find the game files. Run tools/prepare.py first, or pass --files <folder with files.txt>.")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--gfx", choices=["COLOR", "LC"], default="COLOR",
                    help="COLOR = the 640x480 art (default), LC = the 512x384 art")
    ap.add_argument("--files", help="folder holding files.txt (default: work/files or game/data/files)")
    ap.add_argument("--out", help="output folder (default: work/sprites/<gfx>)")
    ap.add_argument("--guard", type=int, default=0, choices=range(8),
                    help="which guard uniform (clut 3000+n) fills palette entries 0x69-0x75")
    ap.add_argument("--no-sheets", action="store_true", help="skip the per-set contact sheets")
    a = ap.parse_args()

    files = find_files_dir(a.files)
    man = read_manifest(files)
    app = next((v for v in man.values() if v[0] == "APPL"), None)
    gfx = man.get(f"Persia({a.gfx})")
    if not app or not gfx:
        sys.exit(f"files.txt has no application or no Persia({a.gfx}) file.")
    app_res = read_resources(app[2].read_bytes())
    gfx_res = read_resources(gfx[2].read_bytes())
    data = gfx[1].read_bytes()

    base = read_clut(app_res, 2000)
    guards = {n: read_clut(app_res, 3000 + n) for n in range(8) if ("clut", 3000 + n) in app_res}
    pal = [(255, 0, 255)] * 256
    for i, c in base.items():
        pal[i] = c
    for i, c in guards.get(a.guard, {}).items():
        pal[0x69 + i] = c

    out = Path(a.out) if a.out else ROOT / "work" / "sprites" / a.gfx
    out.mkdir(parents=True, exist_ok=True)
    index = {"gfx": a.gfx, "guard_uniform": a.guard, "sets": []}
    total = 0

    for (rtype, sid), (name, shpd) in sorted(gfx_res.items(), key=lambda kv: kv[0][1]):
        if rtype != "SHPD":
            continue
        off, plen, ulen = struct.unpack(">III", shpd)
        buf = unpack_lzss(data[off:off + plen], ulen)
        table = gfx_res[("SHPT", sid)][1]
        offsets = [struct.unpack(">i", table[k:k + 4])[0] for k in range(0, len(table), 4)]
        folder = f"{sid}_{safe(name or str(sid))}"
        (out / folder).mkdir(exist_ok=True)
        entry = {"id": sid, "name": name, "folder": folder, "shapes": []}
        sheet = []
        for num, so in enumerate(offsets):
            if so < 0:
                continue
            w, h, xo, yo, rows = decode_shape(buf, so)
            rec = {"shape": num, "width": w, "height": h, "xoff": xo, "yoff": yo}
            if w > 0 and h > 0:
                stem = f"{num:03d}"
                write_rgba(out / folder / f"{stem}.png", rows, pal)
                write_mask(out / folder / f"{stem}_mask.png", rows)
                write_indexed(out / folder / f"{stem}_idx.png", rows, pal)
                rec["file"] = f"{folder}/{stem}.png"
                sheet.append((w, h, rows))
            entry["shapes"].append(rec)
            total += 1
        if not a.no_sheets:
            write_sheet(out / folder / "_sheet.png", sheet, pal)
        index["sets"].append(entry)
        print(f"  {sid:5d}  {name or '':16s} {len(entry['shapes']):4d} shapes")

    (out / "sprites.json").write_text(json.dumps(index, indent=1))
    (out / "palette.json").write_text(json.dumps({
        "note": "clut 2000; entries 0x69-0x75 hold the guard uniform for the level; 196-200 cycle for torches",
        "palette": {str(k): v for k, v in sorted(base.items())},
        "guard_uniforms": {str(3000 + n): {str(0x69 + i): c for i, c in g.items()} for n, g in guards.items()},
    }, indent=1))
    print(f"{total} shapes written to {out}")


if __name__ == "__main__":
    main()
