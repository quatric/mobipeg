"""EncodeJob v1 -- the single versioned job model shared by the CLI and GUI.

Both frontends build one of these and hand it to `mobipeg3ds.backend.resolve_args()`
to get the exact ffmpeg argument array. Neither frontend constructs ffmpeg
arguments itself.

Only `target="moflex"` (flat 2D) is implemented end-to-end this phase.
`target="moflex3d"` (stereoscopic 3D) is accepted by the schema -- the real
`encode.py` reference already implements this format -- but the backend
rejects it explicitly with a clear "deferred, playback-unverified" message
rather than silently producing a job for it. See NOTICE.md.
"""
from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path
from typing import Literal, Optional

JOB_SCHEMA_VERSION = 1

PictureMode = Literal["fit", "fill", "custom_crop"]
QualityPresetName = Literal["balanced", "anime_hq", "maximum", "custom"]
AudioMode = Literal["recommended", "video_only"]
Target = Literal["moflex", "moflex3d"]


@dataclass
class SourceRef:
    """What was probed about the input file. Populated by mobipeg3ds.probe.

    duration_s is the CONTAINER-level duration (ffprobe's format.duration) --
    it reflects whichever stream runs longest, which is very often the audio
    stream, not the video stream. It must NOT be used to estimate an expected
    video frame count (duration_s * fps): a real case in this project's own
    testing showed this producing 1868.76 against an actual, exact,
    zero-frame-loss video_frame_count of 1866 (the audio ran ~0.09s longer
    than the video). Use video_frame_count for anything frame-accounting
    related; duration_s is display-only.
    """
    path: str
    duration_s: Optional[float] = None
    video_duration_s: Optional[float] = None
    video_frame_count: Optional[int] = None
    width: Optional[int] = None
    height: Optional[int] = None
    fps_num: Optional[int] = None
    fps_den: Optional[int] = None
    video_stream_indices: list[int] = field(default_factory=list)
    audio_stream_indices: list[int] = field(default_factory=list)

    @property
    def aspect_ratio(self) -> Optional[float]:
        if self.width and self.height:
            return self.width / self.height
        return None


@dataclass
class PictureSettings:
    mode: PictureMode = "fit"
    # custom_crop: (x, y, w, h) in source pixels, only used when mode == "custom_crop"
    custom_crop: Optional[tuple[int, int, int, int]] = None
    target_fps: Optional[str] = None  # e.g. "30000/1001"; None = keep source fps


@dataclass
class QualitySettings:
    """Advanced values are Candidate B's own accepted settings, carried over
    verbatim from this investigation as an EXPERIMENTAL PLACEHOLDER -- not a
    claim that these are final, Nintendo-equivalent, or even the right choice
    for every source. All four preset names currently resolve to the same
    underlying values; real per-preset differentiation is unresolved."""
    preset: QualityPresetName = "balanced"
    revision: int = 0  # bumped whenever a preset's resolved values change
    qp: int = 25
    mobi_qyx: int = 3
    q_rdo: bool = True
    chroma_dz: Optional[int] = None  # None = unset -> encoder's own MOBI_DZ default
    threads: int = 1


@dataclass
class AudioSettings:
    mode: AudioMode = "recommended"
    stream_index: Optional[int] = None  # None = ffmpeg default stream selection
    downmix_to_stereo: bool = True


@dataclass
class OutputSettings:
    path: str
    temp_dir: Optional[str] = None  # None = same directory as `path`


@dataclass
class VerificationPolicy:
    software_decode: bool = True
    sha256: bool = True


@dataclass
class EncodeJob:
    schema_version: int = JOB_SCHEMA_VERSION
    source: Optional[SourceRef] = None
    video_stream_index: Optional[int] = None
    audio_stream_index: Optional[int] = None
    target: Target = "moflex"
    picture: PictureSettings = field(default_factory=PictureSettings)
    quality: QualitySettings = field(default_factory=QualitySettings)
    audio: AudioSettings = field(default_factory=AudioSettings)
    output: OutputSettings = field(default_factory=lambda: OutputSettings(path=""))
    verification_policy: VerificationPolicy = field(default_factory=VerificationPolicy)

    @property
    def partial_output_path(self) -> str:
        return self.output.path + ".partial"
