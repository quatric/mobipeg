"""Compares the new mobipeg3ds backend's resolved 3DS-moflex arguments
against the actual behavior of the reference C:\\dev\\mobipeg\\encode.py,
read directly (not from memory) to build the comparison.

encode.py's own moflex path (verified by direct source read):
    mode, dmx, scale, moaud, cvc = "vid", "moflex", "400:240", 1, "mobiclip"
    enc_opts = ["-mo_audio", audio, "-c:v", "mobiclip", "-mobiclip", "1"]
    filters = ["scale=400:240"]          # NOTE: no force_original_aspect_ratio, no pad
    cmd = [FFENC, "-nostdin", "-y", "-i", inp, "-vf", "scale=400:240"] + enc_opts + [container]

This test asserts the elements that SHOULD match (the shared codec/format
flags) and explicitly documents, rather than silently ignoring, the real
differences this comparison surfaced:

  1. encode.py never passes -qp or -mobi_qyx under any code path -- it runs
     at whatever the patched encoder's *default* is. The new backend always
     passes both explicitly, resolved from the job's quality preset
     (Candidate B's QP25/QYX3 placeholder). These are NOT expected to match,
     and asserting they did would be asserting a false equivalence.
  2. encode.py's scale filter is a bare `scale=400:240` with no
     force_original_aspect_ratio/pad -- it silently *distorts* (stretches)
     any source whose aspect ratio isn't already exactly 400:240 (5:3). The
     new backend's `fit`/`fill` modes preserve aspect ratio by construction.
     This is a real, material behavior difference, not a bug in either
     comparison -- documented here because it was found while writing this
     test, not assumed beforehand.
  3. encode.py writes directly to the final container path; the new backend
     writes to `<output>.partial` and renames only after a successful run.
"""
from __future__ import annotations

from mobipeg3ds.backend import _picture_filter, resolve_args
from mobipeg3ds.job import EncodeJob, OutputSettings, PictureSettings, SourceRef


def _fake_source(width: int, height: int) -> SourceRef:
    return SourceRef(
        path="C:/dev/MIVF/anime.mp4",
        duration_s=62.292,
        width=width,
        height=height,
        fps_num=30,
        fps_den=1,
        video_stream_indices=[0],
        audio_stream_indices=[1],
    )


def _job(width: int, height: int, mode: str = "fit") -> EncodeJob:
    job = EncodeJob(
        source=_fake_source(width, height),
        target="moflex",
        output=OutputSettings(path="C:/dev/MIVF/mobipeg-3ds-public/tests/_test_out.moflex"),
    )
    job.picture = PictureSettings(mode=mode)
    return job


def test_shared_codec_flags_match_encode_py():
    """The elements encode.py DOES set must appear identically in ours."""
    resolved = resolve_args(_job(1280, 720))
    argv = resolved.argv
    assert "-c:v" in argv and argv[argv.index("-c:v") + 1] == "mobiclip"
    assert "-mobiclip" in argv and argv[argv.index("-mobiclip") + 1] == "1"
    assert "-mo_audio" in argv and argv[argv.index("-mo_audio") + 1] == "adpcm"


def test_qp_qyx_are_a_documented_new_addition_not_in_encode_py():
    """encode.py never passes -qp/-mobi_qyx (grep-verified against the real
    file). The new backend always does. This test locks in that the new
    backend explicitly sets both -- if this regresses to "no qp/qyx flags",
    encodes would silently fall back to the patched encoder's unstated
    defaults, exactly like encode.py's undocumented behavior."""
    resolved = resolve_args(_job(1280, 720))
    argv = resolved.argv
    assert "-qp" in argv, "new backend must always set -qp explicitly (encode.py does not)"
    assert "-mobi_qyx" in argv, "new backend must always set -mobi_qyx explicitly (encode.py does not)"
    assert argv[argv.index("-qp") + 1] == "25"       # Candidate B placeholder
    assert argv[argv.index("-mobi_qyx") + 1] == "3"  # Candidate B placeholder


def test_fit_mode_preserves_aspect_ratio_unlike_encode_py():
    """encode.py's bare scale=400:240 stretches a 16:9 source to 5:3. The
    new backend's 'fit' mode must not -- it must fit-and-letterbox."""
    filt = _picture_filter(_job(1280, 720, mode="fit"))
    assert "force_original_aspect_ratio=decrease" in filt
    assert "pad=400:240" in filt
    # encode.py's actual filter, for explicit contrast:
    encode_py_filter = "scale=400:240"
    assert filt != encode_py_filter


def test_fill_mode_crops_instead_of_stretching():
    filt = _picture_filter(_job(1280, 720, mode="fill"))
    assert "force_original_aspect_ratio=increase" in filt
    assert "crop=400:240" in filt


def test_output_uses_partial_suffix_unlike_encode_py():
    """encode.py writes directly to the final path; the new backend must
    never do this -- .moflex.partial until the encode (and, at the CLI
    level, verification) succeeds."""
    resolved = resolve_args(_job(1280, 720))
    assert resolved.argv[-1].endswith(".moflex.partial")


def test_moflex3d_is_explicitly_rejected_not_silently_handled():
    from mobipeg3ds.backend import UnsupportedTargetError
    import pytest

    job = _job(1280, 720)
    job.target = "moflex3d"
    with pytest.raises(UnsupportedTargetError, match="deferred"):
        resolve_args(job)
