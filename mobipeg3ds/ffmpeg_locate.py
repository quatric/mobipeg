"""Locates the authoritative patched ffmpeg/ffprobe binaries.

Resolution order (first match wins), never silently falls back to a
different, unpatched system ffmpeg without saying so:
  1. MOBIPEG3DS_FFMPEG_DIR environment variable, if set.
  2. The known mobipeg_release prebuilt binary directory (this investigation's
     own reference build), if present on this machine.
  3. `ffmpeg`/`ffprobe` on PATH -- NOT guaranteed to be the mobiclip-patched
     build; a warning is raised so callers can decide whether that matters.
"""
from __future__ import annotations

import os
import shutil
from pathlib import Path

_KNOWN_RELEASE_DIR = Path(
    r"C:\dev\mobipeg_release\deep_extract\root_mobipeg-windows-x86_64"
    r"\unzipped_mobipeg-windows-x86_64"
)


class FfmpegNotFoundError(RuntimeError):
    pass


def _candidate_dirs() -> list[Path]:
    dirs = []
    env_dir = os.environ.get("MOBIPEG3DS_FFMPEG_DIR")
    if env_dir:
        dirs.append(Path(env_dir))
    if _KNOWN_RELEASE_DIR.is_dir():
        dirs.append(_KNOWN_RELEASE_DIR)
    return dirs


def find_ffmpeg() -> Path:
    for d in _candidate_dirs():
        exe = d / "ffmpeg.exe"
        if exe.is_file():
            return exe
    on_path = shutil.which("ffmpeg")
    if on_path:
        return Path(on_path)
    raise FfmpegNotFoundError(
        "no ffmpeg.exe found (checked MOBIPEG3DS_FFMPEG_DIR, the known "
        "mobipeg_release build, and PATH). Set MOBIPEG3DS_FFMPEG_DIR to the "
        "directory containing the mobiclip-patched ffmpeg.exe."
    )


def find_ffprobe() -> Path:
    for d in _candidate_dirs():
        exe = d / "ffprobe.exe"
        if exe.is_file():
            return exe
    on_path = shutil.which("ffprobe")
    if on_path:
        return Path(on_path)
    raise FfmpegNotFoundError(
        "no ffprobe.exe found (checked MOBIPEG3DS_FFMPEG_DIR, the known "
        "mobipeg_release build, and PATH)."
    )


def is_patched_source(ffmpeg_path: Path) -> bool:
    """Best-effort: True if this ffmpeg came from the known release build
    directory (i.e. is known to be the mobiclip-patched binary), False if it
    was resolved from bare PATH and its provenance is unconfirmed."""
    try:
        return ffmpeg_path.parent == _KNOWN_RELEASE_DIR
    except Exception:
        return False
