#!/usr/bin/env python3
"""Decode the FVMV streams in Nintendo's Pokemon GBA Video cartridges.

The cartridge copies its ARM decoder from ROM to IWRAM.  This tool executes
that decoder offline with Unicorn, feeds it the packetized FVMV data directly,
and emits an ordinary media file through FFmpeg.  No GBA emulation or screen/
audio capture is involved.

    python3 -m pip install unicorn
    ./tools/gba_video/fvmv_decode.py pokemon.gba
    ./tools/gba_video/fvmv_decode.py pokemon.gba -s 0 -o episode-1.mp4

Without ``-s``, the tool only lists the streams found in the ROM.  ``--raw-dir``
keeps the decoded BGR555 video and signed 8-bit PCM instead of invoking FFmpeg.
"""

import argparse
import hashlib
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile


FVMV_MAGIC = b"FVMV"
VIDEO_RATE = 8
AUDIO_RATE = 65536
AUDIO_BLOCK_IN = 40
AUDIO_BLOCK_OUT = 1024
FRAME_SIZE = 240 * 160 * 2

# This prefix is at IWRAM +0x5fe8 in the known Pokemon player.  Finding it in
# ROM makes decoder-image discovery independent of the image's cartridge offset.
VIDEO_DECODER_PREFIX = bytes.fromhex("f05f2de90281a0e3419e4fe2")
VIDEO_DECODER_IWRAM_OFF = 0x5FE8
IWRAM_IMAGE_SIZE = 0x8000

ROM_BASE = 0x08000000
IWRAM_BASE = 0x03000000
EWRAM_BASE = 0x02000000
FRAME0 = EWRAM_BASE + 8
FRAME1 = FRAME0 + FRAME_SIZE
WORK = EWRAM_BASE + 0x26000
PCM_WORK = EWRAM_BASE + 0x28000
STACK = EWRAM_BASE + 0x3F000
RETURN = IWRAM_BASE + 0x7FF0


def u32(data, off):
    if off < 0 or off + 4 > len(data):
        raise ValueError("read beyond ROM at %#x" % off)
    return struct.unpack_from("<I", data, off)[0]


class Stream:
    def __init__(self, rom, offset):
        self.offset = offset
        self.frames = u32(rom, offset + 4)
        self.width = u32(rom, offset + 8)
        self.height = u32(rom, offset + 12)
        self.clock = u32(rom, offset + 16)
        self.size = u32(rom, offset + 20)
        self.first_packet = offset + 0x20
        self.data_end = self.first_packet + self.size

    @property
    def duration(self):
        return self.frames / VIDEO_RATE


class Packet:
    def __init__(self, rom, offset):
        self.offset = offset
        self.size = u32(rom, offset)
        self.video_size = u32(rom, offset + 0x10)
        self.video = offset + 0x14
        self.audio_header = offset + 0x10 + self.video_size
        self.audio_size = u32(rom, self.audio_header + 4)
        self.audio = self.audio_header + 8
        self.next = offset + 0x10 + self.size


def find_streams(rom):
    streams = []
    start = 0
    while True:
        off = rom.find(FVMV_MAGIC, start)
        if off < 0:
            break
        start = off + 4
        try:
            stream = Stream(rom, off)
        except ValueError:
            continue
        if not (0 < stream.frames < 100000 and
                stream.width == 240 and stream.height == 160 and
                stream.size >= 0x20 and stream.data_end <= len(rom)):
            continue
        streams.append(stream)
    return streams


def iter_packets(rom, stream, limit=0):
    off = stream.first_packet
    count = stream.frames if not limit else min(stream.frames, limit)
    stream_end = stream.data_end
    for frame in range(count):
        packet = Packet(rom, off)
        if (packet.size < 8 or packet.video_size < 8 or
                packet.audio_size % AUDIO_BLOCK_IN or
                packet.audio + packet.audio_size > packet.next or
                packet.next > stream_end):
            raise ValueError("invalid FVMV packet %d at %#x" % (frame, off))
        yield frame, packet
        off = packet.next


