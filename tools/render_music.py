#!/usr/bin/env python3
r"""
render_music.py - render the Mac Prince of Persia music to .wav

The game's music is SoundMusicSys-style data inside the application's resource fork:
  SONG  -> which MIDI to play + settings (polyphony, release time)
  MIDI  -> a standard MIDI file
  INST  -> program number = INST id -> a 'snd ' sample + base note
  snd   -> the 8-bit instrument samples (with loop points)

This script plays the MIDI notes with those samples, like the game's driver does.

Usage (Windows):
  py -m pip install numpy mido
  py render_music.py "C:\pop1-recomp\pop1-recomp\work\files\disk1\1.rsrc" music_out
(you can also give it the .dc42 disk image or a MacBinary file, like extract_snd.py)
"""
import io
import os
import struct
import sys
import wave

import mido
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import extract_snd as X  # noqa: E402  (resource-fork reading + 'snd ' decoding)

OUT_RATE = 22254  # the Mac's 22,254.5 Hz output rate (what the game's 22k driver mixes at)


def load_fork(path):
    buf = open(path, "rb").read()
    fork = X.from_macbinary(buf) or X.from_appledouble(buf)
    if fork is None and X.looks_like_rsrc_fork(buf):
        fork = buf
    if fork is None:  # disk image: take the application's fork
        for name, rsrc in X.forks_from_hfs_image(buf):
            if X.looks_like_rsrc_fork(rsrc) and "MIDI" in X.read_resources(rsrc):
                return X.read_resources(rsrc)
        sys.exit("No MIDI resources found.")
    return X.read_resources(fork)


class Sample:
    def __init__(self, data):
        ch, width, rate_int, pcm = X.decode_snd(data)
        h = X.find_sound_header(data)
        _p, n, rate_fx, ls, le, _enc, base = struct.unpack(">IIIIIBB", data[h:h + 22])
        self.rate = rate_fx / 65536.0
        self.base = base or 60
        self.x = (np.frombuffer(pcm, np.uint8).astype(np.float32) - 128.0) / 128.0
        self.loop = (ls, le) if le - ls > 16 and le <= len(self.x) else None


def parse_inst(d):
    snd_id, base_note = struct.unpack(">hH", d[:4])
    return {"snd": snd_id, "base": base_note, "flags1": d[5], "flags2": d[6]}


def parse_song(d):
    midi_id, = struct.unpack(">h", d[:2])
    return {"midi": midi_id, "max_notes": d[9] or 8, "flags": d[12], "decay": d[13]}


def render_voice(s, note, base, vel, dur_s, release_s):
    """One note: sample pitched from its base note, loop held while the key is down,
    then a release fade. Returns float32 mono at OUT_RATE."""
    step = s.rate / OUT_RATE * 2.0 ** ((note - base) / 12.0)
    hold = int(dur_s * OUT_RATE)
    rel = int(release_s * OUT_RATE)
    if s.loop:
        total = hold + rel
    else:  # one-shot: plays to the end of the sample (or until released + faded)
        total = min(int(len(s.x) / step), hold + rel)
    if total <= 0:
        return np.zeros(0, np.float32)
    pos = np.arange(total, dtype=np.float64) * step
    if s.loop:
        ls, le = s.loop
        over = pos >= le
        pos[over] = ls + np.mod(pos[over] - ls, le - ls)
    pos = np.minimum(pos, len(s.x) - 1.001)
    i = pos.astype(np.int64)
    f = (pos - i).astype(np.float32)
    nxt = np.minimum(i + 1, len(s.x) - 1)
    out = s.x[i] * (1 - f) + s.x[nxt] * f
    env = np.ones(total, np.float32)
    if total > hold:
        env[hold:] = np.linspace(1, 0, total - hold, dtype=np.float32)
    return out * env * (vel / 127.0)


def midi_notes(mid):
    """Yield (start_s, dur_s, channel, program, note, velocity)."""
    t = 0.0
    tempo = 500000
    prog = {}
    held = {}
    out = []
    tpq = mid.ticks_per_beat
    for msg in mido.merge_tracks(mid.tracks):
        t += mido.tick2second(msg.time, tpq, tempo)
        if msg.type == "set_tempo":
            tempo = msg.tempo
        elif msg.type == "program_change":
            prog[msg.channel] = msg.program
        elif msg.type == "note_on" and msg.velocity:
            held[(msg.channel, msg.note)] = (t, msg.velocity, prog.get(msg.channel, 0))
        elif msg.type in ("note_off", "note_on"):
            k = (msg.channel, msg.note)
            if k in held:
                st, v, p = held.pop(k)
                out.append((st, t - st, msg.channel, p, msg.note, v))
    for (ch, n), (st, v, p) in held.items():
        out.append((st, t - st, ch, p, n, v))
    return sorted(out), t


TRANSPOSE = 12  # the driver plays every note an octave above the sample's base note
                # (checked against the game's own driver output; the MIDI data is written for it)


def render_song(res, midi_id, max_voices=None, release_s=0.25, gain=0.30):
    samples = {rid: Sample(d) for rid, _n, d in res["snd "] if len(d) > 40}
    insts = {rid: parse_inst(d) for rid, _n, d in res["INST"]}
    midi = {rid: d for rid, _n, d in res["MIDI"]}[midi_id]
    mid = mido.MidiFile(file=io.BytesIO(midi))
    notes, end = midi_notes(mid)
    length = int((end + 3.0) * OUT_RATE)
    mix = np.zeros(length, np.float32)
    voices = []  # [start_sample, rendered]
    for st, dur, ch, p, note, vel in notes:
        inst = insts.get(p)
        if not inst or inst["snd"] not in samples:
            continue
        s = samples[inst["snd"]]
        base = inst["base"] or s.base
        a = int(st * OUT_RATE)
        if max_voices:  # voice stealing: the oldest still-sounding note is cut
            live = [vc for vc in voices if vc[0] + len(vc[1]) > a]
            while len(live) >= max_voices:
                old = min(live, key=lambda vc: vc[0])
                keep = max(0, a - old[0])
                fade = min(64, keep)
                old[1] = old[1][:keep].copy()
                if fade:
                    old[1][keep - fade:] *= np.linspace(1, 0, fade, dtype=np.float32)
                live.remove(old)
        v = render_voice(s, note + TRANSPOSE, base, vel, dur, release_s)
        voices.append([a, v[: max(0, length - a)]])
    for a, v in voices:
        mix[a:a + len(v)] += v
    mix *= gain
    # trim trailing silence
    nz = np.nonzero(np.abs(mix) > 1e-3)[0]
    if len(nz):
        mix = mix[: min(len(mix), nz[-1] + int(0.2 * OUT_RATE))]
    return mix


def write_wav(fn, x):
    x = np.clip(x, -1, 1)
    with wave.open(fn, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(OUT_RATE)
        w.writeframes((x * 32767).astype("<i2").tobytes())


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    res = load_fork(sys.argv[1])
    outdir = sys.argv[2]
    os.makedirs(outdir, exist_ok=True)
    for rid, name, _d in sorted(res["MIDI"]):
        x = render_song(res, rid)
        peak = np.abs(x).max()
        if peak > 0.9:  # keep loud tracks from clipping
            x *= 0.9 / peak
        fn = os.path.join(outdir, f"{rid}_{X.safe(name.replace('.mff', ''))}.wav")
        write_wav(fn, x)
        print(f"  {os.path.basename(fn)}  {len(x) / OUT_RATE:5.1f}s  peak {np.abs(x).max():.2f}")


if __name__ == "__main__":
    main()
