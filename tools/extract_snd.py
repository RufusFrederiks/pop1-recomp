#!/usr/bin/env python3
r"""
extract_snd.py - pull sounds (snd / csnd resources) out of a classic Mac file and save them as .wav

Works on:
  * a raw resource fork (e.g. "Prince of Persia/..namedfork/rsrc", or a file from a
    folder named RESOURCE.FRK / .rsrc)
  * a MacBinary file (.bin)
  * an AppleDouble file (._Prince of Persia)
  * an HFS disk image (.dsk/.img/.hfs) - needs:  pip install machfs

Usage (Windows):
  py extract_snd.py "C:\path\to\Prince of Persia.bin" sounds_out
  py extract_snd.py "C:\path\to\pop.dsk" sounds_out
"""
import os
import re
import struct
import sys
import wave


# ---------- getting at the resource fork ----------

def from_macbinary(buf):
    """Return the resource fork from a MacBinary file, or None if it isn't one."""
    if len(buf) < 128 or buf[0] != 0 or buf[74] != 0 or not (1 <= buf[1] <= 63):
        return None
    data_len, rsrc_len = struct.unpack(">II", buf[83:91])
    start = 128 + ((data_len + 127) // 128) * 128
    if rsrc_len == 0 or start + rsrc_len > len(buf):
        return None
    return buf[start:start + rsrc_len]


def from_appledouble(buf):
    """Return the resource fork from an AppleDouble/AppleSingle file, or None."""
    if len(buf) < 26 or buf[:4] not in (b"\x00\x05\x16\x07", b"\x00\x05\x16\x00"):
        return None
    (n,) = struct.unpack(">H", buf[24:26])
    for i in range(n):
        eid, off, ln = struct.unpack(">III", buf[26 + i * 12:38 + i * 12])
        if eid == 2:  # resource fork
            return buf[off:off + ln]
    return None


def forks_from_hfs_image(buf):
    """Yield (path, resource_fork) for every file in an HFS image that has one."""
    try:
        import machfs
    except ImportError:
        sys.exit("This looks like a disk image. Install the reader first:  pip install machfs")
    # a raw volume, or a DiskCopy 4.2 image (84-byte header, data size at offset 64)
    candidates = [buf]
    if len(buf) > 84:
        (dc_size,) = struct.unpack(">I", buf[64:68])
        candidates.append(buf[84:84 + dc_size])
    vol = None
    for img in candidates:
        try:
            vol = machfs.Volume()
            vol.read(img)
            break
        except Exception:
            vol = None
    if vol is None:
        sys.exit("Could not read the disk image as HFS.")
    for path, _dirs, names in vol.walk():
        folder = vol
        for p in path:
            folder = folder[p]
        for name in names:
            f = folder[name]
            if f.rsrc:
                yield "/".join(path + (name,)), f.rsrc


def looks_like_rsrc_fork(buf):
    if len(buf) < 16:
        return False
    d_off, m_off, d_len, m_len = struct.unpack(">IIII", buf[:16])
    return d_off >= 16 and d_off + d_len <= len(buf) and m_off + m_len <= len(buf) and m_len >= 28


# ---------- reading the resource map ----------

def read_resources(fork):
    """Return {type: [(id, name, bytes), ...]} for a resource fork."""
    d_off, m_off, _, _ = struct.unpack(">IIII", fork[:16])
    type_list_off, name_list_off = struct.unpack(">HH", fork[m_off + 24:m_off + 28])
    tl = m_off + type_list_off
    nl = m_off + name_list_off
    (ntypes,) = struct.unpack(">H", fork[tl:tl + 2])
    out = {}
    for t in range((ntypes + 1) & 0xFFFF):
        e = tl + 2 + t * 8
        rtype = fork[e:e + 4].decode("mac_roman")
        count, ref_off = struct.unpack(">HH", fork[e + 4:e + 8])
        for r in range(count + 1):
            re_ = tl + ref_off + r * 12
            rid, name_off = struct.unpack(">hH", fork[re_:re_ + 4])
            data_off = int.from_bytes(fork[re_ + 5:re_ + 8], "big")
            name = ""
            if name_off != 0xFFFF:
                ln = fork[nl + name_off]
                name = fork[nl + name_off + 1:nl + name_off + 1 + ln].decode("mac_roman")
            p = d_off + data_off
            (ln,) = struct.unpack(">I", fork[p:p + 4])
            out.setdefault(rtype, []).append((rid, name, fork[p + 4:p + 4 + ln]))
    return out


# ---------- decoding a 'snd ' resource ----------

def find_sound_header(snd):
    fmt = struct.unpack(">H", snd[:2])[0]
    if fmt == 1:
        (nfmt,) = struct.unpack(">H", snd[2:4])
        p = 4 + nfmt * 6
    elif fmt == 2:
        p = 4
    else:
        raise ValueError(f"unknown 'snd ' format {fmt}")
    (ncmd,) = struct.unpack(">H", snd[p:p + 2])
    p += 2
    for i in range(ncmd):
        cmd, _p1, p2 = struct.unpack(">HHI", snd[p + i * 8:p + i * 8 + 8])
        if cmd in (0x8050, 0x8051):  # soundCmd / bufferCmd with offset into resource
            return p2
    raise ValueError("no sampled-sound command (probably a synth/note resource)")


def decode_snd(snd):
    """Return (channels, sample_width_bytes, rate, pcm_bytes_for_wav)."""
    h = find_sound_header(snd)
    _ptr, length_or_ch, rate_fixed, _ls, _le, encode, _base = struct.unpack(
        ">IIIIIBB", snd[h:h + 22])
    rate = round(rate_fixed / 65536) or 22254
    if encode == 0x00:  # standard header: 8-bit unsigned mono
        return 1, 1, rate, snd[h + 22:h + 22 + length_or_ch]
    if encode == 0xFF:  # extended header
        ch = length_or_ch
        (frames,) = struct.unpack(">I", snd[h + 22:h + 26])
        (bits,) = struct.unpack(">H", snd[h + 48:h + 50])
        width = bits // 8
        data = snd[h + 64:h + 64 + frames * ch * width]
        if width == 2:  # Mac is big-endian signed, WAV wants little-endian
            data = b"".join(data[i + 1:i + 2] + data[i:i + 1] for i in range(0, len(data) - 1, 2))
        return ch, width, rate, data
    if encode == 0xFE:
        raise ValueError("compressed sound (MACE/IMA4) - not handled by this script")
    raise ValueError(f"unknown header encoding 0x{encode:02X}")


def unpack_csnd(csnd):
    """SoundMusicSys 'csnd': u32 (delta type << 24 | size), LZSS, then delta coding.
    Returns the plain 'snd ' resource it was made from."""
    (hdr,) = struct.unpack(">I", csnd[:4])
    dtype, size = hdr >> 24, hdr & 0xFFFFFF
    src, out, i = csnd[4:], bytearray(), 0
    while i < len(src) and len(out) < size:
        flags = src[i]
        i += 1
        for bit in range(8):
            if i >= len(src) or len(out) >= size:
                break
            if flags & (1 << bit):            # literal byte
                out.append(src[i])
                i += 1
            else:                             # back-reference: 4-bit len, 12-bit distance
                prm = (src[i] << 8) | src[i + 1]
                i += 2
                start = len(out) - (0x1000 - (prm & 0xFFF))
                for k in range((prm >> 12) + 3):
                    out.append(out[start + k] if start + k >= 0 else 0)
    out = out[:size]
    if len(out) < size:
        raise ValueError("csnd data ended early")
    step = {0: (1, 1), 1: (2, 1), 2: (1, 2), 3: (2, 2), 0xFF: None}.get(dtype, "bad")
    if step == "bad":
        raise ValueError(f"unknown csnd type {dtype}")
    if step:                                  # undo delta coding (over the whole resource)
        dist, width = step
        if width == 1:
            for x in range(dist, len(out)):
                out[x] = (out[x] + out[x - dist]) & 0xFF
        else:
            v = list(struct.unpack(f">{len(out) // 2}H", out[:len(out) // 2 * 2]))
            for x in range(dist, len(v)):
                v[x] = (v[x] + v[x - dist]) & 0xFFFF
            out[:len(v) * 2] = struct.pack(f">{len(v)}H", *v)
    return bytes(out)


def safe(s):
    return re.sub(r'[<>:"/\\|?*\x00-\x1f]', "_", s).strip() or "unnamed"


def extract(fork, outdir, label=""):
    res = read_resources(fork)
    snds = [("snd", rid, name, d) for rid, name, d in res.get("snd ", [])]
    snds += [("csnd", rid, name, d) for rid, name, d in res.get("csnd", [])]
    if not snds:
        print(f"{label}no 'snd '/'csnd' resources. Types present: {', '.join(sorted(res))}")
        return 0
    os.makedirs(outdir, exist_ok=True)
    n = 0
    for kind, rid, name, data in sorted(snds, key=lambda s: (s[1], s[0])):
        base = f"{kind}_{rid}_{safe(name)}" if name else f"{kind}_{rid}"
        fn = os.path.join(outdir, base + ".wav")
        try:
            ch, width, rate, pcm = decode_snd(unpack_csnd(data) if kind == "csnd" else data)
            if not pcm:
                raise ValueError("empty (no samples)")
        except Exception as e:
            print(f"  skip {kind} {rid} {name!r}: {e}")
            continue
        with wave.open(fn, "wb") as w:
            w.setnchannels(ch)
            w.setsampwidth(width)
            w.setframerate(rate)
            w.writeframes(pcm)
        print(f"  {os.path.basename(fn)}  ({rate} Hz, {8 * width}-bit, {ch} ch)")
        n += 1
    return n


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    src, outdir = sys.argv[1], sys.argv[2]
    buf = open(src, "rb").read()

    fork = from_macbinary(buf) or from_appledouble(buf)
    if fork is None and looks_like_rsrc_fork(buf):
        fork = buf
    if fork is not None:
        total = extract(fork, outdir)
    else:
        total = 0
        for path, rsrc in forks_from_hfs_image(buf):
            if not looks_like_rsrc_fork(rsrc):
                continue
            print(f"{path}:")
            total += extract(rsrc, os.path.join(outdir, safe(path.replace("/", "_"))), "  ")
    print(f"\n{total} wav file(s) written to {outdir}")


if __name__ == "__main__":
    main()
