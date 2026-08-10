#!/usr/bin/env python3
"""Resume point for Caimans RE: reach the Pro sample's known IWRAM waypoint
and single-step forward, printing contiguous PC ranges so jumps into new
code stand out. See doc/caimans_handoff.md for what's already established.

Usage: launch `mGBA -g build_caimans/roms/caimans_pro_pooh_hq.gba` first,
then run this with an optional step count (default 60).
"""
import sys
sys.path.insert(0, __import__("os").path.dirname(__file__))
from gdbrsp import RSP

def reach_waypoint(r):
    r.bp(0x03000000)          # ARM trampoline: LDR r0,[pc]; BX r0
    r.cont()
    r.rmbp(0x03000000)
    r.bp(0x08000218, kind=2)  # Thumb entry the trampoline jumps to
    r.cont()

def trace(r, steps):
    pcs = []
    for _ in range(steps):
        pcs.append(r.regs()[15])
        r.send('s')
        r.recv()
    ranges, start, prev = [], pcs[0], pcs[0]
    for pc in pcs[1:]:
        if pc not in (prev, prev + 2, prev + 4):
            ranges.append((start, prev))
            start = pc
        prev = pc
    ranges.append((start, prev))
    return ranges

if __name__ == "__main__":
    steps = int(sys.argv[1]) if len(sys.argv) > 1 else 60
    r = RSP()
    r.cmd('?')
    reach_waypoint(r)
    for a, b in trace(r, steps):
        print(f"{a:#010x} .. {b:#010x}")
