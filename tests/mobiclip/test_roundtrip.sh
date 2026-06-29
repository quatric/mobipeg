#!/bin/bash
# Copyright (c) 2026 quatric - quatricsoftware@gmail.com
set -e

# Get the directory of this script
DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"

# Export paths
if [ -f "$DIR/../../ffmpeg.exe" ]; then
    export FFMPEG="$DIR/../../ffmpeg.exe"
else
    export FFMPEG="$DIR/../../ffmpeg"
fi
export OUTDIR="$DIR/tmp_output"

mkdir -p "$OUTDIR"

cd "$DIR"

if [ ! -f "stupi.mp4" ]; then
    echo "Error: stupi.mp4 must be present in $DIR"
    exit 1
fi

export ENCODE_PY="$DIR/../../encode.py"

if [ ! -f "$ENCODE_PY" ]; then
    echo "Error: encode.py must be present in $DIR/../../"
    exit 1
fi

echo "Running encoding & decoding tests using encode.py..."

"$ENCODE_PY" mods adpcm >/dev/null 2>&1
"$ENCODE_PY" mods pcm >/dev/null 2>&1
"$ENCODE_PY" mods fastaudio >/dev/null 2>&1
"$ENCODE_PY" moflex3d pcm >/dev/null 2>&1
"$ENCODE_PY" moflex3d fastaudio >/dev/null 2>&1
"$ENCODE_PY" moflex3d adpcm >/dev/null 2>&1
"$ENCODE_PY" moflex pcm >/dev/null 2>&1
"$ENCODE_PY" moflex fastaudio >/dev/null 2>&1
"$ENCODE_PY" moflex adpcm >/dev/null 2>&1
"$ENCODE_PY" mo adpcm >/dev/null 2>&1
"$ENCODE_PY" mo vorbis >/dev/null 2>&1
"$ENCODE_PY" mo fastaudio >/dev/null 2>&1
"$ENCODE_PY" mo pcm >/dev/null 2>&1

echo "All encoding steps completed successfully! ✅"
exit 0
