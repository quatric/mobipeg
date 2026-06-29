#!/usr/bin/env bash
# Copyright (c) 2026 quatric - quatricsoftware@gmail.com
#
# moon.py — round-trip a video/audio through any Nintendo container we support,
#           or decode a Flipnote, then produce a watchable .mp4 / playable .wav.
#
#   ./moon.py [format] [audio] [input]
#
# VIDEO round-trip  (encode input -> container -> decode back to .mp4):
#   mo        Wii   (.mo)      mobiclip video        [default]
#   moflex    3DS   (.moflex)  mobiclip video
#   moflex3d  3DS   (.moflex)  stereoscopic: one video, eyes packed side-by-side
#   mods      DS    (.mods)    mobiclip video
#   vx        DS    (.vx)      mobiclip video (Majesco VXDS)
#   vx2       DS    (.vx2)     mobiclip video (Majesco VX2)
#   thp       GC/Wii (.thp)   MJPEG video
#   ty        TiVo  (.ty)     MPEG-2 video + MP2 audio
#
# AUDIO round-trip  (encode input's audio -> container -> decode to .wav):
#   bns     Wii Banner Sound  (.bns)    THP ADPCM
#   brstm   Revolution Stream (.brstm)  THP ADPCM (big-endian)
#   bfstm   Cafe Stream       (.bfstm)  THP ADPCM (little-endian; 3DS/Wii U/Switch)
#   adx     CRI ADX           (.adx)    CRI ADPCM
#
# DECODE-only  (no encoder exists — input must already be that file):
#   kwz      Flipnote Studio 3D   (.kwz)        -> animation to .mp4
#   ppm      Flipnote Studio DS   (.ppm)        -> animation to .mp4
#   hvqm4    Hudson HVQM4 / H4M   (.h4m/.hvqm)  -> video to .mp4
#   adp      Nintendo DTK ADPCM   (.adp)        -> audio to .wav
#   hca      CRI HCA              (.hca)        -> audio to .wav
#   nus3bank Namco NUS3BANK        (.nus3bank)   -> extracts embedded streams (raw)
#
#   audio : adpcm | fastaudio | pcm | vorbis  (default adpcm; mo/moflex/mods only)
#           codebook  (mods only) FastAudio-codebook / "SX": encoded by the official
#                     AviToMobiclipDs.exe via wine (our ffmpeg has no SX encoder); its
#                     -debugsound gives the decoded .wav, so the round-trip completes.
#                     Needs a MODS-fourcc video AVI as the carrier (we can't make one
#                     on macOS); set MODS_AVI=... (default ~/Downloads/stupi.yu1.avi).
#   SCALE=WxH  override the per-format scale (e.g. SCALE=320x240 ./moon.py moflex ...)
#   input : source for round-trip (default test111.mp4), or the .kwz/.ppm file.
#           for moflex3d this is the LEFT eye; pass the RIGHT eye as a 4th arg:
#             ./moon.py moflex3d adpcm left.mp4 right.mp4
#           decode hstacks both eyes (left|right) into the watchable .mp4.
#
# Everything runs in ONE patched ffmpeg (no pipe to a second binary). Decodes
# force the demuxer (-f) since some formats are headerless. Video decodes
# re-encode with native MPEG-4 (this build's libx264 is MobiClip-only and
# corrupts normal H.264); MPEG-4 in .mp4 plays everywhere.
set -u

ffenc="${FFMPEG:-/Users/larsen/.gemini/antigravity/scratch/ffmpeg_encoder/ffmpeg}"
outdir="${OUTDIR:-/Volumes/SSD/tmp}"

fmt="${1:-mo}"
audio="${2:-adpcm}"
input="${3:-}"
input2="${4:-}"        # right-eye source for moflex3d (4th positional)
scale_ovr="${SCALE:-}" # override per-format scale: SCALE=WxH ./moon.py ...

