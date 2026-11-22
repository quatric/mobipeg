#!/usr/bin/env python3
"""Shared helpers for encode.py and encode_gui.py.

Everything here is import-safe (no argument parsing, no side effects at import
time) and takes ffmpeg/ffprobe paths as parameters, so both front ends and the
tests can use it without knowing where the binaries live.

Contents:
  - audio track listing, offset and peak-normalisation helpers
  - HDR detection and tone-mapping filter selection
  - size-budget maths for --max-size-mb
  - read-only MOFLEX preflight diagnostics
  - a small JSON settings/preset/queue store for the GUI
"""

import json
import os
import re
import struct
import subprocess
import sys

# FAT32 cannot hold a file of 4 GiB or more; the 3DS SD card is FAT32.
FAT32_MAX_BYTES = 4 * 1024**3 - 1

# Peak level the normaliser aims for, and the most gain it will ever apply.
NORMALIZE_TARGET_DB = -1.0
NORMALIZE_MAX_GAIN_DB = 24.0

HDR_MODES = ("auto", "off", "mobius", "hable", "reinhard", "clip")


def _run(cmd, **kw):
    kw.setdefault("capture_output", True)
    kw.setdefault("text", True)
    kw.setdefault("encoding", "utf-8")
    kw.setdefault("errors", "replace")
    return subprocess.run(cmd, **kw)


# --------------------------------------------------------------------------
# Audio tracks, offset, normalisation
# --------------------------------------------------------------------------


def list_audio_tracks(ffprobe, path, input_args=()):
    """Return one dict per audio stream, numbered the way `-map 0:a:N` is."""
    try:
        p = _run(
            [ffprobe, "-v", "error"]
            + list(input_args)
            + [
                "-select_streams",
                "a",
                "-show_entries",
                "stream=index,codec_name,channels,sample_rate:stream_tags=language,title",
                "-of",
                "json",
                path,
            ]
        )
        data = json.loads(p.stdout or "{}")
    except (OSError, ValueError):
        return []
    tracks = []
    for n, s in enumerate(data.get("streams", [])):
        tags = s.get("tags") or {}
        tracks.append(
            {
                "track": n,
                "stream_index": s.get("index"),
                "codec": s.get("codec_name", "?"),
                "channels": s.get("channels") or 0,
                "sample_rate": int(s.get("sample_rate") or 0),
                "language": tags.get("language", ""),
                "title": tags.get("title", ""),
            }
        )
    return tracks


def describe_audio_track(t):
    bits = [f"track {t['track']}", t["codec"]]
    if t["channels"]:
        bits.append(f"{t['channels']}ch")
    if t["sample_rate"]:
        bits.append(f"{t['sample_rate']} Hz")
    if t["language"]:
        bits.append(t["language"])
    if t["title"]:
        bits.append(f'"{t["title"]}"')
    return "  ".join(bits)


def format_audio_tracks(tracks):
    if not tracks:
        return "no audio tracks"
    return "\n".join(describe_audio_track(t) for t in tracks)


def audio_offset_filter(offset_ms):
    """Filter that shifts audio against the video, or None for no shift.

    A positive offset delays the audio (silence is inserted in front of it);
    a negative one starts it earlier by trimming the head off.
    """
    try:
        ms = int(offset_ms)
    except (TypeError, ValueError):
        return None
    if ms > 0:
        return f"adelay={ms}:all=1"
    if ms < 0:
        return f"atrim=start={-ms / 1000.0:.3f},asetpts=PTS-STARTPTS"
    return None


def measure_peak_db(ffmpeg, path, track=None, input_args=()):
    """Highest sample level (dBFS) of the chosen audio track, or None."""
    sel = f"0:a:{track}" if track is not None and track >= 0 else "0:a:0"
    try:
        p = _run(
            [ffmpeg, "-nostdin", "-hide_banner", "-nostats"]
            + list(input_args)
            + [
                "-i",
                path,
                "-map",
                sel,
                "-vn",
                "-af",
                "volumedetect",
                "-f",
                "null",
                "-",
            ]
        )
    except OSError:
        return None
    m = re.search(r"max_volume:\s*(-?[0-9.]+|-inf)\s*dB", p.stderr or "")
    if not m or m.group(1) == "-inf":
        return None
    return float(m.group(1))


