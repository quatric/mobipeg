#!/usr/bin/env python3
import tkinter as tk
from tkinter import ttk, filedialog, messagebox
import subprocess
import threading
import os
import sys

ENCODE_SCRIPT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "encode.py")

class EncodeGUI(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("mobipeg v1.2")
        self.geometry("750x650")
        self.minsize(650, 500)
        self.configure(padx=15, pady=15)
        
        try:
            if hasattr(sys, '_MEIPASS'):
                base_path = sys._MEIPASS
            else:
                base_path = os.path.dirname(os.path.abspath(__file__))
            if sys.platform != 'darwin':
                icon_path = os.path.join(base_path, "logo.png")
                if os.path.exists(icon_path):
                    img = tk.PhotoImage(file=icon_path)
                    self.tk.call('wm', 'iconphoto', self._w, img)
        except Exception:
            pass
        
        style = ttk.Style(self)
        if "aqua" in style.theme_names():
            style.theme_use("aqua")
        elif "clam" in style.theme_names():
            style.theme_use("clam")
            
        self.notebook = ttk.Notebook(self)
        self.notebook.pack(fill=tk.BOTH, expand=True)
        
        # --- ENCODE TAB ---
        self.encode_frame = ttk.Frame(self.notebook, padding=10)
        self.notebook.add(self.encode_frame, text="Encode")
        self.setup_encode_tab()
        
        # --- DECODE TAB ---
        self.decode_frame = ttk.Frame(self.notebook, padding=10)
        self.notebook.add(self.decode_frame, text="Decode")
        self.setup_decode_tab()
        
        # --- CONSOLE ---
        ttk.Label(self, text="Console Output:").pack(anchor="w", pady=(10, 0))
        console_frame = ttk.Frame(self)
        console_frame.pack(fill=tk.BOTH, expand=True)
        
        self.console = tk.Text(console_frame, height=10, state="disabled", bg="#1e1e1e", fg="#cccccc", font=("Menlo", 12))
        self.console.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
        
        scrollbar = ttk.Scrollbar(console_frame, command=self.console.yview)
        scrollbar.pack(side=tk.RIGHT, fill=tk.Y)
        self.console.config(yscrollcommand=scrollbar.set)
        
    def setup_encode_tab(self):
        self.encode_frame.columnconfigure(1, weight=1)
        
        # Row 0: Format
        ttk.Label(self.encode_frame, text="Format:").grid(row=0, column=0, sticky="e", padx=5, pady=5)
        self.enc_fmt_var = tk.StringVar(value="Wii Mobiclip .mo")
        self.formats_map = {
            "Wii Mobiclip .mo": "mo",
            "3DS Mobiclip .moflex (2D)": "moflex",
            "3DS Mobiclip .moflex (3D)": "moflex3d",
            "DS Mobiclip .mods": "mods",
            "DS ActImagine .vx": "vx",
            "GameCube/Wii THP .thp": "thp",
            "DS RocketVideo .rvid": "rvid"
        }
        # Audio codecs each format actually supports (see encode.py):
        #  - vorbis is Wii (.mo) only
        #  - codebook (SX / vx_audio) is DS only (.mods, .vx)
        #  - .vx carries only its own vx_audio codec, so codebook or none
        #  - .thp is always adpcm_thp
        self.audio_options = {
            "mo":       ["adpcm", "fastaudio", "pcm", "vorbis", "none"],
            "moflex":   ["adpcm", "fastaudio", "pcm", "none"],
            "moflex3d": ["adpcm", "fastaudio", "pcm", "none"],
            "mods":     ["adpcm", "fastaudio", "pcm", "codebook", "none"],
            "vx":       ["codebook", "none"],
            "thp":      ["adpcm", "none"],
            "rvid":     ["pcm", "none"],
        }
        self.enc_fmt_cb = ttk.Combobox(self.encode_frame, textvariable=self.enc_fmt_var, values=list(self.formats_map.keys()), state="readonly", width=25)
        self.enc_fmt_cb.grid(row=0, column=1, sticky="ew", padx=5, pady=5)
        self.enc_fmt_cb.bind("<<ComboboxSelected>>", self.on_enc_format_change)
        
        # Row 1: Audio Codec
        ttk.Label(self.encode_frame, text="Audio Codec:").grid(row=1, column=0, sticky="e", padx=5, pady=5)
        self.enc_audio_var = tk.StringVar(value="adpcm")
        self.enc_audio_cb = ttk.Combobox(self.encode_frame, textvariable=self.enc_audio_var, state="readonly")
        self.enc_audio_cb.grid(row=1, column=1, sticky="ew", padx=5, pady=5)
        
        # Row 2: Input 1
        ttk.Label(self.encode_frame, text="Input File:").grid(row=2, column=0, sticky="e", padx=5, pady=5)
        self.enc_input_var = tk.StringVar()
        ttk.Entry(self.encode_frame, textvariable=self.enc_input_var).grid(row=2, column=1, sticky="ew", padx=5, pady=5)
        ttk.Button(self.encode_frame, text="Browse...", command=lambda: self.browse_file(self.enc_input_var)).grid(row=2, column=2, padx=5, pady=5)
        
        # Row 3: Input 2 (3D Right Eye)
        self.enc_input2_label = ttk.Label(self.encode_frame, text="Right Eye (3D):")
        self.enc_input2_label.grid(row=3, column=0, sticky="e", padx=5, pady=5)
        self.enc_input2_var = tk.StringVar()
        self.enc_input2_entry = ttk.Entry(self.encode_frame, textvariable=self.enc_input2_var)
        self.enc_input2_entry.grid(row=3, column=1, sticky="ew", padx=5, pady=5)
        self.enc_input2_btn = ttk.Button(self.encode_frame, text="Browse...", command=lambda: self.browse_file(self.enc_input2_var))
        self.enc_input2_btn.grid(row=3, column=2, padx=5, pady=5)

        # Row 4: MO3D Layout (3D)
        self.enc_layout_label = ttk.Label(self.encode_frame, text="MO3D Layout (default 4):")
        self.enc_layout_label.grid(row=4, column=0, sticky="e", padx=5, pady=5)
        self.enc_layout_var = tk.StringVar(value="4")
        self.enc_layout_entry = ttk.Entry(self.encode_frame, textvariable=self.enc_layout_var, width=8)
        self.enc_layout_entry.grid(row=4, column=1, sticky="w", padx=5, pady=5)
        
        # Row 5: Output Dir
        ttk.Label(self.encode_frame, text="Output Dir:").grid(row=5, column=0, sticky="e", padx=5, pady=5)
        self.enc_outdir_var = tk.StringVar(value="")
        ttk.Entry(self.encode_frame, textvariable=self.enc_outdir_var).grid(row=5, column=1, sticky="ew", padx=5, pady=5)
        ttk.Button(self.encode_frame, text="Browse...", command=lambda: self.browse_dir(self.enc_outdir_var)).grid(row=5, column=2, padx=5, pady=5)
        
        # Row 6: Scale
        ttk.Label(self.encode_frame, text="Scale (e.g. 384x288):").grid(row=6, column=0, sticky="e", padx=5, pady=5)
        self.enc_scale_var = tk.StringVar()
        ttk.Entry(self.encode_frame, textvariable=self.enc_scale_var).grid(row=6, column=1, sticky="ew", padx=5, pady=5)

        # Row 7: Keyframes
        self.enc_keyframes_label = ttk.Label(self.encode_frame, text="Keyframes (0=auto):")
        self.enc_keyframes_label.grid(row=7, column=0, sticky="e", padx=5, pady=5)
        self.enc_keyframes_var = tk.StringVar(value="0")
        self.enc_keyframes_entry = ttk.Entry(self.encode_frame, textvariable=self.enc_keyframes_var, width=8)
        self.enc_keyframes_entry.grid(row=7, column=1, sticky="w", padx=5, pady=5)

        # Row 8: Quantizer / QP (0=default)
        self.enc_quant_label = ttk.Label(self.encode_frame, text="Quantizer / QP (0=default):")
        self.enc_quant_label.grid(row=8, column=0, sticky="e", padx=5, pady=5)
        self.enc_quant_var = tk.StringVar(value="0")
        self.enc_quant_entry = ttk.Entry(self.encode_frame, textvariable=self.enc_quant_var, width=8)
        self.enc_quant_entry.grid(row=8, column=1, sticky="w", padx=5, pady=5)

        # Row 9: Audio rate (vx / mods codebook / thp / rvid) — match the clip you're replacing.
        self.enc_arate_label = ttk.Label(self.encode_frame, text="Audio rate (Hz, 0=source):")
        self.enc_arate_label.grid(row=9, column=0, sticky="e", padx=5, pady=5)
        self.enc_audio_rate_var = tk.StringVar(value="0")
        self.enc_arate_entry = ttk.Entry(self.encode_frame, textvariable=self.enc_audio_rate_var, width=8)
        self.enc_arate_entry.grid(row=9, column=1, sticky="w", padx=5, pady=5)

        # Row 10: FPS — applies to every format; match the clip you're replacing
        # (e.g. 15 or 60000/1001).  Always shown, never gated behind Advanced.
        self.enc_fps_label = ttk.Label(self.encode_frame, text="FPS (blank=source):")
        self.enc_fps_label.grid(row=10, column=0, sticky="e", padx=5, pady=5)
        self.enc_fps_var = tk.StringVar(value="")
        self.enc_fps_entry = ttk.Entry(self.encode_frame, textvariable=self.enc_fps_var, width=12)
        self.enc_fps_entry.grid(row=10, column=1, sticky="w", padx=5, pady=5)

        # Row 11: RVID Mode (rvid only)
        self.enc_rvid_mode_label = ttk.Label(self.encode_frame, text="RVID Mode:")
        self.enc_rvid_mode_label.grid(row=11, column=0, sticky="e", padx=5, pady=5)
        self.enc_rvid_mode_var = tk.StringVar(value="rgb555")
        self.enc_rvid_mode_cb = ttk.Combobox(self.encode_frame, textvariable=self.enc_rvid_mode_var, values=["rgb555", "rgb565", "256"], state="readonly", width=10)
        self.enc_rvid_mode_cb.grid(row=11, column=1, sticky="w", padx=5, pady=5)

        # Row 12: Checkboxes
        self.enc_fast_audio_var = tk.BooleanVar(value=False)
        self.enc_fast_audio_chk = ttk.Checkbutton(
            self.encode_frame,
            text="Fast audio (skip LTP search — ~90x faster, ~2 dB lower quality)",
            variable=self.enc_fast_audio_var)
        self.enc_fast_audio_chk.grid(row=12, column=1, sticky="w", padx=5, pady=2)

        self.enc_roundtrip_var = tk.BooleanVar(value=False)
        self.enc_roundtrip_chk = ttk.Checkbutton(
            self.encode_frame,
            text="Enable round-trip decoding validation",
            variable=self.enc_roundtrip_var)
        self.enc_roundtrip_chk.grid(row=13, column=1, sticky="w", padx=5, pady=2)

        self.enc_hq_var = tk.BooleanVar(value=False)
        self.enc_hq_chk = ttk.Checkbutton(
            self.encode_frame,
            text="Use Highest Quality (Largest Filesize)",
            variable=self.enc_hq_var,
            command=self.on_toggle_hq)
        self.enc_hq_chk.grid(row=14, column=1, sticky="w", padx=5, pady=2)

        self.enc_rvid_nocompress_var = tk.BooleanVar(value=False)
        self.enc_rvid_nocompress_chk = ttk.Checkbutton(
            self.encode_frame,
            text="RVID: Raw 16bpp (no LZ10 compression)",
            variable=self.enc_rvid_nocompress_var)
        self.enc_rvid_nocompress_chk.grid(row=15, column=1, sticky="w", padx=5, pady=2)

        self.enc_rvid_interlaced_var = tk.BooleanVar(value=False)
        self.enc_rvid_interlaced_chk = ttk.Checkbutton(
            self.encode_frame,
            text="RVID: Interlaced (one field per frame)",
            variable=self.enc_rvid_interlaced_var)
        self.enc_rvid_interlaced_chk.grid(row=16, column=1, sticky="w", padx=5, pady=2)

        self.enc_rvid_nodither_var = tk.BooleanVar(value=False)
        self.enc_rvid_nodither_chk = ttk.Checkbutton(
            self.encode_frame,
            text="RVID: Disable 16bpp dithering",
            variable=self.enc_rvid_nodither_var)
        self.enc_rvid_nodither_chk.grid(row=17, column=1, sticky="w", padx=5, pady=2)

        # Row 18: Toggle Advanced Options
        self.enc_adv_toggle_var = tk.BooleanVar(value=False)
        self.enc_adv_toggle_chk = ttk.Checkbutton(
            self.encode_frame,
            text="⚙ Show Advanced MobiClip Encoder Options (MOBI_*)",
            variable=self.enc_adv_toggle_var,
            command=self.on_toggle_advanced)
        self.enc_adv_toggle_chk.grid(row=18, column=1, sticky="w", padx=5, pady=4)

        # Row 19: Advanced Options Frame
        self.enc_adv_frame = ttk.LabelFrame(self.encode_frame, text="Advanced MobiClip Tuning", padding=8)
        self.enc_adv_frame.grid(row=19, column=0, columnspan=3, sticky="ew", padx=5, pady=5)
        self.enc_adv_frame.columnconfigure(1, weight=1)

        # MOBI_QYX
        ttk.Label(self.enc_adv_frame, text="MOBI_QYX (QY Tier 0-15, default 1):").grid(row=0, column=0, sticky="e", padx=5, pady=2)
        self.enc_mobi_qyx_var = tk.StringVar(value="1")
        ttk.Entry(self.enc_adv_frame, textvariable=self.enc_mobi_qyx_var, width=6).grid(row=0, column=1, sticky="w", padx=5, pady=2)

        # MOBI_DZ
        ttk.Label(self.enc_adv_frame, text="MOBI_DZ (Deadzone 1-8, default 5):").grid(row=1, column=0, sticky="e", padx=5, pady=2)
        self.enc_mobi_dz_var = tk.StringVar(value="5")
        ttk.Entry(self.enc_adv_frame, textvariable=self.enc_mobi_dz_var, width=6).grid(row=1, column=1, sticky="w", padx=5, pady=2)

        # MOBI_SUBME
        ttk.Label(self.enc_adv_frame, text="MOBI_SUBME (Subpel Refine, default 2):").grid(row=2, column=0, sticky="e", padx=5, pady=2)
        self.enc_mobi_subme_var = tk.StringVar(value="2")
        ttk.Entry(self.enc_adv_frame, textvariable=self.enc_mobi_subme_var, width=6).grid(row=2, column=1, sticky="w", padx=5, pady=2)

        # MOBI_SKIP
        ttk.Label(self.enc_adv_frame, text="MOBI_SKIP (MB Skip Threshold, default 512):").grid(row=3, column=0, sticky="e", padx=5, pady=2)
        self.enc_mobi_skip_var = tk.StringVar(value="512")
        ttk.Entry(self.enc_adv_frame, textvariable=self.enc_mobi_skip_var, width=6).grid(row=3, column=1, sticky="w", padx=5, pady=2)

        # MOBI_INTRA_ONLY
        self.enc_mobi_intra_var = tk.BooleanVar(value=False)
        ttk.Checkbutton(self.enc_adv_frame, text="MOBI_INTRA_ONLY (Force Keyframes Only)", variable=self.enc_mobi_intra_var).grid(row=4, column=1, sticky="w", padx=5, pady=2)

        # Run Button
        self.enc_run_btn = ttk.Button(self.encode_frame, text="▶ Run Encoding", command=self.run_encoding)
        self.enc_run_btn.grid(row=20, column=1, pady=15)

        # Widgets that only appear for certain formats, keyed by the formats
        # that should show them. Hiding uses grid_remove() (not state=disabled)
        # so rows collapse instead of sitting there greyed out.
        self.enc_conditional_widgets = [
            ({"moflex3d"}, (self.enc_input2_label, self.enc_input2_entry, self.enc_input2_btn)),
            ({"moflex3d"}, (self.enc_layout_label, self.enc_layout_entry)),
            ({"mo", "moflex", "moflex3d", "vx"}, (self.enc_keyframes_label, self.enc_keyframes_entry)),
            ({"vx", "mo", "moflex", "moflex3d", "mods", "thp"}, (self.enc_quant_label, self.enc_quant_entry)),
            ({"vx", "mods", "thp", "rvid"}, (self.enc_arate_label, self.enc_arate_entry)),
            ({"rvid"}, (self.enc_rvid_mode_label, self.enc_rvid_mode_cb)),
            ({"vx", "mods"}, (self.enc_fast_audio_chk,)),
            ({"rvid"}, (self.enc_rvid_nocompress_chk,)),
            ({"rvid"}, (self.enc_rvid_interlaced_chk,)),
            ({"rvid"}, (self.enc_rvid_nodither_chk,)),
            ({"mo", "moflex", "moflex3d", "mods"}, (self.enc_adv_toggle_chk,)),
        ]

        self.enc_input_var.trace_add("write", lambda *a: self.on_input_changed(self.enc_input_var, self.enc_outdir_var))
        for var in (self.enc_quant_var, self.enc_mobi_qyx_var, self.enc_mobi_dz_var, self.enc_mobi_subme_var, self.enc_mobi_skip_var):
            var.trace_add("write", self.update_hq_state_from_fields)
        self.on_enc_format_change()

    def on_toggle_hq(self):
        if self.enc_hq_var.get():
            self.enc_quant_var.set("12")
            self.enc_mobi_qyx_var.set("0")
            self.enc_mobi_dz_var.set("1")
            self.enc_mobi_subme_var.set("7")
            self.enc_mobi_skip_var.set("0")
            fmt = self.formats_map.get(self.enc_fmt_var.get())
            options = self.audio_options.get(fmt, [])
            if "pcm" in options:
                self.enc_audio_var.set("pcm")
        else:
            self.enc_quant_var.set("0")
            self.enc_mobi_qyx_var.set("1")
            self.enc_mobi_dz_var.set("5")
            self.enc_mobi_subme_var.set("2")
            self.enc_mobi_skip_var.set("512")
            fmt = self.formats_map.get(self.enc_fmt_var.get())
            options = self.audio_options.get(fmt, [])
            if "adpcm" in options:
                self.enc_audio_var.set("adpcm")

    def update_hq_state_from_fields(self, *args):
        is_hq = (
            self.enc_quant_var.get().strip() == "12" and
            self.enc_mobi_qyx_var.get().strip() == "0" and
            self.enc_mobi_dz_var.get().strip() == "1" and
            self.enc_mobi_subme_var.get().strip() == "7" and
            self.enc_mobi_skip_var.get().strip() == "0"
        )
        if is_hq and not self.enc_hq_var.get():
            self.enc_hq_var.set(True)
        elif not is_hq and self.enc_hq_var.get():
            self.enc_hq_var.set(False)

    def setup_decode_tab(self):
        self.decode_frame.columnconfigure(1, weight=1)
        
        # Input File
        ttk.Label(self.decode_frame, text="Input (.mo/.moflex/.mods/.vx/.thp/.rvid/.ppm/.kwz/.h4m/.ty):").grid(row=0, column=0, sticky="e", padx=5, pady=5)
        self.dec_input_var = tk.StringVar()
        ttk.Entry(self.decode_frame, textvariable=self.dec_input_var).grid(row=0, column=1, sticky="ew", padx=5, pady=5)
        ttk.Button(self.decode_frame, text="Browse...", command=lambda: self.browse_file(self.dec_input_var)).grid(row=0, column=2, padx=5, pady=5)
        
        # Output Dir
        ttk.Label(self.decode_frame, text="Output Dir:").grid(row=1, column=0, sticky="e", padx=5, pady=5)
        self.dec_outdir_var = tk.StringVar(value="")
        ttk.Entry(self.decode_frame, textvariable=self.dec_outdir_var).grid(row=1, column=1, sticky="ew", padx=5, pady=5)
        ttk.Button(self.decode_frame, text="Browse...", command=lambda: self.browse_dir(self.dec_outdir_var)).grid(row=1, column=2, padx=5, pady=5)
        
        # Run Button
        self.dec_run_btn = ttk.Button(self.decode_frame, text="▶ Run Decoding", command=self.run_decoding)
        self.dec_run_btn.grid(row=2, column=1, pady=15)
        
        self.dec_input_var.trace_add("write", lambda *a: self.on_input_changed(self.dec_input_var, self.dec_outdir_var))

    def on_input_changed(self, var, outdir_var):
        val = var.get()
        if val.startswith("{") and val.endswith("}"):
            var.set(val[1:-1])
            val = var.get()
        if val and os.path.isfile(val):
            directory = os.path.dirname(val)
            outdir_var.set(directory)
            
    def on_toggle_advanced(self):
        fmt = self.formats_map.get(self.enc_fmt_var.get())
        if fmt in ("mo", "moflex", "moflex3d", "mods") and self.enc_adv_toggle_var.get():
            self.enc_adv_frame.grid()
        else:
            self.enc_adv_frame.grid_remove()

    def on_enc_format_change(self, event=None):
        fmt = self.formats_map.get(self.enc_fmt_var.get())

        self.enc_input2_label.config(text="Right Eye (3D):")

        # Show only the fields relevant to the selected format; hidden rows
        # collapse instead of sitting around greyed out.
        for formats, widgets in self.enc_conditional_widgets:
            show = fmt in formats
            for w in widgets:
                w.grid() if show else w.grid_remove()

        self.on_toggle_advanced()

        # Restrict the audio dropdown to what the selected format supports.
        options = self.audio_options.get(fmt, ["adpcm"])
        self.enc_audio_cb.config(values=options)
        if self.enc_audio_var.get() not in options:
            self.enc_audio_var.set(options[0])

    def browse_file(self, var):
        filename = filedialog.askopenfilename()
        if filename:
            var.set(filename)
            
    def browse_dir(self, var):
        directory = filedialog.askdirectory()
        if directory:
            var.set(directory)

    def append_console(self, text):
        self.console.config(state="normal")
        self.console.insert(tk.END, text)
        self.console.see(tk.END)
        self.console.config(state="disabled")

    def execute_cmd(self, cmd, btn):
        btn.config(state="disabled")
        self.console.config(state="normal")
        self.console.delete(1.0, tk.END)
        self.console.config(state="disabled")
        self.append_console(f"$ {' '.join(cmd)}\n\n")
        
        def run_thread():
            process = subprocess.Popen(
                cmd,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                bufsize=1
            )
            for line in process.stdout:
                self.after(0, self.append_console, line)
            
            process.wait()
            self.after(0, self.append_console, f"\nProcess finished with exit code {process.returncode}\n")
            self.after(0, lambda: btn.config(state="normal"))
            
        threading.Thread(target=run_thread, daemon=True).start()

    def run_encoding(self):
        if not getattr(sys, 'frozen', False) and not os.path.exists(ENCODE_SCRIPT):
            messagebox.showerror("Error", f"Could not find encode script at:\n{ENCODE_SCRIPT}")
            return
            
        fmt = self.formats_map.get(self.enc_fmt_var.get())
        audio = self.enc_audio_var.get()
        inp1 = self.enc_input_var.get()
        inp2 = self.enc_input2_var.get()
        scale = self.enc_scale_var.get()
        outdir = self.enc_outdir_var.get()
        
        if not inp1:
            messagebox.showwarning("Warning", "Please select an input file.")
            return
            
        cmd = [sys.executable, "--encode-script"] if getattr(sys, 'frozen', False) else [sys.executable, ENCODE_SCRIPT]
        cmd.extend([fmt, audio, inp1])

        if fmt == "moflex3d":
            if inp2:
                cmd.append(inp2)
            layout = self.enc_layout_var.get().strip()
            if layout and layout != "4":
                cmd.extend(["--layout", layout])
        if scale:
            cmd.extend(["--scale", scale])
        if outdir:
            cmd.extend(["--outdir", outdir])
        if fmt in ("mo", "moflex", "moflex3d", "vx"):
            kf = self.enc_keyframes_var.get().strip()
            if kf and kf != "0":
                cmd.extend(["--keyframes", kf])
        if self.enc_roundtrip_var.get():
            cmd.append("--roundtrip")
        if self.enc_hq_var.get():
            cmd.append("--hq")
        if self.enc_fast_audio_var.get() and fmt in ("vx", "mods"):
            cmd.append("--fast-audio")
        if fmt in ("vx", "mo", "moflex", "moflex3d", "mods", "thp"):
            q = self.enc_quant_var.get().strip()
            if q and q != "0":
                cmd.extend(["--quantizer", q])
        if fmt in ("vx", "thp", "mods"):
            fps = self.enc_fps_var.get().strip()
            if fps:
                cmd.extend(["--fps", fps])
        if fmt in ("vx", "mods", "thp", "rvid"):
            arate = self.enc_audio_rate_var.get().strip()
            if arate and arate != "0":
                cmd.extend(["--audio-rate", arate])
        if fmt in ("mo", "moflex", "moflex3d", "mods") and self.enc_adv_toggle_var.get():
            qyx = self.enc_mobi_qyx_var.get().strip()
            if qyx and qyx != "1":
                cmd.extend(["--mobi-qyx", qyx])
            dz = self.enc_mobi_dz_var.get().strip()
            if dz and dz != "5":
                cmd.extend(["--mobi-dz", dz])
            subme = self.enc_mobi_subme_var.get().strip()
            if subme and subme != "2":
                cmd.extend(["--mobi-subme", subme])
            if self.enc_mobi_intra_var.get():
                cmd.append("--mobi-intra-only")
            skip = self.enc_mobi_skip_var.get().strip()
            if skip and skip != "512":
                cmd.extend(["--mobi-skip", skip])
        if fmt == "rvid":
            rmode = self.enc_rvid_mode_var.get()
            if rmode and rmode != "rgb555":
                cmd.extend(["--rvid-mode", rmode])
            if self.enc_rvid_nocompress_var.get():
                cmd.append("--no-compress")
            if self.enc_rvid_interlaced_var.get():
                cmd.append("--rvid-interlaced")
            if self.enc_rvid_nodither_var.get():
                cmd.append("--rvid-no-dither")

        self.execute_cmd(cmd, self.enc_run_btn)

    def run_decoding(self):
        if not getattr(sys, 'frozen', False) and not os.path.exists(ENCODE_SCRIPT):
            messagebox.showerror("Error", f"Could not find encode script at:\n{ENCODE_SCRIPT}")
            return
            
        inp = self.dec_input_var.get()
        outdir = self.dec_outdir_var.get()
        
        if not inp:
            messagebox.showwarning("Warning", "Please select an input file to decode.")
            return
            
        cmd = [sys.executable, "--encode-script"] if getattr(sys, 'frozen', False) else [sys.executable, ENCODE_SCRIPT]
        cmd.extend(["decode", inp])
        
        if outdir:
            cmd.extend(["--outdir", outdir])
            
        self.execute_cmd(cmd, self.dec_run_btn)

if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "--encode-script":
        import encode
        sys.argv = [sys.argv[0]] + sys.argv[2:]
        encode.main()
        sys.exit(0)
    app = EncodeGUI()
    app.mainloop()
