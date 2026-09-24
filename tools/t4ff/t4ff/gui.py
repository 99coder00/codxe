"""A small window for converting usermaps (tkinter, part of the Python standard library).

    python -m t4ff gui          (or double click t4ff_gui.pyw on Windows)

The window runs the same conversion as ``python -m t4ff convert`` in a background
thread and shows its output. Settings are remembered between runs.
"""

from __future__ import annotations

import contextlib
import io
import json
import os
import queue
import subprocess
import sys
import threading
import traceback
from dataclasses import asdict, dataclass, field
from typing import List

SOUND_RATES = ("keep", "48000", "44100", "32000", "24000")
STREAM_RATES = ("keep", "44100", "32000", "24000", "22050")
TEXTURE_SIZES = ("no limit", "2048", "1024", "512", "256")


@dataclass
class Settings:
    input: str = ""
    output: str = ""
    xma_encoder: str = ""
    console_zones: List[str] = field(default_factory=list)
    iwds: List[str] = field(default_factory=list)
    texture_budget: float = 0.0
    max_texture_size: int = 0
    sound_rate: int = 0
    stream_rate: int = 0
    xma_quality: int = 60
    mono_sounds: bool = False
    mono_streams: bool = False
    no_mips: bool = False
    no_compress: bool = False
    no_mod: bool = False
    no_patch: bool = False
    load_zone: bool = False
    no_sounds: bool = False

    @classmethod
    def load(cls, path: str) -> "Settings":
        try:
            with open(path, "r", encoding="utf-8") as f:
                data = json.load(f)
        except (OSError, ValueError):
            return cls()
        known = {k: v for k, v in data.items() if k in cls.__dataclass_fields__}
        try:
            return cls(**known)
        except TypeError:
            return cls()

    def save(self, path: str):
        try:
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "w", encoding="utf-8") as f:
                json.dump(asdict(self), f, indent=2)
        except OSError:
            pass


def settings_path() -> str:
    base = os.environ.get("APPDATA") or os.path.join(os.path.expanduser("~"), ".config")
    return os.path.join(base, "t4ff", "gui.json")


def convert_args(s: Settings) -> List[str]:
    """Command line of ``python -m t4ff`` for these settings."""
    args = ["convert", s.input, "-o", s.output]
    if s.xma_encoder:
        args += ["--xma-encoder", s.xma_encoder]
    for path in s.console_zones:
        args += ["--console-zone", path]
    for path in s.iwds:
        args += ["--iwd", path]
    if s.texture_budget:
        args += ["--texture-budget", f"{s.texture_budget:g}"]
    if s.max_texture_size:
        args += ["--max-texture-size", str(s.max_texture_size)]
    if s.sound_rate:
        args += ["--sound-rate", str(s.sound_rate)]
    if s.stream_rate:
        args += ["--stream-rate", str(s.stream_rate)]
    if s.xma_quality != 60:
        args += ["--xma-quality", str(s.xma_quality)]
    for flag in ("mono_sounds", "mono_streams", "no_mips", "no_compress", "no_mod", "no_patch", "load_zone", "no_sounds"):
        if getattr(s, flag):
            args.append("--" + flag.replace("_", "-"))
    return args


def check_settings(s: Settings) -> List[str]:
    """Problems that prevent a conversion (empty when it can run)."""
    problems = []
    if not s.input:
        problems.append("Choose the PC usermap folder (or its map .ff).")
    elif not os.path.exists(s.input):
        problems.append(f"The usermap folder does not exist: {s.input}")
    if not s.output:
        problems.append("Choose an output folder.")
    for path in s.console_zones + s.iwds + ([s.xma_encoder] if s.xma_encoder else []):
        if not os.path.exists(path):
            problems.append(f"Not found: {path}")
    return problems


def advice(s: Settings) -> List[str]:
    """Hints printed before a conversion."""
    notes = []
    if not s.console_zones:
        notes.append("No Xbox 360 fastfiles given: technique sets (shaders) and stock assets are only referenced by name.")
    if not s.xma_encoder and not s.no_sounds:
        notes.append("No xma2encode.exe given: sounds are not encoded (loaded sounds come from the 360 fastfiles, if they have them).")
    return notes


class _QueueWriter(io.TextIOBase):
    def __init__(self, q: "queue.Queue[str]"):
        self.q = q

    def write(self, text: str) -> int:
        if text:
            self.q.put(text)
        return len(text)

    def flush(self):
        pass


