#!/usr/bin/env python3
"""
RocketVideo (.rvid) decoder for mobipeg.

Companion to rvid.py (the encoder).  Parses a v5 .rvid produced by Vid2RVID /
RocketVideoPlayer and reconstructs viewable RGB frames + the PCM audio streams.

Supports every mode the container can carry:
  * bmpMode 0 (8bpp palette, 565-packed entries), 1 (16bpp RGB555),
    2 (16bpp RGB565-in-1555)
  * progressive and interlaced (fields reassembled into a persistent buffer)
  * raw and Nintendo LZ10 compressed frames
  * mono / stereo, 8-bit unsigned and 16-bit signed PCM
  * single-screen and (top+bottom stacked) dual-screen DS videos

GBA files (dualScreen == 2, rom-prepended, %4 split outputs) are out of scope
here; those store the video after a 32KB GBA stub and are handled differently.

CLI:
  rvid_decode.py in.rvid out.mp4                 # mux video+audio via ffmpeg
  rvid_decode.py in.rvid frames/ --frames        # dump frame%05d.png + audio.raw
  rvid_decode.py in.rvid --info                  # print header only
"""
import os
import sys
import struct
import subprocess

import numpy as np

_HERE = os.path.dirname(os.path.abspath(__file__))
_HDR = "<IIIBBBBHBBIII"
_HDR_SZ = struct.calcsize(_HDR)


class RvidHeader:
    __slots__ = ("magic", "ver", "frames", "fps_byte", "vres", "interlaced",
                 "dual_screen", "sample_rate", "audio_16bit", "bmp_mode",
                 "comp_off", "snd_left", "snd_right")

    def __init__(self, buf):
        (self.magic, self.ver, self.frames, self.fps_byte, self.vres,
         self.interlaced, self.dual_screen, self.sample_rate, self.audio_16bit,
         self.bmp_mode, self.comp_off, self.snd_left,
         self.snd_right) = struct.unpack(_HDR, buf[:_HDR_SZ])

    @property
    def play_fps(self):
        base = self.fps_byte & 0x7F
        reduce01 = bool(self.fps_byte & 0x80)
        if base == 0:            # dsRefreshRate sentinel (59.8261Hz)
            return 59.8261
        return base - (0.1 if reduce01 else 0.0)

    def describe(self):
        modes = {0: "8bpp palette(565)", 1: "16bpp RGB555", 2: "16bpp RGB565"}
        return (f"RVID v{self.ver}  frames={self.frames}  "
                f"{'GBA240' if self.dual_screen == 2 else '256'}x"
                f"{self.vres * (2 if self.interlaced else 1)}"
                f"{' x2screen' if self.dual_screen == 1 else ''}  "
                f"{modes.get(self.bmp_mode, '?')}  "
                f"{'interlaced' if self.interlaced else 'progressive'}  "
                f"{'LZ10' if self.comp_off else 'raw'}  "
                f"{self.play_fps:.3f}fps  "
                f"audio={self.sample_rate}Hz "
                f"{'16' if self.audio_16bit else '8'}bit"
                f"{' stereo' if self.snd_right else (' mono' if self.snd_left else ' none')}")


# --------------------------------------------------------------------------- #
# LZ10 decompression (mirrors the compressor in rvid_lz.c)
# --------------------------------------------------------------------------- #
def lz10_decompress(data, offset=0):
    """Decode a Nintendo LZ10 stream starting at data[offset]. Returns
    (bytes, consumed) where consumed is the number of input bytes read."""
    if data[offset] != 0x10:
        raise ValueError(f"not an LZ10 stream (tag=0x{data[offset]:02x})")
    out_size = data[offset + 1] | (data[offset + 2] << 8) | (data[offset + 3] << 16)
    out = bytearray()
    p = offset + 4
    while len(out) < out_size:
        flags = data[p]; p += 1
        for _ in range(8):
            if len(out) >= out_size:
                break
            if flags & 0x80:
                b0 = data[p]; b1 = data[p + 1]; p += 2
                length = (b0 >> 4) + 3
                back = (((b0 & 0xF) << 8) | b1) + 1
                start = len(out) - back
                for k in range(length):
                    out.append(out[start + k])
            else:
                out.append(data[p]); p += 1
            flags = (flags << 1) & 0xFF
    return bytes(out), p - offset


