#!/usr/bin/env python3
"""
RocketVideo (.rvid) encoder for mobipeg.

Produces version-5 .rvid files playable by RocketVideoPlayer
(https://github.com/RocketRobz/RocketVideoPlayer).  This is a faithful port of
the Vid2RVID conversion logic, driven by mobipeg's bundled ffmpeg fork:

  source video --ffmpeg--> raw rgb24 frames  --> pack into 16bpp .rvid frames
  source audio --ffmpeg--> raw PCM           --> appended as the sound stream

Container layout (v5, little-endian), matching common/rvidHeader.* and
Vid2RVID/source/main.cpp:

  0x000  header (0x200 bytes; only the first 0x20 are meaningful)
  0x200  frame offset table   : u32[frames], absolute file offset of each frame
  ....   compressed-size table: u32[frames] (16bpp), present iff compressed
  ....   frame data           : per frame, either LZ10 stream or raw 512*vRes
  ....   left  PCM stream
  ....   right PCM stream      (stereo only)

Scope of this encoder (v1): 16bpp modes (RGB555 / RGB565), single screen,
progressive scan, optional LZ10 compression with per-frame raw fallback and
exact-duplicate frame de-duplication.  8bpp/palette, interlacing and dual-screen
are intentionally left for a follow-up (they need external colour reduction /
field splitting the way upstream does).
"""
import os
import sys
import ctypes
import struct
import subprocess
from fractions import Fraction

import numpy as np

_HERE = os.path.dirname(os.path.abspath(__file__))
RVID_MAGIC = 0x44495652  # "RVID"
RVID_VER = 5

# bmpMode values understood by the player.  Mode 0 (8bpp palette) is not
# produced here (see module docstring), but the constants are named for clarity.
BMP_8BPP_565 = 0
BMP_16BPP_555 = 1
BMP_16BPP_565 = 2

# Sample rates the player's UI advertises; we clamp/round to the nearest.
SUPPORTED_RATES = (8000, 11025, 16000, 22050, 32000)


# --------------------------------------------------------------------------- #
# LZ10 compression (ctypes wrapper around rvid_lz.c, pure-python fallback)
# --------------------------------------------------------------------------- #
_lz_lib = None
_lz_tried = False


def _load_lz():
    """Load (compiling on first use) the C LZ10 compressor. Returns the ctypes
    function or None if a compiler isn't available."""
    global _lz_lib, _lz_tried
    if _lz_tried:
        return _lz_lib
    _lz_tried = True
    so = os.path.join(_HERE, "rvid_lz.so")
    src = os.path.join(_HERE, "rvid_lz.c")
    try:
        if (not os.path.exists(so)) or (
            os.path.exists(src) and os.path.getmtime(src) > os.path.getmtime(so)
        ):
            cc = os.environ.get("CC", "cc")
            subprocess.run([cc, "-O3", "-shared", "-fPIC", src, "-o", so],
                           check=True)
        lib = ctypes.CDLL(so)
        lib.rvid_lz10.restype = ctypes.c_int
        lib.rvid_lz10.argtypes = [ctypes.c_char_p, ctypes.c_int,
                                  ctypes.c_char_p, ctypes.c_int]
        _lz_lib = lib
    except Exception as e:
        print(f"   (rvid: C LZ10 unavailable, using slower python fallback: {e})")
        _lz_lib = None
    return _lz_lib


def _lz10_py(data):
    """Pure-python LZ10 compressor (fallback). Correct but slow; mirrors the
    reference greedy search with a 4KB window and 3..18 match length."""
    n = len(data)
    out = bytearray((0x10, n & 0xFF, (n >> 8) & 0xFF, (n >> 16) & 0xFF))
    pos = 0
    while pos < n:
        flag_index = len(out)
        out.append(0)
        flag = 0
        for i in range(8):
            best_len, best_back = 2, 1
            maxnum = min(18, n - pos)
            start = max(0, pos - 0x1000)
            for cand in range(pos - 1, start - 1, -1):
                if data[cand] != data[pos]:
                    continue
                ln = 0
                while ln < maxnum and data[cand + ln] == data[pos + ln]:
                    ln += 1
                if ln > best_len:
                    best_len, best_back = ln, pos - cand
                    if best_len == maxnum:
                        break
            if best_len > 2:
                out.append((((best_back - 1) >> 8) & 0xF) | (((best_len - 3) & 0xF) << 4))
                out.append((best_back - 1) & 0xFF)
                pos += best_len
                flag = (flag << 1) | 1
            else:
                out.append(data[pos])
                pos += 1
                flag <<= 1
            if pos >= n:
                flag <<= (7 - i)
                break
        out[flag_index] = flag
    while len(out) % 4:
        out.append(0)
    return bytes(out)


