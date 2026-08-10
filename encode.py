#!/usr/bin/env python3
import os
import sys
import subprocess
import glob
import argparse
import math
import shlex
import struct
from fractions import Fraction

# The mobiclip libx264 wrapper hard-caps the keyframe interval at 90 frames
# (~3s @ 30fps), matching retail Wii .mo cadence — see libavcodec/libx264.c.
# We never space keyframes coarser than this, so the .mo keeps periodic
# keyframes the way the Nintendo SDK expects.
MOBICLIP_KEYINT_MAX = 90

# Audio-only containers. Every one of these carries GameCube/Wii-family
# DSP-ADPCM except btsnd, which is raw big-endian PCM, and ast, which can do
# either. They share a code path that skips everything video-shaped.
AUDIO_FORMATS = ("dsp", "brstm", "bfstm", "bcstm", "bns", "ast", "btsnd")

# Audio codec each one is written with, keyed by the GUI's "audio" argument.
# "adpcm" means the format's native ADPCM; "pcm" its uncompressed form.
AUDIO_FORMAT_CODECS = {
    "dsp":   {"adpcm": ["-c:a", "adpcm_thp"]},
    "brstm": {"adpcm": ["-c:a", "adpcm_thp"], "pcm": ["-c:a", "pcm_s16be_planar"]},
    "bfstm": {"adpcm": ["-c:a", "adpcm_thp"], "pcm": ["-c:a", "pcm_s16be_planar"]},
    "bcstm": {"adpcm": ["-c:a", "adpcm_thp"], "pcm": ["-c:a", "pcm_s16be_planar"]},
    "bns":   {"adpcm": ["-c:a", "adpcm_thp"]},
    # AST's ADPCM is AFC, a different DSP-ADPCM flavour with a fixed
    # predictor table -- lower quality than adpcm_thp, but what the format
    # takes.
    "ast":   {"adpcm": ["-c:a", "adpcm_afc"], "pcm": ["-c:a", "pcm_s16be_planar"]},
    # The Wii U boot-sound player has no format negotiation: 48 kHz stereo
    # big-endian PCM or nothing.
    "btsnd": {"pcm": ["-c:a", "pcm_s16be", "-ar", "48000", "-ac", "2"]},
}

# .bcstm is read by the bfstm demuxer (same layout, different magic), so it
# has a muxer of its own but no demuxer to name on the way back in.
AUDIO_FORMAT_DEMUXERS = {"bcstm": "bfstm"}

# Config
if getattr(sys, 'frozen', False):
    def _find_frozen_binary(name):
        """Locate a bundled binary regardless of how PyInstaller placed it.

        A onedir macOS .app puts a binary added via `binaries=`/--add-binary
        in Contents/Frameworks, not next to sys._MEIPASS (Contents/MacOS) --
        that split depends on how the build invoked PyInstaller (spec file
        vs CLI flags can disagree), so check every place it could be rather
        than assuming one."""
        exe_dir = os.path.dirname(sys.executable)
        candidates = [
            os.path.join(sys._MEIPASS, name),
            os.path.join(exe_dir, name),
            os.path.join(exe_dir, "..", "Frameworks", name),
            os.path.join(exe_dir, "..", "Resources", name),
        ]
        for c in candidates:
            if os.path.isfile(c):
                return c
        return candidates[0]

    ffmpeg_name = "ffmpeg.exe" if os.name == 'nt' else "ffmpeg"
    FFENC = os.environ.get("FFMPEG", _find_frozen_binary(ffmpeg_name))
    ffprobe_name = "ffprobe.exe" if os.name == 'nt' else "ffprobe"
    FFPROBE = os.environ.get("FFPROBE", _find_frozen_binary(ffprobe_name))
    ffplay_name = "ffplay.exe" if os.name == 'nt' else "ffplay"
    FFPLAY = os.environ.get("FFPLAY", _find_frozen_binary(ffplay_name))
else:
    FFENC = os.environ.get("FFMPEG", os.path.join(os.path.dirname(os.path.abspath(__file__)), "ffmpeg"))
    FFPROBE = os.environ.get("FFPROBE", os.path.join(os.path.dirname(os.path.abspath(__file__)), "ffprobe"))
    FFPLAY = os.environ.get("FFPLAY", os.path.join(os.path.dirname(os.path.abspath(__file__)), "ffplay"))
DEFAULT_OUTDIR = os.environ.get("OUTDIR", "/Volumes/SSD/tmp")


# Inverse of the YCgCo forward transform applied at encode time (see the `ycgco`
# geq in the mods encode path). The mobiclip .mods decoder outputs YCgCo planes
# tagged AVCOL_SPC_YCGCO, which swscale refuses to convert to RGB (error -22);
# copying them straight into a BT601 mpeg4 file misreads the chroma (green/
# magenta cast). Forward: Y=(R+2G+B)/4, Cg=(2G-R-B)/4+128, Co=(R-B)/2+128.
# Inverse: R=Y-cg+co, G=Y+cg, B=Y-cg-co  (cg=Cg-128, co=Co-128). setparams
# relabels the colorspace so swscale chroma-upsamples numerically instead of
# rejecting the YCGCO tag; mergeplanes reinterprets the YUV planes as gbrp.
YCGCO_INV_VF = ("setparams=colorspace=smpte170m:range=pc,format=yuv444p,"
                "mergeplanes=0x000102:gbrp,geq="
                "r='clip(g(X,Y)-(b(X,Y)-128)+(r(X,Y)-128),0,255)':"
                "g='clip(g(X,Y)+(b(X,Y)-128),0,255)':"
                "b='clip(g(X,Y)-(b(X,Y)-128)-(r(X,Y)-128),0,255)'")


