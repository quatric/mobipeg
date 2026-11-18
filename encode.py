#!/usr/bin/env python3
# Copyright (c) 2026 quatric - quatricsoftware@gmail.com
import os
import sys
import subprocess
import glob
import argparse
import math
import shlex
import shutil
import tempfile
import atexit
from fractions import Fraction

if hasattr(sys.stdout, "reconfigure"):
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass
if hasattr(sys.stderr, "reconfigure"):
    try:
        sys.stderr.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass

# The mobiclip libx264 wrapper hard-caps the keyframe interval at 90 frames
# (~3s @ 30fps), matching retail Wii .mo cadence — see libavcodec/libx264.c.
# We never space keyframes coarser than this, so the .mo keeps periodic
# keyframes the way the Nintendo SDK expects.
MOBICLIP_KEYINT_MAX = 90

# Audio-only containers. Every one of these carries GameCube/Wii-family
# DSP-ADPCM except btsnd, which is raw big-endian PCM, and ast, which can do
# either. They share a code path that skips everything video-shaped.
AUDIO_FORMATS = (
    "dsp",
    "brstm",
    "bfstm",
    "bcstm",
    "bns",
    "ast",
    "btsnd",
    "wii_photo_m4a",
    "3ds_sound",
)

# Audio codec each one is written with, keyed by the GUI's "audio" argument.
# "adpcm" means the format's native ADPCM; "pcm" its uncompressed form.
AUDIO_FORMAT_CODECS = {
    "dsp": {"adpcm": ["-c:a", "adpcm_thp"]},
    "brstm": {"adpcm": ["-c:a", "adpcm_thp"], "pcm": ["-c:a", "pcm_s16be_planar"]},
    "bfstm": {"adpcm": ["-c:a", "adpcm_thp"], "pcm": ["-c:a", "pcm_s16be_planar"]},
    "bcstm": {"adpcm": ["-c:a", "adpcm_thp"], "pcm": ["-c:a", "pcm_s16be_planar"]},
    "bns": {"adpcm": ["-c:a", "adpcm_thp"]},
    # AST's ADPCM is AFC, a different DSP-ADPCM flavour with a fixed
    # predictor table -- lower quality than adpcm_thp, but what the format
    # takes.
    "ast": {"adpcm": ["-c:a", "adpcm_afc"], "pcm": ["-c:a", "pcm_s16be_planar"]},
    # The Wii U boot-sound player has no format negotiation: 48 kHz stereo
    # big-endian PCM or nothing.
    "btsnd": {"pcm": ["-c:a", "pcm_s16be", "-ar", "48000", "-ac", "2"]},
    # Wii Photo Channel 1.1 and Nintendo 3DS Sound both accept ordinary
    # AAC-LC M4A. 44.1 kHz / 128 kb/s stereo is inside the 3DS Sound's
    # documented 32-48 kHz and 16-320 kb/s limits.
    "wii_photo_m4a": {
        "aac": [
            "-c:a",
            "aac",
            "-profile:a",
            "aac_low",
            "-b:a",
            "128k",
            "-ar",
            "44100",
            "-ac",
            "2",
        ]
    },
    "3ds_sound": {
        "aac": [
            "-c:a",
            "aac",
            "-profile:a",
            "aac_low",
            "-b:a",
            "128k",
            "-ar",
            "44100",
            "-ac",
            "2",
        ]
    },
}

# .bcstm is read by the bfstm demuxer (same layout, different magic), so it
# has a muxer of its own but no demuxer to name on the way back in.
AUDIO_FORMAT_DEMUXERS = {"bcstm": "bfstm", "wii_photo_m4a": "mov", "3ds_sound": "mov"}
AUDIO_FORMAT_EXTENSIONS = {"wii_photo_m4a": "m4a", "3ds_sound": "m4a"}

# Config
if getattr(sys, "frozen", False):

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
                # PyInstaller's --add-binary copy has, on some platforms/
                # versions, dropped the executable bit on the bundled file;
                # a non-executable ffmpeg makes every encode/decode silently
                # fail with "Permission denied" instead of doing anything.
                try:
                    os.chmod(c, os.stat(c).st_mode | 0o111)
                except OSError:
                    pass
                return c
        return candidates[0]

    ffmpeg_name = "ffmpeg.exe" if os.name == "nt" else "ffmpeg"
    FFENC = os.environ.get("FFMPEG", _find_frozen_binary(ffmpeg_name))
    ffprobe_name = "ffprobe.exe" if os.name == "nt" else "ffprobe"
    FFPROBE = os.environ.get("FFPROBE", _find_frozen_binary(ffprobe_name))
    ffplay_name = "ffplay.exe" if os.name == "nt" else "ffplay"
    FFPLAY = os.environ.get("FFPLAY", _find_frozen_binary(ffplay_name))
    vidinjector_name = (
        "VidInjector9002-CLI.exe" if os.name == "nt" else "VidInjector9002-CLI"
    )
    VIDINJECTOR = os.environ.get("VIDINJECTOR", _find_frozen_binary(vidinjector_name))
else:
    FFENC = os.environ.get(
        "FFMPEG", os.path.join(os.path.dirname(os.path.abspath(__file__)), "ffmpeg")
    )
    FFPROBE = os.environ.get(
        "FFPROBE", os.path.join(os.path.dirname(os.path.abspath(__file__)), "ffprobe")
    )
    FFPLAY = os.environ.get(
        "FFPLAY", os.path.join(os.path.dirname(os.path.abspath(__file__)), "ffplay")
    )
    # Dev tree: the CLI lives in its vendored submodule's own build dir, not
    # alongside encode.py (it's only copied next to ffmpeg for the packaged
    # app, via the CI step that builds third_party/VidInjector9000).
    VIDINJECTOR = os.environ.get(
        "VIDINJECTOR",
        os.path.join(
            os.path.dirname(os.path.abspath(__file__)),
            "third_party",
            "VidInjector9000",
            "VidInjector9002-CLI",
            "build",
            "VidInjector9002-CLI",
        ),
    )
DEFAULT_OUTDIR = os.environ.get("OUTDIR", ".")


def report_outputs(*paths):
    """Show completed files without requiring a platform-specific utility."""
    for path in paths:
        print(f"{path} ({os.path.getsize(path):,} bytes)")


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
            [FFPROBE, "-v", "error"]
            + ifmt
            + [
                "-select_streams",
                "v:0",
                "-show_entries",
                "stream_side_data=side_data_type,type,view,inverted",
                "-of",
                "default=nw=1",
                inp,
            ],
            stderr=subprocess.PIPE,
            stdout=subprocess.PIPE,
            text=True,
            encoding="utf-8",
            errors="replace",
        )
        out = (p.stdout or "").lower()
        if p.returncode != 0:
            print(
                f"   (warning: ffprobe failed on {os.path.basename(inp)}, so a 3D "
                f"layout can't be detected — pass --stereo to set it)\n"
                f"   {(p.stderr or '').strip().splitlines()[-1] if p.stderr else ''}"
            )
            return None, False
    except Exception as e:
        print(
            f"   (warning: could not run ffprobe at {FFPROBE} ({e}) — 3D layout "
            "detection is unavailable; pass --stereo to set it)"
        )
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
        return (
            override[:-2] if override.endswith("-r") else override
        ), override.endswith("-r")
    return stereo_layout(inp, ifmt)


def stereo_in_mode(kind, inverted):
    """stereo3d's *input* mode name for the given packing.

    The l/r suffix says which eye is stored first, so feeding the 'r' variant
    for an inverted stream makes every stereo3d output mode mean what it says
    (out=sbsl really is left-on-the-left)."""
    return {"frameseq": "a", "tb": "ab", "sbs": "sbs"}[kind] + (
        "r" if inverted else "l"
    )


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
    if ext == ".vid":
        # GameCube Factor 5 DivX (.vid) container.
        return ["-f", "f5vid"]
    return []


def _decodes_cleanly(path):
    """Quick sanity probe: can our bundled ffmpeg actually decode this input's
    video? Some codecs (e.g. AV1, without libdav1d linked in) fail silently --
    the encode still "succeeds" but writes a 0-frame video track -- so this
    checks a few real frames decode without error instead of trusting ffprobe,
    which only reads the container header.
    """
    try:
        p = subprocess.run(
            [FFENC, "-v", "error", "-nostdin"]
            + input_fmt(path)
            + ["-i", path, "-frames:v", "3", "-f", "null", "-"],
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=30,
        )
        return p.returncode == 0 and not p.stderr.strip()
    except Exception:
        return True  # don't block the encode on a probe failure


def preprocess_input(inp, outdir):
    """For inputs the bundled ffmpeg can't decode cleanly (e.g. AV1 sources --
    no libdav1d linked in, and its native AV1 decoder is broken on at least
    ARM64), use the system ffmpeg *only* to decode+scale the video to a piped
    y4m stream, then hand that to our OWN bundled ffmpeg to encode into a
    small intermediate .mkv -- so the actual encode (and its libx264) is
    always the tested, known-good one this project ships, never whatever
    libx264 the system ffmpeg happens to be linked against. Audio is read
    from the original file directly by the bundled ffmpeg (its native opus/
    vorbis/etc decoders are unaffected by the AV1 problem).

    Every target format here tops out at 624x352 (.mo, the largest), so the
    intermediate is capped at 960px wide: ample margin over the final scale
    with none of the size/time cost of preserving the source's own resolution
    (a full-res copy of a 4K source can be tens of GB for no quality benefit
    downstream).
    """
    if not os.path.isfile(inp) or _decodes_cleanly(inp):
        return inp
    sys_ffmpeg = shutil.which("ffmpeg")
    if not sys_ffmpeg or os.path.realpath(sys_ffmpeg) == os.path.realpath(FFENC):
        print(
            f"warning: {inp} may not decode correctly with the bundled ffmpeg, "
            "and no separate system ffmpeg was found to fall back to"
        )
        return inp
    os.makedirs(outdir, exist_ok=True)
    stem = os.path.splitext(os.path.basename(inp))[0]
    fd, intermediate = tempfile.mkstemp(
        prefix=f".preprocessed_{stem}_", suffix=".mkv", dir=outdir
    )
    os.close(fd)
    print(
        f">> {os.path.basename(inp)} doesn't decode cleanly with the bundled "
        f"ffmpeg (unsupported/broken codec) -- decoding video with system "
        f"ffmpeg and re-encoding with our own libx264 to {intermediate}"
    )
    decode_cmd = [
        sys_ffmpeg,
        "-nostdin",
        "-v",
        "error",
        "-i",
        inp,
        "-map",
        "0:v:0",
        "-vf",
        "scale='min(960,iw)':-2",
        "-f",
        "yuv4mpegpipe",
        "-pix_fmt",
        "yuv420p",
        "-",
    ]
    encode_cmd = (
        [FFENC, "-nostdin", "-y", "-f", "yuv4mpegpipe", "-i", "-"]
        + input_fmt(inp)
        + [
            "-i",
            inp,
            "-map",
            "0:v:0",
            "-map",
            "1:a:0?",
            "-c:v",
            "libx264",
            "-preset",
            "veryfast",
            "-crf",
            "12",
            "-c:a",
            "pcm_s16le",
            intermediate,
        ]
    )
    decoder = None
    succeeded = False
    try:
        decoder = subprocess.Popen(decode_cmd, stdout=subprocess.PIPE)
        try:
            encoder = subprocess.Popen(encode_cmd, stdin=decoder.stdout)
        finally:
            # Release our copy immediately so an early encoder exit sends
            # SIGPIPE to the decoder instead of leaving it blocked forever.
            decoder.stdout.close()
        encoder_status = encoder.wait()
        decoder_status = decoder.wait()
        succeeded = encoder_status == 0 and decoder_status == 0
    except OSError as e:
        print(f"warning: could not run pre-transcode: {e}")
    finally:
        if decoder is not None and decoder.poll() is None:
            decoder.terminate()
            decoder.wait()
        if not succeeded:
            os.unlink(intermediate)
    if not succeeded:
        print("warning: pre-transcode failed, using original input")
        return inp
    # The intermediate is only needed for this run; don't leave it behind.
    atexit.register(lambda: os.path.exists(intermediate) and os.unlink(intermediate))
    return intermediate


def probe_duration(inp):
    """Return the video stream duration in seconds, or None if it can't be read."""
    try:
        p = subprocess.run(
            [FFPROBE, "-v", "error"]
            + input_fmt(inp)
            + [
                "-select_streams",
                "v:0",
                "-show_entries",
                "format=duration",
                "-of",
                "default=nk=1:nw=1",
                inp,
            ],
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
        )
        return float(p.stdout.strip())
    except Exception:
        return None


