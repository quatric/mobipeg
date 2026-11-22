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


# Presets that ship with the GUI. Keys match the Encode tab's setting names.
BUILTIN_PRESETS = {
    "Best quality (largest file)": {
        "hq": True, "quant": "12", "subme": "9", "skip": "0",
        "bitrate": "", "multipass": False, "max_size_mb": "",
    },
    "Balanced": {
        "hq": False, "quant": "22", "subme": "7", "skip": "512",
        "bitrate": "", "multipass": False, "max_size_mb": "",
    },
    "Small (700k two-pass)": {
        "hq": False, "quant": "0", "subme": "7", "skip": "512",
        "bitrate": "700k", "multipass": True, "max_size_mb": "",
    },
    "Fit 3DS SD (under 4 GiB)": {
        "hq": False, "quant": "0", "subme": "7", "skip": "512",
        "bitrate": "", "multipass": False, "max_size_mb": "4000",
    },
}
