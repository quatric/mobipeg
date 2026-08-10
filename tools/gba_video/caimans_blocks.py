#!/usr/bin/env python3
"""Reference implementation of the Caimans Pro block/texture layer.

This is a transcription of `FUN_03004cd4` (see ``doc/caimans_handoff.md``),
tying together the four pieces that have been recovered:

  * the alternating binary subdivision of a 16x16 macroblock,
  * the per-level leaf *mode* VLCs (`caimans_modevlc` tables, read here),
  * the per-level leaf *value* VLCs (`caimans_valuevlc`),
  * the per-level pattern codebooks (`caimans_codebooks`).

A leaf reconstructs as

    out = clamp(pred + value + sum of the N selected patterns, 0, 255)

with ``pred = 0`` on the intra path. Mode ``N`` is the number of codebook
layers; mode ``-1`` skips the leaf (intra fills 0, inter leaves the
prediction alone).

What this module does **not** cover: the picture header, motion vectors and
motion compensation (all H.263-derived, and handled by the caller in the
original player), and the container framing. So it cannot yet decode a real
frame end to end -- it decodes one macroblock given a bit position and a
prediction. See the next-work list in the handoff doc.

Run directly for the structural self-tests:

    python3 tools/gba_video/caimans_blocks.py
"""
import os
import struct

from caimans_codebooks import Images, load as load_codebooks
from caimans_valuevlc import Tables, decode_inter, decode_intra

# Per-level leaf geometry, read straight out of the IWRAM tables rather than
# hardcoded. DAT_030057c8 = 0x030000b4 (height), DAT_030057cc = 0x030000bc.
HEIGHT_TABLE = 0x030000B4
WIDTH_TABLE = 0x030000BC

# Mode VLC tables. Intra peeks 7 bits with a 0x80 per-level stride, inter
# peeks 6 bits with a 0x40 stride; the four tables are one contiguous block.
MODE_TABLES = {
    "intra": {"value": 0x030015C0, "length": 0x030012C0, "peek": 7, "stride": 0x80},
    "inter": {"value": 0x03001A40, "length": 0x030018C0, "peek": 6, "stride": 0x40},
}

ROOT_LEVEL = 5
MB_SIZE = 16


class Desync(Exception):
    """Raised when the bitstream asks for something no valid stream can.

    This is a *detector*, not a decoder error to be worked around: it means
    the bit position has drifted and everything after it is noise.
    """


class BitReader:
    """MSB-first bit reader, matching FUN_03003ec4 / FUN_03003f68.

    The player keeps {byte pointer, bit position} at 0x03000024 and resets the
    bit position to 0 at the top of each picture, so a frame's bitstream always
    starts byte-aligned.
    """

    def __init__(self, data, bitpos=0):
        self.data = data
        self.pos = bitpos

    def bit(self):
        byte = self.data[self.pos >> 3]
        v = (byte >> (7 - (self.pos & 7))) & 1
        self.pos += 1
        return v

    def peek32(self):
        """The 32 MSB-aligned bits at the current position, zero-padded."""
        i = self.pos >> 3
        chunk = self.data[i:i + 5].ljust(5, b"\0")
        v = int.from_bytes(chunk, "big")
        return (v >> (8 - (self.pos & 7))) & 0xFFFFFFFF

    def skip(self, n):
        self.pos += n