def probe_dar(inp):
    """Return the first video stream's display aspect ratio, or None."""
    try:
        p = subprocess.run(
            [FFPROBE, "-v", "error"]
            + input_fmt(inp)
            + [
                "-select_streams",
                "v:0",
                "-show_entries",
                "stream=width,height,sample_aspect_ratio",
                "-of",
                "default=nk=1:nw=1",
                inp,
            ],
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
        )
        w, h, sar = p.stdout.split()[:3]
        n, _, d = sar.partition(":")
        ratio = (
            Fraction(int(n), int(d))
            if n.isdigit() and d.isdigit() and int(n) and int(d)
            else 1
        )
        return float(Fraction(int(w), int(h)) * ratio)
    except Exception:
        return None


def half_packed_sar(inp, kind):
    """setsar expression that undoes half-resolution stereo packing, or "".

    stereo3d keeps the packed frame's pixel shape when it picks out an eye,
    which is right for full-resolution packing (3840x1080 SBS) but squashes
    the common half-resolution kind (1920x1080 SBS, two 960x1080 eyes meant
    to be shown 16:9). An eye narrower than 6:5 is taken to be half-width
    SBS, one wider than 5:2 half-height TB."""
    dar = probe_dar(inp)
    if not dar:
        return ""
    if kind == "sbs" and dar / 2 < 1.2:
        return "setsar=sar*2,"
    if kind == "tb" and dar * 2 > 2.5:
        return "setsar=sar/2,"
    return ""


TOPOS_FORMATS = ("topos", "topos3d", "topos_mpeg2", "topos3d_mpeg2")
# Frame colour tags. The encoder's -color_* options lose to the frame's own,
# and the stack filters drop them, so they are set again after stacking.
TOPOS_COLOR = (
    "setparams=range=tv:color_primaries=bt709:color_trc=bt709:colorspace=bt709"
)


def topos_picture_filter(scale):
    """Topos's own picture recipe for one screen (or one eye) of WxH.

    Centre-crop to the screen's shape by display aspect, then scale with a
    spline kernel into BT.709 limited range with square pixels: the player
    shows 400x240 per eye with nearest-neighbour sampling, so the pixels it
    gets are the pixels the viewer sees."""
    w, h = scale.split(":")
    return (
        f"crop='min(iw,ih*{w}/{h}/sar)':'min(ih,iw*sar*{h}/{w})',"
        f"scale={w}:{h}:flags=spline+accurate_rnd+full_chroma_int"
        f":out_color_matrix=bt709:out_range=tv,format=yuv420p,setsar=1,{TOPOS_COLOR}"
    )


def mivf_picture_filter(scale):
    """MIVF picture: the Topos centre-crop and spline scale, but into BT.601
    limited range, which is what the player's Y2R conversion assumes."""
    w, h = scale.split(":")
    return (
        f"crop='min(iw,ih*{w}/{h}/sar)':'min(ih,iw*sar*{h}/{w})',"
        f"scale={w}:{h}:flags=spline+accurate_rnd+full_chroma_int"
        f":out_color_matrix=bt601:out_range=tv,format=yuv420p,setsar=1"
    )


def mivf_audio_rate(fps, want=0):
    """A sample rate that splits into whole MIVF frames: each page carries
    exactly rate / fps samples. 29.97 fps needs a multiple of 30000 Hz."""
    rates = ([want] if want else []) + [44100, 48000, 32000, 30000, 24000, 22050]
    for r in rates:
        spf = Fraction(r) / fps if fps else None
        if spf is not None and spf.denominator == 1 and spf <= 0xFFFF:
            return r
    return None


def _rate_kbps(rate):
    """'2000k' / '2M' / '2000000' -> kbit/s as an int."""
    r = rate.strip().lower()
    mult = {"k": 1, "m": 1000}.get(r[-1:], None)
    return int(float(r[:-1]) * mult) if mult else int(float(r) / 1000)


def topos_codec_opts(fmt, audio, bitrate, quant):
    """(video, audio) options for a Topos .mkv: the video encoder's own
    options (after -c:v) with its colour tags, then the audio and stream
    selection ones.

    H.264 follows the player author's New 3DS settings (High@3.1 for the MVD
    hardware decoder); MPEG-2 the Old 3DS ones (Main, CBR 2 Mbit/s, no
    B-frames, a keyframe a second for the software decoder). --bitrate
    replaces the rate on either; --quantizer replaces H.264's CRF."""
    if fmt.endswith("_mpeg2"):
        kbps = _rate_kbps(bitrate) if bitrate else 2000
        opts = [
            "-b:v",
            f"{kbps}k",
            "-maxrate",
            f"{kbps}k",
            "-bufsize",
            f"{2 * kbps}k",
            "-g",
            "30",
            "-bf",
            "0",
            "-profile:v",
            "main",
            "-level:v",
            "main",
        ]
        default_audio = "mp2"
    else:
        opts = ["-preset", "slow"]
        if bitrate:
            kbps = _rate_kbps(bitrate)
            opts += [
                "-b:v",
                f"{kbps}k",
                "-maxrate",
                f"{2 * kbps}k",
                "-bufsize",
                f"{4 * kbps}k",
            ]
        else:
            opts += ["-crf", str(quant if quant > 0 else 14)]
        opts += [
            "-profile:v",
            "high",
            "-level:v",
            "3.1",
            "-x264-params",
            "aq-mode=3:aq-strength=1.0:qcomp=0.65:ref=4:bframes=4:no-fast-pskip=1",
        ]
        default_audio = "aac"
    opts += [
        "-pix_fmt",
        "yuv420p",
        "-fps_mode",
        "cfr",
        "-color_primaries",
        "bt709",
        "-color_trc",
        "bt709",
        "-colorspace",
        "bt709",
        "-color_range",
        "tv",
    ]
    aopts = ["-sn", "-dn"]
    if audio in ("adpcm", "default", ""):
        audio = default_audio
    if audio == "none":
        aopts.append("-an")
    elif audio == "aac":
        aopts += ["-c:a", "aac", "-b:a", "128k", "-ac", "2", "-ar", "48000"]
    elif audio == "mp2":
        aopts += ["-c:a", "mp2", "-b:a", "96k", "-ac", "2", "-ar", "48000"]
    else:
        print(f"error: {fmt} takes aac, mp2 or none for its audio, not '{audio}'")
        sys.exit(2)
    return opts, aopts


def resolve_output_stem(output, default_name, outdir):
    """Return the output path (without extension) to encode to.

    -o/--output lets the caller pick an exact file instead of always landing
    on the generated roundtrip_<fmt>_<audio> name inside --outdir; the
    extension is still supplied by the caller (it depends on the format, not
    on what the user typed), matching how -o already works for decode."""
    if output:
        base = os.path.splitext(os.path.basename(output))[0]
        out_dir = os.path.dirname(output) or "."
        return os.path.join(out_dir, base)
    return os.path.join(outdir, default_name)


def detect_moflex_block_size(inp):
    """Read the fixed block size a .moflex file was written with.

    The 4C32 sync header at offset 0 stores it as BE16(block_size - 1)
    starting at byte 12 (2 magic + 2 unused/flags + 8 timestamp). Returns
    None if the file doesn't look like moflex, so callers can fall back to
    the retail default instead of guessing."""
    try:
        with open(inp, "rb") as f:
            head = f.read(14)
        if len(head) < 14 or head[0:2] != b"\x4c\x32":
            return None
        return int.from_bytes(head[12:14], "big") + 1
    except Exception:
        return None


def probe_fps(inp):
    """Return the source average frame rate as a float, or None."""
    try:
        p = subprocess.run(
            [FFPROBE, "-v", "error"]
            + input_fmt(inp)
            + [
                "-select_streams",
                "v:0",
                "-show_entries",
                "stream=r_frame_rate",
                "-of",
                "default=nk=1:nw=1",
                inp,
            ],
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
        )
        num, den = p.stdout.strip().split("/")
        den = float(den)
        return float(num) / den if den else None
    except Exception:
        return None


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


