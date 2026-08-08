#!/usr/bin/env python3
"""Check the VX++ parser against the container's own seek table.

Every seek entry records the bit offset at which a frame starts, so parsing a
segment and landing exactly on the next entry's offset is a bit-exact check on
the whole grammar -- one that needs no emulator. Run it over a cart and every
stream in it is verified end to end:

    ./gbavx_extract.py <rom.gba> -o ../../build_gbavx    # once
    ./vx_validate.py <rom.gba>

Streams are read from build_gbavx/ (or $VXPP_DATA), matched to the ROM's own
stream order.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from gbavx_extract import find_streams
from vx_sim import DATA, decode_stream


def main(argv):
    if len(argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2

    with open(argv[1], 'rb') as f:
        rom = f.read()

    total_ok = total = 0
    for i, s in enumerate(find_streams(rom)):
        path = os.path.join(DATA, 'stream%02d.video' % i)
        if not os.path.exists(path):
            print('stream%d: %s missing -- run gbavx_extract.py -o first' %
                  (i, path), file=sys.stderr)
            return 1
        mbs = (s.width // 16) * (s.height // 16)
        ok, bad = decode_stream(s.seek, s.nb_frames, s.width, s.height, path)
        n = ok + len(bad)
        total_ok, total = total_ok + ok, total + n
        print('stream%d: %dx%d %3d MBs/frame %6d frames  ->  %d/%d segments exact'
              % (i, s.width, s.height, mbs, s.nb_frames, ok, n))
        for frame, want, got in bad[:5]:
            print('    frame %6d: expected bit %d, ended at %d (%+d)'
                  % (frame, want, got, got - want))

    print('\n%d/%d segments exact' % (total_ok, total))
    return 0 if total_ok == total else 1


if __name__ == '__main__':
    sys.exit(main(sys.argv))
