"""VX++ (GBA Video) reconstruction layer -- work in progress.

vx_sim.py recovers *what* every bitstream symbol is. This module turns symbols
into pixels. Everything here is read off the decoder's IWRAM image and verified
against it; nothing is guessed. What is still missing is listed at the bottom.

The codec is H.264-shaped: 4x4 integer transform, the standard dequant table,
zig-zag scan, and 16x16 intra modes with the usual availability rules. It
differs in its prediction: rather than a small fixed set of partitions, it uses
a recursive rectangular split driven by 16 jump-table dispatchers (see
vx_grammar.py), and residuals are added on top of a prediction that has already
been written into the frame buffer.
"""

# ---------------------------------------------------------------- geometry

STRIDE = 0x100          # frame buffer pitch, luma and chroma alike
CHROMA_BPS = 2          # Cb and Cr interleaved, two bytes per chroma sample

# Block size handled by each dispatcher. The nine with a whole-block intra mode
# give their size directly, as the r4/r5 immediates their mode-6 handler passes
# to the intra predictor at FUN_03000884. The other seven follow from the split
# tree: mode 1 halves the height, mode 2 halves the width, so every dispatcher's
# children pin its size. Each of those seven is reached two independent ways and
# both agree -- 0x03003adc is 8x2 as 16x2's mode 2 and as 8x4's mode 1.
#
# The sixteen are exactly the 4x4 grid of {2,4,8,16} widths and heights, and the
# seven without a whole-block intra mode are exactly those with a side of 2,
# which is below what the intra predictors can address.
BLOCK_SIZE = {
    0x03001dac: (16, 16), 0x03002184: (16, 8),
    0x030024ac: (16, 4),  0x030028ec: (16, 2),
    0x030030f4: (8, 16),  0x03003584: (8, 8),
    0x03003818: (8, 4),   0x03003adc: (8, 2),
    0x030041cc: (4, 16),  0x03004468: (4, 8),
    0x03004750: (4, 4),   0x03004950: (4, 2),
    0x03004fd0: (2, 16),  0x03005294: (2, 8),
    0x03005494: (2, 4),   0x030055fc: (2, 2),
}

# Mode 1 splits into two blocks stacked vertically, mode 2 into two side by
# side; the two 'disp' events of that mode are the halves in that order.
SPLIT_VERTICAL, SPLIT_HORIZONTAL = 1, 2


