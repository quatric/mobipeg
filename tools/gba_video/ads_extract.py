#!/usr/bin/env python3
"""Extract ADS-era (Majesco) GBA Video carts.

Handles the whole container chain that sits above the codec:

    ROM -> SFCD archive -> .mmstr resource files -> typed resources

Type-4 still images decode all the way to pixels (raw RGB555). Type-1 video
decodes as far as the per-frame block-index raster; the codebook that turns
an index into 8 pixels is not yet reverse engineered, so video frames are
dumped as 60x80 index maps rather than full colour. See
doc/gba_video_ads.md for the analysis behind all of this.

Usage:
    ads_extract.py <rom.gba> <outdir> [--video] [--frames N]
"""

import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ads_lzma import decode_blob, decode_raw          # noqa: E402

# Resource types found in .mmstr archives.
T_VIDEO, T_AUDIO, T_IMAGE, T_TEXT = 1, 2, 4, 5

# The obfuscated character encoding used for names/strings inside .mmstr.
_SPECIAL = {
    0xFF: ' ', 0xFE: '\n', 63: '.', 64: ',', 65: ':', 66: ';', 67: '!',
    68: '?', 69: '&', 70: '(', 71: ')', 72: "'", 73: '-', 74: '/', 75: '+',
    78: '_', 79: '$', 80: '"', 81: '<', 82: '>', 83: '*', 84: '=', 86: '#',
    87: '@', 88: '%', 89: '\\', 90: '~', 91: '[', 92: ']',
}


def decode_char(x):
    if x in _SPECIAL:
        return _SPECIAL[x]
    if 1 <= x <= 10:
        return chr(x + 47)          # digits
    if 11 <= x <= 36:
        return chr(x + 86)          # lowercase
    if 37 <= x <= 62:
        return chr(x + 28)          # uppercase
    return '?'


def read_name(d, p):
    """Read a NUL-terminated obfuscated string; returns (text, raw_length)."""
    nb = []
    while p < len(d) and d[p] != 0:
        nb.append(decode_char(d[p]))
        p += 1
    return ''.join(nb), len(nb)


def find_sfcd(rom):
    """Locate the SFCD archive inside a GBA Video ROM."""
    idx = rom.find(b'SFCD')
    return idx if idx >= 0 else None


def parse_sfcd(rom, base):
    """Yield (name, data_bytes) for each file in the SFCD archive."""
    data_off = struct.unpack_from('<H', rom, base + 4)[0] + 8
    count = struct.unpack_from('<I', rom, base + 8)[0]
    p = base + 12
    for _ in range(count):
        size, off = struct.unpack_from('<II', rom, p)
        p += 8
        nb = bytearray()
        while rom[p] != 0:
            nb.append(rom[p])
            p += 1
        p += 1
        while p % 4:
            p += 1
        start = base + data_off + off
        yield nb.decode('latin-1'), rom[start:start + size]


def parse_mmstr(d):
    """Yield (type, data_offset, byte_length) for each resource.

    Entry header is a single u32: size_in_words:28 | type:4, and the payload
    follows immediately, so the next entry sits at data + size*4.
    """
    total, count = struct.unpack_from('<II', d, 0)
    pos = 8
    for _ in range(count & 0xFF):
        if pos + 4 > len(d):
            return
        w = struct.unpack_from('<I', d, pos)[0]
        nbytes = (w & 0x0FFFFFFF) * 4
        yield (w >> 28), pos + 4, nbytes
        pos = pos + 4 + nbytes


def parse_image(d, off):
    """Type-4 resource -> (name, width, height, rgb555_bytes)."""
    hdr = struct.unpack_from('<I', d, off)[0]
    w, h = hdr & 0xFFF, (hdr >> 12) & 0xFFF
    name, ln = read_name(d, off + 4)
    body = off + 4 + ((ln + 4) & ~3)
    pixels, _params, _used = decode_blob(d, body)
    return name, w, h, pixels


