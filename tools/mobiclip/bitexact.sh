#!/bin/bash
# Verify that the encoder's own reconstruction matches what the decoder
# produces, byte for byte, across configurations.
M=~/Downloads/mobipeg
cd "$(dirname "$0")" || exit 1

# The .yuv inputs are generated, not checked in, and have twice been swept away
# by cleanup.  When they are missing reconcheck writes an empty r.yuv, cmp finds
# nothing to compare, and every case reports "FAIL (0 differing bytes)" -- which
# reads like a code regression but is not one.  Regenerate rather than let that
# happen, and refuse to run at all if the tools are missing.
for t in reconcheck; do
  [ -x "./$t" ] || { echo "missing ./$t -- build it first (see README.md)"; exit 2; }
done
gen() { # path, size, frames, lavfi source
  [ -s "$1" ] && return 0
  echo "  (regenerating missing $1)"
  "$M/ffmpeg" -y -hide_banner -loglevel error -f lavfi -i "$4" \
      -frames:v "$3" -pix_fmt yuv420p -f rawvideo "$1" || exit 2
}
gen src.yuv  256x192 60  "testsrc2=size=256x192:rate=30"
gen src2.yuv 320x240 120 "mandelbrot=size=320x240:rate=30"
gen src3.yuv 384x224 120 "testsrc=size=384x224:rate=30"
gen eb.yuv   384x288 150 "smptebars=size=384x288:rate=30"

pass=0; fail=0
run() {
  local yuv=$1 w=$2 h=$3 n=$4 qp=$5 mc=$6 d8=$7
  ./reconcheck "$yuv" "$w" "$h" "$n" "$qp" "$mc" "$d8" r.yuv >/dev/null 2>&1
  # An empty/short reconstruction means the harness itself broke; do not report
  # that as an encoder mismatch.
  if [ ! -s r.yuv ]; then
    printf "  ERROR %-12s %sx%s -- reconcheck produced no output\n" "$(basename $yuv)" "$w" "$h"
    fail=$((fail+1)); return
  fi
  local opt=""; [ "$d8" != "-1" ] && opt="-8x8dct $d8"
  $M/ffmpeg -y -hide_banner -loglevel error -f rawvideo -pix_fmt yuv420p -s "${w}x${h}" -r 30 -i "$yuv" \
      -c:v mobiclip -mobiclip "$mc" -moflex 0 -qp "$qp" -g 30 $opt -f mods b.mods 2>/dev/null
  $M/ffmpeg -y -hide_banner -loglevel error -i b.mods -pix_fmt yuv420p -f rawvideo d.yuv 2>/dev/null
  if cmp -s r.yuv d.yuv; then
    printf "  ok    %-12s %sx%s qp=%-3s mobiclip=%s 8x8dct=%-2s\n" "$(basename $yuv)" "$w" "$h" "$qp" "$mc" "$d8"
    pass=$((pass+1))
  else
    printf "  FAIL  %-12s %sx%s qp=%-3s mobiclip=%s 8x8dct=%-2s  (%s differing bytes)\n" \
        "$(basename $yuv)" "$w" "$h" "$qp" "$mc" "$d8" "$(cmp -l r.yuv d.yuv 2>/dev/null | wc -l | tr -d ' ')"
    fail=$((fail+1))
  fi
}
echo "encoder reconstruction vs decoder output:"
for qp in 12 16 20 26 32 40 48; do run src.yuv 256 192 60 $qp 2 -1; done
for d8 in 0 1;               do run src.yuv 256 192 60 20 2 $d8; done
for mc in 1 2 3;             do run src.yuv 256 192 60 20 $mc -1; done
run src2.yuv 320 240 120 18 2 -1
run src3.yuv 384 224 120 18 2 -1
run eb.yuv   384 288 150 18 2 -1
run eb.yuv   384 288 150 30 1 -1
# retail Wii Internet Channel content at the .mo native size, if present
[ -f te.yuv ] && run te.yuv 624 352 300 16 1 -1
[ -f te.yuv ] && run te.yuv 624 352 300 24 1 -1
echo "$pass passed, $fail failed"
exit $((fail > 0))
