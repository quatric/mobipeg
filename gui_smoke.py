#!/usr/bin/env python3
# Copyright (c) 2026 quatric - quatricsoftware@gmail.com
"""Headless smoke test for encode_gui.py.

The GUI is table-driven: formats_map, audio_options and enc_conditional_widgets
are three hand-maintained lists that have to agree with each other and with
encode.py's own format tables. Nothing enforces that, and a mismatch is quiet
rather than loud -- a format offering an audio codec encode.py does not have
just gets silently substituted at run time. This walks every format through
the real widget tree and checks the things that would otherwise only show up
in front of a user.

Needs a display. On a headless machine:

    xvfb-run -a python3 gui_smoke.py            # assert
    xvfb-run -a python3 gui_smoke.py --dump     # assert, and print the layout
"""

import os
import sys

FAILURES = []


def check(condition, message):
    """Record a failure and keep going, so one run reports everything."""
    if not condition:
        FAILURES.append(message)
    return condition


def is_shown(w):
    """Whether the widget is currently in the grid.

    Not winfo_ismapped(): the toplevel is withdrawn here, which unmaps every
    child regardless. grid_remove() -- which is what the GUI uses to hide a
    row -- drops the widget from the geometry manager, so grid_info() going
    empty is the thing that actually distinguishes shown from hidden.
    """
    try:
        return bool(w.grid_info())
    except Exception:
        return False


def widget_row(w):
    """(row, column, shown) for a gridded widget, or None if never gridded."""
    try:
        info = w.grid_info()
    except Exception:
        return None
    if not info:
        return None
    return int(info["row"]), int(info["column"]), True


def check_tables(gui, encode):
    """The GUI's tables against encode.py's."""
    check(
        set(gui.AUDIO_ONLY_FORMATS) == set(encode.AUDIO_FORMATS),
        "AUDIO_ONLY_FORMATS %s != encode.AUDIO_FORMATS %s"
        % (sorted(gui.AUDIO_ONLY_FORMATS), sorted(encode.AUDIO_FORMATS)),
    )

    for label, fmt in gui.app.formats_map.items():
        check(
            fmt in gui.app.audio_options,
            "format %r (%s) has no audio_options entry" % (fmt, label),
        )
        check(
            gui.app.audio_options.get(fmt),
            "format %r offers an empty audio codec list" % fmt,
        )

    # For the audio-only formats the dropdown choices are keys into encode.py's
    # codec table. Offering anything else means encode.py silently substitutes.
    for fmt in gui.AUDIO_ONLY_FORMATS:
        codecs = encode.AUDIO_FORMAT_CODECS.get(fmt)
        if not check(
            codecs is not None,
            "%r is audio-only but encode.py has no codec table for it" % fmt,
        ):
            continue
        for choice in gui.app.audio_options.get(fmt, []):
            check(
                choice in codecs,
                "GUI offers %r=%r, which encode.py would substitute (has %s)"
                % (fmt, choice, sorted(codecs)),
            )

    for fmt in encode.AUDIO_FORMATS:
        check(
            fmt in gui.app.formats_map.values(),
            "encode.py supports %r but the GUI does not offer it" % fmt,
        )


