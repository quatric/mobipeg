#!/usr/bin/env python3
"""Decode an extracted GBA VX++ ``.audio`` stream to mono 16-bit PCM WAV.

The first 3124 bytes are the trained LPC codebooks.  What follows is a raw
sequence of variable-size AFrames, with no packet lengths: the packing mode in
the second header word selects 8, 5, 4 or 3 following 16-bit pulse words.  Each
AFrame reconstructs 128 samples at the container's 16384 Hz sample rate.

    ./vx_audio_decode.py build_gbavx/stream02.audio -o stream02.wav
"""

import argparse
import os
import re
import struct
import sys
import wave

AFRAME_SAMPLES = 128
EXTRADATA_SIZE = 3 * 64 * 8 * 2 + 8 * 2 + 8 * 4 + 4
PULSE_DATA_LEN = (8, 5, 4, 3)
PULSE_DISTANCE = (3, 3, 4, 5)


def clip16(value):
    return -32768 if value < -32768 else 32767 if value > 32767 else value


def s32(value):
    """ARM/C signed 32-bit wrap used by the fixed-point decoder state."""
    value &= 0xffffffff
    return value - 0x100000000 if value & 0x80000000 else value


class ExtraData:
    def __init__(self, data):
        if len(data) < EXTRADATA_SIZE:
            raise ValueError("audio codebook is truncated")
        pos = 0
        self.lpc_codebooks = []
        for _ in range(3):
            stage = []
            for _ in range(64):
                stage.append(struct.unpack_from("<8h", data, pos))
                pos += 16
            self.lpc_codebooks.append(stage)
        self.scale_modifiers = struct.unpack_from("<8H", data, pos)
        pos += 16
        self.lpc_base = struct.unpack_from("<8i", data, pos)
        pos += 32
        self.scale_initial, = struct.unpack_from("<I", data, pos)


class State:
    def __init__(self, extradata):
        self.ed = extradata
        self.have_prev = False
        self.have_prev2 = False
        self.pulses_prev = [0] * AFRAME_SAMPLES
        self.pulses_prev2 = [0] * AFRAME_SAMPLES
        self.samples_prev = [0] * 8
        self.scale_prev = 0
        self.lpc_filter_prev = [0] * 8
        self.influence_prev = [0] * 8


def aframe_size(data, pos):
    """Return an AFrame's byte size from its second little-endian word."""
    if pos + 4 > len(data):
        return None
    word2, = struct.unpack_from("<H", data, pos + 2)
    return 4 + 2 * PULSE_DATA_LEN[(word2 >> 12) & 3]


def unpack_pulse_values(mode, pulse_data):
    values = []
    if mode == 0:
        for word in pulse_data:
            for shift in range(13, -1, -3):
                values.append((word >> shift) & 7)
        values += [
            ((pulse_data[0] & 1) << 2) |
            ((pulse_data[1] & 1) << 1) | (pulse_data[2] & 1),
            ((pulse_data[3] & 1) << 2) |
            ((pulse_data[4] & 1) << 1) | (pulse_data[5] & 1),
        ]
        return [v * 2 - 7 for v in values]
    for word in pulse_data:
        for shift in range(14, -1, -2):
            values.append((word >> shift) & 3)
    return [v * 2 - 3 for v in values]