def lz10_compress(data):
    """Compress `data` (bytes) as Nintendo LZ10. Returns bytes."""
    lib = _load_lz()
    if lib is None:
        return _lz10_py(data)
    cap = len(data) + len(data) // 8 + 16
    buf = ctypes.create_string_buffer(cap)
    size = lib.rvid_lz10(data, len(data), buf, cap)
    if size < 0:
        return _lz10_py(data)
    return buf.raw[:size]


# --------------------------------------------------------------------------- #
# ffmpeg helpers
# --------------------------------------------------------------------------- #
def _ffprobe_val(ffprobe, inp, ifmt, stream, entry):
    try:
        p = subprocess.run(
            [ffprobe, "-v", "error"] + ifmt +
            ["-select_streams", stream, "-show_entries", entry,
             "-of", "default=nk=1:nw=1", inp],
            capture_output=True, text=True)
        out = p.stdout.strip().splitlines()
        return out[0] if out else None
    except Exception:
        return None


def probe_fps(ffprobe, inp, ifmt):
    v = _ffprobe_val(ffprobe, inp, ifmt, "v:0", "stream=r_frame_rate")
    if not v:
        return None
    try:
        return float(Fraction(v))
    except Exception:
        return None


def has_audio(ffprobe, inp, ifmt):
    return bool(_ffprobe_val(ffprobe, inp, ifmt, "a:0", "stream=index"))


def audio_channels(ffprobe, inp, ifmt):
    v = _ffprobe_val(ffprobe, inp, ifmt, "a:0", "stream=channels")
    try:
        return int(v)
    except Exception:
        return 0


# --------------------------------------------------------------------------- #
# Pixel packing (RGB24 -> DS 16bpp), faithful to Vid2RVID convertFrame()
# --------------------------------------------------------------------------- #
def _dither(frame, frame_parity, green_delta):
    """Apply Vid2RVID's alternating-pixel ordered dither in-place-ish.

    Reference nudges R/B by +4 and G by +green_delta on a checkerboard of
    pixels (cell parity = (x + y + frame_parity) & 1) before quantisation, but
    only where the channel value is within [delta, 0x100-delta) so it can't
    wrap. Returns a new uint16 array (R,G,B still 8-bit) for quantisation."""
    h, w, _ = frame.shape
    yy, xx = np.indices((h, w))
    mask = ((xx + yy + frame_parity) & 1).astype(bool)
    out = frame.astype(np.int16)
    r, g, b = out[..., 0], out[..., 1], out[..., 2]

    def nudge(ch, delta):
        m = mask & (ch >= delta) & (ch < 0x100 - delta)
        ch[m] += delta

    nudge(r, 4)
    nudge(g, green_delta)
    nudge(b, 4)
    return out


def pack_frame(frame, bmp_mode, frame_parity, dither):
    """Convert an (H,W,3) uint8 RGB24 frame to little-endian DS 16bpp bytes."""
    if dither:
        f = _dither(frame, frame_parity, 2 if bmp_mode == BMP_16BPP_565 else 4)
    else:
        f = frame.astype(np.int16)
    r = (f[..., 0] >> 3).astype(np.uint16)
    g8 = f[..., 1].astype(np.uint16)
    b = (f[..., 2] >> 3).astype(np.uint16)

    if bmp_mode == BMP_16BPP_555:
        color = r | ((f[..., 1] >> 3).astype(np.uint16) << 5) | (b << 10) | 0x8000
    else:
        # RGB565 packed into a 1555 word the DS way: 6-bit green split so its
        # LSB lands in bit15 and its top 5 bits in bits5..9 (see convertFrame).
        green6 = (g8 >> 2) & 0x3F
        color = r | (b << 10)
        color |= (green6 & 0x1) << 15          # green bit0 -> bit15
        color |= ((green6 >> 1) & 0x1F) << 5    # green bits1..5 -> bits5..9
    return color.astype("<u2").tobytes()


