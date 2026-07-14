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
    type, so vx/other YCgCo sources get corrected the same as .mods."""
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
    return []


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
    parser.add_argument("fmt", nargs="?", default="mo", help="Format (mo, moflex, moflex3d, mods, vx, thp) — use 'decode' to decode any supported file including .rvid")
    parser.add_argument("audio", nargs="?", default="adpcm", help="Audio codec (or input file if fmt=decode)")
    parser.add_argument("input_file", nargs="?", default="", help="Input video/audio file")
    parser.add_argument("input2", nargs="?", default="", help="Second input file (for moflex3d/cia right eye)")

    parser.add_argument("--scale", default="", help="Override scale (e.g. 320x240)")
    parser.add_argument("--layout", default="4", help="MO3D layout (default 4)")
    parser.add_argument("--keyframes", type=int, default=0, help="Number of evenly-spaced keyframes across the clip. 0 (default) = auto: as few as practical while keeping every gap within the encoder's ~90-frame limit. Scene-cut keyframes are still allowed in addition.")
    parser.add_argument("--roundtrip", action="store_true", help="Enable round-trip decoding validation")
    parser.add_argument("--fast-audio", dest="fast_audio", action="store_true", help="Disable the vx_audio long-term-prediction (LTP) lag search. Much faster (~90x on the audio pass) at ~2 dB lower quality. Only affects vx and mods/codebook (SX) audio; other codecs ignore it. Recommended for long clips, where the LTP drain otherwise runs for minutes after the video finishes.")
    parser.add_argument("--quantizer", type=int, default=32, help="vx only: constant quantizer (12=best/largest .. 161=worst/smallest). Default 32 keeps per-frame sizes within the retail VX envelope so the DS decode buffer doesn't overflow. Lower values look better but produce larger frames that can crash a DS player with a fixed buffer.")
    parser.add_argument("--audio-rate", dest="audio_rate", type=int, default=0, help="vx/mods codebook: resample audio to this rate (Hz). 0 = keep source. Match the sample rate of the retail clip you're replacing (e.g. 22050 for americ1 cutscenes) — a DS player that sizes its audio buffer for the original rate can stall on a higher-rate stream.")
    parser.add_argument("--fps", dest="fps", default="", help="vx only: force this video frame rate. Accepts a decimal (e.g. 15) or an exact fraction (e.g. 60000/1001). Empty = keep source. A DS VX player clocks video off the audio, so the frame rate must match the clip you're replacing or the video plays too slow/fast.")
    parser.add_argument("--outdir", default=DEFAULT_OUTDIR, help="Output directory for generated files")

    parsed = parser.parse_args()
    
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
    OUTDIR = parsed.outdir


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
    else:
        print(f"unknown format '{fmt}' (decode|mo|moflex|moflex3d|mods|vx|thp)")
        sys.exit(2)


    if scale_ovr:
        scale = scale_ovr.replace("x", ":")
    elif audio == "vorbis":
        scale = "384:288"
    
    os.makedirs(OUTDIR, exist_ok=True)

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
        if audio == "fastaudio":
            enc_opts.extend(["-sc_threshold", "0"])
    elif fmt in ["mo", "moflex", "moflex3d"]:
        enc_opts.extend(["-mobiclip", "1"])

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
        # Use constant-QP, NOT rate control. ffmpeg silently defaults -b:v to
        # 200k, which switches the vx encoder into RC and drives the quantizer
        # down to ~24 -> frames up to ~11.8 KB. No retail VX exceeds ~7.7 KB per
        # frame, and a DS player with a fixed decode buffer overflows on the
        # larger frames (a crash we traced by comparing frame_data_size_max vs
        # retail americ1 files). CQP at a retail-like quantizer keeps every frame
        # inside the retail envelope. Retail uses q ~28-36; default 32.
        enc_opts.extend(["-b:v", "0", "-quantizer", str(vx_quant)])
    elif fmt == "thp":
        # THP video is all-intra MJPEG; audio is adpcm_thp (mono or stereo).
        # -qscale:v 2 is a good default (1=best..31); THP frames are cheap.
        enc_opts.extend(["-qscale:v", "2"])
        if audio == "none":
            enc_opts.append("-an")
        else:
            enc_opts.extend(["-c:a", "adpcm_thp"])
            # Retail THP audio is 32000 Hz, and the console's THP player clocks
            # DSP-ADPCM playback at that rate regardless of the header field, so
            # a 48000 Hz stream plays ~0.67x = low-pitched, slow, and choppy.
            # Resample to 32000 unless the user forces a rate.
            enc_opts.extend(["-ar", str(audio_rate if audio_rate > 0 else 32000)])

    fps_filter = ""
    if fmt == "mo":
        fps_filter = "fps=30000/1001"
    elif fmt in ("vx", "thp", "mods") and fps_ovr:
        # A DS VX player clocks video off the audio, so the output frame rate
        # must match the clip being replaced. Resampling the video to that fps
        # fixes "plays slowly/too fast". Accepts a fraction like 60000/1001.
        # THP likewise: retail movies are 29.97 (30000/1001), so allow forcing it.
        # .mods DS slots are ~30 fps; a 60 fps stream overruns the ARM9 decode
        # budget (plays slow/choppy), so honor --fps here too.
        fps_filter = f"fps={fps_ovr}"
        
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
    print(f">> encoding  {inp}  -> {container}  ({scale_disp}{fps_disp}, audio={audio})")

    cmd = [FFENC, "-nostdin", "-y"] + input_fmt(inp) + ["-i", inp] + vf + enc_opts + [container]
    run_cmd(cmd) or sys.exit(1)
    
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