def normalize_gain_db(peak_db, target_db=NORMALIZE_TARGET_DB,
                      max_gain_db=NORMALIZE_MAX_GAIN_DB):
    """Gain in dB that brings `peak_db` to `target_db`, bounded by max_gain_db.

    Returns 0.0 when the peak is unknown or already at/above the target: the
    normaliser only ever raises quiet audio, it never clips loud audio down.
    """
    if peak_db is None:
        return 0.0
    gain = target_db - peak_db
    if gain <= 0:
        return 0.0
    return min(gain, max_gain_db)


def audio_filter_chain(offset_ms=0, gain_db=0.0):
    """Comma-joined -af chain for the requested offset and gain ('' if none)."""
    parts = []
    if gain_db and abs(gain_db) >= 0.05:
        parts.append(f"volume={gain_db:.2f}dB")
    off = audio_offset_filter(offset_ms)
    if off:
        parts.append(off)
    return ",".join(parts)


def merge_af(opts, chain):
    """Add `chain` to an ffmpeg option list, extending an existing -af if any."""
    if not chain:
        return opts
    opts = list(opts)
    for i in range(len(opts) - 1, -1, -1):
        if opts[i] == "-af" and i + 1 < len(opts):
            opts[i + 1] = chain + "," + opts[i + 1]
            return opts
    return opts + ["-af", chain]


# --------------------------------------------------------------------------
# HDR
# --------------------------------------------------------------------------

_filter_cache = {}


def available_filters(ffmpeg):
    """Set of filter names this ffmpeg build has (cached per binary)."""
    if ffmpeg in _filter_cache:
        return _filter_cache[ffmpeg]
    names = set()
    try:
        p = _run([ffmpeg, "-hide_banner", "-filters"])
        for line in (p.stdout or "").splitlines():
            parts = line.split()
            if len(parts) >= 3 and re.fullmatch(r"[T.][S.][C.]?", parts[0]):
                names.add(parts[1])
    except OSError:
        pass
    _filter_cache[ffmpeg] = names
    return names


def detect_hdr(ffprobe, path, input_args=()):
    """'pq' (HDR10/Dolby base layer), 'hlg', or None for the first video stream."""
    try:
        p = _run(
            [ffprobe, "-v", "error"]
            + list(input_args)
            + [
                "-select_streams",
                "v:0",
                "-show_entries",
                "stream=color_transfer",
                "-of",
                "default=nk=1:nw=1",
                path,
            ]
        )
    except OSError:
        return None
    trc = (p.stdout or "").strip().splitlines()
    trc = trc[0].strip() if trc else ""
    if trc == "smpte2084":
        return "pq"
    if trc == "arib-std-b67":
        return "hlg"
    return None


def hdr_tonemap_chain(mode, filters):
    """(filter_chain, warning) that converts PQ/HLG video to BT.709 SDR.

    `mode` is one of HDR_MODES; 'auto' picks Mobius, 'off' returns no chain.
    The standard chain needs zscale (libzimg). Without it there is no honest
    way to linearise PQ/HLG, so the caller gets a warning instead of a
    plausible-looking but wrong image.
    """
    if mode in (None, "", "off"):
        return None, None
    curve = "mobius" if mode == "auto" else mode
    if curve not in ("mobius", "hable", "reinhard", "clip"):
        return None, f"unknown HDR mode '{mode}'"
    missing = [f for f in ("zscale", "tonemap") if f not in filters]
    if missing:
        return None, (
            "this ffmpeg build has no "
            + "/".join(missing)
            + " filter (needs libzimg), so HDR tone mapping was skipped"
        )
    chain = (
        "zscale=t=linear:npl=100,format=gbrpf32le,zscale=p=bt709,"
        f"tonemap=tonemap={curve}:desat=0,"
        "zscale=t=bt709:m=bt709:r=tv,format=yuv420p"
    )
    return chain, None


def _fps_value(text):
    try:
        if "/" in text:
            a, b = text.split("/")
            return float(a) / float(b) if float(b) else 0.0
        return float(text)
    except (TypeError, ValueError):
        return 0.0