def default_jobs(fmt=""):
    """How many chunk encoders to run at once: one per two cores, each on two
    encoder threads.  A Mobiclip picture is small, so one encoder keeps only
    four or five cores busy whatever its thread count; on a 10-core machine
    six two-thread encoders finished a 3-minute .mo in 2.8 s against 4.4 s
    for one encoder on every core."""
    if fmt == "mivf":
        # The M2Y1 encoder runs on one thread: one chunk per core.
        return max(2, os.cpu_count() or 2)
    return max(2, (os.cpu_count() or 2) // 2)


def encode_chunked(chunk_cmd, workdir, ext, n_chunks, jobs):
    """Encode the video as n_chunks independent pieces, `jobs` at a time.

    chunk_cmd(i, dst) is the command that encodes chunk i (0-based) to dst;
    each one decodes and filters its own stretch of the source, so the
    decoding is shared out as well as the encoding and nothing passes
    through this script.  Returns the path of an ffconcat list of the
    encoded chunks, in order, or None if any of them failed (the caller then
    encodes the whole video in one go).  Every chunk starts with a keyframe
    and carries no state over from the one before, so the chunks join by
    plain stream copy."""
    import concurrent.futures

    os.makedirs(workdir, exist_ok=True)
    dsts = [os.path.join(workdir, f"chunk{i:05d}.{ext}") for i in range(n_chunks)]

    def run_chunk(i):
        r = subprocess.run(chunk_cmd(i, dsts[i]))
        return (
            r.returncode == 0
            and os.path.isfile(dsts[i])
            and os.path.getsize(dsts[i]) > 0
        )

    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
        ok = all(pool.map(run_chunk, range(n_chunks)))
    if not ok:
        print("warning: chunked encode failed; encoding in one piece")
        return None
    listfile = os.path.join(workdir, "chunks.ffconcat")
    with open(listfile, "w", encoding="utf-8") as f:
        f.write("ffconcat version 1.0\n")
        for dst in dsts:
            path = os.path.abspath(dst).replace("'", "'\\''")
            f.write(f"file '{path}'\n")
    return listfile


def plan_chunks(inp, jobs, out_fps, lo, hi, unit):
    """(n_chunks, chunk_frames) for a chunked encode, or None to encode in
    one piece.  Chunks run lo..hi seconds, as near dur / jobs as that
    allows, and are a whole number of `unit` frames (the keyframe interval
    the chunks are cut on)."""
    dur = probe_duration(inp)
    if not dur or not out_fps:
        return None
    target = min(hi, max(lo, dur / jobs))
    if dur < 2 * target:
        return None
    chunk_frames = max(1, round(target * out_fps / unit)) * unit
    # The last chunk runs to the end of the video, so it may be up to twice
    # as long: that way it can never start past the end of a video a little
    # shorter than its container.
    return max(1, int(dur * out_fps) // chunk_frames), chunk_frames


def chunked_video(
    inputs,
    filter_args,
    vopts,
    plan,
    out_fps,
    container,
    ext,
    jobs,
    tail=(),
    audio_opts=None,
):
    """Encode the video of `inputs` in the chunks `plan` describes.

    filter_args is the -vf or -filter_complex/-map part of the one-piece
    command and vopts the video encoder's options; every input is seeked to
    the chunk's start, so a two-input 3D encode stays in step.  Given
    audio_opts, the first input's first audio track is encoded with them
    alongside the chunks, into CHUNK_AUDIO next to the list, so the join
    only has to copy streams.  Returns the ffconcat list from
    encode_chunked, or None."""
    n_chunks, chunk_frames = plan
    workdir = tempfile.mkdtemp(
        prefix=".chunks_", dir=os.path.dirname(os.path.abspath(container))
    )
    atexit.register(shutil.rmtree, workdir, True)
    audio_job = None
    if audio_opts is not None and probe_has_audio(inputs[0]):
        audio_job = subprocess.Popen(
            [FFENC, "-nostdin", "-y", "-v", "error"]
            + input_fmt(inputs[0])
            + ["-i", inputs[0], "-map", "0:a:0", "-vn", "-sn", "-dn"]
            + audio_opts
            + [os.path.join(workdir, CHUNK_AUDIO)]
        )

    def chunk_cmd(i, dst):
        # An input -ss seeks exactly (ffmpeg decodes from the keyframe
        # before it and drops what comes earlier), so chunk i starts on
        # output frame i * chunk_frames.
        start = Fraction(i * chunk_frames) / out_fps
        seek = ["-ss", f"{float(start):.6f}"] if i else []
        ins = []
        for path in inputs:
            ins += seek + input_fmt(path) + ["-i", path]
        return (
            [FFENC, "-nostdin", "-y", "-v", "error"]
            + ins
            + filter_args
            + ["-an", "-sn", "-dn"]
            + (["-frames:v", str(chunk_frames)] if i < n_chunks - 1 else [])
            + vopts
            + ["-threads", "2"]
            + list(tail)
            + [dst]
        )

    print(f">> encoding video in {n_chunks} chunks, {jobs} at a time")
    chunk_list = encode_chunked(chunk_cmd, workdir, ext, n_chunks, jobs)
    if audio_job and audio_job.wait() != 0:
        print("warning: audio encode failed; encoding in one piece")
        return None
    return chunk_list


# The audio chunked_video encodes next to its chunks when asked to.
CHUNK_AUDIO = "audio.mka"


def probe_audio_rate(inp):
    """Return the first audio stream's sample rate, or 0 if it can't be read."""
    try:
        p = subprocess.run(
            [FFPROBE, "-v", "error"]
            + input_fmt(inp)
            + [
                "-select_streams",
                "a:0",
                "-show_entries",
                "stream=sample_rate",
                "-of",
                "default=nk=1:nw=1",
            ]
            + [inp],
            capture_output=True,
            text=True,
        )
        return int(p.stdout.split()[0])
    except (OSError, ValueError, IndexError):
        return 0


def bink_audio_rate(inp, want=0):
    """Bink stores the rate in 16 bits; keep the source rate when it fits."""
    if want > 0:
        return want
    rate = probe_audio_rate(inp)
    return rate if 0 < rate <= 48000 else 48000


def probe_has_audio(inp):
    """True if inp has an audio stream."""
    p = subprocess.run(
        [FFPROBE, "-v", "error"]
        + input_fmt(inp)
        + ["-select_streams", "a", "-show_entries", "stream=index", "-of", "csv=p=0"]
        + [inp],
        capture_output=True,
        text=True,
    )
    return bool(p.stdout.strip())


def join_chunks_copy(chunk_list, container, run_cmd):
    """Mux the chunks' video and any CHUNK_AUDIO beside them into container,
    copying both; neither is encoded again."""
    audio = os.path.join(os.path.dirname(chunk_list), CHUNK_AUDIO)
    has_audio = os.path.isfile(audio)
    print(">> joining chunks")
    ok = run_cmd(
        [FFENC, "-nostdin", "-y", "-f", "concat", "-safe", "0", "-i", chunk_list]
        + (["-i", audio, "-map", "0:v", "-map", "1:a"] if has_audio else [])
        + ["-c", "copy", container]
    )
    shutil.rmtree(os.path.dirname(chunk_list), True)
    return ok


def output_fps(inp, fmt, fps_ovr):
    """The frame rate an encode of inp comes out at, or None if unknown."""
    if fps_ovr:
        return Fraction(fps_ovr)
    if fmt == "mo":
        return Fraction(30000, 1001)
    src_fps = probe_fps(inp)
    return Fraction(src_fps).limit_denominator(1001) if src_fps else None


def chunk_shape(fmt):
    """(lo, hi) chunk seconds and the keyframe unit chunks are cut on, for
    the formats whose video can be encoded in chunks; None for the rest."""
    if fmt in ("mo", "moflex", "smoflex", "super_moflex"):
        return 3.0, 6.0, None
    if fmt == "mods":
        # .mods has no periodic keyframes, so every chunk adds one: keep its
        # chunks longer.
        return 10.0, 30.0, None
    if fmt == "mivf":
        # Cut on the 240-frame keyframe interval, as MIVF's own encoder
        # does, so the chunks add no keyframes.
        return 3.0, 10.0, 240
    if fmt in TOPOS_FORMATS:
        if fmt.endswith("_mpeg2"):
            # Cut on its 30-frame GOPs, so the chunks add no keyframes.
            return 3.0, 6.0, 30
        # H.264's own keyframes are 250 frames apart; longer chunks keep the
        # extra ones few, and one encoder already keeps four or five cores
        # busy, so the gain is in running one chunk per job, not many.
        return 5.0, 60.0, None
    return None


def package_cia(parsed):
    """Package an already-encoded .moflex into a 3DS video CIA.

    A separate, explicit step from encoding — build the .moflex first with
    fmt=moflex, then package it here. Wraps the vendored VidInjector9002-CLI
    (third_party/VidInjector9000): writes a throwaway .vi9p parameter file
    pointing at the .moflex, then asks the CLI to build the .cia from it."""
    # fmt=cia has no audio codec, so the natural invocation
    # `encode.py cia clip.moflex -o clip.cia` puts the .moflex path in the
    # "audio" positional slot; fall back to input_file for anyone who passes
    # a placeholder there instead.
    cia_input = parsed.audio if parsed.audio != "adpcm" else parsed.input_file
    if not cia_input:
        print(
            "cia: input .moflex file required, e.g.:\n"
            "  encode.py cia clip.moflex -o clip.cia",
            file=sys.stderr,
        )
        sys.exit(1)
    if not os.path.isfile(cia_input):
        print(f"input not found: {cia_input}", file=sys.stderr)
        sys.exit(1)
    if not cia_input.lower().endswith(".moflex"):
        print(
            f"warning: {cia_input} doesn't look like a .moflex file — "
            f"the CIA will still build, but 3DS may refuse to play it"
        )

    if not os.path.isfile(VIDINJECTOR):
        print(
            f"VidInjector9002-CLI not found at:\n  {VIDINJECTOR}\n"
            f"Build it once:\n"
            f"  git submodule update --init --recursive third_party/VidInjector9000\n"
            f"  cd third_party/VidInjector9000/VidInjector9002-CLI && ./build.sh\n"
            f"(needs cmake and, on macOS, `brew install mbedtls` visible to CMAKE_PREFIX_PATH)",
            file=sys.stderr,
        )
        sys.exit(1)
    try:
        os.chmod(VIDINJECTOR, os.stat(VIDINJECTOR).st_mode | 0o111)
    except OSError:
        pass

    out_stem = resolve_output_stem(parsed.output, "package_cia", parsed.outdir)
    out_cia = os.path.abspath(out_stem + ".cia")
    try:
        os.makedirs(os.path.dirname(out_cia) or ".", exist_ok=True)
    except OSError as e:
        print(
            f"error: cannot create output directory for CIA '{out_cia}': {e}",
            file=sys.stderr,
        )
        sys.exit(1)

    # VidInjector9002-CLI dumps its own working files (romfs/exefs staging,
    # a "VidInjector9000Resources" dir) relative to the process cwd instead
    # of a real temp directory -- run it from an isolated tempdir so it
    # doesn't litter wherever encode.py happened to be launched from.
    cia_input = os.path.abspath(cia_input)
    icon_abs = os.path.abspath(parsed.cia_icon) if parsed.cia_icon else ""
    banner_abs = os.path.abspath(parsed.cia_banner) if parsed.cia_banner else ""

    workdir = tempfile.mkdtemp(prefix="mobipeg_cia_")
    vi9p = os.path.join(workdir, "project.vi9p")

    def vi(*args):
        cmd = [VIDINJECTOR] + [str(a) for a in args]
        try:
            r = subprocess.run(
                cmd,
                capture_output=True,
                text=True,
                encoding="utf-8",
                errors="replace",
                cwd=workdir,
            )
        except FileNotFoundError as e:
            print(f"Command failed: {cmd[0]}: {e}", file=sys.stderr)
            sys.exit(1)
        if r.returncode != 0:
            print(
                f"VidInjector9002-CLI failed: {' '.join(cmd)}\n{r.stdout}{r.stderr}",
                file=sys.stderr,
            )
            sys.exit(1)
        return r.stdout

    try:
        print(f">> packaging  {cia_input}  ->  {out_cia}")
        vi("-new", vi9p)
        vi("-sp", vi9p, 13, cia_input, vi9p)  # STR:MOFLEX(0)
        sname = parsed.cia_title or os.path.splitext(os.path.basename(cia_input))[0]
        lname = parsed.cia_long_title or sname
        vi("-sp", vi9p, 4, sname, vi9p)  # STR:SNAME
        vi("-sp", vi9p, 5, lname, vi9p)  # STR:LNAME
        vi("-sp", vi9p, 6, parsed.cia_publisher, vi9p)  # STR:PUBLISHER
        if icon_abs:
            vi("-sp", vi9p, 2, icon_abs, vi9p)  # STR:ICON
        if banner_abs:
            vi("-sp", vi9p, 1, banner_abs, vi9p)  # STR:BANNER

        if parsed.cia_unique_id:
            vi(
                "-bc",
                vi9p,
                parsed.cia_unique_id,
                sname,
                parsed.cia_product_code,
                out_cia,
            )
        else:
            vi("-bc", vi9p, out_cia)
    finally:
        shutil.rmtree(workdir, ignore_errors=True)

    print("\npackage complete:")
    report_outputs(out_cia)


def main():
    parser = argparse.ArgumentParser(
        description="Encode video/audio for Nintendo formats."
    )
    parser.add_argument(
        "fmt",
        nargs="?",
        default="mo",
        help="Format (mo, moflex, moflex3d, mods, vx, ty, gba_ads, gba_hydrogen, wii_photo, wii_photo_m4a, nintendo_channel, topos, topos3d, topos_mpeg2, topos3d_mpeg2, mivf, bink, 3ds_camera, 3ds_camera3d, 3ds_sound, thp, rvid, dpg, hvqm4, fastvideo, factor5, flipnote, flipnote3d) — use 'decode' to decode any supported file including .gba/.mmstr/.3gp/.m4a/.rvid/.h4m/.vid/.ty, 'play' to play one back without writing a file, or 'cia' to package an already-encoded .moflex into a 3DS video CIA (see --cia-* flags)",
    )
    parser.add_argument(
        "audio",
        nargs="?",
        default="adpcm",
        help="Audio codec (or input file if fmt=decode)",
    )
    parser.add_argument(
        "input_file", nargs="?", default="", help="Input video/audio file"
    )
    parser.add_argument(
        "input2",
        nargs="?",
        default="",
        help="Second input file (right eye for moflex3d / topos3d / 3ds_camera3d). moflex3d and topos3d: omit this to auto-split a single packed stereoscopic source (e.g. a BD3D MKV with StereoMode set) using --stereo or its detected layout.",
    )

    parser.add_argument("--scale", default="", help="Override scale (e.g. 320x240)")
    parser.add_argument("--layout", default="4", help="MO3D layout (default 4)")
    parser.add_argument(
        "--keyframes",
        type=int,
        default=0,
        help="Number of evenly-spaced keyframes across the clip. 0 (default) = auto: as few as practical while keeping every gap within the encoder's ~90-frame limit. Scene-cut keyframes are still allowed in addition.",
    )
    parser.add_argument(
        "--roundtrip", action="store_true", help="Enable round-trip decoding validation"
    )
    parser.add_argument(
        "--fast-audio",
        dest="fast_audio",
        action="store_true",
        help="Disable the vx_audio long-term-prediction (LTP) lag search. Much faster (~90x on the audio pass) at ~2 dB lower quality. Only affects vx and mods/codebook (SX) audio; other codecs ignore it. Recommended for long clips, where the LTP drain otherwise runs for minutes after the video finishes.",
    )
    env_quant = 0
    try:
        env_quant = int(os.environ.get("QUANT", os.environ.get("QP", 0)))
    except ValueError:
        pass
    parser.add_argument(
        "--quantizer",
        "--qp",
        dest="quantizer",
        type=int,
        default=env_quant,
        help="Constant quantizer / QP setting (e.g. 18-28 for MobiClip, 32 for VX, 1-31 for THP/TiVo MPEG-2 qscale). Default 0 = format default (32 for VX, 22 for MobiClip CQP, bitrate mode for TiVo, 2 for THP). Can also be set via QUANT or QP environment variables.",
    )
    parser.add_argument(
        "--audio-rate",
        dest="audio_rate",
        type=int,
        default=0,
        help="Resample audio to this rate (Hz). TiVo defaults to 48000; other formats keep the source unless their preset requires a hardware-specific rate. Match the retail clip when replacing one.",
    )
    parser.add_argument(
        "--fps",
        dest="fps",
        default="",
        help="Force this video frame rate (all formats). Accepts a decimal (e.g. 15) or an exact fraction (e.g. 60000/1001). Empty = keep source (.mo defaults to 30000/1001). The frame rate must usually match the clip you're replacing or the video plays too slow/fast.",
    )
    parser.add_argument(
        "--rvid-mode",
        dest="rvid_mode",
        default="rgb555",
        choices=["rgb555", "rgb565", "256"],
        help="rvid only: pixel mode. rgb555 (unlimited color, default), rgb565 (max color), or 256 (8bpp palette).",
    )
    parser.add_argument(
        "--no-compress",
        dest="rvid_no_compress",
        action="store_true",
        help="rvid only: store raw frames instead of Nintendo LZ10 compression. By default frames are compressed up to the frame rate RocketVideoPlayer can decompress at (Vid2RVID's rule).",
    )
    parser.add_argument(
        "--rvid-interlaced",
        dest="rvid_interlaced",
        action="store_true",
        help="rvid only: store one field per frame (interlaced). By default this is done above the frame rate RocketVideoPlayer can show whole frames at (Vid2RVID's rule).",
    )
    parser.add_argument(
        "--rvid-no-dither",
        dest="rvid_no_dither",
        action="store_true",
        help="rvid only: disable the checkerboard ordered dither used when reducing to 16bpp.",
    )
    parser.add_argument(
        "--flipnote-author",
        dest="flipnote_author",
        default="",
        help="flipnote / flipnote3d: author name stored in the note (max 10 characters, default mobipeg).",
    )
    parser.add_argument(
        "--flipnote-key",
        dest="flipnote_key",
        default="",
        help="flipnote3d only: RSA private key file to sign the note with (PKCS#1 or PKCS#8, DER or PEM). Flipnote Studio 3D only opens notes signed with its own key, which is not included; without a key the note is written unsigned.",
    )
    parser.add_argument(
        "--flipnote-dither",
        dest="flipnote_dither",
        default="4x4",
        choices=["none", "2x2", "4x4", "8x8"],
        help="flipnote3d only: ordered dither matrix used when reducing to the six Flipnote colours (default 4x4; none = nearest colour).",
    )
    parser.add_argument(
        "--flipnote-paper",
        dest="flipnote_paper",
        default="white",
        choices=["white", "black", "red", "yellow", "green", "blue"],
        help="flipnote3d only: paper colour (default white).",
    )
    parser.add_argument(
        "--flipnote-loop",
        dest="flipnote_loop",
        action="store_true",
        help="flipnote3d only: set the note's loop flag.",
    )
    parser.add_argument(
        "--mobi-qyx",
        type=int,
        default=int(os.environ.get("MOBI_QYX", 0)),
        help="MobiClip: coarsen the quantizer by this many whole qy tiers, i.e. QP + 6*N (0-8, default 0). The quantizer used to be the only way to change quality; plain --qp now works over the format's full 12-63 range, so leave this at 0.",
    )
    parser.add_argument(
        "--mobi-subme",
        type=int,
        default=int(os.environ.get("MOBI_SUBME", 0)),
        help="Subpel/RD refinement level 2-11 (0 = the preset's value, normally 7). 9 is the best quality that this encoder can act on; 10-11 need trellis, which MobiClip has no representation for. Below 6 disables RD mode decision.",
    )
    parser.add_argument(
        "--mobi-intra-only",
        dest="mobi_intra_only",
        action="store_true",
        help="Force every frame to be encoded as an I-frame (keyframe only).",
    )
    parser.add_argument(
        "--mobi-skip",
        type=int,
        default=int(os.environ.get("MOBI_SKIP", 512)),
        help="Macroblock skip decision error threshold (default 512). 0 keeps every residual, which is slightly better at low QP; higher values freeze near-static blocks to stop dither flicker.",
    )
    parser.add_argument(
        "--hq",
        "--highest-quality",
        dest="highest_quality",
        action="store_true",
        help="Highest quality MobiClip settings: QP 12 (the format's floor), subme 9, and no skip threshold.",
    )
    parser.add_argument(
        "--mo-block",
        dest="mo_block",
        type=int,
        default=0,
        help="moflex/moflex3d: fixed block size in bytes that every chunk is zero-padded out to (retail=4096, 3DS SDK encoder=2048). Default 0 = auto: when the input file is itself a .moflex, match its own block size so a decode+encode round trip reproduces the original framing byte-for-byte; otherwise 4096.",
    )

    # --- MobiClip rate control, named after the retail encoder's settings ---
    parser.add_argument(
        "--bitrate",
        dest="bitrate",
        default="",
        help="MobiClip: target bitrate for average-bitrate mode, e.g. 700k. Overrides --qp. (Retail 'Bitrate'.)",
    )
    parser.add_argument(
        "--multipass",
        dest="multipass",
        type=int,
        default=1,
        choices=[1, 2],
        help="MobiClip: number of rate-control passes (default 1). 2 runs an analysis pass first and hits the target bitrate more accurately at the same quality. Requires --bitrate. (Retail np1/npn and cbr1/cbrn.)",
    )
    parser.add_argument(
        "--passlog",
        dest="passlog",
        default="",
        help="MobiClip: statistics file for --multipass 2 (default <output>.pass).",
    )
    parser.add_argument(
        "--min-qp",
        dest="min_qp",
        type=int,
        default=0,
        help="MobiClip: lowest quantizer rate control may use, 12-48. (Retail 'MinQuantizer'.)",
    )
    parser.add_argument(
        "--max-qp",
        dest="max_qp",
        type=int,
        default=0,
        help="MobiClip: highest quantizer rate control may use, 12-48. (Retail 'MaxQuantizer'.)",
    )
    parser.add_argument(
        "--i-boost",
        dest="i_boost",
        type=int,
        default=-1,
        help="MobiClip: percent extra bits for I-frames, 0-100 (retail 'IBoostPercent', default 40). Only meaningful with --bitrate.",
    )
    parser.add_argument(
        "--i-threshold",
        dest="i_threshold",
        type=int,
        default=-1,
        help="MobiClip: scene-cut sensitivity for inserting I-frames, 0-100 (retail 'IThreshold', default 90). 0 disables scene-cut keyframes.",
    )
    parser.add_argument(
        "--buffer-size",
        dest="buffer_size",
        default="",
        help="MobiClip: rate-control buffer size, e.g. 400k (retail 'BufferSize'). Bounds how far the bitrate may drift locally.",
    )
    parser.add_argument(
        "--me",
        dest="me_method",
        default="",
        choices=["dia", "hex", "umh", "esa"],
        help="MobiClip: motion search method (retail 'MeMethod'). Default is the preset's (hex).",
    )
    parser.add_argument(
        "--8x8dct",
        dest="dct8x8",
        type=int,
        default=-1,
        choices=[0, 1],
        help="MobiClip: allow the 8x8 luma transform (default on). 0 forces 4x4-only.",
    )
    parser.add_argument(
        "--ffmpeg-args",
        dest="ffmpeg_args",
        default="",
        help="Extra parameters appended verbatim to the ffmpeg command line, just before the output file. Parsed like a shell word list, so quoting works: --ffmpeg-args '-t 5 -af volume=0.5'. Because ffmpeg lets the last occurrence of an option win, these override the format preset. Applies to encode, decode and play; internal analysis passes (the mods keyframe probe and --roundtrip validation) are left alone.",
    )
    parser.add_argument(
        "--jobs",
        type=int,
        default=0,
        help="MobiClip (.mo, .moflex, .mods), Topos and MIVF: encode the video as this many chunks at once, then join them (0 = one per two CPU cores, one per core for MIVF; 1 = encode in one piece). Each chunk is a whole number of keyframe intervals, so the result matches a one-piece encode in size and quality. Two-pass, --ffmpeg-args, 3D and FastAudio .mods always encode in one piece.",
    )

    parser.add_argument(
        "--m2y2",
        action="store_true",
        help="mivf: range-code the video (M2Y2), the same pictures in a smaller file",
    )
    parser.add_argument(
        "--bink-quality",
        dest="bink_quality",
        type=int,
        default=-1,
        choices=range(-1, 11),
        metavar="0-10",
        help="bink: video quality, 10 is highest (default 3). --quantizer 1-15 sets the DCT quantiser directly instead",
    )
    parser.add_argument(
        "--bink-audio-quality",
        dest="bink_audio_quality",
        type=int,
        default=-1,
        choices=range(-1, 11),
        metavar="0-10",
        help="bink: audio quality, 10 is highest (default 6, about 140 kbit/s mono at 44.1 kHz)",
    )

    # --- fmt=cia: package an already-encoded .moflex into a 3DS video CIA. ---
    # A separate, explicit step from encoding on purpose — you build the
    # .moflex first with fmt=moflex, then package it here. Wraps the vendored
    # VidInjector9002-CLI (third_party/VidInjector9000); see VIDINJECTOR above.
    parser.add_argument(
        "--cia-title",
        dest="cia_title",
        default="",
        help="cia only: short title shown on the HOME Menu tile (retail SNAME).",
    )
    parser.add_argument(
        "--cia-long-title",
        dest="cia_long_title",
        default="",
        help="cia only: long title shown in the applet's title bar (retail LNAME). Defaults to --cia-title.",
    )
    parser.add_argument(
        "--cia-publisher",
        dest="cia_publisher",
        default="mobipeg",
        help="cia only: publisher name shown under the title (retail PUBLISHER).",
    )
    parser.add_argument(
        "--cia-icon",
        dest="cia_icon",
        default="",
        help="cia only: path to a PNG/JPEG for the HOME Menu icon (retail ICON). Omit to use VidInjector's built-in placeholder.",
    )
    parser.add_argument(
        "--cia-banner",
        dest="cia_banner",
        default="",
        help="cia only: path to a PNG/JPEG for the applet banner (retail BANNER). Omit to use VidInjector's built-in placeholder.",
    )
    parser.add_argument(
        "--cia-unique-id",
        dest="cia_unique_id",
        default="",
        help="cia only: hex title-ID unique portion, must be C0000-EFFFF to avoid clashing with other titles. Default: VidInjector picks a random one in range.",
    )
    parser.add_argument(
        "--cia-product-code",
        dest="cia_product_code",
        default="VDIJ",
        help="cia only: 4-letter product code suffix (retail default 'VDIJ').",
    )

    parser.add_argument(
        "--outdir", default=DEFAULT_OUTDIR, help="Output directory for generated files"
    )
    parser.add_argument(
        "-o",
        "--output",
        dest="output",
        default="",
        help="Output filename. Decode mode: default is the input's own name with a .mp4 extension; for a stereoscopic input the eye is appended, e.g. gs_op_eng_left.mp4. Encode mode: the extension is still chosen by the target format (this only picks the directory and base name), overriding the generated roundtrip_<fmt>_<audio> name; --outdir is ignored when this is set.",
    )
    parser.add_argument(
        "--stereo",
        dest="stereo",
        default="auto",
        choices=[
            "auto",
            "none",
            "frameseq",
            "frameseq-r",
            "tb",
            "tb-r",
            "sbs",
            "sbs-r",
        ],
        help="Decode/play mode: force the stereoscopic layout instead of reading it from the file. Use this when a 3D file carries no layout descriptor (nothing to detect) so --eyes still splits it. The '-r' variants mean the right eye is stored first. 'none' treats the input as 2D.",
    )
    parser.add_argument(
        "--eyes",
        dest="eyes",
        default="both",
        choices=["both", "left", "right", "packed"],
        help="Stereoscopic 3DS input: which eye to use. Decode mode: 'both' (default) writes a separate file per eye, 'left'/'right' writes just that one, 'packed' keeps the original interleaved stream untouched. Play mode: 'both' shows the eyes side by side in one window, 'left'/'right' plays a single eye full-window, 'packed' plays the stream as stored (eyes alternating). Ignored for 2D input.",
    )

    parser.add_argument(
        "--super",
        "--super-moflex",
        "--smoflex",
        dest="super_moflex",
        action="store_true",
        help="moflex/moflex3d: Enable Super MOFLEX trailer extension (supports secondary audio, multiple subtitles, metadata, poster art)",
    )
    parser.add_argument(
        "--audio2",
        dest="audio2",
        default="",
        help="Super MOFLEX: second audio track file or stream",
    )
    parser.add_argument(
        "--lang", dest="lang", default="", help="Primary audio language tag (e.g. eng)"
    )
    parser.add_argument(
        "--lang2", dest="lang2", default="", help="Second audio language tag (e.g. jpn)"
    )
    parser.add_argument(
        "--srt",
        dest="srt",
        action="append",
        default=[],
        help="Super MOFLEX: subtitle track (.srt file, repeatable)",
    )
    parser.add_argument(
        "--poster",
        dest="poster",
        default="",
        help="Super MOFLEX: poster artwork (image file)",
    )

    parsed = parser.parse_args()
    OUTDIR = (
        (os.path.dirname(parsed.output) or ".")
        if parsed.output
        else (parsed.outdir or ".")
    )

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

    if fmt == "cia":
        package_cia(parsed)
        return

    scale_ovr = parsed.scale
    roundtrip = parsed.roundtrip
    mo_block = parsed.mo_block
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
    mobi_rc = []  # extra MobiClip rate-control / analysis options
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
    if parsed.highest_quality and parsed.fmt == "bink":
        # Bink's quantiser 12 is coarse, not fine: ask for its quality 8 instead.
        if parsed.bink_quality < 0 and parsed.quantizer == 0:
            parsed.bink_quality = 8
    elif parsed.highest_quality:
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
        parser.error(
            "--multipass 2 needs --bitrate; a constant quantizer has nothing to redistribute"
        )

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
    elif fmt in ("moflex", "smoflex", "super_moflex"):
        mode, dmx, scale, moaud, cvc = "vid", "moflex", "400:240", 1, "mobiclip"
        if fmt in ("smoflex", "super_moflex"):
            parsed.super_moflex = True
    elif fmt in ("moflex3d", "smoflex3d", "super_moflex3d"):
        mode, dmx, scale, moaud, cvc = "vid3d", "moflex", "400:240", 1, "mobiclip"
        if fmt in ("smoflex3d", "super_moflex3d"):
            parsed.super_moflex = True
    elif fmt == "mods":
        mode, dmx, scale, moaud, cvc = "vid", "mods", "256:192", 1, "mobiclip"
    elif fmt == "vx":
        mode, dmx, scale, moaud, cvc = "vid", "vx", "256:192", 0, "vx"
    elif fmt in ("gba_ads", "gba_hydrogen"):
        # Majesco GBA Video resource. Both generations use the same ADS video
        # bitstream and .mmstr container; only their blob compressor differs.
        # These are resources suitable for insertion into a compatible ROM,
        # not standalone bootable cartridges.
        mode, dmx, scale, moaud, cvc = "vid", "gbavideo", "240:160", 0, "ads_gba"
    elif fmt == "wii_photo":
        # Wii Photo Channel: ordinary AVI containing Motion JPEG. 640x480 is
        # a practical 4:3 default below its documented 848x480 ceiling.
        mode, dmx, scale, moaud, cvc = "vid", "avi", "640:480", 0, "mjpeg"
    elif fmt == "nintendo_channel":
        # The archived UK Nintendo Channel clips are 3gp6, AVC constrained
        # baseline L2.1, 378x284 at 25 fps, with 32 kHz AAC-LC audio.
        mode, dmx, scale, moaud, cvc = "vid", "3gp", "378:284", 0, "libx264"
    elif fmt in TOPOS_FORMATS:
        # Topos, a 3DS homebrew player: Matroska with H.264 High@3.1 (the New
        # 3DS decodes it in hardware) or MPEG-2 (software decode, for the Old
        # 3DS). It plays 800x240 side-by-side frames as 3D and 400x240 as 2D.
        mode = "vid3d" if fmt.startswith("topos3d") else "vid"
        cvc = "mpeg2video" if fmt.endswith("_mpeg2") else "libx264"
        dmx, scale, moaud = "matroska", "400:240", 0
    elif fmt == "mivf":
        # MIVF, the 3DS homebrew movie player: M2Y1 video at 400x240 with
        # IA4M (mono ADPCM) or PC16 sound packed per frame by the muxer.
        mode, dmx, scale, moaud, cvc = "vid", "mivf", "400:240", 0, "m2y1"
    elif fmt == "bink":
        # Bink 1 (BIKi), the RAD Game Tools movie format PC games play:
        # Bink video and Bink DCT audio. Keeps the source size by default.
        mode, dmx, scale, moaud, cvc = "vid", "bink", "", 0, "binkvideo"
    elif fmt == "3ds_camera":
        # 3DS Camera imports 2D AVI/MJPEG clips at 480x240 / 20 fps with
        # 16 kHz mono IMA-ADPCM. Stereoscopic camera clips have two video
        # tracks; this preset deliberately targets compatible 2D playback.
        mode, dmx, scale, moaud, cvc = "vid", "avi", "480:240", 0, "mjpeg"
    elif fmt == "3ds_camera3d":
        # Native Camera 3D AVI has two synchronized MJPEG tracks: left first,
        # right second. It is not a single packed side-by-side frame.
        mode, dmx, scale, moaud, cvc = "camera3d", "avi", "480:240", 0, "mjpeg"
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
    elif fmt == "ty":
        # TiVo TY Stream: MPEG-2 video with optional MP2/AC-3 audio. Keep the
        # source dimensions and frame rate by default because retail TiVos use
        # several valid raster sizes; --scale/--fps can target a specific box.
        mode, dmx, scale, moaud, cvc = "vid", "ty", "", 0, "mpeg2video"
    elif fmt == "hvqm4":
        # Hudson Soft HVQM4 (.h4m), GameCube/Wii FMV. Encoder+muxer carry
        # video only (no audio support yet), so moaud stays 0 and the audio
        # stream is always dropped further down regardless of --audio.
        mode, dmx, scale, moaud, cvc = "vid", "hvqm4", "320:240", 0, "hvqm4"
    elif fmt == "fastvideo":
        # Gericom's FastVideoDS (.fv): BGR555 DCT video with key and
        # predicted frames, and its own ADPCM (adpcm_ima_fvds) muxed directly
        # rather than through the mobiclip -mo_audio path, so moaud stays 0.
        mode, dmx, scale, moaud, cvc = "vid", "fv", "256:192", 0, "fastvideo"
    elif fmt == "flipnote":
        # Nintendo DSi Flipnote Studio (.ppm): 1-bit 256x192 frames, optional
        # 8192 Hz mono BGM. The muxer signs the file with RSA so the DSi
        # accepts it; ".ppm" also names the PNM image muxer, so -f is forced.
        mode, dmx, scale, moaud, cvc = "vid", "flipnote_ppm", "256:192", 0, "rawvideo"
    elif fmt == "flipnote3d":
        # Nintendo 3DS Flipnote Studio 3D (.kwz): 320x240 frames in the six
        # Flipnote colours on three layers, optional 16364 Hz mono BGM. The
        # muxer does the colour reduction and compresses layers the way the
        # application does; signing needs the application's key (--flipnote-key).
        mode, dmx, scale, moaud, cvc = "vid", "kwz", "320:240", 0, "rawvideo"
    elif fmt in ("factor5", "f5vid"):
        # GameCube Factor 5 DivX (.vid / f5vid container): DivX-compatible
        # MPEG-4 Part 2 ASP video with optional DSP-ADPCM (adpcm_thp) audio.
        mode, dmx, scale, moaud, cvc = "vid", "f5vid", "640:480", 0, "mpeg4"
    elif fmt in AUDIO_FORMATS:
        # Audio-only containers: no video stream, so none of the scaling,
        # keyframe or frame-rate machinery below applies.
        mode, dmx, scale, moaud, cvc = "aud", fmt, "", 0, ""
    else:
        print(
            f"unknown format '{fmt}' "
            f"(play|decode|mo|moflex|moflex3d|mods|vx|ty|gba_ads|gba_hydrogen|wii_photo|wii_photo_m4a|nintendo_channel|topos|topos3d|topos_mpeg2|topos3d_mpeg2|mivf|bink|3ds_camera|3ds_camera3d|3ds_sound|thp|rvid|dpg|hvqm4|fastvideo|factor5|flipnote|flipnote3d|"
            f"{'|'.join(AUDIO_FORMATS)})"
        )
        sys.exit(2)

    if fmt in TOPOS_FORMATS and mobi_passes == 2:
        parser.error(f"--multipass 2 is MobiClip-only; {fmt} encodes in one pass")

    if scale_ovr and fmt not in (
        "3ds_camera",
        "3ds_camera3d",
        "flipnote",
        "flipnote3d",
    ):
        scale = scale_ovr.replace("x", ":")
    elif audio == "vorbis":
        scale = "384:288"

    if parsed.output:
        out_directory = os.path.dirname(parsed.output) or "."
    else:
        out_directory = OUTDIR
    OUTDIR = out_directory
    # Play mode writes nothing, so don't create (or require) an output directory
    # for it.
    if out_directory and mode != "play":
        try:
            os.makedirs(out_directory, exist_ok=True)
        except OSError as e:
            print(
                f"error: cannot create output directory '{out_directory}': {e}",
                file=sys.stderr,
            )
            sys.exit(1)

    def _missing_binary_exit(cmd, e):
        # A bundled ffmpeg/ffprobe that _find_frozen_binary resolved to a path
        # which doesn't actually exist at run time -- most commonly Windows
        # Defender (or another AV) quarantining the packed exe as a false
        # positive, which is a known issue with PyInstaller+ffmpeg bundles.
        print(
            f"error: could not run '{cmd[0]}' ({e}).\n"
            "This binary is bundled with mobipeg-gui -- if it's missing, your "
            "antivirus may have quarantined it (a known false positive for "
            "packaged ffmpeg builds). Check your AV's quarantine/history, "
            "restore it, and add an exclusion for the mobipeg-gui folder."
        )
        sys.exit(2)

    def _handle_binary_error(cmd, e):
        if getattr(e, "errno", None) == 86:
            print(
                f"error: could not run '{cmd[0]}': Bad CPU type in executable.\n"
                "The bundled binary architecture does not match your system architecture."
            )
            sys.exit(2)
        _missing_binary_exit(cmd, e)

    def run_cmd(cmd, check=True, hide_err=False):
        try:
            subprocess.run(cmd, check=check)
        except subprocess.CalledProcessError as e:
            if not hide_err:
                print(f"Command failed: {e}")
            return False
        except OSError as e:
            _handle_binary_error(cmd, e)
        return True

    def run_ffenc_fallback(cmd1, cmd2):
        try:
            if subprocess.run(cmd1).returncode != 0:
                if subprocess.run(cmd2).returncode != 0:
                    sys.exit(1)
        except OSError as e:
            _handle_binary_error(cmd1, e)

    if mode == "play":
        # Play back a source file directly, with no intermediate file: our own
        # ffplay links the same decoders as ffmpeg, so .mo/.moflex/.mods/.vx/
        # .thp/.rvid/.h4m play natively even though no system player knows them.
        inp = audio
        if not os.path.isfile(inp):
            print(f"input not found: {inp}")
            sys.exit(2)
        if not os.path.exists(FFPLAY):
            print(
                f"ffplay not found at {FFPLAY}\n"
                "It is only built when the tree is configured with SDL2; "
                "install SDL2 and rebuild, or point FFPLAY at one."
            )
            sys.exit(2)
        ifmt = input_fmt(inp)
        vf = []

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
            print(
                f">> playing  {inp}  (stereoscopic {kind}"
                f"{', eyes swapped' if inverted else ''}: {shown})"
            )
        else:
            if kind:
                print(
                    f"   (stereoscopic {kind} input played as stored; "
                    "--eyes both shows them side by side)"
                )
            print(f">> playing  {inp}")

        cmd = (
            [
                FFPLAY,
                "-hide_banner",
                "-loglevel",
                "error",
                "-window_title",
                os.path.basename(inp),
            ]
            + ifmt
            + ["-i", inp]
        )
        if vf:
            cmd += ["-vf", ",".join(vf)]
        cmd += extra_args
        try:
            sys.exit(subprocess.run(cmd).returncode)
        except OSError as e:
            _handle_binary_error(cmd, e)

    if mode == "decode":
        # In decode mode, audio argument is actually the input file
        inp = audio
        if not os.path.isfile(inp):
            print(f"input not found: {inp}")
            sys.exit(2)
        source_name = os.path.splitext(os.path.basename(inp))[0]
        inp = preprocess_input(inp, OUTDIR)
        # Name the output after the input rather than a fixed "decoded.mp4", so
        # decoding several files into one directory doesn't overwrite itself.
        if parsed.output:
            base, ext = os.path.splitext(os.path.basename(parsed.output))
            ext = ext or ".mp4"
            outdir_for = os.path.dirname(parsed.output) or OUTDIR
        else:
            base, ext = source_name, ".mp4"
            outdir_for = OUTDIR
        ifmt = input_fmt(inp)
        # The YCgCo bitstreams (.mods MobiClip, .vx) used to reach here tagged
        # AVCOL_SPC_YCGCO and needed an inverse-transform filter bolted on, or
        # the chroma copied through wrong (green/magenta cast). Both decoders
        # now do the conversion themselves and hand out RGB24, so there is
        # nothing left to correct here -- and applying it anyway would
        # transform already-correct color a second time.

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
            pre = ""
            labels, graph, outs = [], [], []
            graph.append(
                "[0:v]split=%d%s"
                % (len(wanted), "".join(f"[s{i}]" for i in range(len(wanted))))
            )
            for i, (name, mode) in enumerate(wanted):
                graph.append(
                    f"[s{i}]{pre}stereo3d={mode},sidedata=delete:type=STEREO3D[{name}]"
                )
                labels.append(name)
                outs.append(os.path.join(outdir_for, f"{base}_{name}{ext}"))
            fc = ";".join(graph)
            print(f">> decoding  {inp}  ({kind}{', eyes swapped' if inverted else ''})")
            for o in outs:
                print(f"   -> {o}")
            cmd = (
                [FFENC, "-nostdin", "-y", "-loglevel", "error"]
                + ifmt
                + ["-i", inp, "-filter_complex", fc]
            )
            for name, o in zip(labels, outs):
                # Each eye is a whole movie, so give it the soundtrack too.
                # '0:a?' keeps the mapping optional for a video-only source.
                cmd += (
                    [
                        "-map",
                        f"[{name}]",
                        "-map",
                        "0:a?",
                        "-c:v",
                        "mpeg4",
                        "-q:v",
                        "3",
                        "-c:a",
                        "aac",
                    ]
                    + extra_args
                    + [o]
                )
            if not run_cmd(cmd, check=True):
                # Fall back to video-only: some sources carry audio we can
                # demux but not decode, and half a movie beats none.
                cmd = (
                    [FFENC, "-nostdin", "-y", "-loglevel", "error"]
                    + ifmt
                    + ["-i", inp, "-filter_complex", fc]
                )
                for name, o in zip(labels, outs):
                    cmd += (
                        ["-map", f"[{name}]", "-c:v", "mpeg4", "-q:v", "3"]
                        + extra_args
                        + [o]
                    )
                run_cmd(cmd) or sys.exit(1)
            print("\ndecode complete:")
            report_outputs(*outs)
            sys.exit(0)

        watch = os.path.join(outdir_for, base + ext)
        if kind and want == "packed":
            print(f"   (stereoscopic {kind} input kept packed; --eyes both splits it)")
        print(f">> decoding  {inp}  ->  {watch}")
        dec_vf = []
        cmd1 = (
            [FFENC, "-nostdin", "-y", "-loglevel", "error"]
            + ifmt
            + ["-i", inp]
            + dec_vf
            + ["-c:v", "mpeg4", "-q:v", "3", "-c:a", "aac"]
            + extra_args
            + [watch]
        )
        cmd2 = (
            [FFENC, "-nostdin", "-y", "-loglevel", "error"]
            + ifmt
            + ["-i", inp, "-map", "0:v"]
            + dec_vf
            + ["-c:v", "mpeg4", "-q:v", "3"]
            + extra_args
            + [watch]
        )
        run_ffenc_fallback(cmd1, cmd2)
        print("\ndecode complete:")
        report_outputs(watch)
        sys.exit(0)

    if mode == "vid3d":
        inp = input_file or "stupi.mp4"
        if not os.path.isfile(inp):
            print(f"left input not found: {inp}")
            sys.exit(2)
        # Two files given (classic mode): left + right eye supplied separately.
        # One file given: treat it as a single packed stereoscopic source (the
        # common shape for a BD3D MKV remux -- one video track carrying both
        # eyes side-by-side/top-bottom, tagged via MKV's StereoMode) and split
        # it into eyes ourselves, the same way decode/play already do.
        dual_file = bool(input2) and os.path.abspath(input2) != os.path.abspath(inp)
        if dual_file:
            inp2 = input2
            if not os.path.isfile(inp2):
                print(f"right input not found: {inp2}")
                sys.exit(2)
            inp = preprocess_input(inp, OUTDIR)
            inp2 = preprocess_input(inp2, OUTDIR)
        else:
            inp = preprocess_input(inp, OUTDIR)
            ifmt = input_fmt(inp)
            kind, inverted = resolve_stereo(inp, ifmt, parsed.stereo)
            if not kind:
                print(
                    f"error: {inp} has no detectable stereoscopic layout (no MKV "
                    "StereoMode / stereo3d side data found), and only one input "
                    "was given.\n"
                    "Either pass --stereo (sbs, sbs-r, tb, tb-r, frameseq, "
                    "frameseq-r) to state the packing, or pass a second input "
                    "file as the right eye."
                )
                sys.exit(2)

        layout = layout_arg
        topos = fmt in TOPOS_FORMATS
        stem = resolve_output_stem(
            parsed.output, f"roundtrip_{fmt if topos else 'moflex3d'}_{audio}", OUTDIR
        )
        container = f"{stem}.{'mkv' if topos else 'moflex'}"
        watch = f"{stem}.mp4"

        eyew, eyeh = scale.split(":")
        tail = f",{TOPOS_COLOR}" if topos else ""
        if topos:
            # Topos tells 3D from 2D by size alone (240 lines, 640+ wide),
            # so each eye only needs its own crop and scale.
            eye = topos_picture_filter(scale)
            if fps_ovr:
                eye += f",fps={fps_ovr}"
            desc = f"{eyew}x{eyeh} per eye, side-by-side, audio={audio}"
        else:
            eye = f"scale={eyew}:{eyeh}"
            desc = (
                f"{eyew}x{eyeh} per eye, side-by-side, layout={layout}, audio={audio}"
            )

        if dual_file:
            print(f">> encoding 3D  L={inp}  R={inp2}  ->  {container}  ({desc})")
            filter_str = (
                f"[0:v:0]{eye}[l];[1:v:0]{eye}[r];[l][r]hstack=inputs=2{tail}[v]"
            )
            in_args = input_fmt(inp) + ["-i", inp] + input_fmt(inp2) + ["-i", inp2]
        else:
            left_mode, right_mode = eye_filters(kind, inverted)
            print(
                f">> encoding 3D  {inp}  ({kind}{', eyes swapped' if inverted else ''})  ->  {container}  ({desc})"
            )
            # Topos crops each eye by its displayed shape, so half-resolution
            # packing has to get its pixel shape back first.
            unhalf = half_packed_sar(inp, kind) if topos else ""
            filter_str = (
                f"[0:v:0]split=2[sl][sr];"
                f"[sl]stereo3d={left_mode},sidedata=delete:type=STEREO3D,{unhalf}{eye}[l];"
                f"[sr]stereo3d={right_mode},sidedata=delete:type=STEREO3D,{unhalf}{eye}[r];"
                f"[l][r]hstack=inputs=2{tail}[v]"
            )
            in_args = input_fmt(inp) + ["-i", inp]

        if topos:
            tv, ta = topos_codec_opts(fmt, audio, mobi_bitrate, vx_quant)
            aud_opts = ([] if audio == "none" else ["-map", "0:a:0?"]) + ta
            codec_opts = ["-c:v", cvc] + tv
        else:
            kf_opts = []
            kf = even_gop(inp, probe_fps(inp), n_keyframes, limit=MOBICLIP_KEYINT_MAX)
            if kf:
                gop, count = kf
                kf_opts = ["-g", str(gop)]
                print(
                    f"   keyframes: ~{count} evenly spaced (-g {gop}, scene cuts kept)"
                )
            # No audio: don't map an audio stream and skip -mo_audio (the moflex
            # muxer has no "none" value — an absent audio stream is enough).
            if audio == "none":
                aud_opts = ["-an"]
            else:
                aud_opts = ["-map", "0:a:0?", "-mo_audio", audio]
            codec_opts = ["-c:v", cvc, "-mo_layout", str(layout)] + kf_opts
        cmd = (
            [FFENC, "-nostdin", "-y"]
            + in_args
            + ["-filter_complex", filter_str, "-map", "[v]"]
            + aud_opts
            + codec_opts
            + extra_args
            + [container]
        )
        chunk_list = None
        jobs = parsed.jobs if parsed.jobs > 0 else default_jobs(fmt)
        if topos and jobs > 1 and not extra_args:
            lo, hi, unit = chunk_shape(fmt)
            out_fps = output_fps(inp, fmt, fps_ovr)
            plan = plan_chunks(inp, jobs, out_fps, lo, hi, unit or 1)
            if plan:
                chunk_list = chunked_video(
                    [inp, inp2] if dual_file else [inp],
                    ["-filter_complex", filter_str, "-map", "[v]"],
                    codec_opts,
                    plan,
                    out_fps,
                    container,
                    "mkv",
                    jobs,
                    audio_opts=None if audio == "none" else ta,
                )
        if chunk_list:
            join_chunks_copy(chunk_list, container, run_cmd) or sys.exit(1)
        else:
            run_cmd(cmd) or sys.exit(1)

        if roundtrip:
            print(f">> decoding  {container}  ->  {watch}  (single SBS video, mpeg4)")
            cmd1 = [
                FFENC,
                "-y",
                "-loglevel",
                "error",
                "-f",
                dmx,
                "-i",
                container,
                "-map",
                "0:0",
                "-c:v",
                "mpeg4",
                "-q:v",
                "3",
                "-map",
                "0:a:0?",
                "-c:a",
                "aac",
                watch,
            ]
            cmd2 = [
                FFENC,
                "-y",
                "-loglevel",
                "error",
                "-f",
                dmx,
                "-i",
                container,
                "-map",
                "0:v",
                "-c:v",
                "mpeg4",
                "-q:v",
                "3",
                watch,
            ]
            run_ffenc_fallback(cmd1, cmd2)

            print("\n3D round-trip complete (frame is left|right side-by-side):")
            report_outputs(container, watch)
        else:
            print("\n3D encode complete:")
            report_outputs(container)
        print()

        p = subprocess.run(
            [FFENC, "-hide_banner", "-f", dmx, "-i", container],
            stderr=subprocess.PIPE,
            text=True,
            encoding="utf-8",
            errors="replace",
        )
        for line in p.stderr.splitlines():
            if "Stream" in line or "Duration" in line:
                print("  " + line)

        sys.exit(0)

    if mode == "camera3d":
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
        stem = resolve_output_stem(
            parsed.output, f"roundtrip_3ds_camera3d_{audio}", OUTDIR
        )
        container = f"{stem}.avi"
        watch = f"{stem}.mp4"
        print(f">> encoding 3DS Camera 3D  L={inp}  R={inp2}  ->  {container}")

        fc = "[0:v:0]scale=480:240,setsar=1[l];" "[1:v:0]scale=480:240,setsar=1[r]"
        cmd = (
            [FFENC, "-nostdin", "-y"]
            + input_fmt(inp)
            + ["-i", inp]
            + input_fmt(inp2)
            + ["-i", inp2, "-filter_complex", fc, "-map", "[l]", "-map", "[r]"]
        )
        if audio == "none":
            cmd += ["-an"]
        else:
            cmd += [
                "-map",
                "0:a:0?",
                "-c:a",
                "adpcm_ima_wav",
                "-ar",
                "16000",
                "-ac",
                "1",
            ]
        cmd += (
            [
                "-c:v",
                "mjpeg",
                "-qscale:v",
                str(vx_quant if vx_quant > 0 else 3),
                "-pix_fmt",
                "yuvj420p",
                "-r",
                "20",
            ]
            + extra_args
            + [container]
        )
        run_cmd(cmd) or sys.exit(1)

        if roundtrip:
            print(f">> decoding  {container}  ->  {watch}  (left|right SBS preview)")
            preview_fc = "[0:v:0][0:v:1]hstack=inputs=2[v]"
            cmd = [
                FFENC,
                "-nostdin",
                "-y",
                "-loglevel",
                "error",
                "-f",
                "avi",
                "-i",
                container,
                "-filter_complex",
                preview_fc,
                "-map",
                "[v]",
                "-map",
                "0:a:0?",
                "-c:v",
                "mpeg4",
                "-q:v",
                "3",
                "-c:a",
                "aac",
                watch,
            ]
            run_cmd(cmd) or sys.exit(1)
        print("\nencode complete:")
        report_outputs(container, *([watch] if roundtrip else []))
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

        out_ext = AUDIO_FORMAT_EXTENSIONS.get(fmt, fmt)
        stem = resolve_output_stem(parsed.output, f"roundtrip_{fmt}_{choice}", OUTDIR)
        container = f"{stem}.{out_ext}"
        watch = f"{stem}.wav"

        enc_opts = ["-vn"] + list(codecs[choice])
        # -ar after the codec so it overrides a rate the format pins itself.
        if audio_rate > 0:
            enc_opts.extend(["-ar", str(audio_rate)])

        print(f">> encoding  {inp}  ->  {container}")
        cmd = (
            [FFENC, "-nostdin", "-y"]
            + input_fmt(inp)
            + ["-i", inp]
            + enc_opts
            + extra_args
            + [container]
        )
        run_cmd(cmd) or sys.exit(1)

        if roundtrip:
            print(f">> decoding  {container}  ->  {watch}")
            run_cmd(
                [
                    FFENC,
                    "-nostdin",
                    "-y",
                    "-loglevel",
                    "error",
                    "-f",
                    AUDIO_FORMAT_DEMUXERS.get(fmt, fmt),
                    "-i",
                    container,
                    "-c:a",
                    "pcm_s16le",
                    watch,
                ]
            ) or sys.exit(1)
            print("\nround-trip complete:")
            report_outputs(container, watch)
        else:
            print("\nencode complete:")
            report_outputs(container)
        print()

        p = subprocess.run(
            [
                FFENC,
                "-hide_banner",
                "-f",
                AUDIO_FORMAT_DEMUXERS.get(fmt, fmt),
                "-i",
                container,
            ],
            stderr=subprocess.PIPE,
            text=True,
            encoding="utf-8",
            errors="replace",
        )
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

    stem = resolve_output_stem(parsed.output, f"roundtrip_{fmt}_{audio}", OUTDIR)
    # The hvqm4 muxer's registered extension is .h4m (the retail extension),
    # not .hvqm4, and the fastvideo muxer is named "fv" not "fastvideo" --
    # ffmpeg picks the muxer from the output filename, so the container name
    # has to match even though the fmt string doesn't.
    out_ext = {
        "hvqm4": "h4m",
        "fastvideo": "fv",
        "gba_ads": "mmstr",
        "gba_hydrogen": "mmstr",
        "wii_photo": "avi",
        "nintendo_channel": "3gp",
        "topos": "mkv",
        "topos_mpeg2": "mkv",
        "3ds_camera": "avi",
        "bink": "bik",
        "factor5": "vid",
        "flipnote": "ppm",
        "flipnote3d": "kwz",
        "smoflex": "moflex",
        "super_moflex": "moflex",
        "smoflex3d": "moflex",
        "super_moflex3d": "moflex",
    }.get(fmt, fmt)
    container = f"{stem}.{out_ext}"
    watch = f"{stem}.mp4"

    enc_opts = []
    extra_inputs = []
    has_super = getattr(parsed, "super_moflex", False) or fmt in (
        "smoflex",
        "super_moflex",
        "smoflex3d",
        "super_moflex3d",
    )
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
            if fmt in (
                "mo",
                "moflex",
                "moflex3d",
                "smoflex",
                "super_moflex",
                "smoflex3d",
                "super_moflex3d",
            ):
                if not has_super or not (parsed.audio2 or parsed.srt or parsed.poster):
                    enc_opts.extend(["-map", "0:v", "-map", "0:a?"])
                else:
                    enc_opts.extend(["-map", "0:v", "-map", "0:a:0?"])
            # SX/codebook audio: the mods muxer runs the vx_audio encoder over
            # the PCM itself, in its trailer.  It trains a codebook over the
            # whole stream anyway, and by then the muxer has seen every video
            # frame, so it knows which periods follow a keyframe and need an
            # intra aframe (the DS re-primes its audio decoder there).
            if audio == "codebook":
                enc_opts.extend(["-c:a", "pcm_s16le"])
                if fast_audio:
                    enc_opts.extend(["-sx_ltp", "0"])
                if audio_rate > 0:
                    enc_opts.extend(["-ar", str(audio_rate)])
    # enc_opts[vopt_start:vopt_end] is the video encoder's own options, which
    # a chunked MobiClip encode hands to the chunk encoders; chunk_unit is the
    # keyframe interval the chunks are cut on.
    vopt_start, vopt_end, chunk_unit = len(enc_opts), None, 1
    if cvc:
        enc_opts.extend(["-c:v", cvc])

    if fmt == "mods":
        enc_opts.extend(["-mobiclip", "2", "-moflex", "0", "-g", "100000"])
        if vx_quant > 0 and not mobi_bitrate:
            enc_opts.extend(["-qp", str(vx_quant)])
        if audio == "fastaudio":
            enc_opts.extend(["-sc_threshold", "0"])
        enc_opts.extend(mobi_rc)
        vopt_end = len(enc_opts)
    elif fmt in [
        "mo",
        "moflex",
        "moflex3d",
        "smoflex",
        "super_moflex",
        "smoflex3d",
        "super_moflex3d",
    ]:
        enc_opts.extend(["-mobiclip", "1"])
        if vx_quant > 0 and not mobi_bitrate:
            enc_opts.extend(["-qp", str(vx_quant)])
        enc_opts.extend(mobi_rc)

        # Evenly-spaced keyframes for the Wii mobiclip formats. mo output is
        # resampled to 30000/1001; moflex keeps source fps. Auto mode uses the
        # fewest keyframes that still stay within the encoder's ~90-frame cap,
        # matching retail cadence; scene-cut keyframes stay enabled on top.
        if fps_ovr:
            out_fps = float(Fraction(fps_ovr))
        else:
            out_fps = 30000.0 / 1001.0 if fmt == "mo" else probe_fps(inp)
        kf = even_gop(inp, out_fps, n_keyframes, limit=MOBICLIP_KEYINT_MAX)
        if kf:
            gop, count = kf
            enc_opts.extend(["-g", str(gop)])
            print(f"   keyframes: ~{count} evenly spaced (-g {gop}, scene cuts kept)")
        chunk_unit = kf[0] if kf else MOBICLIP_KEYINT_MAX
        vopt_end = len(enc_opts)

        if fmt in (
            "moflex",
            "moflex3d",
            "smoflex",
            "super_moflex",
            "smoflex3d",
            "super_moflex3d",
        ):
            # Preserve the source's chunk-padding block size on round trip
            # (2048 = 3DS SDK encoder, 4096 = retail) instead of always
            # falling back to retail's 4096, which would re-pad every chunk
            # to a different boundary than the original file used.
            block = mo_block or detect_moflex_block_size(inp) or 4096
            enc_opts.extend(["-mo_block", str(block)])
            if has_super:
                enc_opts.extend(["-super_moflex", "1"])
                if parsed.lang:
                    enc_opts.extend(["-metadata:s:a:0", f"language={parsed.lang}"])
                input_cursor = 1
                if parsed.audio2:
                    extra_inputs.extend(["-i", parsed.audio2])
                    enc_opts.extend(
                        ["-map", f"{input_cursor}:a:0", "-c:a:1", "pcm_s16le"]
                    )
                    if parsed.lang2:
                        enc_opts.extend(["-metadata:s:a:1", f"language={parsed.lang2}"])
                    input_cursor += 1
                for srt_index, srt_path in enumerate(parsed.srt):
                    extra_inputs.extend(["-i", srt_path])
                    enc_opts.extend(["-map", f"{input_cursor}:s:0", "-c:s", "subrip"])
                    parts = os.path.basename(srt_path).split(".")
                    if len(parts) >= 3 and len(parts[-2]) in (2, 3):
                        enc_opts.extend(
                            [f"-metadata:s:s:{srt_index}", f"language={parts[-2]}"]
                        )
                    input_cursor += 1
                if parsed.poster:
                    extra_inputs.extend(["-i", parsed.poster])
                    enc_opts.extend(
                        [
                            "-map",
                            f"{input_cursor}:v:0",
                            "-disposition:v:1",
                            "attached_pic",
                        ]
                    )
                    input_cursor += 1
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
    elif fmt == "ty":
        # Stand-alone TiVo TY streams carry MPEG-2 video and optionally MP2 or
        # AC-3 audio. Map only the first video/audio streams so attachments or
        # subtitles from a general-purpose input cannot make the muxer reject
        # the output's stream layout.
        enc_opts.extend(["-map", "0:v:0", "-pix_fmt", "yuv420p"])
        if vx_quant > 0:
            enc_opts.extend(["-qscale:v", str(vx_quant)])
        else:
            enc_opts.extend(["-b:v", "6000k", "-maxrate", "8000k", "-bufsize", "1835k"])
        enc_opts.extend(["-g", "15"])
        if audio == "none":
            enc_opts.append("-an")
        else:
            enc_opts.extend(
                [
                    "-map",
                    "0:a:0?",
                    "-c:a",
                    audio,
                    "-b:a",
                    "192k",
                    "-ar",
                    str(audio_rate if audio_rate > 0 else 48000),
                    "-ac",
                    "2",
                ]
            )
    elif fmt == "rvid":
        # RocketVideo: 16bpp (RGB555/565) frames, optional Nintendo LZ10, encoded
        # entirely by ffmpeg's rvid encoder. The encoder consumes rgb24 and packs
        # to 16bpp itself. Audio is raw PCM (the rvid muxer's native stream).
        # Left alone, the encoder interlaces and compresses by Vid2RVID's
        # frame-rate rules (what RocketVideoPlayer can keep up with), so only
        # an explicit choice is passed on.
        enc_opts.extend(["-mode", rvid_mode, "-dither", "0" if rvid_no_dither else "1"])
        if rvid_no_compress:
            enc_opts.extend(["-compress", "0"])
        if rvid_interlaced:
            enc_opts.extend(["-interlaced", "1"])
        if audio == "none":
            enc_opts.append("-an")
        else:
            # 16-bit PCM (pcm_s16le) is the higher-quality of the two rvid audio
            # stream types; the muxer writes it as the left/right sound stream.
            # RocketVideoPlayer refuses anything above 32 kHz.
            enc_opts.extend(["-c:a", "pcm_s16le"])
            enc_opts.extend(
                ["-ar", str(min(audio_rate, 32000) if audio_rate > 0 else 32000)]
            )
    elif fmt == "hvqm4":
        # The hvqm4 muxer carries video only -- drop any audio stream
        # regardless of the --audio choice.
        enc_opts.append("-an")
    elif fmt == "fastvideo":
        if audio == "none":
            enc_opts.append("-an")
        else:
            # FastVideoDSPlayer plays stereo at the DS mixer's 47605 Hz and
            # nothing else, whatever the header says.
            enc_opts.extend(["-c:a", "adpcm_ima_fvds", "-ar", "47605", "-ac", "2"])
    elif fmt in ("factor5", "f5vid"):
        # GameCube Factor 5 DivX: MPEG-4 ASP video + DSP-ADPCM audio. The
        # f5vid muxer re-codes each VOP into Factor 5's entropy layout, which
        # has no resync markers: the mpeg4 encoder adds those whenever it
        # slice-threads, so keep it to one thread (one slice per frame).
        enc_opts.extend(
            [
                "-vtag",
                "DIVX",
                "-pix_fmt",
                "yuv420p",
                "-bf",
                "0",
                "-threads:v",
                "1",
                # A P-VOP's quantizer is a 4-bit field in the VIDD header.
                "-qmax",
                "15",
            ]
        )
        if vx_quant > 0:
            enc_opts.extend(["-qscale:v", str(vx_quant)])
        elif mobi_bitrate:
            enc_opts.extend(["-b:v", mobi_bitrate])
        else:
            enc_opts.extend(["-qscale:v", "3"])
        if audio == "none":
            enc_opts.append("-an")
        else:
            # Every retail .vid carries 32 kHz stereo. No -shortest: the
            # adpcm_thp encoder emits the whole stream as one packet at the
            # end, which -shortest drops, and the muxer trims audio to the
            # video length on its own. Retail audio runs two frames ahead of
            # the video, so pad a little silence to keep the last AUDD
            # chunks from running dry.
            enc_opts.extend(
                [
                    "-c:a",
                    "adpcm_thp",
                    "-ac",
                    "2",
                    "-ar",
                    str(audio_rate or 32000),
                    "-af",
                    "apad=pad_dur=0.25",
                ]
            )
    elif fmt in ("gba_ads", "gba_hydrogen"):
        # GBA audio remains decode-only. The encoder writes a video-only
        # .mmstr and selects the compressor used by the requested lineage.
        enc_opts.extend(
            [
                "-an",
                "-pix_fmt",
                "rgb24",
                "-compression",
                "1" if fmt == "gba_hydrogen" else "0",
            ]
        )
    elif fmt == "wii_photo":
        # The Photo Channel's documented video format is Motion JPEG AVI.
        # PCM is broadly interoperable, but its audio component is not
        # guaranteed by Nintendo's player, so callers can choose --audio none.
        enc_opts.extend(
            ["-qscale:v", str(vx_quant if vx_quant > 0 else 3), "-pix_fmt", "yuvj420p"]
        )
        if audio == "none":
            enc_opts.append("-an")
        else:
            enc_opts.extend(
                [
                    "-c:a",
                    "pcm_s16le",
                    "-ar",
                    str(audio_rate if audio_rate > 0 else 32000),
                ]
            )
    elif fmt == "flipnote":
        # The muxer wants gray 256x192 and inks pixels below 128; dithering
        # through 1-bit first keeps tone instead of hard-thresholding it.
        enc_opts.extend(["-pix_fmt", "gray", "-f", "flipnote_ppm"])
        if parsed.flipnote_author:
            enc_opts.extend(["-ppm_author", parsed.flipnote_author])
        if audio == "none":
            enc_opts.append("-an")
        else:
            enc_opts.extend(["-c:a", "pcm_s16le", "-ar", "8192", "-ac", "1"])
    elif fmt == "flipnote3d":
        # The muxer takes rgb24 and maps it to the Flipnote colours itself.
        enc_opts.extend(
            [
                "-pix_fmt",
                "rgb24",
                "-f",
                "kwz",
                "-kwz_dither",
                parsed.flipnote_dither,
                "-kwz_paper",
                parsed.flipnote_paper,
            ]
        )
        if parsed.flipnote_author:
            enc_opts.extend(["-kwz_author", parsed.flipnote_author])
        if parsed.flipnote_key:
            enc_opts.extend(["-kwz_key", parsed.flipnote_key])
        if parsed.flipnote_loop:
            enc_opts.extend(["-kwz_loop", "1"])
        if audio == "none":
            enc_opts.append("-an")
        else:
            enc_opts.extend(["-c:a", "pcm_s16le", "-ar", "16364", "-ac", "1"])
    elif fmt == "nintendo_channel":
        # Match the UK Nintendo Channel 2009 3GP profile: 3gp6 + AVC
        # constrained baseline L2.1 + stereo 32 kHz AAC-LC. libx264 is also
        # required for the project's MobiClip encoder.
        enc_opts.extend(
            [
                "-profile:v",
                "constrained_baseline",
                "-level:v",
                "2.1",
                "-pix_fmt",
                "yuv420p",
                "-b:v",
                mobi_bitrate or "484k",
                "-g",
                "250",
                "-brand",
                "3gp6",
                "-video_track_timescale",
                "1000000",
                "-movflags",
                "+faststart",
            ]
        )
        if audio == "none":
            enc_opts.append("-an")
        else:
            enc_opts.extend(["-c:a", "aac", "-b:a", "50k", "-ar", "32000", "-ac", "2"])
    elif fmt == "mivf":
        # One page per frame, so constant frame rate.
        enc_opts.extend(["-fps_mode", "cfr"])
        if vx_quant > 0:
            enc_opts.extend(["-qp", str(vx_quant)])
        if parsed.m2y2:
            enc_opts.extend(["-m2y2", "1"])
        vopt_end = len(enc_opts)
        chunk_unit = 240  # the encoder's keyframe interval
        if audio == "none":
            enc_opts.append("-an")
        elif audio in ("ia4m", "adpcm", "pc16"):
            fps = output_fps(inp, fmt, fps_ovr)
            rate = mivf_audio_rate(fps, audio_rate)
            if audio_rate > 0 and rate != audio_rate:
                print(
                    f"error: {audio_rate} Hz does not split into whole {fps} fps MIVF "
                    f"frames; {mivf_audio_rate(fps)} Hz does"
                )
                sys.exit(2)
            if not rate:
                print(
                    f"error: no sample rate splits evenly into {fps} fps MIVF frames; pass --fps"
                )
                sys.exit(2)
            enc_opts.extend(
                ["-map", "0:v", "-map", "0:a:0?", "-c:a", "pcm_s16le", "-ar", str(rate)]
                + ["-ac", "2" if audio == "pc16" else "1", "-sn", "-dn"]
                + ["-mivf_audio", "pc16" if audio == "pc16" else "ia4m"]
            )
        else:
            print(
                f"error: mivf takes ia4m (mono ADPCM), pc16 (PCM) or none, not '{audio}'"
            )
            sys.exit(2)
    elif fmt == "bink":
        # One video packet per frame, so constant frame rate.
        enc_opts.extend(["-fps_mode", "cfr"])
        if vx_quant > 0:
            enc_opts.extend(["-quant", str(min(vx_quant, 15))])
        if parsed.bink_quality >= 0:
            enc_opts.extend(["-quality:v", str(parsed.bink_quality)])
        vopt_end = len(enc_opts)
        if audio == "none":
            enc_opts.extend(["-map", "0:v:0", "-an"])
        elif audio in ("bink", "adpcm", "bink_stereo"):
            # LibBink plays mono only; FFmpeg and RAD's player also take stereo.
            enc_opts.extend(
                ["-map", "0:v:0", "-map", "0:a:0?", "-c:a", "binkaudio_dct"]
                + ["-ac", "2" if audio == "bink_stereo" else "1"]
                + ["-ar", str(bink_audio_rate(inp, audio_rate)), "-sn", "-dn"]
            )
            if parsed.bink_audio_quality >= 0:
                enc_opts.extend(["-quality:a", str(parsed.bink_audio_quality)])
        else:
            print(
                f"error: bink takes bink (mono), bink_stereo or none audio, not '{audio}'"
            )
            sys.exit(2)
    elif fmt in TOPOS_FORMATS:
        tv, ta = topos_codec_opts(fmt, audio, mobi_bitrate, vx_quant)
        enc_opts.extend(tv)
        vopt_end = len(enc_opts)
        enc_opts.extend(ta)
    elif fmt == "3ds_camera":
        enc_opts.extend(
            ["-qscale:v", str(vx_quant if vx_quant > 0 else 3), "-pix_fmt", "yuvj420p"]
        )
        if audio == "none":
            enc_opts.append("-an")
        else:
            enc_opts.extend(["-c:a", "adpcm_ima_wav", "-ar", "16000", "-ac", "1"])

    # --fps applies to every format: the output frame rate generally has to match
    # the clip being replaced.  A DS VX player clocks video off the audio, so a
    # mismatch plays too slow/fast; .mods DS slots are ~30 fps and a 60 fps stream
    # overruns the ARM9 decode budget; retail THP movies are 29.97.  .mo defaults
    # to 29.97 (what the Wii player expects) but --fps still overrides it.
    fps_filter = ""
    if fmt in ("3ds_camera", "3ds_camera3d"):
        fps_filter = "fps=20"
    elif fmt == "flipnote3d":
        # Flipnote Studio 3D only has these playback speeds; take the nearest
        # one so the video and the BGM stay in step.
        speeds = (0.2, 0.5, 1, 2, 4, 6, 8, 12, 20, 24, 30)
        want = float(Fraction(fps_ovr)) if fps_ovr else 12.0
        fps_filter = f"fps={min(speeds, key=lambda v: abs(v - want)):g}"
    elif fps_ovr:
        fps_filter = f"fps={fps_ovr}"
    elif fmt == "mo":
        fps_filter = "fps=30000/1001"
    elif fmt == "dpg":
        # The DS decodes MPEG-1 in software; 15 fps is what MoonShell's own
        # encoders target and what the hardware keeps up with.
        fps_filter = "fps=15"
    elif fmt == "nintendo_channel":
        fps_filter = "fps=25"
    elif fmt == "flipnote":
        fps_filter = "fps=12"
    elif fmt in ("factor5", "f5vid"):
        fps_filter = "fps=30000/1001"

    filters = []
    if fmt in TOPOS_FORMATS:
        filters.append(topos_picture_filter(scale))
    elif fmt == "mivf":
        filters.append(mivf_picture_filter(scale))
    elif scale:
        filters.append(f"scale={scale}")
    if fps_filter:
        filters.append(fps_filter)
    if fmt == "flipnote":
        filters.append("format=monob,format=gray")

    if fmt == "mods":
        # The DS bitstream is YCgCo.  The ycgco filter writes what
        # geq=g='(r+2g+b)/4':b='(2g-r-b)/4+128':r='(r-b)/2+128' did, sample
        # for sample, without evaluating an expression per sample.
        ycgco = "format=gbrp,ycgco,mergeplanes=0x000102:yuv444p,format=yuv420p"
        filters.append(ycgco)
    elif fmt == "3ds_camera":
        # The camera's 480x240 pixels are square. Do not preserve a source
        # video's display aspect ratio through the scale filter.
        filters.append("setsar=1")

    vf = []
    if filters:
        vf = ["-vf", ",".join(filters)]

    fps_disp = f", {fps_filter}" if fps_filter else ""
    scale_disp = scale if scale else "source"

    base = (
        [FFENC, "-nostdin", "-y"]
        + input_fmt(inp)
        + ["-i", inp]
        + extra_inputs
        + vf
        + enc_opts
    )
    if mobi_passes == 2:
        # libx264 keeps its own statistics file (x264's psz_stat_out); ffmpeg's
        # -passlogfile drives avctx->stats_out, which this encoder never fills,
        # so the file has to be named through -x264-params.
        statfile = mobi_passlog or (container + ".pass")
        print(f">> pass 1/2  analysis  ->  {statfile}")
        run_cmd(
            base
            + ["-pass", "1", "-x264-params", f"stats={statfile}"]
            + extra_args
            + [container]
        ) or sys.exit(1)
        print(">> pass 2/2  encode")
        run_cmd(
            base
            + ["-pass", "2", "-x264-params", f"stats={statfile}"]
            + extra_args
            + [container]
        ) or sys.exit(1)
        for leftover in (statfile, statfile + ".mbtree", statfile + ".temp"):
            try:
                os.remove(leftover)
            except OSError:
                pass
    else:
        chunk_list = None
        jobs = parsed.jobs if parsed.jobs > 0 else default_jobs(fmt)
        shape = chunk_shape(fmt)
        if (
            vopt_end is not None
            and shape
            and mode == "vid"
            and jobs > 1
            and not extra_args
            # FastAudio .mods keeps a single keyframe; chunks would add more.
            and not (fmt == "mods" and audio == "fastaudio")
        ):
            lo, hi, unit = shape
            out_fps = output_fps(inp, fmt, fps_ovr)
            plan = plan_chunks(inp, jobs, out_fps, lo, hi, unit or chunk_unit)
            if plan:
                chunk_list = chunked_video(
                    [inp],
                    vf,
                    enc_opts[vopt_start:vopt_end],
                    plan,
                    out_fps,
                    container,
                    out_ext,
                    jobs,
                    ["-mo_audio", "none"] if fmt in ("mo", "mods") else [],
                    (
                        enc_opts[vopt_end:]
                        if fmt in TOPOS_FORMATS and audio != "none"
                        else None
                    ),
                )
        if chunk_list and fmt in TOPOS_FORMATS:
            join_chunks_copy(chunk_list, container, run_cmd) or sys.exit(1)
        elif chunk_list:
            # Mux exactly as the one-piece encode would, but take the video
            # from the joined chunks: the concat input goes after any extra
            # inputs so their stream indices stay put.
            k = 1 + extra_inputs.count("-i")
            fo = enc_opts[:vopt_start] + enc_opts[vopt_end:]
            for i in range(len(fo) - 1):
                if fo[i] == "-map" and fo[i + 1] == "0:v":
                    fo[i + 1] = f"{k}:v"
            if "-map" not in fo:
                fo = (
                    ["-map", f"{k}:v"]
                    + ([] if "-an" in fo else ["-map", "0:a:0?"])
                    + fo
                )
            print(">> joining chunks")
            run_cmd(
                [FFENC, "-nostdin", "-y"]
                + input_fmt(inp)
                + ["-i", inp]
                + extra_inputs
                + ["-f", "concat", "-safe", "0", "-i", chunk_list]
                + fo
                + ["-c:v", "copy", container]
            ) or sys.exit(1)
            shutil.rmtree(os.path.dirname(chunk_list), True)
        else:
            run_cmd(base + extra_args + [container]) or sys.exit(1)

    if roundtrip:
        print(f">> decoding  {container}  ->  {watch}  (single binary, mpeg4)")
        # The mods decoder converts its YCgCo planes to RGB itself now, so the
        # round-trip needs no colour filter of its own.
        dec_vf = []
        cmd1 = (
            [FFENC, "-nostdin", "-y", "-loglevel", "error", "-f", dmx, "-i", container]
            + dec_vf
            + ["-c:v", "mpeg4", "-q:v", "3", "-c:a", "aac", watch]
        )
        cmd2 = (
            [
                FFENC,
                "-nostdin",
                "-y",
                "-loglevel",
                "error",
                "-f",
                dmx,
                "-i",
                container,
                "-map",
                "0:v",
            ]
            + dec_vf
            + ["-c:v", "mpeg4", "-q:v", "3", watch]
        )
        run_ffenc_fallback(cmd1, cmd2)

        print("\nround-trip complete:")
        report_outputs(container, watch)
    else:
        print("\nencode complete:")
        report_outputs(container)
    print()

    p = subprocess.run(
        [FFENC, "-hide_banner", "-f", dmx, "-i", container],
        stderr=subprocess.PIPE,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    for line in p.stderr.splitlines():
        if "Stream" in line or "Duration" in line:
            print("  " + line)


if __name__ == "__main__":
    main()