def check_per_format(gui, dump=False):
    """Drive every format through on_enc_format_change and inspect the tree."""
    app = gui.app
    label_for = {v: k for k, v in app.formats_map.items()}

    # Widgets that describe a video stream. An audio-only format has none, so
    # showing these would be offering settings that go nowhere.
    video_only = [
        ("scale", app.enc_scale_label, app.enc_scale_entry),
        ("fps", app.enc_fps_label, app.enc_fps_entry),
        # --hq only moves video knobs, so offering it for an audio format
        # would be a control that silently does nothing.
        ("highest-quality", app.enc_hq_chk, app.enc_hq_chk),
    ]

    for fmt in sorted(set(app.formats_map.values())):
        app.enc_fmt_var.set(label_for[fmt])
        app.on_enc_format_change()
        app.update_idletasks()

        expected = app.audio_options[fmt]
        actual = list(app.enc_audio_cb.cget("values"))
        check(
            actual == list(expected),
            "%s: audio dropdown is %s, expected %s" % (fmt, actual, expected),
        )
        check(
            app.enc_audio_var.get() in expected,
            "%s: selected audio %r is not among %s"
            % (fmt, app.enc_audio_var.get(), expected),
        )

        audio_only = fmt in gui.AUDIO_ONLY_FORMATS
        for name, label, entry in video_only:
            # A few video formats deliberately leave Highest Quality out.
            no_field = audio_only or (
                name == "highest-quality" and fmt in gui.NO_HQ_FORMATS
            )
            for w in (label, entry):
                mapped = is_shown(w)
                if no_field:
                    check(not mapped, "%s should not show the %s field" % (fmt, name))
                else:
                    check(
                        mapped,
                        "%s is a video format but hides the %s field" % (fmt, name),
                    )

        if dump:
            print("\n--- %s (%s)" % (fmt, label_for[fmt]))
            print("    audio options: %s" % ", ".join(expected))
            rows = []
            for child in app.encode_frame.winfo_children():
                pos = widget_row(child)
                if pos is None:
                    continue
                text = ""
                try:
                    text = child.cget("text")
                except Exception:
                    pass
                rows.append((pos[0], pos[1], pos[2], text or child.winfo_class()))
            for row, col, mapped, text in sorted(rows):
                print(
                    "    row %2d col %d %s %s"
                    % (row, col, "vis " if mapped else "HID", text)
                )


def check_ffargs(gui):
    """The extra-parameters field has to reach encode.py's argv."""
    app = gui.app
    captured = []
    app.execute_cmd = lambda cmd, btn: captured.append(list(cmd))

    # run_encoding needs an input path that exists; any file will do, since
    # execute_cmd is stubbed and nothing is actually run.
    probe = os.path.abspath(__file__)

    for tab, setup, run in (
        (
            "encode",
            lambda v: (app.enc_input_var.set(probe), app.enc_ffargs_var.set(v)),
            lambda: app.run_encoding(),
        ),
        (
            "decode",
            lambda v: (app.dec_input_var.set(probe), app.dec_ffargs_var.set(v)),
            lambda: app.run_decoding(),
        ),
    ):
        setup("-t 5 -af volume=0.5")
        del captured[:]
        run()
        if not check(captured, "%s tab: no command was built" % tab):
            continue
        cmd = captured[-1]
        if check(
            "--ffmpeg-args" in cmd, "%s tab: --ffmpeg-args missing from %s" % (tab, cmd)
        ):
            value = cmd[cmd.index("--ffmpeg-args") + 1]
            check(
                value == "-t 5 -af volume=0.5",
                "%s tab: --ffmpeg-args carried %r" % (tab, value),
            )

        # Blank must not pass an empty argument through.
        setup("")
        del captured[:]
        run()
        if captured:
            check(
                "--ffmpeg-args" not in captured[-1],
                "%s tab: blank field still passed --ffmpeg-args" % tab,
            )


