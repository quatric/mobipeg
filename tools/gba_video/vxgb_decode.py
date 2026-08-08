#!/usr/bin/env python3
"""Reconstruct frames from an extracted original VXGB video bitstream.

This reuses the byte-exact VX++ prediction primitives; VXGB differs in framing
and residual entropy coding, which are supplied by vxgb_sim.
"""

import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import vx_decode as D
import vx_reconstruct as R
import vxgb_sim as S


def _adapt_unit(unit, mb_x, mb_y):
    unit['off'] = ((unit['y'] - mb_y) * R.STRIDE + unit['x'] - mb_x)
    return unit


def load_metadata(path):
    metadata = D.load_header(path)
    with open(path) as f:
        for line in f:
            if line.startswith('quantizer'):
                metadata['quantizer'] = int(line.split()[-1])
    return metadata


def decode_frames(video, width, height, quantizer, count, seek=None):
    br = S.Bits(video)
    tables = S.CAVLCTables()
    mbs_x, mbs_y = width // 16, height // 16
    arena = bytearray([D.GREY]) * (2 * D.MARGIN + D.REF_RING * D.FRAME_SLOT)
    slots = [D.Frame(width, height, arena, slot) for slot in range(D.REF_RING)]
    refs = [slots[3], slots[2], slots[1]]
    row = R.MV_CONTEXT_STRIDE // 2
    starts = ({frame: bit for frame, bit, _, _ in seek if bit or frame == 0}
              if seek else {0: 0})
    pos = 0

    for frame_no in range(count):
        if frame_no in starts:
            if pos != starts[frame_no]:
                raise ValueError('frame %d expected seek bit %d, ended at %d' %
                                 (frame_no, starts[frame_no], pos))
            pos = starts[frame_no]
        frame = slots[frame_no % D.REF_RING]
        state = S.FrameState()
        mvctx = [0] * (row * (mbs_y + 2))
        for mb_y in range(mbs_y):
            for mb_x in range(mbs_x):
                x, y = mb_x * 16, mb_y * 16
                ml = y * R.STRIDE + x
                mc = mb_y * 8 * R.STRIDE + x
                idx = (mb_y + 1) * row + 1 + mb_x
                mvctx[idx] = 0
                pred = R.predict_mv(*[R.unpack_mv(mvctx[idx + delta])
                                      for delta in R.MV_NEIGHBOURS])

                def paint(unit, bx=x, by=y, f=frame, p=pred, i=idx,
                          luma=ml, chroma=mc):
                    D._paint(f, refs, _adapt_unit(unit, bx, by), luma, chroma,
                             quantizer, p, mvctx, i)

                pos = S.decode_unit(br, tables, state, pos, x, y, sink=paint)
        yield frame
        refs = [frame] + refs[:D.REF_DEPTH - 1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('stream', help='extracted streamNN.video')
    parser.add_argument('-n', '--frames', type=int, default=1)
    parser.add_argument('--header', help='extracted header (default beside stream)')
    parser.add_argument('--raw', help='write raw NV12 instead of PGM files')
    parser.add_argument('-o', '--outdir', default='vxgb_frames')
    args = parser.parse_args()
    header_path = args.header or os.path.splitext(args.stream)[0] + '.hdr.txt'
    meta = load_metadata(header_path)
    with open(args.stream, 'rb') as f:
        video = f.read()

    frames = decode_frames(video, meta['width'], meta['height'],
                           meta['quantizer'], args.frames, meta['seek'])
    if args.raw:
        with open(args.raw, 'wb') as output:
            for frame in frames:
                D.write_nv12(frame, output)
    else:
        os.makedirs(args.outdir, exist_ok=True)
        for number, frame in enumerate(frames):
            D.write_pgm(frame, os.path.join(args.outdir,
                                            'frame%04d.pgm' % number))
    return 0


if __name__ == '__main__':
    sys.exit(main())