def probe_source(ffprobe, path, input_args=()):
    """One ffprobe call summarising an input for the GUI.

    Returns {"duration", "width", "height", "fps", "hdr", "audio": [track...]}
    with None/empty values for anything that could not be read.
    """
    info = {"duration": None, "width": 0, "height": 0, "fps": 0.0, "hdr": None,
            "video_codec": "", "audio": []}
    try:
        p = _run(
            [ffprobe, "-v", "error"]
            + list(input_args)
            + [
                "-show_entries",
                "format=duration:stream=index,codec_type,codec_name,width,height,"
                "avg_frame_rate,color_transfer,channels,sample_rate:"
                "stream_tags=language,title",
                "-of",
                "json",
                path,
            ]
        )
        data = json.loads(p.stdout or "{}")
    except (OSError, ValueError):
        return info
    try:
        info["duration"] = float((data.get("format") or {}).get("duration"))
    except (TypeError, ValueError):
        pass
    n_audio = 0
    for s in data.get("streams", []):
        kind = s.get("codec_type")
        if kind == "video" and not info["width"]:
            info["width"] = int(s.get("width") or 0)
            info["height"] = int(s.get("height") or 0)
            info["fps"] = _fps_value(s.get("avg_frame_rate", ""))
            info["video_codec"] = s.get("codec_name", "")
            trc = s.get("color_transfer")
            info["hdr"] = {"smpte2084": "pq", "arib-std-b67": "hlg"}.get(trc)
        elif kind == "audio":
            tags = s.get("tags") or {}
            info["audio"].append(
                {
                    "track": n_audio,
                    "stream_index": s.get("index"),
                    "codec": s.get("codec_name", "?"),
                    "channels": s.get("channels") or 0,
                    "sample_rate": int(s.get("sample_rate") or 0),
                    "language": tags.get("language", ""),
                    "title": tags.get("title", ""),
                }
            )
            n_audio += 1
    return info


def describe_source(info):
    """One-line summary of probe_source() output."""
    if not info or not info.get("width"):
        return "no video stream found" if info is not None else "unreadable"
    bits = [f"{info['width']}x{info['height']}"]
    if info["fps"]:
        bits.append(f"{info['fps']:.3f}".rstrip("0").rstrip(".") + " fps")
    if info["duration"]:
        bits.append(format_clock(info["duration"]))
    if info["hdr"]:
        bits.append("HDR (" + ("PQ" if info["hdr"] == "pq" else "HLG") + ")")
    n = len(info["audio"])
    bits.append(f"{n} audio track{'s' if n != 1 else ''}")
    return "  ·  ".join(bits)


# --------------------------------------------------------------------------
# Output size budget
# --------------------------------------------------------------------------

# Bits per sample of the audio codecs the MobiClip containers carry.
_AUDIO_BITS = {"adpcm": 4.0, "fastaudio": 2.0, "pcm": 16.0, "vorbis": 2.0,
               "codebook": 2.0}


def audio_bps_estimate(codec, sample_rate, channels):
    """Rough audio bitrate in bits/s, used only to size the video budget."""
    if codec in (None, "", "none"):
        return 0
    bits = _AUDIO_BITS.get(codec, 4.0)
    return int(bits * max(sample_rate, 8000) * max(channels, 1))


def video_bps_for_budget(max_bytes, duration, audio_bps, overhead=0.04):
    """Video bitrate (bits/s) that should land the file under `max_bytes`."""
    if not duration or duration <= 0:
        return 0
    usable = max_bytes * (1.0 - overhead) * 8.0 / duration
    return max(int(usable - audio_bps), 20_000)


def retry_bps(current_bps, actual_bytes, max_bytes, safety=0.97):
    """Next bitrate to try after a file came out `actual_bytes` big."""
    if actual_bytes <= 0:
        return current_bps
    return max(int(current_bps * (max_bytes / actual_bytes) * safety), 20_000)


def format_bps(bps):
    return f"{max(int(bps / 1000), 1)}k"