def run_captured(func, q: "queue.Queue[str]"):
    """Call ``func()`` in this thread, sending what it prints to ``q``. None when it fails."""
    writer = _QueueWriter(q)
    with contextlib.redirect_stdout(writer), contextlib.redirect_stderr(writer):
        try:
            return func()
        except SystemExit as e:
            if e.code not in (0, None):
                print(f"error: {e.code}" if not isinstance(e.code, int) else f"error: exit code {e.code}")
                return False
            return True
        except Exception:
            traceback.print_exc()
            return None


def run_cli(args: List[str], q: "queue.Queue[str]") -> bool:
    """Run ``python -m t4ff <args>`` in this thread, sending its output to ``q``."""

    def cli():
        from .__main__ import main as cli_main

        return (cli_main(args) or 0) == 0

    return bool(run_captured(cli, q))


def find_and_install_encoder():
    """Worker: the encoder found on this computer, installed into tools/t4ff/bin (None if absent)."""
    from . import deps

    found = deps.find_xma2encode(search_zips=True)
    if not found:
        print("note: xma2encode.exe was not found on this computer. " + deps.ENCODER_HELP)
        return None
    installed = os.path.join(deps.BIN_DIR, deps.ENCODER_NAME)
    if "::" in found or os.path.abspath(found) != os.path.abspath(installed):
        found = deps.install_xma2encode(found)
    print(f"note: using {found}")
    return found


def open_folder(path: str):
    if sys.platform.startswith("win"):
        os.startfile(path)  # type: ignore[attr-defined]
    elif sys.platform == "darwin":
        subprocess.Popen(["open", path])
    else:
        subprocess.Popen(["xdg-open", path])


# ---------------------------------------------------------------------------
# Window


