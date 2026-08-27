#!/usr/bin/env python3
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
    check(set(gui.AUDIO_ONLY_FORMATS) == set(encode.AUDIO_FORMATS),
          "AUDIO_ONLY_FORMATS %s != encode.AUDIO_FORMATS %s"
          % (sorted(gui.AUDIO_ONLY_FORMATS), sorted(encode.AUDIO_FORMATS)))

    for label, fmt in gui.app.formats_map.items():
        check(fmt in gui.app.audio_options,
              "format %r (%s) has no audio_options entry" % (fmt, label))
        check(gui.app.audio_options.get(fmt),
              "format %r offers an empty audio codec list" % fmt)

    # For the audio-only formats the dropdown choices are keys into encode.py's
    # codec table. Offering anything else means encode.py silently substitutes.
    for fmt in gui.AUDIO_ONLY_FORMATS:
        codecs = encode.AUDIO_FORMAT_CODECS.get(fmt)
        if not check(codecs is not None,
                     "%r is audio-only but encode.py has no codec table for it"
                     % fmt):
            continue
        for choice in gui.app.audio_options.get(fmt, []):
            check(choice in codecs,
                  "GUI offers %r=%r, which encode.py would substitute (has %s)"
                  % (fmt, choice, sorted(codecs)))

    for fmt in encode.AUDIO_FORMATS:
        check(fmt in gui.app.formats_map.values(),
              "encode.py supports %r but the GUI does not offer it" % fmt)


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
        check(actual == list(expected),
              "%s: audio dropdown is %s, expected %s" % (fmt, actual, expected))
        check(app.enc_audio_var.get() in expected,
              "%s: selected audio %r is not among %s"
              % (fmt, app.enc_audio_var.get(), expected))

        audio_only = fmt in gui.AUDIO_ONLY_FORMATS
        for name, label, entry in video_only:
            for w in (label, entry):
                mapped = is_shown(w)
                if audio_only:
                    check(not mapped,
                          "%s is audio-only but still shows the %s field"
                          % (fmt, name))
                else:
                    check(mapped,
                          "%s is a video format but hides the %s field"
                          % (fmt, name))

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
                print("    row %2d col %d %s %s"
                      % (row, col, "vis " if mapped else "HID", text))


def check_ffargs(gui):
    """The extra-parameters field has to reach encode.py's argv."""
    app = gui.app
    captured = []
    app.execute_cmd = lambda cmd, btn: captured.append(list(cmd))

    # run_encoding needs an input path that exists; any file will do, since
    # execute_cmd is stubbed and nothing is actually run.
    probe = os.path.abspath(__file__)

    for tab, setup, run in (
        ("encode", lambda v: (app.enc_input_var.set(probe),
                              app.enc_ffargs_var.set(v)), lambda: app.run_encoding()),
        ("decode", lambda v: (app.dec_input_var.set(probe),
                              app.dec_ffargs_var.set(v)), lambda: app.run_decoding()),
    ):
        setup("-t 5 -af volume=0.5")
        del captured[:]
        run()
        if not check(captured, "%s tab: no command was built" % tab):
            continue
        cmd = captured[-1]
        if check("--ffmpeg-args" in cmd,
                 "%s tab: --ffmpeg-args missing from %s" % (tab, cmd)):
            value = cmd[cmd.index("--ffmpeg-args") + 1]
            check(value == "-t 5 -af volume=0.5",
                  "%s tab: --ffmpeg-args carried %r" % (tab, value))

        # Blank must not pass an empty argument through.
        setup("")
        del captured[:]
        run()
        if captured:
            check("--ffmpeg-args" not in captured[-1],
                  "%s tab: blank field still passed --ffmpeg-args" % tab)


def check_decoder_families(gui):
    exts = " ".join(e for _, e in gui.DECODER_FAMILIES)
    for ext in ("*.dpg", "*.dsp", "*.brstm", "*.bfstm", "*.bcstm",
                "*.bns", "*.ast", "*.btsnd"):
        check(ext in exts, "%s is missing from DECODER_FAMILIES" % ext)
    check(gui.DECODE_FILETYPES[0][0] == "All supported",
          "the decode file dialog lost its 'All supported' entry")


def main():
    dump = "--dump" in sys.argv
    here = os.path.dirname(os.path.abspath(__file__))
    sys.path.insert(0, here)

    try:
        import encode_gui as gui
    except ImportError as e:
        print("cannot import encode_gui (%s).\n"
              "This needs tkinter and a display; on a headless machine run\n"
              "    xvfb-run -a python3 gui_smoke.py" % e)
        return 2
    import encode

    try:
        gui.app = gui.EncodeGUI()
    except Exception as e:
        print("could not create the GUI: %s\n"
              "On a headless machine run it under xvfb-run." % e)
        return 2
    gui.app.withdraw()
    gui.app.update_idletasks()

    check_tables(gui, encode)
    check_per_format(gui, dump)
    check_ffargs(gui)
    check_decoder_families(gui)

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