def fit_to_size(run_once, out_path, max_bytes, start_bps, attempts=4, log=print):
    """Re-encode with a falling bitrate until `out_path` fits in `max_bytes`.

    run_once(bps) must write out_path and return True on success. Returns
    (ok, final_size, final_bps).
    """
    bps = start_bps
    size = 0
    for attempt in range(1, attempts + 1):
        log(f">> size fit {attempt}/{attempts}: video {format_bps(bps)}b/s"
            f" (limit {max_bytes / 1048576:.1f} MiB)")
        if not run_once(bps):
            return False, size, bps
        size = os.path.getsize(out_path)
        log(f"   result {size / 1048576:.2f} MiB")
        if size <= max_bytes:
            return True, size, bps
        bps = retry_bps(bps, size, max_bytes)
    return False, size, bps


# --------------------------------------------------------------------------
# MOFLEX preflight
# --------------------------------------------------------------------------

MOFLEX_MAGIC = 0x4C32
SUPER_TRAILER_TAG = b"CSXTRA01"


def inspect_moflex(path, ffprobe=None, ffmpeg=None, decode_check=False,
                   max_issues=20):
    """Read-only structural check of a .moflex file.

    Never modifies the file. The block-level checks use only the container's
    own layout (fixed-size blocks, a 4C32 sync header at the start of some of
    them, an optional Super MOFLEX trailer); stream details come from ffprobe
    when a binary is supplied. Returns a dict with "errors", "warnings" and
    "notes" lists plus the facts found.
    """
    info = {
        "path": path,
        "size": 0,
        "block_size": 0,
        "sync_blocks": 0,
        "max_sync_gap_s": 0.0,
        "duration_s": None,
        "super": False,
        "streams": [],
        "errors": [],
        "warnings": [],
        "notes": [],
    }
    err, warn, note = info["errors"], info["warnings"], info["notes"]
    try:
        size = os.path.getsize(path)
    except OSError as e:
        err.append(f"cannot read file: {e}")
        return info
    info["size"] = size
    if size < 32:
        err.append("file is too small to be a MOFLEX")
        return info

    with open(path, "rb") as f:
        head = f.read(16)
        if struct.unpack(">H", head[:2])[0] != MOFLEX_MAGIC:
            err.append("does not start with a 4C32 sync header (not a MOFLEX)")
            return info
        block = struct.unpack(">H", head[12:14])[0] + 1
        info["block_size"] = block
        if block not in (2048, 4096):
            warn.append(
                f"unusual block size {block} (retail uses 4096, the 3DS SDK "
                "encoder 2048)"
            )

        f.seek(max(size - 16, 0))
        tail = f.read(16)
        media_end = size
        if tail[8:16] == SUPER_TRAILER_TAG:
            info["super"] = True
            poff = struct.unpack("<Q", tail[:8])[0]
            if 0 < poff < size - 16:
                media_end = poff
                note.append(f"Super MOFLEX trailer at offset {poff}")
            else:
                err.append("Super MOFLEX trailer has an invalid offset")

        if media_end % block:
            warn.append(
                f"media section ({media_end} bytes) is not a whole number of "
                f"{block}-byte blocks"
            )

        # Walk every block start looking for sync headers.
        sync_ts = []
        bad_block_size = 0
        offset = 0
        while offset + 14 <= media_end:
            f.seek(offset)
            h = f.read(14)
            if len(h) < 14:
                break
            if struct.unpack(">H", h[:2])[0] == MOFLEX_MAGIC:
                ts = struct.unpack(">Q", h[4:12])[0]
                sync_ts.append(ts)
                if struct.unpack(">H", h[12:14])[0] + 1 != block:
                    bad_block_size += 1
            offset += block

    info["sync_blocks"] = len(sync_ts)
    if bad_block_size:
        err.append(f"{bad_block_size} sync header(s) disagree about the block size")
    if sync_ts:
        went_back = sum(1 for a, b in zip(sync_ts, sync_ts[1:]) if b < a)
        if went_back:
            err.append(f"{went_back} sync timestamp(s) go backwards")
        gaps = [(b - a) / 1e6 for a, b in zip(sync_ts, sync_ts[1:]) if b >= a]
        if gaps:
            info["max_sync_gap_s"] = max(gaps)
        info["duration_s"] = sync_ts[-1] / 1e6
    else:
        err.append("no sync headers found at block boundaries")

    if size > FAT32_MAX_BYTES:
        err.append(
            f"file is {size / 1073741824:.2f} GiB; FAT32 (the 3DS SD card) "
            "cannot store 4 GiB or more"
        )
    elif size > FAT32_MAX_BYTES * 0.97:
        warn.append("file is within 3% of the FAT32 4 GiB limit")

    if ffprobe:
        try:
            p = _run(
                [
                    ffprobe,
                    "-v",
                    "error",
                    "-f",
                    "moflex",
                    "-show_entries",
                    "stream=index,codec_type,codec_name,width,height,"
                    "avg_frame_rate,sample_rate,channels:format=duration",
                    "-of",
                    "json",
                    path,
                ]
            )
            data = json.loads(p.stdout or "{}")
            info["streams"] = data.get("streams", [])
            dur = (data.get("format") or {}).get("duration")
            if dur:
                info["duration_s"] = float(dur)
            if p.returncode != 0 and (p.stderr or "").strip():
                warn.append("ffprobe: " + p.stderr.strip().splitlines()[0])
            if not info["streams"]:
                err.append("ffprobe found no streams")
        except (OSError, ValueError):
            warn.append("could not run ffprobe for stream details")
        vids = [s for s in info["streams"] if s.get("codec_type") == "video"]
        auds = [s for s in info["streams"] if s.get("codec_type") == "audio"]
        if info["streams"] and not vids:
            err.append("no video stream")
        if info["streams"] and not auds:
            note.append("no audio stream")
        for v in vids[:1]:
            w, h = v.get("width") or 0, v.get("height") or 0
            # Stereo files pack both eyes side by side, so allow double width.
            if h > 240 or w > 800:
                note.append(f"video is {w}x{h}; the 3DS top screen is 400x240 per eye")

    if decode_check and ffmpeg:
        try:
            p = _run([ffmpeg, "-nostdin", "-v", "error", "-f", "moflex", "-i", path,
                      "-f", "null", "-"])
            lines = [l for l in (p.stderr or "").splitlines() if l.strip()]
            if p.returncode != 0 or lines:
                err.append(f"full decode reported {len(lines)} problem line(s)")
                for l in lines[:max_issues]:
                    warn.append("decode: " + l.strip())
            else:
                note.append("full decode completed with no errors")
        except OSError:
            warn.append("could not run ffmpeg for the decode check")
    return info