# format -> mode | demuxer name | scale (video) | -mo_audio? (1/0)
case "$fmt" in
  mo)           mode=vid;     dmx=mobiclip_mo; scale="624:352"; moaud=1; cvc=mobiclip ;;
  moflex)       mode=vid;     dmx=moflex;      scale="400:240"; moaud=1; cvc=mobiclip ;;
  moflex3d)     mode=vid3d;   dmx=moflex;      scale="400:240"; moaud=1; cvc=mobiclip ;;
  mods)         mode=vid;     dmx=mods;        scale="256:192"; moaud=1; cvc=mobiclip ;;
  vx)           mode=vid;     dmx=vx;          scale="256:192"; moaud=0; cvc= ;;
  vx2)          mode=vid;     dmx=vx2;         scale="256:192"; moaud=0; cvc= ;;
  thp)          mode=vid;     dmx=thp;         scale="640:480"; moaud=0; cvc=mjpeg ;;
  ty)           mode=vid;     dmx=ty;          scale="";        moaud=0; cvc= ;;
  bns)          mode=aud;     dmx=bns ;;
  brstm)        mode=aud;     dmx=brstm ;;
  bfstm)        mode=aud;     dmx=bfstm ;;
  adx)          mode=aud;     dmx=adx ;;
  kwz)          mode=dec;     dmx=kwz ;;
  ppm)          mode=dec;     dmx=ppm_flipnote ;;
  hvqm4|h4m)   fmt=hvqm4;    mode=dec;        dmx=hvqm4 ;;
  adp)          mode=deca;    dmx=adp ;;
  hca)          mode=deca;    dmx=hca ;;
  nus3bank|nus3) fmt=nus3bank; mode=extract;   dmx=nus3bank ;;
  *)
    echo "unknown format '$fmt' (mo|moflex|mods|vx|vx2|thp|ty|bns|brstm|bfstm|adx|kwz|ppm|hvqm4|adp|hca|nus3bank)"
    exit 2
    ;;
esac
[ -n "$scale_ovr" ] && scale="$(echo "$scale_ovr" | tr 'x' ':')"
mkdir -p "$outdir"

if [ "$mode" = dec ]; then
  input="${input:-/Volumes/SSD/larsen/Downloads/sample.$fmt}"
  [ -f "$input" ] || { echo "need a file: ./moon.py $fmt - <file.$fmt>"; exit 2; }
  watch="$outdir/decoded_${fmt}.mp4"
  ycgco_dec_vf=""
  [ "$fmt" = "mods" ] && ycgco_dec_vf="-vf setparams=colorspace=bt709,format=yuv444p,mergeplanes=0x000102:gbrp,geq=r='clip(g(X\,Y)-b(X\,Y)+r(X\,Y),0,255)':g='clip(g(X\,Y)+b(X\,Y)-128,0,255)':b='clip(g(X\,Y)-b(X\,Y)-r(X\,Y)+256,0,255)',format=yuv420p"
  echo ">> decoding  $input  ->  $watch"
  $ffenc -y -f "$dmx" -i "$input" $ycgco_dec_vf -c:v mpeg4 -q:v 3 -c:a aac "$watch" \
    || $ffenc -y -f "$dmx" -i "$input" $ycgco_dec_vf -map 0:v -c:v mpeg4 -q:v 3 "$watch" || exit 1
  echo; echo "decode complete:"; ls -la "$watch"
  exit 0
fi

if [ "$mode" = deca ]; then
  input="${input:-/Volumes/SSD/larsen/Downloads/sample.$fmt}"
  [ -f "$input" ] || { echo "need a file: ./moon.py $fmt - <file.$fmt>"; exit 2; }
  out="$outdir/decoded_${fmt}.wav"
  echo ">> decoding audio  $input  ->  $out"
  $ffenc -y -f "$dmx" -i "$input" "$out" || exit 1
  echo; echo "decode complete:"; ls -la "$out"
  exit 0
fi

if [ "$mode" = extract ]; then
  input="${input:-/Volumes/SSD/larsen/Downloads/sample.$fmt}"
  [ -f "$input" ] || { echo "need a file: ./moon.py $fmt - <file.$fmt>"; exit 2; }
  echo ">> NUS3BANK streams in  $input:"
  $ffenc -hide_banner -f "$dmx" -i "$input" 2>&1 | grep -E "Stream|title|nus3_codec" | sed 's/^/  /'
  echo ">> extracting embedded streams -> $outdir/nus3_stream*.bin"
  # Each embedded stream is exposed raw; copy each out for inspection/routing.
  n=$($ffenc -hide_banner -f "$dmx" -i "$input" 2>&1 | grep -c "Stream #0:")
  for i in $(seq 0 $((n > 0 ? n - 1 : 0))); do
    $ffenc -y -f "$dmx" -i "$input" -map 0:$i -c copy -f data \
      "$outdir/nus3_stream$i.bin" 2>/dev/null
  done
  echo; echo "extract complete:"; ls -la "$outdir"/nus3_stream*.bin 2>/dev/null
  exit 0
