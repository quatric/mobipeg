#!/usr/bin/env python3
# Copyright (c) 2026 quatric - quatricsoftware@gmail.com
try:
    import sentry_sdk

    def _sentry_before_send(event, hint):
        if "exc_info" in hint:
            exc_type, exc_value, _ = hint["exc_info"]
            if issubclass(exc_type, (UnicodeEncodeError, KeyboardInterrupt)):
                return None
            # Broken/missing Tcl install on the user's machine; not an app bug.
            if "Can't find a usable" in str(exc_value) and "tcl" in str(exc_value):
                return None
            # Wrong-architecture helper binary (Errno 86 Bad CPU type): user-environment
            # error, now reported to the user by _handle_binary_error.
            if (
                issubclass(exc_type, OSError)
                and getattr(exc_value, "errno", None) == 86
            ):
                return None
            if issubclass(exc_type, FileNotFoundError) and "ls" in str(exc_value):
                return None
            if issubclass(exc_type, (PermissionError, OSError)) and "/Volumes" in str(
                exc_value
            ):
                return None
        return event

    sentry_sdk.init(
        dsn="https://9cff5a454488188a77ee0c3a043d7c94@o107347.ingest.us.sentry.io/4512040240480256",
        send_default_pii=True,
        before_send=_sentry_before_send,
    )
except ImportError:
    pass

import tkinter as tk
from tkinter import ttk, filedialog, messagebox
import shlex
import subprocess
import threading
import os
import sys

if hasattr(sys.stdout, "reconfigure"):
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass
if hasattr(sys.stderr, "reconfigure"):
    try:
        sys.stderr.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass

_script_dir = (
    os.path.dirname(os.path.abspath(__file__))
    if "__file__" in globals()
    else os.getcwd()
)
ENCODE_SCRIPT = os.path.join(_script_dir, "encode.py")

# Shown in the title bar; keep in step with version_info.txt and the release tag.
MOBIPEG_VERSION = "3.0"


# One source of truth for what the decoder accepts: (family, extensions).
# The file dialog's type menu and the expandable format list are both derived
# from this, so they can't drift apart.
DECODER_FAMILIES = [
    ("Mobiclip", "*.mo *.moflex *.mods"),
    ("GBA Video cartridges / resources", "*.gba *.mmstr"),
    ("ActImagine VX", "*.vx"),
    ("THP", "*.thp"),
    ("RocketVideo", "*.rvid"),
    ("HVQM4", "*.h4m"),
    ("FastVideoDS", "*.fv"),
    ("Factor 5 DivX", "*.vid"),
    ("TiVo TyStream", "*.ty *.ty+ *.tmf"),
    ("Swapdoodle / Swapnote", "*.bpk *.bpk1 *.apd"),
    ("Flipnote", "*.ppm *.kwz"),
    ("DS DPG video", "*.dpg"),
    ("Nintendo DSP-ADPCM", "*.dsp"),
    ("Nintendo streams", "*.brstm *.bfstm *.bcstm"),
    ("Wii BNS / AST", "*.bns *.ast"),
    ("Wii U boot sound", "*.btsnd"),
    ("Wii / 3DS AAC audio", "*.m4a"),
]

# Formats with no video stream. The Encode tab hides every video-only control
# for these, and encode.py decodes them to .wav rather than .mp4.
AUDIO_ONLY_FORMATS = {
    "dsp",
    "brstm",
    "bfstm",
    "bcstm",
    "bns",
    "ast",
    "btsnd",
    "wii_photo_m4a",
    "3ds_sound",
}

# Formats Highest Quality is not offered for: their encoder's own default
# is already the sensible quality, and HQ's QP 12 would bloat the files.
NO_HQ_FORMATS = {"mivf"}

# Output container extension per format, mirroring encode.py's out_ext map
# (and AUDIO_FORMAT_EXTENSIONS for the audio-only formats) -- used only to
# preview the default output filename before a run, not to pick the muxer.
ENCODE_EXTENSIONS = {
    "hvqm4": "h4m",
    "fastvideo": "fv",
    "gba_ads": "mmstr",
    "gba_hydrogen": "mmstr",
    "wii_photo": "avi",
    "nintendo_channel": "3gp",
    "topos": "mkv",
    "topos3d": "mkv",
    "topos_mpeg2": "mkv",
    "topos3d_mpeg2": "mkv",
    "3ds_camera": "avi",
    "3ds_camera3d": "avi",
    "wii_photo_m4a": "m4a",
    "3ds_sound": "m4a",
    "factor5": "vid",
    "flipnote": "ppm",
    "flipnote3d": "kwz",
}

# How many family cells sit side by side in the expandable grid.
DECODER_GRID_COLUMNS = 3

_ALL_DECODE_EXTS = " ".join(exts for _, exts in DECODER_FAMILIES)

# "All supported" first so the dialog's default view shows everything.
DECODE_FILETYPES = (
    [("All supported", _ALL_DECODE_EXTS)]
    + list(DECODER_FAMILIES)
    + [("All files", "*.*")]
)


class CollapsibleSection(ttk.Frame):
    """A disclosure triangle whose body is gridded/ungridded beneath it.

    ttk has no built-in disclosure widget, and the format list is long enough
    that showing it unconditionally crowds out the actual controls -- but
    short enough that hiding it behind a dialog would be worse.
    """

    def __init__(self, parent, title, expanded=False):
        super().__init__(parent)
        self.columnconfigure(0, weight=1)
        self._title = title
        self._expanded = bool(expanded)
        self._button = ttk.Label(self, cursor="hand2", foreground="grey")
        self._button.grid(row=0, column=0, sticky="w")
        self._button.bind("<Button-1>", lambda _e: self.toggle())
        self.body = ttk.Frame(self)
        self.body.grid(row=1, column=0, sticky="ew", padx=(16, 0), pady=(3, 0))
        self._sync()

    def toggle(self):
        self._expanded = not self._expanded
        self._sync()

    def _sync(self):
        arrow = (
            "\u25be" if self._expanded else "\u25b8"
        )  # BLACK DOWN/RIGHT-POINTING SMALL TRIANGLE
        self._button.configure(text="%s  %s" % (arrow, self._title))
        if self._expanded:
            self.body.grid()
        else:
            self.body.grid_remove()


