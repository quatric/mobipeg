#!/usr/bin/env python3
"""Experimental block renderer for ADS-era GBA Video.

NOT a correct decoder. The codebook transform is not reverse engineered yet.
This treats each 12-byte codebook entry as 8 luma + 2 Cb + 2 Cr, which is
known to be wrong -- the entry unit is really six little-endian u16 -- but it
is enough to render recognisable geometry, and it is what confirmed that a
frame really is a 60x80 raster of 4x2 blocks covering the 240x160 screen.

Kept in the tree because it is the reproduction case for the remaining work:
run it, and the Majesco logo comes out in the right shape with the wrong
colours and a period-2 checkerboard. See doc/gba_video_ads.md.

Usage:
    ads_render_experiment.py <resource.mmstr> <outdir> [--chunk N]
"""

import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ads_extract import parse_mmstr, parse_video, iter_chunks, T_VIDEO  # noqa: E402
from ads_lzma import decode_blob                                        # noqa: E402


def ycc(y, cb, cr):
    r = y + 1.402 * (cr - 128)
    g = y - 0.344 * (cb - 128) - 0.714 * (cr - 128)
    b = y + 1.772 * (cb - 128)
    return tuple(max(0, min(255, int(v))) for v in (r, g, b))


def render(codebook, frame):
    """60x80 index raster + codebook -> a 240x160 RGB image (approximate)."""
    from PIL import Image
    im = Image.new('RGB', (240, 160))
    px = im.load()
    for by in range(80):
        for bx in range(60):
            e = frame[by * 60 + bx] * 12
            blk = codebook[e:e + 12]
            luma, cb, cr = blk[0:8], blk[8:10], blk[10:12]
            for j in range(2):
                for i in range(4):
                    px[bx * 4 + i, by * 2 + j] = ycc(luma[j * 4 + i],
                                                     cb[i // 2], cr[i // 2])
    return im


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('mmstr')
    ap.add_argument('outdir')
    ap.add_argument('--chunk', type=int, default=0)
    args = ap.parse_args()

    data = open(args.mmstr, 'rb').read()
    info = None
    for rtype, off, nbytes in parse_mmstr(data):
        if rtype == T_VIDEO:
            info = parse_video(data, off, nbytes)
            break
    if info is None:
        sys.exit('no type-1 video resource in this .mmstr')

    chunks = list(iter_chunks(data, info))
    if not 0 <= args.chunk < len(chunks):
        sys.exit(f'chunk out of range (file has {len(chunks)})')
    coff, nframes, sizeA, _sizeB = chunks[args.chunk]

    codebook, _p, _u = decode_blob(data, coff + 8)
    indices, _p, _u = decode_blob(data, coff + 8 + sizeA)
    print(f'chunk {args.chunk}: {nframes} frames, '
          f'codebook {len(codebook)} bytes, indices {len(indices)} bytes')

    os.makedirs(args.outdir, exist_ok=True)
    for f in range(nframes):
        frame = indices[f * 4800:(f + 1) * 4800]
        if len(frame) < 4800:
            break
        out = os.path.join(args.outdir, f'chunk{args.chunk}_f{f:03d}.png')
        render(codebook, frame).save(out)
    print(f'wrote {nframes} frames to {args.outdir}')


if __name__ == '__main__':
    main()
