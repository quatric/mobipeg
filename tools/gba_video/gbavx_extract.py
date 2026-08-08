#!/usr/bin/env python3
"""List and unpack ActImagine movie streams in a 64 MB GBA Video cartridge.

These carts (Shrek + Shark Tale and friends) are ActImagine, but neither the
ADS nor the Hydrogen stack and not the DS '.vx' container either. Their video
is one continuous bitstream with no per-frame sizes, which is why the header's
seek table addresses it in *bits*.  Two GBA revisions are known: ``VX++`` and
the earlier CAVLC-based ``VXGB`` revision.

    ./gbavx_extract.py rom.gba                 # list the streams
    ./gbavx_extract.py rom.gba -o outdir       # dump every stream

Each dumped stream becomes three files: `.hdr.txt` (fields, chapters, seek
table), `.video` (the raw bitstream, starting at bit 0 of frame 0) and
`.audio` (3124 bytes of codebook extradata, then the audio data).

Neither bitstream is the DS VX bitstream. VXGB does reuse its H.264-style CAVLC
residual coder, while VX++ replaces that component with a cartridge-ROM VLC
codebook. See doc/gba_video_vxgb.md and doc/gba_video_vxpp.md.
"""

import argparse
import os
import struct
import sys

MAGICS = (b'VX++', b'VXGB')
HEADER_SIZE = 0x38

# The coefficient codebook and the decoder itself both live in cartridge ROM and
# are copied to RAM at boot, so a parser has to lift them alongside the streams.
# Offsets are fixed for this cart; see doc/gba_video_vxpp.md section 15.
#
# The codebook is one contiguous blob: 4096 LE uint16 VLC cells, then the two
# escape tables at +0x2000 and +0x2080, matching the runtime layout the decoder
# indexes off its base pointer. Note 0x9ae4, *not* the 0xa000 that sections 3-5
# assumed -- that address is 0x51c too far and yields a different table.
#   name -> (rom offset, length)
CODEBOOK = 0x9ae4
VXPP_TABLES = {
    'vlc_rom_full4096.bin':   (CODEBOOK, 8192),           # 4096 LE uint16 cells
    'value_offset_table.bin': (CODEBOOK + 0x2000, 128),   # escape: value offsets
    'run_offset_table.bin':   (CODEBOOK + 0x2080, 128),   # escape: run offsets
    # The decoder runs from IWRAM; this is the image copied there at boot, so
    # ROM 0xbcc8 == IWRAM 0x03000000. Every address in the doc indexes into it.
    'iwram.bin':              (0xbcc8, 0x8000),
}

# The original VXGB decoder has no external VLC codebook.  Its complete IWRAM
# source image begins 0x780 bytes earlier than VX++ in the Shrek Rev 5 ROM.
# A live mGBA dump during movie playback matches this 32 KiB ROM window in
# 32516/32768 bytes; the remaining bytes are self-modified immediates, relocated
# pointers and interrupt state.  Keeping the ROM image makes every 0x0300xxxx
# decoder address available to the disassembler without an emulator.
VXGB_TABLES = {
    'iwram.bin':              (0xb548, 0x8000),
}

# The audio region opens with the trained codebooks: 3*64*8 int16, then
# 8 uint16 scale modifiers, 8 int32 lpc bases and one uint32 initial scale.
# Same block, same size, as the DS files - see libavcodec/vx_audio.c.
AUDIO_EXTRADATA_SIZE = 3 * 64 * 8 * 2 + 8 * 2 + 8 * 4 + 4


