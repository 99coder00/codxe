"""Finding and installing what t4ff needs.

- Python packages (numpy, libclang, imageio-ffmpeg with its FFmpeg build) are
  installed with pip when they are missing.
- ``xma2encode.exe``, the XMA encoder, is part of Microsoft's licensed Xbox
  developer kits (Xbox 360 XDK, Xbox One XDK, Microsoft GDK with Xbox
  extensions) and cannot be downloaded automatically. It is looked up where
  those kits install it (and used from there), in the Downloads folder (also
  inside .zip files) and in ``tools/t4ff/bin``. One found inside a .zip is
  extracted (with the DLLs next to it) into ``tools/t4ff/bin`` where every run
  finds it. Setup checks that it encodes.
- On Linux and macOS the encoder runs through wine, which has to be installed
  with the system's package manager.

This module only uses the standard library at import time so it can run before
the packages are installed.
"""

from __future__ import annotations

import importlib
import os
import shutil
import site
import subprocess
import sys
import tempfile
import zipfile
from typing import Callable, Dict, List, Optional

TOOL_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN_DIR = os.path.join(TOOL_DIR, "bin")
ENCODER_NAME = "xma2encode.exe"

# (module to import, pip package)
PYTHON_PACKAGES = [
    ("numpy", "numpy"),
    ("clang.cindex", "libclang"),
    ("imageio_ffmpeg", "imageio-ffmpeg"),
]

Log = Callable[[str], None]

# Console programs started from the window (which has no console) would each open a console
# window on Windows without this.
NO_WINDOW = {"creationflags": subprocess.CREATE_NO_WINDOW} if sys.platform.startswith("win") else {}


# ---------------------------------------------------------------------------
# Python packages


def missing_python_packages() -> List[str]:
    missing = []
    for module, package in PYTHON_PACKAGES:
        try:
            importlib.import_module(module)
        except Exception:  # ImportError, or a broken install (e.g. libclang without its library)
            missing.append(package)
    return missing


def _pip(args: List[str], log: Log) -> bool:
    cmd = [sys.executable, "-m", "pip"] + args
    log("$ " + " ".join(cmd))
    try:
        result = subprocess.run(cmd, capture_output=True, text=True, errors="replace", stdin=subprocess.DEVNULL, **NO_WINDOW)
    except OSError as e:
        log(f"pip could not be started: {e}")
        return False
    for line in (result.stdout + result.stderr).splitlines():
        if line.strip() and not line.startswith(("Requirement already satisfied", "  ")):
            log(line)
    return result.returncode == 0


def install_python_packages(packages: List[str], log: Log = print) -> bool:
    """pip install ``packages`` for this Python (user installation as a fallback)."""
    if not packages:
        return True
    if subprocess.run([sys.executable, "-m", "pip", "--version"], capture_output=True, **NO_WINDOW).returncode != 0:
        log("pip is missing, installing it (ensurepip)")
        subprocess.run([sys.executable, "-m", "ensurepip", "--upgrade"], capture_output=True, **NO_WINDOW)
    ok = _pip(["install", "--disable-pip-version-check"] + packages, log)
    if not ok:
        ok = _pip(["install", "--disable-pip-version-check", "--user"] + packages, log)
    # packages installed into a user site folder created just now are not on sys.path yet
    try:
        user_site = site.getusersitepackages()
        if os.path.isdir(user_site) and user_site not in sys.path:
            site.addsitedir(user_site)
    except Exception:
        pass
    importlib.invalidate_caches()
    still = missing_python_packages()
    if still:
        log(f"could not install: {', '.join(still)}. Install them with: {sys.executable} -m pip install {' '.join(still)}")
        return False
    return True


def ensure_python_packages(log: Log = print) -> bool:
    missing = missing_python_packages()
    if not missing:
        return True
    log(f"installing missing Python packages: {', '.join(missing)}")
    return install_python_packages(missing, log)


def ffmpeg_path() -> Optional[str]:
    try:
        import imageio_ffmpeg

        return imageio_ffmpeg.get_ffmpeg_exe()
    except Exception:
        return shutil.which("ffmpeg")


# ---------------------------------------------------------------------------
# xma2encode.exe


def _program_files() -> List[str]:
    roots = [os.environ.get(v) for v in ("ProgramFiles(x86)", "ProgramFiles", "ProgramW6432")]
    if sys.platform.startswith("win"):
        roots += [r"C:\Program Files (x86)", r"C:\Program Files"]
    return [r for r in dict.fromkeys(roots) if r and os.path.isdir(r)]


# where the developer kits keep it, looked at before searching the whole kit
KNOWN_SUBFOLDERS = [os.path.join("bin", "win32"), os.path.join("bin", "x64"), os.path.join("bin", "x86"), "bin", ""]


