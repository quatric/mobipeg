#!/usr/bin/env python3
"""Reconstruct VX++ frames: drive vx_reconstruct's primitives from vx_sim's symbols.

    ./vx_decode.py                 # frame 0 of stream00 -> frame000.pgm
    ./vx_decode.py -n 30 -o out    # first 30 frames

vx_sim decodes the bitstream into a tree of units; this walks that tree and
paints each leaf. Modes 0-11 predict, 12-23 predict and then add a residual, so
a leaf's base mode is `mode - 12` when it is 12 or above.

Reference-frame rotation is the one part of the architecture still unread, so
inter modes currently predict from the previous frame for all three reference
slots. Intra-only frames are exact; frames using inter modes will drift wherever
the real decoder would have chosen reference 1 or 2.
"""

import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import vx_reconstruct as R
import vx_sim as S

STRIDE = R.STRIDE
# The hardware frame buffers are far larger than the image, so a motion vector
# pointing off the edge still lands in allocated memory. Mirror that with a
# generous margin above and below rather than one row -- vectors reach +/-128
# pixels, and a block near an edge would otherwise index past the end.
SLACK = 160
MARGIN = SLACK * STRIDE
GREY = 0x80

# Four frame buffers in a ring (0x03006d30): reference 0 is buffer[n & 3],
# reference 1 buffer[(n-1) & 3] and reference 2 buffer[(n-2) & 3], with n
# incremented once per frame. So the three references are simply the last three
# decoded frames, most recent first.
REF_OF_MODE = {0: 0, 3: 0, 8: 1, 9: 1, 10: 2, 11: 2}
REF_DEPTH = 3