fi

input="${input:-stupi.mp4}"
[ -f "$input" ] || { echo "input not found: $input"; exit 2; }

# DS .mods FastAudio-codebook ("SX"): our patched ffmpeg can neither encode nor decode
# this codec, so route through the official AviToMobiclipDs.exe under wine. It ingests a
# MODS-fourcc video AVI + a 16-bit PCM wav and writes the .mods; -debugsound dumps its own
# decoded PCM, which IS our round-trip output (no ffmpeg decode of SX needed). The encoder
# trains the per-channel VQ codebook itself.
if [ "$fmt" = mods ] && [ "$audio" = codebook ]; then
  tools=/Users/larsen/sx-port/tools
  enc="$tools/AviToMobiclipDs.exe"
  ffprobe=/opt/homebrew/bin/ffprobe
  modsavi="${MODS_AVI:-/Users/larsen/Downloads/stupi.yu1.avi}"
  command -v wine >/dev/null 2>&1 || { echo "codebook needs wine (brew install --cask wine-stable)"; exit 2; }
  [ -f "$enc" ]     || { echo "missing official encoder: $enc"; exit 2; }
  [ -f "$modsavi" ] || { echo "need a MODS-fourcc carrier AVI (set MODS_AVI=...): $modsavi"; exit 2; }
  stem="$outdir/roundtrip_mods_codebook"
  container="$stem.mods"
  watch="$stem.wav"
  work="$outdir/.sx_work"; mkdir -p "$work"
  # Match audio and video length: encoder muxes per-frame, so trim both to the shorter.
  vdur=$("$ffprobe" -v error -show_entries format=duration -of default=nk=1:nw=1 "$modsavi" 2>/dev/null)
  adur=$("$ffprobe" -v error -show_entries format=duration -of default=nk=1:nw=1 "$input"  2>/dev/null)
  t=$(awk -v a="$adur" -v v="$vdur" 'BEGIN{print (a<v?a:v)}')
  echo ">> codebook(SX) via official AviToMobiclipDs (wine); carrier=$modsavi, ${t}s"
  $ffenc -y -t "$t" -i "$input"   -vn -ac 2 -ar 48000 -c:a pcm_s16le "$work/aud.wav" || exit 1
  $ffenc -y -t "$t" -i "$modsavi" -an -c:v copy                      "$work/vid.avi" || exit 1
  echo ">> encoding  $input  ->  $container  (SX audio; building codebook...)"
  ( cd "$tools" && WINEDEBUG=-all wine AviToMobiclipDs.exe \
      -in "$work/vid.avi" -audio "$work/aud.wav" -out "$container" -debugsound "$watch" ) \
    2>&1 | tr '\r' '\n' | grep -iE "error|^Converting file...100|codebook...100" | tail -3
  [ -s "$container" ] || { echo "encode failed (no .mods produced)"; exit 1; }
  echo; echo "codebook round-trip complete (decode = encoder -debugsound):"; ls -la "$container" "$watch"; echo
  python3 - "$container" <<'PY'
import struct,sys
d=open(sys.argv[1],'rb').read()
ac=struct.unpack_from('<H',d,0x18)[0]; ch=struct.unpack_from('<H',d,0x1a)[0]
rate=struct.unpack_from('<I',d,0x1c)[0]; cb=struct.unpack_from('<I',d,0x24)[0]
print(f"  .mods: AudioCodec={ac} (1=SX/codebook) channels={ch} rate={rate} codebook@0x{cb:x}")
PY
  exit 0
fi

if [ "$mode" = aud ]; then
  container="$outdir/roundtrip_${fmt}.$fmt"
  out="$outdir/roundtrip_${fmt}.wav"
  echo ">> encoding audio  $input  ->  $container"
  $ffenc -y -i "$input" -vn "$container" || exit 1
  echo ">> decoding  $container  ->  $out"
  $ffenc -y -f "$dmx" -i "$container" "$out" || exit 1
  echo; echo "audio round-trip complete:"; ls -la "$container" "$out"; echo
  $ffenc -hide_banner -f "$dmx" -i "$container" 2>&1 | grep -E "Stream|Duration" | sed 's/^/  /'
  exit 0
