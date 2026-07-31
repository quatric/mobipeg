"""Captures screenshots of the Quality, Review, and Encode & Verify tabs by
driving the real MainWindow (same pattern as gui_smoke.py) and using Qt's
own QWidget.grab() rather than OS-level screen capture -- precise, and
doesn't depend on window focus/foreground state.
"""
from __future__ import annotations

import os
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from PySide6.QtCore import QProcess
from PySide6.QtWidgets import QApplication

from mobipeg3ds.gui.main_window import MainWindow

SOURCE = r"C:\dev\MIVF\anime.mp4"
OUT = r"C:\dev\MIVF\mobipeg-3ds-public\tests\_screenshot_run.moflex"
SHOT_DIR = r"C:\dev\MIVF\mobipeg-3ds-public\docs\screenshots"


def pump(app: QApplication, seconds: float) -> None:
    end = time.time() + seconds
    while time.time() < end:
        app.processEvents()
        time.sleep(0.02)


def main() -> int:
    if os.path.exists(OUT):
        os.remove(OUT)
    if os.path.exists(OUT + ".partial"):
        os.remove(OUT + ".partial")

    app = QApplication.instance() or QApplication(sys.argv)
    win = MainWindow()
    win.resize(820, 680)
    win.show()
    pump(app, 0.3)

    win._load_source(SOURCE)
    win.output_path_edit.setText(OUT)
    pump(app, 0.2)

    win.tabs.setCurrentWidget(win.tabs.widget(2))  # Quality
    pump(app, 0.2)
    win.grab().save(os.path.join(SHOT_DIR, "gui_quality_tab.png"))
    print("saved gui_quality_tab.png")

    win.tabs.setCurrentWidget(win.tabs.widget(5))  # Review
    win._refresh_review()
    pump(app, 0.2)
    win.grab().save(os.path.join(SHOT_DIR, "gui_review_tab.png"))
    print("saved gui_review_tab.png")

    win.tabs.setCurrentWidget(win.tabs.widget(6))  # Encode & Verify
    pump(app, 0.2)
    win.start_btn.click()
    deadline = time.time() + 90
    while (win._encode_process is not None
           and win._encode_process.state() != QProcess.ProcessState.NotRunning
           and time.time() < deadline):
        pump(app, 1)
    pump(app, 0.5)
    win.grab().save(os.path.join(SHOT_DIR, "gui_encode_verify_tab.png"))
    print("saved gui_encode_verify_tab.png")

    for p in (OUT, OUT + ".partial"):
        if os.path.exists(p):
            os.remove(p)
    win.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
