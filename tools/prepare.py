#!/usr/bin/env python3
"""Turn your own Prince of Persia disk images into what the build needs.

    python tools/prepare.py [original/]

Looks in the given folder (default: original/) for the three DiskCopy 4.2
images, then:

  1. extracts the application on disk 1 into work/app/
     (code/CODE_<n>.bin, rsrc/<TYPE>_<id>.bin, jumptable.json, inventory.json)
  2. writes every file on every disk (data + resource forks) into
     work/files/disk<n>/, plus a combined work/files/files.txt manifest the
     runtime reads to serve the File Manager
  3. decodes the two music drivers (MDRV 11 and 22, which ship encrypted and
     compressed) into work/app/code/MDRV_<id>.bin
  4. disassembles CODE 1-4 and the drivers into work/disasm/ (for reading)
  5. lifts all of it to C in src/gen/ with tools/lift.py (macrecomp's
     lift68k.py plus a few 68000 forms it lacks)

Everything it writes is derived from the game, so work/ and src/gen/ are
gitignored. Nothing here downloads anything; bring your own copy.
"""
import json
import os
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
MR = ROOT / "ext" / "macrecomp" / "tools"
WORK = ROOT / "work"
GEN = ROOT / "src" / "gen"

# Segments of the application, and the extra function starts the lifter
# cannot find on its own (computed-jump targets). Grows as the bring-up finds
# them; see DEV-LOG.md.
SEGMENTS = [1, 2, 3, 4]
ENTRIES = {}

# The music drivers are lifted as pseudo-segments named after their resource
# id: MDRV 11 ("11k Note and MIDI driver") -> fn_11_*, MDRV 22 -> fn_22_*.
DRIVERS = [11, 22]


def mdrv_decrypt(b):
    """CODE 2 0x44fe, applied from byte 7 on by CODE 2 0x4532: the eexec-style
    stream cipher (seed 0xDCE5, c1 52845, c2 22719)."""
    b = bytearray(b)
    r = 0xDCE5
    for i in range(7, len(b)):
        c = b[i]
        b[i] = c ^ (r >> 8)
        r = ((c + r) * 0xCE6D + 0x58BF) & 0xFFFF
    return bytes(b)


def mdrv_unpack(src, n):
    """CODE 2 0x4c98: LZSS. A flag byte, read LSB first: 1 = literal byte,
    0 = big-endian word, low 12 bits a position and high 4 bits a length;
    copy length+3 bytes from (output - 4096 + position)."""
    out = bytearray()
    i = 0
    while i < len(src) and len(out) < n:
        flags = src[i]; i += 1
        for _ in range(8):
            if len(out) >= n or i >= len(src):
                break
            if flags & 1:
                out.append(src[i]); i += 1
            else:
                w = (src[i] << 8) | src[i + 1]; i += 2
                pos = len(out) - 4096 + (w & 0xFFF)
                for k in range((w >> 12) + 3):
                    if len(out) >= n:
                        break
                    if pos + k < 0:
                        raise ValueError("reference before the start of the output")
                    out.append(out[pos + k])
            flags >>= 1
    if len(out) != n:
        raise ValueError(f"unpacked {len(out)} of {n} bytes")
    return bytes(out)


def decode_drivers():
    for d in DRIVERS:
        raw = (WORK / "app" / "rsrc" / f"MDRV_{d}.bin").read_bytes()
        plain = mdrv_decrypt(raw)
        n = int.from_bytes(plain[:4], "big")
        code = mdrv_unpack(plain[4:], n)
        # 4 dummy bytes stand in for a CODE resource's header, so that the
        # lifter's offsets are offsets into the block the game unpacks to.
        (WORK / "app" / "code" / f"MDRV_{d}.bin").write_bytes(b"\0\0\0\0" + code)
        print(f"MDRV {d}: {len(raw)} -> {n} bytes")
    # The lifter wants a jump table; a driver's only entry point is offset 0.
    (WORK / "app" / "mdrv_jt.json").write_text(json.dumps({"entries": [
        {"idx": i, "offset": 0, "segment": d, "thunk": True} for i, d in enumerate(DRIVERS)]}))