def _search(root: str, depth: int) -> Optional[str]:
    """``xma2encode.exe`` under ``root`` (at most ``depth`` folders deep)."""
    if not root or not os.path.isdir(root):
        return None
    for sub in KNOWN_SUBFOLDERS:
        path = os.path.join(root, sub, ENCODER_NAME)
        if os.path.isfile(path):
            return path
    base = root.rstrip("\\/").count(os.sep)
    for folder, dirs, files in os.walk(root):
        for f in files:
            if f.lower() == ENCODER_NAME:
                return os.path.join(folder, f)
        if folder.count(os.sep) - base >= depth:
            dirs[:] = []
    return None


def _zip_member(path: str) -> Optional[str]:
    try:
        with zipfile.ZipFile(path) as archive:
            for name in archive.namelist():
                if os.path.basename(name.replace("\\", "/")).lower() == ENCODER_NAME:
                    return name
    except (zipfile.BadZipFile, OSError):
        pass
    return None


def candidate_locations() -> List[str]:
    """Places searched for the encoder, most specific first."""
    places = [os.path.join(BIN_DIR, ENCODER_NAME)]
    if os.environ.get("XMA2ENCODE"):
        places.append(os.environ["XMA2ENCODE"])
    for var in ("XEDK", "DurangoXDK", "GXDKLatest", "GameDKLatest", "GameDKXboxLatest", "GameDK"):
        if os.environ.get(var):
            places.append(os.environ[var])
    for root in _program_files():
        places += [
            os.path.join(root, "Microsoft Xbox 360 SDK"),
            os.path.join(root, "Microsoft Durango XDK"),
            os.path.join(root, "Microsoft GDK"),
        ]
    home = os.path.expanduser("~")
    places += [os.path.join(home, "Downloads"), os.path.join(home, "Desktop"), TOOL_DIR]
    return places


def find_xma2encode(search_zips: bool = False) -> Optional[str]:
    """Path of an installed ``xma2encode.exe`` (or ``zip::member`` when ``search_zips``)."""
    found = shutil.which("xma2encode") or shutil.which(ENCODER_NAME)
    if found:
        return found
    for place in candidate_locations():
        if os.path.isfile(place) and os.path.basename(place).lower() == ENCODER_NAME:
            return place
        hit = _search(place, 5)
        if hit:
            return hit
    if search_zips:
        downloads = os.path.join(os.path.expanduser("~"), "Downloads")
        if os.path.isdir(downloads):
            for f in sorted(os.listdir(downloads)):
                if f.lower().endswith(".zip"):
                    member = _zip_member(os.path.join(downloads, f))
                    if member:
                        return os.path.join(downloads, f) + "::" + member
    return None


def _common_folder(folder: str) -> bool:
    """A folder holding unrelated files (Downloads, Desktop, the home folder)."""
    home = os.path.expanduser("~")
    folder = os.path.normcase(os.path.abspath(folder))
    return folder in {os.path.normcase(os.path.abspath(os.path.join(home, f))) for f in ("", "Downloads", "Desktop")}


def install_xma2encode(source: str, log: Log = print) -> str:
    """Copy the encoder from ``source`` (the .exe, a folder or a .zip containing it, or a path
    returned by :func:`find_xma2encode`) and the DLLs next to it into ``tools/t4ff/bin``."""
    os.makedirs(BIN_DIR, exist_ok=True)
    target = os.path.join(BIN_DIR, ENCODER_NAME)
    member = None
    if "::" in source:
        source, member = source.split("::", 1)
    if os.path.isdir(source):
        found = _search(source, 6)
        if not found:
            raise FileNotFoundError(f"no {ENCODER_NAME} in {source}")
        source = found
    if source.lower().endswith(".zip"):
        member = (member or _zip_member(source) or "").replace("\\", "/")
        if not member:
            raise FileNotFoundError(f"no {ENCODER_NAME} in {source}")
        folder = os.path.dirname(member)
        with zipfile.ZipFile(source) as archive:
            for name in archive.namelist():
                clean = name.replace("\\", "/")
                if os.path.dirname(clean) != folder:
                    continue
                if clean == member:
                    out = target
                elif clean.lower().endswith(".dll"):
                    out = os.path.join(BIN_DIR, os.path.basename(clean))
                else:
                    continue
                with archive.open(name) as src, open(out, "wb") as dst:
                    shutil.copyfileobj(src, dst)
        log(f"installed {ENCODER_NAME} from {source} into {BIN_DIR}")
        return target
    if not os.path.isfile(source):
        raise FileNotFoundError(source)
    if os.path.abspath(source) != os.path.abspath(target):
        shutil.copy2(source, target)
        folder = os.path.dirname(os.path.abspath(source))
        if not _common_folder(folder):
            for f in os.listdir(folder):
                if f.lower().endswith(".dll"):
                    shutil.copy2(os.path.join(folder, f), os.path.join(BIN_DIR, f))
        log(f"installed {ENCODER_NAME} from {folder} into {BIN_DIR}")
    return target