class Frame:
    """Luma and interleaved-chroma planes with the decoder's 0x100 pitch."""

    def __init__(self, width, height):
        self.width, self.height = width, height
        self.luma = bytearray([GREY]) * (STRIDE * (height + 2 * SLACK))
        self.chroma = bytearray([GREY]) * (STRIDE * (height // 2 + 2 * SLACK))

    def copy(self):
        f = Frame(self.width, self.height)
        f.luma[:] = self.luma
        f.chroma[:] = self.chroma
        return f


def _paint(fr, refs, unit, mb_luma, mb_chroma, qp, mv_pred, mvctx, mvidx):
    """Paint one leaf of the partition tree."""
    mode = unit["mode"]
    base = mode - 12 if mode >= 12 else mode
    if base in (1, 2):                      # a split: the children did the work
        return
    size = unit["size"]
    if size is None:
        return
    w, h = size
    off = unit["off"]
    lo = MARGIN + mb_luma + off
    # A block's chroma offset halves its position but keeps its byte width.
    co = MARGIN + mb_chroma + (off & ~0xff) // 2 + (off & 0xff)
    cw, ch = w // 2, h // 2
    se = unit["se"]
    # Frame-relative offset: what the ARM tests for edge availability.
    fo = mb_luma + off
    have_left, have_top = (fo & 0xff) != 0, (fo & 0xff00) != 0

    if base == 6:
        luma_mode, chroma_mode = unit["intra"]
        if luma_mode == 0:
            R.intra_vertical(fr.luma, lo, w, h)
        elif luma_mode == 1:
            R.intra_horizontal(fr.luma, lo, w, h)
        elif luma_mode == 2:
            R.intra_dc(fr.luma, lo, w, h, have_left, have_top)
        else:
            R.intra_midpoint(fr.luma, lo, w, h)
        if cw and ch:
            if chroma_mode == 0:
                R.chroma_dc(fr.chroma, co, cw, ch, have_left, have_top)
            elif chroma_mode == 1:
                R.chroma_horizontal(fr.chroma, co, cw, ch)
            elif chroma_mode == 2:
                R.chroma_vertical(fr.chroma, co, cw, ch)
            else:
                R.chroma_midpoint(fr.chroma, co, cw, ch)
    elif base == 7:
        flags, chroma_mode = unit["intra4"]
        # Modes are predicted from the neighbouring blocks; 9 marks unavailable.
        ctx = {}
        for i, rem in enumerate(flags):
            bx, by = i % (w // 4), i // (w // 4)
            above = ctx.get((bx, by - 1), R.I4_UNAVAILABLE)
            left = ctx.get((bx - 1, by), R.I4_UNAVAILABLE)
            m = R.intra4x4_mode(above, left, rem is None, rem)
            ctx[(bx, by)] = m
            R.intra4x4(fr.luma, lo + by * 4 * STRIDE + bx * 4, m)
        if cw and ch:
            if chroma_mode == 0:
                R.chroma_dc(fr.chroma, co, cw, ch, have_left, have_top)
            elif chroma_mode == 1:
                R.chroma_horizontal(fr.chroma, co, cw, ch)
            elif chroma_mode == 2:
                R.chroma_vertical(fr.chroma, co, cw, ch)
            else:
                R.chroma_midpoint(fr.chroma, co, cw, ch)
    elif base == 4:
        R.intra_midpoint_corrected(fr.luma, lo, w, h, se[0] if se else 0)
        if cw and ch and len(se) >= 3:
            R.chroma_midpoint_corrected(fr.chroma, co, cw, ch, se[1:3])
    elif base == 5:
        # An explicit vector, not a delta: 0x03001854 builds it straight from
        # its two se(v). It also never writes the context back.
        mv_x, mv_y = (se + [0] * 5)[:2]
        dc = (se + [0] * 5)[2:5]
        ref = refs[0]
        R.inter_copy_dc(fr.luma, ref.luma, lo, mv_x, mv_y, w, h, dc[0])
        R.inter_copy_chroma(fr.chroma, ref.chroma, co, mv_x, mv_y, w, h)
    else:
        # 0/8/10 take the predicted vector as-is; 3/9/11 add a delta to it
        # (0x030015a4's two se(v)). Both then store the result for later
        # macroblocks to predict from.
        mv_x, mv_y = mv_pred
        if len(se) >= 2:
            mv_x += se[0]
            mv_y += se[1]
        ref = refs[REF_OF_MODE[base]]
        R.inter_copy(fr.luma, ref.luma, lo, mv_x, mv_y, w, h)
        R.inter_copy_chroma(fr.chroma, ref.chroma, co, mv_x, mv_y, w, h)
        mvctx[mvidx] = R.pack_mv(mv_x, mv_y)

    if mode >= 12 and "resid" in unit:
        _residual(fr, unit, lo, co, qp)


def _residual(fr, unit, lo, co, qp):
    """Four 8x8 quadrants, each a 6-bit CBP over four luma blocks then Cb, Cr."""
    for q, (_, blocks) in enumerate(unit["resid"]):
        qx, qy = (q % 2) * 8, (q // 2) * 8
        for b, coeffs in blocks:
            res = R.idct4x4(coeffs, qp)
            if b < 4:
                bx, by = (b % 2) * 4, (b // 2) * 4
                R.add_residual(fr.luma, lo + (qy + by) * STRIDE + qx + bx, res, 0)
            else:
                R.add_residual(fr.chroma,
                               co + (qy // 2) * STRIDE + qx, res, b - 3)


def decode_frames(path, width, height, n_frames, start_bit=0):
    """Yield reconstructed Frames."""
    tab = S.load16(S.VLC)
    vofs, rofs = S.load(S.VOFS), S.load(S.ROFS)
    br = S.Bits(S.load(path))
    mbs_x, mbs_y = width // 16, height // 16
    # Each seek segment opens with its own quantiser (doc section 13).
    qp, pos = S.read_ue(br, start_bit)
    refs = [Frame(width, height) for _ in range(REF_DEPTH)]
    # One halfword per macroblock, 18 to a row (stride 0x24), with a border row
    # above and a border column to the left so the three neighbours always read
    # an allocated zero at the frame edges.
    row = R.MV_CONTEXT_STRIDE // 2
    for _ in range(n_frames):
        fr = refs[0].copy()
        mvctx = [0] * (row * (mbs_y + 2))
        for mb in range(mbs_x * mbs_y):
            mb_x, mb_y = mb % mbs_x, mb // mbs_x
            ml = mb_y * 16 * STRIDE + mb_x * 16
            mc = mb_y * 8 * STRIDE + mb_x * 16
            idx = (mb_y + 1) * row + 1 + mb_x
            mvctx[idx] = 0            # `strh r4,[r8]` with r4 = 0, before the mode
            pred = R.predict_mv(*[R.unpack_mv(mvctx[idx + d])
                                  for d in R.MV_NEIGHBOURS])
            pos = S.decode_unit(br, tab, vofs, rofs, pos, S.TOP, 0, None, 0,
                                lambda u, a=ml, b=mc, f=fr, p=pred, i=idx:
                                _paint(f, refs, u, a, b, qp, p, mvctx, i))
        pos += 1                                   # inter-frame marker
        yield fr
        refs = [fr] + refs[:REF_DEPTH - 1]


def write_pgm(fr, path):
    with open(path, "wb") as f:
        f.write(b"P5\n%d %d\n255\n" % (fr.width, fr.height))
        for y in range(fr.height):
            row = MARGIN + y * STRIDE
            f.write(bytes(fr.luma[row:row + fr.width]))


def write_nv12(fr, fh):
    """Raw NV12: the luma plane, then the chroma plane as-is.

    The decoder's chroma layout -- Cb and Cr interleaved, half resolution in both
    directions -- is exactly NV12's UV plane, so no repacking is needed.
    """
    for y in range(fr.height):
        row = MARGIN + y * STRIDE
        fh.write(bytes(fr.luma[row:row + fr.width]))
    for y in range(fr.height // 2):
        row = MARGIN + y * STRIDE
        fh.write(bytes(fr.chroma[row:row + fr.width]))


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-n", "--frames", type=int, default=1)
    ap.add_argument("-o", "--outdir", default=".")
    ap.add_argument("--width", type=int, default=240)
    ap.add_argument("--height", type=int, default=112)
    ap.add_argument("--stream", default=S.STREAM)
    ap.add_argument("--raw", help="write raw NV12 here instead of PGMs")
    args = ap.parse_args()

    if args.raw:
        with open(args.raw, "wb") as fh:
            for i, fr in enumerate(decode_frames(args.stream, args.width,
                                                 args.height, args.frames)):
                write_nv12(fr, fh)
                if not (i % 25):
                    print("frame", i, flush=True)
        print("wrote", args.raw)
        return 0

    os.makedirs(args.outdir, exist_ok=True)
    for i, fr in enumerate(decode_frames(args.stream, args.width, args.height,
                                         args.frames)):
        p = os.path.join(args.outdir, "frame%03d.pgm" % i)
        write_pgm(fr, p)
        print("wrote", p)
    return 0


if __name__ == "__main__":
    sys.exit(main())