def check_flipnote(gui):
    """The Flipnote panel has to show for both Flipnote formats, offer the
    .kwz-only settings for .kwz alone, and reach encode.py's argv."""
    app = gui.app
    label_for = {v: k for k, v in app.formats_map.items()}
    kwz_only = (
        app.enc_flip_dither_cb,
        app.enc_flip_paper_cb,
        app.enc_flip_key_entry,
        app.enc_flip_loop_chk,
    )
    captured = []
    app.execute_cmd = lambda cmd, btn: captured.append(list(cmd))
    app.enc_input_var.set(os.path.abspath(__file__))
    app.enc_ffargs_var.set("")

    for fmt in sorted(set(app.formats_map.values())):
        app.enc_fmt_var.set(label_for[fmt])
        app.on_enc_format_change()
        app.update_idletasks()
        flip = fmt in ("flipnote", "flipnote3d")
        check(
            is_shown(app.enc_flip_frame) == flip,
            "%s: Flipnote options are %s" % (fmt, "hidden" if flip else "shown"),
        )
        if flip:
            for w in kwz_only:
                check(
                    is_shown(w) == (fmt == "flipnote3d"),
                    "%s: wrong visibility for a .kwz-only Flipnote setting" % fmt,
                )

    for fmt in ("flipnote", "flipnote3d"):
        app.enc_fmt_var.set(label_for[fmt])
        app.on_enc_format_change()
        app.enc_flip_author_var.set("tester")
        app.enc_flip_dither_var.set("2x2")
        app.enc_flip_paper_var.set("black")
        app.enc_flip_key_var.set("key.der")
        app.enc_flip_loop_var.set(True)
        del captured[:]
        app.run_encoding()
        if not check(captured, "%s: no command was built" % fmt):
            continue
        cmd = captured[-1]
        check(
            (
                cmd[cmd.index("--flipnote-author") + 1] == "tester"
                if "--flipnote-author" in cmd
                else False
            ),
            "%s: --flipnote-author missing from %s" % (fmt, cmd),
        )
        for flag, value in (
            ("--flipnote-dither", "2x2"),
            ("--flipnote-paper", "black"),
            ("--flipnote-key", "key.der"),
            ("--flipnote-loop", None),
        ):
            if fmt == "flipnote3d":
                ok = flag in cmd and (
                    value is None or cmd[cmd.index(flag) + 1] == value
                )
                check(ok, "%s: %s missing from %s" % (fmt, flag, cmd))
            else:
                check(flag not in cmd, "%s: passed the .kwz-only %s" % (fmt, flag))
    app.enc_flip_author_var.set("")
    app.enc_flip_key_var.set("")
    app.enc_flip_loop_var.set(False)


def check_decoder_families(gui):
    exts = " ".join(e for _, e in gui.DECODER_FAMILIES)
    for ext in (
        "*.dpg",
        "*.dsp",
        "*.brstm",
        "*.bfstm",
        "*.bcstm",
        "*.bns",
        "*.ast",
        "*.btsnd",
    ):
        check(ext in exts, "%s is missing from DECODER_FAMILIES" % ext)
    check(
        gui.DECODE_FILETYPES[0][0] == "All supported",
        "the decode file dialog lost its 'All supported' entry",
    )


def pump(app, until, timeout=20.0):
    """Run the real Tk main loop until `until()` is true; False on timeout.

    The GUI's worker threads hand results back with app.after(), which Tk only
    accepts while the main loop is running, so update() polling is not enough.
    """
    import time

    state = {"ok": False}
    end = time.time() + timeout

    def tick():
        if until():
            state["ok"] = True
            app.quit()
        elif time.time() > end:
            app.quit()
        else:
            app.after(20, tick)

    app.after(20, tick)
    app.mainloop()
    return state["ok"]


def build(app, fmt_label, **overrides):
    """The encode command for a format, with the form set by `overrides`."""
    app.enc_fmt_var.set(fmt_label)
    app.on_enc_format_change()
    for key, value in overrides.items():
        app.persist_vars[key].set(value)
    probe = os.path.abspath(__file__)
    return app.build_encode_cmd(probe, "", "")


