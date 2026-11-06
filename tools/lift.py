#!/usr/bin/env python3
"""macrecomp's lift68k.py, plus the few 68000 forms Prince of Persia's music
driver needs and the lifter does not have yet.

    python tools/lift.py <same arguments as lift68k.py>

It imports ext/macrecomp/tools/lift68k.py as a module, wraps its per-
instruction emitter, and then runs its main() unchanged, so the submodule stays
untouched and everything else is lifted exactly as upstream would. Anything
added here is a candidate to send upstream.

Added:
  ori/andi/eori  #imm,ccr     the driver returns errors in the carry flag
  ori/andi/eori  #imm,sr      the same on the low byte; the interrupt mask in
                              the high byte is kept in pop_ipl, so the trap
                              layer does not run a VBL task inside a critical
                              section
  move.w sr,<ea> / move.w <ea>,sr / move.w <ea>,ccr
  movep.w/.l  Dn,d(An) and d(An),Dn   (byte lanes, every other address)
  (An,Xn.s*k) and d(An,Xn.s*k)          68020 scaled index (k = 1, 2, 4, 8).
                              Prince of Persia's 4- and 8-bit sprite blitters
                              (CODE 2 0x3a24, 0x3dc4) index their handle
                              tables with (a0,d1.w*4); every colour Mac had a
                              68020 or better, so the colour path uses them.
                              The lifter's "not a 68000 instruction, so not a
                              function start" rule is relaxed for this form
                              only (memory-indirect still rejects a start).
"""
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "ext" / "macrecomp" / "tools"))
import lift68k as L  # noqa: E402

# 68000 condition codes in the status register's low byte.
CCR_BITS = [(0x10, "x"), (0x08, "n"), (0x04, "z"), (0x02, "v"), (0x01, "c")]

# The CCR as a C expression, and a statement setting the flags from one.
CCR_R = "(uint32_t)((M.x<<4)|(M.n<<3)|(M.z<<2)|(M.v<<1)|M.c)"
def ccr_w(v):
    return " ".join(f"M.{f}=(({v})>>{b.bit_length()-1})&1;" for b, f in CCR_BITS)

IPL = "{ extern int pop_ipl; %s }"
# The status register: supervisor bit, interrupt mask (pop_ipl), CCR. Used
# inside a block that has declared pop_ipl.
SR_R = "(uint32_t)(0x2000|(pop_ipl<<8)|%s)" % CCR_R


def imm(tok):
    """#$1f (hex, as capstone prints it) or #31."""
    m = re.fullmatch(r"#(\$?)([0-9a-fA-F]+)", tok.strip())
    if not m:
        return None
    return int(m.group(2), 16 if m.group(1) else 10)


