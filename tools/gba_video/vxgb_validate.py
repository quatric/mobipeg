#!/usr/bin/env python3
"""Validate VXGB video parsing against every container seek bit.

Usage:
    python3 tools/gba_video/gbavx_extract.py ROM -o build_gbavx
    python3 tools/gba_video/vxgb_validate.py ROM [build_gbavx]
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from gbavx_extract import find_streams
from vxgb_sim import validate_seek


def main(argv):
    if len(argv) not in (2, 3):
        print(__doc__, file=sys.stderr)
        return 2
    data_dir = (argv[2] if len(argv) == 3 else
                os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(
                    os.path.abspath(__file__)))), 'build_gbavx'))
    with open(argv[1], 'rb') as f:
        rom = f.read()

    streams = [(i, stream) for i, stream in enumerate(find_streams(rom))
               if stream.magic == b'VXGB']
    if not streams:
        print('no VXGB streams found', file=sys.stderr)
        return 1

    total_exact = total = 0
    for index, stream in streams:
        path = os.path.join(data_dir, 'stream%02d.video' % index)
        try:
            with open(path, 'rb') as f:
                video = f.read()
        except FileNotFoundError:
            print('%s missing; run gbavx_extract.py -o first' % path,
                  file=sys.stderr)
            return 1
        exact, bad = validate_seek(video, stream.seek, stream.nb_frames,
                                   stream.width, stream.height)
        count = exact + len(bad)
        total_exact += exact
        total += count
        print('stream%d: %dx%d %d frames -> %d/%d segments exact' %
              (index, stream.width, stream.height, stream.nb_frames,
               exact, count))
        for frame, want, got in bad[:5]:
            print('    frame %6d: expected bit %d, ended at %d (%+d)' %
                  (frame, want, got, got - want))

    print('%d/%d segments exact' % (total_exact, total))
    return 0 if total_exact == total else 1


if __name__ == '__main__':
    sys.exit(main(sys.argv))
