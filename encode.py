#!/usr/bin/env python3
import os
import sys
import subprocess
import glob
import argparse

# Config
if getattr(sys, 'frozen', False):
    ffmpeg_name = "ffmpeg.exe" if os.name == 'nt' else "ffmpeg"
    FFENC = os.path.join(sys._MEIPASS, ffmpeg_name)
else:
    FFENC = os.environ.get("FFMPEG", os.path.join(os.path.dirname(os.path.abspath(__file__)), "ffmpeg"))
DEFAULT_OUTDIR = os.environ.get("OUTDIR", "/Volumes/SSD/tmp")

def main():
    parser = argparse.ArgumentParser(description="Encode video/audio for Nintendo formats.")
    parser.add_argument("fmt", nargs="?", default="mo", help="Format (mo, moflex, moflex3d, mods)")
    parser.add_argument("audio", nargs="?", default="adpcm", help="Audio codec (or input file if fmt=decode)")
    parser.add_argument("input_file", nargs="?", default="", help="Input video/audio file")
    parser.add_argument("input2", nargs="?", default="", help="Second input file (for moflex3d right eye)")
    
    parser.add_argument("--scale", default="", help="Override scale (e.g. 320x240)")
    parser.add_argument("--layout", default="4", help="MO3D layout (default 4)")
    parser.add_argument("--roundtrip", action="store_true", help="Enable round-trip decoding validation")
    parser.add_argument("--outdir", default=DEFAULT_OUTDIR, help="Output directory for generated files")
    
    parsed = parser.parse_args()
    
    fmt = parsed.fmt
    audio = parsed.audio
    input_file = parsed.input_file
    input2 = parsed.input2
    scale_ovr = parsed.scale
    roundtrip = parsed.roundtrip
    layout_arg = parsed.layout
    OUTDIR = parsed.outdir

    # format -> mode | demuxer name | scale (video) | moaud? | cvc
    mode = ""
    dmx = ""
    scale = ""
    moaud = 0
    cvc = ""
    
    if fmt == "decode":
        mode = "decode"
        dmx = ""
        scale = ""
        moaud = 0
        cvc = ""
    elif fmt == "mo":
        mode, dmx, scale, moaud, cvc = "vid", "mobiclip_mo", "624:352", 1, "mobiclip"
    elif fmt == "moflex":
        mode, dmx, scale, moaud, cvc = "vid", "moflex", "400:240", 1, "mobiclip"
    elif fmt == "moflex3d":
        mode, dmx, scale, moaud, cvc = "vid3d", "moflex", "400:240", 1, "mobiclip"
    elif fmt == "mods":
        mode, dmx, scale, moaud, cvc = "vid", "mods", "256:192", 1, "mobiclip"
    else:
        print(f"unknown format '{fmt}' (decode|mo|moflex|moflex3d|mods)")
        sys.exit(2)

    if scale_ovr:
        scale = scale_ovr.replace("x", ":")
    elif audio == "vorbis":
        scale = "384:288"
    
    os.makedirs(OUTDIR, exist_ok=True)

    def run_cmd(cmd, check=True, hide_err=False):
        try:
            subprocess.run(cmd, check=check)
        except subprocess.CalledProcessError as e:
            if not hide_err:
                print(f"Command failed: {e}")
            return False
        return True

    def run_ffenc_fallback(cmd1, cmd2):
        if subprocess.run(cmd1).returncode != 0:
            if subprocess.run(cmd2).returncode != 0:
                sys.exit(1)

    if mode == "decode":
        # In decode mode, audio argument is actually the input file
        inp = audio
        if not os.path.isfile(inp):
            print(f"input not found: {inp}")
            sys.exit(2)
        watch = f"{OUTDIR}/decoded.mp4"
        print(f">> decoding  {inp}  ->  {watch}")
        cmd1 = [FFENC, "-nostdin", "-y", "-loglevel", "error", "-i", inp, "-c:v", "mpeg4", "-q:v", "3", "-c:a", "aac", watch]
        cmd2 = [FFENC, "-nostdin", "-y", "-loglevel", "error", "-i", inp, "-map", "0:v", "-c:v", "mpeg4", "-q:v", "3", watch]
        run_ffenc_fallback(cmd1, cmd2)
        print("\ndecode complete:")
        run_cmd(["ls", "-la", watch])
        sys.exit(0)



    if mode == "vid3d":
        inp = input_file or "stupi.mp4"
        inp2 = input2 or inp
        if not os.path.isfile(inp):
            print(f"left input not found: {inp}")
            sys.exit(2)
        if not os.path.isfile(inp2):
            print(f"right input not found: {inp2}")
            sys.exit(2)
            
        layout = layout_arg
        stem = f"{OUTDIR}/roundtrip_moflex3d_{audio}"
        container = f"{stem}.moflex"
        watch = f"{stem}.mp4"
        
        eyew, eyeh = scale.split(":")
        
        print(f">> encoding 3D  L={inp}  R={inp2}  ->  {container}  ({eyew}x{eyeh} per eye, side-by-side, layout={layout}, audio={audio})")
        
        filter_str = f"[0:v:0]scale={eyew}:{eyeh}[l];[1:v:0]scale={eyew}:{eyeh}[r];[l][r]hstack=inputs=2[v]"
        cmd = [FFENC, "-nostdin", "-y", "-i", inp, "-i", inp2, "-filter_complex", filter_str, "-map", "[v]", "-map", "0:a:0?", "-c:v", cvc, "-mo_audio", audio, "-mo_layout", str(layout), container]
        run_cmd(cmd) or sys.exit(1)
        
        if roundtrip:
            print(f">> decoding  {container}  ->  {watch}  (single SBS video, mpeg4)")
            cmd1 = [FFENC, "-y", "-loglevel", "error", "-f", dmx, "-i", container, "-map", "0:0", "-c:v", "mpeg4", "-q:v", "3", "-map", "0:a:0?", "-c:a", "aac", watch]
            cmd2 = [FFENC, "-y", "-loglevel", "error", "-f", dmx, "-i", container, "-map", "0:v", "-c:v", "mpeg4", "-q:v", "3", watch]
            run_ffenc_fallback(cmd1, cmd2)
            
            print("\n3D round-trip complete (frame is left|right side-by-side):")
            run_cmd(["ls", "-la", container, watch])
        else:
            print("\n3D encode complete:")
            run_cmd(["ls", "-la", container])
        print()
        
        p = subprocess.run([FFENC, "-hide_banner", "-f", dmx, "-i", container], stderr=subprocess.PIPE, text=True)
        for line in p.stderr.splitlines():
            if "Stream" in line or "Duration" in line:
                print("  " + line)
        sys.exit(0)

    # Video round-trip (2D)
    inp = input_file or "stupi.mp4"
    if not os.path.isfile(inp):
        print(f"input not found: {inp}")
        sys.exit(2)
        
    stem = f"{OUTDIR}/roundtrip_{fmt}_{audio}"
    container = f"{stem}.{fmt}"
    watch = f"{stem}.mp4"
    
    enc_opts = []
    if moaud == 1:
        enc_opts.extend(["-mo_audio", audio])
    if cvc:
        enc_opts.extend(["-c:v", cvc])
        
    if fmt == "mods":
        enc_opts.extend(["-mobiclip", "2", "-moflex", "0", "-g", "100000"])
        if audio == "fastaudio":
            enc_opts.extend(["-sc_threshold", "0"])
    elif fmt in ["mo", "moflex", "moflex3d"]:
        enc_opts.extend(["-mobiclip", "1"])

    fps_filter = ""
    if fmt == "mo":
        fps_filter = "fps=30000/1001"
        
    filters = []
    if scale:
        filters.append(f"scale={scale}")
    if fps_filter:
        filters.append(fps_filter)
        
    if fmt == "mods":
        ycgco = "format=gbrp,geq=g='(r(X,Y)+2*g(X,Y)+b(X,Y))/4':b='(2*g(X,Y)-r(X,Y)-b(X,Y))/4+128':r='(r(X,Y)-b(X,Y))/2+128',mergeplanes=0x000102:yuv444p,format=yuv420p"
        filters.append(ycgco)
        
    vf = []
    if filters:
        vf = ["-vf", ",".join(filters)]
        
    fps_disp = f", {fps_filter}" if fps_filter else ""
    scale_disp = scale if scale else "source"
    print(f">> encoding  {inp}  -> {container}  ({scale_disp}{fps_disp}, audio={audio})")
    
    cmd = [FFENC, "-nostdin", "-y", "-i", inp] + vf + enc_opts + [container]
    run_cmd(cmd) or sys.exit(1)
    
    if roundtrip:
        print(f">> decoding  {container}  ->  {watch}  (single binary, mpeg4)")
        cmd1 = [FFENC, "-nostdin", "-y", "-loglevel", "error", "-f", dmx, "-i", container, "-c:v", "mpeg4", "-q:v", "3", "-c:a", "aac", watch]
        cmd2 = [FFENC, "-nostdin", "-y", "-loglevel", "error", "-f", dmx, "-i", container, "-map", "0:v", "-c:v", "mpeg4", "-q:v", "3", watch]
        run_ffenc_fallback(cmd1, cmd2)
        
        print("\nround-trip complete:")
        run_cmd(["ls", "-la", container, watch])
    else:
        print("\nencode complete:")
        run_cmd(["ls", "-la", container])
    print()
    
    p = subprocess.run([FFENC, "-hide_banner", "-f", dmx, "-i", container], stderr=subprocess.PIPE, text=True)
    for line in p.stderr.splitlines():
        if "Stream" in line or "Duration" in line:
            print("  " + line)

if __name__ == "__main__":
    main()
