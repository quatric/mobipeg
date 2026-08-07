#!/usr/bin/env python3
"""
VX++ GBA codec decoder simulation, rebuilt from ROM + the disassembly recorded in
VXpp_GBA_Codec_Handoff.md (scratchpad + /tmp/gbavx were lost to a reboot).

Faithful port of FUN_03005794's coefficient loop, incl. all 3 escape variants.

Key ARM detail (0x0300580c-0x03005818), the bug fixed this pass:
    add r12, r12, r5, lsl #2   ; cpos += run
    str r6, [r12], #4          ; coeffs[cpos] = value ; cpos += 1   <-- POST-INCREMENT
    tst r4, #1
    beq <loop start>           ; bit15==0 -> decode another coefficient
"""
import os
import sys

ROM = os.environ.get("VXPP_ROM", "")  # path to the GBA Video cart dump
VLC = "/tmp/gbavx/vlc_rom_full4096.bin"
VOFS = "/tmp/gbavx/value_offset_table.bin"
ROFS = "/tmp/gbavx/run_offset_table.bin"
STREAM = "/tmp/gbavx/stream00.video"

CBP_PERMTAB = [
    0x00,0x0f,0x1f,0x08,0x02,0x01,0x04,0x3f,0x0a,0x05,0x0e,0x0b,0x03,0x0c,0x10,0x0d,
    0x07,0x2f,0x06,0x09,0x20,0x1b,0x1e,0x17,0x1a,0x1d,0x15,0x11,0x13,0x12,0x18,0x14,
    0x1c,0x37,0x3b,0x3e,0x19,0x2b,0x21,0x27,0x16,0x2a,0x2e,0x25,0x22,0x3d,0x2d,0x28,
    0x24,0x35,0x23,0x3a,0x33,0x2c,0x29,0x30,0x26,0x31,0x3c,0x32,0x39,0x36,0x34,0x38,
]

# mode index < 12 -> prediction only (skip); >= 12 -> same predictor + residual
MODE_SKIP_MAX = 12
# modes whose handler reads 2 extra ue(v) sub-mode codes (FUN_03000884: intra luma+chroma)
INTRA_MODES = {6, 18}


def load(p):
    with open(p, "rb") as f:
        return f.read()


def load16(p):
    r = load(p)
    return [r[i] | (r[i + 1] << 8) for i in range(0, len(r), 2)]


class Bits:
    """Bit source matching FUN_0300076c/030007a8's real refill mechanism: the ARM
    code does `ldrh r10,[r1],#2` -- a little-endian HALFWORD load -- then treats the
    result MSB-first. That means each 2-byte pair is byte-SWAPPED relative to file
    order before the MSB-first bit interpretation applies (confirmed live: the first
    two stream bytes `69 04` load as halfword 0x0469, which decodes as ue(v)=34,
    matching a real mGBA capture -- naive raw-byte-order MSB-first reading of `69 04`
    gives 2, which is wrong). Precompute the swap once so peek32 stays a simple
    MSB-first byte reader.
    """
    def __init__(self, data):
        n = len(data) & ~1
        swapped = bytearray(len(data))
        for i in range(0, n, 2):
            swapped[i] = data[i + 1]
            swapped[i + 1] = data[i]
        if len(data) & 1:
            swapped[-1] = data[-1]
        self.d = bytes(swapped)

    def peek32(self, pos):
        acc = 0
        bi, bo = divmod(pos, 8)
        got = 0
        while got < 32:
            b = self.d[bi] if bi < len(self.d) else 0
            avail = 8 - bo
            take = min(avail, 32 - got)
            acc = (acc << take) | ((b >> (avail - take)) & ((1 << take) - 1))
            got += take
            bo += take
            if bo == 8:
                bo = 0
                bi += 1
        return acc & 0xFFFFFFFF


def sext(v, bits):
    m = 1 << (bits - 1)
    return (v ^ m) - m


def read_ue(br, pos):
    """FUN_0300076c: unsigned exp-golomb -> (value, new_pos)."""
    p = pos
    n = 0
    while True:
        if (br.peek32(p) >> 31) & 1:
            p += 1
            break
        p += 1
        n += 1
        if n > 31:
            return None, p
    if n == 0:
        return 0, p
    suffix = br.peek32(p) >> (32 - n)
    return suffix + (1 << n) - 1, p + n


