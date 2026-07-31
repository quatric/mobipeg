"""Resolves an EncodeJob into the exact ffmpeg invocation.

This is the ONLY place that constructs ffmpeg arguments. The CLI and the GUI
both call `resolve_args()` and then either print it (dry-run / Review step)
or actually run it (encode). Neither frontend builds argument lists itself.
"""
from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path

from .ffmpeg_locate import find_ffmpeg
from .job import EncodeJob

# Confirmed-working audio modes for -mo_audio, per this investigation's own
# testing and the encode.py reference. "recommended" resolves to "adpcm" --
# the smallest, most broadly-compatible option among the four.
_RECOMMENDED_AUDIO_CODEC = "adpcm"

TARGET_WIDTH = 400
TARGET_HEIGHT = 240


class UnsupportedTargetError(RuntimeError):
    """Raised for target='moflex3d' -- deliberately not silently handled."""


@dataclass
class ResolvedCommand:
    env: dict[str, str]
    argv: list[str]

    def display_string(self) -> str:
        env_prefix = " ".join(f"{k}={v}" for k, v in self.env.items())
        cmd = " ".join(_quote(a) for a in self.argv)
        return f"{env_prefix} {cmd}".strip()


def _quote(arg: str) -> str:
    if " " in arg or arg == "":
        return f'"{arg}"'
    return arg


def _picture_filter(job: EncodeJob) -> str:
    """Builds the scale/pad or scale/crop filter for the job's picture mode.
    Never hardcodes a specific source resolution -- always derived from the
    probed source and the fixed 400x240 target, per the audit's own finding
    that 400x224 was a test-clip-specific value, not a product constant."""
    picture = job.picture
    filters: list[str] = []

    if picture.mode == "custom_crop":
        if not picture.custom_crop:
            raise ValueError("custom_crop mode requires picture.custom_crop to be set")
        x, y, w, h = picture.custom_crop
        filters.append(f"crop={w}:{h}:{x}:{y}")
        filters.append(
            f"scale={TARGET_WIDTH}:{TARGET_HEIGHT}:force_original_aspect_ratio=decrease"
        )
        filters.append(
            f"pad={TARGET_WIDTH}:{TARGET_HEIGHT}:(ow-iw)/2:(oh-ih)/2"
        )
    elif picture.mode == "fill":
        filters.append(
            f"scale={TARGET_WIDTH}:{TARGET_HEIGHT}:force_original_aspect_ratio=increase"
        )
        filters.append(f"crop={TARGET_WIDTH}:{TARGET_HEIGHT}")
    else:  # "fit" (default, letterbox)
        filters.append(
            f"scale={TARGET_WIDTH}:{TARGET_HEIGHT}:force_original_aspect_ratio=decrease"
        )
        filters.append(
            f"pad={TARGET_WIDTH}:{TARGET_HEIGHT}:(ow-iw)/2:(oh-ih)/2"
        )

    if picture.target_fps:
        filters.append(f"fps={picture.target_fps}")

    filters.append("format=yuv420p")
    return ",".join(filters)


def resolve_args(job: EncodeJob) -> ResolvedCommand:
    if job.target == "moflex3d":
        raise UnsupportedTargetError(
            "moflex3d (stereoscopic 3D) is source-implemented upstream "
            "(encode.py's moflex3d path) but is deliberately deferred here: "
            "not playback-verified against this project's own quality work, "
            "and not exposed by this CLI/GUI. Use target='moflex' (flat 2D)."
        )
    if job.target != "moflex":
        raise UnsupportedTargetError(f"unsupported target '{job.target}'")
    if job.source is None:
        raise ValueError("job.source must be set (run probe() first)")
    if not job.output.path:
        raise ValueError("job.output.path must be set")

    ffmpeg = find_ffmpeg()
    env: dict[str, str] = {}
    argv: list[str] = [str(ffmpeg), "-y", "-nostdin", "-loglevel", "error", "-stats"]

    argv += ["-i", job.source.path]

    argv += ["-map", f"0:{job.video_stream_index}"] if job.video_stream_index is not None else ["-map", "0:v:0"]

    if job.audio.mode == "video_only":
        argv += ["-an"]
    else:
        if job.audio_stream_index is not None:
            argv += ["-map", f"0:{job.audio_stream_index}"]
        else:
            argv += ["-map", "0:a:0?"]
        if job.audio.downmix_to_stereo:
            argv += ["-ac", "2"]
        argv += ["-mo_audio", _RECOMMENDED_AUDIO_CODEC]

    argv += ["-vf", _picture_filter(job)]

    q = job.quality
    argv += ["-c:v", "mobiclip", "-mobiclip", "1"]
    argv += ["-qp", str(q.qp)]
    argv += ["-mobi_qyx", str(q.mobi_qyx)]
    if q.q_rdo:
        env["MOBI_QYX_RDO"] = "1"
    if q.chroma_dz is not None:
        env["MOBI_CHROMA_DZ"] = str(q.chroma_dz)
    argv += ["-threads:v", str(q.threads)]

    argv += ["-f", "moflex", job.partial_output_path]

    return ResolvedCommand(env=env, argv=argv)