def main():
    import tkinter as tk
    from tkinter import filedialog, messagebox, ttk
    from tkinter.scrolledtext import ScrolledText

    if sys.stdout is None:  # pythonw
        sys.stdout = sys.stderr = open(os.devnull, "w")

    path = settings_path()
    settings = Settings.load(path)

    root = tk.Tk()
    root.title("t4ff - World at War PC to Xbox 360 usermap converter")
    root.minsize(760, 640)
    pad = {"padx": 6, "pady": 3}

    var = {
        "input": tk.StringVar(value=settings.input),
        "output": tk.StringVar(value=settings.output),
        "xma_encoder": tk.StringVar(value=settings.xma_encoder),
        "texture_budget": tk.StringVar(value=f"{settings.texture_budget:g}"),
        "max_texture_size": tk.StringVar(value=str(settings.max_texture_size or TEXTURE_SIZES[0])),
        "sound_rate": tk.StringVar(value=str(settings.sound_rate or SOUND_RATES[0])),
        "stream_rate": tk.StringVar(value=str(settings.stream_rate or STREAM_RATES[0])),
        "xma_quality": tk.StringVar(value=str(settings.xma_quality)),
    }
    flags = {
        name: tk.BooleanVar(value=getattr(settings, name))
        for name in ("mono_sounds", "mono_streams", "no_mips", "no_compress", "no_mod", "no_patch", "load_zone", "no_sounds")
    }

    # -- paths ----------------------------------------------------------------
    files = ttk.LabelFrame(root, text="Usermap")
    files.pack(fill="x", **pad)
    files.columnconfigure(1, weight=1)

    def path_row(frame, row, label, key, browse):
        ttk.Label(frame, text=label).grid(row=row, column=0, sticky="w", **pad)
        ttk.Entry(frame, textvariable=var[key]).grid(row=row, column=1, sticky="ew", **pad)
        buttons = ttk.Frame(frame)
        buttons.grid(row=row, column=2, sticky="e")
        for text, command in browse:
            ttk.Button(buttons, text=text, command=command).pack(side="left", padx=2)

    def ask_dir(key, title):
        def pick():
            chosen = filedialog.askdirectory(title=title, initialdir=var[key].get() or None)
            if chosen:
                var[key].set(chosen)

        return pick

    def ask_file(key, title, types):
        def pick():
            chosen = filedialog.askopenfilename(title=title, filetypes=types)
            if chosen:
                var[key].set(chosen)

        return pick

    path_row(
        files,
        0,
        "PC usermap",
        "input",
        [("Folder...", ask_dir("input", "PC usermap folder (with <map>.ff, mod.ff, .iwd)")), ("File...", ask_file("input", "PC map fastfile", [("Fastfiles", "*.ff")]))],
    )
    path_row(files, 1, "Output folder", "output", [("Folder...", ask_dir("output", "Output folder (a _codxe folder is created inside)"))])
    path_row(files, 2, "xma2encode.exe", "xma_encoder", [("File...", ask_file("xma_encoder", "xma2encode.exe (Xbox 360 XDK)", [("Programs", "*.exe"), ("All files", "*")]))])

    # -- lists ----------------------------------------------------------------
    lists = ttk.Frame(root)
    lists.pack(fill="x", **pad)
    lists.columnconfigure(0, weight=1)
    lists.columnconfigure(1, weight=1)

    def path_list(column, title, items, add_files, add_folder, hint):
        frame = ttk.LabelFrame(lists, text=title)
        frame.grid(row=0, column=column, sticky="nsew", padx=3)
        box = tk.Listbox(frame, height=5, selectmode="extended", exportselection=False)
        box.pack(fill="both", expand=True, padx=4, pady=(4, 0))
        for item in items:
            box.insert("end", item)
        ttk.Label(frame, text=hint, foreground="gray").pack(anchor="w", padx=4)
        buttons = ttk.Frame(frame)
        buttons.pack(fill="x", padx=2, pady=3)

        def add(paths):
            existing = set(box.get(0, "end"))
            for p in paths:
                if p and p not in existing:
                    box.insert("end", p)

        ttk.Button(buttons, text="Add files...", command=lambda: add(filedialog.askopenfilenames(title=title, filetypes=add_files))).pack(side="left", padx=2)
        if add_folder:
            ttk.Button(buttons, text="Add folder...", command=lambda: add([filedialog.askdirectory(title=title)])).pack(side="left", padx=2)

        def remove():
            for index in reversed(box.curselection()):
                box.delete(index)

        ttk.Button(buttons, text="Remove", command=remove).pack(side="left", padx=2)
        return box

    zones_box = path_list(
        0,
        "Xbox 360 fastfiles (technique sets, stock assets)",
        settings.console_zones,
        [("Fastfiles", "*.ff"), ("All files", "*")],
        True,
        "Stock zones of your 360 game, or CoD Xenon converted maps",
    )
    iwds_box = path_list(
        1,
        "Extra PC .iwd files / folders",
        settings.iwds,
        [("IWD archives", "*.iwd"), ("All files", "*")],
        True,
        "e.g. the PC game's main folder, for stock textures",
    )

    # -- options --------------------------------------------------------------
    options = ttk.LabelFrame(root, text="Memory and quality")
    options.pack(fill="x", **pad)

    def option(row, column, label, widget):
        ttk.Label(options, text=label).grid(row=row, column=column, sticky="w", **pad)
        widget.grid(row=row, column=column + 1, sticky="w", **pad)

    option(0, 0, "Texture budget (MiB, 0 = none)", ttk.Spinbox(options, from_=0, to=512, increment=8, width=8, textvariable=var["texture_budget"]))
    option(0, 2, "Max texture size", ttk.Combobox(options, values=TEXTURE_SIZES, width=10, state="readonly", textvariable=var["max_texture_size"]))
    option(1, 0, "Loaded sound rate (Hz)", ttk.Combobox(options, values=SOUND_RATES, width=10, state="readonly", textvariable=var["sound_rate"]))
    option(1, 2, "Streamed sound rate (Hz)", ttk.Combobox(options, values=STREAM_RATES, width=10, state="readonly", textvariable=var["stream_rate"]))
    option(2, 0, "XMA quality (1-100)", ttk.Spinbox(options, from_=1, to=100, width=8, textvariable=var["xma_quality"]))

    checks = ttk.Frame(options)
    checks.grid(row=3, column=0, columnspan=4, sticky="w", **pad)
    for i, (name, text) in enumerate(
        [
            ("mono_sounds", "Mono loaded sounds"),
            ("mono_streams", "Mono streamed sounds"),
            ("no_mips", "No mip maps"),
            ("no_compress", "Keep uncompressed textures"),
            ("no_mod", "Skip mod.ff"),
            ("no_patch", "Skip _patch.ff"),
            ("no_sounds", "Skip sounds"),
            ("load_zone", "Write _load.ff (loading screen, experimental)"),
        ]
    ):
        ttk.Checkbutton(checks, text=text, variable=flags[name]).grid(row=i // 4, column=i % 4, sticky="w", padx=6, pady=1)

    # -- actions and log -------------------------------------------------------
    actions = ttk.Frame(root)
    actions.pack(fill="x", **pad)
    convert_button = ttk.Button(actions, text="Convert")
    convert_button.pack(side="left", padx=2)
    inspect_button = ttk.Button(actions, text="Inspect fastfile...")
    inspect_button.pack(side="left", padx=2)
    open_button = ttk.Button(actions, text="Open output folder")
    open_button.pack(side="left", padx=2)
    setup_button = ttk.Button(actions, text="Set up dependencies")
    setup_button.pack(side="left", padx=2)
    progress = ttk.Progressbar(actions, mode="determinate", length=160)
    progress.pack(side="right", padx=4)
    status = ttk.Label(actions, text="Ready")
    status.pack(side="right", padx=6)

    log = ScrolledText(root, height=14, wrap="char", state="disabled", font=("Consolas", 9) if sys.platform.startswith("win") else ("TkFixedFont", 9))
    log.pack(fill="both", expand=True, **pad)
    log.tag_configure("warning", foreground="#9a6700")
    log.tag_configure("error", foreground="#b42318")
    log.tag_configure("note", foreground="#175cd3")

    output_queue: "queue.Queue[str]" = queue.Queue()
    state = {"running": False, "pending": ""}

    def append(text: str, tag: str = ""):
        log.configure(state="normal")
        log.insert("end", text, tag)
        log.see("end")
        log.configure(state="disabled")

    def current_settings() -> Settings:
        def number(key, cast, default):
            text = var[key].get().strip()
            try:
                return cast(text)
            except ValueError:
                return default

        return Settings(
            input=var["input"].get().strip(),
            output=var["output"].get().strip(),
            xma_encoder=var["xma_encoder"].get().strip(),
            console_zones=list(zones_box.get(0, "end")),
            iwds=list(iwds_box.get(0, "end")),
            texture_budget=number("texture_budget", float, 0.0),
            max_texture_size=number("max_texture_size", int, 0),
            sound_rate=number("sound_rate", int, 0),
            stream_rate=number("stream_rate", int, 0),
            xma_quality=max(1, min(100, number("xma_quality", int, 60))),
            **{name: flag.get() for name, flag in flags.items()},
        )

    def set_running(running: bool, text: str):
        state["running"] = running
        for button in (convert_button, inspect_button, setup_button):
            button.configure(state="disabled" if running else "normal")
        status.configure(text=text)
        if running:
            progress.configure(mode="indeterminate")
            progress.start(12)
        else:
            progress.stop()
            progress.configure(mode="determinate", value=0)

    def start_task(label: str, func, on_done=None, quiet: bool = False):
        """Run ``func`` in a worker thread; ``on_done(result)`` runs in the window afterwards."""
        state["memory"] = ""
        set_running(True, label)

        def work():
            result = run_captured(func, output_queue)
            output_queue.put(("done", result, on_done, quiet))

        threading.Thread(target=work, daemon=True).start()

    def start(args: List[str], label: str, on_done=None):
        append("\n$ python -m t4ff " + " ".join(f'"{a}"' if " " in a else a for a in args) + "\n", "note")
        start_task(label, lambda: run_cli(args, output_queue), on_done)

    def poll():
        try:
            while True:
                item = output_queue.get_nowait()
                if isinstance(item, tuple):
                    _, result, on_done, quiet = item
                    ok = result not in (None, False)
                    if state["pending"]:
                        flush_line(state["pending"])
                        state["pending"] = ""
                    if quiet:
                        set_running(False, "Ready")
                    else:
                        summary = f"Done: the map needs {state['memory']}" if ok and state.get("memory") else "Done" if ok else "Failed, see the log"
                        set_running(False, summary)
                        append(("Finished.\n" if ok else "Failed.\n"), "note" if ok else "error")
                    if on_done is not None:
                        on_done(result)
                    continue
                text = state["pending"] + item
                lines = text.split("\n")
                state["pending"] = lines.pop()
                for line in lines:
                    flush_line(line + "\n")
        except queue.Empty:
            pass
        root.after(100, poll)

    def flush_line(line: str):
        if line.startswith("  memory:") and not state.get("memory"):
            state["memory"] = line.split(":", 1)[1].split("(")[0].strip()  # the first zone written is the map
        lower = line.lower()
        tag = "error" if lower.startswith(("error", "traceback")) or "error:" in lower[:40] else "warning" if lower.startswith("warning") else ""
        append(line, tag)

    def convert():
        s = current_settings()
        problems = check_settings(s)
        if problems:
            messagebox.showwarning("t4ff", "\n".join(problems))
            return
        s.save(path)
        for note in advice(s):
            append("note: " + note + "\n", "note")

        def done(ok):
            if ok:
                target = os.path.join(s.output, "_codxe")
                messagebox.showinfo("t4ff", f"Conversion finished.\n\nCopy {target} into the World at War game folder on the console.")

        start(convert_args(s), "Converting...", done)

    def inspect():
        chosen = filedialog.askopenfilename(title="Fastfile to inspect (PC or Xbox 360)", filetypes=[("Fastfiles", "*.ff"), ("All files", "*")])
        if chosen:
            start(["info", chosen], "Reading...")

    def open_output():
        s = current_settings()
        target = os.path.join(s.output, "_codxe") if s.output else ""
        if target and not os.path.isdir(target):
            target = s.output
        if target and os.path.isdir(target):
            open_folder(target)
        else:
            messagebox.showinfo("t4ff", "The output folder does not exist yet.")

    def encoder_found(found):
        if found:
            var["xma_encoder"].set(found)
            current_settings().save(path)

    def set_up():
        """Install missing Python packages, find / install / test xma2encode.exe."""
        source = var["xma_encoder"].get().strip() or None
        append("\nSetting up dependencies...\n", "note")

        def work():
            from . import deps

            return deps.setup(source)

        def done(result):
            if not result:
                return
            encoder_found(result.get("xma2encode"))
            lines = [
                "Python packages: " + ("installed" if result.get("python") else "missing"),
                "FFmpeg: " + ("found" if result.get("ffmpeg") else "missing"),
                "xma2encode.exe: " + ("installed and working" if result.get("xma2encode") else "not found (sounds will not be encoded)"),
            ]
            if not sys.platform.startswith("win"):
                lines.append("wine: " + ("found" if result.get("wine") else "missing"))
            messagebox.showinfo("t4ff", "\n".join(lines) + ("" if result.get("xma2encode") else "\n\nxma2encode.exe comes with Microsoft's Xbox developer kits and cannot be downloaded automatically. Put it (or a .zip with it) in your Downloads folder, or pick it with File..., then set up again."))

        start_task("Setting up...", work, done)

    def startup():
        """Offer to install missing Python packages, then look for xma2encode.exe."""
        from . import deps

        missing = deps.missing_python_packages()
        encoder = var["xma_encoder"].get().strip()
        need_encoder = not encoder or not os.path.exists(encoder)
        if missing:
            if messagebox.askyesno("t4ff", f"t4ff needs these Python packages: {', '.join(missing)}.\n\nInstall them now?"):
                append(f"\nInstalling {', '.join(missing)}...\n", "note")

                def work():
                    if not deps.install_python_packages(missing):
                        return None
                    return find_and_install_encoder() if need_encoder else encoder

                start_task("Installing...", work, encoder_found)
            else:
                append("The packages are needed to convert. Use Set up dependencies to install them later.\n", "warning")
            return
        if need_encoder:
            start_task("Looking for xma2encode.exe...", find_and_install_encoder, encoder_found, quiet=True)

    convert_button.configure(command=convert)
    inspect_button.configure(command=inspect)
    open_button.configure(command=open_output)
    setup_button.configure(command=set_up)

    def close():
        if state["running"] and not messagebox.askyesno("t4ff", "A conversion is running. Quit anyway?"):
            return
        current_settings().save(path)
        root.destroy()

    root.protocol("WM_DELETE_WINDOW", close)
    append("Choose the PC usermap folder, an output folder and the Xbox 360 fastfiles to take shaders from, then Convert.\n", "note")
    root.after(100, poll)
    root.after(300, startup)
    root.mainloop()


if __name__ == "__main__":
    main()