def decode_one(br, tab, vofs, rofs, pos):
    """One coefficient. Returns (value, run, last, new_pos, tag)."""
    acc = br.peek32(pos)
    if (acc >> 25) != 3:
        cell = tab[acc >> 20]
        ln = cell & 0xF
        val = (cell >> 4) & 0x1F
        run = (cell >> 9) & 0x3F
        last = (cell >> 15) & 1
        if ln and (acc >> (32 - ln)) & 1:
            val = -val
        return val, run, last, pos + ln, ("N", acc >> 20)

    if not ((acc >> 24) & 1):                    # variant 1: value offset
        p = pos + 8
        a = br.peek32(p)
        cell = tab[a >> 20]
        ln = cell & 0xF
        val = ((cell >> 4) & 0x1F) + vofs[cell >> 9]
        run = (cell >> 9) & 0x3F
        last = (cell >> 15) & 1
        if ln and (a >> (32 - ln)) & 1:
            val = -val
        return val, run, last, p + ln, ("V1", a >> 20)

    if not ((acc >> 23) & 1):                    # variant 2: run offset
        p = pos + 9
        a = br.peek32(p)
        cell = tab[a >> 20]
        ln = cell & 0xF
        val = (cell >> 4) & 0x1F
        last = (cell >> 15) & 1
        run = ((cell >> 9) & 0x3F) + rofs[last * 0x40 + val]
        if ln and (a >> (32 - ln)) & 1:
            val = -val
        return val, run, last, p + ln, ("V2", a >> 20)

    p = pos + 9                                  # variant 3: raw literal
    last = (br.peek32(p) >> 31) & 1
    p += 1
    run = br.peek32(p) >> 26
    p += 6
    val = sext(br.peek32(p) >> 20, 12)
    p += 12
    return val, run, last, p, ("V3", None)


def decode_block(br, tab, vofs, rofs, pos, stop_when_set=True, verbose=False):
    """Returns (coeffs, new_pos, overflow)."""
    co = [0] * 16
    cpos = 0
    over = False
    for _ in range(64):
        val, run, last, npos, tag = decode_one(br, tab, vofs, rofs, pos)
        cpos += run
        if cpos < 16:
            co[cpos] = val
        else:
            over = True
        cpos += 1                                # post-increment (str r6,[r12],#4)
        if verbose:
            print(f"      [{pos}:{npos}] {tag} run={run} last={last} val={val} slot={cpos-1}")
        pos = npos
        if last if stop_when_set else not last:
            return co, pos, over
    return co, pos, True


def decode_mb(br, tab, vofs, rofs, pos, stop_when_set=True, verbose=False):
    """One 16x16 MB. Returns (dict, new_pos) or (None, pos) on desync."""
    mode, pos = read_ue(br, pos)
    if mode is None or mode >= 24:
        return None, pos, f"bad mode {mode}"
    r = {"mode": mode, "groups": [], "blocks": 0, "over": False}
    if mode in INTRA_MODES:
        lm, pos = read_ue(br, pos)
        cm, pos = read_ue(br, pos)
        if lm is None or cm is None or lm >= 4 or cm >= 4:
            return None, pos, f"bad intra submode {lm}/{cm}"
        r["intra"] = (lm, cm)
    if mode < MODE_SKIP_MAX:
        return r, pos, None
    for g in range(4):
        cbp, pos = read_ue(br, pos)
        if cbp is None or cbp >= 64:
            return None, pos, f"bad cbp {cbp} in group {g}"
        mask = CBP_PERMTAB[cbp]
        r["groups"].append((cbp, mask))
        if verbose:
            print(f"    group{g} cbp={cbp} mask={mask:06b}")
        for b in range(6):
            if mask & (1 << b):
                co, pos, ov = decode_block(br, tab, vofs, rofs, pos,
                                           stop_when_set, verbose)
                r["blocks"] += 1
                r["over"] |= ov
                if verbose:
                    print(f"      -> {['Y0','Y1','Y2','Y3','Cb','Cr'][b]}: {co}")
    return r, pos, None


def run(stop_when_set, n_mb=105, quiet=True):
    tab = load16(VLC)
    vofs, rofs = load(VOFS), load(ROFS)
    br = Bits(load(STREAM))
    pos = 0
    ok = over = 0
    for m in range(n_mb):
        r, npos, err = decode_mb(br, tab, vofs, rofs, pos, stop_when_set)
        if err:
            return ok, over, m, err, pos
        if r["over"]:
            over += 1
        if not quiet:
            print(f"MB{m:3d} mode={r['mode']:2d} blocks={r['blocks']:2d} "
                  f"bits={npos-pos}{' OVERFLOW' if r['over'] else ''}")
        pos = npos
        ok += 1
    return ok, over, None, None, pos


if __name__ == "__main__":
    for sws in (True, False):
        label = "stop_when_set=True (disassembly)" if sws else "stop_when_set=False (legacy guess)"
        ok, over, failm, err, pos = run(sws)
        print(f"{label}:")
        print(f"   MBs decoded: {ok}/105   with-overflow: {over}")
        if err:
            print(f"   FAILED at MB{failm}: {err} (bit {pos})")
        else:
            print(f"   full frame OK, ended at bit {pos}")
        print()