def _pack_565_word(rgb):
    """Pack an (...,3) uint8 RGB array into DS 565-in-1555 u16 words (the packing
    convertFrame() uses for bmpMode 0 and 2: 6-bit green split across bit15 and
    bits5..9)."""
    r = (rgb[..., 0] >> 3).astype(np.uint16)
    b = (rgb[..., 2] >> 3).astype(np.uint16)
    green6 = ((rgb[..., 1] >> 2) & 0x3F).astype(np.uint16)
    c = r | (b << 10)
    c |= (green6 & 0x1) << 15
    c |= ((green6 >> 1) & 0x1F) << 5
    return c.astype(np.uint16)


def pack_frame_8bpp(frame, frame_parity, dither):
    """Convert an (H,W,3) RGB24 frame to bmpMode-0 storage: a 256-entry palette
    (565-packed u16 words, 0x200 bytes) followed by H*W 8-bit indices.

    Vid2RVID bakes the checkerboard dither and a 256-colour reduction into the
    PNGs (via ImageMagick) before indexing; we do the equivalent here by
    dithering, snapping to the RGB565 grid, then median-cut quantising to 256
    colours with Pillow. Returns (palette_bytes, index_bytes)."""
    from PIL import Image
    if dither:
        f = np.clip(_dither(frame, frame_parity, 2), 0, 255).astype(np.uint8)
    else:
        f = np.ascontiguousarray(frame)
    # Snap to the RGB565 grid the palette words can represent.
    snapped = np.empty_like(f)
    r5 = f[..., 0] & 0xF8
    g6 = f[..., 1] & 0xFC
    b5 = f[..., 2] & 0xF8
    snapped[..., 0] = r5 | (r5 >> 5)
    snapped[..., 1] = g6 | (g6 >> 6)
    snapped[..., 2] = b5 | (b5 >> 5)

    img = Image.fromarray(snapped).quantize(
        colors=256, method=Image.MEDIANCUT, dither=Image.Dither.NONE)
    idx = np.asarray(img, np.uint8)
    pal = np.zeros((256, 3), np.uint8)
    raw_pal = np.frombuffer(bytes(img.getpalette() or b""), np.uint8)
    n = min(len(raw_pal), 256 * 3)
    pal.reshape(-1)[:n] = raw_pal[:n]
    words = _pack_565_word(pal)                      # (256,) u16
    return words.astype("<u2").tobytes(), idx.reshape(-1).tobytes()


# --------------------------------------------------------------------------- #
# Main pack routine
# --------------------------------------------------------------------------- #
def _decode_fps(fps_float):
    """Map a source fps to the (fps_byte_base, reduce_by_0.1) pair the header
    uses. e.g. 29.97 -> (30, True); 30.0 -> (30, False)."""
    base = int(round(fps_float))
    reduce01 = (base - fps_float) > 0.02
    return max(1, min(120, base)), reduce01


