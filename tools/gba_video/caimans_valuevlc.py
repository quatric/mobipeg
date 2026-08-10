#!/usr/bin/env python3
"""Reconstruct the Caimans Pro leaf *value* VLCs (the block DC term).

`FUN_03004cd4` decodes a leaf as ``out = clamp(pred + value + sum of N
codebook patterns)``. This module recovers the coding of ``value``.

Both paths peek 32 MSB-aligned bits and dispatch through a ladder of
magnitude comparisons into parallel value/length byte tables, at a
progressively finer bit granularity for smaller values -- a hand-rolled
multi-tier VLC. The inter path reads its values **signed** (a residual DC);
the intra path reads them **unsigned** (an absolute 0..255 level).

The two decoders below are direct transcriptions of the decompiled ladders.
Codewords are then recovered by driving each decoder over every candidate
prefix and keeping the prefixes it consumes whole, which is the same
emulate-then-invert method that cracked the MV table.

Run directly to print both code tables and the consistency checks:

    python3 tools/gba_video/caimans_valuevlc.py
"""
import os
import struct

from caimans_codebooks import Images

MAX_CODE_BITS = 24

# Literal-pool values, named by the DAT_ symbol that holds them.
DAT_030057D8 = 0x03001100
DAT_030057DC = 0x030011E0
DAT_030057F0 = 0x03000C20
DAT_030057F4 = 0x03000D20
DAT_0300580C = 0x03001060
DAT_03005810 = 0x03000AE0
DAT_03005814 = 0x03000B80
DAT_03005818 = 0x03000EA0
DAT_0300581C = 0x03000F80
DAT_03005824 = 0x030009E0
DAT_03005828 = 0x03000A60
DAT_0300582C = 0x03000E20
DAT_03005830 = 0x03000E60
DAT_03005834 = 0x030008A0
DAT_03005838 = 0x03000940
DAT_0300583C = 0x03000800
DAT_03005840 = 0x030005A0


class Tables:
    """Byte/halfword reads against the IWRAM image, by GBA address."""

    def __init__(self, img=None):
        self.img = img or Images()

    def u8(self, addr):
        return self.img.read(addr, 1)[0]

    def s8(self, addr):
        b = self.u8(addr)
        return b - 256 if b > 127 else b

    def s16(self, addr):
        return struct.unpack("<h", self.img.read(addr, 2))[0]


def decode_inter(t, v):
    """Signed residual DC. Returns (value, bits_consumed).

    Transcribed from the `param_3 == 0` ladder at 0x03004e70.
    """
    if v < 0x0B000000:
        if v < 0x01200000:
            if v < 0x002E0000:
                if v < 0x00094000:
                    if v < 0x00049000:
                        idx = v >> 10
                        n = 0x16 if idx < 0x106 else 0x15
                        return t.s16(DAT_03005840 + idx * 2), n
                    idx = v >> 0xC
                    n = 0x14 if idx < 0x5A else 0x13
                    return t.s16(DAT_0300583C + idx * 2 - 0x92), n
                idx = v >> 0xE
                return t.s8(DAT_03005838 + idx - 0x25), t.u8(DAT_03005834 + idx - 0x25)
            idx = v >> 0x11
            return t.s8(DAT_03005828 + idx - 0x17), t.u8(DAT_03005824 + idx - 0x17)
        idx = v >> 0x14
        return t.s8(DAT_03005814 + idx - 0x12), t.u8(DAT_03005810 + idx - 0x12)
    idx = v >> 0x18
    return t.s8(DAT_030057F4 + idx - 0x0B), t.u8(DAT_030057F0 + idx - 0x0B)


def decode_intra(t, v):
    """Unsigned absolute level. Returns (value, bits_consumed).

    Transcribed from the `param_3 != 0` ladder at 0x03004f30-ish.
    """
    if v > 0x24FFFFFF:
        idx = v >> 0x18
        return t.u8(DAT_030057DC + idx - 0x25), t.u8(DAT_030057D8 + idx - 0x25)
    if v < 0x03400000:
        if v >= 0x00040000:
            idx = v >> 0x12
            return t.u8(DAT_0300581C + idx - 1), t.u8(DAT_03005818 + idx - 1)
        idx = v >> 0xC
        return t.u8(DAT_03005830 + idx), t.u8(DAT_0300582C + idx)
    idx = v >> 0x16
    n = 10 if idx < 0x2C else 9
    return t.u8(DAT_0300580C + idx - 0x0D), n


def invert(decoder, t, max_bits=MAX_CODE_BITS):
    """Recover the codeword table by driving the decoder over every prefix.

    A prefix of length L is a genuine codeword when the decoder consumes
    exactly L bits for it *and* does so regardless of what follows -- checked
    by padding the tail with both zeros and ones. Prefixes of already-accepted
    codewords are skipped, which keeps the walk prefix-free by construction.
    """
    codes = {}
    for length in range(1, max_bits + 1):
        for code in range(1 << length):
            bits = format(code, "0%db" % length)
            if any(bits.startswith(c) for c in codes):
                continue
            tail = max_bits + 8 - length
            lo = decoder(t, (code << (32 - length)) & 0xFFFFFFFF)
            hi = decoder(t, ((code << (32 - length)) | ((1 << (32 - length)) - 1)) & 0xFFFFFFFF)
            if lo != hi or lo[1] != length:
                continue
            codes[bits] = lo[0]
    return codes


def check_kraft(codes):
    """Sum of 2^-len over the code set. 1.0 means a complete prefix code."""
    return sum(2.0 ** -len(c) for c in codes)


def check_prefix_free(codes):
    ordered = sorted(codes)
    for i, a in enumerate(ordered):
        for b in ordered[i + 1:]:
            if b.startswith(a):
                return (a, b)
    return None


def report(name, codes):
    print("=== %s value VLC: %d codewords" % (name, len(codes)))
    vals = sorted(codes.values())
    lens = [len(c) for c in codes]
    print("    value range %+d .. %+d, code lengths %d..%d, Kraft sum %.6f"
          % (vals[0], vals[-1], min(lens), max(lens), check_kraft(codes)))
    clash = check_prefix_free(codes)
    print("    prefix-free: %s" % ("yes" if clash is None else "NO -- %s vs %s" % clash))
    dupes = len(vals) - len(set(vals))
    print("    distinct values: %d (%d duplicate assignments)" % (len(set(vals)), dupes))
    print("    shortest codes:")
    for c in sorted(codes, key=lambda c: (len(c), c))[:12]:
        print("        %-14s -> %+d" % (c, codes[c]))
    print("    longest codes:")
    for c in sorted(codes, key=lambda c: (-len(c), c))[:4]:
        print("        %-14s -> %+d" % (c, codes[c]))


def load(img=None):
    """Return {"intra": {code: value}, "inter": {code: value}}."""
    t = Tables(img)
    return {"inter": invert(decode_inter, t), "intra": invert(decode_intra, t)}


def main():
    tables = load()
    for name in ("inter", "intra"):
        report(name, tables[name])
        print()

    # The coverage each ladder tier is responsible for should partition the
    # 32-bit peek space with no value left undecodable.
    t = Tables()
    for name, dec in (("inter", decode_inter), ("intra", decode_intra)):
        bad = 0
        for i in range(0, 1 << 12):
            v = i << 20
            try:
                val, n = dec(t, v)
                if not 1 <= n <= MAX_CODE_BITS:
                    bad += 1
            except Exception:
                bad += 1
        print("%s ladder: %d/%d sampled peeks decoded to a sane length"
              % (name, (1 << 12) - bad, 1 << 12))


if __name__ == "__main__":
    main()