def format_inspection(info):
    out = [f"file:        {info['path']}"]
    if info["size"]:
        out.append(f"size:        {info['size'] / 1048576:.2f} MiB ({info['size']} bytes)")
    if info["block_size"]:
        out.append(f"block size:  {info['block_size']}")
    out.append(f"sync blocks: {info['sync_blocks']}")
    if info["duration_s"] is not None:
        out.append(f"duration:    {info['duration_s']:.2f} s")
    if info["max_sync_gap_s"]:
        out.append(f"longest gap between sync points: {info['max_sync_gap_s']:.2f} s")
    out.append("super:       " + ("yes" if info["super"] else "no"))
    for s in info["streams"]:
        kind = s.get("codec_type", "?")
        if kind == "video":
            out.append(f"video:       {s.get('codec_name')} "
                       f"{s.get('width')}x{s.get('height')} @ {s.get('avg_frame_rate')}")
        elif kind == "audio":
            out.append(f"audio:       {s.get('codec_name')} "
                       f"{s.get('sample_rate')} Hz {s.get('channels')}ch")
        else:
            out.append(f"{kind}:  {s.get('codec_name')}")
    for label, key in (("ERROR", "errors"), ("warning", "warnings"), ("note", "notes")):
        for m in info[key]:
            out.append(f"{label}: {m}")
    out.append("result:      " + ("FAIL" if info["errors"] else "ok"))
    return "\n".join(out)


# --------------------------------------------------------------------------
# GUI settings / presets / queue persistence
# --------------------------------------------------------------------------