fi

# Stereoscopic 3D moflex round-trip.
#
# 3DS 3D is NOT two video chunks (the player only renders the first chunk).  It
# is a SINGLE mobiclip video stream whose frames pack BOTH eyes, plus a type-3
# "VideoWithLayout" descriptor whose layout byte tells the 3DS how the eyes are
# arranged.  (Confirmed from Gericom/MobiclipDecoder MoLiveStreamVideoWithLayout
# and the retail "Nintendo Show 3D" file, a single 400x240 stream, layout 0.)
#
#   VideoLayout enum (low nibble of the layout byte):
#     0 Interleave3DLeftFirst   1 Interleave3DRightFirst
#     2 TopToBottom3DLeftFirst  3 TopToBottom3DRightFirst
#     4 SideBySide3DLeftFirst   5 SideBySide3DRightFirst   6 Simple2D
#
# We hstack the two eyes (L|R) into one 800x240 frame and tag it
# SideBySide3DLeftFirst (4).  Override with MO3D_LAYOUT=<n>.
if [ "$mode" = vid3d ]; then
  input="${input:-stupi.mp4}"
  input2="${input2:-$input}"  # default: same clip for both eyes (path test)
  [ -f "$input" ]  || { echo "left input not found: $input";  exit 2; }
  [ -f "$input2" ] || { echo "right input not found: $input2"; exit 2; }
  layout="${MO3D_LAYOUT:-4}" # 4 = SideBySide3DLeftFirst; override with MO3D_LAYOUT=<n>
  stem="$outdir/roundtrip_moflex3d_${audio}"
  container="$stem.moflex"
  watch="$stem.mp4"
  eyew=$(echo "$scale" | cut -d: -f1) # per-eye width (e.g. 400)
  eyeh=$(echo "$scale" | cut -d: -f2) # per-eye height (e.g. 240)
  echo ">> encoding 3D  L=$input  R=$input2  ->  $container  (${eyew}x${eyeh} per eye, side-by-side, layout=$layout, audio=$audio)"
  # One video stream: scale each eye then hstack into a single side-by-side frame.
  $ffenc -y -i "$input" -i "$input2" \
    -filter_complex "[0:v:0]scale=${eyew}:${eyeh}[l];[1:v:0]scale=${eyew}:${eyeh}[r];[l][r]hstack=inputs=2[v]" \
    -map "[v]" -map 0:a:0? \
    -c:v "$cvc" -mo_audio "$audio" -mo_layout "$layout" "$container" || exit 1
  ycgco_dec_vf=""
  [ "$fmt" = "mods" ] && ycgco_dec_vf="-vf setparams=colorspace=bt709,format=yuv444p,mergeplanes=0x000102:gbrp,geq=r='clip(g(X\,Y)-b(X\,Y)+r(X\,Y),0,255)':g='clip(g(X\,Y)+b(X\,Y)-128,0,255)':b='clip(g(X\,Y)-b(X\,Y)-r(X\,Y)+256,0,255)',format=yuv420p"
  echo ">> decoding  $container  ->  $watch  (single SBS video, mpeg4)"
  # -loglevel error hides the harmless audio-DTS reordering warnings the AAC
  # encoder emits on the demuxer's per-block audio timestamps.
  $ffenc -y -loglevel error -f "$dmx" -i "$container" \
    -map 0:0 $ycgco_dec_vf -c:v mpeg4 -q:v 3 -map 0:a:0? -c:a aac "$watch" \
    || $ffenc -y -loglevel error -f "$dmx" -i "$container" $ycgco_dec_vf -map 0:v -c:v mpeg4 -q:v 3 "$watch" || exit 1
  echo; echo "3D round-trip complete (frame is left|right side-by-side):"; ls -la "$container" "$watch"; echo
  $ffenc -hide_banner -f "$dmx" -i "$container" 2>&1 | grep -E "Stream|Duration" | sed 's/^/  /'
  exit 0
fi

