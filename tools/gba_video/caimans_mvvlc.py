#!/usr/bin/env python3
"""The Caimans Pro motion-vector VLC.

Transcription of the bit-read ladder inline in `FUN_03005848`. One MV
component (dx or dy) at a time: a 1-bit escape for zero, else a two-tier
magnitude+sign ladder very like the leaf value VLC in `caimans_valuevlc`.

    if peek32 has its top bit set:           value = 0, length = 1
    elif peek32 < 0x06000000:                small-magnitude tier
    else:                                     large-magnitude tier

Each tier looks up (magnitude, total_length) from a byte table indexed by a
handful of top bits, then re-examines bit `total_length - 1` of the *same*
peeked word as the delta's sign -- so `total_length` already counts the sign
bit, and no separate sign read happens.

This was already established (a prior session) to reconstruct bit-exact
`ff_mvtab`, with median-of-three prediction and 6-bit `(v << 26) >> 26`
wraparound applied by the caller (`caimans_inter.read_mv`). This module only
reconstructs the raw per-component VLC, independent of that.

Run directly to print the reconstructed codebook:

    python3 tools/gba_video/caimans_mvvlc.py
"""
from caimans_codebooks import Images

SMALL_MAG = 0x03001C20   # DAT_030059fc, offset -2
SMALL_LEN = 0x03001BC0   # DAT_030059f8, offset -2
LARGE_MAG = 0x03001CC0   # DAT_030059f4, offset -3
LARGE_LEN = 0x03001C80   # DAT_030059f0, offset -3
SMALL_THRESHOLD = 0x06000000
MAX_CODE_BITS = 20


class Tables:
    def __init__(self, img=None):
        self.img = img or Images()

    def s8(self, addr):
        b = self.img.read(addr, 1)[0]
        return b - 256 if b > 127 else b


def decode_component(t, peek32):
    """One dx or dy delta. Returns (value, bits_consumed)."""
    if peek32 & 0x80000000:
        return 0, 1
    if peek32 < SMALL_THRESHOLD:
        idx = (peek32 >> 0x14) - 2
        mag = t.s8(SMALL_MAG + idx)
        length = t.s8(SMALL_LEN + idx)
    else:
        idx = (peek32 >> 0x19) - 3
        mag = t.s8(LARGE_MAG + idx)
        length = t.s8(LARGE_LEN + idx)
    if length < 1:
        # Off the end of a table (an index the real ladder cannot reach for
        # any valid bitstream); report as unterminated rather than crash.
        return None, None
    sign = -1 if (peek32 << (length - 1)) & 0x80000000 else 0
    value = (sign ^ mag) - sign
    return value, length


def invert(t, max_bits=MAX_CODE_BITS):
    """Recover the codeword table by driving the decoder over every prefix."""
    codes = {}
    for length in range(1, max_bits + 1):
        for code in range(1 << length):
            bits = format(code, "0%db" % length)
            if any(bits.startswith(c) for c in codes):
                continue
            lo = decode_component(t, (code << (32 - length)) & 0xFFFFFFFF)
            hi = decode_component(t, ((code << (32 - length))
                                      | ((1 << (32 - length)) - 1)) & 0xFFFFFFFF)
            if lo[1] is None or lo != hi or lo[1] != length:
                continue
            codes[bits] = lo[0]
    return codes


def main():
    t = Tables()
    codes = invert(t)
    vals = sorted(codes.values())
    kraft = sum(2.0 ** -len(c) for c in codes)
    print("%d codewords, range %+d..%+d, Kraft sum %.6f"
          % (len(codes), vals[0], vals[-1], kraft))
    for c in sorted(codes, key=lambda c: (len(c), c))[:12]:
        print("  %-14s -> %+d" % (c, codes[c]))


if __name__ == "__main__":
    main()