# --------------------------------------------------------------------------- #
# Pixel unpacking (DS 16bpp word -> RGB24), inverse of rvid.py pack_frame()
# --------------------------------------------------------------------------- #
def _unpack_555(words):
    r = (words & 0x1F).astype(np.uint16)
    g = ((words >> 5) & 0x1F).astype(np.uint16)
    b = ((words >> 10) & 0x1F).astype(np.uint16)
    out = np.empty(words.shape + (3,), np.uint8)
    out[..., 0] = (r << 3) | (r >> 2)
    out[..., 1] = (g << 3) | (g >> 2)
    out[..., 2] = (b << 3) | (b >> 2)
    return out


def _unpack_565in1555(words):
    # green6 = bit15 (lsb) | bits5..9 (<<1); r5 = bits0..4; b5 = bits10..14
    r = (words & 0x1F).astype(np.uint16)
    b = ((words >> 10) & 0x1F).astype(np.uint16)
    g6 = (((words >> 15) & 0x1) | (((words >> 5) & 0x1F) << 1)).astype(np.uint16)
    out = np.empty(words.shape + (3,), np.uint8)
    out[..., 0] = (r << 3) | (r >> 2)
    out[..., 1] = (g6 << 2) | (g6 >> 4)
    out[..., 2] = (b << 3) | (b >> 2)
    return out


def unpack_words(words, bmp_mode):
    return _unpack_555(words) if bmp_mode == 1 else _unpack_565in1555(words)


# --------------------------------------------------------------------------- #
# Frame iteration
# --------------------------------------------------------------------------- #
def _screen_width(hdr):
    return 240 if hdr.dual_screen == 2 else 256


def _read_stored_frame(full, hdr, abs_off, width, comp_table):
    """Return the (vres,width,3) RGB field/frame stored at abs_off for one
    screen. comp_table is None for raw frames."""
    vres = hdr.vres
    if hdr.bmp_mode == 0:
        pal_words = np.frombuffer(full, np.uint16, count=256, offset=abs_off).copy()
        pal_rgb = unpack_words(pal_words, 2)          # entries are 565-packed
        idx_off = abs_off + 0x200
        idx_size = width * vres
        if comp_table is None:
            idx = np.frombuffer(full, np.uint8, count=idx_size, offset=idx_off)
        else:
            raw, _ = lz10_decompress(full, idx_off)
            idx = np.frombuffer(raw, np.uint8, count=idx_size)
        rgb = pal_rgb[idx].reshape(vres, width, 3)
    else:
        px = width * vres
        if comp_table is None:
            words = np.frombuffer(full, "<u2", count=px, offset=abs_off)
        else:
            raw, _ = lz10_decompress(full, abs_off)
            words = np.frombuffer(raw, "<u2", count=px)
        rgb = unpack_words(words.reshape(vres, width), hdr.bmp_mode)
    return rgb


def iter_frames(path):
    """Yield full-resolution RGB24 (H,W,3) uint8 frames for a .rvid file."""
    with open(path, "rb") as f:
        full = f.read()
    hdr = RvidHeader(full)
    if hdr.magic != 0x44495652:
        raise ValueError("not a RVID file")
    if hdr.dual_screen == 2:
        raise NotImplementedError("GBA (.rvid.gba) decoding not supported")

    width = _screen_width(hdr)
    nscreens = hdr.dual_screen + 1              # 1 or 2 (top/bottom)
    total = hdr.frames * nscreens
    ftab = np.frombuffer(full, "<u4", count=total, offset=0x200)
    comp_table = 1 if hdr.comp_off else None    # presence flag only

    vres = hdr.vres
    fh = vres * (2 if hdr.interlaced else 1)     # per-screen display height
    persist = [np.zeros((fh, width, 3), np.uint8) for _ in range(nscreens)]

    for i in range(hdr.frames):
        screens = []
        for b in range(nscreens):
            num = (i * nscreens) + b if hdr.dual_screen else i
            field = _read_stored_frame(full, hdr, int(ftab[num]), width, comp_table)
            if hdr.interlaced:
                persist[b][(i & 1)::2] = field       # update alternate scanlines
                screens.append(persist[b].copy())
            else:
                screens.append(field)
        yield np.vstack(screens) if nscreens == 2 else screens[0]


