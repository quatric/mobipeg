#!/usr/bin/env python3
"""Tk widgets used by encode_gui.py that are big enough to live on their own.

  ScrollableFrame  a vertically scrolling container for the long Encode form
  PathListEditor   a small add/remove list of file paths (Super MOFLEX subtitles)
  QueueTab         the batch queue tab, backed by mobipeg_tools.JobQueue
  InspectTab       the MOFLEX preflight tab, backed by `encode.py inspect`

Nothing here touches ffmpeg directly; the tabs call back into the app
(`start_process`, `build_encode_cmd`, ...), so the one place that spawns
processes stays EncodeGUI.
"""

import os
import sys
import tkinter as tk
from tkinter import ttk, filedialog, messagebox

import mobipeg_tools as mt


def enable_file_drop(root, targets):
    """Let files be dropped on pages: targets = [(page, callback(paths), skip)].

    Every widget inside a page except the `skip` subtrees accepts the drop, and
    only existing files are passed on. Needs the optional tkinterdnd2 package;
    without it (or if it cannot load) nothing happens and False is returned.
    """
    try:
        from tkinterdnd2 import TkinterDnD, DND_FILES

        TkinterDnD._require(root)
    except Exception:
        return False

    def register(widget, callback, skip):
        if widget in skip:
            return

        def on_drop(event):
            paths = [p for p in root.tk.splitlist(event.data) if os.path.isfile(p)]
            if paths:
                callback(paths)
            return event.action

        try:
            widget.drop_target_register(DND_FILES)
            widget.dnd_bind("<<Drop>>", on_drop)
        except Exception:
            pass
        for child in widget.winfo_children():
            register(child, callback, skip)

    for page, callback, skip in targets:
        register(page, callback, skip)
    return True


class FlatTabs(ttk.Frame):
    """Tab strip with square, flat tabs and an underline on the selected one.

    ttk.Notebook on macOS draws native rounded tabs that cannot be restyled,
    so this stands in for it. It offers the three calls the app uses:
    add(page, text=...), select(page) and the usual winfo_reqheight().
    """

    ACCENT = "#2f7fd8"

    def __init__(self, parent):
        super().__init__(parent)
        self.columnconfigure(0, weight=1)
        self.rowconfigure(2, weight=1)
        self._bg = ttk.Style().lookup("TFrame", "background") or "#ececec"
        self.bar = ttk.Frame(self)
        self.bar.grid(row=0, column=0, sticky="w")
        ttk.Separator(self).grid(row=1, column=0, sticky="ew")
        self._tabs = []

    def add(self, page, text=""):
        cell = ttk.Frame(self.bar)
        cell.pack(side="left")
        label = ttk.Label(cell, text=text, padding=(14, 6))
        label.pack()
        line = tk.Frame(cell, height=3, bg=self._bg)
        line.pack(fill="x")
        label.bind("<Button-1>", lambda _e, p=page: self.select(p))
        self._tabs.append((page, label, line))
        page.grid(row=2, column=0, sticky="nsew")
        self.select(page) if len(self._tabs) == 1 else page.grid_remove()

    def select(self, page):
        for p, label, line in self._tabs:
            on = p is page
            label.configure(foreground="" if on else "grey")
            line.configure(bg=self.ACCENT if on else self._bg)
            p.grid() if on else p.grid_remove()


class ScrollableFrame(ttk.Frame):
    """A frame whose `inner` child scrolls vertically inside a fixed viewport."""

    def __init__(self, parent, height=560):
        super().__init__(parent)
        self.canvas = tk.Canvas(self, highlightthickness=0, height=height, borderwidth=0)
        self.vbar = ttk.Scrollbar(self, orient="vertical", command=self.canvas.yview)
        self.canvas.configure(yscrollcommand=self.vbar.set)
        self.canvas.pack(side="left", fill="both", expand=True)
        self.vbar.pack(side="right", fill="y")
        self.inner = ttk.Frame(self.canvas, padding=10)
        self._window = self.canvas.create_window((0, 0), window=self.inner, anchor="nw")
        self.inner.bind("<Configure>", self._on_inner)
        self.canvas.bind("<Configure>", self._on_canvas)
        for w in (self.canvas, self.inner):
            w.bind("<Enter>", self._bind_wheel)
            w.bind("<Leave>", self._unbind_wheel)

    def _on_inner(self, _event=None):
        self.canvas.configure(scrollregion=self.canvas.bbox("all"))
        self._sync_bar()

    def _on_canvas(self, event):
        self.canvas.itemconfigure(self._window, width=event.width)
        self._sync_bar()

    def _sync_bar(self):
        # Hide the scrollbar when everything fits, so short formats look as
        # they always did.
        try:
            need = self.inner.winfo_reqheight() > self.canvas.winfo_height() > 1
        except tk.TclError:
            return
        if need and not self.vbar.winfo_ismapped():
            self.vbar.pack(side="right", fill="y")
        elif not need and self.vbar.winfo_ismapped():
            self.vbar.pack_forget()
            self.canvas.yview_moveto(0)

    def _bind_wheel(self, _event=None):
        self.canvas.bind_all("<MouseWheel>", self._on_wheel)
        self.canvas.bind_all("<Button-4>", self._on_wheel)
        self.canvas.bind_all("<Button-5>", self._on_wheel)

    def _unbind_wheel(self, _event=None):
        for seq in ("<MouseWheel>", "<Button-4>", "<Button-5>"):
            self.canvas.unbind_all(seq)

    def _on_wheel(self, event):
        if not self.vbar.winfo_ismapped():
            return
        if event.num == 4:
            step = -1
        elif event.num == 5:
            step = 1
        elif sys.platform == "darwin":
            step = -1 if event.delta > 0 else 1
        else:
            step = -1 if event.delta > 0 else 1
        self.canvas.yview_scroll(step * 2, "units")


