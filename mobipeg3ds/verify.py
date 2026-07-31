"""Post-encode verification: existence, decode check, frame count, SHA-256.

Explicitly software-only. Never claims Azahar-equivalence or hardware proof --
see docs (README/architecture notes) for the three-tier separation this
project insists on: software verification, emulator testing, physical
hardware testing are different things and must not be conflated.
"""
from __future__ import annotations

import hashlib
import re
import subprocess
from dataclasses import dataclass
from pathlib import Path

from .ffmpeg_locate import find_ffmpeg


@dataclass
class VerificationResult:
    exists: bool
    sha256: str | None = None
    size_bytes: int | None = None
    decodes_cleanly: bool | None = None
    decoded_frame_count: int | None = None
    expected_frame_count: int | None = None
    decode_error: str | None = None

    @property
    def frame_count_matches(self) -> bool | None:
        """None if either count is unknown -- an unknown expectation must
        never be reported as a pass or a fail. Compares against the
        source's own video_frame_count (nb_frames), NOT a duration*fps
        estimate -- see SourceRef's docstring for why that estimate can be
        off by several frames without any actual frame loss."""
        if self.expected_frame_count is None or self.decoded_frame_count is None:
            return None
        return self.decoded_frame_count == self.expected_frame_count

    @property
    def passed(self) -> bool:
        if self.frame_count_matches is False:
            return False
        return self.exists and (self.decodes_cleanly is not False)


def sha256_of(path: str) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def verify(path: str, check_decode: bool = True, expected_frame_count: int | None = None) -> VerificationResult:
    p = Path(path)
    if not p.is_file():
        return VerificationResult(exists=False)

    result = VerificationResult(
        exists=True,
        size_bytes=p.stat().st_size,
        sha256=sha256_of(str(p)),
        expected_frame_count=expected_frame_count,
    )

    if not check_decode:
        return result

    ffmpeg = find_ffmpeg()
    cmd = [str(ffmpeg), "-nostdin", "-v", "info", "-i", str(p), "-f", "null", "-"]
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True, timeout=300)
    except subprocess.TimeoutExpired:
        result.decodes_cleanly = False
        result.decode_error = "decode timed out"
        return result

    stderr = proc.stderr
    if proc.returncode != 0:
        result.decodes_cleanly = False
        result.decode_error = stderr.strip()[-2000:]
        return result

    frame_matches = re.findall(r"frame=\s*(\d+)", stderr)
    result.decoded_frame_count = int(frame_matches[-1]) if frame_matches else None
    result.decodes_cleanly = True
    return result