def check_extras(gui):
    """Controls added on top of the original form: they must reach the argv."""
    app = gui.app
    if "execute_cmd" in vars(app):
        del app.execute_cmd  # check_ffargs stubbed it; restore the real one

    # --- audio / size / HDR / jobs flags (MobiClip 2D) ---
    app.enc_track_cb.config(values=["Default", "track 0", "track 1"])
    app.enc_track_var.set("track 1")
    cmd = build(app, "3DS Mobiclip .moflex (2D)", offset="-120", normalize=True,
                max_size_mb="64", hdr="hable", jobs="2")
    for flag, value in (("--audio-track", "1"), ("--audio-offset-ms", "-120"),
                        ("--max-size-mb", "64"), ("--hdr-mode", "hable"),
                        ("--jobs", "2")):
        if check(flag in cmd, "moflex: %s missing from %s" % (flag, cmd)):
            check(cmd[cmd.index(flag) + 1] == value,
                  "moflex: %s carried %r" % (flag, cmd[cmd.index(flag) + 1]))
    check("--normalize-audio" in cmd, "moflex: --normalize-audio missing")

    # Defaults must stay off the command line.
    app.enc_track_var.set("Default")
    cmd = build(app, "3DS Mobiclip .moflex (2D)", offset="0", normalize=False,
                max_size_mb="", hdr="auto", jobs="0")
    for flag in ("--audio-track", "--audio-offset-ms", "--normalize-audio",
                 "--max-size-mb", "--hdr-mode", "--jobs", "--super"):
        check(flag not in cmd, "moflex defaults: %s should not be passed (%s)" % (flag, cmd))

    # Audio controls vanish, and their flags with them, when there is no audio.
    cmd = build(app, "3DS Mobiclip .moflex (2D)", audio="none", offset="250", normalize=True)
    check("--audio-offset-ms" not in cmd and "--normalize-audio" not in cmd,
          "audio none: audio flags still passed: %s" % cmd)
    check(not is_shown(app.enc_offset_entry), "audio none: offset field still shown")

    # --max-size-mb is MobiClip-only; HDR is video-only.
    cmd = build(app, "DS ActImagine .vx", max_size_mb="10")
    check("--max-size-mb" not in cmd, "vx must not pass --max-size-mb")
    app.enc_fmt_var.set("Nintendo DSP-ADPCM .dsp")
    app.on_enc_format_change()
    check(not is_shown(app.enc_hdr_cb), "audio-only format shows the HDR control")
    check(not is_shown(app.enc_out_frame) or not is_shown(app.enc_maxsize_entry),
          "audio-only format shows size controls")

    # --- Super MOFLEX ---
    app.enc_super_var.set(True)
    app.enc_audio2_var.set("/tmp/second.wav")
    app.enc_lang_var.set("eng")
    app.enc_lang2_var.set("jpn")
    app.enc_srt_list.set(["/tmp/a.eng.srt", "/tmp/b.jpn.srt"])
    app.enc_poster_var.set("/tmp/poster.png")
    cmd = build(app, "3DS Mobiclip .moflex (2D)")
    check("--super" in cmd, "super: flag missing")
    check(cmd.count("--srt") == 2, "super: expected two --srt in %s" % cmd)
    for flag, value in (("--audio2", "/tmp/second.wav"), ("--lang", "eng"),
                        ("--lang2", "jpn"), ("--poster", "/tmp/poster.png")):
        if check(flag in cmd, "super: %s missing" % flag):
            check(cmd[cmd.index(flag) + 1] == value, "super: %s wrong" % flag)
    cmd = build(app, "DS Mobiclip .mods")
    check("--super" not in cmd, "mods must not pass --super")
    app.enc_super_var.set(False)

    # --- advanced MobiClip options only with the section open ---
    app.enc_minqp_var.set("14")
    app.enc_me_var.set("umh")
    app.enc_dct8_var.set("off")
    app.enc_block_var.set("2048")
    app.enc_level_var.set("Intermediate")
    cmd = build(app, "3DS Mobiclip .moflex (2D)")
    check("--min-qp" not in cmd, "advanced closed: --min-qp still passed")
    app.enc_level_var.set("Advanced")
    cmd = build(app, "3DS Mobiclip .moflex (2D)")
    for flag, value in (("--min-qp", "14"), ("--me", "umh"), ("--8x8dct", "0"),
                        ("--mo-block", "2048")):
        if check(flag in cmd, "advanced: %s missing from %s" % (flag, cmd)):
            check(cmd[cmd.index(flag) + 1] == value, "advanced: %s wrong" % flag)
    app.enc_level_var.set("Intermediate")
    app.enc_minqp_var.set("")
    app.enc_me_var.set("default")
    app.enc_dct8_var.set("default")
    app.enc_block_var.set("auto")

    # --- presets and saved settings ---
    app.enc_fmt_var.set("3DS Mobiclip .moflex (2D)")
    app.on_enc_format_change()
    app.apply_builtin_preset("Small (700k two-pass)")
    check(app.enc_bitrate_var.get() == "700k" and app.enc_multipass_var.get(),
          "preset: bitrate/multipass not applied")
    check(not app.enc_hq_var.get(), "preset: Highest Quality should be off")
    cmd = app.build_encode_cmd(os.path.abspath(__file__), "", "")
    check("--bitrate" in cmd and "--multipass" in cmd,
          "preset: advanced flags not sent (%s)" % cmd)
    app.apply_builtin_preset("Fit 3DS SD (under 4 GiB)")
    check(app.enc_maxsize_var.get() == "4000", "preset: max size not applied")
    app.apply_builtin_preset("Best quality (largest file)")
    check(app.enc_hq_var.get() and app.enc_quant_var.get() == "12",
          "preset: best quality not applied")

    app.enc_offset_var.set("77")
    snap = app.snapshot_settings()
    app.enc_offset_var.set("0")
    app.apply_settings(snap)
    check(app.enc_offset_var.get() == "77", "settings: round trip lost the offset")
    app.apply_settings({"fmt": "no such format", "bogus": 1, "quant": "20"})
    check(app.enc_quant_var.get() == "20", "settings: unknown keys broke apply")
    app.enc_offset_var.set("0")

    # --- processes: progress, exit code, stop ---
    import sys as _sys
    result = {}
    ok = app.start_process(
        [_sys.executable, "-c",
         "print('>> pass 1/1'); print('frame=1 time=00:00:01.00 bitrate=1')"],
        buttons=[app.enc_run_btn], duration=2.0, title="t",
        on_done=lambda code, stopped: result.update(code=code, stopped=stopped),
    )
    check(ok, "start_process refused to start")
    check(pump(app, lambda: "code" in result), "start_process never finished")
    check(result.get("code") == 0, "start_process exit code %r" % result.get("code"))
    check(not app.busy(), "start_process left the app busy")
    check(str(app.enc_run_btn.cget("state")) == "normal", "run button not re-enabled")
    check(abs(float(app.progress["value"]) - 100.0) < 0.01, "progress should end at 100")

    result.clear()
    app.start_process(
        [_sys.executable, "-c", "import time; time.sleep(60)"],
        on_done=lambda code, stopped: result.update(code=code, stopped=stopped),
    )
    pump(app, lambda: app.busy(), 3)
    import tkinter.messagebox as _mb
    _info = _mb.showinfo
    _mb.showinfo = lambda *a, **k: None  # the "already running" notice is modal
    try:
        check(not app.start_process([_sys.executable, "-c", "pass"]),
              "a second job started while one was running")
    finally:
        _mb.showinfo = _info
    app.stop_process()
    check(pump(app, lambda: "code" in result, 10), "stop_process did not end the job")
    check(result.get("stopped") is True, "stopped job not reported as stopped")

    # --- queue ---
    clip = os.path.abspath(__file__)
    app.enc_fmt_var.set("3DS Mobiclip .moflex (2D)")
    app.on_enc_format_change()
    q = app.queue_tab
    q.queue.jobs[:] = []
    n = q.add_paths([clip, clip])
    check(n == 2 and len(q.queue.jobs) == 2, "queue: expected two jobs, got %d" % n)
    outs = [j["output"] for j in q.queue.jobs]
    check(len(set(outs)) == 2, "queue: duplicate output names %s" % outs)
    check(all(j["cmd"][-0 or 0] for j in q.queue.jobs), "queue: empty command")
    app.enc_fmt_var.set("Nintendo 3DS Camera 3D .avi")
    app.on_enc_format_change()
    before = len(q.queue.jobs)
    import tkinter.messagebox as mb
    shown = []
    orig = mb.showwarning
    mb.showwarning = lambda *a, **k: shown.append(a)
    try:
        q.add_paths([clip])
    finally:
        mb.showwarning = orig
    check(len(q.queue.jobs) == before, "queue: a two-input format was queued")
    q.queue.jobs[:] = []
    q.refresh()

    # --- inspect tab ---
    import struct, tempfile
    d = tempfile.mkdtemp()
    mo = os.path.join(d, "x.moflex")
    with open(mo, "wb") as f:
        for i in range(4):
            b = bytearray(2048)
            if i % 2 == 0:
                b[0:2] = b"\x4c\x32"
                b[4:12] = struct.pack(">Q", i * 1_000_000)
                b[12:14] = struct.pack(">H", 2047)
            f.write(b)
    os.environ["FFPROBE"] = "/nonexistent/ffprobe"
    try:
        app.inspect_tab.set_input(mo)
        app.inspect_tab.run()
        check(pump(app, lambda: not app.busy()), "inspect never finished")
    finally:
        os.environ.pop("FFPROBE", None)
    check("OK" in app.inspect_tab.result.cget("text"),
          "inspect result: %r" % app.inspect_tab.result.cget("text"))
    check("disabled" not in app.inspect_tab.cia_btn.state(), "CIA hand-off not enabled")


