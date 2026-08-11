#!/usr/bin/env python3
"""Measure the real tag-0x3100 cell walk on hardware, byte by byte.

Background: `caimans22_frame.decode_raster_3100` was written from the Ghidra
decompile of `FUN_08000418`'s 0x3100 branch. A blit-set diff (see
`caimans_diff_blits.py`) showed it is structurally wrong -- only 2 of
hardware's 57 written cells overlap with the 402 cells it writes. So the
decompile reading must be discarded and the loop rebuilt from measurement.

This tool takes the measurement. It breaks on both blit primitives and, at
each hit, records:

  - which primitive (simple 0x03000114 / complex 0x0300015c)
  - the destination address (-> cell row/col, record-relative)
  - the live bitstream byte cursor (`*0x03005028`)

The byte cursor is advanced by the loop *before* it calls a blit, so the
delta between consecutive hits is exactly the bytes consumed by that cell
plus any mask reloads plus any skipped cells (skips consume no bytes). That
is enough to reconstruct the true per-cell cost without guessing.

    python3 tools/gba_video/caimans_measure_3100.py --record 1 --hits 80
"""
import argparse
import collections
import os
import struct
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from gdbrsp import RSP

ROM = "/Volumes/SSD/dlz/Folders/mobipeg/build_caimans/roms/caimans_badboys2.gba"
MGBA = "/Applications/mGBA.app/Contents/MacOS/mGBA"

FUN_08000418 = 0x08000418
BLIT_SIMPLE = 0x03000114
BLIT_COMPLEX = 0x0300015C
CURSOR_VAR = 0x03005028
VRAM = 0x06000000
STRIDE = 480


def launch(port):
    for attempt in range(3):
        proc = subprocess.Popen([MGBA, "-g", ROM],
                                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        for _ in range(40):
            time.sleep(0.5)
            try:
                return proc, RSP(port=port)
            except OSError:
                continue
        proc.kill()
    raise SystemExit("could not connect to mGBA on port %d" % port)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=2345)
    ap.add_argument("--record", type=int, default=1)
    ap.add_argument("--hits", type=int, default=80)
    args = ap.parse_args()

    proc, rsp = launch(args.port)
    try:
        rsp.bp(FUN_08000418 | 1)
        for _ in range(args.record + 1):
            rsp.cont()
        dest_base = rsp.regs()[3]
        rsp.rmbp(FUN_08000418 | 1)

        rsp.bp(BLIT_SIMPLE | 1)
        rsp.bp(BLIT_COMPLEX | 1)
        rsp.bp(FUN_08000418 | 1)     # stop if we run into the next record

        hits = []
        for _ in range(args.hits):
            rsp.cont()
            regs = rsp.regs()
            pc = regs[15] & ~1
            if pc == FUN_08000418:
                break
            cursor = struct.unpack("<I", rsp.mem(CURSOR_VAR, 4))[0]
            hits.append({
                "kind": "S" if pc == BLIT_SIMPLE else "C",
                "dest": regs[0],
                "cursor": cursor,
            })
    finally:
        proc.kill()

    print("record %d, dest base 0x%08x (abs row %d)"
          % (args.record, dest_base, (dest_base - VRAM) // STRIDE))
    print("%d blit hits captured\n" % len(hits))

    base_row = (dest_base - VRAM) // STRIDE
    print(" idx k   dest        row  col   cursor      d_cursor")
    prev = None
    for i, h in enumerate(hits):
        off = h["dest"] - VRAM
        row = off // STRIDE - base_row
        col = (off % STRIDE) // 2
        d = "" if prev is None else "%+d" % (h["cursor"] - prev)
        prev = h["cursor"]
        print(" %3d %s   0x%08x  %3d  %3d   0x%08x  %s"
              % (i, h["kind"], h["dest"], row, col, h["cursor"], d))

    # Pair complex blits into cells: a cell is (r,c) plus (r+2,c).
    print("\n--- structure ---")
    kinds = collections.Counter(h["kind"] for h in hits)
    print("blit kinds: %s" % dict(kinds))

    cells = []
    for h in hits:
        off = h["dest"] - VRAM
        cells.append((off // STRIDE - base_row, (off % STRIDE) // 2, h["kind"]))
    tops = sorted({r for r, c, k in cells})
    print("distinct rows touched: %s" % tops)
    print("row spacings: %s" % [b - a for a, b in zip(tops, tops[1:])])

    cols = sorted({c for r, c, k in cells})
    print("distinct cols touched: %s" % cols)
    print("col spacings: %s" % sorted({b - a for a, b in zip(cols, cols[1:])}))

    # Cursor deltas tell us bytes per cell.
    deltas = [b["cursor"] - a["cursor"] for a, b in zip(hits, hits[1:])]
    print("cursor deltas histogram: %s" % dict(collections.Counter(deltas)))


if __name__ == "__main__":
    main()
