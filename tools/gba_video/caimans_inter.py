#!/usr/bin/env python3
"""Caimans Pro **inter** picture decoding: macroblock types, MVs, MC.

Transcription of `FUN_03005a00`'s inter branch and `FUN_03005848` (the MV
reader), tying in `caimans_mvvlc` for the raw VLC and `caimans_blocks` for the
residual/block layer, which inter macroblocks share with intra ones.

## Macroblock types

A 3-bit peek into the table at `0x03001d00` (stride 4: `int16` value, `int8`
length at offset 2) gives a clean, complete prefix code:

    '1'   -> type 0   '01' -> type 1   '001' -> type 2   '000' -> type 3

- **type 0** -- copy the co-located 16x16 block from the reference frame.
  No motion, no residual.
- **type 1** -- one motion vector for the whole 16x16 macroblock (median
  predicted), then a residual through the block layer with `intra=False`.
- **type 2** -- four independent 8x8 motion vectors, then a *single* residual
  call over the full 16x16 (`intra=False`).
- **type 3** -- an intra macroblock inside an inter picture: no motion, the
  block layer runs with `intra=True` exactly as in an intra picture.

Types 0 and 3 have no motion vector, and the original clears both the
persistent "left" register and this macroblock's slot in the "top row" MV
cache to zero when it sees them -- so a neighbouring predicted macroblock
treats a copy/intra neighbour as zero motion, not as "no data".

## Motion vectors and half-pel interpolation

Each component (dx, dy) is decoded from `caimans_mvvlc`, then median-of-three
predicted against three neighbours from the MV cache (see `MVCache`), then
wrapped to a signed 6-bit delta: `(raw << 26) >> 26`. MVs are in **half-pel**
units: the integer displacement is `mv >> 1` (arithmetic), the fractional flag
is `mv & 1`. Full/half-pel selection then picks one of four interpolations,
all bit-identical to the corresponding SIMD routine's arithmetic once reduced
to scalar per-pixel math:

    full:       src[y][x]
    horizontal: (src[y][x] + src[y][x+1] + 1) >> 1
    vertical:   (src[y][x] + src[y+1][x] + 1) >> 1
    diagonal:   (src[y][x] + src[y][x+1] + src[y+1][x] + src[y+1][x+1] + 2) >> 2

If the resulting motion-compensation source would fall above or left of the
frame, that whole component (integer part and half-pel bit) is clamped to
zero *for the source address only* -- the stored/predicted MV keeps its
unclamped value.

## The MV prediction cache

See `MVCache`'s docstring. The exact addressing was recovered by capturing
every `FUN_03005848` call's three input addresses and their live values
against real hardware in mGBA -- the decompile's pointer choreography alone
was genuinely ambiguous (an earlier revision of this module guessed wrong),
but the hardware capture is not.
"""
from caimans_blocks import BitReader, BlockDecoder
from caimans_codebooks import Images
from caimans_mvvlc import Tables as MVTables, decode_component as decode_mv_component

MB_TYPE_TABLE = 0x03001D00   # DAT_03006608: 8 entries, stride 4
MB_TYPE_PEEK = 3


class MVCache:
    """The MV prediction context for one plane.

    Reverse engineered by capturing `FUN_03005848`'s three input addresses
    and their live values against real hardware for many consecutive calls
    (see the handoff doc's "MV prediction cache" section) -- this is the
    literal observed address pattern, not a guess. There are three kinds of
    storage:

    - `left`: one persistent (x, y) pair, shared by type 1 and type 2,
      carried from macroblock to macroblock within a plane. Hardware calls
      this its "CARRY" register (a single fixed address).
    - `top`: one (x, y) pair per 8-pixel column, carried from macroblock ROW
      to macroblock ROW (read by the row below as its "top" neighbour).
    - a type-2-only scratch (x, y) pair, live only for the duration of one
      macroblock's four sub-block reads (hardware's "ROLL" register),
      carrying sub-block 0's result to sub-blocks 1-3. It never survives to
      the next macroblock, so it isn't part of this class -- see
      `InterDecoder._decode_four_mv`.
    """

    def __init__(self):
        self.left = (0, 0)
        self.top = {}

    def top_at(self, col8):
        return self.top.get(col8, (0, 0))

    def clear(self, col8):
        self.left = (0, 0)
        self.top[col8] = (0, 0)
        self.top[col8 + 1] = (0, 0)


def read_mb_type(br, img):
    idx = br.peek32() >> (32 - MB_TYPE_PEEK)
    addr = MB_TYPE_TABLE + idx * 4
    value = int.from_bytes(img.read(addr, 2), "little", signed=True)
    length = img.read(addr + 2, 1)[0]
    br.skip(length)
    return value


def _wrap6(v):
    return ((v & 0x3F) ^ 0x20) - 0x20   # (v << 26) >> 26


def _median3(left, top, topright):
    """Literal port of FUN_03005848's comparison chain (not a generic
    median-of-3 -- a from-scratch "obvious" formula gave wrong answers here
    and was replaced with this direct transcription after diffing against
    captured hardware predictor values)."""
    b1 = topright <= top
    if left < top:
        b1 = not b1
    if b1:
        return top
    b2 = top < topright
    if left < topright:
        b2 = not b2
    if b2:
        return topright
    return left


def read_mv(br, mvt, left, top, topright):
    """Decode one (dx, dy) pair: raw VLC, median predict, 6-bit wrap.

    `left`, `top`, `topright` are each a (x, y) pair -- the label names are
    just for readability; hardware imposes no compass-direction meaning on
    these three slots (see the module docstring).
    """
    out = []
    for comp in range(2):
        raw, length = decode_mv_component(mvt, br.peek32())
        br.skip(length)
        pred = _median3(left[comp], top[comp], topright[comp])
        out.append(_wrap6(pred + raw))
    return out[0], out[1]


