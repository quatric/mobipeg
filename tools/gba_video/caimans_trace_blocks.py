#!/usr/bin/env python3
"""Differential trace: real player vs `caimans_blocks`, per macroblock.

Breaks on `FUN_03004cd4` in mGBA and records the bit-reader state
(`0x03000024` = {base pointer, bit position}) plus the call's arguments at
every macroblock, then replays the same picture through the Python decoder and
reports the **first macroblock whose bit position disagrees**. That localises
the desync (see "Open problem" in doc/caimans_handoff.md) to the leaves of one
macroblock rather than leaving it to guesswork.

Everything happens inside a single unbroken GDB connection -- mGBA's stub
resets the core whenever a new client connects, so a second script would start
over from power-on rather than resume.

    python3 tools/gba_video/caimans_trace_blocks.py --hits 300
"""
import argparse
import os
import struct
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from gdbrsp import RSP
from caimans_blocks import BitReader, BlockDecoder, Desync
from caimans_codebooks import Images, ROM_PATH
from caimans_frame import DATA_BASE, parse_header, read_index

MGBA = "/Applications/mGBA.app/Contents/MacOS/mGBA"
BLOCK_FN = 0x03004CD4
BITSTATE = 0x03000024
ROM_ADDR_BASE = 0x08000000


def launch(rom, port):
    proc = subprocess.Popen([MGBA, "-g", rom],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for _ in range(40):
        time.sleep(0.5)
        try:
            r = RSP(port=port)
            return proc, r
        except OSError:
            continue
    proc.kill()
    raise SystemExit("could not connect to mGBA's stub on port %d" % port)


def collect(rsp, hits):
    """[(bitbase, bitpos, args...)] at each entry to the block routine."""
    rsp.bp(BLOCK_FN)
    out = []
    while len(out) < hits:
        rsp.cont()
        regs = rsp.regs()
        base, bitpos = struct.unpack("<II", rsp.mem(BITSTATE, 8))
        out.append({"base": base, "bitpos": bitpos, "dst": regs[0],
                    "stride": regs[1], "intra": regs[2], "x": regs[3]})
    return out


def replay(rom, off, count):
    """The Python decoder's bit position at the start of each macroblock."""
    img = Images()
    dec = BlockDecoder(img)
    data = rom[off:off + 0x4E20]
    br = BitReader(data)
    pic = parse_header(br, img)
    buf = bytearray(pic.buffer_size())
    marks = []
    for offset, w, h in pic.planes():
        for y in range(0, h, 16):
            for x in range(0, w, 16):
                marks.append({"bitpos": br.pos, "stride": w, "x": x})
                if len(marks) > count:
                    return pic, marks
                try:
                    dec.decode_macroblock(br, buf, offset + y * w + x, w, intra=True)
                except Desync as e:
                    marks.append({"bitpos": br.pos, "stride": w, "x": x, "desync": str(e)})
                    return pic, marks
    return pic, marks


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rom", default=ROM_PATH)
    ap.add_argument("--port", type=int, default=2345)
    ap.add_argument("--hits", type=int, default=300)
    args = ap.parse_args()

    with open(args.rom, "rb") as f:
        rom = f.read()

    proc, rsp = launch(args.rom, args.port)
    try:
        trace = collect(rsp, args.hits)
    finally:
        proc.kill()

    bases = {}
    for t in trace:
        bases.setdefault(t["base"], []).append(t)
    print("bit-reader bases seen: %s" % ", ".join(
        "0x%08x (%d hits)" % (b, len(v)) for b, v in bases.items()))

    base = trace[0]["base"]
    off = base - ROM_ADDR_BASE
    print("first picture at ROM 0x%x; index table says 0x%x"
          % (off, DATA_BASE + read_index(rom)[0]))

    hw = [t for t in trace if t["base"] == base]
    pic, marks = replay(rom, off, len(hw))
    print("picture %dx%d, %d hardware macroblocks, %d replayed"
          % (pic.width, pic.height, len(hw), len(marks)))

    for i, (a, b) in enumerate(zip(hw, marks)):
        if a["bitpos"] != b["bitpos"]:
            print("\nFIRST DISAGREEMENT at macroblock %d" % i)
            print("  hardware bitpos %d, python bitpos %d (delta %+d)"
                  % (a["bitpos"], b["bitpos"], b["bitpos"] - a["bitpos"]))
            print("  hardware stride %d x %d | python stride %d x %d"
                  % (a["stride"], a["x"], b["stride"], b["x"]))
            lo = max(0, i - 3)
            print("  preceding macroblocks (hw vs py):")
            for j in range(lo, i + 1):
                print("    %3d  hw %6d  py %6d" % (j, hw[j]["bitpos"], marks[j]["bitpos"]))
            break
    else:
        print("\nno disagreement over %d macroblocks" % min(len(hw), len(marks)))
        if any("desync" in m for m in marks):
            print("python hit: %s" % [m["desync"] for m in marks if "desync" in m][0])


if __name__ == "__main__":
    main()
