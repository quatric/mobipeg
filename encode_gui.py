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
import re
import sys

import mobipeg_tools as mt
from gui_extras import FlatTabs, ScrollableFrame, PathListEditor, QueueTab, InspectTab

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
MOBIPEG_VERSION = "4.0"


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
    ("Bink", "*.bik *.bk2"),
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
NO_HQ_FORMATS = {"mivf", "bink"}

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
    "moflex3d": "moflex",
    "topos": "mkv",
    "topos3d": "mkv",
    "topos_mpeg2": "mkv",
    "topos3d_mpeg2": "mkv",
    "3ds_camera": "avi",
    "3ds_camera3d": "avi",
    "wii_photo_m4a": "m4a",
    "3ds_sound": "m4a",
    "factor5": "vid",
    "bink": "bik",
    "bink2": "bk2",
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

        self.notebook = FlatTabs(self.panes)
        self.panes.add(self.notebook, weight=1)

        # --- ENCODE TAB ---
        # The form is taller than most screens, so it scrolls inside a fixed
        # viewport (the scrollbar only appears when something is cut off).
        self.encode_scroll = ScrollableFrame(self.notebook, height=520)
        self.encode_frame = self.encode_scroll.inner
        self.notebook.add(self.encode_scroll, text="Encode")
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

        # --- QUEUE TAB: batch conversions, one after another ---
        self.queue_tab = QueueTab(self.notebook, self)
        self.notebook.add(self.queue_tab, text="Queue")

        # --- INSPECT TAB: read-only MOFLEX preflight ---
        self.inspect_tab = InspectTab(self.notebook, self)
        self.notebook.add(self.inspect_tab, text="Inspect")

        # --- CONSOLE ---
        console_box = ttk.Frame(self.panes)
        self.panes.add(console_box, weight=1)
        self.build_status_bar(console_box)
        ttk.Label(console_box, text="Console Output:").pack(anchor="w", pady=(6, 0))
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

        self.load_settings()
        self.protocol("WM_DELETE_WINDOW", self.on_close)

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
            "PC Bink 1 video .bik": "bink",
            "PC Bink 2 video .bk2": "bink2",
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
            # LibBink plays mono Bink audio only; FFmpeg also decodes stereo.
            "bink": ["bink", "bink_stereo", "none"],
            "bink2": ["bink", "bink_stereo", "none"],
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
            row=25, column=0, sticky="e", padx=5, pady=5
        )
        self.enc_ffargs_var = tk.StringVar(value="")
        ttk.Entry(self.encode_frame, textvariable=self.enc_ffargs_var).grid(
            row=25, column=1, sticky="ew", padx=5, pady=5
        )
        ttk.Label(
            self.encode_frame,
            foreground="grey",
            wraplength=440,
            text="passed to ffmpeg verbatim, after the settings above (e.g. -t 5 -af volume=0.5)",
        ).grid(row=26, column=1, columnspan=2, sticky="w", padx=5)

        # Run Button
        run_row = ttk.Frame(self.encode_frame)
        run_row.grid(row=27, column=1, columnspan=2, pady=15, sticky="w")
        self.enc_run_btn = ttk.Button(
            run_row, text="▶ Run Encoding", command=self.run_encoding
        )
        self.enc_run_btn.pack(side="left")
        ttk.Button(run_row, text="Add to queue", command=self.add_current_to_queue).pack(
            side="left", padx=(10, 0)
        )
        ttk.Button(run_row, text="Copy command", command=self.copy_command).pack(
            side="left", padx=(10, 0)
        )

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
                    "bink",
                    "bink2",
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
                    "bink",
                    "bink2",
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

        self.setup_extra_encode_controls()

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

    # ------------------------------------------------------------------
    # Extra Encode-tab controls: source info, audio, size, HDR, parallel
    # jobs, Super MOFLEX, more MobiClip tuning, presets, saved settings.
    # ------------------------------------------------------------------

    MOBI_SIZE_FORMATS = {"mo", "moflex", "moflex3d", "mods"}
    JOBS_FORMATS = {
        "mo", "moflex", "mods", "topos", "topos3d", "topos_mpeg2",
        "topos3d_mpeg2", "mivf",
    }
    STEREO_FORMATS = {"moflex3d", "topos3d", "topos3d_mpeg2"}
    SUPER_FORMATS = {"moflex", "moflex3d"}
    NO_AUDIO_FORMATS = {"hvqm4", "gba_ads", "gba_hydrogen"}
    LEVELS = ["Simple", "Intermediate", "Advanced"]
    # (lowest level that shows them, widget attributes). Applied on top of the
    # per-format visibility, so a control is shown only if both allow it.
    # The old "Show Advanced" checkbox is replaced by the Advanced level.
    LEVEL_GATES = (
        (1, ("enc_scale_label", "enc_scale_entry", "enc_fps_label", "enc_fps_entry",
             "enc_keyframes_label", "enc_keyframes_entry", "enc_arate_label",
             "enc_arate_entry", "enc_roundtrip_chk", "enc_fast_audio_chk",
             "enc_quant_label", "enc_quant_entry", "enc_offset_label",
             "enc_offset_entry", "enc_offset_hint", "enc_normalize_chk",
             "enc_hdr_label", "enc_hdr_cb", "enc_hdr_hint", "enc_super_frame")),
        (2, ("enc_layout_label", "enc_layout_entry", "enc_jobs_label",
             "enc_jobs_entry", "enc_jobs_hint")),
        (3, ("enc_adv_toggle_chk",)),  # never: the level picker replaces it
    )
    EXTRA_FFARGS_ROWS = (25, 26)  # grid rows of the "Extra FFmpeg parameters" field

    def level_index(self):
        return self.LEVELS.index(self.enc_level_var.get())

    def on_level_change(self):
        self.enc_adv_toggle_var.set(self.level_index() == 2)
        self.on_enc_format_change()

    def apply_level_gates(self):
        lvl = self.level_index()
        for minimum, names in self.LEVEL_GATES:
            if lvl < minimum:
                for n in names:
                    getattr(self, n).grid_remove()
        if lvl < 2:
            for w in self.enc_ffargs_widgets:
                w.grid_remove()
        if not any(w.grid_info() for w in self.enc_out_frame.winfo_children()):
            self.enc_out_frame.grid_remove()

    STEREO_CHOICES = ["auto", "sbs", "sbs-r", "tb", "tb-r", "frameseq", "frameseq-r"]

    def setup_extra_encode_controls(self):
        f = self.encode_frame
        self.src_info = None
        self._src_job = None
        # Gridded once and kept, since grid_slaves() cannot find a hidden widget.
        self.enc_ffargs_widgets = [
            w for r in self.EXTRA_FFARGS_ROWS for w in f.grid_slaves(row=r)
        ]

        # --- Level: how many options to show ---
        lvl = ttk.Frame(f)
        lvl.grid(row=1, column=2, padx=5, pady=5)
        ttk.Label(lvl, text="Level:").pack(side="left", padx=(0, 4))
        self.enc_level_var = tk.StringVar(value="Intermediate")
        ttk.Combobox(
            lvl, textvariable=self.enc_level_var, values=self.LEVELS,
            state="readonly", width=12,
        ).pack(side="left")
        self.enc_level_var.trace_add("write", lambda *a: self.on_level_change())

        # --- Presets: a menu button beside the format picker ---
        self.preset_store = mt.JsonStore("presets.json")
        self.preset_btn = ttk.Menubutton(f, text="Presets ▾")
        self.preset_menu = tk.Menu(
            self.preset_btn, tearoff=0, postcommand=self.rebuild_preset_menu
        )
        self.preset_btn["menu"] = self.preset_menu
        self.preset_btn.grid(row=0, column=2, padx=5, pady=5)

        # --- Source & audio ---
        self.enc_src_frame = ttk.LabelFrame(f, text="Source and audio", padding=8)
        self.enc_src_frame.grid(row=22, column=0, columnspan=3, sticky="ew", padx=5, pady=5)
        sf = self.enc_src_frame
        sf.columnconfigure(1, weight=1)
        ttk.Label(sf, text="Source:").grid(row=0, column=0, sticky="e", padx=5, pady=2)
        self.enc_src_label = ttk.Label(sf, text="choose an input file", foreground="grey", wraplength=470)
        self.enc_src_label.grid(row=0, column=1, columnspan=2, sticky="w", padx=5, pady=2)

        self.enc_track_label = ttk.Label(sf, text="Audio track:")
        self.enc_track_label.grid(row=1, column=0, sticky="e", padx=5, pady=2)
        self.enc_track_var = tk.StringVar(value="Default")
        self.enc_track_cb = ttk.Combobox(sf, textvariable=self.enc_track_var, state="readonly", values=["Default"])
        self.enc_track_cb.grid(row=1, column=1, columnspan=2, sticky="ew", padx=5, pady=2)

        self.enc_offset_label = ttk.Label(sf, text="Audio offset (ms):")
        self.enc_offset_label.grid(row=2, column=0, sticky="e", padx=5, pady=2)
        self.enc_offset_var = tk.StringVar(value="0")
        self.enc_offset_entry = ttk.Entry(sf, textvariable=self.enc_offset_var, width=8)
        self.enc_offset_entry.grid(row=2, column=1, sticky="w", padx=5, pady=2)
        self.enc_offset_hint = ttk.Label(sf, foreground="grey", text="+ delays the audio, − starts it earlier")
        self.enc_offset_hint.grid(row=2, column=2, sticky="w", padx=5)

        self.enc_normalize_var = tk.BooleanVar(value=False)
        self.enc_normalize_chk = ttk.Checkbutton(
            sf, text="Normalize audio peak to −1 dBFS (adds an analysis pass)",
            variable=self.enc_normalize_var,
        )
        self.enc_normalize_chk.grid(row=3, column=1, columnspan=2, sticky="w", padx=5, pady=2)

        self.enc_stereo_label = ttk.Label(sf, text="3D packing:")
        self.enc_stereo_label.grid(row=4, column=0, sticky="e", padx=5, pady=2)
        self.enc_stereo_var = tk.StringVar(value="auto")
        self.enc_stereo_cb = ttk.Combobox(
            sf, textvariable=self.enc_stereo_var, state="readonly", values=self.STEREO_CHOICES, width=10
        )
        self.enc_stereo_cb.grid(row=4, column=1, sticky="w", padx=5, pady=2)
        self.enc_stereo_hint = ttk.Label(
            sf, foreground="grey", text="only for one packed 3D file (side by side, top/bottom...)"
        )
        self.enc_stereo_hint.grid(row=4, column=2, sticky="w", padx=5)

        # --- Output & size ---
        self.enc_out_frame = ttk.LabelFrame(f, text="Output and speed", padding=8)
        self.enc_out_frame.grid(row=23, column=0, columnspan=3, sticky="ew", padx=5, pady=5)
        of = self.enc_out_frame
        of.columnconfigure(2, weight=1)

        self.enc_maxsize_label = ttk.Label(of, text="Max size (MiB):")
        self.enc_maxsize_label.grid(row=0, column=0, sticky="e", padx=5, pady=2)
        self.enc_maxsize_var = tk.StringVar(value="")
        self.enc_maxsize_entry = ttk.Entry(of, textvariable=self.enc_maxsize_var, width=8)
        self.enc_maxsize_entry.grid(row=0, column=1, sticky="w", padx=5, pady=2)
        self.enc_fat32_btn = ttk.Button(
            of, text="FAT32 limit (4000)", command=lambda: self.enc_maxsize_var.set("4000")
        )
        self.enc_fat32_btn.grid(row=0, column=2, sticky="w", padx=5, pady=2)
        self.enc_est_label = ttk.Label(of, text="", foreground="grey", wraplength=470)
        self.enc_est_label.grid(row=1, column=1, columnspan=2, sticky="w", padx=5)

        self.enc_hdr_label = ttk.Label(of, text="HDR → SDR:")
        self.enc_hdr_label.grid(row=2, column=0, sticky="e", padx=5, pady=2)
        self.enc_hdr_var = tk.StringVar(value="auto")
        self.enc_hdr_cb = ttk.Combobox(
            of, textvariable=self.enc_hdr_var, state="readonly", values=list(mt.HDR_MODES), width=10
        )
        self.enc_hdr_cb.grid(row=2, column=1, sticky="w", padx=5, pady=2)
        self.enc_hdr_hint = ttk.Label(of, foreground="grey", text="auto tone-maps HDR sources only")
        self.enc_hdr_hint.grid(row=2, column=2, sticky="w", padx=5)

        self.enc_jobs_label = ttk.Label(of, text="Parallel jobs (0=auto):")
        self.enc_jobs_label.grid(row=3, column=0, sticky="e", padx=5, pady=2)
        self.enc_jobs_var = tk.StringVar(value="0")
        self.enc_jobs_entry = ttk.Entry(of, textvariable=self.enc_jobs_var, width=8)
        self.enc_jobs_entry.grid(row=3, column=1, sticky="w", padx=5, pady=2)
        self.enc_jobs_hint = ttk.Label(of, foreground="grey", text="1 = encode in one piece")
        self.enc_jobs_hint.grid(row=3, column=2, sticky="w", padx=5)

        # --- Super MOFLEX ---
        self.enc_super_frame = ttk.LabelFrame(f, text="Super MOFLEX", padding=8)
        self.enc_super_frame.grid(row=24, column=0, columnspan=3, sticky="ew", padx=5, pady=5)
        sp = self.enc_super_frame
        sp.columnconfigure(1, weight=1)
        self.enc_super_var = tk.BooleanVar(value=False)
        ttk.Checkbutton(
            sp, text="Write a Super MOFLEX trailer (second audio, subtitles, poster)",
            variable=self.enc_super_var, command=self.refresh_super_state,
        ).grid(row=0, column=0, columnspan=3, sticky="w", padx=5, pady=2)

        ttk.Label(sp, text="Second audio:").grid(row=1, column=0, sticky="e", padx=5, pady=2)
        self.enc_audio2_var = tk.StringVar(value="")
        self.enc_audio2_entry = ttk.Entry(sp, textvariable=self.enc_audio2_var)
        self.enc_audio2_entry.grid(row=1, column=1, sticky="ew", padx=5, pady=2)
        self.enc_audio2_btn = ttk.Button(
            sp, text="Browse...", command=lambda: self.browse_file(self.enc_audio2_var)
        )
        self.enc_audio2_btn.grid(row=1, column=2, padx=5, pady=2)

        ttk.Label(sp, text="Languages:").grid(row=2, column=0, sticky="e", padx=5, pady=2)
        langs = ttk.Frame(sp)
        langs.grid(row=2, column=1, sticky="w", padx=5, pady=2)
        self.enc_lang_var = tk.StringVar(value="")
        self.enc_lang2_var = tk.StringVar(value="")
        self.enc_lang_entry = ttk.Entry(langs, textvariable=self.enc_lang_var, width=6)
        self.enc_lang_entry.pack(side="left")
        ttk.Label(langs, text=" first audio     ", foreground="grey").pack(side="left")
        self.enc_lang2_entry = ttk.Entry(langs, textvariable=self.enc_lang2_var, width=6)
        self.enc_lang2_entry.pack(side="left")
        ttk.Label(langs, text=" second audio  (e.g. eng, jpn)", foreground="grey").pack(side="left")

        ttk.Label(sp, text="Subtitles:").grid(row=3, column=0, sticky="ne", padx=5, pady=2)
        self.enc_srt_list = PathListEditor(
            sp, "Add subtitle files", [("SubRip", "*.srt"), ("All files", "*.*")]
        )
        self.enc_srt_list.grid(row=3, column=1, columnspan=2, sticky="ew", padx=5, pady=2)

        ttk.Label(sp, text="Poster:").grid(row=4, column=0, sticky="e", padx=5, pady=2)
        self.enc_poster_var = tk.StringVar(value="")
        self.enc_poster_entry = ttk.Entry(sp, textvariable=self.enc_poster_var)
        self.enc_poster_entry.grid(row=4, column=1, sticky="ew", padx=5, pady=2)
        self.enc_poster_btn = ttk.Button(
            sp, text="Browse...",
            command=lambda: self.browse_file(
                self.enc_poster_var, [("Image", "*.png *.jpg *.jpeg"), ("All files", "*.*")]
            ),
        )
        self.enc_poster_btn.grid(row=4, column=2, padx=5, pady=2)

        # --- more MobiClip tuning, inside the existing Advanced frame ---
        af = self.enc_adv_frame
        self.enc_minqp_var = tk.StringVar(value="")
        self.enc_maxqp_var = tk.StringVar(value="")
        self.enc_iboost_var = tk.StringVar(value="")
        self.enc_ithresh_var = tk.StringVar(value="")
        self.enc_bufsize_var = tk.StringVar(value="")
        self.enc_me_var = tk.StringVar(value="default")
        self.enc_dct8_var = tk.StringVar(value="default")
        self.enc_block_var = tk.StringVar(value="auto")
        rows = [
            ("Min / max QP (12-48, blank = auto):", [(self.enc_minqp_var, 6), (self.enc_maxqp_var, 6)]),
            ("I-frame boost % (blank = 40):", [(self.enc_iboost_var, 6)]),
            ("Scene-cut sensitivity 0-100 (blank = 90):", [(self.enc_ithresh_var, 6)]),
            ("Rate buffer (e.g. 400k):", [(self.enc_bufsize_var, 8)]),
        ]
        r = 5
        for text, fields in rows:
            ttk.Label(af, text=text).grid(row=r, column=0, sticky="e", padx=5, pady=2)
            box = ttk.Frame(af)
            box.grid(row=r, column=1, sticky="w", padx=5, pady=2)
            for var, width in fields:
                ttk.Entry(box, textvariable=var, width=width).pack(side="left", padx=(0, 6))
            r += 1
        for text, var, values in (
            ("Motion search:", self.enc_me_var, ["default", "dia", "hex", "umh", "esa"]),
            ("8x8 transform:", self.enc_dct8_var, ["default", "on", "off"]),
            ("MOFLEX block size:", self.enc_block_var, ["auto", "2048", "4096"]),
        ):
            ttk.Label(af, text=text).grid(row=r, column=0, sticky="e", padx=5, pady=2)
            ttk.Combobox(af, textvariable=var, values=values, state="readonly", width=10).grid(
                row=r, column=1, sticky="w", padx=5, pady=2
            )
            r += 1

        # Everything the Encode tab remembers between runs and saves in presets.
        self.persist_vars = {
            "fmt": self.enc_fmt_var, "level": self.enc_level_var,
            "audio": self.enc_audio_var,
            "outdir": self.enc_outdir_var, "scale": self.enc_scale_var,
            "keyframes": self.enc_keyframes_var, "quant": self.enc_quant_var,
            "audio_rate": self.enc_audio_rate_var, "fps": self.enc_fps_var,
            "hq": self.enc_hq_var, "roundtrip": self.enc_roundtrip_var,
            "fast_audio": self.enc_fast_audio_var, "adv": self.enc_adv_toggle_var,
            "bitrate": self.enc_bitrate_var, "multipass": self.enc_multipass_var,
            "subme": self.enc_mobi_subme_var, "skip": self.enc_mobi_skip_var,
            "intra": self.enc_mobi_intra_var,
            "offset": self.enc_offset_var, "normalize": self.enc_normalize_var,
            "stereo": self.enc_stereo_var, "max_size_mb": self.enc_maxsize_var,
            "hdr": self.enc_hdr_var, "jobs": self.enc_jobs_var,
            "super": self.enc_super_var, "lang": self.enc_lang_var,
            "lang2": self.enc_lang2_var,
            "min_qp": self.enc_minqp_var, "max_qp": self.enc_maxqp_var,
            "i_boost": self.enc_iboost_var, "i_threshold": self.enc_ithresh_var,
            "buffer": self.enc_bufsize_var, "me": self.enc_me_var,
            "dct8": self.enc_dct8_var, "block": self.enc_block_var,
        }
        # Keys a preset may carry. Paths and per-file choices stay out of it.
        self.preset_keys = [k for k in self.persist_vars if k not in ("outdir",)]

        self.enc_input_var.trace_add("write", lambda *a: self.schedule_source_probe())
        for var in (self.enc_maxsize_var, self.enc_bitrate_var, self.enc_audio_var,
                    self.enc_hq_var, self.enc_quant_var):
            var.trace_add("write", lambda *a: self.update_size_estimate())
        self.enc_audio_var.trace_add("write", lambda *a: self.refresh_extra_controls())

        # Rows hidden by format are tracked here, not in enc_conditional_widgets,
        # because some depend on the chosen audio codec as well as the format.
        self.refresh_super_state()

    # ----- visibility ----------------------------------------------------

    def refresh_extra_controls(self):
        if not hasattr(self, "enc_src_frame"):
            return
        fmt = self.current_format()
        audio_only = fmt in AUDIO_ONLY_FORMATS
        has_audio = (
            fmt not in self.NO_AUDIO_FORMATS
            and self.enc_audio_var.get() != "none"
            and self.audio_options.get(fmt, ["none"]) != ["none"]
        )

        def show(widgets, on):
            for w in widgets:
                w.grid() if on else w.grid_remove()

        show((self.enc_track_label, self.enc_track_cb, self.enc_offset_label,
              self.enc_offset_entry, self.enc_offset_hint, self.enc_normalize_chk), has_audio)
        show((self.enc_stereo_label, self.enc_stereo_cb, self.enc_stereo_hint),
             fmt in self.STEREO_FORMATS)
        show((self.enc_maxsize_label, self.enc_maxsize_entry, self.enc_fat32_btn,
              self.enc_est_label), fmt in self.MOBI_SIZE_FORMATS)
        show((self.enc_hdr_label, self.enc_hdr_cb, self.enc_hdr_hint), not audio_only)
        show((self.enc_jobs_label, self.enc_jobs_entry, self.enc_jobs_hint),
             fmt in self.JOBS_FORMATS)
        out_any = (fmt in self.MOBI_SIZE_FORMATS) or (not audio_only) or (fmt in self.JOBS_FORMATS)
        self.enc_out_frame.grid() if out_any else self.enc_out_frame.grid_remove()
        self.enc_super_frame.grid() if fmt in self.SUPER_FORMATS else self.enc_super_frame.grid_remove()
        for w in self.enc_ffargs_widgets:
            w.grid()
        self.apply_level_gates()
        self.update_size_estimate()

    def refresh_super_state(self):
        on = self.enc_super_var.get()
        state = "normal" if on else "disabled"
        for w in (self.enc_audio2_entry, self.enc_audio2_btn, self.enc_lang_entry,
                  self.enc_lang2_entry, self.enc_poster_entry, self.enc_poster_btn):
            w.config(state=state)
        self.enc_srt_list.set_enabled(on)

    # ----- source probing ---------------------------------------------------

    def schedule_source_probe(self):
        if self._src_job is not None:
            self.after_cancel(self._src_job)
        self._src_job = self.after(400, self._start_source_probe)

    def _start_source_probe(self):
        self._src_job = None
        path = self.enc_input_var.get().strip()
        if not (path and os.path.isfile(path)):
            self._apply_source(path, None)
            return
        self.enc_src_label.config(text="reading...")

        def work():
            info = None
            try:
                import encode as _enc

                info = mt.probe_source(_enc.FFPROBE, path, _enc.input_fmt(path))
            except Exception:
                info = None
            self.after(0, self._apply_source, path, info)

        threading.Thread(target=work, daemon=True).start()

    def _apply_source(self, path, info):
        if path != self.enc_input_var.get().strip():
            return  # the user has moved on
        self.src_info = info
        if info is None:
            self.enc_src_label.config(
                text="choose an input file" if not path else "could not read this file",
                foreground="grey",
            )
            self.enc_track_cb.config(values=["Default"])
            self.enc_track_var.set("Default")
        else:
            warn = ""
            if info["hdr"]:
                try:
                    import encode as _enc

                    if "zscale" not in mt.available_filters(_enc.FFENC):
                        warn = "  — this ffmpeg cannot tone-map HDR (no zscale)"
                except Exception:
                    pass
            self.enc_src_label.config(text=mt.describe_source(info) + warn, foreground="")
            tracks = ["Default"] + [mt.describe_audio_track(t) for t in info["audio"]]
            self.enc_track_cb.config(values=tracks)
            if self.enc_track_var.get() not in tracks:
                self.enc_track_var.set("Default")
        self.update_size_estimate()

    def selected_track(self):
        """0-based audio track chosen in the dropdown, or -1 for the default."""
        values = list(self.enc_track_cb.cget("values"))
        cur = self.enc_track_var.get()
        if cur in values and values.index(cur) > 0:
            return values.index(cur) - 1
        return -1

    def update_size_estimate(self):
        if not hasattr(self, "enc_est_label"):
            return
        text = ""
        dur = (self.src_info or {}).get("duration") if self.src_info else None
        limit = self.enc_maxsize_var.get().strip()
        try:
            limit_mb = float(limit) if limit else 0
        except ValueError:
            limit_mb = 0
        if limit_mb and dur:
            bps = mt.video_bps_for_budget(int(limit_mb * 1048576), dur, 0)
            text = (f"{mt.format_clock(dur)} of video in {limit_mb:g} MiB ≈ "
                    f"{mt.format_bps(bps)}b/s overall; re-encodes lower if it doesn't fit")
        elif limit_mb:
            text = f"output will be kept under {limit_mb:g} MiB"
        else:
            rate = self.enc_bitrate_var.get().strip().lower()
            m = re.fullmatch(r"(\d+(?:\.\d+)?)([km]?)", rate)
            if m and dur:
                bps = float(m.group(1)) * {"": 1, "k": 1000, "m": 1_000_000}[m.group(2)]
                text = f"≈ {bps * dur / 8 / 1048576:.1f} MiB of video at {rate}"
        self.enc_est_label.config(text=text)

    # ----- command flags ------------------------------------------------------

    def add_extra_encode_flags(self, cmd, fmt, audio):
        """Append the flags for the controls added by setup_extra_encode_controls."""
        has_audio = (
            fmt not in self.NO_AUDIO_FORMATS
            and audio != "none"
            and self.audio_options.get(fmt, ["none"]) != ["none"]
        )
        if has_audio:
            track = self.selected_track()
            if track >= 0:
                cmd.extend(["--audio-track", str(track)])
            off = self.enc_offset_var.get().strip()
            if off and off not in ("0", "+0", "-0"):
                try:
                    cmd.extend(["--audio-offset-ms", str(int(off))])
                except ValueError:
                    pass
            if self.enc_normalize_var.get():
                cmd.append("--normalize-audio")
        if fmt in self.STEREO_FORMATS and self.enc_stereo_var.get() != "auto":
            cmd.extend(["--stereo", self.enc_stereo_var.get()])
        if fmt in self.MOBI_SIZE_FORMATS:
            size = self.enc_maxsize_var.get().strip()
            if size:
                try:
                    if float(size) > 0:
                        cmd.extend(["--max-size-mb", size])
                except ValueError:
                    pass
        if fmt not in AUDIO_ONLY_FORMATS and self.enc_hdr_var.get() != "auto":
            cmd.extend(["--hdr-mode", self.enc_hdr_var.get()])
        if fmt in self.JOBS_FORMATS:
            jobs = self.enc_jobs_var.get().strip()
            if jobs and jobs != "0":
                cmd.extend(["--jobs", jobs])
        if fmt in self.SUPER_FORMATS and self.enc_super_var.get():
            cmd.append("--super")
            a2 = self.enc_audio2_var.get().strip()
            if a2:
                cmd.extend(["--audio2", a2])
            for key, flag in (("lang", "--lang"), ("lang2", "--lang2")):
                v = self.persist_vars[key].get().strip()
                if v:
                    cmd.extend([flag, v])
            for srt in self.enc_srt_list.get():
                cmd.extend(["--srt", srt])
            poster = self.enc_poster_var.get().strip()
            if poster:
                cmd.extend(["--poster", poster])
        if fmt in ("mo", "moflex", "moflex3d", "mods") and self.enc_adv_toggle_var.get():
            for var, flag in (
                (self.enc_minqp_var, "--min-qp"), (self.enc_maxqp_var, "--max-qp"),
                (self.enc_iboost_var, "--i-boost"), (self.enc_ithresh_var, "--i-threshold"),
                (self.enc_bufsize_var, "--buffer-size"),
            ):
                v = var.get().strip()
                if v:
                    cmd.extend([flag, v])
            if self.enc_me_var.get() != "default":
                cmd.extend(["--me", self.enc_me_var.get()])
            if self.enc_dct8_var.get() != "default":
                cmd.extend(["--8x8dct", "1" if self.enc_dct8_var.get() == "on" else "0"])
            if fmt in ("moflex", "moflex3d") and self.enc_block_var.get() != "auto":
                cmd.extend(["--mo-block", self.enc_block_var.get()])

    # ----- saved settings and presets ------------------------------------------

    def snapshot_settings(self, keys=None):
        out = {}
        for k in keys or self.persist_vars:
            try:
                out[k] = self.persist_vars[k].get()
            except tk.TclError:
                pass
        out["srt"] = self.enc_srt_list.get()
        out["audio2"] = self.enc_audio2_var.get()
        out["poster"] = self.enc_poster_var.get()
        return out

    def apply_settings(self, data):
        """Set the form from a dict of persist_vars keys; unknown keys are ignored."""
        if not isinstance(data, dict):
            return
        # Format first: changing it resets the audio choice and shows/hides rows.
        if "fmt" in data and data["fmt"] in self.formats_map:
            self.enc_fmt_var.set(data["fmt"])
            self.on_enc_format_change()
        for k, v in data.items():
            if k in ("fmt", "adv") or k not in self.persist_vars:
                continue
            var = self.persist_vars[k]
            try:
                if isinstance(var, tk.BooleanVar):
                    var.set(bool(v))
                else:
                    var.set(str(v))
            except tk.TclError:
                pass
        if data.get("adv") and self.level_index() < 2:
            self.enc_level_var.set("Advanced")  # the settings need that section
        if "srt" in data and isinstance(data["srt"], list):
            self.enc_srt_list.set([p for p in data["srt"] if isinstance(p, str)])
        for key, var in (("audio2", self.enc_audio2_var), ("poster", self.enc_poster_var)):
            if isinstance(data.get(key), str):
                var.set(data[key])
        self.on_toggle_advanced()
        self.refresh_super_state()
        self.refresh_extra_controls()

    def load_settings(self):
        store = mt.JsonStore("gui.json")
        data = store.load()
        # Remembered form state minus anything that points at a specific file
        # that may be gone.
        for k in ("audio2", "poster"):
            if k in data and not os.path.isfile(str(data[k])):
                data.pop(k)
        if isinstance(data.get("srt"), list):
            data["srt"] = [p for p in data["srt"] if os.path.isfile(str(p))]
        self.apply_settings(data)

    def save_settings(self):
        mt.JsonStore("gui.json").save(self.snapshot_settings())

    def on_close(self):
        if self.busy():
            if not messagebox.askyesno(
                "mobipeg", "A job is still running. Stop it and quit?"
            ):
                return
            self.stop_process()
        try:
            self.save_settings()
            self.queue_tab.save()
        finally:
            self.destroy()

    def rebuild_preset_menu(self):
        m = self.preset_menu
        m.delete(0, tk.END)
        for name in mt.BUILTIN_PRESETS:
            m.add_command(label=name, command=lambda n=name: self.apply_builtin_preset(n))
        users = self.preset_store.load()
        if users:
            m.add_separator()
            for name in sorted(users):
                m.add_command(label=name, command=lambda n=name: self.apply_user_preset(n))
        m.add_separator()
        m.add_command(label="Save current settings as preset...", command=self.save_user_preset)
        if users:
            sub = tk.Menu(m, tearoff=0)
            for name in sorted(users):
                sub.add_command(label=name, command=lambda n=name: self.delete_user_preset(n))
            m.add_cascade(label="Delete preset", menu=sub)

    def apply_builtin_preset(self, name):
        p = dict(mt.BUILTIN_PRESETS[name])
        data = {
            "hq": p["hq"], "quant": p["quant"], "subme": p["subme"], "skip": p["skip"],
            "bitrate": p["bitrate"], "multipass": p["multipass"],
            "max_size_mb": p["max_size_mb"],
        }
        # subme/skip/bitrate are only sent with the Advanced section on.
        data["adv"] = bool(p["bitrate"]) and not p["hq"]
        # Fields first, then hq, so a preset that is not HQ stays that way.
        self.apply_settings({k: v for k, v in data.items() if k != "hq"})
        self.enc_hq_var.set(bool(p["hq"]))
        self.set_status(f"Preset: {name}")

    def apply_user_preset(self, name):
        data = self.preset_store.load().get(name)
        if data:
            self.apply_settings(data)
            self.set_status(f"Preset: {name}")

    def save_user_preset(self):
        from tkinter import simpledialog

        name = simpledialog.askstring("Save preset", "Preset name:", parent=self)
        if not name or not name.strip():
            return
        name = name.strip()
        if name in mt.BUILTIN_PRESETS:
            messagebox.showwarning("Preset", "That name is used by a built-in preset.")
            return
        users = self.preset_store.load()
        snap = self.snapshot_settings(self.preset_keys)
        # A preset describes encoder settings, not which files were chosen.
        for k in ("srt", "audio2", "poster"):
            snap.pop(k, None)
        users[name] = snap
        if self.preset_store.save(users):
            self.set_status(f"Saved preset: {name}")
        else:
            messagebox.showerror("Preset", "Could not write the preset file.")

    def delete_user_preset(self, name):
        users = self.preset_store.load()
        if name in users and messagebox.askyesno("Delete preset", f'Delete "{name}"?'):
            users.pop(name)
            self.preset_store.save(users)

    # ------------------------------------------------------------------
    # Running processes: progress, Stop, follow-up buttons
    # ------------------------------------------------------------------

    def build_status_bar(self, parent):
        bar = ttk.Frame(parent)
        bar.pack(fill=tk.X, pady=(8, 0))
        bar.columnconfigure(1, weight=1)
        self.progress = ttk.Progressbar(bar, mode="determinate", maximum=100, length=160)
        self.progress.grid(row=0, column=0, sticky="w", padx=(0, 8))
        self.status_var = tk.StringVar(value="Ready")
        ttk.Label(bar, textvariable=self.status_var).grid(row=0, column=1, sticky="ew")
        self.after_btns = ttk.Frame(bar)
        self.after_btns.grid(row=0, column=2, sticky="e")
        self.stop_btn = ttk.Button(self.after_btns, text="■ Stop", command=self.stop_process)
        self.stop_btn.pack(side="left", padx=2)
        self.stop_btn.state(["disabled"])
        self.show_btn = ttk.Button(self.after_btns, text="Show output", command=self.show_last_output)
        self.inspect_out_btn = ttk.Button(self.after_btns, text="Inspect", command=self.inspect_last_output)
        self.cia_out_btn = ttk.Button(self.after_btns, text="Package CIA", command=self.cia_last_output)
        self._proc = None
        self._stopped = False
        self.last_output = ""
        self._pending_run = None

    def set_status(self, text):
        self.status_var.set(text)

    def busy(self):
        return self._proc is not None

    def _set_progress(self, frac, text=None):
        self.progress.config(mode="determinate")
        self.progress["value"] = max(0.0, min(frac, 1.0)) * 100
        if text:
            self.set_status(text)

    def stop_process(self):
        if self._proc is not None:
            self._stopped = True
            self.append_console("\nStopping...\n")
            mt.terminate_tree(self._proc)

    def script_cmd(self):
        return (
            [sys.executable, "--encode-script"]
            if getattr(sys, "frozen", False)
            else [sys.executable, ENCODE_SCRIPT]
        )

    def select_tab(self, frame):
        try:
            self.notebook.select(frame)
        except tk.TclError:
            pass

    def _hide_followups(self):
        for b in (self.show_btn, self.inspect_out_btn, self.cia_out_btn):
            b.pack_forget()

    def offer_followups(self, path):
        """After a successful encode: reveal / inspect / package buttons."""
        self._hide_followups()
        self.last_output = path
        if not path or not os.path.exists(path):
            return
        self.show_btn.pack(side="left", padx=2)
        if path.lower().endswith(".moflex"):
            self.inspect_out_btn.pack(side="left", padx=2)
            self.cia_out_btn.pack(side="left", padx=2)

    def show_last_output(self):
        path = self.last_output
        if not path or not os.path.exists(path):
            return
        try:
            if sys.platform == "darwin":
                subprocess.Popen(["open", "-R", path])
            elif os.name == "nt":
                subprocess.Popen(["explorer", "/select,", os.path.normpath(path)])
            else:
                subprocess.Popen(["xdg-open", os.path.dirname(path)])
        except OSError as e:
            messagebox.showerror("Show output", str(e))

    def inspect_last_output(self):
        self.inspect_tab.set_input(self.last_output)
        self.select_tab(self.inspect_tab)
        self.inspect_tab.run()

    def cia_last_output(self):
        self.cia_input_var.set(self.last_output)
        self.select_tab(self.cia_frame)

    def start_process(self, cmd, buttons=None, on_done=None, clear=True, title="",
                      duration=None, probe_input=None, separator=False):
        """Run `cmd` in the background, streaming its output to the console.

        buttons    widgets disabled while it runs
        on_done    callback(returncode, stopped) on the Tk thread
        duration   seconds of media, for the progress bar (or probe_input to
                   have it read from a file once the job starts)
        """
        if self._proc is not None:
            messagebox.showinfo("mobipeg", "Another job is already running.")
            return False
        buttons = list(buttons or [])
        for b in buttons:
            b.config(state="disabled")
        self.stop_btn.state(["!disabled"])
        self._hide_followups()
        self._stopped = False
        self._set_progress(0.0, title or "Working...")
        self.console.config(state="normal")
        if clear:
            self.console.delete(1.0, tk.END)
        elif separator:
            self.console.insert(tk.END, "\n" + "-" * 60 + "\n")
        self.console.config(state="disabled")
        self.append_console(f"$ {shlex.join(cmd)}\n\n")

        def run_thread():
            code = -1
            total = duration
            # --windowed builds have no console, so on Linux/Windows/macOS an
            # uncaught exception here (e.g. a bundled binary that lost its
            # executable bit, or the frozen re-exec failing) would otherwise
            # vanish silently: the thread dies, the Run button never
            # re-enables, and the user sees the GUI simply do nothing.
            try:
                if total is None and probe_input:
                    try:
                        import encode as _enc

                        total = _enc.probe_duration(probe_input)
                    except Exception:
                        total = None
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
                    **mt.popen_kwargs_new_group(),
                )
                self._proc = process
                last = 0.0
                for line in process.stdout:
                    self.after(0, self.append_console, line)
                    sec = mt.parse_progress_seconds(line)
                    if sec is not None and total:
                        frac = min(sec / total, 1.0)
                        if abs(frac - last) >= 0.005 or frac in (0.0, 1.0):
                            last = frac
                            self.after(
                                0, self._set_progress, frac,
                                f"{(title + '  ') if title else ''}{frac * 100:.0f}%  "
                                f"({mt.format_clock(sec)} / {mt.format_clock(total)})",
                            )
                    elif line.startswith(">>"):
                        note = line[2:].strip()
                        self.after(
                            0, self.set_status, f"{(title + '  ') if title else ''}{note}"[:140]
                        )

                process.wait()
                code = process.returncode
                self.after(
                    0,
                    self.append_console,
                    f"\nProcess finished with exit code {process.returncode}\n",
                )
            except Exception as e:
                self.after(0, self.append_console, f"\nFailed to run command: {e}\n")
            finally:
                self.after(0, finish, code)

        def finish(code):
            stopped = self._stopped
            self._proc = None
            self._stopped = False
            for b in buttons:
                b.config(state="normal")
            self.stop_btn.state(["disabled"])
            if stopped:
                self.set_status("Stopped")
            elif code == 0:
                self._set_progress(1.0, "Done")
            else:
                self.set_status(f"Failed (exit code {code})")
            if on_done:
                on_done(code, stopped)

        threading.Thread(target=run_thread, daemon=True).start()
        return True

    # ------------------------------------------------------------------
    # Command building shared by Run, Copy command and the queue
    # ------------------------------------------------------------------

    def current_format(self):
        return self.formats_map.get(self.enc_fmt_var.get())

    def derived_output_for(self, inp):
        """Default output path for `inp` with the current format/outdir, or None
        when the format cannot be queued (it needs a second input)."""
        fmt = self.current_format()
        if fmt in ("3ds_camera3d",):
            return None
        ext = ENCODE_EXTENSIONS.get(fmt, fmt)
        outdir = self.enc_outdir_var.get().strip() or os.path.dirname(inp)
        return os.path.join(outdir, os.path.splitext(os.path.basename(inp))[0] + "." + ext)

    def copy_command(self):
        inp1 = self.enc_input_var.get().strip()
        if not inp1:
            messagebox.showwarning("Warning", "Please select an input file.")
            return
        cmd = self.build_encode_cmd(
            inp1, self.enc_input2_var.get(), self.enc_output_var.get().strip()
        )
        if cmd:
            self.clipboard_clear()
            self.clipboard_append(shlex.join(cmd))
            self.set_status("Command copied to the clipboard")

    def add_current_to_queue(self):
        self.queue_tab.add_current()
        self.select_tab(self.queue_tab)

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
        self.refresh_extra_controls()

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
        """Run `cmd` with `btn` disabled meanwhile (Decode, Play, CIA, Encode)."""
        ctx, self._pending_run = self._pending_run, None
        ctx = ctx or {}
        output = ctx.get("output", "")

        def done(code, stopped):
            if code == 0 and not stopped and output:
                self.offer_followups(output)

        self.start_process(
            cmd,
            buttons=[btn],
            on_done=done,
            title=ctx.get("title", ""),
            duration=ctx.get("duration"),
        )

    def run_encoding(self):
        if not getattr(sys, "frozen", False) and not os.path.exists(ENCODE_SCRIPT):
            messagebox.showerror(
                "Error", f"Could not find encode script at:\n{ENCODE_SCRIPT}"
            )
            return

        inp1 = self.enc_input_var.get()
        if not inp1:
            messagebox.showwarning("Warning", "Please select an input file.")
            return
        outfile = self.enc_output_var.get().strip()

        cmd = self.build_encode_cmd(inp1, self.enc_input2_var.get(), outfile)
        if cmd is None:
            return
        fmt = self.current_format()
        # The container extension comes from the format, not from the field.
        shown = outfile or self.derived_output_for(inp1) or ""
        if shown:
            shown = os.path.splitext(shown)[0] + "." + ENCODE_EXTENSIONS.get(fmt, fmt)
        duration = (self.src_info or {}).get("duration") if self.src_info else None
        self._pending_run = {
            "output": shown,
            "duration": duration,
            "title": os.path.basename(inp1),
        }
        self.execute_cmd(cmd, self.enc_run_btn)

    def build_encode_cmd(self, inp1, inp2, outfile):
        """The encode.py command line for the Encode tab's current settings."""
        fmt = self.formats_map.get(self.enc_fmt_var.get())
        audio = self.enc_audio_var.get()
        scale = self.enc_scale_var.get()
        outdir = self.enc_outdir_var.get()

        cmd = self.script_cmd()
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
            "bink",
            "bink2",
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
            "bink",
            "bink2",
        ):
            fps = self.enc_fps_var.get().strip()
            if fps:
                cmd.extend(["--fps", fps])
        if fmt in (
            {
                "vx",
                "mods",
                "ty",
                "thp",
                "rvid",
                "dpg",
                "wii_photo",
                "factor5",
                "mivf",
                "bink",
                "bink2",
            }
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

        self.add_extra_encode_flags(cmd, fmt, audio)
        self.add_ffargs(cmd, self.enc_ffargs_var)
        return cmd

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

        more = CollapsibleSection(self.cia_frame, "More options")
        more.grid(row=9, column=0, columnspan=3, sticky="ew", padx=5)
        more.body.columnconfigure(1, weight=1)
        self.cia_long_title_var = tk.StringVar()
        self.cia_unique_id_var = tk.StringVar()
        self.cia_product_code_var = tk.StringVar(value="VDIJ")
        for r, (text, var, hint) in enumerate(
            (
                ("Long title:", self.cia_long_title_var,
                 "title bar of the video applet; blank = the title above"),
                ("Title ID (hex):", self.cia_unique_id_var,
                 "unique part, C0000-EFFFF; blank = random"),
                ("Product code:", self.cia_product_code_var,
                 "four letters, default VDIJ"),
            )
        ):
            ttk.Label(more.body, text=text).grid(row=r, column=0, sticky="e", padx=5, pady=2)
            ttk.Entry(more.body, textvariable=var).grid(row=r, column=1, sticky="ew", padx=5, pady=2)
            ttk.Label(more.body, text=hint, foreground="grey").grid(
                row=r, column=2, sticky="w", padx=5
            )

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
        long_title = self.cia_long_title_var.get().strip()
        if long_title:
            cmd.extend(["--cia-long-title", long_title])
        publisher = self.cia_publisher_var.get().strip()
        if publisher:
            cmd.extend(["--cia-publisher", publisher])
        unique_id = self.cia_unique_id_var.get().strip()
        if unique_id:
            cmd.extend(["--cia-unique-id", unique_id])
        code = self.cia_product_code_var.get().strip()
        if code and code != "VDIJ":
            cmd.extend(["--cia-product-code", code])
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