class Stream:
    def __init__(self, rom, off):
        f = struct.unpack_from('<4s13I', rom, off)
        self.off = off
        (self.magic, self.nb_frames, self.width, self.height, self.frame_rate,
         self.quantizer, self.sample_rate, self.audio_off, self.chapter_off,
         self.last_chapter, self.seek_off, self.nb_seek, self.unk30,
         self.unk34) = f

        n = struct.unpack_from('<I', rom, off + self.chapter_off)[0]
        self.chapters = list(struct.unpack_from('<%dI' % n, rom,
                                                off + self.chapter_off + 4))
        self.seek = [struct.unpack_from('<4I', rom,
                                        off + self.seek_off + 16 * i)
                     for i in range(self.nb_seek)]

        # The chapter table is the last thing in the stream, so it ends it.
        self.size = self.chapter_off + 4 + 4 * n

    @property
    def fps(self):
        """Playback cadence for the retail 0x30c3 timing field.

        This field is not Q10 frames/second.  The audio stream proves the
        cadence independently: 128/7 AFrames per video frame, 128 samples per
        AFrame and 16384 samples/second gives exactly 7 video frames/second.
        """
        if self.frame_rate == 0x30c3:
            return 7.0
        raise ValueError("unknown GBA VX frame timing field %#x" % self.frame_rate)

    def ok(self, romlen):
        return (self.width in range(16, 241) and self.height in range(16, 241)
                and not (self.width % 16) and not (self.height % 16)
                and 0 < self.nb_frames < (1 << 20)
                and 0 < self.nb_seek < (1 << 16)
                and 0 < self.audio_off < self.chapter_off
                and self.off + self.size <= romlen)

    def describe(self):
        secs = self.nb_frames / self.fps
        out = [
            'magic           %s' % self.magic.decode('ascii'),
            'offset          0x%08x' % self.off,
            'size            0x%08x' % self.size,
            'frames          %d' % self.nb_frames,
            'geometry        %dx%d' % (self.width, self.height),
            'frame rate      0x%04x  (%.2f fps)' % (self.frame_rate, self.fps),
            'duration        %d:%02d' % (secs // 60, secs % 60),
            'quantizer field %d' % self.quantizer,
            'sample rate     %d' % self.sample_rate,
            'video           0x%08x .. 0x%08x' % (HEADER_SIZE, self.audio_off),
            'audio           0x%08x .. 0x%08x' % (self.audio_off,
                                                  self.chapter_off),
            'chapters        %d at 0x%08x' % (len(self.chapters),
                                              self.chapter_off),
            'seek entries    %d at 0x%08x' % (self.nb_seek, self.seek_off),
            'unknown +30/+34 %d / %d' % (self.unk30, self.unk34),
            '',
            'chapter start frames:',
            '  ' + ' '.join(str(c) for c in self.chapters),
            '',
            'seek table (frame, video bit, audio byte):',
        ]
        out += ['  %8d  %12d  %10d' % (e[0], e[1], e[2]) for e in self.seek]
        return '\n'.join(out) + '\n'


def find_streams(rom):
    out, positions = [], set()
    for magic in MAGICS:
        pos = 0
        while True:
            pos = rom.find(magic, pos)
            if pos < 0:
                break
            positions.add(pos)
            pos += 4

    for pos in sorted(positions):
        if not (pos & 3):
            try:
                s = Stream(rom, pos)
            except struct.error:
                s = None
            if s and s.ok(len(rom)):
                out.append(s)
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('rom')
    ap.add_argument('-o', '--outdir', help='dump the streams here')
    args = ap.parse_args()

    with open(args.rom, 'rb') as f:
        rom = f.read()

    streams = find_streams(rom)
    if not streams:
        print('no VX++ or VXGB streams in %s' % args.rom, file=sys.stderr)
        return 1

    for i, s in enumerate(streams):
        print('[%d] %s 0x%08x  %dx%d  %d frames  %.2f fps  %d Hz  %d chapters'
              % (i, s.magic.decode('ascii'), s.off, s.width, s.height,
                 s.nb_frames, s.fps,
                 s.sample_rate, len(s.chapters)))
        if not args.outdir:
            continue

        os.makedirs(args.outdir, exist_ok=True)
        base = os.path.join(args.outdir, 'stream%02d' % i)
        with open(base + '.hdr.txt', 'w') as f:
            f.write(s.describe())
        with open(base + '.video', 'wb') as f:
            f.write(rom[s.off + HEADER_SIZE:s.off + s.audio_off])
        with open(base + '.audio', 'wb') as f:
            f.write(rom[s.off + s.audio_off:s.off + s.chapter_off])
        print('     -> %s.{hdr.txt,video,audio}' % base)

    magics = {s.magic for s in streams}
    if args.outdir and b'VX++' in magics:
        for name, (off, n) in sorted(VXPP_TABLES.items()):
            with open(os.path.join(args.outdir, name), 'wb') as f:
                f.write(rom[off:off + n])
        print('[tables] -> %s' % ', '.join(sorted(VXPP_TABLES)))
    elif args.outdir and b'VXGB' in magics:
        for name, (off, n) in sorted(VXGB_TABLES.items()):
            with open(os.path.join(args.outdir, name), 'wb') as f:
                f.write(rom[off:off + n])
        print('[VXGB decoder] -> %s' % ', '.join(sorted(VXGB_TABLES)))

    return 0


if __name__ == '__main__':
    sys.exit(main())
