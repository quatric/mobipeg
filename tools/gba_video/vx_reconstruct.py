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

# Block size handled by each dispatcher, from the r4/r5 immediates its mode-6
# handler passes to the intra predictor at FUN_03000884.
BLOCK_SIZE = {
    0x03001dac: (16, 16),
    0x03002184: (16, 8),
    0x030024ac: (16, 4),
    0x030030f4: (8, 16),
    0x03003584: (8, 8),
    0x03003818: (8, 4),
    0x030041cc: (4, 16),
    0x03004468: (4, 8),
    0x03004750: (4, 4),
    # The remaining seven dispatchers sit below this and offer no whole-block
    # intra mode, so their mode-6 slot carries no size.
}

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


def add_residual(buf, off, residual):
    """clip(pred + residual), in place -- the frame buffer already holds the
    prediction when the residual arrives (0x03005abc onward)."""
    for r in range(4):
        base = off + r * STRIDE
        for c in range(4):
            p = buf[base + c] + residual[r * 4 + c]
            buf[base + c] = 0 if p < 0 else 255 if p > 255 else p


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


# ---------------------------------------------------------------- TODO
#
# Still to reverse and port before this can render a picture:
#
#   * Whole-block intra mode 3 (0x03000ba4) -- plane prediction.
#   * The four chroma intra modes at 0x03000874:
#       0x03000c08, 0x03000d90, 0x03000dd4, 0x03000e44
#   * The nine intra 4x4 modes at 0x030008d8, selected per sub-block by the
#     flag/remainder coding in FUN_030008fc:
#       0x03000ecc 0x03000eec 0x03000f90 0x03000fe0 0x030010c4
#       0x030011ac 0x030012b0 0x030013b4 0x030014b0
#   * Inter prediction: the predictor families reached from the mode tables,
#     with the half-pel interpolation loops around 0x03001704/0x03002a8c, and
#     the motion vectors carried as se(v) side data (3 for mode 4, 5 for mode 5).
#   * The six reconstruct variants dispatched at 0x0300598c via the table at
#     0x0300577c -- 0x03005a94 (implemented above) plus 0x03005b2c, 0x03005be4,
#     0x03005c9c, 0x03005e30, 0x03006044.
#   * Frame buffer plumbing: which of Cb/Cr the residual loop's plane index
#     selects, and reference-frame management for inter prediction.