class PathListEditor(ttk.Frame):
    """Listbox of file paths with Add / Remove."""

    def __init__(self, parent, title, filetypes, height=3):
        super().__init__(parent)
        self.filetypes = filetypes
        self.title = title
        self.columnconfigure(0, weight=1)
        self.listbox = tk.Listbox(self, height=height, exportselection=False)
        self.listbox.grid(row=0, column=0, rowspan=2, sticky="ew")
        self.add_btn = ttk.Button(self, text="Add...", command=self.add)
        self.add_btn.grid(row=0, column=1, padx=(5, 0), sticky="ew")
        self.rm_btn = ttk.Button(self, text="Remove", command=self.remove)
        self.rm_btn.grid(row=1, column=1, padx=(5, 0), sticky="ew")

    def add(self):
        for path in filedialog.askopenfilenames(title=self.title, filetypes=self.filetypes):
            if path not in self.get():
                self.listbox.insert(tk.END, path)

    def remove(self):
        for i in reversed(self.listbox.curselection()):
            self.listbox.delete(i)

    def get(self):
        return list(self.listbox.get(0, tk.END))

    def set(self, paths):
        # A disabled Listbox silently ignores insert/delete, so open it up
        # for the edit and put its state back afterwards.
        state = str(self.listbox.cget("state"))
        self.listbox.config(state="normal")
        self.listbox.delete(0, tk.END)
        for p in paths:
            self.listbox.insert(tk.END, p)
        self.listbox.config(state=state)

    def set_enabled(self, enabled):
        state = "normal" if enabled else "disabled"
        self.listbox.config(state=state)
        self.add_btn.config(state=state)
        self.rm_btn.config(state=state)


STATUS_LABELS = {
    mt.QUEUE_PENDING: "waiting",
    mt.QUEUE_RUNNING: "encoding...",
    mt.QUEUE_DONE: "done",
    mt.QUEUE_FAILED: "failed",
    mt.QUEUE_STOPPED: "stopped",
}


