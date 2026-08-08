#!/usr/bin/env python3
"""Validate native FFmpeg GBA VX++ video packetization against the ROM.

Each native packet must cover exactly one container seek interval and carry a
GVX1 prefix with the interval's leading bit skip, available bit count and frame
count.  The payload must also equal the original video bytes after the GBA
decoder's little-endian-halfword byte swap.

    ./vx_native_packet_validate.py <rom.gba>
    ./vx_native_packet_validate.py <rom.gba> --ffmpeg ../../ffmpeg
"""

import argparse
import os
import struct
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from gbavx_extract import HEADER_SIZE, find_streams


PACKET_MAGIC = 0x31585647  # MKTAG('G', 'V', 'X', '1')
PACKET_HEADER_SIZE = 16


def swapped_halfwords(data):
    out = bytearray(data)
    for i in range(0, len(out) - 1, 2):
        out[i], out[i + 1] = out[i + 1], out[i]
    return bytes(out)


def validate_stream(rom, stream, data):
    real = [entry for entry in stream.seek if entry[1] or entry[0] == 0]
    video_bits = (stream.audio_off - HEADER_SIZE) * 8
    pos = 0

    for i, (frame, bit, _, _) in enumerate(real):
        if pos + PACKET_HEADER_SIZE > len(data):
            raise ValueError("packet %d header is truncated" % i)
        magic, skip, valid, frames = struct.unpack_from("<4I", data, pos)
        end_bit = real[i + 1][1] if i + 1 < len(real) else video_bits
        end_frame = (real[i + 1][0]
                     if i + 1 < len(real) else stream.nb_frames)
        expected = (PACKET_MAGIC, bit % 16,
                    end_bit - (bit & ~15), end_frame - frame)
        if (magic, skip, valid, frames) != expected:
            raise ValueError("packet %d prefix %r != %r" %
                             (i, (magic, skip, valid, frames), expected))

        payload_size = ((valid + 15) // 16) * 2
        payload = data[pos + PACKET_HEADER_SIZE:
                       pos + PACKET_HEADER_SIZE + payload_size]
        source = stream.off + HEADER_SIZE + (bit & ~15) // 8
        expected_payload = swapped_halfwords(rom[source:source + payload_size])
        if payload != expected_payload:
            raise ValueError("packet %d payload differs from ROM" % i)
        pos += PACKET_HEADER_SIZE + payload_size

    if pos != len(data):
        raise ValueError("%d trailing packet bytes" % (len(data) - pos))
    return len(real)


def main():
    repo = os.path.dirname(os.path.dirname(os.path.dirname(
        os.path.abspath(__file__))))
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("rom")
    ap.add_argument("--ffmpeg", default=os.path.join(repo, "ffmpeg"))
    args = ap.parse_args()

    with open(args.rom, "rb") as f:
        rom = f.read()
    streams = find_streams(rom)
    if not streams:
        print("no VX++ streams found", file=sys.stderr)
        return 1

    total = 0
    with tempfile.TemporaryDirectory(prefix="gbavx-packets-") as tmp:
        for i, stream in enumerate(streams):
            path = os.path.join(tmp, "stream%02d.gvx" % i)
            command = [args.ffmpeg, "-hide_banner", "-loglevel", "error",
                       "-y", "-resource", str(i), "-i", args.rom,
                       "-map", "0:v:0", "-c", "copy", "-f", "data", path]
            try:
                subprocess.run(command, check=True)
                with open(path, "rb") as f:
                    data = f.read()
                count = validate_stream(rom, stream, data)
            except (OSError, subprocess.CalledProcessError, ValueError) as e:
                print("stream%d: %s" % (i, e), file=sys.stderr)
                return 1
            total += count
            print("stream%d: %d/%d native video packets exact" %
                  (i, count, count))

    print("\n%d/%d native video segment packets exact" % (total, total))
    return 0


if __name__ == "__main__":
    sys.exit(main())
