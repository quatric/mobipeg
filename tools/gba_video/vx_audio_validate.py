#!/usr/bin/env python3
"""Validate raw VX++ audio framing against every container seek offset.

Each seek point names an exact byte offset and begins with an intra AFrame, so
walking variable-size frames from one entry to the next is an end-to-end check
of the GBA audio framing.  With no arguments, validate every extracted stream
under ``build_gbavx`` (or ``$VXPP_DATA``).
"""

import argparse
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import vx_audio_decode as A


def load_seek(path):
    entries = []
    active = False
    for line in open(path):
        if line.startswith("seek table"):
            active = True
            continue
        if active:
            match = re.match(r"\s*(\d+)\s+(\d+)\s+(\d+)", line)
            if match:
                entries.append(tuple(map(int, match.groups())))
    return entries


def validate_stream(audio_path, header_path):
    data = open(audio_path, "rb").read()
    starts = [(frame, audio) for frame, _, audio in load_seek(header_path)
              if audio]
    failures = []
    total_frames = 0
    for index, (video_frame, start) in enumerate(starts):
        end = starts[index + 1][1] if index + 1 < len(starts) else len(data)
        pos, aframes, intras = start, 0, []
        while pos + 4 <= end:
            size = A.aframe_size(data, pos)
            if size is None or pos + size > end:
                break
            word1, = struct.unpack_from("<H", data, pos)
            if ((word1 >> 9) & 0x7f) == 0x7f:
                intras.append(aframes)
            pos += size
            aframes += 1
        padding = data[pos:end]
        if (pos != end and any(padding)) or intras != [0]:
            failures.append((video_frame, start, end, pos, intras, len(padding)))
        total_frames += aframes
    return len(starts) - len(failures), failures, total_frames


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("directory", nargs="?", default=os.environ.get(
        "VXPP_DATA", os.path.join(os.path.dirname(os.path.dirname(
            os.path.dirname(os.path.abspath(__file__)))), "build_gbavx")))
    args = ap.parse_args()

    total_ok = total = 0
    for index in range(100):
        base = os.path.join(args.directory, "stream%02d" % index)
        audio, header = base + ".audio", base + ".hdr.txt"
        if not os.path.exists(audio):
            break
        ok, failures, aframes = validate_stream(audio, header)
        n = ok + len(failures)
        total_ok += ok
        total += n
        print("stream%d: %d AFrames -> %d/%d seek segments exact" %
              (index, aframes, ok, n))
        for failure in failures[:5]:
            print("    frame %d: start=%d end=%d stopped=%d intra=%r tail=%d" %
                  failure)
    print("\n%d/%d audio segments exact" % (total_ok, total))
    return 0 if total and total_ok == total else 1


if __name__ == "__main__":
    sys.exit(main())