def ensure_xma2encode(source: Optional[str] = None, log: Log = print) -> Optional[str]:
    """Path of a usable encoder (``source``, or the one found on this computer), None if there is
    none. An installed encoder is used where it is; one inside a .zip is extracted into
    ``tools/t4ff/bin``."""
    found = source or find_xma2encode(search_zips=True)
    if not found:
        return None
    if os.path.isdir(found):
        hit = _search(found, 6)
        if not hit:
            raise FileNotFoundError(f"no {ENCODER_NAME} in {found}")
        return hit
    if "::" in found or found.lower().endswith(".zip"):
        return install_xma2encode(found, log)
    if not os.path.isfile(found):
        raise FileNotFoundError(found)
    return found


def wine_path() -> Optional[str]:
    return shutil.which("wine") or shutil.which("wine64")


def wine_advice() -> str:
    if sys.platform == "darwin":
        return "install wine: brew install --cask wine-stable"
    return "install wine with your package manager, e.g.: sudo apt install wine   (32-bit support: sudo dpkg --add-architecture i386 && sudo apt install wine32)"


def test_xma2encode(path: str, log: Log = print) -> bool:
    """Encode a short tone to check the encoder runs."""
    import numpy as np

    from . import audio

    rate = 44100
    t = np.arange(rate // 4) / rate
    tone = (np.sin(2 * np.pi * 440 * t) * 8000).astype(np.int16).reshape(-1, 1)
    encoder = audio.XmaEncoder(path)
    try:
        stream = encoder.encode(audio.Pcm(rate, tone))
    except audio.AudioError as e:
        log(f"{ENCODER_NAME} test failed: {e}")
        if os.path.exists(path):
            usage = encoder.usage()
            if usage:
                log(f"{ENCODER_NAME} prints (include this when reporting the problem):")
                for line in usage.splitlines()[:40]:
                    log("    " + line)
        return False
    if not stream.packets:
        log(f"{ENCODER_NAME} test failed: no XMA data")
        return False
    log(f"{ENCODER_NAME} works ({stream.packets} XMA packet(s) for a 0.25 s test tone)")
    return True


# ---------------------------------------------------------------------------
# Everything


ENCODER_HELP = (
    f"{ENCODER_NAME} is part of Microsoft's Xbox developer kits (Xbox 360 XDK, Xbox One XDK, or the "
    "Microsoft GDK with Xbox extensions) and cannot be downloaded automatically. Put it (or a .zip "
    f"containing it) in your Downloads folder or in {BIN_DIR}, or give its path, and run setup again. "
    "Without it, sounds are not encoded."
)


def setup(encoder_source: Optional[str] = None, test: bool = True, log: Log = print) -> Dict[str, Optional[str]]:
    """Install what is missing. Returns the state: python, ffmpeg, xma2encode, wine (None = missing)."""
    state: Dict[str, Optional[str]] = {}

    ok = ensure_python_packages(log)
    state["python"] = "ok" if ok else None
    log("Python packages: " + ("installed" if ok else "MISSING"))

    state["ffmpeg"] = ffmpeg_path() if ok else None
    log("FFmpeg: " + (state["ffmpeg"] or "MISSING (pip install imageio-ffmpeg)"))

    oat = os.path.join(os.path.dirname(os.path.dirname(TOOL_DIR)), "OpenAssetTools", "src", "Common", "Game", "T4", "T4_Assets.h")
    state["openassettools"] = oat if os.path.exists(oat) else None
    log("OpenAssetTools: " + ("found" if state["openassettools"] else f"MISSING ({oat}): use the full repository"))

    encoder = None
    try:
        encoder = ensure_xma2encode(encoder_source, log)
    except (OSError, zipfile.BadZipFile) as e:
        log(f"{ENCODER_NAME}: cannot use {encoder_source or 'the one found'}: {e}")
    state["xma2encode"] = encoder
    if encoder is None:
        log(f"{ENCODER_NAME}: NOT FOUND. {ENCODER_HELP}")
    else:
        log(f"{ENCODER_NAME}: {encoder}")

    needs_wine = not sys.platform.startswith("win")
    state["wine"] = (wine_path() or None) if needs_wine else "not needed"
    if needs_wine:
        log("wine: " + (state["wine"] or f"MISSING, {wine_advice()}"))

    if encoder and test and ok and (state["wine"] is not None):
        if not test_xma2encode(encoder, log):
            state["xma2encode"] = None
    return state