class BlockDecoder:
    def __init__(self, img=None):
        self.img = img or Images()
        self.tables = Tables(self.img)
        self.codebooks = load_codebooks(self.img)
        # The tables hold *selectors*, not plain dimensions. A width of 0
        # selects the 16-pixel fill callback, and each callback compares its
        # count against half its width to choose half or full height
        # (FUN_03004438 `cmp r1,#2`, FUN_03004468 `cmp r1,#4`,
        # FUN_030044c4 `cmp r1,#8`). Verified against the running player.
        self.height = list(self.img.read(HEIGHT_TABLE, 6))
        self.width = list(self.img.read(WIDTH_TABLE, 6))
        self.fill_size = []
        for level in range(6):
            w = self.width[level] or 16
            half = w // 2
            self.fill_size.append((w, half if self.height[level] == half else w))
        self.mode = {p: self._mode_tables(p) for p in ("intra", "inter")}

    def _mode_tables(self, path):
        spec = MODE_TABLES[path]
        out = []
        for level in range(6):
            size = 1 << spec["peek"]
            vals = self.img.read(spec["value"] + level * spec["stride"], size)
            lens = self.img.read(spec["length"] + level * spec["stride"], size)
            out.append([(v - 256 if v > 127 else v, l) for v, l in zip(vals, lens)])
        return out

    # -- syntax elements ---------------------------------------------------

    def read_mode(self, br, path, level):
        spec = MODE_TABLES[path]
        idx = br.peek32() >> (32 - spec["peek"])
        value, length = self.mode[path][level][idx]
        br.skip(length)
        return value

    def read_value(self, br, path):
        decoder = decode_intra if path == "intra" else decode_inter
        value, length = decoder(self.tables, br.peek32())
        br.skip(length)
        return value

    # -- subdivision -------------------------------------------------------

    def walk(self, br, stride, base=0, on_leaf=None):
        """Walk the bintree, calling `on_leaf(offset, level)` as each is found.

        Transcribed from the outer loop of FUN_03004cd4: a queue of node
        offsets, one split-flag bit per node, with the split axis alternating
        on `level & 1` and the step scaled by `(level >> 1) + 1`.

        The callback matters. In the original the leaf decode is **inline in
        this same loop**, so a leaf's mode/value/pattern bits sit in the
        bitstream *between* the split flags either side of it. Reading all the
        split flags first and decoding the leaves afterwards consumes exactly
        the same bits in a different order, and desynchronises immediately.
        """
        nodes = [base]
        head = 0
        tail = 1
        level_end = 1
        level = ROOT_LEVEL
        leaves = []
        while True:
            split = False
            if level > 0:
                if head != level_end:
                    split = True
                else:
                    level -= 1
                    level_end = tail
                    split = level != 0
            if split and br.bit():
                offset = nodes[head]
                shift = (level >> 1) + 1
                delta = (stride << shift) if (level & 1) else (1 << shift)
                nodes.append(offset)
                nodes.append(offset + delta)
                tail += 2
                head += 1
                continue
            leaves.append((nodes[head], level))
            if on_leaf is not None:
                on_leaf(nodes[head], level)
            head += 1
            if head >= tail:
                return leaves

    def subdivide(self, br, stride, base=0):
        """The leaf *shape* only, reading nothing but split flags.

        Used by the structural tests. Real decoding must go through `walk`
        with a callback -- see the note there.
        """
        return self.walk(br, stride, base)

    # -- leaf reconstruction ----------------------------------------------

    def decode_leaf(self, br, buf, offset, stride, level, intra):
        """Reconstruct one leaf into `buf` (a bytearray framebuffer)."""
        path = "intra" if intra else "inter"
        fw, fh = self.fill_size[level]
        mode = self.read_mode(br, path, level)

        if mode == -1:
            if intra:
                self._fill(buf, offset, stride, fw, fh, 0)
            return  # inter: skipped, prediction survives untouched

        value = self.read_value(br, path)

        if mode == 0 and intra:
            self._fill(buf, offset, stride, fw, fh, value & 0xFF)
            return

        # The codebook path does *not* go through the fill callbacks: it loops
        # `height[level]` rows of `width[level] / 4` words, using the raw table
        # values. Those equal the true leaf size at levels 0-3, and are 0 at
        # levels 4-5 -- which is harmless for mode 0 (0 layers: the pixel loop
        # below is naturally a no-op at those levels, since only mode > 0
        # actually indexes a codebook).
        w, h = self.width[level], self.height[level]

        # Mode N: sum of N codebook layers. Each layer spends 4 bits picking
        # one of 16 patterns, and layer i draws from slots 16*i .. 16*i+15.
        if mode > 0 and level not in self.codebooks[path]:
            # Levels 4 and 5 have no codebook: their entries in the pointer
            # tables at 0x03001d20 / 0x03002640 are not pointers, and those
            # tables are byte-identical in ROM and IWRAM, so nothing fills
            # them in at runtime. A real stream therefore cannot ask for this,
            # and reaching here means the bit position has drifted.
            raise Desync("mode %d at level %d has no codebook" % (mode, level))
        book = self.codebooks[path].get(level)
        patterns = []
        bits = 0
        for _ in range(mode):
            bits = (bits << 4) | (br.peek32() >> 28)
            br.skip(4)
        for i in range(mode):
            nib = (bits >> (4 * (mode - 1 - i))) & 0xF
            patterns.append(book[i * 16 + nib])

        for row in range(h):
            for col in range(w):
                k = row * w + col
                acc = value + sum(p[k] for p in patterns)
                if not intra:
                    acc += buf[offset + row * stride + col]
                buf[offset + row * stride + col] = 0 if acc < 0 else (255 if acc > 255 else acc)

    @staticmethod
    def _fill(buf, offset, stride, w, h, value):
        for row in range(h):
            start = offset + row * stride
            buf[start:start + w] = bytes([value]) * w

    def decode_macroblock(self, br, buf, offset, stride, intra):
        self.walk(br, stride, offset,
                  lambda o, level: self.decode_leaf(br, buf, o, stride, level, intra))


