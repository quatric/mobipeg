"""Mobipeg 3DS -- Experimental Preview GUI.

Source -> Picture -> Quality -> Audio -> Output -> Review -> Encode -> Verify.

This module builds ffmpeg arguments NOWHERE. Every resolved command comes
from mobipeg3ds.backend.resolve_args(job) -- exactly what the CLI uses. If a
future change needs a different argument, it goes in backend.py, not here.
"""
from __future__ import annotations

import os
import re
import time

from PySide6.QtCore import QProcess, Qt, QTimer
from PySide6.QtGui import QDragEnterEvent, QDropEvent
from PySide6.QtWidgets import (
    QButtonGroup,
    QCheckBox,
    QComboBox,
    QFileDialog,
    QGroupBox,
    QHBoxLayout,
    QLabel,
    QLineEdit,
    QMainWindow,
    QMessageBox,
    QPlainTextEdit,
    QProgressBar,
    QPushButton,
    QRadioButton,
    QSpinBox,
    QTabWidget,
    QVBoxLayout,
    QWidget,
)

from ..backend import UnsupportedTargetError, resolve_args
from ..job import EncodeJob, OutputSettings, PictureSettings, VerificationPolicy
from ..presets import PRESETS, resolve_preset
from ..probe import ProbeError, probe
from ..verify import verify

APP_TITLE = "Mobipeg 3DS -- Experimental Preview"

_STATS_RE = re.compile(
    r"frame=\s*(?P<frame>\d+).*?fps=\s*(?P<fps>[\d.]+).*?time=(?P<time>\S+).*?speed=\s*(?P<speed>[\d.]+)x"
)


