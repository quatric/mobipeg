"""GUI smoke test: drives the REAL MainWindow widgets (button .click(),
combo box selection, QLineEdit text) rather than mocking anything, so it
exercises the actual signal/slot wiring and the actual QProcess encode path.

Not a pytest test (a single QApplication instance driven through several
real scenarios in sequence is simpler to reason about as a standalone
script than across separate test functions with Qt's app-singleton
lifecycle). Run directly: `python tests/gui_smoke.py`.

Prints PASS/FAIL for each check and exits non-zero on any failure.
"""
from __future__ import annotations

import os
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from PySide6.QtCore import QProcess
from PySide6.QtWidgets import QApplication

from mobipeg3ds.backend import resolve_args
from mobipeg3ds.gui.main_window import MainWindow

SOURCE = r"C:\dev\MIVF\anime.mp4"
OUT_REAL = r"C:\dev\MIVF\mobipeg-3ds-public\tests\_gui_smoke_out.moflex"
OUT_CANCEL = r"C:\dev\MIVF\mobipeg-3ds-public\tests\_gui_smoke_cancel.moflex"

failures: list[str] = []


def check(label: str, condition: bool) -> None:
    status = "PASS" if condition else "FAIL"
    print(f"[{status}] {label}")
    if not condition:
        failures.append(label)


def pump(app: QApplication, seconds: float) -> None:
    end = time.time() + seconds
    while time.time() < end:
        app.processEvents()
        time.sleep(0.02)


def main() -> int:
    for p in (OUT_REAL, OUT_REAL + ".partial", OUT_CANCEL, OUT_CANCEL + ".partial"):
        if os.path.exists(p):
            os.remove(p)

    app = QApplication.instance() or QApplication(sys.argv)
    win = MainWindow()

    # ---- drive the actual Source tab ----
    win._load_source(SOURCE)
    check("source loaded (job.source set)", win.job.source is not None)
    check("video_frame_count populated", win.job.source.video_frame_count == 1866)

    # ---- drive the actual Picture radio buttons ----
    win.fill_radio.click()
    check("Picture: Fill radio click updates job", win.job.picture.mode == "fill")
    win.fit_radio.click()
    check("Picture: Fit radio click updates job", win.job.picture.mode == "fit")

    # ---- drive the actual Quality combo box ----
    idx = win.preset_combo.findData("balanced")
    win.preset_combo.setCurrentIndex(idx)
    check("Quality: preset combo selects balanced", win.job.quality.preset == "balanced")
    check("Quality: QP resolved to 25", win.job.quality.qp == 25)
    check("Quality: QYX resolved to 3", win.job.quality.mobi_qyx == 3)

    # ---- drive the actual Output line edit ----
    win.output_path_edit.setText(OUT_REAL)
    check("Output: path set on job", win.job.output.path == OUT_REAL)

    # ---- Review tab must match the shared backend exactly ----
    win._refresh_review()
    review_text = win.review_text.toPlainText()
    direct_resolved = resolve_args(win.job)
    check(
        "Review tab command == direct resolve_args() output",
        direct_resolved.display_string() in review_text,
    )

    # ---- real encode via the actual Start Encode button + QProcess ----
    win.start_btn.click()
    check("Start Encode: process object created", win._encode_process is not None)
    deadline = time.time() + 90
    while (win._encode_process is not None
           and win._encode_process.state() != QProcess.ProcessState.NotRunning
           and time.time() < deadline):
        pump(app, 1)

    check("Real encode: output file exists", os.path.exists(OUT_REAL))
    check("Real encode: .partial cleaned up (renamed away)", not os.path.exists(OUT_REAL + ".partial"))
    check("Real encode: status shows verification passed",
          "Verification passed" in win.status_label.text())
    check("Real encode: verify output mentions matches=True",
          "matches=True" in win.output_text.toPlainText())

    gui_cmd = None
    for line in win.output_text.toPlainText().splitlines():
        if line.startswith("$ "):
            gui_cmd = line[2:]
            break
    check("Encode used the exact reviewed command (not a re-derived one)",
          gui_cmd is not None and gui_cmd == direct_resolved.display_string())

    # ---- cancellation test: start a second encode, cancel almost immediately ----
    win.output_path_edit.setText(OUT_CANCEL)
    win._refresh_review()
    win.start_btn.click()
    pump(app, 0.4)  # let ffmpeg actually spawn and open the output file
    win.cancel_btn.click()
    pump(app, 3)

    check("Cancel: final output was never created", not os.path.exists(OUT_CANCEL))
    check("Cancel: .partial file remains on disk (not silently deleted)",
          os.path.exists(OUT_CANCEL + ".partial"))
    check("Cancel: status does not claim success",
          "Verification passed" not in win.status_label.text())

    for p in (OUT_REAL, OUT_REAL + ".partial", OUT_CANCEL, OUT_CANCEL + ".partial"):
        if os.path.exists(p):
            os.remove(p)

    print(f"\n{len(failures)} failure(s) out of checks run.")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