# -- self-tests ------------------------------------------------------------

class _Bits:
    """A bit source driven by a callable, for structural tests."""

    def __init__(self, fn):
        self.fn = fn
        self.pos = 0

    def bit(self):
        v = self.fn(self.pos)
        self.pos += 1
        return v


def test_ladder(dec):
    """The size tables should describe the ladder the split arithmetic implies."""
    expect = [(4, 2), (4, 4), (8, 4), (8, 8), (16, 8)]
    got = [(dec.width[i], dec.height[i]) for i in range(5)]
    assert got == expect, "leaf ladder %s != %s" % (got, expect)
    # The raw level-5 entry is (0, 0); decoded through the fill callbacks'
    # selector semantics that means a full 16x16, which is what the un-split
    # root must paint.
    assert (dec.width[5], dec.height[5]) == (0, 0)
    assert dec.fill_size == [(4, 2), (4, 4), (8, 4), (8, 8), (16, 8), (16, 16)]
    return "fill ladder: %s" % " ".join("%dx%d" % wh for wh in dec.fill_size)


def test_pattern_size(dec):
    """A pattern must be exactly one leaf's worth of samples at every level."""
    for level in range(4):
        w, h = dec.width[level], dec.height[level]
        words = 1 << (level + 1)  # the decoder's (nib + 16i) << (level+1) stride
        assert words * 4 == w * h, "level %d: %d words != %dx%d" % (level, words, w, h)
        for path in ("intra", "inter"):
            assert len(dec.codebooks[path][level][0]) == w * h
    return "pattern stride == leaf size at levels 0-3, both paths"


def test_full_split_tiles(dec):
    """Splitting at every node must tile the macroblock exactly once."""
    stride = 240
    leaves = dec.subdivide(_Bits(lambda i: 1), stride, 0)
    assert all(level == 0 for _, level in leaves), "not all leaves bottomed out"
    assert len(leaves) == (MB_SIZE * MB_SIZE) // (4 * 2), "got %d leaves" % len(leaves)
    seen = set()
    for offset, level in leaves:
        w, h = dec.fill_size[level]
        for row in range(h):
            for col in range(w):
                px = offset + row * stride + col
                assert px not in seen, "pixel %d written twice" % px
                seen.add(px)
    expect = {r * stride + c for r in range(MB_SIZE) for c in range(MB_SIZE)}
    assert seen == expect, "coverage is not the 16x16 macroblock"
    return "full split: %d 4x2 leaves tile the 16x16 exactly once" % len(leaves)


def test_no_split_is_empty(dec):
    """A root that never splits yields one zero-extent leaf -- a skipped MB."""
    leaves = dec.subdivide(_Bits(lambda i: 0), 240, 0)
    assert leaves == [(0, ROOT_LEVEL)]
    return "no split: one level-5 leaf, painting the full 16x16"


def test_mixed_split_tiles(dec):
    """An arbitrary split pattern must still tile the macroblock exactly."""
    import random

    rng = random.Random(20260809)
    for trial in range(200):
        stride = 240
        leaves = dec.subdivide(_Bits(lambda i: rng.randint(0, 1)), stride, 0)
        seen = set()
        for offset, level in leaves:
            w, h = dec.fill_size[level]
            for row in range(h):
                for col in range(w):
                    px = offset + row * stride + col
                    assert px not in seen, "trial %d: pixel %d twice" % (trial, px)
                    seen.add(px)
        expect = {r * stride + c for r in range(MB_SIZE) for c in range(MB_SIZE)}
        assert seen == expect, "trial %d: covered %d px" % (trial, len(seen))
    return "200 random split patterns all tile the 16x16 exactly once"


def main():
    dec = BlockDecoder()
    for test in (test_ladder, test_pattern_size, test_full_split_tiles,
                 test_no_split_is_empty, test_mixed_split_tiles):
        print("PASS  %s" % test(dec))


if __name__ == "__main__":
    main()