def decode_aframe(state, data, pos):
    """Decode one AFrame, returning ``(samples, next_byte, is_intra)``."""
    size = aframe_size(data, pos)
    if size is None or pos + size > len(data):
        raise ValueError("truncated AFrame at byte %d" % pos)
    word1, word2 = struct.unpack_from("<HH", data, pos)
    prev_frame_offset = (word1 >> 9) & 0x7f
    scale_modifier_index = (word1 >> 6) & 7
    pulse_start = (word2 >> 14) & 3
    pulse_mode = (word2 >> 12) & 3
    lpc_idx = (word1 & 0x3f, (word2 >> 6) & 0x3f, word2 & 0x3f)
    nwords = PULSE_DATA_LEN[pulse_mode]
    pulse_data = struct.unpack_from("<%dH" % nwords, data, pos + 4)
    pulse_values = unpack_pulse_values(pulse_mode, pulse_data)

    if prev_frame_offset != 0x7f and not state.have_prev:
        raise ValueError("inter AFrame without state at byte %d" % pos)

    scale = state.scale_prev if state.have_prev else state.ed.scale_initial
    if prev_frame_offset == 0x7f:
        scale = state.ed.scale_initial
    scale = s32(scale * state.ed.scale_modifiers[scale_modifier_index]) >> 13

    lpc_filter = list(state.ed.lpc_base if prev_frame_offset == 0x7f
                      else state.lpc_filter_prev)
    for k in range(8):
        lpc_filter[k] = s32(lpc_filter[k] + sum(
            state.ed.lpc_codebooks[stage][lpc_idx[stage]][k]
            for stage in range(3)))

    if prev_frame_offset < 0x7e:
        concat = ((state.pulses_prev2 if state.have_prev2 else [0] * 128) +
                  (state.pulses_prev if state.have_prev else [0] * 128))
        pulses = []
        for i in range(AFRAME_SAMPLES):
            volume = min(8, i + 1, AFRAME_SAMPLES - i)
            pulses.append(s32(concat[i + 0x7f - prev_frame_offset] * volume) >> 4)
    else:
        pulses = [0] * AFRAME_SAMPLES

    distance = PULSE_DISTANCE[pulse_mode]
    for i in range(pulse_start, AFRAME_SAMPLES, distance):
        index = (i - pulse_start) // distance
        if index < len(pulse_values):
            pulses[i] = s32(pulses[i] + pulse_values[index] * scale)

    psi = []
    for coeff in lpc_filter:
        old = psi[:]
        psi = [s32(old[j] + ((old[len(old) - j - 1] * coeff) >> 15))
               for j in range(len(old))]
        psi.append(coeff)
    influence = [s32(-(value >> 1)) for value in psi]

    if prev_frame_offset == 0x7f:
        quarters = [influence[:] for _ in range(4)]
    else:
        q3 = influence[:]
        q1 = [s32(a + b) >> 1 for a, b in zip(state.influence_prev, q3)]
        q0 = [s32(a + b) >> 1 for a, b in zip(state.influence_prev, q1)]
        q2 = [s32(a + b) >> 1 for a, b in zip(q1, q3)]
        quarters = [q0, q1, q2, q3]

    samples = []
    for i in range(AFRAME_SAMPLES):
        inf = quarters[i // 32]
        sample = pulses[i] * 0x4000
        for j in range(8):
            source = i - 1 - j
            previous = state.samples_prev[8 + source] if source < 0 else samples[source]
            sample += previous * inf[j]
        samples.append(s32(sample >> 14))

    state.pulses_prev2 = state.pulses_prev
    state.have_prev2 = state.have_prev
    state.pulses_prev = pulses
    state.samples_prev = samples[-8:]
    state.scale_prev = scale
    state.lpc_filter_prev = lpc_filter
    state.influence_prev = influence
    state.have_prev = True
    return [clip16(v) for v in samples], pos + size, prev_frame_offset == 0x7f


def sample_rate_from_header(path, default=16384):
    if not path or not os.path.exists(path):
        return default
    for line in open(path):
        match = re.match(r"sample rate\s+(\d+)", line)
        if match:
            return int(match.group(1))
    return default


def decode_file(path, output, sample_rate=16384, limit=None):
    data = open(path, "rb").read()
    state = State(ExtraData(data[:EXTRADATA_SIZE]))
    pos, count, intra = EXTRADATA_SIZE, 0, 0
    with wave.open(output, "wb") as wav:
        wav.setnchannels(1)
        wav.setsampwidth(2)
        wav.setframerate(sample_rate)
        while pos + 4 <= len(data) and (limit is None or count < limit):
            size = aframe_size(data, pos)
            if size is None or pos + size > len(data):
                break
            samples, pos, is_intra = decode_aframe(state, data, pos)
            wav.writeframesraw(struct.pack("<128h", *samples))
            count += 1
            intra += is_intra
    tail = data[pos:] if limit is None else b""
    if any(tail):
        raise ValueError("%d nonzero trailing bytes at byte %d" % (len(tail), pos))
    return count, intra, len(tail)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("audio", help="extracted .audio file")
    ap.add_argument("-o", "--output", required=True, help="output mono WAV")
    ap.add_argument("--header", help="extracted .hdr.txt (default: beside audio)")
    ap.add_argument("--sample-rate", type=int)
    ap.add_argument("--limit", type=int, help="decode only this many AFrames")
    args = ap.parse_args()
    header = args.header or os.path.splitext(args.audio)[0] + ".hdr.txt"
    rate = args.sample_rate or sample_rate_from_header(header)
    count, intra, tail = decode_file(args.audio, args.output, rate, args.limit)
    print("decoded %d AFrames, %d intra, %d padding bytes -> %s (%d Hz)" %
          (count, intra, tail, args.output, rate))
    return 0


if __name__ == "__main__":
    sys.exit(main())
