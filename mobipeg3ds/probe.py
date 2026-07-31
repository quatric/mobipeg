"""Source probing: duration, resolution, fps, stream indices.

Uses ffprobe's JSON output (`-show_format -show_streams -of json`), not
stderr banner parsing -- reliable and stable across ffmpeg versions.
"""
from __future__ import annotations

import json
import subprocess
from fractions import Fraction
from pathlib import Path

from .ffmpeg_locate import find_ffprobe
from .job import SourceRef


class ProbeError(RuntimeError):
    pass


def probe(path: str) -> SourceRef:
    p = Path(path)
    if not p.is_file():
        raise ProbeError(f"input not found: {path}")

    ffprobe = find_ffprobe()
    cmd = [
        str(ffprobe), "-v", "error",
        "-show_format", "-show_streams", "-of", "json",
        str(p),
    ]
    try:
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=30)
    except subprocess.TimeoutExpired as e:
        raise ProbeError(f"ffprobe timed out on {path}") from e
    if result.returncode != 0:
        raise ProbeError(f"ffprobe failed on {path}: {result.stderr.strip()}")

    try:
        data = json.loads(result.stdout)
    except json.JSONDecodeError as e:
        raise ProbeError(f"ffprobe produced unparseable output for {path}") from e

    fmt = data.get("format", {})
    streams = data.get("streams", [])
    video_indices = [s["index"] for s in streams if s.get("codec_type") == "video"]
    audio_indices = [s["index"] for s in streams if s.get("codec_type") == "audio"]

    ref = SourceRef(
        path=str(p),
        duration_s=float(fmt["duration"]) if fmt.get("duration") else None,
        video_stream_indices=video_indices,
        audio_stream_indices=audio_indices,
    )

    if video_indices:
        v = next(s for s in streams if s["index"] == video_indices[0])
        ref.width = v.get("width")
        ref.height = v.get("height")
        rate = v.get("avg_frame_rate") or v.get("r_frame_rate")
        if rate and rate != "0/0":
            frac = Fraction(rate)
            ref.fps_num, ref.fps_den = frac.numerator, frac.denominator

    if not video_indices:
        raise ProbeError(f"no video stream found in {path}")

    return ref
