"""Mobipeg 3DS -- a dedicated Nintendo 3DS MOFLEX encoder.

Experimental Preview. Independent community fork of Mobipeg
(https://github.com/quatric/mobipeg), itself a fork of FFmpeg. Not affiliated
with or endorsed by Nintendo.

This package is the single authoritative backend shared by the CLI and the
future GUI: it resolves a versioned EncodeJob into the exact ffmpeg argument
array, and nothing else constructs those arguments independently.
"""

__version__ = "0.0.1-experimental"
