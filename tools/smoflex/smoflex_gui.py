#!/usr/bin/env python3
"""SUPER MOFLEX Builder — a small desktop front end for smoflex_build.

Runs anywhere Python does (macOS, Windows, Linux): Tkinter ships with Python, and the only
external requirement is ffmpeg. Pick the source video and the encoded .moflex, press Build.

    python3 smoflex_gui.py
"""
import json
import os
import queue
import subprocess
import sys
import tempfile
import threading
import traceback
import webbrowser

import tkinter as tk
from tkinter import filedialog, messagebox, ttk

import smoflex_build as sb

APP = 'SUPER MOFLEX Builder'
CONFIG = os.path.join(os.path.expanduser('~'), '.smoflex_gui.json')
VIDEO_TYPES = [('Video files', '*.mkv *.mp4 *.m4v *.mov *.avi *.ts'), ('All files', '*.*')]
MOFLEX_TYPES = [('Moflex video', '*.moflex'), ('All files', '*.*')]
SRT_TYPES = [('Subtitles', '*.srt *.ass *.ssa *.vtt'), ('All files', '*.*')]
LANGS = ['ENG', 'JPN', 'FRE', 'GER', 'SPA', 'CAS', 'ITA', 'POR', 'RUS', 'KOR',
         'CHI', 'TUR', 'POL', 'GRE', 'FIN', 'TGL', 'DUT', 'SWE']


def load_config():
    try:
        with open(CONFIG) as f:
            return json.load(f)
    except Exception:
        return {}


def save_config(cfg):
    try:
        with open(CONFIG, 'w') as f:
            json.dump(cfg, f, indent=1)
    except Exception:
        pass                      # a read-only home directory must not break the app