def pack_rvid(ffmpeg, ffprobe, inp, out_path, *,
              ifmt=None, width=256, height=192, bmp_mode=BMP_16BPP_555,
              fps=None, compress=True, dither=True, gba=False, interlaced=False,
              audio="pcm", sample_rate=0, audio_16bit=True):
    """Encode `inp` to a v5 .rvid at `out_path`. Returns the output path.

    `height` is the full display height. When `interlaced` is True each stored
    frame is one field (height/2 scanlines) and the header vRes is height/2;
    the player rebuilds the full image by updating alternate scanlines. `bmp_mode`
    0 selects the 8bpp palette mode (256-colour, per-frame palette)."""
    ifmt = ifmt or []
    if width not in (256, 240):
        # rvidHRes is fixed at the screen width (256 NDS / 240 GBA); reject
        # anything else so frames stay the size the player DMAs.
        raise ValueError("rvid width must be 256 (NDS) or 240 (GBA)")
    if height < 1 or height > 510:
        raise ValueError("rvid height must be 1..510 (vRes 1..255)")
    if interlaced and (height % 2):
        raise ValueError("interlaced rvid needs an even display height")
    vres = height // 2 if interlaced else height
    if vres < 1 or vres > 255:
        raise ValueError("rvid vRes (height, or height/2 if interlaced) must be 1..255")
    is8 = (bmp_mode == BMP_8BPP_565)

    src_fps = fps or probe_fps(ffprobe, inp, ifmt) or 30.0
    fps_base, reduce01 = _decode_fps(src_fps)
    play_fps = Fraction(fps_base * 1000 - (100 if reduce01 else 0), 1000)

    # --- decode video to raw rgb24 frames -------------------------------- #
    vf = f"fps={play_fps.numerator}/{play_fps.denominator},scale={width}:{height}:flags=lanczos"
    vcmd = [ffmpeg, "-nostdin", "-v", "error"] + ifmt + ["-i", inp,
            "-an", "-vf", vf, "-pix_fmt", "rgb24", "-f", "rawvideo", "pipe:1"]
    proc = subprocess.Popen(vcmd, stdout=subprocess.PIPE)
    frame_bytes = width * height * 3
    # Per-stored-frame raw payload size (the LZ target): 16bpp = 2B/px over the
    # stored (field) height; 8bpp = 1 index byte/px (palette is stored separately
    # and never compressed).
    payload_px = width * vres
    raw_payload_size = payload_px * (1 if is8 else 2)

    # Assemble frame data + tables. De-dup identical stored frames.
    frame_blobs = []          # list of bytes actually appended to the file
    frame_offsets = []        # per-source-frame relative offset (into frame data)
    comp_sizes = []           # per-source-frame stored payload size (for the table)
    seen = {}                 # stored-bytes -> (offset, index-in-blobs)

    # Offsets can't be computed until the tables are sized, so accumulate a
    # running data cursor relative to the start of frame data, then rebase.
    data_cursor = 0
    idx = 0                   # source-frame index (drives field parity + dither)
    nframes = 0
    while True:
        buf = proc.stdout.read(frame_bytes)
        if len(buf) < frame_bytes:
            break
        frame = np.frombuffer(buf, dtype=np.uint8).reshape(height, width, 3)
        # Interlaced: keep one field (alternate scanlines) per stored frame.
        field = frame[(idx & 1)::2] if interlaced else frame
        # Dither parity mirrors Vid2RVID: only when progressive, using frame idx.
        parity = 0 if interlaced else (idx & 1)

        if is8:
            pal, payload = pack_frame_8bpp(field, parity, dither)
        else:
            pal, payload = b"", pack_frame(field, bmp_mode, parity, dither)

        stored_payload = payload
        size = raw_payload_size
        if compress:
            packed = lz10_compress(payload)
            if len(packed) < raw_payload_size:
                stored_payload, size = packed, len(packed)

        blob = pal + stored_payload      # palette (if any) precedes the payload
        key = blob
        if key in seen:
            off, _ = seen[key]
            frame_offsets.append(off)
        else:
            off = data_cursor
            seen[key] = (off, len(frame_blobs))
            frame_blobs.append(blob)
            frame_offsets.append(off)
            data_cursor += len(blob)
        comp_sizes.append(size)
        nframes += 1
        idx += 1

    proc.wait()
    if nframes == 0:
        raise RuntimeError("no video frames decoded from input")

    # --- audio ----------------------------------------------------------- #
    left = right = b""
    want_audio = audio != "none" and has_audio(ffprobe, inp, ifmt)
    if want_audio:
        ch = audio_channels(ffprobe, inp, ifmt) or 1
        rate = sample_rate or 32000
        rate = min(SUPPORTED_RATES, key=lambda r: abs(r - rate))
        fmt = "s16le" if audio_16bit else "u8"
        acmd = [ffmpeg, "-nostdin", "-v", "error"] + ifmt + ["-i", inp,
                "-vn", "-ar", str(rate), "-f", fmt, "pipe:1"]
        if ch >= 2:
            acmd[-1:-1] = ["-ac", "2"]
        else:
            acmd[-1:-1] = ["-ac", "1"]
        pcm = subprocess.run(acmd, capture_output=True).stdout
        if ch >= 2:
            arr = np.frombuffer(pcm, dtype="<i2" if audio_16bit else np.uint8)
            arr = arr[: (len(arr) // 2) * 2].reshape(-1, 2)
            left = arr[:, 0].tobytes()
            right = arr[:, 1].tobytes()
        else:
            left = pcm
        sample_rate_hdr = rate
    else:
        sample_rate_hdr = 0

    # --- lay out the file ------------------------------------------------ #
    frame_table_size = 4 * nframes
    # compressed-size table: u32/frame for 16bpp, u16/frame for 8bpp (rounded up
    # to a 4-byte boundary), matching Vid2RVID's compressedFrameSizeTableSize.
    if compress:
        comp_entry = "I" if not is8 else "H"
        comp_table_size = nframes * (4 if not is8 else 2)
        comp_table_size += (-comp_table_size) % 4
    else:
        comp_entry = None
        comp_table_size = 0
    frames_base = 0x200 + frame_table_size + comp_table_size

    abs_offsets = [frames_base + o for o in frame_offsets]
    frames_total = sum(len(b) for b in frame_blobs)
    size_no_audio = frames_base + frames_total
    sound_left_off = size_no_audio if left else 0
    sound_right_off = (size_no_audio + len(left)) if right else 0

    comp_table_off = (0x200 + frame_table_size) if compress else 0

    fps_byte = fps_base + (0x80 if reduce01 else 0)
    header = struct.pack(
        "<IIIBBBBHBBIII",
        RVID_MAGIC, RVID_VER, nframes,
        fps_byte, vres, (1 if interlaced else 0), (2 if gba else 0),
        sample_rate_hdr, (1 if audio_16bit else 0), bmp_mode,
        comp_table_off, sound_left_off, sound_right_off)
    header = header + b"\x00" * (0x200 - len(header))

    with open(out_path, "wb") as f:
        f.write(header)
        f.write(struct.pack(f"<{nframes}I", *abs_offsets))
        if compress:
            tbl = struct.pack(f"<{nframes}{comp_entry}", *comp_sizes)
            tbl += b"\x00" * (comp_table_size - len(tbl))
            f.write(tbl)
        for blob in frame_blobs:
            f.write(blob)
        if left:
            f.write(left)
        if right:
            f.write(right)

    dup = nframes - len(frame_blobs)
    modestr = {BMP_8BPP_565: "8bpp/256", BMP_16BPP_555: "RGB555",
               BMP_16BPP_565: "RGB565"}.get(bmp_mode, "?")
    print(f"   rvid: {nframes} frames ({width}x{height}, "
          f"{modestr}, {'interlaced' if interlaced else 'progressive'}, "
          f"{'LZ10' if compress else 'raw'}), {dup} duplicate frames deduped, "
          f"play rate {float(play_fps):.3f} fps"
          + (f", audio {sample_rate_hdr}Hz "
             f"{'stereo' if right else 'mono'} {'16' if audio_16bit else '8'}-bit"
             if left else ", no audio"))
    return out_path


# --------------------------------------------------------------------------- #
# CLI (used by encode.py; also runnable standalone)
# --------------------------------------------------------------------------- #
def _main(argv):
    import argparse
    ap = argparse.ArgumentParser(description="Encode a video to RocketVideo .rvid")
    ap.add_argument("input")
    ap.add_argument("output")
    ap.add_argument("--scale", default="256x192")
    ap.add_argument("--gba", action="store_true", help="target GBA (240 wide)")
    ap.add_argument("--mode", choices=["rgb555", "rgb565", "256"], default="rgb555",
                    help="pixel mode: rgb555/rgb565 (16bpp) or 256 (8bpp palette)")
    ap.add_argument("--interlaced", action="store_true",
                    help="store alternate scanlines per frame (halves per-frame data)")
    ap.add_argument("--fps", type=float, default=0)
    ap.add_argument("--no-compress", dest="compress", action="store_false")
    ap.add_argument("--no-dither", dest="dither", action="store_false")
    ap.add_argument("--audio", default="pcm", help="pcm or none")
    ap.add_argument("--audio-rate", type=int, default=0)
    ap.add_argument("--audio-8bit", dest="a16", action="store_false")
    ap.add_argument("--ffmpeg", default=os.path.join(_HERE, "ffmpeg"))
    ap.add_argument("--ffprobe", default=os.path.join(_HERE, "ffprobe"))
    a = ap.parse_args(argv)

    w, h = (int(x) for x in a.scale.lower().replace(":", "x").split("x"))
    if a.gba and w == 256:
        w = 240
    bmp = {"rgb565": BMP_16BPP_565, "256": BMP_8BPP_565}.get(a.mode, BMP_16BPP_555)
    pack_rvid(a.ffmpeg, a.ffprobe, a.input, a.output,
              width=w, height=h, bmp_mode=bmp,
              fps=a.fps or None, compress=a.compress, dither=a.dither,
              gba=a.gba, interlaced=a.interlaced, audio=a.audio,
              sample_rate=a.audio_rate, audio_16bit=a.a16)


if __name__ == "__main__":
    _main(sys.argv[1:])
