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
        self.title("mobipeg")
        self.geometry("750x600")
        self.minsize(600, 450)
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
        
        # Format
        ttk.Label(self.encode_frame, text="Format:").grid(row=0, column=0, sticky="e", padx=5, pady=5)
        self.enc_fmt_var = tk.StringVar(value="Wii Mobiclip .mo")
        self.formats_map = {
            "Wii Mobiclip .mo": "mo",
            "3DS Mobiclip .moflex (2D)": "moflex",
            "3DS Mobiclip .moflex (3D)": "moflex3d",
            "DS Mobiclip .mods": "mods"
        }
        self.enc_fmt_cb = ttk.Combobox(self.encode_frame, textvariable=self.enc_fmt_var, values=list(self.formats_map.keys()), state="readonly", width=25)
        self.enc_fmt_cb.grid(row=0, column=1, sticky="ew", padx=5, pady=5)
        self.enc_fmt_cb.bind("<<ComboboxSelected>>", self.on_enc_format_change)
        
        # Audio Codec
        ttk.Label(self.encode_frame, text="Audio Codec:").grid(row=1, column=0, sticky="e", padx=5, pady=5)
        self.enc_audio_var = tk.StringVar(value="adpcm")
        self.enc_audio_cb = ttk.Combobox(self.encode_frame, textvariable=self.enc_audio_var, values=["adpcm", "fastaudio", "pcm", "vorbis"])
        self.enc_audio_cb.grid(row=1, column=1, sticky="ew", padx=5, pady=5)
        
        # Input 1
        ttk.Label(self.encode_frame, text="Input File:").grid(row=2, column=0, sticky="e", padx=5, pady=5)
        self.enc_input_var = tk.StringVar()
        ttk.Entry(self.encode_frame, textvariable=self.enc_input_var).grid(row=2, column=1, sticky="ew", padx=5, pady=5)
        ttk.Button(self.encode_frame, text="Browse...", command=lambda: self.browse_file(self.enc_input_var)).grid(row=2, column=2, padx=5, pady=5)
        
        # Input 2 (3D Right Eye)
        self.enc_input2_label = ttk.Label(self.encode_frame, text="Right Eye (3D):")
        self.enc_input2_label.grid(row=3, column=0, sticky="e", padx=5, pady=5)
        self.enc_input2_var = tk.StringVar()
        self.enc_input2_entry = ttk.Entry(self.encode_frame, textvariable=self.enc_input2_var)
        self.enc_input2_entry.grid(row=3, column=1, sticky="ew", padx=5, pady=5)
        self.enc_input2_btn = ttk.Button(self.encode_frame, text="Browse...", command=lambda: self.browse_file(self.enc_input2_var))
        self.enc_input2_btn.grid(row=3, column=2, padx=5, pady=5)
        
        # Output Dir
        ttk.Label(self.encode_frame, text="Output Dir:").grid(row=4, column=0, sticky="e", padx=5, pady=5)
        self.enc_outdir_var = tk.StringVar(value="")
        ttk.Entry(self.encode_frame, textvariable=self.enc_outdir_var).grid(row=4, column=1, sticky="ew", padx=5, pady=5)
        ttk.Button(self.encode_frame, text="Browse...", command=lambda: self.browse_dir(self.enc_outdir_var)).grid(row=4, column=2, padx=5, pady=5)
        
        # Scale
        ttk.Label(self.encode_frame, text="Scale (e.g. 384x288):").grid(row=5, column=0, sticky="e", padx=5, pady=5)
        self.enc_scale_var = tk.StringVar()
        ttk.Entry(self.encode_frame, textvariable=self.enc_scale_var).grid(row=5, column=1, sticky="ew", padx=5, pady=5)
        
        # Run Button
        self.enc_run_btn = ttk.Button(self.encode_frame, text="▶ Run Encoding", command=self.run_encoding)
        self.enc_run_btn.grid(row=6, column=1, pady=15)
        
        self.enc_input_var.trace_add("write", lambda *a: self.on_input_changed(self.enc_input_var, self.enc_outdir_var))
        self.on_enc_format_change()

    def setup_decode_tab(self):
        self.decode_frame.columnconfigure(1, weight=1)
        
        # Input File
        ttk.Label(self.decode_frame, text="Input (.mo/.moflex/.mods):").grid(row=0, column=0, sticky="e", padx=5, pady=5)
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
            
    def on_enc_format_change(self, event=None):
        fmt = self.formats_map.get(self.enc_fmt_var.get())
        if fmt == "moflex3d":
            self.enc_input2_entry.config(state="normal")
            self.enc_input2_btn.config(state="normal")
        else:
            self.enc_input2_entry.config(state="disabled")
            self.enc_input2_btn.config(state="disabled")

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
        
        if fmt == "moflex3d" and inp2:
            cmd.append(inp2)
        if scale:
            cmd.extend(["--scale", scale])
        if outdir:
            cmd.extend(["--outdir", outdir])
            
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
