#!/usr/bin/env python3
"""Record the real player's exact sequence of bitstream reads for one macroblock.

Runs to a chosen macroblock via a breakpoint on `FUN_03004cd4`, then swaps in
breakpoints on the three bit-reader entry points and logs `(reader, n,
bitpos)` for every read until the next macroblock starts. That gives the
ground-truth syntax sequence to compare against `caimans_blocks`.

All of it happens on one GDB connection, because mGBA's stub resets the core
when a new client attaches.

    python3 tools/gba_video/caimans_trace_reads.py --macroblock 48
"""
import argparse
import os
import struct
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from gdbrsp import RSP
from caimans_codebooks import ROM_PATH

MGBA = "/Applications/mGBA.app/Contents/MacOS/mGBA"
BLOCK_FN = 0x03004CD4
BITSTATE = 0x03000024

READERS = {
    0x03003EC4: "get_bits",     # n in r0, the wide path
    0x03003E3C: "get_bits_hw",  # n in r0, the halfword path
    0x03003F68: "get_bit",      # no argument
}


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


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rom", default=ROM_PATH)
    ap.add_argument("--port", type=int, default=2345)
    ap.add_argument("--macroblock", type=int, default=48)
    ap.add_argument("--max-reads", type=int, default=400)
    args = ap.parse_args()

    proc, rsp = launch(args.rom, args.port)
    try:
        rsp.bp(BLOCK_FN)
        for i in range(args.macroblock + 1):
            rsp.cont()
        base, start_bitpos = struct.unpack("<II", rsp.mem(BITSTATE, 8))
        print("macroblock %d starts at bitpos %d (base 0x%08x)"
              % (args.macroblock, start_bitpos, base))

        for addr in READERS:
            rsp.bp(addr)
        log = []
        while len(log) < args.max_reads:
            rsp.cont()
            regs = rsp.regs()
            pc = regs[15]
            pos = struct.unpack("<II", rsp.mem(BITSTATE, 8))[1]
            if pc == BLOCK_FN:
                print("next macroblock reached at bitpos %d" % pos)
                break
            name = READERS.get(pc & ~1)
            if name is None:
                continue
            log.append((name, regs[0] if name != "get_bit" else 1, pos))
    finally:
        proc.kill()

    print("\n%d reads, %d bits total" % (len(log), sum(n for _, n, _ in log)))
    print(" idx  reader      n   bitpos")
    for i, (name, n, pos) in enumerate(log):
        print(" %3d  %-10s %3d  %6d" % (i, name, n, pos))


if __name__ == "__main__":
    main()