class QueueTab(ttk.Frame):
    """Batch conversions: add files, they run one after another."""

    def __init__(self, parent, app):
        super().__init__(parent, padding=10)
        self.app = app
        self.store = mt.JsonStore("queue.json")
        self.queue = mt.JobQueue.from_list(self.store.load(default=[]))
        self.running = False
        self.stop_requested = False
        self.columnconfigure(0, weight=1)
        self.rowconfigure(1, weight=1)

        ttk.Label(
            self,
            foreground="grey",
            wraplength=640,
            text="Each file is converted with the Encode tab's settings as they are when "
            "you add it. Files run one at a time; the queue is kept if you close the app.",
        ).grid(row=0, column=0, sticky="w", pady=(0, 6))

        cols = ("file", "format", "status", "output")
        self.tree = ttk.Treeview(self, columns=cols, show="headings", selectmode="extended", height=8)
        for name, text, width in (
            ("file", "Input", 230),
            ("format", "Format", 80),
            ("status", "Status", 110),
            ("output", "Output", 230),
        ):
            self.tree.heading(name, text=text)
            self.tree.column(name, width=width, anchor="w")
        self.tree.grid(row=1, column=0, sticky="nsew")
        sb = ttk.Scrollbar(self, orient="vertical", command=self.tree.yview)
        sb.grid(row=1, column=1, sticky="ns")
        self.tree.configure(yscrollcommand=sb.set)

        bar = ttk.Frame(self)
        bar.grid(row=2, column=0, columnspan=2, sticky="ew", pady=(8, 0))
        self.add_btn = ttk.Button(bar, text="Add files...", command=self.add_files)
        self.add_btn.pack(side="left")
        ttk.Button(bar, text="Add Encode-tab input", command=self.add_current).pack(side="left", padx=4)
        ttk.Button(bar, text="Remove", command=self.remove_selected).pack(side="left", padx=4)
        ttk.Button(bar, text="Up", width=4, command=lambda: self.move(-1)).pack(side="left")
        ttk.Button(bar, text="Down", width=5, command=lambda: self.move(1)).pack(side="left", padx=(2, 4))
        ttk.Button(bar, text="Clear finished", command=self.clear_finished).pack(side="left")
        ttk.Button(bar, text="Retry failed", command=self.retry_failed).pack(side="left", padx=4)

        run = ttk.Frame(self)
        run.grid(row=3, column=0, columnspan=2, sticky="ew", pady=(8, 0))
        self.run_btn = ttk.Button(run, text="▶ Start queue", command=self.start)
        self.run_btn.pack(side="left")
        self.summary = ttk.Label(run, text="", foreground="grey")
        self.summary.pack(side="left", padx=10)

        self.refresh()

    # ----- model <-> view -------------------------------------------------

    def save(self):
        self.store.save(self.queue.to_list())

    def refresh(self):
        selected = set(self.tree.selection())
        self.tree.delete(*self.tree.get_children())
        for j in self.queue.jobs:
            status = STATUS_LABELS.get(j["status"], j["status"])
            if j["detail"]:
                status += f" ({j['detail']})"
            self.tree.insert(
                "", "end", iid=str(j["id"]),
                values=(os.path.basename(j["label"]), j["fmt"], status, os.path.basename(j["output"])),
            )
        keep = [i for i in selected if self.tree.exists(i)]
        if keep:
            self.tree.selection_set(keep)
        c = self.queue.counts()
        parts = [f"{c[k]} {STATUS_LABELS[k]}" for k in
                 (mt.QUEUE_PENDING, mt.QUEUE_RUNNING, mt.QUEUE_DONE, mt.QUEUE_FAILED, mt.QUEUE_STOPPED)
                 if c.get(k)]
        self.summary.config(text=", ".join(parts) if parts else "queue is empty")
        self.save()

    def selected_ids(self):
        return [int(i) for i in self.tree.selection()]

    # ----- editing ----------------------------------------------------------

    def _unique_output(self, path, taken):
        base, ext = os.path.splitext(path)
        n, cand = 2, path
        while cand in taken or os.path.exists(cand):
            cand = f"{base}_{n}{ext}"
            n += 1
        return cand

    def add_paths(self, paths):
        taken = {j["output"] for j in self.queue.jobs}
        added, skipped = 0, []
        for p in paths:
            out = self.app.derived_output_for(p)
            if out is None:
                skipped.append(p)
                continue
            out = self._unique_output(out, taken)
            cmd = self.app.build_encode_cmd(p, "", out)
            if cmd is None:
                skipped.append(p)
                continue
            taken.add(out)
            self.queue.add(cmd, p, out, fmt=self.app.current_format())
            added += 1
        self.refresh()
        if skipped:
            messagebox.showwarning(
                "Queue",
                "Skipped (formats that need a second input file cannot be queued):\n"
                + "\n".join(os.path.basename(s) for s in skipped[:10]),
            )
        return added

    def add_files(self):
        paths = filedialog.askopenfilenames(title="Add files to the queue")
        if paths:
            self.add_paths(list(paths))

    def add_current(self):
        p = self.app.enc_input_var.get().strip()
        if not p or not os.path.isfile(p):
            messagebox.showwarning("Queue", "Choose an input file on the Encode tab first.")
            return
        self.add_paths([p])

    def remove_selected(self):
        self.queue.remove(self.selected_ids())
        self.refresh()

    def move(self, delta):
        self.queue.move(self.selected_ids(), delta)
        self.refresh()

    def clear_finished(self):
        self.queue.clear_finished()
        self.refresh()

    def retry_failed(self):
        self.queue.retry_failed()
        self.refresh()

    # ----- running ------------------------------------------------------------

    def start(self):
        if self.running:
            self.stop_requested = True
            self.app.stop_process()
            return
        if self.app.busy():
            messagebox.showinfo("Queue", "Another job is running. Wait for it or stop it first.")
            return
        if not self.queue.next_pending():
            messagebox.showinfo("Queue", "Nothing is waiting in the queue.")
            return
        self.running = True
        self.stop_requested = False
        self.run_btn.config(text="■ Stop queue")
        self.app.append_console("")  # make sure the console is live
        self._run_next(first=True)

    def _finish(self):
        self.running = False
        self.run_btn.config(text="▶ Start queue")
        self.refresh()
        self.app.set_status("Queue finished" if not self.stop_requested else "Queue stopped")

    def _run_next(self, first=False):
        job = self.queue.next_pending()
        if self.stop_requested or job is None:
            self._finish()
            return
        self.queue.set_status(job["id"], mt.QUEUE_RUNNING)
        self.refresh()
        n_left = self.queue.counts().get(mt.QUEUE_PENDING, 0)

        def done(code, stopped):
            if stopped or self.stop_requested:
                self.queue.set_status(job["id"], mt.QUEUE_STOPPED)
                self.stop_requested = True
            elif code == 0:
                self.queue.set_status(job["id"], mt.QUEUE_DONE)
            else:
                self.queue.set_status(job["id"], mt.QUEUE_FAILED, f"exit {code}")
            self.refresh()
            self.after(50, self._run_next)

        self.app.start_process(
            job["cmd"],
            buttons=[],
            on_done=done,
            clear=first,
            title=f"{os.path.basename(job['label'])}  ({n_left} more after this)",
            probe_input=job["label"],
            separator=True,
        )


