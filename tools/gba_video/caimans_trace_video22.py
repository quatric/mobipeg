#!/usr/bin/env python3
"""Validate caimans22_frame.decode_raster against a live mGBA capture.

Breaks right after FUN_08000418's header parse for a chosen record (the
address 0x080004d6 is the instruction right before the tag-0x3000 raster
loop begins), reads out the already-built codebook tables/cache and the raw
bitstream bytes, decodes them in pure Python, then lets hardware finish the
same record and compares the resulting VRAM.

    python3 tools/gba_video/caimans_trace_video22.py --record 1
"""
import argparse
import os
import struct
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from gdbrsp import RSP
from caimans22_frame import (CELL_SIZE, ROM_PATH, build_color_tables,
                             decode_codebook_chunk, decode_raster_3100)

MGBA = "/Applications/mGBA.app/Contents/MacOS/mGBA"
FUN_08000418 = 0x08000418
HEADER_DONE = 0x080004d6   # right before the tag-0x3000 raster loop starts
CURSOR_VAR = 0x03005028    # the canonical bitstream-position pointer variable
TABLE_A_PTR = 0x03005030
TABLE_B_PTR = 0x03005034
DISPCNT = 0x04000000
BLIT_SIMPLE = 0x03000114
BLIT_COMPLEX = 0x0300015C
VRAM = 0x06000000
VRAM_SIZE = 240 * 160 * 2


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
    ap.add_argument("--record", type=int, default=1,
                    help="which call to FUN_08000418 to decode (0 = keyframe)")
    ap.add_argument("--dump", default=None, help="write both PPMs here (prefix)")
    args = ap.parse_args()

    proc, rsp = launch(args.rom, args.port)
    try:
        rsp.bp(FUN_08000418 | 1)
        for _ in range(args.record + 1):
            rsp.cont()
        regs = rsp.regs()
        state, record_ptr, length, dest = regs[0], regs[1], regs[2], regs[3]
        print("record %d: state=0x%x ptr=0x%x len=%d dest=0x%x"
              % (args.record, state, record_ptr, length, dest))

        rsp.rmbp(FUN_08000418 | 1)
        rsp.bp(HEADER_DONE | 1)
        rsp.cont()

        sp = rsp.regs()[13]
        stack = rsp.mem(sp, 0x48)
        remaining = struct.unpack_from("<i", stack, 0x14)[0]
        height = struct.unpack_from("<i", stack, 0x20)[0]
        # [sp+0x38] = param_1[1] = the SIMPLE cell array
        # [sp+0x3c] = param_1[0] = the COMPLEX cell array
        # (these are genuinely two different arrays; swapping them yields
        #  plausible-looking garbage rather than an obvious failure)
        simple_base = struct.unpack_from("<I", stack, 0x38)[0]
        complex_base = struct.unpack_from("<I", stack, 0x3C)[0]
        width = 240
        stride = width * 2

        cursor = struct.unpack("<I", rsp.mem(CURSOR_VAR, 4))[0]
        table_a_ptr, table_b_ptr = struct.unpack("<II", rsp.mem(TABLE_A_PTR, 8))
        print("header: remaining=%d height=%d cursor=0x%x" % (remaining, height, cursor))
        print("cells: simple=0x%x complex=0x%x" % (simple_base, complex_base))
        print("tables: A=0x%x B=0x%x" % (table_a_ptr, table_b_ptr))

        # `cursor` points at the FIRST sub-chunk of the record, which is a
        # codebook chunk (0x2100/0x2300), not the raster pass. Walk the
        # sub-chunk chain -- each is {uint16 tag, uint16 total_len} followed
        # by total_len-4 data bytes -- to find the raster chunk and its own
        # byte budget. Feeding the raster decoder the record-wide remaining
        # count instead of the chunk's own length makes it run far past the
        # end of its data and invent hundreds of bogus cells.
        raster = None
        walk = cursor
        for _ in range(32):
            tag, ln = struct.unpack("<HH", rsp.mem(walk, 4))
            if ln < 4:
                break
            if tag in (0x3000, 0x3100, 0x3200, 0x8000, 0x8100, 0x8200):
                raster = (tag, walk, ln)
                break
            walk += ln
        if raster is None:
            raise SystemExit("no raster sub-chunk found in record %d" % args.record)
        tag, chunk_at, chunk_len = raster
        data_at = chunk_at + 4
        data_len = chunk_len - 4
        print("raster chunk: tag=0x%04x at 0x%x, %d data bytes" % (tag, chunk_at, data_len))

        bitstream = bytearray(rsp.mem(data_at, data_len))
        table_a, table_b, table_base = build_color_tables()
        hw_table_a = rsp.mem(table_a_ptr - table_base, len(table_a))
        hw_table_b = rsp.mem(table_b_ptr - table_base, len(table_b))
        print("colour-table bytes differing: A=%d B=%d" %
              (sum(x != y for x, y in zip(table_a, hw_table_a)),
               sum(x != y for x, y in zip(table_b, hw_table_b))))

        # Rebuild both persistent caches from their state at the start of the
        # record. This validates the codebook chunks independently of the
        # raster pass instead of trusting hardware's already-built cells.
        simple_cache = bytearray(rsp.mem(simple_base, 256 * CELL_SIZE))
        complex_cache = bytearray(rsp.mem(complex_base, 256 * CELL_SIZE))
        codebook_tags = []
        walk = cursor
        while walk < chunk_at:
            cb_tag, cb_len = struct.unpack("<HH", rsp.mem(walk, 4))
            if cb_len < 4 or walk + cb_len > chunk_at:
                raise SystemExit("invalid sub-chunk before raster at 0x%x" % walk)
            if cb_tag in (0x2000, 0x2100, 0x2200, 0x2300):
                payload = rsp.mem(walk + 4, cb_len - 4)
                decode_codebook_chunk(cb_tag, payload,
                                      simple_cache, complex_cache,
                                      table_a, table_b, table_base)
                codebook_tags.append(cb_tag)
            walk += cb_len
        print("codebooks rebuilt: %s" %
              ", ".join("0x%04x" % x for x in codebook_tags))

        # Sample the cell caches and the VRAM seed at the FIRST blit, and
        # read the final VRAM when execution reaches the next record. Both
        # breakpoints stay installed across the whole raster pass: taking the
        # seed by removing the blit breakpoints and free-running instead gave
        # a subtly different starting framebuffer and a spurious ~0.7%
        # mismatch, so keep this single-loop shape.
        rsp.rmbp(HEADER_DONE | 1)
        rsp.bp(BLIT_SIMPLE | 1)
        rsp.bp(BLIT_COMPLEX | 1)
        rsp.bp(FUN_08000418 | 1)

        my_buf = None
        for _ in range(4096):
            rsp.cont()
            pc = rsp.regs()[15] & ~1
            if pc == FUN_08000418:
                break
            if my_buf is None:
                my_buf = bytearray(rsp.mem(VRAM, VRAM_SIZE))
                hw_simple = rsp.mem(simple_base, 256 * CELL_SIZE)
                hw_complex = rsp.mem(complex_base, 256 * CELL_SIZE)
                simple_diff = sum(x != y for x, y in zip(simple_cache, hw_simple))
                complex_diff = sum(x != y for x, y in zip(complex_cache, hw_complex))
                print("cache bytes differing: simple=%d complex=%d"
                      % (simple_diff, complex_diff))
        hw_vram = rsp.mem(VRAM, VRAM_SIZE)
        if my_buf is None:
            raise SystemExit("record %d performed no blits" % args.record)

        decode_raster_3100(bitstream, 0, data_len,
                           simple_cache, 0, complex_cache, 0,
                           width, height, stride, my_buf, dest - VRAM)
    finally:
        proc.kill()

    diff = sum(1 for x, y in zip(my_buf, hw_vram) if x != y)
    print("\nVRAM bytes compared: %d, differing: %d (%.2f%%)"
          % (VRAM_SIZE, diff, 100.0 * diff / VRAM_SIZE))

    if args.dump:
        for name, buf in (("mine", my_buf), ("hw", hw_vram)):
            with open("%s.%s.ppm" % (args.dump, name), "wb") as f:
                f.write(b"P6\n240 160\n255\n")
                out = bytearray(240 * 160 * 3)
                for i in range(240 * 160):
                    p = struct.unpack_from("<H", buf, i * 2)[0]
                    r = (p & 0x1F) << 3
                    g = ((p >> 5) & 0x1F) << 3
                    b = ((p >> 10) & 0x1F) << 3
                    out[i * 3:i * 3 + 3] = bytes([r, g, b])
                f.write(bytes(out))
        print("wrote %s.mine.ppm and %s.hw.ppm" % (args.dump, args.dump))


if __name__ == "__main__":
    main()
