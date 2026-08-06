#!/usr/bin/env python3
"""Block renderer for ADS-era GBA Video, built from the recovered pixel model.

Codebook and colour handling are now taken from the ROM rather than guessed
(see doc/gba_video_ads.md):

  * a codebook always holds 256 entries;
  * an entry is planar -- blk_w*blk_h luma bytes, then blk_h Cb, then blk_h Cr,
    i.e. chroma is subsampled 4:1 horizontally, one sample per block row;
  * block geometry comes from the stream's mode nibble
    (4x4, 4x3, 3x3, 4x2 for mode pairs 0/1, 2/3, 4/5, 6/7);
  * the IWRAM converter at 0x030004f8 gives the exact inverse colour transform
        R = clamp(Y + 2*Cr)   G = clamp(Y - Cr - Cb/2)   B = clamp(Y + 2*Cb)
    with *signed* chroma, packed as BGR555.

Entries are residuals accumulated over time, not absolute pixels. Plain
per-frame accumulation is implemented here and gets the picture out
recognisably, but it drifts (a period-2 checkerboard on flat areas), so the
predictor is not yet exactly right. This is the reproduction case for the
remaining work.

Usage:
    ads_render.py <resource.mmstr> <out.png> [--frame N] [--block WxH]
"""

import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ads_extract import parse_mmstr, parse_video, iter_chunks, T_VIDEO  # noqa: E402
from ads_lzma import decode_blob                                        # noqa: E402


def s8(v):
    return v - 256 if v > 127 else v


def decode_frames(path, bw, bh, upto):
    """Accumulate residuals up to frame `upto`; return (Y, Cb, Cr, geometry)."""
    d = open(path, 'rb').read()
    info = [parse_video(d, o, n) for t, o, n in parse_mmstr(d) if t == T_VIDEO][0]

    first = next(iter_chunks(d, info))
    per = len(decode_blob(d, first[0] + 8 + first[2])[0]) // first[1]
    gw = 60
    gh = per // gw
    npx, nch, esz = bw * bh, bh, bw * bh + 2 * bh
    W, H = gw * bw, gh * bh

    Y = [0] * (W * H)
    CB = [0] * (gw * H)
    CR = [0] * (gw * H)
    seen = 0
    for p, nf, sa, sb in iter_chunks(d, info):
        A = bytes(decode_blob(d, p + 8)[0])
        B = bytes(decode_blob(d, p + 8 + sa)[0])
        for f in range(nf):
            fr = B[f * per:(f + 1) * per]
            for by in range(gh):
                for bx in range(gw):
                    e = fr[by * gw + bx] * esz
                    for j in range(bh):
                        row = (by * bh + j) * W + bx * bw
                        for i in range(bw):
                            v = Y[row + i] + s8(A[e + j * bw + i])
                            Y[row + i] = 0 if v < 0 else (255 if v > 255 else v)
                        c = (by * bh + j) * gw + bx
                        CB[c] = max(-128, min(127, CB[c] + s8(A[e + npx + j])))
                        CR[c] = max(-128, min(127, CR[c] + s8(A[e + npx + nch + j])))
            seen += 1
            if seen >= upto:
                return Y, CB, CR, (W, H, gw, bw)
    return Y, CB, CR, (W, H, gw, bw)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('mmstr')
    ap.add_argument('out')
    ap.add_argument('--frame', type=int, default=20)
    ap.add_argument('--block', default='4x2')
    args = ap.parse_args()
    bw, bh = (int(x) for x in args.block.split('x'))

    Y, CB, CR, (W, H, gw, bw) = decode_frames(args.mmstr, bw, bh, args.frame)

    from PIL import Image
    im = Image.new('RGB', (W, H))
    px = im.load()
    for y in range(H):
        for x in range(W):
            yy, cb, cr = Y[y * W + x], CB[y * gw + x // bw], CR[y * gw + x // bw]
            px[x, y] = tuple(max(0, min(255, v)) for v in
                             (yy + 2 * cr, yy - cr - (cb >> 1), yy + 2 * cb))
    im.save(args.out)
    print('wrote %s (%dx%d, %d frames)' % (args.out, W, H, args.frame))


if __name__ == '__main__':
    main()