class MainWindow(QMainWindow):
    def __init__(self) -> None:
        super().__init__()
        self.setWindowTitle(APP_TITLE)
        self.resize(760, 620)
        self.setAcceptDrops(True)

        self.job = EncodeJob(output=OutputSettings(path=""))
        self._encode_process: QProcess | None = None
        self._encode_started_at: float | None = None
        self._user_cancelled: bool = False

        self.tabs = QTabWidget()
        self.setCentralWidget(self.tabs)

        self._build_source_tab()
        self._build_picture_tab()
        self._build_quality_tab()
        self._build_audio_tab()
        self._build_output_tab()
        self._build_review_tab()
        self._build_encode_tab()

        banner = QLabel(
            "EXPERIMENTAL PREVIEW -- Nintendo 3DS 2D MOFLEX only. "
            "Quality presets are unresolved placeholders (Candidate B: QP25/QYX3)."
        )
        banner.setStyleSheet("background:#553300; color:#ffdd88; padding:4px;")
        banner.setWordWrap(True)
        outer = QWidget()
        outer_layout = QVBoxLayout(outer)
        outer_layout.setContentsMargins(0, 0, 0, 0)
        outer_layout.addWidget(banner)
        outer_layout.addWidget(self.tabs)
        self.setCentralWidget(outer)

    # ---------- drag and drop ----------
    def dragEnterEvent(self, event: QDragEnterEvent) -> None:
        if event.mimeData().hasUrls():
            event.acceptProposedAction()

    def dropEvent(self, event: QDropEvent) -> None:
        urls = event.mimeData().urls()
        if urls:
            self._load_source(urls[0].toLocalFile())

    # ---------- Source tab ----------
    def _build_source_tab(self) -> None:
        w = QWidget()
        layout = QVBoxLayout(w)

        row = QHBoxLayout()
        self.source_path_edit = QLineEdit()
        self.source_path_edit.setReadOnly(True)
        self.source_path_edit.setPlaceholderText("Drag and drop a video file here, or Browse...")
        browse_btn = QPushButton("Browse...")
        browse_btn.clicked.connect(self._browse_source)
        row.addWidget(self.source_path_edit)
        row.addWidget(browse_btn)
        layout.addLayout(row)

        self.source_info = QPlainTextEdit()
        self.source_info.setReadOnly(True)
        self.source_info.setPlaceholderText("Source probe information will appear here.")
        layout.addWidget(self.source_info)

        self.tabs.addTab(w, "Source")

    def _browse_source(self) -> None:
        path, _ = QFileDialog.getOpenFileName(self, "Select source video")
        if path:
            self._load_source(path)

    def _load_source(self, path: str) -> None:
        try:
            src = probe(path)
        except ProbeError as e:
            QMessageBox.warning(self, "Source problem", str(e))
            return
        self.job.source = src
        self.source_path_edit.setText(path)
        lines = [
            f"path:          {src.path}",
            f"duration:      {src.duration_s:.3f}s (container -- reflects the longest "
            f"stream, often audio, not necessarily the video frame count)"
            if src.duration_s else "duration:      unknown",
            f"resolution:    {src.width}x{src.height}" if src.width else "resolution:    unknown",
        ]
        if src.video_duration_s:
            lines.append(f"video duration:{src.video_duration_s:.3f}s")
        if src.video_frame_count is not None:
            lines.append(f"video frames:  {src.video_frame_count}")
        if src.fps_num and src.fps_den:
            lines.append(f"frame rate:    {src.fps_num}/{src.fps_den} ({src.fps_num/src.fps_den:.3f} fps)")
        lines.append(f"video streams: {src.video_stream_indices}")
        lines.append(f"audio streams: {src.audio_stream_indices}")
        if not src.audio_stream_indices:
            lines.append("(no audio stream detected -- video-only encode will be forced)")
        self.source_info.setPlainText("\n".join(lines))
        self._refresh_audio_tab()
        self._refresh_review()

    # ---------- Picture tab ----------
    def _build_picture_tab(self) -> None:
        w = QWidget()
        layout = QVBoxLayout(w)
        layout.addWidget(QLabel("Output is always 400x240. Choose how the source is fit into it:"))

        self.picture_group = QButtonGroup(w)
        self.fit_radio = QRadioButton("Fit (letterbox) -- preserve aspect ratio, pad with black bars")
        self.fill_radio = QRadioButton("Fill (crop) -- preserve aspect ratio, crop overflow")
        self.fit_radio.setChecked(True)
        self.picture_group.addButton(self.fit_radio)
        self.picture_group.addButton(self.fill_radio)
        layout.addWidget(self.fit_radio)
        layout.addWidget(self.fill_radio)
        self.fit_radio.toggled.connect(self._on_picture_mode_changed)
        self.fill_radio.toggled.connect(self._on_picture_mode_changed)

        layout.addWidget(QLabel(
            "Custom crop is not exposed in this experimental preview -- available in the "
            "backend (mobipeg3ds.job.PictureSettings.custom_crop) but not wired to a control yet."
        ))
        layout.addStretch(1)
        self.tabs.addTab(w, "Picture")

    def _on_picture_mode_changed(self) -> None:
        self.job.picture = PictureSettings(mode="fill" if self.fill_radio.isChecked() else "fit")
        self._refresh_review()

    # ---------- Quality tab ----------
    def _build_quality_tab(self) -> None:
        w = QWidget()
        layout = QVBoxLayout(w)

        self.preset_combo = QComboBox()
        for name, entry in PRESETS.items():
            label = entry["label"] + (" [unresolved placeholder]" if entry["unresolved"] else "")
            self.preset_combo.addItem(label, userData=name)
        self.preset_combo.currentIndexChanged.connect(self._on_preset_changed)
        layout.addWidget(QLabel("Quality preset:"))
        layout.addWidget(self.preset_combo)

        self.preset_description = QLabel("")
        self.preset_description.setWordWrap(True)
        layout.addWidget(self.preset_description)

        advanced = QGroupBox("Advanced")
        adv_layout = QVBoxLayout(advanced)

        qp_row = QHBoxLayout()
        qp_row.addWidget(QLabel("QP (12-161):"))
        self.qp_spin = QSpinBox()
        self.qp_spin.setRange(12, 161)
        self.qp_spin.setValue(25)
        qp_row.addWidget(self.qp_spin)
        adv_layout.addLayout(qp_row)

        qyx_row = QHBoxLayout()
        qyx_row.addWidget(QLabel("QYX (0-15):"))
        self.qyx_spin = QSpinBox()
        self.qyx_spin.setRange(0, 15)
        self.qyx_spin.setValue(3)
        qyx_row.addWidget(self.qyx_spin)
        adv_layout.addLayout(qyx_row)

        self.qrdo_check = QCheckBox("Q-RDO enabled")
        self.qrdo_check.setChecked(True)
        adv_layout.addWidget(self.qrdo_check)

        threads_row = QHBoxLayout()
        threads_row.addWidget(QLabel("Video encode threads:"))
        self.threads_spin = QSpinBox()
        self.threads_spin.setRange(1, 32)
        self.threads_spin.setValue(1)
        threads_row.addWidget(self.threads_spin)
        adv_layout.addLayout(threads_row)

        for widget in (self.qp_spin, self.qyx_spin, self.qrdo_check, self.threads_spin):
            if isinstance(widget, QSpinBox):
                widget.valueChanged.connect(self._on_advanced_changed)
            else:
                widget.toggled.connect(self._on_advanced_changed)

        layout.addWidget(advanced)
        layout.addStretch(1)
        self.tabs.addTab(w, "Quality")
        self._on_preset_changed(0)

    def _on_preset_changed(self, _index: int) -> None:
        name = self.preset_combo.currentData()
        entry = PRESETS[name]
        self.preset_description.setText(entry["description"])
        is_custom = name == "custom"
        for widget in (self.qp_spin, self.qyx_spin, self.qrdo_check, self.threads_spin):
            widget.setEnabled(is_custom)
        if not is_custom:
            values = entry["values"]
            self.qp_spin.setValue(values["qp"])
            self.qyx_spin.setValue(values["mobi_qyx"])
            self.qrdo_check.setChecked(values["q_rdo"])
            self.threads_spin.setValue(values["threads"])
        self._apply_quality()

    def _on_advanced_changed(self, *_args) -> None:
        self._apply_quality()

    def _apply_quality(self) -> None:
        name = self.preset_combo.currentData()
        if name == "custom":
            overrides = dict(
                qp=self.qp_spin.value(),
                mobi_qyx=self.qyx_spin.value(),
                q_rdo=self.qrdo_check.isChecked(),
                threads=self.threads_spin.value(),
            )
            self.job.quality = resolve_preset("custom", overrides)
        else:
            self.job.quality = resolve_preset(name)
        self._refresh_review()

    # ---------- Audio tab ----------
    def _build_audio_tab(self) -> None:
        w = QWidget()
        layout = QVBoxLayout(w)

        layout.addWidget(QLabel("Audio track:"))
        self.audio_track_combo = QComboBox()
        self.audio_track_combo.addItem("(recommended -- first detected track)", userData=None)
        self.audio_track_combo.currentIndexChanged.connect(self._on_audio_changed)
        layout.addWidget(self.audio_track_combo)

        self.video_only_check = QCheckBox("Video only (no audio in output)")
        self.video_only_check.toggled.connect(self._on_audio_changed)
        layout.addWidget(self.video_only_check)

        layout.addStretch(1)
        self.tabs.addTab(w, "Audio")

    def _refresh_audio_tab(self) -> None:
        self.audio_track_combo.clear()
        self.audio_track_combo.addItem("(recommended -- first detected track)", userData=None)
        if self.job.source:
            for idx in self.job.source.audio_stream_indices:
                self.audio_track_combo.addItem(f"stream #{idx}", userData=idx)
            if not self.job.source.audio_stream_indices:
                self.video_only_check.setChecked(True)
                self.video_only_check.setEnabled(False)
            else:
                self.video_only_check.setEnabled(True)

    def _on_audio_changed(self, *_args) -> None:
        self.job.audio_stream_index = self.audio_track_combo.currentData()
        self.job.audio.mode = "video_only" if self.video_only_check.isChecked() else "recommended"
        self._refresh_review()

    # ---------- Output tab ----------
    def _build_output_tab(self) -> None:
        w = QWidget()
        layout = QVBoxLayout(w)
        row = QHBoxLayout()
        self.output_path_edit = QLineEdit()
        self.output_path_edit.textChanged.connect(self._on_output_changed)
        browse_btn = QPushButton("Browse...")
        browse_btn.clicked.connect(self._browse_output)
        row.addWidget(self.output_path_edit)
        row.addWidget(browse_btn)
        layout.addLayout(row)
        layout.addWidget(QLabel(
            "Output is always encoded to <path>.moflex.partial first, then renamed to "
            "<path> only after a successful encode and basic verification."
        ))
        layout.addStretch(1)
        self.tabs.addTab(w, "Output")

    def _browse_output(self) -> None:
        path, _ = QFileDialog.getSaveFileName(self, "Choose output file", filter="MOFLEX (*.moflex)")
        if path:
            if not path.lower().endswith(".moflex"):
                path += ".moflex"
            self.output_path_edit.setText(path)

    def _on_output_changed(self, text: str) -> None:
        self.job.output = OutputSettings(path=text)
        self._refresh_review()

    # ---------- Review tab ----------
    def _build_review_tab(self) -> None:
        w = QWidget()
        layout = QVBoxLayout(w)
        self.review_text = QPlainTextEdit()
        self.review_text.setReadOnly(True)
        layout.addWidget(self.review_text)
        self.tabs.addTab(w, "Review")

    def _refresh_review(self) -> None:
        if not hasattr(self, "review_text"):
            return  # called during tab construction, before the Review tab exists yet
        if not self.job.source or not self.job.output.path:
            self.review_text.setPlainText("Select a source and an output path to see the resolved command.")
            return
        try:
            resolved = resolve_args(self.job)
        except (UnsupportedTargetError, ValueError) as e:
            self.review_text.setPlainText(f"Cannot resolve command yet: {e}")
            return
        lines = [
            f"Source:          {self.job.source.path}",
            f"Picture mode:    {self.job.picture.mode}",
            f"Quality preset:  {self.job.quality.preset} (QP={self.job.quality.qp}, "
            f"QYX={self.job.quality.mobi_qyx}, Q-RDO={self.job.quality.q_rdo}, "
            f"threads={self.job.quality.threads})",
            f"Audio mode:      {self.job.audio.mode}",
            f"Output:          {self.job.output.path}",
            "",
            "Exact resolved ffmpeg command:",
            resolved.display_string(),
        ]
        self.review_text.setPlainText("\n".join(lines))

    # ---------- Encode & Verify tab ----------
    def _build_encode_tab(self) -> None:
        w = QWidget()
        layout = QVBoxLayout(w)

        btn_row = QHBoxLayout()
        self.start_btn = QPushButton("Start Encode")
        self.start_btn.clicked.connect(self._start_encode)
        self.cancel_btn = QPushButton("Cancel")
        self.cancel_btn.setEnabled(False)
        self.cancel_btn.clicked.connect(self._cancel_encode)
        btn_row.addWidget(self.start_btn)
        btn_row.addWidget(self.cancel_btn)
        layout.addLayout(btn_row)

        self.progress_bar = QProgressBar()
        self.progress_bar.setRange(0, 0)  # indeterminate; total frame count not always known up front
        self.progress_bar.setVisible(False)
        layout.addWidget(self.progress_bar)

        self.status_label = QLabel("Idle.")
        layout.addWidget(self.status_label)

        self.output_text = QPlainTextEdit()
        self.output_text.setReadOnly(True)
        layout.addWidget(self.output_text)

        self._encode_timer = QTimer(self)
        self._encode_timer.setInterval(500)
        self._encode_timer.timeout.connect(self._tick_elapsed)

        self.tabs.addTab(w, "Encode && Verify")

    def _start_encode(self) -> None:
        if not self.job.source or not self.job.output.path:
            QMessageBox.warning(self, "Not ready", "Select a source and an output path first.")
            return
        try:
            resolved = resolve_args(self.job)
        except (UnsupportedTargetError, ValueError) as e:
            QMessageBox.critical(self, "Cannot start", str(e))
            return

        if os.path.exists(self.job.output.path):
            reply = QMessageBox.question(
                self, "Overwrite?",
                f"{self.job.output.path} already exists. Overwrite it when the encode completes?",
            )
            if reply != QMessageBox.StandardButton.Yes:
                return

        self.output_text.clear()
        self.output_text.appendPlainText(f"$ {resolved.display_string()}\n")
        self.status_label.setText("Encoding...")
        self.progress_bar.setVisible(True)
        self.start_btn.setEnabled(False)
        self.cancel_btn.setEnabled(True)

        self._encode_process = QProcess(self)
        env = self._encode_process.processEnvironment()
        for k, v in resolved.env.items():
            env.insert(k, v)
        self._encode_process.setProcessEnvironment(env)
        self._encode_process.setProgram(resolved.argv[0])
        self._encode_process.setArguments(resolved.argv[1:])
        self._encode_process.readyReadStandardError.connect(self._read_encode_output)
        self._encode_process.readyReadStandardOutput.connect(self._read_encode_output)
        self._encode_process.finished.connect(self._encode_finished)
        self._encode_started_at = time.time()
        self._encode_timer.start()
        self._encode_process.start()

    def _read_encode_output(self) -> None:
        proc = self._encode_process
        if proc is None:
            return
        chunk = bytes(proc.readAllStandardError()).decode(errors="replace")
        chunk += bytes(proc.readAllStandardOutput()).decode(errors="replace")
        if not chunk:
            return
        self.output_text.appendPlainText(chunk.strip())
        m = _STATS_RE.search(chunk)
        if m:
            self.status_label.setText(
                f"Encoding -- frame {m['frame']} | {m['fps']} fps | speed {m['speed']}x | time {m['time']}"
            )

    def _tick_elapsed(self) -> None:
        if self._encode_started_at is None:
            return
        elapsed = time.time() - self._encode_started_at
        current_text = self.status_label.text()
        # keep the stats info if present, just note elapsed separately in the title bar
        self.setWindowTitle(f"{APP_TITLE} -- elapsed {elapsed:0.0f}s")

    def _cancel_encode(self) -> None:
        if self._encode_process is not None:
            self._user_cancelled = True
            self._encode_process.kill()

    def _encode_finished(self, exit_code: int, _status) -> None:
        self._encode_timer.stop()
        self.setWindowTitle(APP_TITLE)
        self.start_btn.setEnabled(True)
        self.cancel_btn.setEnabled(False)
        self.progress_bar.setVisible(False)

        if exit_code != 0:
            partial = self.job.partial_output_path
            if self._user_cancelled:
                msg = "Cancelled."
            else:
                msg = f"Encode failed (exit code {exit_code})."
            if os.path.exists(partial):
                # Never auto-delete .partial -- on cancellation this is a
                # hard requirement (the partial must remain and must never
                # be presented as complete); on a genuine failure it's also
                # useful to inspect rather than silently discard. The final
                # output path is never touched here either way -- only an
                # explicit successful rename (below) ever produces it.
                msg += f" Partial file retained at {partial}"
            self.status_label.setText(msg)
            self._user_cancelled = False
            return

        try:
            os.replace(self.job.partial_output_path, self.job.output.path)
        except OSError as e:
            self.status_label.setText(f"Encode finished but rename failed: {e}")
            return

        self.status_label.setText("Encode complete. Verifying...")
        expected = self.job.source.video_frame_count if self.job.source else None
        result = verify(self.job.output.path, check_decode=True, expected_frame_count=expected)
        summary = (
            f"exists={result.exists}  size={result.size_bytes} bytes\n"
            f"sha256={result.sha256}\n"
            f"decodes_cleanly={result.decodes_cleanly}  frames={result.decoded_frame_count}  "
            f"expected={result.expected_frame_count}  matches={result.frame_count_matches}\n"
        )
        if result.decode_error:
            summary += f"decode_error={result.decode_error}\n"
        self.output_text.appendPlainText("\n--- verification ---\n" + summary)
        self.status_label.setText(
            "Verification passed (software only -- not emulator or hardware tested)."
            if result.passed else "Verification FAILED -- see output above."
        )
