#!/usr/bin/env python3
"""Recover the Caimans Pro audio framing from the running player.

The audio handler is at IWRAM `0x03006630` -- a DMA1/FIFO interrupt routine.
It stops DMA1, picks one of two 64-byte ping-pong buffers, decodes into it and
restarts the transfer:

    decode(src = *(uint8_t **)0x03007080,   /* r0 */
           dst = toggle ? 0x03000070 : 0x0300002c,
           count = 0x40)                    /* 64 samples */
    *(uint8_t **)0x03007080 += 0x20;        /* 32 bytes consumed */

32 bytes in, 64 samples out is exactly 4 bits per sample, and the codec state
at `0x03007050` is never reset between calls -- so the stream is **flat ADPCM
with no block headers at all**.

This script breaks on the decode call, records the source pointer, the codec
state and the decoded output for each invocation, then replays the same bytes
through `caimans_adpcm` and compares the PCM byte for byte.

    python3 tools/gba_video/caimans_trace_audio.py --calls 24
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
AUDIO_CALL = 0x03006690     # the `bl 0x03004a98` inside the handler
CODEC_STATE = 0x03007050    # {int32 predictor, int32 step_index}
SRC_PTR = 0x03007080
TOGGLE = 0x0300708C
BLOCK_BYTES = 0x20
BLOCK_SAMPLES = 0x40
ROM_ADDR_BASE = 0x08000000
TM0CNT = 0x04000100
PICTURE_FN = 0x03005A00     # one hit per picture: a cheap way to advance time


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


def capture(rom_path, port, calls, skip_frames=0):
    """Record every audio call: inputs at the call, outputs right after it.

    `skip_frames` advances playback cheaply first, by breaking once per
    picture instead of once per 64 audio samples -- the opening of this
    trailer is near-silent, and silence exercises none of the kernel's
    interesting paths.
    """
    proc, rsp = launch(rom_path, port)
    out = []
    try:
        if skip_frames:
            rsp.bp(PICTURE_FN)
            for _ in range(skip_frames):
                rsp.cont()
            rsp.rmbp(PICTURE_FN)
        rsp.bp(AUDIO_CALL)
        rsp.bp(AUDIO_CALL + 4)
        for _ in range(calls):
            rsp.cont()                       # stops at the call
            regs = rsp.regs()
            src, dst, count = regs[0], regs[1], regs[2]
            pred, index = struct.unpack("<ii", rsp.mem(CODEC_STATE, 8))
            payload = rsp.mem(src, BLOCK_BYTES)
            rsp.cont()                       # stops just after it returns
            pcm = rsp.mem(dst, BLOCK_SAMPLES)
            pred_a, index_a = struct.unpack("<ii", rsp.mem(CODEC_STATE, 8))
            out.append({"src": src, "dst": dst, "count": count,
                        "pred_before": pred, "index_before": index,
                        "payload": payload, "pcm": pcm,
                        "pred_after": pred_a, "index_after": index_a})
        timer = struct.unpack("<HH", rsp.mem(TM0CNT, 4))
        return out, timer
    finally:
        proc.kill()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rom", default=ROM_PATH)
    ap.add_argument("--port", type=int, default=2345)
    ap.add_argument("--calls", type=int, default=24)
    ap.add_argument("--skip-frames", type=int, default=0,
                    help="advance this many pictures before capturing audio")
    args = ap.parse_args()

    trace, timer = capture(args.rom, args.port, args.calls, args.skip_frames)

    print("%d audio calls" % len(trace))
    print("  count argument: %s" % sorted({t["count"] for t in trace}))
    print("  destinations:   %s" % sorted("0x%08x" % t["dst"] for t in {
        (t["dst"],): t for t in trace}.values()))
    srcs = [t["src"] for t in trace]
    deltas = sorted({b - a for a, b in zip(srcs, srcs[1:])})
    print("  source pointer: 0x%08x .. 0x%08x, deltas %s"
          % (srcs[0], srcs[-1], deltas))
    print("  first source in ROM file terms: 0x%x" % (srcs[0] - ROM_ADDR_BASE))

    reload_, control = timer
    print("  TM0CNT reload 0x%04x control 0x%04x" % (reload_, control))
    if control & 3 == 0 and reload_:
        rate = 16777216.0 / (0x10000 - reload_)
        print("  implied sample rate: %.1f Hz" % rate)

    # Replay EVERY block through the transcribed kernel and compare both the
    # PCM and the resulting codec state.
    from caimans_adpcm import decode
    import collections
    bad_pcm = bad_state = 0
    active = 0
    mags = collections.Counter()
    for t in trace:
        pcm, pred, index = decode(t["payload"], BLOCK_SAMPLES,
                                  t["pred_before"], t["index_before"])
        if bytes(pcm) != t["pcm"]:
            bad_pcm += 1
            if bad_pcm == 1:
                print("\n  first PCM mismatch at src 0x%08x" % t["src"])
                print("    hardware: %s" % " ".join("%02x" % b for b in t["pcm"][:16]))
                print("    python  : %s" % " ".join("%02x" % b for b in bytes(pcm)[:16]))
        if (pred, index) != (t["pred_after"], t["index_after"]):
            bad_state += 1
        if len(set(t["pcm"])) > 2:
            active += 1
        for b in t["payload"]:
            mags[b & 7] += 1
            mags[(b >> 4) & 7] += 1
    print("\nreplayed %d blocks (32 bytes -> 64 samples each)" % len(trace))
    print("  %d carry real signal (more than 2 distinct PCM values)" % active)
    print("  nibble magnitudes exercised: %s" % dict(sorted(mags.items())))
    if mags[7] == 0:
        print("  WARNING: magnitude 7 never occurred, so the extra")
        print("           `diff += step >> 1` deviation is NOT exercised here")
    print("  PCM mismatches:   %d" % bad_pcm)
    print("  state mismatches: %d" % bad_state)
    print("\n%s" % ("AUDIO KERNEL VALIDATED" if not bad_pcm and not bad_state
                    else "MISMATCH"))
    return 0 if not bad_pcm and not bad_state else 1

def _unused(trace):
    last = trace[-1]
    pcm, pred, index = decode(last["payload"], BLOCK_SAMPLES,
                              last["pred_before"], last["index_before"])
    print("\nreplay of the final block (32 bytes -> 64 samples):")
    print("  state in            : predictor %6d  index %2d"
          % (last["pred_before"], last["index_before"]))
    print("  state out hardware  : predictor %6d  index %2d"
          % (last["pred_after"], last["index_after"]))
    print("  state out python    : predictor %6d  index %2d" % (pred, index))
    same_state = (pred, index) == (last["pred_after"], last["index_after"])
    same_pcm = bytes(pcm) == last["pcm"]
    print("  PCM: %s" % ("IDENTICAL" if same_pcm
                         else "%d of %d bytes differ" % (
                             sum(1 for a, b in zip(pcm, last["pcm"]) if a != b),
                             BLOCK_SAMPLES)))
    if not same_pcm:
        print("    hardware: %s" % " ".join("%02x" % b for b in last["pcm"][:16]))
        print("    python  : %s" % " ".join("%02x" % b for b in bytes(pcm)[:16]))
    print("\n%s" % ("AUDIO KERNEL VALIDATED" if same_pcm and same_state else "MISMATCH"))
    return 0 if (same_pcm and same_state) else 1


if __name__ == "__main__":
    sys.exit(main() or 0)
