#!/usr/bin/env python3
"""Extract and decode synchronized Caimans 2.2 audio from the demo ROM."""
import argparse
import os
import sys
import wave

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from caimans_adpcm import decode
from caimans22_decode import ROM_PATH, records

AUDIO_OFFSET = 0x707720
SAMPLE_RATE = 0x2910       # 10512 Hz, literal passed to the Timer-0 divider
VIDEO_RATE = 16
BLOCK_BYTES = 16
BLOCK_SAMPLES = 32


def decode_audio(rom, sample_count=None):
    """Decode flat mono ADPCM, optionally trimming to an exact sample count."""
    encoded = rom[AUDIO_OFFSET:]
    predictor = step_index = 0
    pcm = bytearray()
    for pos in range(0, len(encoded) - BLOCK_BYTES + 1, BLOCK_BYTES):
        block, predictor, step_index = decode(
            encoded[pos:pos + BLOCK_BYTES], BLOCK_SAMPLES,
            predictor, step_index)
        pcm.extend(block)
        if sample_count is not None and len(pcm) >= sample_count:
            del pcm[sample_count:]
            break
    return bytes(pcm)


def synchronized_sample_count(rom):
    """One video record is presented every 1/16 s: exactly 657 samples."""
    frame_count = sum(1 for _ in records(rom))
    assert SAMPLE_RATE % VIDEO_RATE == 0
    return frame_count, frame_count * (SAMPLE_RATE // VIDEO_RATE)


def write_wav(path, signed_pcm):
    # RIFF/WAVE represents 8-bit PCM as unsigned, while the GBA FIFO and
    # decoder output are signed. Bias by 128 without changing bit depth.
    unsigned_pcm = bytes(sample ^ 0x80 for sample in signed_pcm)
    with wave.open(path, "wb") as out:
        out.setnchannels(1)
        out.setsampwidth(1)
        out.setframerate(SAMPLE_RATE)
        out.writeframes(unsigned_pcm)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--rom", default=ROM_PATH)
    parser.add_argument("--output", default="caimans22.wav")
    parser.add_argument("--no-trim", action="store_true",
                        help="decode the entire ROM tail instead of video duration")
    args = parser.parse_args()
    rom = open(args.rom, "rb").read()
    frames, synchronized = synchronized_sample_count(rom)
    requested = None if args.no_trim else synchronized
    pcm = decode_audio(rom, requested)
    write_wav(args.output, pcm)
    print("video: %d frames at %d fps (%.6f s)" %
          (frames, VIDEO_RATE, frames / VIDEO_RATE))
    print("audio: %d samples at %d Hz (%.6f s)" %
          (len(pcm), SAMPLE_RATE, len(pcm) / SAMPLE_RATE))
    if not args.no_trim:
        print("trimmed to exactly %d samples/frame" %
              (SAMPLE_RATE // VIDEO_RATE))
    print("wrote %s" % args.output)


if __name__ == "__main__":
    main()
