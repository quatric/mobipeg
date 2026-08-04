#!/usr/bin/env python3
import os
import sys
import subprocess
import glob
import argparse
import math
import struct

# The mobiclip libx264 wrapper hard-caps the keyframe interval at 90 frames
# (~3s @ 30fps), matching retail Wii .mo cadence — see libavcodec/libx264.c.
# We never space keyframes coarser than this, so the .mo keeps periodic
# keyframes the way the Nintendo SDK expects.
MOBICLIP_KEYINT_MAX = 90

# Config
if getattr(sys, 'frozen', False):
    ffmpeg_name = "ffmpeg.exe" if os.name == 'nt' else "ffmpeg"
    FFENC = os.path.join(sys._MEIPASS, ffmpeg_name)
    ffprobe_name = "ffprobe.exe" if os.name == 'nt' else "ffprobe"
    FFPROBE = os.environ.get("FFPROBE", os.path.join(sys._MEIPASS, ffprobe_name))
else:
    FFENC = os.environ.get("FFMPEG", os.path.join(os.path.dirname(os.path.abspath(__file__)), "ffmpeg"))
    FFPROBE = os.environ.get("FFPROBE", os.path.join(os.path.dirname(os.path.abspath(__file__)), "ffprobe"))
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

    .mods and .vx are *always* YCgCo, so short-circuit on extension: the ffprobe
    frame probe can fail silently (e.g. FFPROBE path issues in the frozen GUI
    build), and we must not skip the color transform for those known formats."""
    if os.path.splitext(inp)[1].lower() in (".mods", ".vx"):
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
    parser.add_argument("fmt", nargs="?", default="mo", help="Format (mo, moflex, moflex3d, mods, vx, thp, rvid) — use 'decode' to decode any supported file including .rvid/.h4m/.ty")
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
    parser.add_argument("--outdir", default=DEFAULT_OUTDIR, help="Output directory for generated files")

    parsed = parser.parse_args()
    OUTDIR = parsed.outdir
    
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
    
    if fmt == "decode":
        mode = "decode"
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
    else:
        print(f"unknown format '{fmt}' (decode|mo|moflex|moflex3d|mods|vx|thp|rvid)")
        sys.exit(2)


    if scale_ovr:
        scale = scale_ovr.replace("x", ":")
    elif audio == "vorbis":
        scale = "384:288"
    
    out_directory = parsed.outdir or "."
    if out_directory:
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

    if mode == "decode":
        # In decode mode, audio argument is actually the input file
        inp = audio
        if not os.path.isfile(inp):
            print(f"input not found: {inp}")
            sys.exit(2)
        inp = preprocess_input(inp, OUTDIR)
        watch = f"{OUTDIR}/decoded.mp4"
        print(f">> decoding  {inp}  ->  {watch}")
        ifmt = input_fmt(inp)
        # mods (and any YCgCo-tagged) video needs the inverse-YCgCo filter, else
        # the chroma copies through wrong (green/magenta). Detect via the first
        # decoded frame's colorspace so it also covers non-.mods YCgCo inputs.
        dec_vf = ["-vf", YCGCO_INV_VF] if is_ycgco(inp, ifmt) else []
        if dec_vf:
            print("   (YCgCo input: applying inverse color transform)")
        cmd1 = [FFENC, "-nostdin", "-y", "-loglevel", "error"] + ifmt + ["-i", inp] + dec_vf + ["-c:v", "mpeg4", "-q:v", "3", "-c:a", "aac", watch]
        cmd2 = [FFENC, "-nostdin", "-y", "-loglevel", "error"] + ifmt + ["-i", inp, "-map", "0:v"] + dec_vf + ["-c:v", "mpeg4", "-q:v", "3", watch]
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
        cmd = [FFENC, "-nostdin", "-y"] + input_fmt(inp) + ["-i", inp] + input_fmt(inp2) + ["-i", inp2, "-filter_complex", filter_str, "-map", "[v]"] + aud_opts + ["-c:v", cvc, "-mo_layout", str(layout)] + kf_opts + [container]
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
        run_cmd(base + ["-pass", "1", "-x264-params", f"stats={statfile}", container]) or sys.exit(1)
        print(">> pass 2/2  encode")
        run_cmd(base + ["-pass", "2", "-x264-params", f"stats={statfile}", container]) or sys.exit(1)
        for leftover in (statfile, statfile + ".mbtree", statfile + ".temp"):
            try:
                os.remove(leftover)
            except OSError:
                pass
    else:
        run_cmd(base + [container]) or sys.exit(1)
    
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