class EncodeGUI(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title(f"mobipeg v{MOBIPEG_VERSION}")
        self.geometry("750x900")
        self.minsize(700, 750)
        self.configure(padx=15, pady=15)

        try:
            if hasattr(sys, "_MEIPASS"):
                base_path = sys._MEIPASS
            elif "__file__" in globals():
                base_path = os.path.dirname(os.path.abspath(__file__))
            else:
                base_path = os.getcwd()
            if sys.platform != "darwin":
                icon_path = os.path.join(base_path, "logo.png")
                if os.path.exists(icon_path):
                    img = tk.PhotoImage(file=icon_path)
                    self._icon_img = img
                    self.tk.call("wm", "iconphoto", self._w, img)
        except Exception:
            pass

        style = ttk.Style(self)
        if "aqua" in style.theme_names():
            style.theme_use("aqua")
        elif "clam" in style.theme_names():
            style.theme_use("clam")

        # Tabs above, console below, with a sash between them.  Every tab
        # asks for more height than a short screen has left once the window
        # is clamped to it; packed one after the other, whichever came second
        # was squeezed to nothing, which is how the console used to vanish.
        self.panes = ttk.PanedWindow(self, orient=tk.VERTICAL)
        self.panes.pack(fill=tk.BOTH, expand=True)

        self.notebook = ttk.Notebook(self.panes)
        self.panes.add(self.notebook, weight=1)

        # --- ENCODE TAB ---
        self.encode_frame = ttk.Frame(self.notebook, padding=10)
        self.notebook.add(self.encode_frame, text="Encode")
        self.setup_encode_tab()

        # --- DECODE TAB ---
        self.decode_frame = ttk.Frame(self.notebook, padding=10)
        self.notebook.add(self.decode_frame, text="Decode")
        self.setup_decode_tab()

        # --- PACKAGE CIA TAB ---
        # Deliberately its own step, not chained onto Encode: you build the
        # .moflex first, then package it here once you're happy with it.
        self.cia_frame = ttk.Frame(self.notebook, padding=10)
        self.notebook.add(self.cia_frame, text="Package CIA")
        self.setup_cia_tab()

        # --- CONSOLE ---
        console_box = ttk.Frame(self.panes)
        self.panes.add(console_box, weight=1)
        ttk.Label(console_box, text="Console Output:").pack(anchor="w", pady=(10, 0))
        console_frame = ttk.Frame(console_box)
        console_frame.pack(fill=tk.BOTH, expand=True)

        # bg is close enough to the window's own dark background that without
        # a visible border, the whole pane reads as empty dead space rather
        # than a text widget -- highlightthickness draws a border even though
        # this is a plain tk.Text (not ttk), which has no built-in themed one.
        self.console = tk.Text(
            console_frame,
            height=10,
            state="disabled",
            bg="#1e1e1e",
            fg="#cccccc",
            font=("Menlo", 12),
            highlightthickness=1,
            highlightbackground="#3a3a3a",
            highlightcolor="#3a3a3a",
        )
        self.console.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)

        scrollbar = ttk.Scrollbar(console_frame, command=self.console.yview)
        scrollbar.pack(side=tk.RIGHT, fill=tk.Y)
        self.console.config(yscrollcommand=scrollbar.set)

        self.after_idle(self._place_sash)
        self.panes.bind("<Configure>", lambda e: self._place_sash(keep_user=True))

    # Height the console keeps however short the window gets: its label and
    # about five lines of output.
    CONSOLE_MIN_HEIGHT = 130

    def _place_sash(self, keep_user=False):
        """Give the tabs the height they ask for, as far as that leaves the
        console CONSOLE_MIN_HEIGHT.  On a resize (keep_user) a sash the user
        moved is left alone unless the console has dropped below that."""
        self.update_idletasks()
        total = self.panes.winfo_height()
        if total <= 1:
            return
        limit = max(0, total - self.CONSOLE_MIN_HEIGHT)
        if keep_user:
            if self.panes.sashpos(0) > limit:
                self.panes.sashpos(0, limit)
            return
        self.panes.sashpos(0, min(self.notebook.winfo_reqheight(), limit))

    def setup_encode_tab(self):
        self.encode_frame.columnconfigure(1, weight=1)

        # Row 0: Format
        ttk.Label(self.encode_frame, text="Format:").grid(
            row=0, column=0, sticky="e", padx=5, pady=5
        )
        self.enc_fmt_var = tk.StringVar(value="Wii Mobiclip .mo")
        self.formats_map = {
            "Wii Mobiclip .mo": "mo",
            "3DS Mobiclip .moflex (2D)": "moflex",
            "3DS Mobiclip .moflex (3D)": "moflex3d",
            "DS Mobiclip .mods": "mods",
            "DS ActImagine .vx": "vx",
            "TiVo TY Stream .ty": "ty",
            "GBA Video ADS/LZMA .mmstr": "gba_ads",
            "GBA Video Hydrogen/Inflate .mmstr": "gba_hydrogen",
            "Wii Photo Channel Motion-JPEG .avi": "wii_photo",
            "Wii Photo Channel AAC .m4a": "wii_photo_m4a",
            "Wii Nintendo Channel .3gp": "nintendo_channel",
            "3DS Topos player H.264 .mkv (2D)": "topos",
            "3DS Topos player H.264 .mkv (3D)": "topos3d",
            "3DS Topos player MPEG-2 .mkv (2D)": "topos_mpeg2",
            "3DS Topos player MPEG-2 .mkv (3D)": "topos3d_mpeg2",
            "3DS MIVF player .mivf": "mivf",
            "Nintendo 3DS Camera 2D .avi": "3ds_camera",
            "Nintendo 3DS Camera 3D .avi": "3ds_camera3d",
            "Nintendo 3DS Sound AAC .m4a": "3ds_sound",
            "GameCube/Wii THP .thp": "thp",
            "DS RocketVideo .rvid": "rvid",
            "GameCube/Wii HVQM4 .h4m": "hvqm4",
            "GameCube Factor 5 DivX .vid": "factor5",
            "DS ActImagine FastVideoDS .fv": "fastvideo",
            "DS MoonShell .dpg": "dpg",
            "DSi Flipnote Studio .ppm": "flipnote",
            "3DS Flipnote Studio 3D .kwz": "flipnote3d",
            "Nintendo DSP-ADPCM .dsp": "dsp",
            "Wii stream .brstm": "brstm",
            "Wii U stream .bfstm": "bfstm",
            "3DS stream .bcstm": "bcstm",
            "Wii banner sound .bns": "bns",
            "Wii stream .ast": "ast",
            "Wii U boot sound .btsnd": "btsnd",
        }
        # Audio codecs each format actually supports (see encode.py):
        #  - vorbis is Wii (.mo) only
        #  - codebook (SX / vx_audio) is DS only (.mods, .vx)
        #  - .vx carries only its own vx_audio codec, so codebook or none
        #  - .thp is always adpcm_thp
        self.audio_options = {
            "mo": ["adpcm", "fastaudio", "pcm", "vorbis", "none"],
            "moflex": ["adpcm", "fastaudio", "pcm", "none"],
            "moflex3d": ["adpcm", "fastaudio", "pcm", "none"],
            "mods": ["adpcm", "fastaudio", "pcm", "codebook", "none"],
            "vx": ["codebook", "none"],
            "ty": ["mp2", "ac3", "none"],
            "gba_ads": ["none"],
            "gba_hydrogen": ["none"],
            "wii_photo": ["pcm", "none"],
            "wii_photo_m4a": ["aac"],
            "nintendo_channel": ["aac", "none"],
            "topos": ["aac", "mp2", "none"],
            "topos3d": ["aac", "mp2", "none"],
            "topos_mpeg2": ["mp2", "aac", "none"],
            "topos3d_mpeg2": ["mp2", "aac", "none"],
            "mivf": ["ia4m", "pc16", "none"],
            "3ds_camera": ["adpcm", "none"],
            "3ds_camera3d": ["adpcm", "none"],
            "3ds_sound": ["aac"],
            "thp": ["adpcm", "none"],
            "rvid": ["pcm", "none"],
            # hvqm4 has no audio support yet -- video only.
            "hvqm4": ["none"],
            "factor5": ["adpcm", "none"],
            "fastvideo": ["adpcm", "none"],
            "dpg": ["mp2", "none"],
            "flipnote": ["pcm", "none"],
            "flipnote3d": ["pcm", "none"],
            # Audio-only formats: the choice is which of the container's own
            # codecs to write, so "none" is not on offer.
            "dsp": ["adpcm"],
            "brstm": ["adpcm", "pcm"],
            "bfstm": ["adpcm", "pcm"],
            "bcstm": ["adpcm", "pcm"],
            "bns": ["adpcm"],
            "ast": ["adpcm", "pcm"],
            "btsnd": ["pcm"],
        }
        self.enc_fmt_cb = ttk.Combobox(
            self.encode_frame,
            textvariable=self.enc_fmt_var,
            values=list(self.formats_map.keys()),
            state="readonly",
            width=25,
        )
        self.enc_fmt_cb.grid(row=0, column=1, sticky="ew", padx=5, pady=5)
        self.enc_fmt_cb.bind("<<ComboboxSelected>>", self.on_enc_format_change)

        # Row 1: Audio Codec
        ttk.Label(self.encode_frame, text="Audio Codec:").grid(
            row=1, column=0, sticky="e", padx=5, pady=5
        )
        self.enc_audio_var = tk.StringVar(value="pcm")
        self.enc_audio_cb = ttk.Combobox(
            self.encode_frame, textvariable=self.enc_audio_var, state="readonly"
        )
        self.enc_audio_cb.grid(row=1, column=1, sticky="ew", padx=5, pady=5)

        # Row 2: Input 1
        ttk.Label(self.encode_frame, text="Input File:").grid(
            row=2, column=0, sticky="e", padx=5, pady=5
        )
        self.enc_input_var = tk.StringVar()
        ttk.Entry(self.encode_frame, textvariable=self.enc_input_var).grid(
            row=2, column=1, sticky="ew", padx=5, pady=5
        )
        ttk.Button(
            self.encode_frame,
            text="Browse...",
            command=lambda: self.browse_file(self.enc_input_var),
        ).grid(row=2, column=2, padx=5, pady=5)

        # Row 3: Input 2 (3D Right Eye)
        self.enc_input2_label = ttk.Label(self.encode_frame, text="Right Eye (3D):")
        self.enc_input2_label.grid(row=3, column=0, sticky="e", padx=5, pady=5)
        self.enc_input2_var = tk.StringVar()
        self.enc_input2_entry = ttk.Entry(
            self.encode_frame, textvariable=self.enc_input2_var
        )
        self.enc_input2_entry.grid(row=3, column=1, sticky="ew", padx=5, pady=5)
        self.enc_input2_btn = ttk.Button(
            self.encode_frame,
            text="Browse...",
            command=lambda: self.browse_file(self.enc_input2_var),
        )
        self.enc_input2_btn.grid(row=3, column=2, padx=5, pady=5)

        # Row 4: MO3D Layout (3D)
        self.enc_layout_label = ttk.Label(
            self.encode_frame, text="MO3D Layout (default 4):"
        )
        self.enc_layout_label.grid(row=4, column=0, sticky="e", padx=5, pady=5)
        self.enc_layout_var = tk.StringVar(value="4")
        self.enc_layout_entry = ttk.Entry(
            self.encode_frame, textvariable=self.enc_layout_var, width=8
        )
        self.enc_layout_entry.grid(row=4, column=1, sticky="w", padx=5, pady=5)

        # Row 5: Output Dir
        ttk.Label(self.encode_frame, text="Output Dir:").grid(
            row=5, column=0, sticky="e", padx=5, pady=5
        )
        self.enc_outdir_var = tk.StringVar(value="")
        ttk.Entry(self.encode_frame, textvariable=self.enc_outdir_var).grid(
            row=5, column=1, sticky="ew", padx=5, pady=5
        )
        ttk.Button(
            self.encode_frame,
            text="Browse...",
            command=lambda: self.browse_dir(self.enc_outdir_var),
        ).grid(row=5, column=2, padx=5, pady=5)

        # Row 6: Output File (optional exact path; defaults to basename + the
        # format's own extension in Output Dir above, but "Save As..." lets
        # you point at a specific file instead of only picking a directory).
        ttk.Label(self.encode_frame, text="Output File (optional):").grid(
            row=6, column=0, sticky="e", padx=5, pady=5
        )
        self.enc_output_var = tk.StringVar(value="")
        ttk.Entry(self.encode_frame, textvariable=self.enc_output_var).grid(
            row=6, column=1, sticky="ew", padx=5, pady=5
        )
        ttk.Button(
            self.encode_frame, text="Save As...", command=self.browse_save_encode
        ).grid(row=6, column=2, padx=5, pady=5)

        # Row 6: Scale
        self.enc_scale_label = ttk.Label(
            self.encode_frame, text="Scale (e.g. 384x288):"
        )
        self.enc_scale_label.grid(row=7, column=0, sticky="e", padx=5, pady=5)
        self.enc_scale_var = tk.StringVar()
        self.enc_scale_entry = ttk.Entry(
            self.encode_frame, textvariable=self.enc_scale_var
        )
        self.enc_scale_entry.grid(row=7, column=1, sticky="ew", padx=5, pady=5)

        # Row 7: Keyframes
        self.enc_keyframes_label = ttk.Label(
            self.encode_frame, text="Keyframes (0=auto):"
        )
        self.enc_keyframes_label.grid(row=8, column=0, sticky="e", padx=5, pady=5)
        self.enc_keyframes_var = tk.StringVar(value="0")
        self.enc_keyframes_entry = ttk.Entry(
            self.encode_frame, textvariable=self.enc_keyframes_var, width=8
        )
        self.enc_keyframes_entry.grid(row=8, column=1, sticky="w", padx=5, pady=5)

        # Row 8: Quantizer / QP (0=default)
        self.enc_quant_label = ttk.Label(
            self.encode_frame, text="Quantizer / QP (0=default):"
        )
        self.enc_quant_label.grid(row=9, column=0, sticky="e", padx=5, pady=5)
        self.enc_quant_var = tk.StringVar(value="12")
        self.enc_quant_entry = ttk.Entry(
            self.encode_frame, textvariable=self.enc_quant_var, width=8
        )
        self.enc_quant_entry.grid(row=9, column=1, sticky="w", padx=5, pady=5)

        # Row 9: Audio rate (vx / mods codebook / thp / rvid) — match the clip you're replacing.
        self.enc_arate_label = ttk.Label(
            self.encode_frame, text="Audio rate (Hz, 0=source):"
        )
        self.enc_arate_label.grid(row=10, column=0, sticky="e", padx=5, pady=5)
        self.enc_audio_rate_var = tk.StringVar(value="0")
        self.enc_arate_entry = ttk.Entry(
            self.encode_frame, textvariable=self.enc_audio_rate_var, width=8
        )
        self.enc_arate_entry.grid(row=10, column=1, sticky="w", padx=5, pady=5)

        # Row 10: FPS — applies to every format; match the clip you're replacing
        # (e.g. 15 or 60000/1001).  Always shown, never gated behind Advanced.
        self.enc_fps_label = ttk.Label(self.encode_frame, text="FPS (blank=source):")
        self.enc_fps_label.grid(row=11, column=0, sticky="e", padx=5, pady=5)
        self.enc_fps_var = tk.StringVar(value="")
        self.enc_fps_entry = ttk.Entry(
            self.encode_frame, textvariable=self.enc_fps_var, width=12
        )
        self.enc_fps_entry.grid(row=11, column=1, sticky="w", padx=5, pady=5)

        # Row 11: RVID Mode (rvid only)
        self.enc_rvid_mode_label = ttk.Label(self.encode_frame, text="RVID Mode:")
        self.enc_rvid_mode_label.grid(row=12, column=0, sticky="e", padx=5, pady=5)
        self.enc_rvid_mode_var = tk.StringVar(value="rgb555")
        self.enc_rvid_mode_cb = ttk.Combobox(
            self.encode_frame,
            textvariable=self.enc_rvid_mode_var,
            values=["rgb555", "rgb565", "256"],
            state="readonly",
            width=10,
        )
        self.enc_rvid_mode_cb.grid(row=12, column=1, sticky="w", padx=5, pady=5)

        # Row 12: Checkboxes
        self.enc_fast_audio_var = tk.BooleanVar(value=False)
        self.enc_fast_audio_chk = ttk.Checkbutton(
            self.encode_frame,
            text="Fast audio (skip LTP search — ~90x faster, ~2 dB lower quality)",
            variable=self.enc_fast_audio_var,
        )
        self.enc_fast_audio_chk.grid(row=13, column=1, sticky="w", padx=5, pady=2)

        self.enc_roundtrip_var = tk.BooleanVar(value=False)
        self.enc_roundtrip_chk = ttk.Checkbutton(
            self.encode_frame,
            text="Enable round-trip decoding validation",
            variable=self.enc_roundtrip_var,
        )
        self.enc_roundtrip_chk.grid(row=14, column=1, sticky="w", padx=5, pady=2)

        self.enc_hq_var = tk.BooleanVar(value=True)
        self.enc_hq_chk = ttk.Checkbutton(
            self.encode_frame,
            text="Use Highest Quality (Largest Filesize)",
            variable=self.enc_hq_var,
            command=self.on_toggle_hq,
        )
        self.enc_hq_chk.grid(row=15, column=1, sticky="w", padx=5, pady=2)

        # MIVF only, where Highest Quality is hidden, so it takes that row.
        self.enc_m2y2_var = tk.BooleanVar(value=False)
        self.enc_m2y2_chk = ttk.Checkbutton(
            self.encode_frame,
            text="M2Y2: range-code the video (same pictures, ~25-30% smaller)",
            variable=self.enc_m2y2_var,
        )
        self.enc_m2y2_chk.grid(row=15, column=1, sticky="w", padx=5, pady=2)

        self.enc_rvid_nocompress_var = tk.BooleanVar(value=False)
        self.enc_rvid_nocompress_chk = ttk.Checkbutton(
            self.encode_frame,
            text="RVID: Raw 16bpp (no LZ10 compression)",
            variable=self.enc_rvid_nocompress_var,
        )
        self.enc_rvid_nocompress_chk.grid(row=16, column=1, sticky="w", padx=5, pady=2)

        self.enc_rvid_interlaced_var = tk.BooleanVar(value=False)
        self.enc_rvid_interlaced_chk = ttk.Checkbutton(
            self.encode_frame,
            text="RVID: Interlaced (one field per frame)",
            variable=self.enc_rvid_interlaced_var,
        )
        self.enc_rvid_interlaced_chk.grid(row=17, column=1, sticky="w", padx=5, pady=2)

        self.enc_rvid_nodither_var = tk.BooleanVar(value=False)
        self.enc_rvid_nodither_chk = ttk.Checkbutton(
            self.encode_frame,
            text="RVID: Disable 16bpp dithering",
            variable=self.enc_rvid_nodither_var,
        )
        self.enc_rvid_nodither_chk.grid(row=18, column=1, sticky="w", padx=5, pady=2)

        # Row 19: Flipnote options (.ppm author; .kwz author, colours, signing)
        self.enc_flip_frame = ttk.LabelFrame(
            self.encode_frame, text="Flipnote Options", padding=8
        )
        self.enc_flip_frame.grid(
            row=19, column=0, columnspan=3, sticky="ew", padx=5, pady=5
        )
        self.enc_flip_frame.columnconfigure(1, weight=1)

        ttk.Label(self.enc_flip_frame, text="Author (max 10 characters):").grid(
            row=0, column=0, sticky="e", padx=5, pady=2
        )
        self.enc_flip_author_var = tk.StringVar(value="")
        ttk.Entry(
            self.enc_flip_frame, textvariable=self.enc_flip_author_var, width=14
        ).grid(row=0, column=1, sticky="w", padx=5, pady=2)

        self.enc_flip_dither_label = ttk.Label(self.enc_flip_frame, text="Dither:")
        self.enc_flip_dither_label.grid(row=1, column=0, sticky="e", padx=5, pady=2)
        self.enc_flip_dither_var = tk.StringVar(value="4x4")
        self.enc_flip_dither_cb = ttk.Combobox(
            self.enc_flip_frame,
            textvariable=self.enc_flip_dither_var,
            values=["none", "2x2", "4x4", "8x8"],
            state="readonly",
            width=10,
        )
        self.enc_flip_dither_cb.grid(row=1, column=1, sticky="w", padx=5, pady=2)

        self.enc_flip_paper_label = ttk.Label(self.enc_flip_frame, text="Paper colour:")
        self.enc_flip_paper_label.grid(row=2, column=0, sticky="e", padx=5, pady=2)
        self.enc_flip_paper_var = tk.StringVar(value="white")
        self.enc_flip_paper_cb = ttk.Combobox(
            self.enc_flip_frame,
            textvariable=self.enc_flip_paper_var,
            values=["white", "black", "red", "yellow", "green", "blue"],
            state="readonly",
            width=10,
        )
        self.enc_flip_paper_cb.grid(row=2, column=1, sticky="w", padx=5, pady=2)

        self.enc_flip_key_label = ttk.Label(
            self.enc_flip_frame, text="Signing key (blank = unsigned):"
        )
        self.enc_flip_key_label.grid(row=3, column=0, sticky="e", padx=5, pady=2)
        self.enc_flip_key_var = tk.StringVar(value="")
        self.enc_flip_key_entry = ttk.Entry(
            self.enc_flip_frame, textvariable=self.enc_flip_key_var
        )
        self.enc_flip_key_entry.grid(row=3, column=1, sticky="ew", padx=5, pady=2)
        self.enc_flip_key_btn = ttk.Button(
            self.enc_flip_frame,
            text="Browse...",
            command=lambda: self.browse_file(
                self.enc_flip_key_var,
                [("RSA private key", "*.der *.pem *.key"), ("All files", "*.*")],
            ),
        )
        self.enc_flip_key_btn.grid(row=3, column=2, padx=5, pady=2)

        self.enc_flip_loop_var = tk.BooleanVar(value=False)
        self.enc_flip_loop_chk = ttk.Checkbutton(
            self.enc_flip_frame,
            text="Loop playback",
            variable=self.enc_flip_loop_var,
        )
        self.enc_flip_loop_chk.grid(row=4, column=1, sticky="w", padx=5, pady=2)

        # Row 20: Toggle Advanced Options
        self.enc_adv_toggle_var = tk.BooleanVar(value=False)
        self.enc_adv_toggle_chk = ttk.Checkbutton(
            self.encode_frame,
            text="⚙ Show Advanced MobiClip Encoder Options (MOBI_*)",
            variable=self.enc_adv_toggle_var,
            command=self.on_toggle_advanced,
        )
        self.enc_adv_toggle_chk.grid(row=20, column=1, sticky="w", padx=5, pady=4)

        # Row 21: Advanced Options Frame
        self.enc_adv_frame = ttk.LabelFrame(
            self.encode_frame, text="Advanced MobiClip Tuning", padding=8
        )
        self.enc_adv_frame.grid(
            row=21, column=0, columnspan=3, sticky="ew", padx=5, pady=5
        )
        self.enc_adv_frame.columnconfigure(1, weight=1)

        # Bitrate (average-bitrate mode; overrides the quantizer)
        ttk.Label(self.enc_adv_frame, text="Bitrate (e.g. 700k, blank = use QP):").grid(
            row=0, column=0, sticky="e", padx=5, pady=2
        )
        self.enc_bitrate_var = tk.StringVar(value="")
        ttk.Entry(self.enc_adv_frame, textvariable=self.enc_bitrate_var, width=10).grid(
            row=0, column=1, sticky="w", padx=5, pady=2
        )

        # Multipass
        self.enc_multipass_var = tk.BooleanVar(value=False)
        ttk.Checkbutton(
            self.enc_adv_frame,
            text="Two-pass rate control (needs a bitrate)",
            variable=self.enc_multipass_var,
        ).grid(row=1, column=0, columnspan=2, sticky="w", padx=5, pady=2)

        # MOBI_SUBME
        ttk.Label(
            self.enc_adv_frame, text="Subpel/RD refine 2-9 (blank = preset):"
        ).grid(row=2, column=0, sticky="e", padx=5, pady=2)
        self.enc_mobi_subme_var = tk.StringVar(value="9")
        ttk.Entry(
            self.enc_adv_frame, textvariable=self.enc_mobi_subme_var, width=6
        ).grid(row=2, column=1, sticky="w", padx=5, pady=2)

        # MOBI_SKIP
        ttk.Label(
            self.enc_adv_frame, text="MOBI_SKIP (MB Skip Threshold, default 512):"
        ).grid(row=3, column=0, sticky="e", padx=5, pady=2)
        self.enc_mobi_skip_var = tk.StringVar(value="0")
        ttk.Entry(
            self.enc_adv_frame, textvariable=self.enc_mobi_skip_var, width=6
        ).grid(row=3, column=1, sticky="w", padx=5, pady=2)

        # MOBI_INTRA_ONLY
        self.enc_mobi_intra_var = tk.BooleanVar(value=False)
        ttk.Checkbutton(
            self.enc_adv_frame,
            text="MOBI_INTRA_ONLY (Force Keyframes Only)",
            variable=self.enc_mobi_intra_var,
        ).grid(row=4, column=1, sticky="w", padx=5, pady=2)

        # Extra ffmpeg parameters. Always visible: it's the escape hatch for
        # every option this tab doesn't have a widget for, and it goes on the
        # ffmpeg command line last, so it overrides the format preset.
        ttk.Label(self.encode_frame, text="Extra FFmpeg parameters:").grid(
            row=22, column=0, sticky="e", padx=5, pady=5
        )
        self.enc_ffargs_var = tk.StringVar(value="")
        ttk.Entry(self.encode_frame, textvariable=self.enc_ffargs_var).grid(
            row=22, column=1, sticky="ew", padx=5, pady=5
        )
        ttk.Label(
            self.encode_frame,
            foreground="grey",
            wraplength=440,
            text="passed to ffmpeg verbatim, after the settings above (e.g. -t 5 -af volume=0.5)",
        ).grid(row=23, column=1, columnspan=2, sticky="w", padx=5)

        # Run Button
        self.enc_run_btn = ttk.Button(
            self.encode_frame, text="▶ Run Encoding", command=self.run_encoding
        )
        self.enc_run_btn.grid(row=24, column=1, pady=15)

        # Widgets that only appear for certain formats, keyed by the formats
        # that should show them. Hiding uses grid_remove() (not state=disabled)
        # so rows collapse instead of sitting there greyed out.
        self.enc_conditional_widgets = [
            (
                {"moflex3d", "topos3d", "topos3d_mpeg2", "3ds_camera3d"},
                (self.enc_input2_label, self.enc_input2_entry, self.enc_input2_btn),
            ),
            ({"moflex3d"}, (self.enc_layout_label, self.enc_layout_entry)),
            (
                {"mo", "moflex", "moflex3d", "vx"},
                (self.enc_keyframes_label, self.enc_keyframes_entry),
            ),
            (
                {
                    "vx",
                    "mo",
                    "moflex",
                    "moflex3d",
                    "mods",
                    "ty",
                    "thp",
                    "wii_photo",
                    "3ds_camera",
                    "3ds_camera3d",
                    "factor5",
                    "topos",
                    "topos3d",
                    "mivf",
                },
                (self.enc_quant_label, self.enc_quant_entry),
            ),
            (
                {
                    "vx",
                    "mods",
                    "ty",
                    "thp",
                    "rvid",
                    "dpg",
                    "wii_photo",
                    "3ds_camera",
                    "factor5",
                    "mivf",
                }
                | AUDIO_ONLY_FORMATS,
                (self.enc_arate_label, self.enc_arate_entry),
            ),
            # Scale, FPS and highest-quality describe a video stream, so they
            # go away entirely for the audio-only containers.
            (
                set(self.formats_map.values()) - AUDIO_ONLY_FORMATS,
                (self.enc_scale_label, self.enc_scale_entry),
            ),
            (
                set(self.formats_map.values()) - AUDIO_ONLY_FORMATS,
                (self.enc_fps_label, self.enc_fps_entry),
            ),
            # MIVF's encoder has its own sensible default (QP 34); Highest
            # Quality's QP 12 would make files several times larger.
            (
                set(self.formats_map.values()) - AUDIO_ONLY_FORMATS - NO_HQ_FORMATS,
                (self.enc_hq_chk,),
            ),
            ({"rvid"}, (self.enc_rvid_mode_label, self.enc_rvid_mode_cb)),
            ({"vx", "mods"}, (self.enc_fast_audio_chk,)),
            ({"mivf"}, (self.enc_m2y2_chk,)),
            ({"rvid"}, (self.enc_rvid_nocompress_chk,)),
            ({"rvid"}, (self.enc_rvid_interlaced_chk,)),
            ({"rvid"}, (self.enc_rvid_nodither_chk,)),
            ({"flipnote", "flipnote3d"}, (self.enc_flip_frame,)),
            (
                {"flipnote3d"},
                (
                    self.enc_flip_dither_label,
                    self.enc_flip_dither_cb,
                    self.enc_flip_paper_label,
                    self.enc_flip_paper_cb,
                    self.enc_flip_key_label,
                    self.enc_flip_key_entry,
                    self.enc_flip_key_btn,
                    self.enc_flip_loop_chk,
                ),
            ),
            ({"mo", "moflex", "moflex3d", "mods"}, (self.enc_adv_toggle_chk,)),
        ]

        self.enc_input_var.trace_add(
            "write",
            lambda *a: self.on_input_changed(self.enc_input_var, self.enc_outdir_var),
        )
        self.enc_input_var.trace_add("write", lambda *a: self.on_encode_input_changed())
        self.enc_outdir_var.trace_add(
            "write", lambda *a: self.on_encode_input_changed()
        )
        self.enc_fmt_var.trace_add("write", lambda *a: self.on_encode_input_changed())
        for var in (
            self.enc_quant_var,
            self.enc_mobi_subme_var,
            self.enc_mobi_skip_var,
        ):
            var.trace_add("write", self.update_hq_state_from_fields)
        self.on_enc_format_change()

    def on_toggle_hq(self):
        if self.enc_hq_var.get():
            self.enc_quant_var.set("12")
            self.enc_mobi_subme_var.set("9")
            self.enc_mobi_skip_var.set("0")
            fmt = self.formats_map.get(self.enc_fmt_var.get())
            options = self.audio_options.get(fmt, [])
            if "pcm" in options:
                self.enc_audio_var.set("pcm")
        else:
            self.enc_quant_var.set("0")
            self.enc_mobi_subme_var.set("")
            self.enc_mobi_skip_var.set("512")
            fmt = self.formats_map.get(self.enc_fmt_var.get())
            options = self.audio_options.get(fmt, [])
            if "adpcm" in options:
                self.enc_audio_var.set("adpcm")

    def update_hq_state_from_fields(self, *args):
        is_hq = (
            self.enc_quant_var.get().strip() == "12"
            and self.enc_mobi_subme_var.get().strip() == "9"
            and self.enc_mobi_skip_var.get().strip() == "0"
        )
        if is_hq and not self.enc_hq_var.get():
            self.enc_hq_var.set(True)
        elif not is_hq and self.enc_hq_var.get():
            self.enc_hq_var.set(False)

    def setup_decode_tab(self):
        self.decode_frame.columnconfigure(1, weight=1)

        # Input File. The supported extensions used to sit in the label, which
        # made one very wide line; they live in the file dialog's type filter
        # and in the wrapped hint underneath instead.
        ttk.Label(self.decode_frame, text="Input File:").grid(
            row=0, column=0, sticky="e", padx=5, pady=5
        )
        self.dec_input_var = tk.StringVar()
        ttk.Entry(self.decode_frame, textvariable=self.dec_input_var).grid(
            row=0, column=1, sticky="ew", padx=5, pady=5
        )
        ttk.Button(
            self.decode_frame,
            text="Browse...",
            command=lambda: self.browse_file(self.dec_input_var, DECODE_FILETYPES),
        ).grid(row=0, column=2, padx=5, pady=5)

        # Collapsed by default: the file dialog's own type filter is what
        # people actually need day to day, this is just for "does it support
        # X" questions.
        formats = CollapsibleSection(self.decode_frame, "Supported formats")
        formats.grid(row=1, column=0, columnspan=3, sticky="ew", padx=5, pady=(0, 8))
        for i, (name, exts) in enumerate(DECODER_FAMILIES):
            cell = ttk.Frame(formats.body)
            cell.grid(
                row=i // DECODER_GRID_COLUMNS,
                column=i % DECODER_GRID_COLUMNS,
                sticky="nw",
                padx=(0, 24),
                pady=(0, 6),
            )
            ttk.Label(cell, text=name).pack(anchor="w")
            ttk.Label(
                cell,
                foreground="grey",
                text="  ".join(e.lstrip("*") for e in exts.split()),
            ).pack(anchor="w")
        for col in range(DECODER_GRID_COLUMNS):
            formats.body.columnconfigure(col, weight=1, uniform="fmt")
        ttk.Label(
            formats.body, foreground="grey", text="(plus any format ffmpeg reads)"
        ).grid(
            row=(len(DECODER_FAMILIES) - 1) // DECODER_GRID_COLUMNS + 1,
            column=0,
            columnspan=DECODER_GRID_COLUMNS,
            sticky="w",
            pady=(2, 0),
        )

        # Output file: directory + filename in one field ("Save As..." picks
        # both at once). For a stereoscopic input with "Both" eyes, the eye
        # name is appended to this same location (name_left.mp4 / name_right.mp4)
        # -- there's no separate output-directory field, since it would just be
        # redundant with this path's dirname.
        ttk.Label(self.decode_frame, text="Output File:").grid(
            row=3, column=0, sticky="e", padx=5, pady=5
        )
        self.dec_output_var = tk.StringVar(value="")
        ttk.Entry(self.decode_frame, textvariable=self.dec_output_var).grid(
            row=3, column=1, sticky="ew", padx=5, pady=5
        )
        ttk.Button(
            self.decode_frame, text="Save As...", command=self.browse_save_decode
        ).grid(row=3, column=2, padx=5, pady=5)

        # A stereoscopic input is always detected and split into separate
        # left/right files automatically -- no eye picker, no layout override.
        # This note just says whether that happened.
        self.dec_eyes_note = ttk.Label(self.decode_frame, text="", foreground="grey")
        self.dec_eyes_note.grid(row=4, column=1, sticky="w", padx=5)

        # Buttons. Play is the no-output-file path: it decodes straight to a
        # window, so it's the quick way to check a file before committing to a
        # full decode.
        ttk.Label(self.decode_frame, text="Extra FFmpeg parameters:").grid(
            row=5, column=0, sticky="e", padx=5, pady=5
        )
        self.dec_ffargs_var = tk.StringVar(value="")
        ttk.Entry(self.decode_frame, textvariable=self.dec_ffargs_var).grid(
            row=5, column=1, sticky="ew", padx=5, pady=5
        )
        # On its own row, spanning columns 1-2 with a wraplength: an unwrapped
        # single-line hint here (as a same-row column-2 sibling) forces column
        # 2 wide enough to fit the whole sentence, which starves column 1 --
        # the column the actual input fields live in -- down to a few pixels.
        # This tab has fewer competing rows than Encode's to counteract that
        # pull, so it showed up here as entries rendering only a few px wide.
        ttk.Label(
            self.decode_frame,
            foreground="grey",
            wraplength=440,
            text="passed to ffmpeg verbatim, after the settings above (applies to Play too)",
        ).grid(row=6, column=1, columnspan=2, sticky="w", padx=5)

        btns = ttk.Frame(self.decode_frame)
        btns.grid(row=7, column=1, pady=15, sticky="w")
        self.dec_play_btn = ttk.Button(btns, text="▶ Play", command=self.run_play)
        self.dec_play_btn.grid(row=0, column=0, padx=(0, 10))
        self.dec_run_btn = ttk.Button(
            btns, text="▶ Run Decoding", command=self.run_decoding
        )
        self.dec_run_btn.grid(row=0, column=1)

        self.dec_input_var.trace_add("write", lambda *a: self.on_decode_input_changed())

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
        if (
            fmt in ("mo", "moflex", "moflex3d", "mods")
            and self.enc_adv_toggle_var.get()
        ):
            self.enc_adv_frame.grid()
        else:
            self.enc_adv_frame.grid_remove()

    def on_enc_format_change(self, event=None):
        fmt = self.formats_map.get(self.enc_fmt_var.get())

        # Highest Quality does not apply to these formats: leave their
        # quantizer at the format default while one is selected, and put
        # Highest Quality back on when moving to a format it applies to.
        prev = getattr(self, "_enc_prev_fmt", None)
        if fmt in NO_HQ_FORMATS and prev not in NO_HQ_FORMATS:
            self._hq_before_no_hq = self.enc_hq_var.get()
            if self._hq_before_no_hq:
                self.enc_quant_var.set("0")
        elif fmt not in NO_HQ_FORMATS and prev in NO_HQ_FORMATS:
            if getattr(self, "_hq_before_no_hq", False):
                self.enc_hq_var.set(True)
                self.on_toggle_hq()
        self._enc_prev_fmt = fmt

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

    def browse_file(self, var, filetypes=None):
        filename = filedialog.askopenfilename(
            filetypes=filetypes or [("All files", "*.*")]
        )
        if filename:
            var.set(filename)

    def browse_dir(self, var):
        directory = filedialog.askdirectory()
        if directory:
            var.set(directory)

    def browse_save_decode(self):
        """Save As for the decoded output, seeded with the derived name."""
        current = self.dec_output_var.get().strip()
        initial = os.path.basename(current) if current else self.derived_decode_name()
        initialdir = os.path.dirname(current) if current else ""
        filename = filedialog.asksaveasfilename(
            title="Save decoded video as",
            initialfile=initial or "decoded.mp4",
            initialdir=initialdir or None,
            defaultextension=".mp4",
            filetypes=[("MP4 video", "*.mp4"), ("All files", "*.*")],
        )
        if filename:
            self.dec_output_var.set(filename)

    def derived_encode_name(self):
        """Default output filename for the currently selected encode input/format."""
        inp = self.enc_input_var.get().strip()
        if not inp:
            return ""
        fmt = self.formats_map.get(self.enc_fmt_var.get(), "mo")
        ext = ENCODE_EXTENSIONS.get(fmt, fmt)
        return os.path.splitext(os.path.basename(inp))[0] + "." + ext

    def on_encode_input_changed(self):
        name = self.derived_encode_name()
        # Only auto-fill while the user hasn't typed their own path, so an
        # explicit choice survives picking a different input/format/outdir.
        current = self.enc_output_var.get().strip()
        if name and (not current or getattr(self, "_enc_output_auto", "") == current):
            outdir = self.enc_outdir_var.get().strip()
            full = os.path.join(outdir, name) if outdir else name
            self.enc_output_var.set(full)
            self._enc_output_auto = full

    def browse_save_encode(self):
        fmt = self.formats_map.get(self.enc_fmt_var.get(), "mo")
        ext = ENCODE_EXTENSIONS.get(fmt, fmt)
        filename = filedialog.asksaveasfilename(
            initialfile=self.derived_encode_name(),
            defaultextension="." + ext,
            filetypes=[(f"{ext} file", f"*.{ext}"), ("All files", "*.*")],
        )
        if filename:
            self.enc_output_var.set(filename)

    def derived_decode_name(self):
        """Default output filename for the currently selected decode input."""
        inp = self.dec_input_var.get().strip()
        if not inp:
            return ""
        return os.path.splitext(os.path.basename(inp))[0] + ".mp4"

    def on_decode_input_changed(self):
        inp = self.dec_input_var.get()
        if inp.startswith("{") and inp.endswith("}"):
            self.dec_input_var.set(inp[1:-1])
            inp = self.dec_input_var.get()
        name = self.derived_decode_name()
        # Only auto-fill while the user hasn't typed their own name, so an
        # explicit choice survives picking a different input.
        current = self.dec_output_var.get().strip()
        if name and (not current or getattr(self, "_dec_output_auto", "") == current):
            outdir = os.path.dirname(inp) if inp and os.path.isfile(inp) else ""
            full = os.path.join(outdir, name) if outdir else name
            self.dec_output_var.set(full)
            self._dec_output_auto = full
        self.update_decode_eyes_note()

    def update_decode_eyes_note(self):
        """Say whether the selected input actually is stereoscopic.

        The check runs ffprobe, so it is debounced and done off the Tk thread
        to keep typing a path responsive.
        """
        pending = getattr(self, "_eyes_note_job", None)
        if pending is not None:
            self.after_cancel(pending)
        self._eyes_note_job = self.after(300, self._start_eyes_probe)

    def _start_eyes_probe(self):
        self._eyes_note_job = None
        inp = self.dec_input_var.get().strip()
        if not (inp and os.path.isfile(inp)):
            self._set_eyes_note(inp, "")
            return

        def probe():
            note = ""
            try:
                import encode as _enc

                kind, inverted = _enc.stereo_layout(inp, _enc.input_fmt(inp))
                if kind:
                    note = (
                        f"stereoscopic ({kind}{', eyes swapped' if inverted else ''})"
                    )
                else:
                    note = "2D input - eye selection ignored"
            except Exception:
                note = ""
            self.after(0, self._set_eyes_note, inp, note)

        threading.Thread(target=probe, daemon=True).start()

    def _set_eyes_note(self, inp, note):
        # Drop results for an input the user has since changed.
        if inp != self.dec_input_var.get().strip():
            return
        if hasattr(self, "dec_eyes_note"):
            self.dec_eyes_note.config(text=note)

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
        self.append_console(f"$ {shlex.join(cmd)}\n\n")

        def run_thread():
            # --windowed builds have no console, so on Linux/Windows/macOS an
            # uncaught exception here (e.g. a bundled binary that lost its
            # executable bit, or the frozen re-exec failing) would otherwise
            # vanish silently: the thread dies, the Run button never
            # re-enables, and the user sees the GUI simply do nothing.
            try:
                process = subprocess.Popen(
                    cmd,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.STDOUT,
                    text=True,
                    encoding="utf-8",
                    errors="replace",
                    bufsize=1,
                    # The child's stdout is a pipe, so Python would otherwise
                    # block-buffer it and the console would stay empty until
                    # the encode finished; also pin its encoding to match.
                    env=dict(
                        os.environ, PYTHONUNBUFFERED="1", PYTHONIOENCODING="utf-8"
                    ),
                )
                for line in process.stdout:
                    self.after(0, self.append_console, line)

                process.wait()
                self.after(
                    0,
                    self.append_console,
                    f"\nProcess finished with exit code {process.returncode}\n",
                )
            except Exception as e:
                self.after(0, self.append_console, f"\nFailed to run command: {e}\n")
            finally:
                self.after(0, lambda: btn.config(state="normal"))

        threading.Thread(target=run_thread, daemon=True).start()

    def run_encoding(self):
        if not getattr(sys, "frozen", False) and not os.path.exists(ENCODE_SCRIPT):
            messagebox.showerror(
                "Error", f"Could not find encode script at:\n{ENCODE_SCRIPT}"
            )
            return

        fmt = self.formats_map.get(self.enc_fmt_var.get())
        audio = self.enc_audio_var.get()
        inp1 = self.enc_input_var.get()
        inp2 = self.enc_input2_var.get()
        scale = self.enc_scale_var.get()
        outdir = self.enc_outdir_var.get()
        outfile = self.enc_output_var.get().strip()

        if not inp1:
            messagebox.showwarning("Warning", "Please select an input file.")
            return

        cmd = (
            [sys.executable, "--encode-script"]
            if getattr(sys, "frozen", False)
            else [sys.executable, ENCODE_SCRIPT]
        )
        cmd.extend([fmt, audio, inp1])

        if fmt in ("moflex3d", "topos3d", "topos3d_mpeg2", "3ds_camera3d"):
            if inp2:
                cmd.append(inp2)
            if fmt == "moflex3d":
                layout = self.enc_layout_var.get().strip()
                if layout and layout != "4":
                    cmd.extend(["--layout", layout])
        if scale:
            cmd.extend(["--scale", scale])
        # An explicit output file wins over Output Dir (it carries its own
        # directory); --outdir still governs where intermediate/roundtrip
        # preview files land even when -o is set.
        if outfile:
            cmd.extend(["-o", outfile])
        if outdir:
            cmd.extend(["--outdir", outdir])
        if fmt in ("mo", "moflex", "moflex3d", "vx"):
            kf = self.enc_keyframes_var.get().strip()
            if kf and kf != "0":
                cmd.extend(["--keyframes", kf])
        if self.enc_roundtrip_var.get():
            cmd.append("--roundtrip")
        if self.enc_hq_var.get() and fmt not in NO_HQ_FORMATS:
            cmd.append("--hq")
        if self.enc_m2y2_var.get() and fmt == "mivf":
            cmd.append("--m2y2")
        if self.enc_fast_audio_var.get() and fmt in ("vx", "mods"):
            cmd.append("--fast-audio")
        if fmt in (
            "vx",
            "mo",
            "moflex",
            "moflex3d",
            "mods",
            "ty",
            "thp",
            "wii_photo",
            "3ds_camera",
            "3ds_camera3d",
            "factor5",
            "topos",
            "topos3d",
            "mivf",
        ):
            q = self.enc_quant_var.get().strip()
            if q and q != "0":
                cmd.extend(["--quantizer", q])
        if fmt in (
            "mo",
            "moflex",
            "moflex3d",
            "mods",
            "vx",
            "ty",
            "gba_ads",
            "gba_hydrogen",
            "wii_photo",
            "nintendo_channel",
            "thp",
            "rvid",
            "dpg",
            "factor5",
            "flipnote",
            "flipnote3d",
            "topos",
            "topos3d",
            "topos_mpeg2",
            "topos3d_mpeg2",
            "mivf",
        ):
            fps = self.enc_fps_var.get().strip()
            if fps:
                cmd.extend(["--fps", fps])
        if fmt in (
            {"vx", "mods", "ty", "thp", "rvid", "dpg", "wii_photo", "factor5", "mivf"}
            | AUDIO_ONLY_FORMATS
        ):
            arate = self.enc_audio_rate_var.get().strip()
            if arate and arate != "0":
                cmd.extend(["--audio-rate", arate])
        if (
            fmt in ("mo", "moflex", "moflex3d", "mods")
            and self.enc_adv_toggle_var.get()
        ):
            bitrate = self.enc_bitrate_var.get().strip()
            if bitrate:
                cmd.extend(["--bitrate", bitrate])
                if self.enc_multipass_var.get():
                    cmd.extend(["--multipass", "2"])
            subme = self.enc_mobi_subme_var.get().strip()
            if subme:
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
        if fmt in ("flipnote", "flipnote3d"):
            author = self.enc_flip_author_var.get().strip()
            if author:
                cmd.extend(["--flipnote-author", author])
        if fmt == "flipnote3d":
            cmd.extend(["--flipnote-dither", self.enc_flip_dither_var.get()])
            cmd.extend(["--flipnote-paper", self.enc_flip_paper_var.get()])
            key = self.enc_flip_key_var.get().strip()
            if key:
                cmd.extend(["--flipnote-key", key])
            if self.enc_flip_loop_var.get():
                cmd.append("--flipnote-loop")

        self.add_ffargs(cmd, self.enc_ffargs_var)

        self.execute_cmd(cmd, self.enc_run_btn)

    @staticmethod
    def add_ffargs(cmd, var):
        """Append the tab's extra-parameters field, if it has anything in it.

        encode.py splits the string itself (shlex), so it travels as one argv
        entry and quoted filter graphs survive the trip."""
        extra = var.get().strip()
        if extra:
            cmd.extend(["--ffmpeg-args", extra])

    def run_play(self):
        """Play the selected file in a window without writing an output file."""
        if not getattr(sys, "frozen", False) and not os.path.exists(ENCODE_SCRIPT):
            messagebox.showerror(
                "Error", f"Could not find encode script at:\n{ENCODE_SCRIPT}"
            )
            return

        inp = self.dec_input_var.get()
        if not inp:
            messagebox.showwarning("Warning", "Please select a file to play.")
            return

        cmd = (
            [sys.executable, "--encode-script"]
            if getattr(sys, "frozen", False)
            else [sys.executable, ENCODE_SCRIPT]
        )
        cmd.extend(["play", inp])
        self.add_ffargs(cmd, self.dec_ffargs_var)

        self.execute_cmd(cmd, self.dec_play_btn)

    def run_decoding(self):
        if not getattr(sys, "frozen", False) and not os.path.exists(ENCODE_SCRIPT):
            messagebox.showerror(
                "Error", f"Could not find encode script at:\n{ENCODE_SCRIPT}"
            )
            return

        inp = self.dec_input_var.get()

        if not inp:
            messagebox.showwarning("Warning", "Please select an input file to decode.")
            return

        cmd = (
            [sys.executable, "--encode-script"]
            if getattr(sys, "frozen", False)
            else [sys.executable, ENCODE_SCRIPT]
        )
        cmd.extend(["decode", inp])

        outfile = self.dec_output_var.get().strip()
        if outfile:
            cmd.extend(["-o", outfile])
        self.add_ffargs(cmd, self.dec_ffargs_var)

        self.execute_cmd(cmd, self.dec_run_btn)

    def setup_cia_tab(self):
        self.cia_frame.columnconfigure(1, weight=1)

        ttk.Label(self.cia_frame, text="Input .moflex:").grid(
            row=0, column=0, sticky="e", padx=5, pady=5
        )
        self.cia_input_var = tk.StringVar()
        ttk.Entry(self.cia_frame, textvariable=self.cia_input_var).grid(
            row=0, column=1, sticky="ew", padx=5, pady=5
        )
        ttk.Button(
            self.cia_frame,
            text="Browse...",
            command=lambda: self.browse_file(
                self.cia_input_var,
                [("MobiClip MOFLEX", "*.moflex"), ("All files", "*.*")],
            ),
        ).grid(row=0, column=2, padx=5, pady=5)

        ttk.Label(self.cia_frame, text="Title:").grid(
            row=1, column=0, sticky="e", padx=5, pady=5
        )
        self.cia_title_var = tk.StringVar()
        ttk.Entry(self.cia_frame, textvariable=self.cia_title_var).grid(
            row=1, column=1, sticky="ew", padx=5, pady=5
        )
        ttk.Label(
            self.cia_frame,
            foreground="grey",
            text="shown on the HOME Menu; leave blank to use the file name",
        ).grid(row=2, column=1, sticky="w", padx=5)

        ttk.Label(self.cia_frame, text="Publisher:").grid(
            row=3, column=0, sticky="e", padx=5, pady=5
        )
        self.cia_publisher_var = tk.StringVar(value="mobipeg")
        ttk.Entry(self.cia_frame, textvariable=self.cia_publisher_var).grid(
            row=3, column=1, sticky="ew", padx=5, pady=5
        )

        ttk.Label(self.cia_frame, text="Icon (optional):").grid(
            row=4, column=0, sticky="e", padx=5, pady=5
        )
        self.cia_icon_var = tk.StringVar()
        ttk.Entry(self.cia_frame, textvariable=self.cia_icon_var).grid(
            row=4, column=1, sticky="ew", padx=5, pady=5
        )
        ttk.Button(
            self.cia_frame,
            text="Browse...",
            command=lambda: self.browse_file(
                self.cia_icon_var,
                [("Image", "*.png *.jpg *.jpeg"), ("All files", "*.*")],
            ),
        ).grid(row=4, column=2, padx=5, pady=5)

        ttk.Label(self.cia_frame, text="Banner (optional):").grid(
            row=5, column=0, sticky="e", padx=5, pady=5
        )
        self.cia_banner_var = tk.StringVar()
        ttk.Entry(self.cia_frame, textvariable=self.cia_banner_var).grid(
            row=5, column=1, sticky="ew", padx=5, pady=5
        )
        ttk.Button(
            self.cia_frame,
            text="Browse...",
            command=lambda: self.browse_file(
                self.cia_banner_var,
                [("Image", "*.png *.jpg *.jpeg"), ("All files", "*.*")],
            ),
        ).grid(row=5, column=2, padx=5, pady=5)
        ttk.Label(
            self.cia_frame,
            foreground="grey",
            wraplength=440,
            text="leave icon/banner blank to use VidInjector's built-in placeholders",
        ).grid(row=6, column=1, columnspan=2, sticky="w", padx=5)

        ttk.Label(self.cia_frame, text="Output .cia:").grid(
            row=7, column=0, sticky="e", padx=5, pady=5
        )
        self.cia_output_var = tk.StringVar()
        ttk.Entry(self.cia_frame, textvariable=self.cia_output_var).grid(
            row=7, column=1, sticky="ew", padx=5, pady=5
        )
        ttk.Button(
            self.cia_frame, text="Save As...", command=self.browse_save_cia
        ).grid(row=7, column=2, padx=5, pady=5)

        btns = ttk.Frame(self.cia_frame)
        btns.grid(row=8, column=1, pady=15, sticky="w")
        self.cia_run_btn = ttk.Button(
            btns, text="▶ Package CIA", command=self.run_cia_packaging
        )
        self.cia_run_btn.grid(row=0, column=0)

        self.cia_input_var.trace_add("write", lambda *a: self.on_cia_input_changed())

    def on_cia_input_changed(self):
        """Seed the title field and output path from the chosen .moflex, same
        courtesy the Encode/Decode tabs give their own inputs."""
        inp = self.cia_input_var.get()
        if inp.startswith("{") and inp.endswith("}"):
            self.cia_input_var.set(inp[1:-1])
            inp = self.cia_input_var.get()
        if not inp or not os.path.isfile(inp):
            return
        stem = os.path.splitext(os.path.basename(inp))[0]
        if not self.cia_title_var.get().strip():
            self.cia_title_var.set(stem)
        if not self.cia_output_var.get().strip():
            self.cia_output_var.set(os.path.join(os.path.dirname(inp), stem + ".cia"))

    def browse_save_cia(self):
        current = self.cia_output_var.get().strip()
        stem = (
            os.path.splitext(os.path.basename(self.cia_input_var.get()))[0]
            if self.cia_input_var.get()
            else "package"
        )
        initial = os.path.basename(current) if current else stem + ".cia"
        initialdir = os.path.dirname(current) if current else ""
        filename = filedialog.asksaveasfilename(
            title="Save CIA as",
            initialfile=initial,
            initialdir=initialdir or None,
            defaultextension=".cia",
            filetypes=[("3DS CIA", "*.cia"), ("All files", "*.*")],
        )
        if filename:
            self.cia_output_var.set(filename)

    def run_cia_packaging(self):
        if not getattr(sys, "frozen", False) and not os.path.exists(ENCODE_SCRIPT):
            messagebox.showerror(
                "Error", f"Could not find encode script at:\n{ENCODE_SCRIPT}"
            )
            return

        inp = self.cia_input_var.get().strip()
        if not inp:
            messagebox.showwarning(
                "Warning", "Please select a .moflex file to package."
            )
            return

        cmd = (
            [sys.executable, "--encode-script"]
            if getattr(sys, "frozen", False)
            else [sys.executable, ENCODE_SCRIPT]
        )
        cmd.extend(["cia", inp])

        title = self.cia_title_var.get().strip()
        if title:
            cmd.extend(["--cia-title", title])
        publisher = self.cia_publisher_var.get().strip()
        if publisher:
            cmd.extend(["--cia-publisher", publisher])
        icon = self.cia_icon_var.get().strip()
        if icon:
            cmd.extend(["--cia-icon", icon])
        banner = self.cia_banner_var.get().strip()
        if banner:
            cmd.extend(["--cia-banner", banner])
        outfile = self.cia_output_var.get().strip()
        if outfile:
            cmd.extend(["-o", outfile])

        self.execute_cmd(cmd, self.cia_run_btn)


MobipegGUI = EncodeGUI


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "--encode-script":
        import encode

        sys.argv = [sys.argv[0]] + sys.argv[2:]
        encode.main()
        sys.exit(0)
    app = EncodeGUI()
    app.mainloop()