class InspectTab(ttk.Frame):
    """Read-only MOFLEX preflight: structure, streams, FAT32 limit, optional decode."""

    def __init__(self, parent, app):
        super().__init__(parent, padding=10)
        self.app = app
        self.columnconfigure(1, weight=1)

        ttk.Label(self, text="MOFLEX file:").grid(row=0, column=0, sticky="e", padx=5, pady=5)
        self.input_var = tk.StringVar()
        ttk.Entry(self, textvariable=self.input_var).grid(row=0, column=1, sticky="ew", padx=5, pady=5)
        ttk.Button(
            self, text="Browse...",
            command=lambda: app.browse_file(
                self.input_var, [("MobiClip MOFLEX", "*.moflex"), ("All files", "*.*")]
            ),
        ).grid(row=0, column=2, padx=5, pady=5)

        self.decode_var = tk.BooleanVar(value=False)
        ttk.Checkbutton(
            self, text="Also decode the whole file and report any error (slower)",
            variable=self.decode_var,
        ).grid(row=1, column=1, sticky="w", padx=5)

        ttk.Label(
            self, foreground="grey", wraplength=560,
            text="Checks the container without changing it: block size and sync points, "
            "timestamps, the Super MOFLEX trailer, streams, and whether the file fits on "
            "a FAT32 SD card (under 4 GiB).",
        ).grid(row=2, column=1, columnspan=2, sticky="w", padx=5, pady=(6, 0))

        btns = ttk.Frame(self)
        btns.grid(row=3, column=1, sticky="w", pady=15)
        self.run_btn = ttk.Button(btns, text="▶ Run Check", command=self.run)
        self.run_btn.grid(row=0, column=0, padx=(0, 10))
        self.cia_btn = ttk.Button(btns, text="Package as CIA →", command=self.to_cia)
        self.cia_btn.grid(row=0, column=1)
        self.cia_btn.state(["disabled"])

        self.result = ttk.Label(self, text="", font=("TkDefaultFont", 13, "bold"))
        self.result.grid(row=4, column=1, sticky="w", padx=5)
        self.input_var.trace_add("write", lambda *a: self._reset())

    def _reset(self):
        self.result.config(text="")
        self.cia_btn.state(["disabled"])

    def set_input(self, path):
        self.input_var.set(path)

    def run(self):
        path = self.input_var.get().strip()
        if not path or not os.path.isfile(path):
            messagebox.showwarning("Warning", "Please select a .moflex file to check.")
            return
        cmd = self.app.script_cmd() + ["inspect", path]
        if self.decode_var.get():
            cmd.append("--decode-check")
        self.result.config(text="checking...")

        def done(code, stopped):
            if stopped:
                self.result.config(text="stopped")
                return
            if code == 0:
                self.result.config(text="OK: no problems found", foreground="#2e8b57")
                self.cia_btn.state(["!disabled"])
            elif code == 1:
                self.result.config(text="Problems found (see console)", foreground="#c0392b")
            else:
                self.result.config(text="Could not run the check", foreground="#c0392b")

        self.app.start_process(cmd, buttons=[self.run_btn], on_done=done,
                               title="Checking " + os.path.basename(path))

    def to_cia(self):
        self.app.cia_input_var.set(self.input_var.get().strip())
        self.app.select_tab(self.app.cia_frame)