# Video round-trip.
stem="$outdir/roundtrip_${fmt}_${audio}"
container="$stem.$fmt"
watch="$stem.mp4"
[ "$moaud" = 1 ] && audio_opt="-mo_audio $audio" || audio_opt=""
[ -n "$cvc" ] && cvc_opt="-c:v $cvc" || cvc_opt=""
# DS .mods uses a DIFFERENT Mobiclip variant than Wii(.mo)/3DS(.moflex): the
# "MODS" VLC tables (-mobiclip 2), the moflex header bit cleared (-moflex 0),
# and YCgCo colorspace. Without these the DS decodes our stream with the wrong
# tables -> static / swapped colors / drift. mo & moflex keep the defaults.
# DS .mods: place keyframes only at real scene changes (like retail/Wii/3DS),
# NOT on a fixed periodic GOP. The DS re-primes its ADPCM audio decoder at every
# video keyframe, so a forced periodic keyframe (ffmpeg's default GOP) injects a
# discontinuity every N frames -> audible periodic warble ("monster voice").
# -g 100000 disables the forced cadence; scene-cut detection stays on. Verified
# on hardware: scene-cut-only audio is clean. mo/moflex keep ffmpeg defaults.
[ "$fmt" = mods ] && cvc_opt="$cvc_opt -mobiclip 2 -moflex 0 -g 100000"
# DS .mods FastAudio: force a SINGLE keyframe (-sc_threshold 0 disables scene-cut
# keyframes; only frame 0 stays intra). Unlike ADPCM (which carries an inline
# predictor/step re-prime header on every keyframe), enhanced FastAudio has NO
# per-keyframe audio header, so the DS RESETS its FastAudio lattice state at each
# video keyframe. Our encoder keeps continuous audio state, so any mid-stream
# keyframe became a loud ringing transient (15 of them ~= "earrape"). With one
# keyframe there is no mid-stream reset -> clean. Verified on the America's Test
# Kitchen game decoder: 1-keyframe FastAudio plays fine, 15-keyframe earrapes.
# (Retail uses 2 keyframes and its encoder resets audio state at them; a proper
# fix would reset our FastAudio encoder state at keyframes to allow seeking.)
[ "$fmt" = mods ] && [ "$audio" = fastaudio ] && cvc_opt="$cvc_opt -sc_threshold 0"

# The Wii is NTSC: its video clock is 59.94 Hz, so frame rates must be the
# NTSC-fractional values (n*1000/1001), NOT integers. Retail clips confirm this
# (Internet Channel video = 29.97 = 30000/1001; Nintendo Week = 23.976 =
# 24000/1001). Using an integer rate like 30.0 runs 0.1%% fast versus the Wii's
# 29.97 and the audio drifts ~0.18 s every 3 min. Default .mo output to 29.97.
# (The Wii also does 59.94 fps Mobiclip — use 60000/1001 once that's wired up.)
# THP: match retail exactly — 29.97 fps (30000/1001). Every shipping WW:SM
# cutscene (AshleyPromotion/Prologue/Epilogue) is 29.970 fps, 32028 Hz, ~1078
# samples/frame, and plays at correct speed. The game's cutscene loop pulls one
# THP frame per game tick at a fixed ~29.97 fps (NTSC, one frame per 2 VBLANKs),
# independent of the file's fps field. Encoding at 60 fps gives 2x the frames,
# so they get consumed at ~30 fps -> 0.5x speed (audio drags in lockstep because
# the per-frame audio chunk stretches to the same cadence). 29.97 fixes it.
# Also clear cvc_opt — codec is specified via thp_opts below.
[ "$fmt" = thp ] && { fps_filter="fps=30000/1001"; cvc_opt=""; }
fps_filter=""
case "$fmt" in mo) fps_filter="fps=30000/1001" ;; esac
filters=""
[ -n "$scale" ]      && filters="scale=$scale"
[ -n "$fps_filter" ] && filters="${filters:+$filters,}$fps_filter"
# DS .mods stores video in YCgCo, not YCbCr (decoder: !moflex => AVCOL_SPC_YCGCO).
# The mobiclip encoder does NOT convert, so we must feed it real YCgCo planes:
#   Y'=(R+2G+B)/4 in the Y plane, Cg=(2G-R-B)/4+128 in U, Co=(R-B)/2+128 in V.
# Build the planes in RGB via geq (no color matrix), then mergeplanes into yuv.
# Without this the DS applies YCgCo->RGB to YCbCr data -> wrong/garbled colors.
if [ "$fmt" = mods ]; then
  ycgco="format=gbrp,geq=g='(r(X,Y)+2*g(X,Y)+b(X,Y))/4':b='(2*g(X,Y)-r(X,Y)-b(X,Y))/4+128':r='(r(X,Y)-b(X,Y))/2+128',mergeplanes=0x000102:yuv444p,format=yuv420p"
  filters="${filters:+$filters,}$ycgco"
