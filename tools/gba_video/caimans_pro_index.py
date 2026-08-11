#!/usr/bin/env python3
"""Capture the authoritative Caimans Pro picture index from the player.

The Pro container has no per-picture length field that has been recovered
statically.  Its ARM picture routine, however, is called once per picture with
the ROM address in r1 and a monotonically increasing picture number in r3.
This tool records those calls from mGBA and derives each packet extent from
the next call.  It deliberately stops when either value repeats, which makes
it safe to run through the player's looping playback.

The resulting JSON is an RE artifact, not an input format: a native demuxer
must only be written after the equivalent table/parser is recovered from the
ROM code.
"""
import argparse
import hashlib
import json
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from caimans_codebooks import ROM_PATH
from gdbrsp import RSP

MGBA = "/Applications/mGBA.app/Contents/MacOS/mGBA"
PICTURE_FN = 0x03005A00
ROM_BASE = 0x08000000


def launch(rom, port):
    proc = subprocess.Popen([MGBA, "-g", rom], stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL)
    for _ in range(100):
        time.sleep(0.1)
        try:
            return proc, RSP(port=port)
        except OSError:
            pass
    proc.kill()
    raise RuntimeError("could not connect to mGBA on port %d" % port)


def capture(rom, port=2345, limit=2000, skip_calls=0):
    """Return call records through the first loop or *limit* calls."""
    proc, rsp = launch(rom, port)
    records = []
    seen = set()
    try:
        rsp.bp(PICTURE_FN)
        for _ in range(skip_calls):
            rsp.cont()
        for _ in range(limit):
            rsp.cont()
            regs = rsp.regs()
            offset = regs[1] - ROM_BASE
            number = regs[3]
            key = (number, offset)
            if key in seen:
                break
            if not (0 <= offset < os.path.getsize(rom)):
                raise RuntimeError("picture %d has non-ROM pointer 0x%08x" %
                                   (number, regs[1]))
            seen.add(key)
            records.append({"number": number, "offset": offset})
    finally:
        if proc.poll() is None:
            proc.kill()
    if len(records) < 2:
        raise RuntimeError("captured fewer than two picture calls")
    return records


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rom", default=ROM_PATH)
    ap.add_argument("--port", type=int, default=2345)
    ap.add_argument("--limit", type=int, default=2000)
    ap.add_argument("--skip-calls", type=int, default=0,
                    help="advance this many picture calls before recording")
    ap.add_argument("--out", help="write complete capture JSON")
    args = ap.parse_args()

    records = capture(args.rom, args.port, args.limit, args.skip_calls)
    rom_size = os.path.getsize(args.rom)
    result = {
        "rom_sha256": hashlib.sha256(open(args.rom, "rb").read()).hexdigest(),
        "picture_count": len(records),
        "skipped_calls": args.skip_calls,
        "first_offset": records[0]["offset"],
        "last_offset": records[-1]["offset"],
        "records": records,
    }
    print("%d pictures: ROM 0x%x .. 0x%x (ROM size 0x%x)" %
          (len(records), result["first_offset"], result["last_offset"], rom_size))
    print("first: %s" % records[:3])
    print("last:  %s" % records[-3:])
    if args.out:
        with open(args.out, "w") as f:
            json.dump(result, f, indent=2)
            f.write("\n")
        print("wrote %s" % args.out)


if __name__ == "__main__":
    main()