def check_levels(gui):
    """Simple / Intermediate / Advanced hide controls without losing any."""
    app = gui.app
    app.enc_fmt_var.set("3DS Mobiclip .moflex (2D)")
    expect = {
        # widget: lowest level index that shows it
        "enc_audio_cb": 0, "enc_hq_chk": 0, "enc_maxsize_entry": 0,
        "enc_scale_entry": 0, "enc_profile_cb": 0, "enc_speed_cb": 0, "enc_fps_entry": 1, "enc_offset_entry": 1, "enc_hdr_cb": 1,
        "enc_super_frame": 1, "enc_jobs_entry": 2, "enc_layout_entry": None,
    }
    for index, name in enumerate(app.LEVELS):
        app.enc_level_var.set(name)
        app.update_idletasks()
        for attr, minimum in expect.items():
            if minimum is None:
                continue
            shown = is_shown(getattr(app, attr))
            check(shown == (index >= minimum),
                  "%s level: %s shown=%s, expected %s" % (name, attr, shown, index >= minimum))
        check(not is_shown(app.enc_adv_toggle_chk), "%s: old Advanced checkbox is back" % name)
        check(is_shown(app.enc_adv_frame) == (name == "Advanced"),
              "%s: advanced tuning frame visibility wrong" % name)
        ff = [w for w in app.enc_ffargs_widgets if w.grid_info()]
        check(bool(ff) == (name == "Advanced"), "%s: extra ffmpeg row visibility wrong" % name)
    # An audio-only format has nothing left in the Output frame at Simple.
    app.enc_level_var.set("Simple")
    app.enc_fmt_var.set("Nintendo DSP-ADPCM .dsp")
    app.on_enc_format_change()
    check(not is_shown(app.enc_out_frame), "Simple + audio-only: empty Output frame shown")
    # A preset that needs the Advanced section raises the level.
    app.enc_fmt_var.set("3DS Mobiclip .moflex (2D)")
    app.on_enc_format_change()
    app.apply_builtin_preset("Small (700k two-pass)")
    check(app.enc_level_var.get() == "Advanced", "bitrate preset did not raise the level")
    app.enc_level_var.set("Advanced")