def parse_video(d, off, nbytes):
    """Type-1 resource -> dict describing the video stream."""
    w0 = struct.unpack_from('<I', d, off)[0]
    info = {
        'chapters': w0 & 0xFF,
        'frames': (w0 >> 8) & 0xFFFF,
        'magic': struct.unpack_from('<I', d, off + 4)[0],
        'chunks': struct.unpack_from('<I', d, off + 8)[0],
    }
    title, ln = read_name(d, off + 12)
    info['title'] = title
    p = off + 12 + ((ln + 4) & ~3)

    chapters = []
    for _ in range(info['chapters']):
        v = struct.unpack_from('<H', d, p)[0]
        s, sl = read_name(d, p + 2)
        chapters.append((v, s))
        p += 2 + ((sl + 4) & ~3)
    info['chapter_list'] = chapters

    info['stream'] = (p + 3) & ~3
    info['end'] = off + nbytes - 4          # trailing u32 terminator
    return info


def iter_chunks(d, info):
    """Yield (offset, frames, sizeA, sizeB) for each chunk in a video stream."""
    p = info['stream']
    while p + 8 <= info['end']:
        a, b = struct.unpack_from('<II', d, p)
        sa = (b & 0x1FFF) * 4
        sb = (b >> 13) * 4
        yield p, (a >> 16), sa, sb
        p += 8 + sa + sb


def save_png(path, w, h, rgb):
    try:
        from PIL import Image
    except ImportError:
        return False
    im = Image.new('RGB', (w, h))
    im.putdata(rgb)
    im.save(path)
    return True


def rgb555(v):
    return ((v & 31) * 255 // 31, ((v >> 5) & 31) * 255 // 31,
            ((v >> 10) & 31) * 255 // 31)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('rom')
    ap.add_argument('outdir')
    ap.add_argument('--video', action='store_true',
                    help='also dump video frame index rasters')
    ap.add_argument('--frames', type=int, default=8,
                    help='max video frames to dump per stream')
    args = ap.parse_args()

    rom = open(args.rom, 'rb').read()
    base = find_sfcd(rom)
    if base is None:
        sys.exit('no SFCD archive found - not an ADS-era GBA Video ROM '
                 '(GCC-era carts use ActImagine VX; look for "VXDS")')
    print(f'SFCD @ {base} (0x{base:x})')
    os.makedirs(args.outdir, exist_ok=True)

    for name, data in parse_sfcd(rom, base):
        print(f'\n== {name}  ({len(data)} bytes)')
        raw = os.path.join(args.outdir, name)
        open(raw, 'wb').write(data)
        if not name.endswith('.mmstr'):
            continue
        for rtype, off, nbytes in parse_mmstr(data):
            if rtype == T_IMAGE:
                try:
                    iname, w, h, px = parse_image(data, off)
                except Exception as e:                        # noqa: BLE001
                    print(f'   image: FAILED ({e})')
                    continue
                ok = len(px) == w * h * 2
                print(f'   image {iname!r} {w}x{h} '
                      f'{"ok" if ok else "SIZE MISMATCH"}')
                if ok:
                    rgb = [rgb555(struct.unpack_from('<H', px, i * 2)[0])
                           for i in range(w * h)]
                    out = os.path.join(args.outdir, f'{name}.{iname}.png')
                    if not save_png(out, w, h, rgb):
                        print('      (install Pillow to write PNGs)')
            elif rtype == T_VIDEO:
                info = parse_video(data, off, nbytes)
                print(f'   video {info["title"]!r} frames={info["frames"]} '
                      f'chunks={info["chunks"]} chapters={info["chapters"]}')
                for v, s in info['chapter_list']:
                    print(f'      chunk {v:5d}: {s}')
                if not args.video:
                    continue
                dumped = 0
                for coff, nf, sa, sb in iter_chunks(data, info):
                    if dumped >= args.frames or not sb:
                        break
                    idx, _p, _u = decode_blob(data, coff + 8 + sa)
                    for f in range(nf):
                        if dumped >= args.frames:
                            break
                        fr = idx[f * 4800:(f + 1) * 4800]
                        if len(fr) < 4800:
                            break
                        # 60x80 raster of block indices; each block is 4x2
                        # pixels, so a full frame is 240x160.
                        out = os.path.join(
                            args.outdir, f'{name}.frame{dumped:04d}.png')
                        save_png(out, 60, 80, [(b, b, b) for b in fr])
                        dumped += 1
            elif rtype == T_AUDIO:
                print(f'   audio: {nbytes} bytes (format not yet reversed)')


if __name__ == '__main__':
    main()