class App(ttk.Frame):
    def __init__(self, root):
        super().__init__(root, padding=10)
        self.root = root
        self.cfg = load_config()
        self.q = queue.Queue()            # worker -> UI log lines
        self.cancel = None                # threading.Event while a build runs
        self.worker = None
        self.srts = []                    # extra subtitle files
        self.audio_tracks = []            # [(stream index, label)] from the source probe

        self.grid(sticky='nsew')
        root.columnconfigure(0, weight=1)
        root.rowconfigure(0, weight=1)
        self.columnconfigure(0, weight=1)
        self.rowconfigure(0, weight=1)

        # The form is taller than a laptop screen once every field is on it, so it lives in a
        # scrolling canvas -- while Build/Cancel and the log stay pinned below, where they can
        # never be pushed off the bottom.
        self.canvas = tk.Canvas(self, highlightthickness=0)
        self.canvas.grid(row=0, column=0, sticky='nsew')
        vs = ttk.Scrollbar(self, orient='vertical', command=self.canvas.yview)
        vs.grid(row=0, column=1, sticky='ns')
        self.canvas.configure(yscrollcommand=vs.set)
        self.form = ttk.Frame(self.canvas)
        self.form.columnconfigure(1, weight=1)
        self.canvas.create_window((0, 0), window=self.form, anchor='nw', tags='form')
        self.form.bind('<Configure>',
                       lambda e: self.canvas.configure(scrollregion=self.canvas.bbox('all')))
        self.canvas.bind('<Configure>', lambda e: self.canvas.itemconfigure('form', width=e.width))
        for seq in ('<MouseWheel>', '<Button-4>', '<Button-5>'):
            self.canvas.bind_all(seq, self._on_wheel)

        if self.cfg.get('ffmpeg'):
            sb.set_tools(self.cfg['ffmpeg'])
        else:
            found, _ = sb.find_tools()
            if found:
                sb.set_tools(found)

        self.banner = None
        self._build_ui()
        self._poll_log()
        if not sb.tools_ready():
            self.log('ffmpeg was not found — nothing can be built until it is installed.')
            self._ffmpeg_banner()
            self.after(400, self._ffmpeg_help)      # once the window is actually on screen

    # ---- layout ---------------------------------------------------------------------------

    def _build_ui(self):
        r = 0
        self.v_video = tk.StringVar()
        self.v_moflex = tk.StringVar()
        self.v_outdir = tk.StringVar(value=self.cfg.get('outdir', ''))
        self.v_title = tk.StringVar()
        self.v_year = tk.StringVar()
        self.v_token = tk.StringVar(value=self.cfg.get('tmdb', ''))
        self.v_norm = tk.BooleanVar(value=True)
        self.v_offset = tk.StringVar(value='0')
        self.v_suboffset = tk.StringVar(value='0')
        self.v_eptitle = tk.StringVar()
        self.v_date = tk.StringVar()
        self.v_genres = tk.StringVar()
        self.v_runtime = tk.StringVar()
        self.v_cat = tk.StringVar(value='Movies')
        self.v_poster = tk.StringVar()
        self.v_kind = tk.StringVar(value='Movie')
        self.v_srtcode = tk.StringVar()
        self.v_skipaudio = tk.BooleanVar(value=False)
        self.v_srcsubs = tk.BooleanVar(value=True)
        self.v_lang_in = tk.StringVar(value='ENG')
        self.v_lang_alt = tk.StringVar(value='JPN')
        self.v_season = tk.StringVar()
        self.v_episode = tk.StringVar()
        self.v_3d = tk.StringVar(value='Auto')
        self.v_fmt = tk.StringVar(value='converted')
        self.v_status = tk.StringVar(value='Ready.')
        self.v_ain = tk.StringVar()
        self.v_aalt = tk.StringVar()

        r = self._file_row(r, 'Source video', self.v_video, self._pick_video,
                           'The original MKV/MP4 — its audio tracks and subtitles are the source')
        r = self._file_row(r, 'Encoded .moflex', self.v_moflex, self._pick_moflex,
                           'Your mobiclip encode of the same title (video comes from here)')
        r = self._file_row(r, 'Output folder', self.v_outdir, self._pick_outdir,
                           'Where the finished file lands; the name is built from the metadata')

        # audio track choice
        af = ttk.LabelFrame(self.form, text='Audio', padding=6)
        af.grid(row=r, column=0, columnspan=3, sticky='ew', pady=(8, 2))
        af.columnconfigure(1, weight=1)
        af.columnconfigure(3, weight=1)
        ttk.Label(af, text='In-band (all players):').grid(row=0, column=0, sticky='w')
        self.cb_ain = ttk.Combobox(af, textvariable=self.v_ain, state='readonly', width=24)
        self.cb_ain.grid(row=0, column=1, sticky='ew', padx=4)
        ttk.Label(af, text='Second track:').grid(row=0, column=2, sticky='w', padx=(10, 0))
        self.cb_aalt = ttk.Combobox(af, textvariable=self.v_aalt, state='readonly', width=24)
        self.cb_aalt.grid(row=0, column=3, sticky='ew', padx=4)
        # a rip with no language tags probes as UND; these are the labels the player shows on
        # its audio button, so they are editable rather than taken on faith from the container
        ttk.Label(af, text='Label them:').grid(row=1, column=0, sticky='w', pady=(4, 0))
        lf = ttk.Frame(af)
        lf.grid(row=1, column=1, columnspan=3, sticky='w', padx=4, pady=(4, 0))
        ttk.Combobox(lf, textvariable=self.v_lang_in, values=LANGS, width=6).pack(side='left')
        ttk.Label(lf, text='and').pack(side='left', padx=6)
        ttk.Combobox(lf, textvariable=self.v_lang_alt, values=LANGS, width=6).pack(side='left')
        ttk.Label(lf, text='— three-letter codes, shown on the audio button in the player',
                  foreground='#666').pack(side='left', padx=(8, 0))
        ttk.Checkbutton(af, text='Make it as loud as the 3DS speakers need '
                                 '(−16 LUFS, peaks limited so nothing clips)',
                        variable=self.v_norm).grid(row=2, column=0, columnspan=4, sticky='w',
                                                   pady=(6, 0))
        ttk.Checkbutton(af, text='Keep the audio already in the .moflex — subtitles and metadata '
                                 'only (much faster; no source video needed)',
                        variable=self.v_skipaudio, command=self._skip_audio_changed
                        ).grid(row=4, column=0, columnspan=4, sticky='w', pady=(6, 0))
        ttk.Label(af, text='Sync offset:').grid(row=3, column=0, sticky='w', pady=(6, 0))
        off = ttk.Frame(af)
        off.grid(row=3, column=1, columnspan=3, sticky='w', pady=(6, 0))
        ttk.Entry(off, textvariable=self.v_offset, width=8).pack(side='left', padx=(4, 4))
        ttk.Label(off, text='seconds — only when the encode came from a different master than '
                            'this source. Positive if the encode starts earlier (a leader the '
                            'source lacks). Moves the audio and the source\'s own subtitles; '
                            'added .srt files are left alone.', foreground='#666', wraplength=430,
                  justify='left').pack(side='left')
        # The row above deliberately leaves added .srt files alone, so downloaded subtitles that
        # are simply mistimed need their own control.
        ttk.Label(af, text='Subtitle offset:').grid(row=5, column=0, sticky='w', pady=(6, 0))
        soff = ttk.Frame(af)
        soff.grid(row=5, column=1, columnspan=3, sticky='w', pady=(6, 0))
        ttk.Entry(soff, textvariable=self.v_suboffset, width=8).pack(side='left', padx=(4, 4))
        ttk.Label(soff, text='seconds — shifts the .srt files YOU added, and nothing else. Use the '
                             'figure your player needed: if VLC wanted a subtitle delay of -1.0, '
                             'put -1.0 here. Positive shows them later.',
                  foreground='#666', wraplength=430, justify='left').pack(side='left')
        r += 1

        # subtitles
        sf = ttk.LabelFrame(self.form, text='Extra subtitles (text .srt — image subs cannot be used)',
                            padding=6)
        sf.grid(row=r, column=0, columnspan=3, sticky='ew', pady=2)
        sf.columnconfigure(0, weight=1)
        self.lst = tk.Listbox(sf, height=6, activestyle='none', exportselection=False,
                              font=('Menlo' if sys.platform == 'darwin' else 'Consolas', 11))
        self.lst.grid(row=0, column=0, sticky='ew')
        self.lst.bind('<<ListboxSelect>>', self._srt_selected)
        sb_side = ttk.Frame(sf)
        sb_side.grid(row=0, column=1, sticky='n', padx=4)
        ttk.Button(sb_side, text='Add…', width=9, command=self._add_srt).pack(fill='x')
        ttk.Button(sb_side, text='Remove', width=9, command=self._del_srt).pack(fill='x', pady=(3, 8))
        code = ttk.Frame(sb_side)
        code.pack(fill='x')
        ttk.Entry(code, textvariable=self.v_srtcode, width=5).pack(side='left')
        ttk.Button(code, text='Set', width=4, command=self._set_srt_code).pack(side='left', padx=(3, 0))
        mv = ttk.Frame(sb_side)
        mv.pack(fill='x', pady=(8, 0))
        ttk.Button(mv, text='↑', width=4, command=lambda: self._move_srt(-1)).pack(side='left')
        ttk.Button(mv, text='↓', width=4, command=lambda: self._move_srt(1)).pack(side='left', padx=(3, 0))
        ttk.Checkbutton(sf, text="Also include the subtitle tracks inside the source video "
                                    "(text tracks only — picture subtitles cannot be used)",
                        variable=self.v_srcsubs).grid(row=1, column=0, columnspan=2, sticky='w',
                                                      pady=(4, 0))
        ttk.Label(sf, text='The language code is what the player shows — pick a row, type a '
                           '3-letter code (ENG, JPN, FRE…) and press Set. Order matters: the top '
                           'one is the default track.', foreground='#666', wraplength=560,
                  justify='left').grid(row=2, column=0, columnspan=2, sticky='w', pady=(4, 0))
        r += 1

        # metadata / options
        of = ttk.LabelFrame(self.form, text='Metadata', padding=6)
        of.grid(row=r, column=0, columnspan=3, sticky='ew', pady=2)
        of.columnconfigure(1, weight=1)
        kr = ttk.Frame(of)
        kr.grid(row=0, column=0, columnspan=4, sticky='w', pady=(0, 4))
        ttk.Label(kr, text='This is a:').pack(side='left')
        for k in ('Movie', 'TV episode', 'Short', 'Music video'):
            ttk.Radiobutton(kr, text=k, value=k, variable=self.v_kind,
                            command=self._kind_changed).pack(side='left', padx=(6, 0))
        self.lbl_kind = ttk.Label(kr, text='', foreground='#666')
        self.lbl_kind.pack(side='left', padx=(10, 0))

        ttk.Label(of, text='Title:').grid(row=1, column=0, sticky='w')
        ttk.Entry(of, textvariable=self.v_title).grid(row=1, column=1, sticky='ew', padx=4)
        ttk.Label(of, text='Year:').grid(row=1, column=2, sticky='w', padx=(8, 0))
        ttk.Entry(of, textvariable=self.v_year, width=6).grid(row=1, column=3, sticky='w', padx=4)

        # season / episode / episode title -- only meaningful for a TV episode, so the whole row
        # is removed from the layout for anything else
        self.tvrow = ttk.Frame(of)
        self.tvrow.grid(row=2, column=0, columnspan=4, sticky='ew', pady=(4, 0))
        ttk.Label(self.tvrow, text='Season:').pack(side='left')
        ttk.Entry(self.tvrow, textvariable=self.v_season, width=4).pack(side='left', padx=(4, 8))
        ttk.Label(self.tvrow, text='Episode:').pack(side='left')
        ttk.Entry(self.tvrow, textvariable=self.v_episode, width=4).pack(side='left', padx=(4, 8))
        ttk.Label(self.tvrow, text='Episode title:').pack(side='left')
        ttk.Entry(self.tvrow, textvariable=self.v_eptitle).pack(side='left', fill='x', expand=True,
                                                                padx=(4, 0))

        ttk.Label(of, text='Genres:').grid(row=3, column=0, sticky='w', pady=(4, 0))
        ttk.Entry(of, textvariable=self.v_genres).grid(row=3, column=1, sticky='ew', padx=4,
                                                       pady=(4, 0))
        ttk.Label(of, text='Runtime (min):').grid(row=3, column=2, sticky='w', padx=(8, 0),
                                                  pady=(4, 0))
        ttk.Entry(of, textvariable=self.v_runtime, width=6).grid(row=3, column=3, sticky='w',
                                                                 padx=4, pady=(4, 0))

        # The kind above decides this; it is shown (and editable) only because a library can
        # have shelves of its own -- "Anime", "Concerts" -- that no kind maps to.
        ttk.Label(of, text='Library shelf:').grid(row=4, column=0, sticky='w', pady=(4, 0))
        ttk.Combobox(of, textvariable=self.v_cat,
                     values=['Movies', 'TV Shows', 'Shorts', 'Music Videos', 'Anime',
                             'Concerts', 'Uncategorized'],
                     width=14).grid(row=4, column=1, sticky='w', padx=4, pady=(4, 0))
        ttk.Label(of, text='Aired/released:').grid(row=4, column=2, sticky='w', padx=(8, 0),
                                                   pady=(4, 0))
        ttk.Entry(of, textvariable=self.v_date, width=12).grid(row=4, column=3, sticky='w', padx=4,
                                                               pady=(4, 0))

        ttk.Label(of, text='3D:').grid(row=5, column=0, sticky='w', pady=(4, 0))
        f3 = ttk.Frame(of)
        f3.grid(row=5, column=1, sticky='w', padx=4, pady=(4, 0))
        ttk.Combobox(f3, textvariable=self.v_3d, values=['Auto', '2D', '3D'], state='readonly',
                     width=5).pack(side='left')
        ttk.Combobox(f3, textvariable=self.v_fmt, values=['converted', 'native'], state='readonly',
                     width=10).pack(side='left', padx=(4, 0))

        ttk.Label(of, text='Description:').grid(row=6, column=0, sticky='nw', pady=(4, 0))
        dfr = ttk.Frame(of)
        dfr.grid(row=6, column=1, columnspan=3, sticky='ew', padx=4, pady=(4, 0))
        dfr.columnconfigure(0, weight=1)
        self.txt_desc = tk.Text(dfr, height=3, wrap='word')
        self.txt_desc.grid(row=0, column=0, sticky='ew')
        ds = ttk.Scrollbar(dfr, command=self.txt_desc.yview)
        ds.grid(row=0, column=1, sticky='ns')
        self.txt_desc['yscrollcommand'] = ds.set

        ttk.Label(of, text='Poster:').grid(row=7, column=0, sticky='w', pady=(4, 0))
        ttk.Entry(of, textvariable=self.v_poster).grid(row=7, column=1, columnspan=2, sticky='ew',
                                                       padx=4, pady=(4, 0))
        pb = ttk.Frame(of)
        pb.grid(row=7, column=3, sticky='w', padx=4, pady=(4, 0))
        ttk.Button(pb, text='Browse…', width=9, command=self._pick_poster).pack(side='left')
        ttk.Button(pb, text='View', width=6, command=self._view_poster).pack(side='left', padx=(4, 0))
        # live thumbnail of whatever is in the field (ffmpeg is already required, so it can
        # convert a JPEG to something Tk can display)
        self.poster_img = None
        self.lbl_poster = ttk.Label(of)
        self.lbl_poster.grid(row=7, column=4, rowspan=2, sticky='w', padx=(8, 0), pady=(4, 0))
        self.v_poster.trace_add('write', lambda *a: self._poster_preview())

        ttk.Label(of, text='TMDB token:').grid(row=8, column=0, sticky='w', pady=(6, 0))
        ttk.Entry(of, textvariable=self.v_token, show='•').grid(row=8, column=1, columnspan=2,
                                                                sticky='ew', padx=4, pady=(6, 0))
        tb = ttk.Frame(of)
        tb.grid(row=8, column=3, sticky='w', padx=4, pady=(6, 0))
        self.btn_look = ttk.Button(tb, text='Look up', width=8, command=self._lookup)
        self.btn_look.pack(side='left')
        ttk.Button(tb, text='Get token', width=9,
                   command=lambda: webbrowser.open('https://www.themoviedb.org/settings/api')
                   ).pack(side='left', padx=(4, 0))
        self.lbl_meta = ttk.Label(of, foreground='#666', wraplength=600, justify='left',
                                  text='Anything you type wins. "Look up" fills these in from '
                                       'TMDB so you can check them first; Build fetches them '
                                       'anyway for whatever you leave blank.')
        self.lbl_meta.grid(row=9, column=0, columnspan=4, sticky='w', pady=(4, 0))
        r += 1

        # actions
        bar = ttk.Frame(self)
        bar.grid(row=1, column=0, columnspan=2, sticky='ew', pady=(8, 4))
        bar.columnconfigure(2, weight=1)
        self.btn_build = ttk.Button(bar, text='Build SUPER MOFLEX', command=self._start)
        self.btn_build.grid(row=0, column=0)
        self.btn_cancel = ttk.Button(bar, text='Cancel', command=self._cancel, state='disabled')
        self.btn_cancel.grid(row=0, column=1, padx=6)
        ttk.Label(bar, textvariable=self.v_status).grid(row=0, column=2, sticky='w', padx=8)
        self.bar = ttk.Progressbar(bar, mode='indeterminate', length=120)
        self.bar.grid(row=0, column=3, sticky='e')
        r += 1

        # log
        lf = ttk.Frame(self)
        lf.grid(row=2, column=0, columnspan=2, sticky='nsew')
        lf.columnconfigure(0, weight=1)
        lf.rowconfigure(0, weight=1)
        self.txt = tk.Text(lf, height=8, wrap='word', state='disabled',
                           font=('Menlo' if sys.platform == 'darwin' else 'Consolas', 10))
        self.txt.grid(row=0, column=0, sticky='nsew')
        sc = ttk.Scrollbar(lf, command=self.txt.yview)
        sc.grid(row=0, column=1, sticky='ns')
        self.txt['yscrollcommand'] = sc.set

        # menu
        m = tk.Menu(self.root)
        tools = tk.Menu(m, tearoff=0)
        tools.add_command(label='Locate ffmpeg…', command=self._pick_ffmpeg)
        tools.add_command(label='Check a built file…', command=self._verify_file)
        tools.add_separator()
        tools.add_command(label='About', command=self._about)
        m.add_cascade(label='Tools', menu=tools)
        self.root.config(menu=m)
        self._kind_changed()          # start with the TV row hidden for the default kind

    def _file_row(self, r, label, var, cmd, hint):
        f = self.form
        ttk.Label(f, text=label + ':').grid(row=r, column=0, sticky='w', pady=2)
        ttk.Entry(f, textvariable=var).grid(row=r, column=1, sticky='ew', padx=4, pady=2)
        ttk.Button(f, text='Browse…', command=cmd).grid(row=r, column=2, pady=2)
        ttk.Label(f, text=hint, foreground='#666').grid(row=r + 1, column=1, columnspan=2,
                                                        sticky='w')
        return r + 2

    # ---- pickers --------------------------------------------------------------------------

    def _pick_video(self):
        p = filedialog.askopenfilename(title='Source video', filetypes=VIDEO_TYPES,
                                       initialdir=self.cfg.get('videodir', ''))
        if not p:
            return
        self.v_video.set(p)
        self.cfg['videodir'] = os.path.dirname(p)
        save_config(self.cfg)
        self._probe(p)

    def _pick_moflex(self):
        p = filedialog.askopenfilename(title='Encoded .moflex', filetypes=MOFLEX_TYPES,
                                       initialdir=self.cfg.get('moflexdir', ''))
        if p:
            self.v_moflex.set(p)
            self.cfg['moflexdir'] = os.path.dirname(p)
            if not self.v_outdir.get():
                self.v_outdir.set(os.path.join(os.path.dirname(p), 'super'))
            save_config(self.cfg)

    def _pick_outdir(self):
        p = filedialog.askdirectory(title='Output folder', initialdir=self.v_outdir.get() or '')
        if p:
            self.v_outdir.set(p)
            self.cfg['outdir'] = p
            save_config(self.cfg)

    def _pick_ffmpeg(self):
        p = filedialog.askopenfilename(title='Locate the ffmpeg binary')
        if p:
            sb.set_tools(p)
            self.cfg['ffmpeg'] = p
            save_config(self.cfg)
            ok = sb.tools_ready()
            self.log(f'ffmpeg: {p}' + ('' if ok else '  (ffprobe still missing — it normally '
                                                     'sits in the same folder)'))
            self._ffmpeg_banner()

    def _pick_poster(self):
        p = filedialog.askopenfilename(title='Poster image',
                                       filetypes=[('Images', '*.jpg *.jpeg *.png *.webp'),
                                                  ('All files', '*.*')])
        if p:
            self.v_poster.set(p)

    def _view_poster(self):
        """Open the poster full size in whatever the system uses for images."""
        p = self.v_poster.get().strip()
        if not p:
            return messagebox.showinfo(APP, 'No poster chosen yet. Browse for one, or press '
                                            'Look up to fetch the one TMDB has.')
        if not os.path.isfile(p):
            return messagebox.showwarning(APP, f'Not found:\n\n{p}')
        try:
            if sys.platform == 'darwin':
                subprocess.run(['open', p])
            elif sys.platform == 'win32':
                os.startfile(p)                      # noqa: S606 - the platform's own opener
            else:
                subprocess.run(['xdg-open', p])
        except Exception as ex:
            messagebox.showwarning(APP, f'Could not open it:\n\n{ex}')

    def _poster_preview(self):
        """Show a small thumbnail of the current poster (Tk cannot read JPEG, so ffmpeg makes a
        PNG first). Silently leaves the space blank if anything goes wrong."""
        p = self.v_poster.get().strip()
        self.lbl_poster.configure(image='')
        self.poster_img = None
        if not (p and os.path.isfile(p) and sb.tools_ready()):
            return
        png = os.path.join(tempfile.gettempdir(), 'smoflex_thumb.png')
        try:
            rc, _, _ = sb._run([sb.FFMPEG, '-y', '-v', 'error', '-i', p,
                                '-vf', 'scale=-1:96', '-frames:v', '1', png])
            if rc == 0 and os.path.exists(png):
                self.poster_img = tk.PhotoImage(file=png)   # keep the reference or Tk drops it
                self.lbl_poster.configure(image=self.poster_img)
        except Exception:
            pass

    # ---- ffmpeg presence -------------------------------------------------------------------

    def _ffmpeg_banner(self):
        """Show a standing warning while ffmpeg is missing; hide it once it is found."""
        if sb.tools_ready():
            if self.banner is not None:
                self.banner.destroy()
                self.banner = None
            return
        if self.banner is not None:
            return
        self.banner = tk.Frame(self, bg='#7a2a2a')
        self.banner.grid(row=99, column=0, columnspan=3, sticky='ew', pady=(6, 0))
        self.banner.columnconfigure(0, weight=1)
        tk.Label(self.banner, bg='#7a2a2a', fg='white', anchor='w', padx=8, pady=5,
                 text='ffmpeg was not found — builds cannot run without it.'
                 ).grid(row=0, column=0, sticky='ew')
        tk.Button(self.banner, text='How do I install it?', command=self._ffmpeg_help
                  ).grid(row=0, column=1, padx=4, pady=3)
        tk.Button(self.banner, text='Locate…', command=self._pick_ffmpeg
                  ).grid(row=0, column=2, padx=(0, 6), pady=3)

    def _ffmpeg_help(self):
        if sys.platform == 'darwin':
            body = ('macOS\n\n'
                    '1. Install Homebrew if you do not have it — paste this in Terminal:\n'
                    '     /bin/bash -c "$(curl -fsSL '
                    'https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"\n\n'
                    '2. Then:\n'
                    '     brew install ffmpeg\n\n'
                    'No Homebrew? Download a build from evermeet.cx/ffmpeg (you need BOTH '
                    'ffmpeg and ffprobe), put the two files in one folder, and use '
                    'Tools ▸ Locate ffmpeg… to point at ffmpeg.')
            url = 'https://evermeet.cx/ffmpeg/'
        elif sys.platform == 'win32':
            body = ('Windows\n\n'
                    'Easiest — in PowerShell:\n'
                    '     winget install Gyan.FFmpeg\n'
                    'then close and reopen this app.\n\n'
                    'By hand:\n'
                    '1. Download the "release full" zip from gyan.dev.\n'
                    '2. Unzip it.\n'
                    '3. Copy ffmpeg.exe and ffprobe.exe out of its bin folder into a folder '
                    'named "ffmpeg" next to this app — it looks there automatically.\n\n'
                    'Or put them anywhere and use Tools ▸ Locate ffmpeg….')
            url = 'https://www.gyan.dev/ffmpeg/builds/'
        else:
            body = ('Linux\n\n'
                    '     sudo apt install ffmpeg        (Debian/Ubuntu)\n'
                    '     sudo dnf install ffmpeg        (Fedora)\n'
                    '     sudo pacman -S ffmpeg          (Arch)\n\n'
                    'Then reopen this app.')
            url = 'https://ffmpeg.org/download.html'
        body += '\n\nffprobe is required as well as ffmpeg — every distribution ships both.'
        if messagebox.askyesno(APP, body + '\n\nOpen the download page now?'):
            webbrowser.open(url)

    # ---- subtitle list: each row is [path, 3-letter code], first row = default track ---------

    def _refresh_srts(self, select=None):
        self.lst.delete(0, 'end')
        for i, (p, c) in enumerate(self.srts):
            self.lst.insert('end', f'{c:<4} {os.path.basename(p)}'
                            + ('   (default)' if i == 0 else ''))
        if select is not None and 0 <= select < len(self.srts):
            self.lst.selection_set(select)
            self.lst.see(select)
            self.v_srtcode.set(self.srts[select][1])

    def _add_srt(self):
        for p in filedialog.askopenfilenames(title='Subtitle files', filetypes=SRT_TYPES,
                                             initialdir=self.cfg.get('srtdir', '')):
            if any(p == q for q, _ in self.srts):
                continue
            self.srts.append([p, sb.srt_lang(p) or 'SUB'])       # guess now, editable after
            self.cfg['srtdir'] = os.path.dirname(p)
        save_config(self.cfg)
        self._refresh_srts(len(self.srts) - 1)
        if any(c == 'SUB' for _, c in self.srts):
            self.log('Some subtitles have no language in their filename — select each one, type '
                     'its code and press Set.')

    def _del_srt(self):
        sel = list(self.lst.curselection())
        for i in reversed(sel):
            del self.srts[i]
        self._refresh_srts(sel[0] - 1 if sel and sel[0] else None)

    def _skip_audio_changed(self):
        """Grey out the audio controls when the encode's own audio is being kept."""
        state = 'disabled' if self.v_skipaudio.get() else 'readonly'
        self.cb_ain.configure(state=state)
        self.cb_aalt.configure(state=state)
        if self.v_skipaudio.get():
            self.log('Audio will be left exactly as it is in the .moflex; only the trailer is '
                     'rebuilt. A source video is optional now — add .srt files below.')

    def _srt_selected(self, _evt=None):
        sel = self.lst.curselection()
        if sel:
            self.v_srtcode.set(self.srts[sel[0]][1])

    def _set_srt_code(self):
        sel = self.lst.curselection()
        if not sel:
            return messagebox.showinfo(APP, 'Select a subtitle row first, then type its code.')
        c = self.v_srtcode.get().strip().upper()[:3]
        if not c.isalpha():
            return messagebox.showwarning(APP, 'A language code is 2-3 letters, like ENG or JPN.')
        self.srts[sel[0]][1] = c
        self._refresh_srts(sel[0])

    def _move_srt(self, delta):
        sel = self.lst.curselection()
        if not sel:
            return
        i = sel[0]
        j = i + delta
        if 0 <= j < len(self.srts):
            self.srts[i], self.srts[j] = self.srts[j], self.srts[i]
            self._refresh_srts(j)

    # ---- source probe ---------------------------------------------------------------------

    def _probe(self, path):
        if not sb.tools_ready():
            self.log('Cannot read the source until ffmpeg is available.')
            return
        try:
            info = sb.probe_source(path)
        except Exception as ex:
            self.log(f'Could not read {os.path.basename(path)}: {ex}')
            return
        self.audio_tracks = info['audio']
        labels = [f'{i}: {d}' for i, d in info['audio']]
        self.cb_ain['values'] = labels
        self.cb_aalt['values'] = ['(none)'] + labels
        # default: English in-band, the first other track in the trailer
        eng = next((k for k, (i, d) in enumerate(info['audio']) if d.startswith('ENG')), 0)
        self.v_ain.set(labels[eng] if labels else '')
        alt = next((k for k in range(len(labels)) if k != eng), None)
        self.v_aalt.set(labels[alt] if alt is not None else '(none)')
        def code_of(k):
            return info['audio'][k][1].split()[0] if 0 <= k < len(info['audio']) else ''
        ci, ca = code_of(eng), code_of(alt if alt is not None else -1)
        self.v_lang_in.set(ci if ci and ci != 'UND' else 'ENG')
        self.v_lang_alt.set(ca if ca and ca != 'UND' else ('JPN' if alt is not None else ''))
        if 'UND' in (ci, ca):
            self.log('This file has untagged audio (UND) — set the labels under the track '
                     'menus so the player shows the right languages.')
        t, y, se = sb.parse_name(path)
        if se:                        # an SxxEyy tag in the name -> preselect TV and fill it in
            self.v_kind.set('TV episode')
            self.v_season.set(str(se[0]))
            self.v_episode.set(str(se[1]))
            self.lbl_kind['text'] = '(detected from the filename)'
        else:
            self.lbl_kind['text'] = '(no SxxEyy in the name — set it here if that is wrong)'
        self._kind_changed()
        self.log(f'{os.path.basename(path)}: {len(info["audio"])} audio track(s), '
                 f'{len(info["subs"])} text subtitle track(s)'
                 + (f', {info["imagesubs"]} image subtitle track(s) that cannot be used'
                    if info['imagesubs'] else ''))
        self.log(f'Parsed as: {t!r} ({y or "year?"})'
                 + (f' S{se[0]:02d}E{se[1]:02d}' if se else ' — movie'))
        if info['imagesubs'] and not info['subs']:
            self.log('This disc rip carries picture subtitles only. Add text .srt files above if '
                     'you want subtitles in the finished file.')

    # ---- kind (movie / TV episode / short / music video) -----------------------------------

    KIND_SHELF = {'Movie': 'Movies', 'TV episode': 'TV Shows', 'Short': 'Shorts',
                  'Music video': 'Music Videos'}

    def _kind_changed(self):
        """Show the season/episode row only for TV, and steer lookup + naming to match."""
        k = self.v_kind.get()
        # the shelf follows the kind unless it has been pointed at something custom
        if self.v_cat.get() in ('', '(auto)') or self.v_cat.get() in self.KIND_SHELF.values():
            self.v_cat.set(self.KIND_SHELF.get(k, 'Movies'))
        if k == 'TV episode':
            self.tvrow.grid()
        else:
            self.tvrow.grid_remove()
        # Music videos are not in TMDB, so there is nothing to look up for them
        music = (k == 'Music video')
        self.btn_look['state'] = 'disabled' if music else 'normal'
        self.lbl_meta['text'] = (
            'Music videos: name the title "Artist - Song" — the library groups them by the part '
            'before the dash. TMDB has no music videos, so fill these in yourself.' if music else
            'Shorts are looked up as movies on TMDB.' if k == 'Short' else
            'The episode title goes into the filename, and the lookup fetches it along with the '
            'air date and synopsis for that episode.' if k == 'TV episode' else
            'Anything you type wins. "Look up" fills these in from TMDB so you can check them '
            'first; Build fetches them anyway for whatever you leave blank.')

    def _kind_se(self):
        """(season, episode) when the fields make sense for the chosen kind, else None."""
        if self.v_kind.get() != 'TV episode':
            return None
        s, e = self.v_season.get().strip(), self.v_episode.get().strip()
        return (int(s), int(e)) if s.isdigit() and e.isdigit() else None

    def _lookup(self):
        """Fetch the TMDB entry now and show it, so nothing is a surprise at build time."""
        token = self.v_token.get().strip()
        if not token:
            return messagebox.showinfo(APP, 'Paste a TMDB read token first — "Get token" opens '
                                            'the page (themoviedb.org ▸ Settings ▸ API).')
        name = self.v_video.get().strip() or self.v_moflex.get().strip()
        if not name:
            return messagebox.showinfo(APP, 'Choose the source video (or the .moflex) first — '
                                            'the title and year are read from its filename.')
        t, y, _ = sb.parse_name(name)
        se = self._kind_se()          # the radio buttons decide, not the filename
        title = self.v_title.get().strip() or t
        year = self.v_year.get().strip()
        year = int(year) if year.isdigit() else y
        self.cfg['tmdb'] = token
        save_config(self.cfg)
        self.btn_look['state'] = 'disabled'
        self.log(f'Looking up {title!r} ({year or "no year"})'
                 + (f' S{se[0]:02d}E{se[1]:02d}' if se else '') + ' …')

        def run():
            info = sb.fetch_meta(token, title, year, se, self.q.put)
            self.q.put(('lookup', info))
        threading.Thread(target=run, daemon=True).start()

    def _show_lookup(self, info):
        self.btn_look['state'] = 'normal'
        if not info:
            return self.log('Nothing came back — check the title, or clear the year and retry.')
        self.log(f"  title    {info.get('title','?')} ({info.get('year','?')})")
        if info.get('eptitle'):
            self.log(f"  episode  {info['eptitle']}"
                     + (f"   aired {info['date']}" if info.get('date') else ''))
        self.log(f"  genres   {', '.join(info.get('genres', [])) or '(none)'}")
        self.log(f"  runtime  {info.get('runtime', 0)} min")
        self.log(f"  poster   {'yes' if info.get('poster') else 'none found'}")
        desc = (info.get('desc') or info.get('showdesc') or '').strip()
        if desc:
            self.log(f"  {desc[:300]}{'…' if len(desc) > 300 else ''}")
        # drop it into the fields so you can edit before building; anything you already typed wins
        def fill(var, val):
            if val and not var.get().strip():
                var.set(str(val))
        fill(self.v_title, info.get('title'))
        fill(self.v_year, info.get('year'))
        fill(self.v_eptitle, info.get('eptitle'))
        fill(self.v_date, info.get('date'))
        fill(self.v_genres, ', '.join(info.get('genres', [])))
        fill(self.v_runtime, info.get('runtime'))
        if not self.txt_desc.get('1.0', 'end').strip():
            self.txt_desc.insert('1.0', (info.get('desc') or info.get('showdesc') or '').strip())
        # pull the artwork down now and show the path, rather than leaving it to build time.
        # It goes in the output folder (named after the title) so it sits with the finished
        # file instead of somewhere in the system temp directory.
        if info.get('poster') and not self.v_poster.get().strip():
            name = f"{info.get('title', 'poster')} ({info.get('year', '')})".strip()
            for bad, rep in (('/', ' - '), (':', ' -'), ('*', ''), ('?', ''), ('"', "'"),
                             ('<', ''), ('>', ''), ('|', ''), ('\\', '')):
                name = name.replace(bad, rep)
            dest_dir = self.v_outdir.get().strip() or tempfile.gettempdir()
            try:
                os.makedirs(dest_dir, exist_ok=True)
                dest = os.path.join(dest_dir, f'{name}.jpg')
                self.v_poster.set(sb.download_poster(info['poster'], dest))
                self.log(f'  poster   saved to {dest}')
            except Exception as ex:
                self.log(f'  poster   download failed ({ex}) — pick one by hand if you want art')
        self.log('Fields filled in — edit anything you like before building.')

    def _sel_index(self, var):
        """Stream index for a combobox row. None = 'decide automatically' (nothing chosen yet),
        -1 = the explicit '(none)' row; never conflate the two or a silent path would drop the
        second audio track."""
        s = var.get()
        if not s:
            return None
        if s == '(none)':
            return -1
        try:
            return int(s.split(':', 1)[0])
        except ValueError:
            return None

    # ---- build ----------------------------------------------------------------------------

    def _start(self):
        video, moflex = self.v_video.get().strip(), self.v_moflex.get().strip()
        skip_audio = self.v_skipaudio.get()
        if not (video and os.path.isfile(video)):
            if not skip_audio:
                return messagebox.showerror(APP, 'Choose the source video first.')
            video = None            # subtitles/metadata only: nothing is taken from a source
        if skip_audio and not self.srts and not self.v_token.get().strip():
            return messagebox.showerror(APP, 'Nothing to add: with the audio kept as-is, add '
                                             'subtitle files or fill in metadata first.')
        if not (moflex and os.path.isfile(moflex)):
            return messagebox.showerror(APP, 'Choose the encoded .moflex first.')
        if not sb.tools_ready():
            self._ffmpeg_help()
            return
        outdir = self.v_outdir.get().strip() or os.path.join(os.path.dirname(moflex), 'super')
        year = self.v_year.get().strip()
        try:
            offset = float(self.v_offset.get().strip() or 0)
        except ValueError:
            return messagebox.showerror(APP, 'Sync offset must be a number of seconds, e.g. 14.45')
        try:
            suboffset = float(self.v_suboffset.get().strip() or 0)
        except ValueError:
            return messagebox.showerror(APP, 'Subtitle offset must be a number of seconds, '
                                             'e.g. -1.0')
        rt = self.v_runtime.get().strip()
        cat = self.v_cat.get().strip()
        kind = self.v_kind.get()
        se = self._kind_se()
        if kind == 'TV episode' and se is None:
            return messagebox.showerror(APP, 'Fill in the season and episode numbers, or pick a '
                                             'different kind above.')
        if cat in ('', '(auto)'):     # empty box -> fall back to whatever the kind implies
            cat = self.KIND_SHELF.get(kind, 'Movies')
        if kind == 'Music video' and ' - ' not in self.v_title.get():
            if not messagebox.askyesno(APP, 'Music videos group by artist in the library, using '
                                            'the text before " - " in the title.\n\nThis title '
                                            'has no " - ", so it will not group under an artist.'
                                            '\n\nBuild anyway?'):
                return
        # everything validated -- only now touch the disk, so a refused build leaves nothing behind
        os.makedirs(outdir, exist_ok=True)
        self.cfg['outdir'] = outdir
        self.cfg['tmdb'] = self.v_token.get().strip()
        save_config(self.cfg)
        # anything typed here overrides TMDB; blank fields fall back to it (or to defaults)
        meta = {'title': self.v_title.get().strip(),
                'year': int(year) if year.isdigit() else 0,
                'eptitle': self.v_eptitle.get().strip(),
                'date': self.v_date.get().strip(),
                'genres': [g.strip() for g in self.v_genres.get().split(',') if g.strip()],
                'runtime': int(rt) if rt.isdigit() else 0,
                'category': cat,
                'desc': self.txt_desc.get('1.0', 'end').strip()}
        opts = dict(out_dir=outdir, extra_srts=[(c, p) for p, c in self.srts], boost=self.v_norm.get(),
                    offset=offset, srt_offset=suboffset,
                    is3d={'Auto': None, '2D': False, '3D': True}[self.v_3d.get()],
                    fmt=self.v_fmt.get(),
                    # TMDB has no music videos: a lookup there would only mismatch
                    tmdb_token=None if kind == 'Music video' else (self.v_token.get().strip() or None),
                    title=self.v_title.get().strip() or None,
                    year=int(year) if year.isdigit() else None,
                    poster=self.v_poster.get().strip() or None,
                    meta=meta, se=se if se else False,
                    skip_audio=skip_audio, use_source_subs=self.v_srcsubs.get(),
                    lang_in=self.v_lang_in.get().strip().upper() or None,
                    lang_alt=self.v_lang_alt.get().strip().upper() or None,
                    audio_in=self._sel_index(self.v_ain),
                    audio_alt=self._sel_index(self.v_aalt))

        self.cancel = threading.Event()
        self.btn_build['state'] = 'disabled'
        self.btn_cancel['state'] = 'normal'
        self.bar.start(12)
        self.v_status.set('Building…')
        self.log('─' * 60)

        def run():
            try:
                out = sb.build(video, moflex, log=self.q.put, cancel=self.cancel, **opts)
                self.q.put(('done', out))
            except sb.Cancelled:
                self.q.put(('cancelled', None))
            except Exception as ex:
                self.q.put(str(ex) if isinstance(ex, RuntimeError) else traceback.format_exc())
                self.q.put(('failed', str(ex)))

        self.worker = threading.Thread(target=run, daemon=True)
        self.worker.start()

    def _cancel(self):
        if self.cancel:
            self.cancel.set()
            self.v_status.set('Cancelling…')

    def _finish(self, state, payload):
        self.bar.stop()
        self.btn_build['state'] = 'normal'
        self.btn_cancel['state'] = 'disabled'
        self.cancel = None
        if state == 'done':
            self.v_status.set('Done.')
            self.log(f'Finished: {payload}')
            if messagebox.askyesno(APP, f'Built:\n\n{os.path.basename(payload)}\n\nShow the file?'):
                self._reveal(payload)
        elif state == 'cancelled':
            self.v_status.set('Cancelled.')
            self.log('Cancelled — no output was written.')
        else:
            self.v_status.set('Failed.')
            messagebox.showerror(APP, f'Build failed:\n\n{payload}')

    @staticmethod
    def _reveal(path):
        try:
            if sys.platform == 'darwin':
                subprocess.run(['open', '-R', path])
            elif sys.platform == 'win32':
                subprocess.run(['explorer', '/select,', os.path.normpath(path)])
            else:
                subprocess.run(['xdg-open', os.path.dirname(path)])
        except Exception:
            pass

    # ---- misc -----------------------------------------------------------------------------

    def _verify_file(self):
        p = filedialog.askopenfilename(title='Check a SUPER MOFLEX', filetypes=MOFLEX_TYPES)
        if not p:
            return
        try:
            secs = sb.verify(p)
            names = {'NFO0': 'library info', 'ART5': 'poster', 'LNG0': 'in-band language',
                     'SUB0': 'subtitle', 'SUB1': 'subtitle', 'AUD0': 'audio track',
                     'AUD1': 'audio track'}
            body = '\n'.join(f'  {names.get(k, k)}  ×{v}' for k, v in secs.items())
            messagebox.showinfo(APP, f'{os.path.basename(p)} is a valid SUPER MOFLEX:\n\n{body}')
        except Exception as ex:
            messagebox.showwarning(APP, f'{os.path.basename(p)}\n\n{ex}\n\n'
                                        'A plain moflex (no trailer) reports this too.')

    def _about(self):
        messagebox.showinfo(APP,
                            f'{APP}\n\nBuilds a self-contained SUPER MOFLEX: one audio track in '
                            'band so stock players keep working, plus a second language, every '
                            'subtitle, the library info and the poster in a trailer that only '
                            'players which know about it read.\n\nNeeds ffmpeg. Nothing else.')

    def _on_wheel(self, e):
        step = -1 if getattr(e, 'num', 0) == 4 or getattr(e, 'delta', 0) > 0 else 1
        self.canvas.yview_scroll(step, 'units')

    def log(self, line):
        self.txt['state'] = 'normal'
        self.txt.insert('end', line.rstrip() + '\n')
        self.txt.see('end')
        self.txt['state'] = 'disabled'

    def _poll_log(self):
        try:
            while True:
                item = self.q.get_nowait()
                if not isinstance(item, tuple):
                    self.log(item)
                    continue
                # worker threads post (what, payload); a lookup result is NOT a build result,
                # and routing it into _finish() reported a failed build and left the Look up
                # button disabled with no way back
                what, payload = item
                if what == 'lookup':
                    self._show_lookup(payload)
                else:
                    self._finish(what, payload)
        except queue.Empty:
            pass
        self.root.after(100, self._poll_log)


def main():
    root = tk.Tk()
    root.title(APP)
    root.minsize(720, 480)
    root.update_idletasks()
    h = min(1000, root.winfo_screenheight() - 120)      # never taller than the display
    root.geometry(f'820x{h}')
    try:                                   # crisper widgets than the 1990s default theme
        ttk.Style().theme_use('aqua' if sys.platform == 'darwin' else 'vista')
    except tk.TclError:
        pass
    App(root)
    root.mainloop()


if __name__ == '__main__':
    main()