def check_presets_and_drop(gui):
    """Q-ladder presets work at the Simple level; dropped paths reach the form."""
    app = gui.app
    app.enc_fmt_var.set("3DS Mobiclip .moflex (2D)")
    app.on_enc_format_change()
    app.enc_level_var.set("Simple")
    app.apply_builtin_preset("Quality — the sweet spot (Q30)")
    check(app.enc_level_var.get() == "Simple", "a Q preset must not raise the level")
    cmd = app.build_encode_cmd(os.path.abspath(__file__), "", "")
    for flag, value in (("--quantizer", "30"), ("--mobi-subme", "9"),
                        ("--mobi-skip", "0"), ("--me", "hex")):
        if check(flag in cmd, "Q30 preset: %s missing from %s" % (flag, cmd)):
            check(cmd[cmd.index(flag) + 1] == value, "Q30 preset: %s wrong" % flag)
    check("--hq" not in cmd, "Q30 preset: Highest Quality should be off")

    # Encoding speed: Fast lowers the search depth and picks the diamond search.
    app.enc_speed_var.set(app.SPEED_CHOICES[1])
    app.apply_builtin_preset("Best \u2014 wide search (Q27)")  # subme 9, me umh
    cmd = app.build_encode_cmd(os.path.abspath(__file__), "", "")
    check(cmd[cmd.index("--mobi-subme") + 1] == "6", "Fast: search depth not capped at 6")
    check(cmd[cmd.index("--me") + 1] == "umh", "Fast: an explicit motion search was overridden")
    app.enc_me_var.set("default")
    cmd = app.build_encode_cmd(os.path.abspath(__file__), "", "")
    check(cmd[cmd.index("--me") + 1] == "dia", "Fast: diamond search not used")
    app.enc_speed_var.set(app.SPEED_CHOICES[0])
    cmd = app.build_encode_cmd(os.path.abspath(__file__), "", "")
    check(cmd[cmd.index("--mobi-subme") + 1] == "9" and "--me" not in cmd,
          "Standard speed changed the search settings: %s" % cmd)
    app.apply_profile("Quality \u2014 the sweet spot (Q30)")
    check(app.enc_quant_var.get() == "30", "profile dropdown did not apply the preset")
    app.enc_level_var.set("Advanced")

    app.enc_fmt_var.set("3DS Mobiclip .moflex (3D)")
    app.on_enc_format_change()
    app.drop_encode(["/tmp/left.mkv", "/tmp/right.mkv"])
    check(app.enc_input_var.get() == "/tmp/left.mkv" and
          app.enc_input2_var.get() == "/tmp/right.mkv", "drop: both eyes not filled")
    app.enc_fmt_var.set("3DS Mobiclip .moflex (2D)")
    app.on_enc_format_change()
    app.enc_input2_var.set("")
    app.drop_encode(["/tmp/a.mkv", "/tmp/b.mkv"])
    check(app.enc_input_var.get() == "/tmp/a.mkv" and not app.enc_input2_var.get(),
          "drop: a 2D format must ignore the second file")
    check(isinstance(gui.enable_file_drop(app, []), bool), "enable_file_drop must not raise")