fi
[ -n "$filters" ] && vf="-vf $filters" || vf=""

echo ">> encoding  $input  -> $container  (${scale:-source}${fps_filter:+, $fps_filter}, audio=$audio)"
# THP uses MJPEG video; -pix_fmt yuvj420p gives the full-range YUV the Wii
# expects.  -q:v 4 = high quality JPEG.  The muxer handles DHT split,
# 32-byte frame alignment, and COM/APP stripping automatically.
# -ar 32028: match retail WW:SM THPs exactly (the Wii DSP's native ~32 kHz rate;
#            48000 forces a resample path and is not what any shipping file uses).
# -ac 2:     force stereo — mono THPs are not supported by the Wii player.
if [ "$fmt" = thp ]; then
  thp_opts="-c:v mjpeg -pix_fmt yuvj420p -q:v 4 -ar 32028 -ac 2"
else
  thp_opts=""
fi
# shellcheck disable=SC2086
$ffenc -y -i "$input" $vf $cvc_opt $thp_opts $audio_opt "$container" || exit 1
ycgco_dec_vf=""
[ "$fmt" = "mods" ] && ycgco_dec_vf="-vf setparams=colorspace=bt709,format=yuv444p,mergeplanes=0x000102:gbrp,geq=r='clip(g(X\,Y)-b(X\,Y)+r(X\,Y),0,255)':g='clip(g(X\,Y)+b(X\,Y)-128,0,255)':b='clip(g(X\,Y)-b(X\,Y)-r(X\,Y)+256,0,255)',format=yuv420p"
echo ">> decoding  $container  ->  $watch  (single binary, mpeg4)"
# -loglevel error hides harmless audio-DTS reordering warnings from the AAC encoder.
$ffenc -y -loglevel error -f "$dmx" -i "$container" $ycgco_dec_vf -c:v mpeg4 -q:v 3 -c:a aac "$watch" \
  || $ffenc -y -loglevel error -f "$dmx" -i "$container" $ycgco_dec_vf -map 0:v -c:v mpeg4 -q:v 3 "$watch" || exit 1
echo; echo "round-trip complete:"; ls -la "$container" "$watch"; echo
$ffenc -hide_banner -f "$dmx" -i "$container" 2>&1 | grep -E "Stream|Duration" | sed 's/^/  /'

# THP-specific: validate frame structure (32-byte alignment, navigation fields)
if [ "$fmt" = thp ]; then
  echo
  echo ">> validating THP frame structure..."
  python3 - "$container" << 'PYEOF'
import struct, sys
path = sys.argv[1]
with open(path, 'rb') as f: d = f.read()
rb32 = lambda o: struct.unpack_from('>I', d, o)[0]
comp = rb32(32)
has_audio = rb32(comp) > 1 and d[comp+5] == 1
fh = 16 if has_audio else 12
ff_off = rb32(40); ff_size = rb32(24)
cur = ff_off; own = ff_size; errs = 0; nframes = rb32(20); prev_own = own
for i in range(nframes):
    if cur + fh > len(d): errs += 1; break
    nxt = rb32(cur); prv = rb32(cur+4); img = rb32(cur+8)
    aud = rb32(cur+12) if has_audio else 0
    payload = img + aud
    padded = (fh + payload + 31) & ~31
    if own != padded:
        print(f'  frame {i}: own={own} != padded={padded} (misaligned!)'); errs += 1
    exp_prev = own if i == 0 else prev_own
    if prv != exp_prev:
        print(f'  frame {i}: prev={prv} != expected {exp_prev}'); errs += 1
    pad_start = cur + fh + payload   # after header + video + audio
    pad_bytes = d[pad_start : cur + own]
    if any(b != 0 for b in pad_bytes):
        print(f'  frame {i}: non-zero padding bytes'); errs += 1
    prev_own = own; cur += own; own = nxt
last_frame_off = cur - prev_own
if rb32(44) != last_frame_off:
    print(f'  lastFrameOff=0x{rb32(44):08x} expected 0x{last_frame_off:08x}'); errs += 1
if errs == 0:
    print(f'  {nframes} frames, all 32-byte aligned, navigation correct \u2713')
else:
    print(f'  {errs} structural errors found')
PYEOF
fi