def config_dir():
    """Per-user folder for GUI state. MOBIPEG_CONFIG_DIR overrides it."""
    override = os.environ.get("MOBIPEG_CONFIG_DIR")
    if override:
        return override
    if sys.platform == "darwin":
        base = os.path.expanduser("~/Library/Application Support")
    elif os.name == "nt":
        base = os.environ.get("APPDATA") or os.path.expanduser("~")
    else:
        base = os.environ.get("XDG_CONFIG_HOME") or os.path.expanduser("~/.config")
    return os.path.join(base, "mobipeg")


class JsonStore:
    """A tiny dict-in-a-file store that never raises on a bad or missing file."""

    def __init__(self, name, directory=None):
        self.path = os.path.join(directory or config_dir(), name)

    def load(self, default=None):
        try:
            with open(self.path, "r", encoding="utf-8") as f:
                return json.load(f)
        except (OSError, ValueError):
            return {} if default is None else default

    def save(self, data):
        try:
            os.makedirs(os.path.dirname(self.path), exist_ok=True)
            tmp = self.path + ".tmp"
            with open(tmp, "w", encoding="utf-8") as f:
                json.dump(data, f, indent=2, sort_keys=True)
            os.replace(tmp, self.path)
            return True
        except OSError:
            return False


def round_scale16(text):
    """'WxH' or 'W:H' with both sides rounded to the nearest multiple of 16
    (MobiClip codes 16x16 macroblocks). Anything else comes back unchanged."""
    m = re.fullmatch(r"\s*(\d+)\s*[x:]\s*(\d+)\s*", text or "")
    if not m:
        return text
    w, h = (max(16, 16 * round(int(v) / 16)) for v in m.groups())
    return f"{w}x{h}"


# Quality profiles that ship with the GUI. Keys match the Encode tab's setting
# names; the ladder follows the Mobipeg-MOFLEX Studio (quantizer, search depth,
# motion search). Lower quantizer = better quality, bigger file.
def _preset(quant, subme, me, skip="0"):
    return {"quant": quant, "subme": subme, "skip": skip, "me": me}


BUILTIN_PRESETS = {
    "Best": _preset("27", "9", "umh"),
    "Quality - the sweet spot": _preset("30", "9", "hex"),
    "Balanced": _preset("32", "7", "hex"),
    "Small": _preset("36", "7", "hex"),
}


# --------------------------------------------------------------------------
# Progress parsing and process control (GUI)
# --------------------------------------------------------------------------

_TIME_RE = re.compile(r"\btime=(\d+):(\d{2}):(\d{2}(?:\.\d+)?)")


def parse_progress_seconds(line):
    """Seconds of media encoded so far, from an ffmpeg status line, or None."""
    m = _TIME_RE.search(line)
    if not m:
        return None
    return int(m.group(1)) * 3600 + int(m.group(2)) * 60 + float(m.group(3))


def format_clock(seconds):
    """H:MM:SS (or M:SS) for a duration in seconds."""
    if seconds is None or seconds < 0:
        return "?"
    s = int(round(seconds))
    h, rem = divmod(s, 3600)
    m, s = divmod(rem, 60)
    return f"{h}:{m:02d}:{s:02d}" if h else f"{m}:{s:02d}"


def eta_text(frac, elapsed, now=None):
    """'ETA 12:34 (14:05)': time left and the clock time it finishes, or ''
    while there is too little progress to say anything useful."""
    import time

    if frac < 0.01 or frac >= 1.0 or elapsed < 5:
        return ""
    left = elapsed * (1.0 - frac) / frac
    done_at = time.strftime("%H:%M", time.localtime((now or time.time()) + left))
    return f"ETA {format_clock(left)} ({done_at})"


def popen_kwargs_new_group():
    """Popen options that put a child (and the ffmpeg it spawns) in its own group."""
    if os.name == "nt":
        return {"creationflags": getattr(subprocess, "CREATE_NEW_PROCESS_GROUP", 0)}
    return {"start_new_session": True}