def check_scroll(gui):
    """Two-finger / wheel scrolling must work with the pointer over any child."""
    app = gui.app
    app.deiconify()
    app.update()
    sc = app.encode_scroll
    check(sc.canvas.yview() != (0.0, 1.0), "Encode form should be taller than its viewport")
    child = app.enc_input_var and app.enc_run_btn  # a widget deep inside the form
    sc.winfo_containing = lambda x, y: child

    class Ev:
        num = 0
        delta = -2  # macOS trackpad: small deltas
        x_root = y_root = 0

    top = sc.canvas.yview()[0]
    for _ in range(5):
        sc._on_wheel(Ev)
    check(sc.canvas.yview()[0] > top, "scrolling over a child widget did nothing")
    Ev.delta = 3
    before = sc.canvas.yview()[0]
    for _ in range(5):
        sc._on_wheel(Ev)
    check(sc.canvas.yview()[0] < before, "scrolling back up did nothing")
    sc.winfo_containing = lambda x, y: app.console  # pointer outside the form
    before = sc.canvas.yview()[0]
    Ev.delta = -2
    sc._on_wheel(Ev)
    check(sc.canvas.yview()[0] == before, "scrolled while the pointer was elsewhere")
    del sc.winfo_containing
    app.withdraw()


def main():
    dump = "--dump" in sys.argv
    here = os.path.dirname(os.path.abspath(__file__))
    sys.path.insert(0, here)
    # Never read or write the real settings/queue/presets.
    import tempfile
    os.environ["MOBIPEG_CONFIG_DIR"] = tempfile.mkdtemp(prefix="mobipeg-smoke-")

    try:
        import encode_gui as gui
    except ImportError as e:
        print(
            "cannot import encode_gui (%s).\n"
            "This needs tkinter and a display; on a headless machine run\n"
            "    xvfb-run -a python3 gui_smoke.py" % e
        )
        return 2
    import encode

    try:
        gui.app = gui.EncodeGUI()
    except Exception as e:
        print(
            "could not create the GUI: %s\n"
            "On a headless machine run it under xvfb-run." % e
        )
        return 2
    gui.app.withdraw()
    gui.app.enc_level_var.set("Advanced")  # the format checks expect every control
    gui.app.update_idletasks()

    check_tables(gui, encode)
    check_per_format(gui, dump)
    check_ffargs(gui)
    check_flipnote(gui)
    check_decoder_families(gui)
    check_extras(gui)
    check_levels(gui)
    check_presets_and_drop(gui)
    check_scroll(gui)

    gui.app.destroy()

    if FAILURES:
        print("\n%d problem(s):" % len(FAILURES))
        for f in FAILURES:
            print("  - %s" % f)
        return 1

    print("\ngui_smoke: all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