def is_ycgco(inp, ifmt):
    """True if the input's decoded video is tagged YCgCo. The mods decoder only
    sets this after decoding a frame, so probe the first decoded frame rather
    than the container header (which reports 'unknown'). Works for any input
    type, so vx/other YCgCo sources get corrected the same as .mods.

    .mods is *always* YCgCo, so short-circuit on extension: the ffprobe frame
    probe can fail silently (e.g. FFPROBE path issues in the frozen GUI build),
    and we must not skip the color transform for that format.

    .vx must NOT be listed here even though its bitstream is equally YCgCo: the
    vx decoder converts to RGB24 itself (see vx.c, avctx->pix_fmt), so applying
    the inverse on top of that transforms already-correct color a second time.
    That wrecks the chroma by a fixed amount at every quantizer, which reads as
    "the quantizer setting does nothing" -- decoded PSNR sat at ~20 dB for every
    QP until this was removed, and is ~43 dB at QP 12 without it."""
    if os.path.splitext(inp)[1].lower() == ".mods":
        return True
    try:
        p = subprocess.run(
            [FFPROBE, "-v", "error"] + ifmt +
            ["-select_streams", "v:0", "-show_entries", "frame=color_space",
             "-read_intervals", "%+#1", "-of", "default=nk=1:nw=1", inp],
            stderr=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
        return "ycgco" in (p.stdout or "").lower()
    except Exception:
        return False


def stereo_layout(inp, ifmt):
    """Return (type, inverted) for a stereoscopic input, else (None, False).

    type is ffmpeg's stereo3d name: "frameseq", "tb" or "sbs". inverted means
    the right eye comes first. MobiClip carries this as the moflex descriptor's
    ImageLayout, which the demuxer exports as AV_PKT_DATA_STEREO3D.

    A file that carries no such tag is indistinguishable from a 2D one here, so
    a probe that cannot run must say so: silently returning "not stereoscopic"
    turns a broken ffprobe into a 3D file that quietly decodes as one flat
    double-width video. Use --stereo to state the layout when it is missing."""
    try:
        p = subprocess.run(
            [FFPROBE, "-v", "error"] + ifmt +
            ["-select_streams", "v:0", "-show_entries",
             "stream_side_data=side_data_type,type,view,inverted",
             "-of", "default=nw=1", inp],
            stderr=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
        out = (p.stdout or "").lower()
        if p.returncode != 0:
            print(f"   (warning: ffprobe failed on {os.path.basename(inp)}, so a 3D "
                  f"layout can't be detected — pass --stereo to set it)\n"
                  f"   {(p.stderr or '').strip().splitlines()[-1] if p.stderr else ''}")
            return None, False
    except Exception as e:
        print(f"   (warning: could not run ffprobe at {FFPROBE} ({e}) — 3D layout "
              "detection is unavailable; pass --stereo to set it)")
        return None, False
    if "stereo 3d" not in out and "stereo3d" not in out:
        return None, False
    if "type=2d" in out or "type=unspecified" in out:
        return None, False
    inverted = "inverted=1" in out or "inverted=true" in out
    if "frame alternate" in out or "frameseq" in out or "framesequence" in out:
        return "frameseq", inverted
    if "top" in out and "bottom" in out:
        return "tb", inverted
    if "side by side" in out or "sidebyside" in out:
        return "sbs", inverted
    return None, False


def resolve_stereo(inp, ifmt, override):
    """(kind, inverted) for the input, letting --stereo overrule detection."""
    if override == "none":
        return None, False
    if override != "auto":
        return override[:-2] if override.endswith("-r") else override, override.endswith("-r")
    return stereo_layout(inp, ifmt)


def stereo_in_mode(kind, inverted):
    """stereo3d's *input* mode name for the given packing.

    The l/r suffix says which eye is stored first, so feeding the 'r' variant
    for an inverted stream makes every stereo3d output mode mean what it says
    (out=sbsl really is left-on-the-left)."""
    return {"frameseq": "a", "tb": "ab", "sbs": "sbs"}[kind] + ("r" if inverted else "l")


def eye_filters(kind, inverted):
    """stereo3d input mode for (left, right) given the packing.

    stereo3d's *l/*r output modes pick a single eye; sidedata=delete drops the
    now-meaningless stereo tag so players don't try to re-interpret the result.
    'inverted' swaps which eye is stored first."""
    src = {"frameseq": "al", "tb": "abl", "sbs": "sbsl"}[kind]
    left, right = f"{src}:ml", f"{src}:mr"
    if inverted:
        left, right = right, left
    return left, right


def input_fmt(path):
    """Force the right demuxer for our custom/ambiguous source inputs.

    `.kwz` and `.thp` auto-detect fine, but forcing them is harmless and keeps
    behaviour explicit; `.ppm` collides with FFmpeg's built-in PNM image
    demuxer, so it *must* be forced. Returns the args to place before the
    source `-i`, or [] for anything else.
    """
    ext = os.path.splitext(path)[1].lower()
    if ext == ".ppm":
        return ["-f", "flipnote_ppm"]
    if ext == ".kwz":
        return ["-f", "kwz"]
    if ext == ".thp":
        return ["-f", "thp"]
    if ext == ".rvid":
        return ["-f", "rvid"]
    if ext == ".h4m":
        # Hudson Soft HVQM4 (.h4m) GameCube/Wii FMV.
        return ["-f", "hvqm4"]
    if ext == ".odh":
        return ["-f", "odh_pipe"]
    # ODH (AJPG) stills ship as bare `.bin` on disc - Super Mario Galaxy's
    # allcompleteimage*.bin are the known ones - so go by the magic, since the
    # extension says nothing.
    try:
        with open(path, "rb") as fh:
            if fh.read(4) == b"AJPG":
                return ["-f", "odh_pipe"]
    except OSError:
        pass
    return []


def preprocess_input(inp, outdir):
    """Transcode-source shim for input files (returns input path unchanged)."""
    return inp


def probe_duration(inp):
    """Return the video stream duration in seconds, or None if it can't be read."""
    try:
        p = subprocess.run(
            [FFPROBE, "-v", "error"] + input_fmt(inp) +
            ["-select_streams", "v:0",
             "-show_entries", "format=duration", "-of", "default=nk=1:nw=1", inp],
            capture_output=True, text=True)
        return float(p.stdout.strip())
    except Exception:
        return None


def probe_fps(inp):
    """Return the source average frame rate as a float, or None."""
    try:
        p = subprocess.run(
            [FFPROBE, "-v", "error"] + input_fmt(inp) +
            ["-select_streams", "v:0",
             "-show_entries", "stream=r_frame_rate", "-of", "default=nk=1:nw=1", inp],
            capture_output=True, text=True)
        num, den = p.stdout.strip().split("/")
        den = float(den)
        return float(num) / den if den else None
    except Exception:
        return None


def probe_audio_rate(inp):
    """Return the source audio sample rate (Hz) as an int, or None."""
    try:
        p = subprocess.run(
            [FFPROBE, "-v", "error"] + input_fmt(inp) +
            ["-select_streams", "a:0",
             "-show_entries", "stream=sample_rate", "-of", "default=nk=1:nw=1", inp],
            capture_output=True, text=True)
        return int(p.stdout.strip())
    except Exception:
        return None


def read_mods_keyframes(path):
    """Parse a .mods file's keyframe table -> sorted list of keyframe frame indices."""
    try:
        with open(path, "rb") as f:
            b = f.read()
        kf_off = struct.unpack_from("<I", b, 0x28)[0]
        kf_cnt = struct.unpack_from("<I", b, 0x2C)[0]
        frames = [struct.unpack_from("<II", b, kf_off + 8 * i)[0] for i in range(kf_cnt)]
        return sorted(set(frames))
    except Exception:
        return []


def even_gop(inp, out_fps, n_keyframes, limit=None):
    """Even GOP size (in output frames) for the clip.

    Returns (gop, count) or None when the clip can't be probed, so the caller
    can fall back to the codec default. Scene-cut detection is left enabled by
    callers, so this acts as a maximum spacing between keyframes.

    n_keyframes > 0 : spread exactly that many keyframes evenly.
    n_keyframes <= 0: auto — use the fewest evenly-spaced keyframes that keep
                      every gap within `limit` (i.e. as coarse as practical
                      without exceeding the encoder's hard keyint cap).
    """
    dur = probe_duration(inp)
    if not dur or not out_fps:
        return None
    total_frames = dur * out_fps
    if n_keyframes and n_keyframes > 0:
        count = n_keyframes
    elif limit:
        count = math.ceil(total_frames / limit)
    else:
        return None
    count = max(1, count)
    gop = max(1, round(total_frames / count))
    if limit:
        gop = min(gop, limit)
    return gop, count

def main():
    parser = argparse.ArgumentParser(description="Encode video/audio for Nintendo formats.")
    parser.add_argument("fmt", nargs="?", default="mo", help="Format (mo, moflex, moflex3d, mods, vx, thp, rvid) — use 'decode' to decode any supported file including .rvid/.h4m/.ty and ODH/AJPG stills, or 'play' to play one back without writing a file")
    parser.add_argument("audio", nargs="?", default="adpcm", help="Audio codec (or input file if fmt=decode)")
    parser.add_argument("input_file", nargs="?", default="", help="Input video/audio file")
    parser.add_argument("input2", nargs="?", default="", help="Second input file (for moflex3d/cia right eye)")

    parser.add_argument("--scale", default="", help="Override scale (e.g. 320x240)")
    parser.add_argument("--layout", default="4", help="MO3D layout (default 4)")
    parser.add_argument("--keyframes", type=int, default=0, help="Number of evenly-spaced keyframes across the clip. 0 (default) = auto: as few as practical while keeping every gap within the encoder's ~90-frame limit. Scene-cut keyframes are still allowed in addition.")
    parser.add_argument("--roundtrip", action="store_true", help="Enable round-trip decoding validation")
    parser.add_argument("--fast-audio", dest="fast_audio", action="store_true", help="Disable the vx_audio long-term-prediction (LTP) lag search. Much faster (~90x on the audio pass) at ~2 dB lower quality. Only affects vx and mods/codebook (SX) audio; other codecs ignore it. Recommended for long clips, where the LTP drain otherwise runs for minutes after the video finishes.")
    env_quant = 0
    try:
        env_quant = int(os.environ.get("QUANT", os.environ.get("QP", 0)))
    except ValueError:
        pass
    parser.add_argument("--quantizer", "--qp", dest="quantizer", type=int, default=env_quant, help="Constant quantizer / QP setting (e.g. 18-28 for MobiClip, 32 for VX, 1-31 for THP qscale). Default 0 = format default (32 for VX, 22 for MobiClip CQP, 2 for THP). Can also be set via QUANT or QP environment variables.")
    parser.add_argument("--audio-rate", dest="audio_rate", type=int, default=0, help="vx/mods codebook: resample audio to this rate (Hz). 0 = keep source. Match the sample rate of the retail clip you're replacing (e.g. 22050 for americ1 cutscenes) — a DS player that sizes its audio buffer for the original rate can stall on a higher-rate stream.")
    parser.add_argument("--fps", dest="fps", default="", help="Force this video frame rate (all formats). Accepts a decimal (e.g. 15) or an exact fraction (e.g. 60000/1001). Empty = keep source (.mo defaults to 30000/1001). The frame rate must usually match the clip you're replacing or the video plays too slow/fast.")
    parser.add_argument("--rvid-mode", dest="rvid_mode", default="rgb555", choices=["rgb555", "rgb565", "256"], help="rvid only: pixel mode. rgb555 (unlimited color, default), rgb565 (max color), or 256 (8bpp palette).")
    parser.add_argument("--no-compress", dest="rvid_no_compress", action="store_true", help="rvid only: store raw 16bpp frames instead of Nintendo LZ10 compression.")
    parser.add_argument("--rvid-interlaced", dest="rvid_interlaced", action="store_true", help="rvid only: store one field per frame (interlaced).")
    parser.add_argument("--rvid-no-dither", dest="rvid_no_dither", action="store_true", help="rvid only: disable the checkerboard ordered dither used when reducing to 16bpp.")
    parser.add_argument("--mobi-qyx", type=int, default=int(os.environ.get("MOBI_QYX", 0)), help="MobiClip: coarsen the quantizer by this many whole qy tiers, i.e. QP + 6*N (0-8, default 0). The quantizer used to be the only way to change quality; plain --qp now works over the format's full 12-63 range, so leave this at 0.")
    parser.add_argument("--mobi-subme", type=int, default=int(os.environ.get("MOBI_SUBME", 0)), help="Subpel/RD refinement level 2-11 (0 = the preset's value, normally 7). 9 is the best quality that this encoder can act on; 10-11 need trellis, which MobiClip has no representation for. Below 6 disables RD mode decision.")
    parser.add_argument("--mobi-intra-only", dest="mobi_intra_only", action="store_true", help="Force every frame to be encoded as an I-frame (keyframe only).")
    parser.add_argument("--mobi-skip", type=int, default=int(os.environ.get("MOBI_SKIP", 512)), help="Macroblock skip decision error threshold (default 512). 0 keeps every residual, which is slightly better at low QP; higher values freeze near-static blocks to stop dither flicker.")
    parser.add_argument("--hq", "--highest-quality", dest="highest_quality", action="store_true", help="Highest quality MobiClip settings: QP 12 (the format's floor), subme 9, and no skip threshold.")

    # --- MobiClip rate control, named after the retail encoder's settings ---
    parser.add_argument("--bitrate", dest="bitrate", default="", help="MobiClip: target bitrate for average-bitrate mode, e.g. 700k. Overrides --qp. (Retail 'Bitrate'.)")
    parser.add_argument("--multipass", dest="multipass", type=int, default=1, choices=[1, 2], help="MobiClip: number of rate-control passes (default 1). 2 runs an analysis pass first and hits the target bitrate more accurately at the same quality. Requires --bitrate. (Retail np1/npn and cbr1/cbrn.)")
    parser.add_argument("--passlog", dest="passlog", default="", help="MobiClip: statistics file for --multipass 2 (default <output>.pass).")
    parser.add_argument("--min-qp", dest="min_qp", type=int, default=0, help="MobiClip: lowest quantizer rate control may use, 12-48. (Retail 'MinQuantizer'.)")
    parser.add_argument("--max-qp", dest="max_qp", type=int, default=0, help="MobiClip: highest quantizer rate control may use, 12-48. (Retail 'MaxQuantizer'.)")
    parser.add_argument("--i-boost", dest="i_boost", type=int, default=-1, help="MobiClip: percent extra bits for I-frames, 0-100 (retail 'IBoostPercent', default 40). Only meaningful with --bitrate.")
    parser.add_argument("--i-threshold", dest="i_threshold", type=int, default=-1, help="MobiClip: scene-cut sensitivity for inserting I-frames, 0-100 (retail 'IThreshold', default 90). 0 disables scene-cut keyframes.")
    parser.add_argument("--buffer-size", dest="buffer_size", default="", help="MobiClip: rate-control buffer size, e.g. 400k (retail 'BufferSize'). Bounds how far the bitrate may drift locally.")
    parser.add_argument("--me", dest="me_method", default="", choices=["dia", "hex", "umh", "esa"], help="MobiClip: motion search method (retail 'MeMethod'). Default is the preset's (hex).")
    parser.add_argument("--8x8dct", dest="dct8x8", type=int, default=-1, choices=[0, 1], help="MobiClip: allow the 8x8 luma transform (default on). 0 forces 4x4-only.")
    parser.add_argument("--ffmpeg-args", dest="ffmpeg_args", default="", help="Extra parameters appended verbatim to the ffmpeg command line, just before the output file. Parsed like a shell word list, so quoting works: --ffmpeg-args '-t 5 -af volume=0.5'. Because ffmpeg lets the last occurrence of an option win, these override the format preset. Applies to encode, decode and play; internal analysis passes (the mods keyframe probe and --roundtrip validation) are left alone.")
    parser.add_argument("--outdir", default=DEFAULT_OUTDIR, help="Output directory for generated files")
    parser.add_argument("-o", "--output", dest="output", default="", help="Output filename (decode mode). Default is the input's own name with a .mp4 extension; for a stereoscopic input the eye is appended, e.g. gs_op_eng_left.mp4.")
    parser.add_argument("--stereo", dest="stereo", default="auto", choices=["auto", "none", "frameseq", "frameseq-r", "tb", "tb-r", "sbs", "sbs-r"], help="Decode/play mode: force the stereoscopic layout instead of reading it from the file. Use this when a 3D file carries no layout descriptor (nothing to detect) so --eyes still splits it. The '-r' variants mean the right eye is stored first. 'none' treats the input as 2D.")
    parser.add_argument("--eyes", dest="eyes", default="both", choices=["both", "left", "right", "packed"], help="Stereoscopic 3DS input: which eye to use. Decode mode: 'both' (default) writes a separate file per eye, 'left'/'right' writes just that one, 'packed' keeps the original interleaved stream untouched. Play mode: 'both' shows the eyes side by side in one window, 'left'/'right' plays a single eye full-window, 'packed' plays the stream as stored (eyes alternating). Ignored for 2D input.")

    parsed = parser.parse_args()
    OUTDIR = parsed.outdir

    # Escape hatch for anything the presets below don't expose.  shlex keeps
    # quoted filter graphs in one piece; ffmpeg resolves duplicates by taking
    # the last one, and these go last, so the user always wins.
    try:
        extra_args = shlex.split(parsed.ffmpeg_args)
    except ValueError as e:
        parser.error(f"--ffmpeg-args is not a valid command line: {e}")

    fmt = parsed.fmt
    audio = parsed.audio
    input_file = parsed.input_file
    input2 = parsed.input2
    scale_ovr = parsed.scale
    roundtrip = parsed.roundtrip
    layout_arg = parsed.layout
    n_keyframes = parsed.keyframes
    fast_audio = parsed.fast_audio
    vx_quant = parsed.quantizer
    audio_rate = parsed.audio_rate
    fps_ovr = parsed.fps.strip()
    rvid_mode = parsed.rvid_mode
    rvid_no_compress = parsed.rvid_no_compress
    rvid_interlaced = parsed.rvid_interlaced
    rvid_no_dither = parsed.rvid_no_dither
    mobi_bitrate = parsed.bitrate.strip()
    mobi_passes = parsed.multipass
    mobi_passlog = parsed.passlog.strip()
    mobi_rc = []          # extra MobiClip rate-control / analysis options
    if mobi_bitrate:
        mobi_rc += ["-b:v", mobi_bitrate]
    if parsed.min_qp:
        mobi_rc += ["-qmin", str(parsed.min_qp)]
    if parsed.max_qp:
        mobi_rc += ["-qmax", str(parsed.max_qp)]
    if parsed.i_boost >= 0:
        # Retail IBoostPercent is "I-frames get N% more bits"; x264 spends that
        # as a QP factor, and ffmpeg passes it in negated (see libx264.c).
        mobi_rc += ["-i_qfactor", f"{-1.0 / (1.0 + parsed.i_boost / 100.0):.4f}"]
    if parsed.i_threshold >= 0:
        mobi_rc += ["-sc_threshold", str(parsed.i_threshold)]
    if parsed.buffer_size:
        mobi_rc += ["-bufsize", parsed.buffer_size]
    if parsed.me_method:
        mobi_rc += ["-me_method", parsed.me_method]
    if parsed.dct8x8 >= 0:
        mobi_rc += ["-8x8dct", str(parsed.dct8x8)]
    # --hq is "as good as MobiClip gets", not "every knob to its extreme":
    # QP 12 is the lowest quantizer the decoder accepts, subme 9 is the highest
    # refinement this encoder can actually act on (10-11 only add trellis, which
    # is forced off), and skip 0 stops near-static blocks being frozen.
    if parsed.highest_quality:
        if parsed.quantizer == 0 and not parsed.bitrate:
            parsed.quantizer = 12
        if parsed.mobi_subme == 0:
            parsed.mobi_subme = 9
        if parsed.mobi_skip == 512:
            parsed.mobi_skip = 0

    # --hq may have set a quantizer above; vx_quant was captured before that,
    # so pick the value up again or --hq's QP never reaches the encoder.
    vx_quant = parsed.quantizer

    if parsed.quantizer > 0:
        os.environ["QUANT"] = str(parsed.quantizer)
        os.environ["QP"] = str(parsed.quantizer)
    if parsed.mobi_qyx:
        os.environ["MOBI_QYX"] = str(parsed.mobi_qyx)
    if parsed.mobi_subme:
        os.environ["MOBI_SUBME"] = str(parsed.mobi_subme)
    if parsed.mobi_intra_only:
        os.environ["MOBI_INTRA_ONLY"] = "1"
    if parsed.mobi_skip != 512:
        os.environ["MOBI_SKIP"] = str(parsed.mobi_skip)

    if parsed.multipass == 2 and not parsed.bitrate:
        parser.error("--multipass 2 needs --bitrate; a constant quantizer has nothing to redistribute")


    # format -> mode | demuxer name | scale (video) | moaud? | cvc
    mode = ""
    dmx = ""
    scale = ""
    moaud = 0
    cvc = ""
    
    if fmt in ("decode", "play"):
        mode = fmt
        dmx = ""
        scale = ""
        moaud = 0
        cvc = ""
    elif fmt == "mo":
        mode, dmx, scale, moaud, cvc = "vid", "mobiclip_mo", "624:352", 1, "mobiclip"
    elif fmt == "moflex":
        mode, dmx, scale, moaud, cvc = "vid", "moflex", "400:240", 1, "mobiclip"
    elif fmt == "moflex3d":
        mode, dmx, scale, moaud, cvc = "vid3d", "moflex", "400:240", 1, "mobiclip"
    elif fmt == "mods":
        mode, dmx, scale, moaud, cvc = "vid", "mods", "256:192", 1, "mobiclip"
    elif fmt == "vx":
        mode, dmx, scale, moaud, cvc = "vid", "vx", "256:192", 0, "vx"
    elif fmt == "thp":
        # GameCube/Wii THP: motion-JPEG video + adpcm_thp (DSP-ADPCM) audio.
        # Not a mobiclip codec, so no -mobiclip/-mo_audio; keeps source size.
        mode, dmx, scale, moaud, cvc = "vid", "thp", "", 0, "mjpeg"
    elif fmt == "rvid":
        # RocketVideo (.rvid) for the DS. Encoded natively by ffmpeg's rvid
        # encoder + muxer (LZ10 compression lives in libavcodec/rvid.c); no
        # external helper. DS screen is 256x192; source is packed to rgb24.
        mode, dmx, scale, moaud, cvc = "vid", "rvid", "256:192", 0, "rvid"
    elif fmt == "dpg":
        # Nintendo DS DPG (MoonShell): MPEG-1 video + MP2 audio in separate
        # regions of the file. DS screen is 256x192.
        mode, dmx, scale, moaud, cvc = "vid", "dpg", "256:192", 0, "mpeg1video"
    elif fmt in AUDIO_FORMATS:
        # Audio-only containers: no video stream, so none of the scaling,
        # keyframe or frame-rate machinery below applies.
        mode, dmx, scale, moaud, cvc = "aud", fmt, "", 0, ""
    else:
        print(f"unknown format '{fmt}' "
              f"(play|decode|mo|moflex|moflex3d|mods|vx|thp|rvid|dpg|"
              f"{'|'.join(AUDIO_FORMATS)})")
        sys.exit(2)


    if scale_ovr:
        scale = scale_ovr.replace("x", ":")
    elif audio == "vorbis":
        scale = "384:288"
    
    out_directory = parsed.outdir or "."
    # Play mode writes nothing, so don't create (or require) an output directory
    # for it -- the default one lives on a removable volume.
    if out_directory and mode != "play":
        os.makedirs(out_directory, exist_ok=True)

    def run_cmd(cmd, check=True, hide_err=False):
        try:
            subprocess.run(cmd, check=check)
        except subprocess.CalledProcessError as e:
            if not hide_err:
                print(f"Command failed: {e}")
            return False
        return True

    def run_ffenc_fallback(cmd1, cmd2):
        if subprocess.run(cmd1).returncode != 0:
            if subprocess.run(cmd2).returncode != 0:
                sys.exit(1)

    if mode == "play":
        # Play back a source file directly, with no intermediate file: our own
        # ffplay links the same decoders as ffmpeg, so .mo/.moflex/.mods/.vx/
        # .thp/.rvid/.h4m play natively even though no system player knows them.
        inp = audio
        if not os.path.isfile(inp):
            print(f"input not found: {inp}")
            sys.exit(2)
        if not os.path.exists(FFPLAY):
            print(f"ffplay not found at {FFPLAY}\n"
                  "It is only built when the tree is configured with SDL2; "
                  "install SDL2 and rebuild, or point FFPLAY at one.")
            sys.exit(2)
        ifmt = input_fmt(inp)
        vf = []
        if is_ycgco(inp, ifmt):
            print("   (YCgCo input: applying inverse color transform)")
            vf.append(YCGCO_INV_VF)

        kind, inverted = resolve_stereo(inp, ifmt, parsed.stereo)
        want = parsed.eyes
        if kind and want != "packed":
            src = stereo_in_mode(kind, inverted)
            if want == "both":
                # One window, both eyes side by side. stereo3d does the
                # de-interleaving itself, so a frame-alternate stream plays at
                # its true per-eye rate instead of flickering between eyes.
                # Name the options explicitly: the positional shorthand isn't
                # accepted by every build's option parser.
                vf.append(f"stereo3d=in={src}:out=sbsl")
                shown = "left | right side by side"
            else:
                vf.append(f"stereo3d=in={src}:out=m{want[0]}")
                shown = f"{want} eye only"
            print(f">> playing  {inp}  (stereoscopic {kind}"
                  f"{', eyes swapped' if inverted else ''}: {shown})")
        else:
            if kind:
                print(f"   (stereoscopic {kind} input played as stored; "
                      "--eyes both shows them side by side)")
            print(f">> playing  {inp}")

        cmd = [FFPLAY, "-hide_banner", "-loglevel", "error",
               "-window_title", os.path.basename(inp)] + ifmt + ["-i", inp]
        if vf:
            cmd += ["-vf", ",".join(vf)]
        cmd += extra_args
        sys.exit(subprocess.run(cmd).returncode)

    if mode == "decode":
        # In decode mode, audio argument is actually the input file
        inp = audio
        if not os.path.isfile(inp):
            print(f"input not found: {inp}")
            sys.exit(2)
        inp = preprocess_input(inp, OUTDIR)
        # Name the output after the input rather than a fixed "decoded.mp4", so
        # decoding several files into one directory doesn't overwrite itself.
        if parsed.output:
            base, ext = os.path.splitext(os.path.basename(parsed.output))
            ext = ext or ".mp4"
            outdir_for = os.path.dirname(parsed.output) or OUTDIR
        else:
            base, ext = os.path.splitext(os.path.basename(inp))[0], ".mp4"
            outdir_for = OUTDIR
        ifmt = input_fmt(inp)
        # mods (and any YCgCo-tagged) video needs the inverse-YCgCo filter, else
        # the chroma copies through wrong (green/magenta). Detect via the first
        # decoded frame's colorspace so it also covers non-.mods YCgCo inputs.
        ycgco = is_ycgco(inp, ifmt)
        if ycgco:
            print("   (YCgCo input: applying inverse color transform)")

        kind, inverted = resolve_stereo(inp, ifmt, parsed.stereo)
        want = parsed.eyes
        if kind and want != "packed":
            # Stereoscopic: 3DS layout 0/1 stores the eyes as alternating
            # frames, so both must be decoded before they can be separated --
            # hence one filter graph that splits and picks an eye per output.
            left_mode, right_mode = eye_filters(kind, inverted)
            wanted = [("left", left_mode), ("right", right_mode)]
            if want in ("left", "right"):
                wanted = [w for w in wanted if w[0] == want]
            pre = f"{YCGCO_INV_VF}," if ycgco else ""
            labels, graph, outs = [], [], []
            graph.append("[0:v]split=%d%s" % (len(wanted), "".join(f"[s{i}]" for i in range(len(wanted)))))
            for i, (name, mode) in enumerate(wanted):
                graph.append(f"[s{i}]{pre}stereo3d={mode},sidedata=delete:type=STEREO3D[{name}]")
                labels.append(name)
                outs.append(os.path.join(outdir_for, f"{base}_{name}{ext}"))
            fc = ";".join(graph)
            print(f">> decoding  {inp}  ({kind}{', eyes swapped' if inverted else ''})")
            for o in outs:
                print(f"   -> {o}")
            cmd = [FFENC, "-nostdin", "-y", "-loglevel", "error"] + ifmt + ["-i", inp,
                   "-filter_complex", fc]
            for name, o in zip(labels, outs):
                # Each eye is a whole movie, so give it the soundtrack too.
                # '0:a?' keeps the mapping optional for a video-only source.
                cmd += ["-map", f"[{name}]", "-map", "0:a?",
                        "-c:v", "mpeg4", "-q:v", "3", "-c:a", "aac"] + extra_args + [o]
            if not run_cmd(cmd, check=True):
                # Fall back to video-only: some sources carry audio we can
                # demux but not decode, and half a movie beats none.
                cmd = [FFENC, "-nostdin", "-y", "-loglevel", "error"] + ifmt + \
                    ["-i", inp, "-filter_complex", fc]
                for name, o in zip(labels, outs):
                    cmd += ["-map", f"[{name}]", "-c:v", "mpeg4", "-q:v", "3"] + extra_args + [o]
                run_cmd(cmd) or sys.exit(1)
            print("\ndecode complete:")
            run_cmd(["ls", "-la"] + outs)
            sys.exit(0)

        watch = os.path.join(outdir_for, base + ext)
        if kind and want == "packed":
            print(f"   (stereoscopic {kind} input kept packed; --eyes both splits it)")
        print(f">> decoding  {inp}  ->  {watch}")
        dec_vf = ["-vf", YCGCO_INV_VF] if ycgco else []
        cmd1 = [FFENC, "-nostdin", "-y", "-loglevel", "error"] + ifmt + ["-i", inp] + dec_vf + ["-c:v", "mpeg4", "-q:v", "3", "-c:a", "aac"] + extra_args + [watch]
        cmd2 = [FFENC, "-nostdin", "-y", "-loglevel", "error"] + ifmt + ["-i", inp, "-map", "0:v"] + dec_vf + ["-c:v", "mpeg4", "-q:v", "3"] + extra_args + [watch]
        run_ffenc_fallback(cmd1, cmd2)
        print("\ndecode complete:")
        run_cmd(["ls", "-la", watch])
        sys.exit(0)

    if mode == "vid3d":
        inp = input_file or "stupi.mp4"
        inp2 = input2 or inp
        if not os.path.isfile(inp):
            print(f"left input not found: {inp}")
            sys.exit(2)
        if not os.path.isfile(inp2):
            print(f"right input not found: {inp2}")
            sys.exit(2)
        inp = preprocess_input(inp, OUTDIR)
        inp2 = preprocess_input(inp2, OUTDIR)

        layout = layout_arg
        stem = f"{OUTDIR}/roundtrip_moflex3d_{audio}"
        container = f"{stem}.moflex"
        watch = f"{stem}.mp4"
        
        eyew, eyeh = scale.split(":")
        
        print(f">> encoding 3D  L={inp}  R={inp2}  ->  {container}  ({eyew}x{eyeh} per eye, side-by-side, layout={layout}, audio={audio})")
        
        filter_str = f"[0:v:0]scale={eyew}:{eyeh}[l];[1:v:0]scale={eyew}:{eyeh}[r];[l][r]hstack=inputs=2[v]"
        kf_opts = []
        kf = even_gop(inp, probe_fps(inp), n_keyframes, limit=MOBICLIP_KEYINT_MAX)
        if kf:
            gop, count = kf
            kf_opts = ["-g", str(gop)]
            print(f"   keyframes: ~{count} evenly spaced (-g {gop}, scene cuts kept)")
        # No audio: don't map an audio stream and skip -mo_audio (the moflex
        # muxer has no "none" value — an absent audio stream is enough).
        if audio == "none":
            aud_opts = ["-an"]
        else:
            aud_opts = ["-map", "0:a:0?", "-mo_audio", audio]
        cmd = [FFENC, "-nostdin", "-y"] + input_fmt(inp) + ["-i", inp] + input_fmt(inp2) + ["-i", inp2, "-filter_complex", filter_str, "-map", "[v]"] + aud_opts + ["-c:v", cvc, "-mo_layout", str(layout)] + kf_opts + extra_args + [container]
        run_cmd(cmd) or sys.exit(1)
        
        if roundtrip:
            print(f">> decoding  {container}  ->  {watch}  (single SBS video, mpeg4)")
            cmd1 = [FFENC, "-y", "-loglevel", "error", "-f", dmx, "-i", container, "-map", "0:0", "-c:v", "mpeg4", "-q:v", "3", "-map", "0:a:0?", "-c:a", "aac", watch]
            cmd2 = [FFENC, "-y", "-loglevel", "error", "-f", dmx, "-i", container, "-map", "0:v", "-c:v", "mpeg4", "-q:v", "3", watch]
            run_ffenc_fallback(cmd1, cmd2)
            
            print("\n3D round-trip complete (frame is left|right side-by-side):")
            run_cmd(["ls", "-la", container, watch])
        else:
            print("\n3D encode complete:")
            run_cmd(["ls", "-la", container])
        print()
        
        p = subprocess.run([FFENC, "-hide_banner", "-f", dmx, "-i", container], stderr=subprocess.PIPE, text=True)
        for line in p.stderr.splitlines():
            if "Stream" in line or "Duration" in line:
                print("  " + line)

        sys.exit(0)

    if mode == "aud":
        inp = input_file
        if not inp or not os.path.isfile(inp):
            print(f"input not found: {inp or '(none)'}")
            sys.exit(2)

        codecs = AUDIO_FORMAT_CODECS[fmt]
        choice = audio if audio in codecs else next(iter(codecs))
        if audio not in codecs:
            print(f"   ({fmt} has no '{audio}' audio; using '{choice}')")

        container = f"{OUTDIR}/roundtrip_{fmt}_{choice}.{fmt}"
        watch     = f"{OUTDIR}/roundtrip_{fmt}_{choice}.wav"

        enc_opts = ["-vn"] + list(codecs[choice])
        # -ar after the codec so it overrides a rate the format pins itself.
        if audio_rate > 0:
            enc_opts.extend(["-ar", str(audio_rate)])

        print(f">> encoding  {inp}  ->  {container}")
        cmd = ([FFENC, "-nostdin", "-y"] + input_fmt(inp) + ["-i", inp]
               + enc_opts + extra_args + [container])
        run_cmd(cmd) or sys.exit(1)

        if roundtrip:
            print(f">> decoding  {container}  ->  {watch}")
            run_cmd([FFENC, "-nostdin", "-y", "-loglevel", "error",
                     "-f", AUDIO_FORMAT_DEMUXERS.get(fmt, fmt), "-i", container,
                     "-c:a", "pcm_s16le", watch]) or sys.exit(1)
            print("\nround-trip complete:")
            run_cmd(["ls", "-la", container, watch])
        else:
            print("\nencode complete:")
            run_cmd(["ls", "-la", container])
        print()

        p = subprocess.run([FFENC, "-hide_banner", "-f",
                            AUDIO_FORMAT_DEMUXERS.get(fmt, fmt), "-i", container],
                           stderr=subprocess.PIPE, text=True)
        for line in p.stderr.splitlines():
            if "Stream" in line or "Duration" in line:
                print("  " + line)
        sys.exit(0)

    # Video round-trip (2D)
    inp = input_file or "stupi.mp4"
    if not os.path.isfile(inp):
        print(f"input not found: {inp}")
        sys.exit(2)
    inp = preprocess_input(inp, OUTDIR)

    stem = f"{OUTDIR}/roundtrip_{fmt}_{audio}"
    container = f"{stem}.{fmt}"
    watch = f"{stem}.mp4"
    
    enc_opts = []
    if moaud == 1:
        if audio == "none":
            # No audio: drop the input's audio stream so only video is muxed.
            enc_opts.append("-an")
            # .mo (A0 tag) and .mods (no_audio=3) have an explicit -mo_audio none
            # value; the moflex muxer has no such enum and just needs the audio
            # stream absent, so don't pass it there.
            if fmt in ("mo", "mods"):
                enc_opts.extend(["-mo_audio", "none"])
        else:
            enc_opts.extend(["-mo_audio", audio])
            if fmt in ("mo", "moflex", "moflex3d"):
                enc_opts.extend(["-map", "0:v", "-map", "0:a?"])
            # SX/codebook audio is produced by its own encoder (trains a per-file
            # codebook over the whole stream), not the muxer's built-in packing.
            if audio == "codebook":
                enc_opts.extend(["-c:a", "vx_audio"])
                if fast_audio:
                    enc_opts.extend(["-ltp", "0"])
                if audio_rate > 0:
                    enc_opts.extend(["-ar", str(audio_rate)])
    if cvc:
        enc_opts.extend(["-c:v", cvc])

    if fmt == "mods":
        enc_opts.extend(["-mobiclip", "2", "-moflex", "0", "-g", "100000"])
        if vx_quant > 0 and not mobi_bitrate:
            enc_opts.extend(["-qp", str(vx_quant)])
        if audio == "fastaudio":
            enc_opts.extend(["-sc_threshold", "0"])
        enc_opts.extend(mobi_rc)
    elif fmt in ["mo", "moflex", "moflex3d"]:
        enc_opts.extend(["-mobiclip", "1"])
        if vx_quant > 0 and not mobi_bitrate:
            enc_opts.extend(["-qp", str(vx_quant)])
        enc_opts.extend(mobi_rc)

        # Evenly-spaced keyframes for the Wii mobiclip formats. mo output is
        # resampled to 30000/1001; moflex keeps source fps. Auto mode uses the
        # fewest keyframes that still stay within the encoder's ~90-frame cap,
        # matching retail cadence; scene-cut keyframes stay enabled on top.
        out_fps = 30000.0 / 1001.0 if fmt == "mo" else probe_fps(inp)
        kf = even_gop(inp, out_fps, n_keyframes, limit=MOBICLIP_KEYINT_MAX)
        if kf:
            gop, count = kf
            enc_opts.extend(["-g", str(gop)])
            print(f"   keyframes: ~{count} evenly spaced (-g {gop}, scene cuts kept)")
    elif fmt == "vx":
        # Same audio codec as .mods codebook/SX (VXDS AFrame == SX bitstream);
        # -an skips it entirely, otherwise it's the muxer's default -c:a.
        # The VXDS container's audio_extradata block is fixed-size (one
        # codebook set), so the format only supports mono audio.
        if audio == "none":
            enc_opts.append("-an")
        else:
            enc_opts.extend(["-c:a", "vx_audio", "-ac", "1"])
            if fast_audio:
                enc_opts.extend(["-ltp", "0"])
            if audio_rate > 0:
                enc_opts.extend(["-ar", str(audio_rate)])
        if n_keyframes and n_keyframes > 0:
            enc_opts.extend(["-keyint", str(n_keyframes)])
        qval = vx_quant if vx_quant > 0 else 32
        enc_opts.extend(["-b:v", "0", "-quantizer", str(qval)])
    elif fmt == "thp":
        # THP video is all-intra MJPEG; audio is adpcm_thp (mono or stereo).
        # -qscale:v 2 is default (1=best..31); THP frames are cheap.
        qval = str(vx_quant) if vx_quant > 0 else "2"
        enc_opts.extend(["-qscale:v", qval])
        if audio == "none":
            enc_opts.append("-an")
        else:
            enc_opts.extend(["-c:a", "adpcm_thp"])
            # Retail THP audio is 32000 Hz, and the console's THP player clocks
            # DSP-ADPCM playback at that rate regardless of the header field, so
            # a 48000 Hz stream plays ~0.67x = low-pitched, slow, and choppy.
            # Resample to 32000 unless the user forces a rate.
            enc_opts.extend(["-ar", str(audio_rate if audio_rate > 0 else 32000)])
    elif fmt == "dpg":
        # MoonShell plays these at 15 fps, which is not one of the frame rates
        # MPEG-1 permits, so the encoder needs -strict -1 to accept it.
        enc_opts.extend(["-strict", "-1"])
        enc_opts.extend(["-b:v", mobi_bitrate or "300k"])
        # One GOP per second: the seek index only holds GOP starts, so a
        # coarser spacing is what makes seeking feel unresponsive on hardware.
        dpg_fps = float(Fraction(fps_ovr)) if fps_ovr else 15.0
        enc_opts.extend(["-g", str(max(1, int(round(dpg_fps))))])
        if audio == "none":
            enc_opts.append("-an")
        else:
            enc_opts.extend(["-c:a", "mp2", "-b:a", "128k"])
            enc_opts.extend(["-ar", str(audio_rate if audio_rate > 0 else 32000)])
    elif fmt == "rvid":
        # RocketVideo: 16bpp (RGB555/565) frames, optional Nintendo LZ10, encoded
        # entirely by ffmpeg's rvid encoder. The encoder consumes rgb24 and packs
        # to 16bpp itself. Audio is raw PCM (the rvid muxer's native stream).
        enc_opts.extend(["-mode", rvid_mode,
                         "-compress", "0" if rvid_no_compress else "1",
                         "-interlaced", "1" if rvid_interlaced else "0",
                         "-dither", "0" if rvid_no_dither else "1"])
        if audio == "none":
            enc_opts.append("-an")
        else:
            # 16-bit PCM (pcm_s16le) is the higher-quality of the two rvid audio
            # stream types; the muxer writes it as the left/right sound stream.
            enc_opts.extend(["-c:a", "pcm_s16le"])
            if audio_rate > 0:
                enc_opts.extend(["-ar", str(audio_rate)])

    # --fps applies to every format: the output frame rate generally has to match
    # the clip being replaced.  A DS VX player clocks video off the audio, so a
    # mismatch plays too slow/fast; .mods DS slots are ~30 fps and a 60 fps stream
    # overruns the ARM9 decode budget; retail THP movies are 29.97.  .mo defaults
    # to 29.97 (what the Wii player expects) but --fps still overrides it.
    fps_filter = ""
    if fps_ovr:
        fps_filter = f"fps={fps_ovr}"
    elif fmt == "mo":
        fps_filter = "fps=30000/1001"
    elif fmt == "dpg":
        # The DS decodes MPEG-1 in software; 15 fps is what MoonShell's own
        # encoders target and what the hardware keeps up with.
        fps_filter = "fps=15"
        
    filters = []
    if scale:
        filters.append(f"scale={scale}")
    if fps_filter:
        filters.append(fps_filter)
        
    if fmt == "mods":
        ycgco = "format=gbrp,geq=g='(r(X,Y)+2*g(X,Y)+b(X,Y))/4':b='(2*g(X,Y)-r(X,Y)-b(X,Y))/4+128':r='(r(X,Y)-b(X,Y))/2+128',mergeplanes=0x000102:yuv444p,format=yuv420p"
        filters.append(ycgco)
        
    vf = []
    if filters:
        vf = ["-vf", ",".join(filters)]
        
    # mods SX/codebook: the DS re-primes its audio decoder at every video
    # keyframe (retail writes an intra aframe there).  Do a fast video-only pass
    # to learn the keyframe frame indices, map each to the audio period position
    # the muxer will give it (cursor = floor(frame * sample_rate / fps / 128)),
    # and tell vx_audio to emit an intra aframe at those periods so the stream
    # matches retail and doesn't stutter on the game's per-keyframe reset.
    if fmt == "mods" and audio == "codebook":
        kf_probe = os.path.join(OUTDIR or ".", ".kf_probe.mods")
        probe_cmd = ([FFENC, "-nostdin", "-y", "-loglevel", "error"]
                     + input_fmt(inp) + ["-i", inp] + vf
                     + ["-an", "-mo_audio", "none", "-c:v", cvc,
                        "-mobiclip", "2", "-moflex", "0", "-g", "100000", kf_probe])
        if subprocess.run(probe_cmd).returncode == 0:
            kfs = read_mods_keyframes(kf_probe)
            sr  = audio_rate if audio_rate > 0 else probe_audio_rate(inp)
            src_fps = probe_fps(inp)
            if kfs and sr and src_fps:
                spf = sr / src_fps
                periods = sorted({int(kf * spf / 128.0) for kf in kfs})
                enc_opts.extend(["-intra_periods", ",".join(str(p) for p in periods)])
                print(f"   mods/sx: {len(periods)} keyframe audio-reset points")
        try:
            os.remove(kf_probe)
        except OSError:
            pass

    fps_disp = f", {fps_filter}" if fps_filter else ""
    scale_disp = scale if scale else "source"

    base = [FFENC, "-nostdin", "-y"] + input_fmt(inp) + ["-i", inp] + vf + enc_opts
    if mobi_passes == 2:
        # libx264 keeps its own statistics file (x264's psz_stat_out); ffmpeg's
        # -passlogfile drives avctx->stats_out, which this encoder never fills,
        # so the file has to be named through -x264-params.
        statfile = mobi_passlog or (container + ".pass")
        print(f">> pass 1/2  analysis  ->  {statfile}")
        run_cmd(base + ["-pass", "1", "-x264-params", f"stats={statfile}"] + extra_args + [container]) or sys.exit(1)
        print(">> pass 2/2  encode")
        run_cmd(base + ["-pass", "2", "-x264-params", f"stats={statfile}"] + extra_args + [container]) or sys.exit(1)
        for leftover in (statfile, statfile + ".mbtree", statfile + ".temp"):
            try:
                os.remove(leftover)
            except OSError:
                pass
    else:
        run_cmd(base + extra_args + [container]) or sys.exit(1)
    
    if roundtrip:
        print(f">> decoding  {container}  ->  {watch}  (single binary, mpeg4)")
        # mods video decodes to YCgCo planes that must be inverted before mpeg4,
        # else the chroma copies through wrong (green/magenta). See YCGCO_INV_VF.
        dec_vf = ["-vf", YCGCO_INV_VF] if fmt == "mods" else []
        cmd1 = [FFENC, "-nostdin", "-y", "-loglevel", "error", "-f", dmx, "-i", container] + dec_vf + ["-c:v", "mpeg4", "-q:v", "3", "-c:a", "aac", watch]
        cmd2 = [FFENC, "-nostdin", "-y", "-loglevel", "error", "-f", dmx, "-i", container, "-map", "0:v"] + dec_vf + ["-c:v", "mpeg4", "-q:v", "3", watch]
        run_ffenc_fallback(cmd1, cmd2)
        
        print("\nround-trip complete:")
        run_cmd(["ls", "-la", container, watch])
    else:
        print("\nencode complete:")
        run_cmd(["ls", "-la", container])
    print()
    
    p = subprocess.run([FFENC, "-hide_banner", "-f", dmx, "-i", container], stderr=subprocess.PIPE, text=True)
    for line in p.stderr.splitlines():
        if "Stream" in line or "Duration" in line:
            print("  " + line)


if __name__ == "__main__":
    main()