def mc_full(src, dst, sx, sy, soff, dx, dy, doff, stride, w, h):
    for row in range(h):
        so = soff + (sy + row) * stride + sx
        do = doff + (dy + row) * stride + dx
        dst[do:do + w] = src[so:so + w]


def mc_horiz(src, dst, sx, sy, soff, dx, dy, doff, stride, w, h):
    for row in range(h):
        so = soff + (sy + row) * stride + sx
        do = doff + (dy + row) * stride + dx
        for col in range(w):
            a, b = src[so + col], src[so + col + 1]
            dst[do + col] = (a + b + 1) >> 1


def mc_vert(src, dst, sx, sy, soff, dx, dy, doff, stride, w, h):
    for row in range(h):
        so = soff + (sy + row) * stride + sx
        so2 = so + stride
        do = doff + (dy + row) * stride + dx
        for col in range(w):
            a, b = src[so + col], src[so2 + col]
            dst[do + col] = (a + b + 1) >> 1


def mc_diag(src, dst, sx, sy, soff, dx, dy, doff, stride, w, h):
    for row in range(h):
        so = soff + (sy + row) * stride + sx
        so2 = so + stride
        do = doff + (dy + row) * stride + dx
        for col in range(w):
            a, b = src[so + col], src[so + col + 1]
            c, d = src[so2 + col], src[so2 + col + 1]
            dst[do + col] = (a + b + c + d + 2) >> 2


def pick_mc(fx, fy):
    if fx == 0 and fy == 0:
        return mc_full
    if fx and not fy:
        return mc_horiz
    if fy and not fx:
        return mc_vert
    return mc_diag


def _clamp_source(px, py, mx, my):
    """Zero a whole MV component (int part + half-pel bit) if it would place
    the motion-compensation source above or left of frame origin."""
    if py + (my >> 1) < 0:
        my = 0
    if px + (mx >> 1) < 0:
        mx = 0
    return mx, my


class InterDecoder:
    def __init__(self, img=None):
        self.img = img or Images()
        self.block = BlockDecoder(self.img)
        self.mvt = MVTables(self.img)

    def decode_plane(self, br, cur, ref, offset, stride, w, h):
        cache = MVCache()
        for y in range(0, h, 16):
            for x in range(0, w, 16):
                self._decode_macroblock(br, cur, ref, offset, stride, x, y, cache)

    def _decode_macroblock(self, br, cur, ref, offset, stride, x, y, cache):
        col8 = x // 8
        mtype = read_mb_type(br, self.img)
        if mtype in (0, 3):
            cache.clear(col8)

        dst = offset + y * stride + x

        if mtype == 0:
            mc_full(ref, cur, x, y, offset, x, y, offset, stride, 16, 16)
            return

        if mtype == 3:
            self.block.decode_macroblock(br, cur, dst, stride, intra=True)
            return

        if mtype == 1:
            top = cache.top_at(col8)
            topright = cache.top_at(col8 + 2)
            mx, my = read_mv(br, self.mvt, cache.left, top, topright)
            cache.left = (mx, my)
            cache.top[col8] = (mx, my)
            cache.top[col8 + 1] = (mx, my)
            emx, emy = _clamp_source(x, y, mx, my)
            fx, fy = emx & 1, emy & 1
            sx, sy = x + (emx >> 1), y + (emy >> 1)
            pick_mc(fx, fy)(ref, cur, sx, sy, offset, x, y, offset, stride, 16, 16)
            self.block.decode_macroblock(br, cur, dst, stride, intra=False)
            return

        if mtype == 2:
            self._decode_four_mv(br, cur, ref, offset, stride, x, y, col8, cache)
            self.block.decode_macroblock(br, cur, dst, stride, intra=False)
            return

        raise ValueError("mb-type %d out of range" % mtype)

    def _decode_four_mv(self, br, cur, ref, offset, stride, x, y, col8, cache):
        """Four 8x8 sub-block MVs, in the exact call order and addressing
        hardware uses -- see `MVCache`'s docstring. Sub-blocks are decoded
        top-left, top-right, bottom-left, bottom-right, matching both the
        write order onto the persistent registers and the spatial layout
        used for motion compensation below."""
        c = col8
        mv_a = read_mv(br, self.mvt, cache.left, cache.top_at(c), cache.top_at(c + 2))
        scratch = mv_a
        mv_b = read_mv(br, self.mvt, scratch, cache.top_at(c + 1), cache.top_at(c + 2))
        cache.left = mv_b
        mv_c = read_mv(br, self.mvt, scratch, cache.left, cache.top_at(c - 1))
        cache.top[c] = mv_c
        mv_d = read_mv(br, self.mvt, scratch, cache.left, cache.top[c])
        cache.top[c + 1] = mv_d

        for k, (mx, my) in enumerate((mv_a, mv_b, mv_c, mv_d)):
            sub_x = x + (8 if k in (1, 3) else 0)
            sub_y = y + (8 if k in (2, 3) else 0)
            emx, emy = _clamp_source(sub_x, sub_y, mx, my)
            fx, fy = emx & 1, emy & 1
            sx = sub_x + (emx >> 1)
            sy = sub_y + (emy >> 1)
            pick_mc(fx, fy)(ref, cur, sx, sy, offset, sub_x, sub_y, offset, stride, 8, 8)
