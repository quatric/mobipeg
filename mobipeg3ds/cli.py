"""mobipeg3ds -- Experimental Preview CLI.

Nintendo 3DS 2D MOFLEX only. No Wii/DS/generic-platform selection exists in
this surface at all (per the corrected product audit, that legacy lives only
in the reference encode.py/encode_gui.py, which this project does not carry
forward). Unrecognized platform/format requests get an explicit message, not
silent handling.
"""
from __future__ import annotations

import argparse
import sys

from . import __version__
from .backend import UnsupportedTargetError, resolve_args
from .job import EncodeJob, OutputSettings, VerificationPolicy
from .presets import PRESETS, resolve_preset
from .probe import ProbeError, probe
from .verify import verify

_DISCONTINUED_MESSAGE = "This Mobipeg 3DS release supports Nintendo 3DS MOFLEX output only."


def _cmd_version(_args: argparse.Namespace) -> int:
    print(f"mobipeg3ds {__version__} (Experimental Preview)")
    return 0


def _cmd_presets(_args: argparse.Namespace) -> int:
    for name, entry in PRESETS.items():
        flag = " [UNRESOLVED -- placeholder values]" if entry["unresolved"] else ""
        print(f"{name:10s} {entry['label']}{flag}")
        print(f"           {entry['description']}")
    return 0


def _cmd_inspect(args: argparse.Namespace) -> int:
    try:
        src = probe(args.input)
    except ProbeError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    print(f"path:            {src.path}")
    print(f"duration:        {src.duration_s:.3f}s" if src.duration_s else "duration:        unknown")
    print(f"resolution:      {src.width}x{src.height}" if src.width else "resolution:      unknown")
    if src.fps_num and src.fps_den:
        print(f"frame rate:      {src.fps_num}/{src.fps_den} ({src.fps_num/src.fps_den:.3f} fps)")
    print(f"video streams:   {src.video_stream_indices}")
    print(f"audio streams:   {src.audio_stream_indices}")
    if src.aspect_ratio:
        print(f"aspect ratio:    {src.aspect_ratio:.4f}")
    return 0


def _build_job(args: argparse.Namespace) -> EncodeJob:
    src = probe(args.input)
    quality = resolve_preset(args.preset)
    job = EncodeJob(
        source=src,
        target="moflex",
        output=OutputSettings(path=args.output),
        verification_policy=VerificationPolicy(software_decode=not args.no_verify),
    )
    job.quality = quality
    if args.video_only:
        job.audio.mode = "video_only"
    return job


def _cmd_encode(args: argparse.Namespace) -> int:
    try:
        job = _build_job(args)
    except ProbeError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    except KeyError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1

    try:
        resolved = resolve_args(job)
    except UnsupportedTargetError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1

    print(resolved.display_string())

    if args.dry_run:
        print("\n(dry run -- no encode started)")
        return 0

    import subprocess
    env_full = {**__import__("os").environ, **resolved.env}
    proc = subprocess.run(resolved.argv, env=env_full)
    if proc.returncode != 0:
        print(f"error: ffmpeg exited with code {proc.returncode}", file=sys.stderr)
        return proc.returncode

    import os as _os
    _os.replace(job.partial_output_path, job.output.path)

    if job.verification_policy.software_decode or job.verification_policy.sha256:
        result = verify(job.output.path, check_decode=job.verification_policy.software_decode)
        print(f"verify: exists={result.exists} sha256={result.sha256} "
              f"decodes_cleanly={result.decodes_cleanly} frames={result.decoded_frame_count}")
        if not result.passed:
            return 1
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="mobipeg3ds",
        description="Mobipeg 3DS -- Experimental Preview. Nintendo 3DS 2D MOFLEX only.",
    )
    sub = parser.add_subparsers(dest="command", required=True)

    p_inspect = sub.add_parser("inspect", help="probe a source file")
    p_inspect.add_argument("input")
    p_inspect.set_defaults(func=_cmd_inspect)

    p_encode = sub.add_parser("encode", help="encode a source file to 3DS MOFLEX")
    p_encode.add_argument("input")
    p_encode.add_argument("--preset", default="balanced", choices=list(PRESETS))
    p_encode.add_argument("--output", required=True)
    p_encode.add_argument("--dry-run", action="store_true",
                           help="resolve and print the exact ffmpeg command, do not encode")
    p_encode.add_argument("--video-only", action="store_true")
    p_encode.add_argument("--no-verify", action="store_true")
    p_encode.set_defaults(func=_cmd_encode)

    p_presets = sub.add_parser("presets", help="list quality presets")
    p_presets.set_defaults(func=_cmd_presets)

    p_version = sub.add_parser("version", help="show version")
    p_version.set_defaults(func=_cmd_version)

    return parser


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