def decode_audio(path):
    """Return (pcm_bytes, sample_rate, channels, is16). pcm is interleaved."""
    with open(path, "rb") as f:
        full = f.read()
    hdr = RvidHeader(full)
    if not hdr.snd_left:
        return b"", 0, 0, False
    end_left = hdr.snd_right if hdr.snd_right else len(full)
    left = full[hdr.snd_left:end_left]
    if hdr.snd_right:
        right = full[hdr.snd_right:]
        n = min(len(left), len(right))
        step = 2 if hdr.audio_16bit else 1
        n -= n % step
        if hdr.audio_16bit:
            l = np.frombuffer(left[:n], "<i2"); r = np.frombuffer(right[:n], "<i2")
            pcm = np.empty(len(l) * 2, "<i2"); pcm[0::2] = l; pcm[1::2] = r
        else:
            l = np.frombuffer(left[:n], np.uint8); r = np.frombuffer(right[:n], np.uint8)
            pcm = np.empty(len(l) * 2, np.uint8); pcm[0::2] = l; pcm[1::2] = r
        return pcm.tobytes(), hdr.sample_rate, 2, bool(hdr.audio_16bit)
    return left, hdr.sample_rate, 1, bool(hdr.audio_16bit)


# --------------------------------------------------------------------------- #
# CLI
# --------------------------------------------------------------------------- #
def _to_video(path, out_path, ffmpeg):
    hdr = RvidHeader(open(path, "rb").read(_HDR_SZ + 0))
    frames = iter_frames(path)
    first = next(frames)
    h, w, _ = first.shape
    fps = hdr.play_fps

    vcmd = [ffmpeg, "-nostdin", "-y", "-v", "error",
            "-f", "rawvideo", "-pix_fmt", "rgb24", "-s", f"{w}x{h}",
            "-r", f"{fps:.4f}", "-i", "pipe:0"]
    pcm, rate, ch, is16 = decode_audio(path)
    apath = None
    if pcm:
        apath = out_path + ".pcm"
        with open(apath, "wb") as af:
            af.write(pcm)
        vcmd += ["-f", "s16le" if is16 else "u8", "-ar", str(rate),
                 "-ac", str(ch), "-i", apath]
    vcmd += ["-c:v", "libx264", "-pix_fmt", "yuv420p", "-crf", "18"]
    if pcm:
        vcmd += ["-c:a", "aac", "-shortest"]
    vcmd += [out_path]

    proc = subprocess.Popen(vcmd, stdin=subprocess.PIPE)
    proc.stdin.write(first.tobytes())
    n = 1
    for fr in frames:
        proc.stdin.write(fr.tobytes()); n += 1
    proc.stdin.close()
    proc.wait()
    if apath:
        os.remove(apath)
    print(f"decoded {n} frames -> {out_path} ({w}x{h}, {fps:.3f}fps)")


def _main(argv):
    import argparse
    ap = argparse.ArgumentParser(description="Decode a RocketVideo .rvid")
    ap.add_argument("input")
    ap.add_argument("output", nargs="?")
    ap.add_argument("--info", action="store_true", help="print header and exit")
    ap.add_argument("--frames", action="store_true",
                    help="dump PNG frames + audio.raw into output dir")
    ap.add_argument("--ffmpeg", default=os.path.join(_HERE, "ffmpeg"))
    a = ap.parse_args(argv)

    hdr = RvidHeader(open(a.input, "rb").read(max(_HDR_SZ, 0x200)))
    print(hdr.describe())
    if a.info:
        return 0
    if not a.output:
        ap.error("output required unless --info")

    if a.frames:
        from PIL import Image
        os.makedirs(a.output, exist_ok=True)
        for i, fr in enumerate(iter_frames(a.input)):
            Image.fromarray(fr).save(os.path.join(a.output, f"frame{i:05d}.png"))
        pcm, rate, ch, is16 = decode_audio(a.input)
        if pcm:
            with open(os.path.join(a.output, "audio.raw"), "wb") as f:
                f.write(pcm)
        print(f"dumped {i + 1} PNG frames"
              + (f" + audio.raw ({rate}Hz {ch}ch)" if pcm else ""))
    else:
        _to_video(a.input, a.output, a.ffmpeg)
    return 0


if __name__ == "__main__":
    sys.exit(_main(sys.argv[1:]))