def emit_extra(ins):
    mn = ins.mnemonic
    ops = [o.strip() for o in L.ops_of(ins)]
    base = mn.split(".")[0]

    # ori/andi/eori to ccr or sr
    if base in ("ori", "andi", "eori") and len(ops) == 2 and ops[1] in ("ccr", "sr"):
        v = imm(ops[0])
        if v is None:
            return None
        op = {"ori": "|", "andi": "&", "eori": "^"}[base]
        C = []
        lo = v & 0xFF if ops[1] == "sr" else v
        if base == "andi" and ops[1] == "ccr":
            lo |= 0xE0                       # bits above X are not flags
        C.append("{ uint32_t _c=%s %s 0x%xu; %s }" % (CCR_R, op, lo, ccr_w("_c")))
        if ops[1] == "sr":
            hi = (v >> 8) & 7
            if base == "andi":
                C.append(IPL % f"pop_ipl &= {hi};")
            elif base == "ori":
                C.append(IPL % f"pop_ipl |= {hi};")
            else:
                C.append(IPL % f"pop_ipl ^= {hi};")
        return C

    # move to/from sr and ccr
    if base == "move" and len(ops) == 2 and "sr" in (ops[0], ops[1]) or \
       base == "move" and len(ops) == 2 and ops[1] == "ccr":
        if ops[0] == "sr":
            b = L.parse(ops[1], 2)
            if not b:
                return None
            return list(b.pre) + ["{ extern int pop_ipl; uint32_t _r=%s; %s }" % (SR_R, b.w("_r"))] + list(b.post)
        a = L.parse(ops[0], 2)
        if not a:
            return None
        C = list(a.pre) + ["{ uint32_t _r=%s; %s" % (a.r, ccr_w("_r"))]
        if ops[1] == "sr":
            C[-1] += " " + IPL % "pop_ipl = (_r>>8)&7;"
        C[-1] += " }"
        return C + list(a.post)

    # movep: every other byte, starting at d(An)
    if base == "movep" and len(ops) == 2:
        sz = 4 if mn.endswith(".l") else 2
        if ops[0].startswith("d"):
            d = L.dnum(ops[0])
            m = re.fullmatch(r"(-?)\$?([0-9a-fA-F]+)?\((a\d)\)", ops[1].replace(" ", ""))
            if d is None or not m:
                return None
            disp = int(m.group(2) or "0", 16) * (-1 if m.group(1) else 1)
            a = L.anum(m.group(3))
            addr = f"(M.a[{a}]+({disp}))"
            return ["{ uint32_t _v=M.d[%d]; %s }" % (d, " ".join(
                f"m68k_w8({addr}+{2*i},_v>>{8*(sz-1-i)});" for i in range(sz)))]
        else:
            d = L.dnum(ops[1])
            m = re.fullmatch(r"(-?)\$?([0-9a-fA-F]+)?\((a\d)\)", ops[0].replace(" ", ""))
            if d is None or not m:
                return None
            disp = int(m.group(2) or "0", 16) * (-1 if m.group(1) else 1)
            a = L.anum(m.group(3))
            addr = f"(M.a[{a}]+({disp}))"
            val = "|".join(f"(m68k_r8({addr}+{2*i})<<{8*(sz-1-i)})" for i in range(sz))
            if sz == 4:
                return ["M.d[%d]=%s;" % (d, val)]
            return ["M.d[%d]=(M.d[%d]&0xFFFF0000u)|(%s);" % (d, d, val)]
    return None


# ---- 68020 scaled index ----
# capstone prints "(a0, d1.w * 4)" / "$10(a3, d1.l * 2)"; norm_op squeezes the
# space after the comma but not around the '*'.
_SCALED = re.compile(r"(-?\$?[0-9a-fA-F]+)?\(a(\d),(d|a)(\d)\.(w|l)\s*\*\s*([1248])\)")
_orig_parse = L.parse


def parse(tok, sz):
    t = L.norm_op(tok)
    m = _SCALED.fullmatch(t)
    if m:
        d = int(m.group(1).replace("$", "0x"), 0) if m.group(1) else 0
        n = int(m.group(2)); xr = m.group(3); xn = int(m.group(4))
        idx = f"M.{xr}[{xn}]" if m.group(5) == "l" else f"(int32_t)(int16_t)M.{xr}[{xn}]"
        return L.mem(f"(M.a[{n}]+({d})+({idx})*{m.group(6)})", sz)
    return _orig_parse(tok, sz)


L.parse = parse

_orig_not_68000 = L.not_68000


def not_68000(code, start, probe=16, window=96):
    """As upstream, but a scaled index alone no longer disqualifies a start."""
    seen = 0
    for ins in L.md.disasm(code[start:min(start + window, len(code))], start):
        if seen >= probe:
            break
        seen += 1
        op = ins.op_str
        if "([" in op or "invalid" in op:
            return True
    return False


L.not_68000 = not_68000

_orig_emit_inner = L.emit_inner


def emit_inner(ins, targets):
    extra = emit_extra(ins)
    if extra is not None:
        return extra, False
    return _orig_emit_inner(ins, targets)


L.emit_inner = emit_inner

if __name__ == "__main__":
    L.main()
