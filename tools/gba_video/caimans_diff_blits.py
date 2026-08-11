#!/usr/bin/env python3
"""Diff `decode_raster_3100`'s claimed blit order against hardware's actual order.

This is the diagnostic the 2.2 video residual needs (see "the 0x3100
skip-capable raster pass" in doc/caimans_handoff.md): the 9.21%-of-VRAM
delta is unlocalised, and the prime suspect is the row/col arithmetic in
`decode_raster_3100`, which is still decompile-derived and was never
live-cross-checked (unlike the 0x3100 skip semantics, justified by the
33760->7073 byte drop).

It captures **two sequences of (kind, row, col) tuples** for the same delta
record and diffs them:

  - hardware: set execution breakpoints at the two blit entry points
    (`0x03000114` simple, `0x0300015c` complex), let the core run, and on
    each hit read `pc` (which routine) and `r0` (dest); convert dest to a
    grid position with `row = (dest-VRAM)//stride`, `col = ((dest-VRAM)%stride)//2`.
    This is hardware's *actual* screen position -- independent of any
    decompile reading (same technique that produced the 24-coordinate list
    in the handoff doc).
  - mine: run `decode_raster_3100` in Python with `blit_simple`/`blit_complex`
    monkeypatched to log `(kind, row, col)`, recovered from the same
    dest/stride math, so a tuple match means the decoder's row/col
    arithmetic is right at that cell.

Then find the first tuple where the two sequences diverge. A divergence in
*coordinates* implicates the decoder's row/col arithmetic; a divergence in
*kind at the same coordinate* implicates mask reading (skip/simple/complex
bit) or the `FUN_08001a7c` complex-rows-2/3 assumption.

Reuses the proven `caimans_trace_video22.py` harness verbatim for reaching
record N and reading the header fields (dest from `regs[3]` at the
`FUN_08000418` entry, not from the stack), so the comparison is fair.

    python3 tools/gba_video/caimans_diff_blits.py --record 1 --n 500
"""
import argparse
import os
import struct
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from gdbrsp import RSP
import caimans22_frame as C

MGBA = "/Applications/mGBA.app/Contents/MacOS/mGBA"
FUN_08000418 = 0x08000418
HEADER_DONE = 0x080004d6   # right before the raster loop starts
BLIT_SIMPLE = 0x03000114    # IWRAM, Thumb (set |1 for the bp)
BLIT_COMPLEX = 0x0300015c   # IWRAM, Thumb
VRAM = 0x06000000
STRIDE = 480               # 240 px * 2 bytes
ROM_PATH = "/Volumes/SSD/dlz/Folders/mobipeg/build_caimans/roms/caimans_badboys2.gba"