def run(*args, check=True):
    print(">>", " ".join(str(a) for a in args[1:]))
    subprocess.run([sys.executable, *map(str, args)], check=check)


def find_images(folder):
    imgs = {}
    for p in sorted(Path(folder).iterdir()):
        if p.suffix.lower() not in (".dc42", ".image", ".img", ".dsk"):
            continue
        m = re.search(r"disk\s*(\d)", p.name, re.I)
        if m:
            imgs[int(m.group(1))] = p
    if 1 not in imgs:
        sys.exit(f"no 'Disk 1' image found in {folder} -- bring your own copy (see README)")
    return imgs


def main():
    folder = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "original"
    imgs = find_images(folder)
    for n, p in sorted(imgs.items()):
        print(f"disk {n}: {p.name}")

    # 1. the application
    run(MR / "extract_resources.py", imgs[1], "-o", WORK / "app")

    # 2. every file on every disk, for the File Manager
    manifest = []
    for n, p in sorted(imgs.items()):
        out = WORK / "files" / f"disk{n}"
        # Disks 2 and 3 hold no application, so the extractor reports an
        # error after writing the forks; only the forks are wanted here.
        run(MR / "extract_resources.py", p, "-o", WORK / "files" / f"_scratch{n}",
            "--forks", out, check=False)
        if not (out / "files.json").exists():
            sys.exit(f"could not read the files on {p.name}")
        idx = json.loads((out / "files.json").read_text())
        for f in idx["files"]:
            dpath = out / f"{f['n']}.data" if f["data"] else None
            rpath = out / f"{f['n']}.rsrc" if f["rsrc"] else None
            manifest.append((f["name"], f["type"], f["creator"],
                             dpath, f["data"], rpath, f["rsrc"]))
    # Mac Roman, not UTF-8: names, types and creators are compared byte for
    # byte with the guest's own strings (the creator is 'PoƒP', 4 bytes).
    with open(WORK / "files" / "files.txt", "w", encoding="mac_roman", newline="\n") as fh:
        fh.write("# name<TAB>type<TAB>creator<TAB>datapath<TAB>datalen<TAB>rsrcpath<TAB>rsrclen\n")
        for name, t, c, dp, dl, rp, rl in manifest:
            rel = lambda q: os.path.relpath(q, WORK).replace("\\", "/") if q else "-"
            fh.write(f"{name}\t{t}\t{c}\t{rel(dp)}\t{dl}\t{rel(rp)}\t{rl}\n")
    print(f"{len(manifest)} files -> work/files/files.txt")

    # 3. the music drivers
    decode_drivers()

    # 4. disassembly, for reading during bring-up (offsets are file offsets:
    #    the lifter's segment offsets are 4 less)
    (WORK / "disasm").mkdir(parents=True, exist_ok=True)
    for name in [f"CODE_{s}" for s in SEGMENTS] + [f"MDRV_{d}" for d in DRIVERS]:
        with open(WORK / "disasm" / f"{name}.s", "w") as fh:
            subprocess.run([sys.executable, str(MR / "disasm_code.py"),
                            str(WORK / "app" / "code" / f"{name}.bin"), "--count", "0"],
                           check=True, stdout=fh)

    # 5. lift
    GEN.mkdir(parents=True, exist_ok=True)
    for s in SEGMENTS:
        args = [ROOT / "tools" / "lift.py", WORK / "app" / "code" / f"CODE_{s}.bin",
                "--seg", s, "--jt", WORK / "app" / "jumptable.json", "-o", GEN]
        if s in ENTRIES:
            args += ["--entry", ",".join(f"0x{e:x}" for e in ENTRIES[s])]
        run(*args)
    for d in DRIVERS:
        run(ROOT / "tools" / "lift.py", WORK / "app" / "code" / f"MDRV_{d}.bin",
            "--seg", d, "--jt", WORK / "app" / "mdrv_jt.json", "-o", GEN)
    print("done: now build (see README)")


if __name__ == "__main__":
    main()