def find_iwram_image(rom):
    decode = rom.find(VIDEO_DECODER_PREFIX)
    if decode < VIDEO_DECODER_IWRAM_OFF:
        raise ValueError("FVMV IWRAM decoder image not found in ROM")
    base = decode - VIDEO_DECODER_IWRAM_OFF
    image = rom[base:base + IWRAM_IMAGE_SIZE]
    if len(image) != IWRAM_IMAGE_SIZE:
        raise ValueError("truncated FVMV IWRAM decoder image")
    return base, image


class ArmDecoder:
    def __init__(self, rom, iwram):
        try:
            from unicorn import Uc, UC_ARCH_ARM, UC_MODE_ARM, UC_MODE_LITTLE_ENDIAN
            from unicorn.arm_const import (UC_ARM_REG_CPSR, UC_ARM_REG_LR,
                                            UC_ARM_REG_PC, UC_ARM_REG_R0,
                                            UC_ARM_REG_R1, UC_ARM_REG_R2,
                                            UC_ARM_REG_SP)
        except ImportError:
            raise SystemExit("FVMV decoding needs Unicorn: "
                             "python3 -m pip install unicorn")

        self.UC_ARM_REG_CPSR = UC_ARM_REG_CPSR
        self.UC_ARM_REG_LR = UC_ARM_REG_LR
        self.UC_ARM_REG_PC = UC_ARM_REG_PC
        self.UC_ARM_REG_R0 = UC_ARM_REG_R0
        self.UC_ARM_REG_R1 = UC_ARM_REG_R1
        self.UC_ARM_REG_R2 = UC_ARM_REG_R2
        self.UC_ARM_REG_SP = UC_ARM_REG_SP
        self.uc = Uc(UC_ARCH_ARM, UC_MODE_ARM | UC_MODE_LITTLE_ENDIAN)
        self.uc.mem_map(EWRAM_BASE, 0x40000)
        self.uc.mem_map(IWRAM_BASE, IWRAM_IMAGE_SIZE)
        rom_size = (len(rom) + 0xFFF) & ~0xFFF
        self.uc.mem_map(ROM_BASE, rom_size)
        self.uc.mem_write(IWRAM_BASE, iwram)
        self.uc.mem_write(ROM_BASE, rom)
        self.uc.mem_write(FRAME0, bytes(FRAME_SIZE))
        self.uc.mem_write(FRAME1, bytes(FRAME_SIZE))
        self.src = FRAME0
        self.dst = FRAME1

    def call(self, pc, r0, r1=0, r2=0, count=20000000):
        uc = self.uc
        uc.reg_write(self.UC_ARM_REG_CPSR, 0x1F)
        uc.reg_write(self.UC_ARM_REG_R0, r0)
        uc.reg_write(self.UC_ARM_REG_R1, r1)
        uc.reg_write(self.UC_ARM_REG_R2, r2)
        uc.reg_write(self.UC_ARM_REG_SP, STACK)
        uc.reg_write(self.UC_ARM_REG_LR, RETURN)
        uc.emu_start(pc, RETURN, count=count)
        if uc.reg_read(self.UC_ARM_REG_PC) != RETURN:
            raise RuntimeError("ARM decoder did not return from %#x" % pc)

    def video(self, packet, width, height):
        obj = struct.pack("<IIIIII", self.src, self.dst,
                          ROM_BASE + packet.video, width, height, 0)
        self.uc.mem_write(WORK, obj + bytes(0x40 - len(obj)))
        self.call(IWRAM_BASE + 0x5FE8, WORK)
        image = bytes(self.uc.mem_read(self.dst, width * height * 2))
        self.src, self.dst = self.dst, self.src
        return image

    def audio_blocks(self, packet):
        for off in range(0, packet.audio_size, AUDIO_BLOCK_IN):
            self.call(IWRAM_BASE + 0x1B88,
                      ROM_BASE + packet.audio + off, WORK, count=2000000)
            self.call(IWRAM_BASE + 0x1D18,
                      WORK, PCM_WORK, AUDIO_BLOCK_OUT, count=2000000)
            yield bytes(self.uc.mem_read(PCM_WORK, AUDIO_BLOCK_OUT))