def launch(rom, port):
    proc = subprocess.Popen([MGBA, "-g", rom],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for _ in range(40):
        time.sleep(0.5)
        try:
            return proc, RSP(port=port)
        except OSError:
            continue
    proc.kill()
    raise SystemExit("could not connect to mGBA on port %d" % port)


def grid(dest):
    """Absolute VRAM dest -> (row, col) in 4x4-cell units (px coords /1).

    Same conversion the 24-call capture used: row = off//stride, col =
    (off%stride)//2, where off = dest - VRAM_BASE.
    """
    off = dest - VRAM
    return off // STRIDE, (off % STRIDE) // 2


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rom", default=ROM_PATH)
    ap.add_argument("--port", type=int, default=2345)
    ap.add_argument("--record", type=int, default=1,
                    help="which FUN_08000418 call (0 = keyframe, 1 = first delta)")
    ap.add_argument("--n", type=int, default=500,
                    help="how many hardware blit hits to capture")
    args = ap.parse_args()

    proc, rsp = launch(args.rom, args.port)
    try:
        # --- reach record N (proven harness from caimans_trace_video22.py) ---
        rsp.bp(FUN_08000418 | 1)
        for _ in range(args.record + 1):
            rsp.cont()
        regs = rsp.regs()
        dest = regs[3]                       # <-- from r3 at ENTRY, not [sp+0x40]
        dest_off = dest - VRAM
        print("record %d: dest=0x%x (off=0x%x)" % (args.record, dest, dest_off))

        # --- step to the header-done point, read the raster-loop inputs ---
        rsp.rmbp(FUN_08000418 | 1)
        rsp.bp(HEADER_DONE | 1)
        rsp.cont()
        sp = rsp.regs()[13]
        stack = rsp.mem(sp, 0x48)
        remaining = struct.unpack_from("<i", stack, 0x14)[0]
        height = struct.unpack_from("<i", stack, 0x20)[0]
        simple_base = struct.unpack_from("<I", stack, 0x38)[0]
        complex_base = struct.unpack_from("<I", stack, 0x3C)[0]
        cursor = struct.unpack("<I", rsp.mem(0x03005028, 4))[0]
        print("header: remaining=%d height=%d cursor=0x%x"
              % (remaining, height, cursor))
        print("cells: simple=0x%x complex=0x%x" % (simple_base, complex_base))

        bitstream = bytearray(rsp.mem(cursor, remaining + 2000))
        simple_cache = rsp.mem(simple_base, 256 * C.CELL_SIZE)
        complex_cache = rsp.mem(complex_base, 256 * C.CELL_SIZE)

        # --- capture hardware's actual blit order ---
        rsp.rmbp(HEADER_DONE | 1)
        rsp.bp(BLIT_SIMPLE | 1)
        rsp.bp(BLIT_COMPLEX | 1)
        base_row = dest_off // STRIDE         # VRAM row of the record's origin
        hw = []
        for i in range(args.n):
            rsp.cont()
            g = rsp.regs()
            pc = g[15] & ~1
            kind = 'S' if pc == BLIT_SIMPLE else ('C' if pc == BLIT_COMPLEX else '?')
            if kind == '?':
                # landed somewhere unexpected (e.g. record ended / next record)
                print("hw: unexpected pc=0x%x at hit %d -- stopping capture" % (pc, i))
                break
            abs_row, col = grid(g[0])
            hw.append((kind, abs_row - base_row, col))   # <-- record-relative
        print("\nhw: captured %d blit hits, all RECORD-RELATIVE (record base row=%d)"
              % (len(hw), base_row))
        print("hw first 12: %s" % hw[:12])
        print("hw all (row,col) Unique:")
        hw_cells = sorted(set((r, c) for _, r, c in hw))
        print("  unique cells touched by hw: %d  -> %s"
              % (len(hw_cells), hw_cells))

        hw_vram = rsp.mem(VRAM, 240 * 160 * 2)   # also grab the reference frame
    finally:
        proc.kill()

    # --- run mine with logging (already record-relative: l[1] = decode loop's row/col) ---
    log = []
    orig_s, orig_c = C.blit_simple, C.blit_complex

    def ls(buf, off, cache, coff, stride):
        rel = off - dest_off
        log.append(('S', rel // stride, (rel % stride) // 2))
        return orig_s(buf, off, cache, coff, stride)

    def lc(buf, off, ca, oa, cb, ob, stride):
        rel = off - dest_off
        log.append(('C', rel // stride, (rel % stride) // 2))
        return orig_c(buf, off, ca, oa, cb, ob, stride)

    C.blit_simple = ls
    C.blit_complex = lc
    # Seed with the live framebuffer (skipped cells keep previous contents --
    # a requirement for delta records, see caimans_trace_video22.py).
    my_buf = bytearray(hw_vram)
    C.decode_raster_3100(bitstream, 0, remaining,
                         simple_cache, 0, complex_cache, 0,
                         240, height, STRIDE, my_buf, dest_off)
    C.blit_simple, C.blit_complex = orig_s, orig_c
    print("\nmine: %d blit calls (first 12: %s)" % (len(log), log[:12]))
    mine_cells = sorted(set((r, c) for _, r, c in log))
    print("mine unique cells touched: %d" % len(mine_cells))

    # sanity: re-diff VRAM to confirm this run reproduces the ~9% delta
    diff = sum(1 for x, y in zip(my_buf, hw_vram) if x != y)
    print("\nVRAM sanity: %d/%d differing (%.2f%%) "
          "(transcript rec1b was 7073/9.21%% -- same ballpark, not identical)"
          % (diff, len(hw_vram), 100.0 * diff / len(hw_vram)))

    # --- the decisive question: is hw's cell set a SUBSET of mine's, or disjoint? ---
    hw_set = set((r, c) for _, r, c in hw)
    mine_set = set((r, c) for _, r, c in log)
    overlap = hw_set & mine_set
    hw_only = hw_set - mine_set
    mine_only = mine_set - hw_set
    print("\n=== SET OVERLAP (the decisive test) ===")
    print("hw unique cells:    %3d" % len(hw_set))
    print("mine unique cells:  %3d" % len(mine_set))
    print("overlap (both):     %3d" % len(overlap))
    print("hw-only (mine skips these orCoords differ):     %3d -> %s"
          % (len(hw_only), sorted(hw_only)[:20]))
    print("mine-only (mine writes these, hw doesn't):      %3d -> %s"
          % (len(mine_only), sorted(mine_only)[:20]))
    if hw_only:
        print("\nhw-only min/max row: %d..%d   mine-only min/max row: %d..%d"
              % (min(r for r, _ in hw_only), max(r for r, _ in hw_only),
                 min(r for r, _ in mine_only), max(r for r, _ in mine_only)))

    # --- sequence-order comparison (secondary: even on shared cells, does ORDER match?) ---
    n = min(len(hw), len(log))
    mism = [i for i in range(n) if hw[i] != log[i]]
    print("\n=== SEQUENCE ORDER (first %d of hw=%d / mine=%d) ===" %
          (n, len(hw), len(log)))
    if not mism:
        print("order matches for the first %d calls." % n)
    else:
        i = mism[0]
        print("FIRST ORDER MISMATCH at index %d:" % i)
        for j in range(max(0, i - 3), min(n, i + 4)):
            mark = "  <<<" if j == i else ""
            print("  [%3d] hw=%s mine=%s%s" % (j, hw[j], log[j], mark))


if __name__ == "__main__":
    main()