def split_offsets(disp, mode):
    """Byte offsets of the two children of a split, relative to the parent."""
    w, h = BLOCK_SIZE[disp]
    if mode == SPLIT_VERTICAL:
        return (0, (h // 2) * STRIDE)
    return (0, w // 2)

# A macroblock's position is carried as a byte offset into the frame buffer:
#   offset = mb_y * 16 * STRIDE + mb_x * 16
# which is why the intra predictors test it directly for edge availability --
# bits 0-7 are zero exactly when mb_x == 0, bits 8-15 when mb_y == 0.
def mb_offset(mb_x, mb_y):
    return mb_y * 16 * STRIDE + mb_x * 16


# ---------------------------------------------------------------- residual

# Standard H.264 zig-zag, confirmed from the coefficient addresses the column
# transform loads: column j reads scan indices that map exactly to raster
# positions j, j+4, j+8, j+12 under this table.
ZIGZAG = [0, 1, 4, 8, 5, 2, 3, 6, 9, 12, 13, 10, 7, 11, 14, 15]

# Dequant factors, byte-identical to H.264's: (both-even, mixed, both-odd)
# indexed by qp % 6, then shifted left by qp // 6. Tables live at 0x03000680
# (factors), 0x030006c8 (row offset) and 0x030006f8 (shift).
DEQUANT = [
    (10, 13, 16), (11, 14, 18), (13, 16, 20),
    (14, 18, 23), (16, 20, 25), (18, 23, 29),
]


def dequant_factors(qp):
    """The three multipliers the transform applies, as set up at 0x03000730."""
    a, c, b = DEQUANT[qp % 6]
    sh = qp // 6
    return a << sh, c << sh, b << sh


def _butterfly(d0, d1, d2, d3):
    """H.264 integer inverse transform butterfly (0x03005828)."""
    e0 = d0 + d2
    e1 = d0 - d2
    e2 = (d1 >> 1) - d3
    e3 = d1 + (d3 >> 1)
    return e0 + e3, e1 + e2, e1 - e2, e0 - e3


def idct4x4(coeffs, qp):
    """Dequantise + inverse transform 16 scan-ordered coefficients -> 4x4 residual.

    Columns first (0x03005828 onward), then rows with the +32 rounding and >>6
    that the reconstruct routines apply (0x03005a94).
    """
    fa, fc, fb = dequant_factors(qp)
    blk = [0] * 16
    for k, v in enumerate(coeffs):
        blk[ZIGZAG[k]] = v
    # Dequantise: factor depends on the parity of (row, col), as the transform's
    # interleaved use of the three multipliers shows.
    for r in range(4):
        for c in range(4):
            f = fa if (r % 2 == 0 and c % 2 == 0) else \
                fb if (r % 2 == 1 and c % 2 == 1) else fc
            blk[r * 4 + c] *= f
    tmp = [0] * 16
    for c in range(4):
        v = _butterfly(blk[c], blk[4 + c], blk[8 + c], blk[12 + c])
        for r in range(4):
            tmp[r * 4 + c] = v[r]
    out = [0] * 16
    for r in range(4):
        v = _butterfly(tmp[r * 4] + 32, tmp[r * 4 + 1], tmp[r * 4 + 2], tmp[r * 4 + 3])
        for c in range(4):
            out[r * 4 + c] = v[c] >> 6
    return out


# The residual loop (0x03005650) walks a macroblock in 8x8 quadrants. Each
# quadrant reads one ue(v), maps it through the permutation table at 0x03005610,
# and treats the result as a **6-bit coded-block-pattern**: bits 0-3 are the
# quadrant's four luma 4x4 blocks, bit 4 is Cb and bit 5 is Cr. Four quadrants
# per macroblock gives the 16 luma and 8 chroma blocks section 10 describes.
#
# While walking, r10 carries a plane index -- 0 luma, 1 Cb, 2 Cr -- and that
# index selects the reconstruct variant. There are two tables of three:
#   0x0300577c  full inverse transform, per plane
#   0x03005788  DC-only fast path, per plane
# so the "six reconstruct variants" are three planes times two paths, not six
# different transforms. Chroma variants write every other byte and leave the
# companion component untouched, since the two are interleaved.

CBP_LUMA_BITS = 0x0f
CBP_CB, CBP_CR = 0x10, 0x20

# plane index -> (bytes per sample, byte offset within the sample)
PLANE = {0: (1, 0), 1: (CHROMA_BPS, 0), 2: (CHROMA_BPS, 1)}


def add_residual(buf, off, residual, plane=0):
    """clip(pred + residual) for one 4x4 block (0x03005a94 and friends).

    The frame buffer already holds the prediction when the residual arrives, so
    this is an in-place add. `plane` picks the reconstruct variant: luma writes
    consecutive bytes, Cb and Cr write every other one.
    """
    bps, base_off = PLANE[plane]
    for r in range(4):
        base = off + base_off + r * STRIDE
        for c in range(4):
            i = base + c * bps
            p = buf[i] + residual[r * 4 + c]
            buf[i] = 0 if p < 0 else 255 if p > 255 else p


def add_residual_dc(buf, off, dc, plane=0):
    """DC-only fast path (0x03005788's three entries, e.g. 0x03005c9c).

    When a block carries nothing but its DC there is no transform at all: one
    value is added to every pixel. `dc` is the dequantised DC, added as dc >> 6
    -- the same shift the full path applies after its row pass.
    """
    bps, base_off = PLANE[plane]
    v = dc >> 6
    for r in range(4):
        base = off + base_off + r * STRIDE
        for c in range(4):
            i = base + c * bps
            p = buf[i] + v
            buf[i] = 0 if p < 0 else 255 if p > 255 else p


# ------------------------------------------------- whole-block intra modes

def intra_vertical(buf, off, w, h):
    """Mode 0 (0x030009b8): replicate the row above."""
    for r in range(h):
        buf[off + r * STRIDE: off + r * STRIDE + w] = \
            buf[off - STRIDE: off - STRIDE + w]


def intra_horizontal(buf, off, w, h):
    """Mode 1 (0x03000a04): replicate the pixel to the left of each row."""
    for r in range(h):
        base = off + r * STRIDE
        buf[base: base + w] = bytes([buf[base - 1]]) * w


def intra_dc(buf, off, w, h, have_left, have_top):
    """Mode 2 (0x03000a54): mean of the top row and left column, with the
    availability fallbacks the ARM takes (neither -> 128)."""
    if not have_left and not have_top:
        dc = 0x80
    elif have_top and not have_left:
        dc = (sum(buf[off - STRIDE + i] for i in range(w)) + w // 2) // w
    elif have_left and not have_top:
        dc = (sum(buf[off + r * STRIDE - 1] for r in range(h)) + h // 2) // h
    else:
        s = sum(buf[off - STRIDE + i] for i in range(w))
        s += sum(buf[off + r * STRIDE - 1] for r in range(h))
        dc = (s + (w + h) // 2) // (w + h)
    for r in range(h):
        base = off + r * STRIDE
        buf[base: base + w] = bytes([dc]) * w


def intra_midpoint(buf, off, w, h):
    """Mode 3 (0x03000ba4): recursive midpoint subdivision -- NOT H.264 plane.

    The entry seeds the block's bottom-right corner from the two far neighbours,
    then dispatches to one of eleven size-specialised fills through the table at
    0x03000b78, indexed by (w>>3) + 4*(h>>3) -- indices 3 and 7 are zero, being
    the combinations that cannot occur. All eleven are the same three writes
    around a recursive quadrant split, so they collapse into one function.
    """
    br = ((buf[off + (w - 1) - STRIDE] +          # (w-1, -1)
           buf[off + (h - 1) * STRIDE - 1]) + 1) >> 1   # (-1, h-1)
    buf[off + (h - 1) * STRIDE + (w - 1)] = br
    _midpoint_fill(buf, off, w, h)


def _midpoint_fill(buf, off, w, h, bps=1, corner=None):
    """One subdivision level: bottom-edge, right-edge, then centre, then recurse.

    The corner at (w-1, h-1) is already set on entry. Note these three use a
    bare >>1 with no rounding, unlike the +1 the corner seed above applies.

    Which pair of points the centre interpolates between depends on the block's
    aspect: vertically down column sw-1 when log2(w)+log2(h) is even, and
    horizontally along row sh-1 when it is odd. That parity is unchanged by
    halving both sides, so a block keeps one axis for its whole recursion --
    which is why 16x16 recurses into 8x8 (both vertical) and 16x8 into 8x4
    (both horizontal). Verified against all nine size variants.
    """
    if w <= 1 and h <= 1:
        return
    # A block that has become one pixel thick keeps bisecting along its long
    # side: the 4x2 leaf at 0x0300479c fills its last four pixels inline as
    # ((-1,y) + (1,y)) >> 1, which is this same step on 2x1 children. Halving
    # both sides only terminates cleanly on squares, which is why every
    # non-square size was left half unwritten.
    if h == 1:
        sw = w // 2
        mid = (buf[off - bps] + (corner if corner is not None
                                 else buf[off + (w - 1) * bps])) >> 1
        buf[off + (sw - 1) * bps] = mid & 0xff
        _midpoint_fill(buf, off, sw, 1, bps, mid)
        _midpoint_fill(buf, off + sw * bps, w - sw, 1, bps, corner)
        return
    if w == 1:
        sh = h // 2
        mid = (buf[off - STRIDE] + (corner if corner is not None
                                    else buf[off + (h - 1) * STRIDE])) >> 1
        buf[off + (sh - 1) * STRIDE] = mid & 0xff
        _midpoint_fill(buf, off, 1, sh, bps, mid)
        _midpoint_fill(buf, off + sh * STRIDE, 1, h - sh, bps, corner)
        return
    sw, sh = w // 2, h // 2
    bot_row = (h - 1) * STRIDE
    mid_row = (sh - 1) * STRIDE
    # The fill takes the corner in a register (r6), not by reading the byte
    # back, which matters for mode 4: it stores corner + 2*delta with strb --
    # truncating -- but passes the untruncated value on to the fill.
    if corner is None:
        corner = buf[off + bot_row + (w - 1) * bps]

    bot = (buf[off + bot_row - bps] + corner) >> 1               # (-1, h-1)
    buf[off + bot_row + (sw - 1) * bps] = bot & 0xff             # (sw-1, h-1)

    right = (buf[off + (w - 1) * bps - STRIDE] + corner) >> 1    # (w-1, -1)
    buf[off + mid_row + (w - 1) * bps] = right & 0xff            # (w-1, sh-1)

    if not ((w.bit_length() + h.bit_length()) & 1):
        centre = (bot + buf[off + (sw - 1) * bps - STRIDE]) >> 1  # (sw-1, -1)
    else:
        centre = (right + buf[off + mid_row - bps]) >> 1          # (-1, sh-1)
    buf[off + mid_row + (sw - 1) * bps] = centre & 0xff

    _midpoint_fill(buf, off, sw, sh, bps)
    _midpoint_fill(buf, off + sw * bps, sw, sh, bps)
    _midpoint_fill(buf, off + sh * STRIDE, sw, sh, bps)
    _midpoint_fill(buf, off + sh * STRIDE + sw * bps, sw, sh, bps)


# -------------------------------------------------------- chroma intra modes
#
# Cb and Cr are *interleaved* in one plane, two bytes per chroma sample, at the
# same 0x100 pitch as luma -- mode 1 gives it away by loading a neighbour with
# ldrh and replicating it with `orr r9, r9, r9, lsl #16`. A chroma block is
# (w/2)x(h/2) samples, i.e. w bytes wide, and the predictors are handed the luma
# dimensions and halve them themselves.
#
# Chroma also uses a different base pair than luma: [r0,#-4] + [r0,#4], where
# luma uses [r0,#-8] + [r0,#0].
#
# Mode order differs from luma, which is 0=vertical 1=horizontal:
#   0 = DC, 1 = horizontal, 2 = vertical, 3 = midpoint subdivision.

# log2 of a chroma dimension, indexed by the *luma* dimension >> 2. Table of
# bytes at 0x03000c00; entries 0 and 3 are the unreachable slots.
DC_SHIFT = [0, 1, 2, 0, 3]


def chroma_dc(buf, off, cw, ch, have_left, have_top):
    """Chroma mode 0 (0x03000c08).

    The ARM reads availability straight off the block's byte offset rather than
    tracking it: the whole offset zero means no neighbours at all, `tst #0xff`
    zero means mb_x == 0, `tst #0xff00` zero means mb_y == 0.

    Note this averages the two edge DCs -- (top + left + 1) >> 1 -- instead of
    summing every neighbouring sample and dividing once as H.264 does. With one
    edge available its DC is used as-is, with neither the result is 0x80.
    """
    def edge_dc(vals, n):
        return (sum(vals) + n // 2) >> DC_SHIFT[n >> 1]

    if not have_left and not have_top:
        cb = cr = 0x80
    elif have_top and not have_left:
        cb = edge_dc([buf[off - STRIDE + 2 * i] for i in range(cw)], cw)
        cr = edge_dc([buf[off - STRIDE + 2 * i + 1] for i in range(cw)], cw)
    elif have_left and not have_top:
        cb = edge_dc([buf[off + r * STRIDE - 2] for r in range(ch)], ch)
        cr = edge_dc([buf[off + r * STRIDE - 1] for r in range(ch)], ch)
    else:
        tb = edge_dc([buf[off - STRIDE + 2 * i] for i in range(cw)], cw)
        tr = edge_dc([buf[off - STRIDE + 2 * i + 1] for i in range(cw)], cw)
        lb = edge_dc([buf[off + r * STRIDE - 2] for r in range(ch)], ch)
        lr_ = edge_dc([buf[off + r * STRIDE - 1] for r in range(ch)], ch)
        cb, cr = (tb + lb + 1) >> 1, (tr + lr_ + 1) >> 1

    for r in range(ch):
        base = off + r * STRIDE
        buf[base:base + 2 * cw] = bytes([cb, cr]) * cw


def chroma_horizontal(buf, off, cw, ch):
    """Chroma mode 1 (0x03000d90): replicate the sample pair to the left."""
    for r in range(ch):
        base = off + r * STRIDE
        buf[base:base + 2 * cw] = bytes(buf[base - 2:base]) * cw


def chroma_vertical(buf, off, cw, ch):
    """Chroma mode 2 (0x03000dd4): replicate the row above."""
    row = bytes(buf[off - STRIDE:off - STRIDE + 2 * cw])
    for r in range(ch):
        buf[off + r * STRIDE:off + r * STRIDE + 2 * cw] = row


def chroma_midpoint(buf, off, cw, ch):
    """Chroma mode 3 (0x03000e44): the mode-3 subdivision, run once per
    component. The ARM seeds both corners, then calls one fill routine twice --
    at byte +0 for Cb and +1 for Cr -- through its own 11-entry table at
    0x03000e18, indexed by the luma dimensions exactly as luma's is.
    """
    for c in range(CHROMA_BPS):
        o = off + c
        a = buf[o + (cw - 1) * CHROMA_BPS - STRIDE]      # (cw-1, -1)
        b = buf[o + (ch - 1) * STRIDE - CHROMA_BPS]      # (-1, ch-1)
        buf[o + (ch - 1) * STRIDE + (cw - 1) * CHROMA_BPS] = (a + b + 1) >> 1
        _midpoint_fill(buf, o, cw, ch, CHROMA_BPS)


# ------------------------------------------------------------ intra 4x4 modes
#
# All nine are H.264's, in H.264's numbering, at the table at 0x030008d8:
#   0 V, 1 H, 2 DC, 3 diagonal down-left, 4 diagonal down-right,
#   5 vertical-right, 6 horizontal-down, 7 vertical-left, 8 horizontal-up.
#
# One real difference from H.264: there is no neighbour substitution. The
# directional modes read T4..T7 unconditionally rather than replicating T3 when
# the above-right block is unavailable, so whatever the frame buffer already
# holds there is what gets filtered.

def _nb(buf, off):
    """Neighbours of a 4x4 block: 8 above (incl. above-right), 4 left, corner."""
    return ([buf[off - STRIDE + i] for i in range(8)],
            [buf[off + r * STRIDE - 1] for r in range(4)],
            buf[off - STRIDE - 1])


def _avg2(a, b):
    return (a + b + 1) >> 1


def _avg3(a, b, c):
    return (a + 2 * b + c + 2) >> 2


def intra4x4(buf, off, mode):
    """Predict one 4x4 luma block. `off` is its byte offset in the frame buffer,
    which is also what the ARM tests for edge availability."""
    t, l, c = _nb(buf, off)
    p = [[0] * 4 for _ in range(4)]

    if mode == 0:
        for y in range(4):
            for x in range(4):
                p[y][x] = t[x]
    elif mode == 1:
        for y in range(4):
            for x in range(4):
                p[y][x] = l[y]
    elif mode == 2:
        # 0x03000f90: +2 per available edge, shift 1 + (number of edges).
        have_left = (off & 0xff) != 0
        have_top = (off & 0xff00) != 0
        if not have_left and not have_top:
            dc = 0x80
        else:
            s, sh = 0, 1
            if have_left:
                s += sum(l) + 2
                sh += 1
            if have_top:
                s += sum(t[:4]) + 2
                sh += 1
            dc = s >> sh
        for y in range(4):
            for x in range(4):
                p[y][x] = dc
    elif mode == 3:
        for y in range(4):
            for x in range(4):
                k = x + y
                p[y][x] = _avg3(t[6], t[7], t[7]) if k == 6 else \
                    _avg3(t[k], t[k + 1], t[k + 2])
    elif mode == 4:
        for y in range(4):
            for x in range(4):
                if x > y:
                    k = x - y
                    p[y][x] = _avg3(t[k - 2] if k >= 2 else c, t[k - 1], t[k])
                elif x < y:
                    k = y - x
                    p[y][x] = _avg3(l[k - 2] if k >= 2 else c, l[k - 1], l[k])
                else:
                    p[y][x] = _avg3(t[0], c, l[0])
    elif mode == 5:
        for y in range(4):
            for x in range(4):
                z = 2 * x - y
                if z >= 0:
                    k = x - (y >> 1)
                    p[y][x] = _avg2(t[k - 1] if k >= 1 else c, t[k]) if not (z & 1) \
                        else _avg3(t[k - 2] if k >= 2 else c, t[k - 1] if k >= 1 else c, t[k])
                elif z == -1:
                    p[y][x] = _avg3(l[0], c, t[0])
                else:
                    p[y][x] = _avg3(l[y - 1], l[y - 2], l[y - 3] if y >= 3 else c)
    elif mode == 6:
        for y in range(4):
            for x in range(4):
                z = 2 * y - x
                if z >= 0:
                    k = y - (x >> 1)
                    p[y][x] = _avg2(l[k - 1] if k >= 1 else c, l[k]) if not (z & 1) \
                        else _avg3(l[k - 2] if k >= 2 else c, l[k - 1] if k >= 1 else c, l[k])
                elif z == -1:
                    p[y][x] = _avg3(t[0], c, l[0])
                else:
                    p[y][x] = _avg3(t[x - 1], t[x - 2], t[x - 3] if x >= 3 else c)
    elif mode == 7:
        for y in range(4):
            for x in range(4):
                k = x + (y >> 1)
                p[y][x] = _avg2(t[k], t[k + 1]) if not (y & 1) \
                    else _avg3(t[k], t[k + 1], t[k + 2])
    elif mode == 8:
        for y in range(4):
            for x in range(4):
                z = x + 2 * y
                k = y + (x >> 1)
                if z < 5:
                    p[y][x] = _avg2(l[k], l[k + 1]) if not (z & 1) \
                        else _avg3(l[k], l[k + 1], l[k + 2])
                elif z == 5:
                    p[y][x] = _avg3(l[2], l[3], l[3])
                else:
                    p[y][x] = l[3]
    else:
        raise ValueError("intra 4x4 mode %d" % mode)

    for y in range(4):
        base = off + y * STRIDE
        buf[base:base + 4] = bytes(p[y])


# The mode of a 4x4 block is coded against its neighbours exactly as in H.264:
# predicted = min(above, left), a 1 bit accepts it, otherwise 3 bits carry the
# remainder with the predicted value skipped. The decoder keeps the neighbour
# modes in a byte array at 0x030008c2 whose row stride is 5 -- four block columns
# plus one border column preset to 9, the "unavailable" marker that maps to DC.
I4_UNAVAILABLE = 9
I4_CTX_STRIDE = 5


def intra4x4_mode(above, left, flag, rem=None):
    """Resolve one block's mode from its neighbours (loop at 0x030008fc)."""
    pred = min(above, left)
    if pred == I4_UNAVAILABLE:
        pred = 2
    if flag:
        return pred
    return rem + 1 if rem >= pred else rem


# ------------------------------------------------------------ inter prediction
#
# Motion vectors are **full-pel**. There is no sub-pel interpolation anywhere:
# the four-entry table at 0x03001568, indexed by `fp & 3`, selects between four
# ldm alignment fixups, not filter phases. Variant 1 loads from one byte below
# the source, pulls 17 bytes and shifts the window right by 8 bits; variants 2
# and 3 do the same at halfword and 3-byte offsets, and 2 and 3 additionally fix
# up chroma, which is misaligned by 2 exactly when mv_x & 3 >= 2. All four
# produce the same pixels, so a byte-wise copy reproduces them.
#
# A macroblock names one of THREE reference frames through the context pointer:
#   [r0,#-0x20]/[r0,#-0x1c]   luma/chroma base of reference 0
#   [r0,#-0x18]/[r0,#-0x14]   reference 1
#   [r0,#-0x10]/[r0,#-0x0c]   reference 2
# Modes 0/8/10 copy from reference 0/1/2 with the MV the dispatcher predicted
# from neighbours; modes 3/9/11 are the same with a two-se(v) delta added.

def mv_offsets(mv_x, mv_y):
    """Byte offsets a motion vector produces in each plane (0x03001578).

    Luma is simply mv_x + mv_y*STRIDE. Chroma halves the vector, but because
    samples are two bytes the x term comes back out as mv_x rounded down to
    even -- which is what `bic #1` does on a two's-complement value.
    """
    return (mv_x + mv_y * STRIDE,
            (mv_x & ~1) + ((mv_y & ~1) // 2) * STRIDE)


def inter_copy(buf, ref, off, mv_x, mv_y, w, h):
    """Full-pel motion compensation of one luma block."""
    ly, _ = mv_offsets(mv_x, mv_y)
    for r in range(h):
        s = off + ly + r * STRIDE
        d = off + r * STRIDE
        buf[d:d + w] = ref[s:s + w]


def inter_copy_chroma(buf, ref, off, mv_x, mv_y, w, h):
    """The chroma half: w bytes wide (w/2 interleaved samples), h/2 rows.

    Luma and chroma live in separate planes, so this takes its own buffer pair
    rather than sharing luma's -- the vector is halved by mv_offsets.
    """
    _, lc = mv_offsets(mv_x, mv_y)
    for r in range(h // 2):
        s = off + lc + r * STRIDE
        d = off + r * STRIDE
        buf[d:d + w] = ref[s:s + w]


def inter_copy_dc(buf, ref, off, mv_x, mv_y, w, h, dc):
    """Mode 5 (0x03001854): motion compensation plus a coded DC correction.

    The vector is explicit -- two se(v) rather than a predicted candidate -- and
    three further se(v) carry one offset per component, applied as
    clip(pixel + 2*delta). The doubling and the clamp are both in the ARM:
    `adds sb, sb, r8, lsl #1`, `movlt #0`, `cmp #0xff`, `movgt #0xff`.
    """
    ly, _ = mv_offsets(mv_x, mv_y)
    for r in range(h):
        s = off + ly + r * STRIDE
        d = off + r * STRIDE
        for i in range(w):
            p = ref[s + i] + 2 * dc
            buf[d + i] = 0 if p < 0 else 255 if p > 255 else p


def intra_midpoint_corrected(buf, off, w, h, delta):
    """Mode 4 (0x03001ac0): the mode-3 subdivision with a *coded* corner.

    Instead of taking the bottom-right corner straight from the neighbours, it
    adds a signed correction: corner = ((TR + BL + 1) >> 1) + 2*se(v). The fill
    that follows is the same 0x03001a68 routine mode 3 uses. Chroma gets its own
    correction per component through 0x03001a3c, which is why the mode carries
    three se(v) in total.
    """
    a = buf[off + (w - 1) - STRIDE]
    b = buf[off + (h - 1) * STRIDE - 1]
    # strb truncates rather than clamps, so the correction wraps at 8 bits.
    corner = ((a + b + 1) >> 1) + 2 * delta
    buf[off + (h - 1) * STRIDE + (w - 1)] = corner & 0xff
    _midpoint_fill(buf, off, w, h, 1, corner)


def chroma_midpoint_corrected(buf, off, cw, ch, deltas):
    """Chroma half of mode 4 (0x03001a3c), once per component."""
    for c in range(CHROMA_BPS):
        o = off + c
        a = buf[o + (cw - 1) * CHROMA_BPS - STRIDE]
        b = buf[o + (ch - 1) * STRIDE - CHROMA_BPS]
        corner = ((a + b + 1) >> 1) + 2 * deltas[c]
        buf[o + (ch - 1) * STRIDE + (cw - 1) * CHROMA_BPS] = corner & 0xff
        _midpoint_fill(buf, o, cw, ch, CHROMA_BPS, corner)


# The 16x16 dispatcher's mode space, confirmed against vx_grammar.py's se(v)
# counts. Modes 12-23 are exactly modes 0-11 with a residual appended.
#   0, 8,10  copy from reference 0/1/2, predicted MV        0 se
#   3, 9,11  same, plus a two-se(v) MV delta                2 se
#   1        split into two 16x8      2        two 8x16
#   4        midpoint, corrected corners                    3 se
#   5        motion compensation with DC correction         5 se
#   6        intra 16x16              7        intra 4x4
# Four frame buffers in a ring (0x03006d30): reference 0 is buffer[n & 3],
# reference 1 buffer[(n-1) & 3], reference 2 buffer[(n-2) & 3], with n bumped
# once per frame -- so the references are just the last three decoded frames.
INTER_REFS = 3
REF_RING = 4


# ------------------------------------------------------ macroblock loop (0x03000520)

# A macroblock's motion vector is predicted from three neighbours by the median,
# computed the cheap way -- sum minus min minus max, per component -- which is
# H.264's predictor. The result goes into fp/ip and the dispatcher at 0x03001dac
# is called with it; modes 0/8/10 use it as-is, 3/9/11 add a two-se(v) delta.
#
# The predictor stores its final vector back with `strh`, packed as
# mv_x | (mv_y << 8), so **each component is a signed byte** -- vectors are
# limited to roughly +/-128 full pixels. The context is one such halfword per
# macroblock with a row stride of 0x24 (18 entries: the frame's 15 plus border).
MV_CONTEXT_STRIDE = 0x24
MV_ENTRY_SIZE = 2


def predict_mv(a, b, c):
    """Median of three neighbouring vectors (0x030005b0)."""
    def med(x, y, z):
        return x + y + z - min(x, y, z) - max(x, y, z)
    return med(a[0], b[0], c[0]), med(a[1], b[1], c[1])


def pack_mv(mv_x, mv_y):
    """The halfword the predictor writes: `add fp, fp, ip, lsl #8`.

    An arithmetic sum, not a bitfield -- so a negative mv_x borrows from the
    high byte.
    """
    return (mv_x + (mv_y << 8)) & 0xffff


def unpack_mv(v):
    """Undo that, including the borrow.

    The ARM reads the entry with ldrsh, takes the high byte with asr #8 and the
    low byte by sign-extending, then adds one back to y when x is negative
    (`addlt r5, r5, #1`) -- undoing the borrow the packing sum introduced.
    """
    s = v - 0x10000 if v & 0x8000 else v
    x = s & 0xff
    if x > 127:
        x -= 256
    y = s >> 8
    if x < 0:
        y += 1
    return x, y


# The three neighbours the median takes, as halfword offsets from a macroblock's
# own context entry: left, above, above-right -- H.264's A, B and C.
MV_NEIGHBOURS = (-1, -MV_CONTEXT_STRIDE // 2, -MV_CONTEXT_STRIDE // 2 + 1)


# Per-macroblock the loop advances luma and chroma by 16 bytes each; per row of
# macroblocks luma advances 0x1000 (16 lines) and chroma 0x800 (8 lines), with
# the width read from [r0,#0x18].
MB_STEP = 16
MB_ROW_STEP_LUMA = 0x1000
MB_ROW_STEP_CHROMA = 0x800

# The frame driver passes ctx + 0xbc, but FUN_03000520 re-bases it by 4 + 0x2c,
# so the predictors see r0 = ctx + 0xec. That pins every [r0,#N] to a field the
# driver at 0x03006d90 demonstrably writes:
#
#   [r0,#-0x20] ctx+0xcc  reference 0, luma base
#   [r0,#-0x1c] ctx+0xd0  reference 0, chroma base
#   [r0,#-0x18] ctx+0xd4  reference 1, luma base
#   [r0,#-0x14] ctx+0xd8  reference 1, chroma base
#   [r0,#-0x10] ctx+0xdc  reference 2, luma base
#   [r0,#-0x0c] ctx+0xe0  reference 2, chroma base
#   [r0,#-0x08] ctx+0xe4  current frame, luma base
#   [r0,#-0x04] ctx+0xe8  current frame, chroma base
#   [r0,# 0x00] ctx+0xec  luma byte offset of the current macroblock
#   [r0,# 0x04] ctx+0xf0  chroma byte offset of the current macroblock
#   [r0,# 0x08] ctx+0xf4  motion-vector context pointer
#   [r0,# 0x18] ctx+0x104 frame width in pixels
#
# The two offsets are stored as zero right before the loop runs, which is the
# check that fixes the base: only at 0xec/0xf0 do the driver's stores line up.
#
# The two offsets at +0x00/+0x04 are what every predictor adds to its base, and
# what the intra modes test for edge availability -- so a macroblock's position
# and its neighbour availability are the same number.
CTX_BASE = 0xec
CTX_FIELDS = {
    "ref0_luma": -0x20, "ref0_chroma": -0x1c,   # relative to ctx + CTX_BASE
    "ref1_luma": -0x18, "ref1_chroma": -0x14,
    "ref2_luma": -0x10, "ref2_chroma": -0x0c,
    "cur_luma": -0x08, "cur_chroma": -0x04,
    "off_luma": 0x00, "off_chroma": 0x04,
    "mv_ctx": 0x08, "width": 0x18,
}


# ---------------------------------------------------------------- TODO
#
# Still to reverse and port before this can render a picture:
#
#   * Which of the three reference frames a mode names is known, but not how the
#     three are rotated as frames are decoded -- that lives in the frame-level
#     setup around 0x03006e40, not in the macroblock loop.
#   * Tying it all together: walk vx_sim's symbol stream into a frame buffer.
#     Every primitive it needs is now here; what is missing is the driver.
#
# Done: prediction and residual, end to end. 16x16 intra modes 0-3, the four
# chroma modes, all nine 4x4 modes with their neighbour-predicted mode coding,
# full-pel motion compensation from three reference frames, the corrected-corner
# midpoint mode, the DC-corrected motion compensation mode, median motion vector
# prediction, and both residual paths for all three planes.