def decode_raw(rom, stream, video_path, audio_path, limit=0):
    iwram_off, iwram = find_iwram_image(rom)
    decoder = ArmDecoder(rom, iwram)
    count = stream.frames if not limit else min(stream.frames, limit)
    print("decoder image: ROM %#x -> IWRAM %#x" % (iwram_off, IWRAM_BASE))
    with open(video_path, "wb") as video, open(audio_path, "wb") as audio:
        for frame, packet in iter_packets(rom, stream, limit):
            video.write(decoder.video(packet, stream.width, stream.height))
            for block in decoder.audio_blocks(packet):
                audio.write(block)
            if frame == 0 or (frame + 1) % 256 == 0 or frame + 1 == count:
                print("decoded %d/%d frames" % (frame + 1, count), flush=True)


def mux(ffmpeg, stream, video_path, audio_path, output):
    cmd = [ffmpeg, "-hide_banner", "-loglevel", "warning", "-y",
           "-f", "rawvideo", "-pixel_format", "bgr555le",
           "-video_size", "%dx%d" % (stream.width, stream.height),
           "-framerate", str(VIDEO_RATE), "-i", os.fspath(video_path),
           "-f", "s8", "-ar", str(AUDIO_RATE), "-ac", "1",
           "-i", os.fspath(audio_path),
           "-c:v", "mpeg4", "-q:v", "2", "-pix_fmt", "yuv420p",
           "-c:a", "aac", "-ar", "48000", "-b:a", "128k",
           "-shortest"]
    if output.suffix.lower() in (".mp4", ".mov", ".m4v"):
        cmd += ["-movflags", "+faststart"]
    cmd.append(os.fspath(output))
    print("muxing", output)
    subprocess.run(cmd, check=True)


def format_duration(seconds):
    minutes, seconds = divmod(seconds, 60)
    return "%d:%05.2f" % (minutes, seconds)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("rom", type=Path)
    parser.add_argument("-s", "--stream", type=int,
                        help="zero-based FVMV stream to decode")
    parser.add_argument("-o", "--output", type=Path,
                        help="output media file (normally .mp4 or .mkv)")
    parser.add_argument("--raw-dir", type=Path,
                        help="keep raw .bgr555le and .s8 output here")
    local_ffmpeg = Path(__file__).resolve().parents[2] / "ffmpeg"
    default_ffmpeg = (os.fspath(local_ffmpeg) if os.access(local_ffmpeg, os.X_OK)
                      else (shutil.which("ffmpeg") or "ffmpeg"))
    parser.add_argument("--ffmpeg", default=default_ffmpeg)
    parser.add_argument("--max-frames", type=int, default=0,
                        help="decode only this many frames (testing only)")
    args = parser.parse_args(argv)

    rom = args.rom.read_bytes()
    streams = find_streams(rom)
    if not streams:
        parser.error("no FVMV streams found")
    print("ROM SHA-256", hashlib.sha256(rom).hexdigest())
    for index, stream in enumerate(streams):
        print("[%d] offset=%#x size=%#x frames=%d %dx%d duration=%s" %
              (index, stream.offset, stream.size, stream.frames,
               stream.width, stream.height,
               format_duration(stream.duration)))

    if args.stream is None:
        return 0
    if not 0 <= args.stream < len(streams):
        parser.error("stream index out of range")
    if not args.output and not args.raw_dir:
        parser.error("decoding needs --output or --raw-dir")
    if args.max_frames < 0:
        parser.error("--max-frames cannot be negative")

    stream = streams[args.stream]
    if args.raw_dir:
        raw_dir = args.raw_dir
        raw_dir.mkdir(parents=True, exist_ok=True)
        video = raw_dir / ("stream-%d.bgr555le" % args.stream)
        audio = raw_dir / ("stream-%d.s8" % args.stream)
        decode_raw(rom, stream, video, audio, args.max_frames)
        if args.output:
            args.output.parent.mkdir(parents=True, exist_ok=True)
            mux(args.ffmpeg, stream, video, audio, args.output)
    else:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix=".fvmv-",
                                         dir=os.fspath(args.output.parent)) as tmp:
            video = Path(tmp) / "video.bgr555le"
            audio = Path(tmp) / "audio.s8"
            decode_raw(rom, stream, video, audio, args.max_frames)
            mux(args.ffmpeg, stream, video, audio, args.output)
    return 0


if __name__ == "__main__":
    sys.exit(main())