def terminate_tree(proc):
    """Stop `proc` and everything it started (encode.py launches ffmpeg)."""
    if proc is None or proc.poll() is not None:
        return
    try:
        if os.name == "nt":
            subprocess.run(
                ["taskkill", "/T", "/F", "/PID", str(proc.pid)],
                capture_output=True,
            )
        else:
            import signal

            os.killpg(os.getpgid(proc.pid), signal.SIGTERM)
    except (OSError, ProcessLookupError):
        try:
            proc.terminate()
        except OSError:
            pass


# --------------------------------------------------------------------------
# Batch queue model
# --------------------------------------------------------------------------

QUEUE_PENDING, QUEUE_RUNNING, QUEUE_DONE, QUEUE_FAILED, QUEUE_STOPPED = (
    "pending", "running", "done", "failed", "stopped")


class JobQueue:
    """An ordered list of encode jobs, each a ready-to-run argv.

    The GUI snapshots the Encode tab's settings into `cmd` when a job is added,
    so later edits to the form do not change queued work. Jobs survive a restart
    through to_list()/from_list(); one that was running when the app closed
    comes back as pending.
    """

    def __init__(self):
        self.jobs = []
        self._next_id = 1

    def add(self, cmd, label, output="", duration=None, fmt=""):
        job = {
            "id": self._next_id,
            "cmd": list(cmd),
            "label": label,
            "output": output,
            "duration": duration,
            "fmt": fmt,
            "status": QUEUE_PENDING,
            "detail": "",
        }
        self._next_id += 1
        self.jobs.append(job)
        return job

    def get(self, job_id):
        for j in self.jobs:
            if j["id"] == job_id:
                return j
        return None

    def next_pending(self):
        for j in self.jobs:
            if j["status"] == QUEUE_PENDING:
                return j
        return None

    def set_status(self, job_id, status, detail=""):
        j = self.get(job_id)
        if j:
            j["status"] = status
            j["detail"] = detail
        return j

    def remove(self, ids):
        ids = set(ids)
        self.jobs = [j for j in self.jobs
                     if j["id"] not in ids or j["status"] == QUEUE_RUNNING]

    def clear_finished(self):
        self.jobs = [j for j in self.jobs
                     if j["status"] in (QUEUE_PENDING, QUEUE_RUNNING)]

    def retry_failed(self):
        for j in self.jobs:
            if j["status"] in (QUEUE_FAILED, QUEUE_STOPPED):
                j["status"], j["detail"] = QUEUE_PENDING, ""

    def move(self, ids, delta):
        """Shift the selected, not-running jobs up (delta<0) or down (delta>0)."""
        ids = set(ids)
        order = range(len(self.jobs)) if delta < 0 else range(len(self.jobs) - 1, -1, -1)
        for i in order:
            j = self.jobs[i]
            k = i + (-1 if delta < 0 else 1)
            if j["id"] in ids and j["status"] != QUEUE_RUNNING and 0 <= k < len(self.jobs):
                other = self.jobs[k]
                if other["id"] in ids or other["status"] == QUEUE_RUNNING:
                    continue
                self.jobs[i], self.jobs[k] = other, j

    def counts(self):
        out = {}
        for j in self.jobs:
            out[j["status"]] = out.get(j["status"], 0) + 1
        return out

    def to_list(self):
        return [dict(j) for j in self.jobs]

    @classmethod
    def from_list(cls, data):
        q = cls()
        if not isinstance(data, list):
            return q
        for raw in data:
            try:
                j = {
                    "id": int(raw["id"]),
                    "cmd": [str(x) for x in raw["cmd"]],
                    "label": str(raw.get("label", "")),
                    "output": str(raw.get("output", "")),
                    "duration": raw.get("duration"),
                    "fmt": str(raw.get("fmt", "")),
                    "status": raw.get("status", QUEUE_PENDING),
                    "detail": str(raw.get("detail", "")),
                }
            except (KeyError, TypeError, ValueError):
                continue
            if j["status"] == QUEUE_RUNNING:
                j["status"] = QUEUE_PENDING
            if j["status"] not in (QUEUE_PENDING, QUEUE_DONE, QUEUE_FAILED, QUEUE_STOPPED):
                j["status"] = QUEUE_PENDING
            q.jobs.append(j)
        q._next_id = max([j["id"] for j in q.jobs] + [0]) + 1
        return q
