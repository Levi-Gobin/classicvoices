#!/usr/bin/env python3
"""Build the Classic Voices app for Windows and its installer.

  python desktop/build_windows.py [--iscc PATH]

Needs (on Windows, x64 Python): pip install wxPython pyinstaller; the engine
DLLs from nvda/build.py (build/nvda/cvengine64.dll, cvengine32.dll) and
windows/sapi/build.py (build/sapi/cvsapi64.dll, cvsapi32.dll); and Inno Setup
6 for the installer (ISCC.exe on PATH or --iscc). Output:
build/windows/dist/Classic Voices/ (the app) and
build/windows/ClassicVoicesSetup.exe.

Nothing of Apple's goes in: the app gets the voices from the user's own
firmware, like the NVDA add-on.
"""
import argparse
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "build", "windows")
LIB = os.path.join(ROOT, "nvda", "addon", "synthDrivers", "_classicvoices")

# The _classicvoices modules are imported from files at run time (they're
# shared with the NVDA add-on and the Linux tool), so PyInstaller can't see
# what they use: name it here.
HIDDEN = ["bz2", "base64", "codecs", "ctypes.util", "hashlib", "hmac", "io", "json", "lzma", "plistlib",
          "queue", "struct", "tarfile", "tempfile", "urllib.parse", "urllib.request", "zipfile", "zlib",
          "winreg", "bisect", "argparse", "collections", "importlib.util", "platform"]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--iscc", help="Inno Setup's ISCC.exe")
    a = ap.parse_args()
    for dll in ("nvda/cvengine64.dll", "nvda/cvengine32.dll", "sapi/cvsapi64.dll", "sapi/cvsapi32.dll"):
        if not os.path.isfile(os.path.join(ROOT, "build", dll)):
            sys.exit("missing build/%s: build it first (nvda/build.py, windows/sapi/build.py)" % dll)
    work = os.path.join(OUT, "work")
    shutil.rmtree(work, ignore_errors=True)
    stage = os.path.join(work, "stage")
    # the library, as files the app imports at run time
    shutil.copytree(LIB, os.path.join(stage, "lib", "_classicvoices"),
                    ignore=shutil.ignore_patterns("__pycache__", "*.pyc", "*.dll"))
    for bits in (64, 32):
        shutil.copy(os.path.join(ROOT, "build", "nvda", "cvengine%d.dll" % bits),
                    os.path.join(stage, "lib", "_classicvoices"))
    # the recompiler, for "Speed up the voices"
    os.makedirs(os.path.join(stage, "tools"))
    shutil.copy(os.path.join(ROOT, "tools", "recomp.py"), os.path.join(stage, "tools"))
    shutil.copytree(os.path.join(ROOT, "tools", "profiles"), os.path.join(stage, "tools", "profiles"),
                    ignore=shutil.ignore_patterns("mt*"))
    os.makedirs(os.path.join(stage, "core"))
    for f in os.listdir(os.path.join(ROOT, "core")):
        if f.endswith(".h"):
            shutil.copy(os.path.join(ROOT, "core", f), os.path.join(stage, "core"))
    os.makedirs(os.path.join(stage, "nvda", "src"))
    shutil.copy(os.path.join(ROOT, "nvda", "src", "native_glue.c"), os.path.join(stage, "nvda", "src"))

    sep = ";"
    cmd = [sys.executable, "-m", "PyInstaller", "--noconfirm", "--windowed", "--name", "Classic Voices",
           "--distpath", os.path.join(OUT, "dist"), "--workpath", os.path.join(work, "pyi"),
           "--specpath", work, "--paths", os.path.join(ROOT, "desktop")]
    for h in HIDDEN:
        cmd += ["--hidden-import", h]
    for d in ("lib", "tools", "core", "nvda"):
        cmd += ["--add-data", os.path.join(stage, d) + sep + d]
    cmd.append(os.path.join(ROOT, "desktop", "ClassicVoices.pyw"))
    subprocess.check_call(cmd)
    app = os.path.join(OUT, "dist", "Classic Voices")
    for bits in (64, 32):  # the SAPI engine, registered by the installer
        shutil.copy(os.path.join(ROOT, "build", "sapi", "cvsapi%d.dll" % bits), app)
    print("app:", app)

    iscc = a.iscc or shutil.which("iscc") or next(
        (p for p in (r"C:\Program Files\Inno Setup 7\ISCC.exe", r"C:\Program Files (x86)\Inno Setup 6\ISCC.exe",
                   r"C:\Program Files\Inno Setup 6\ISCC.exe")
         if os.path.isfile(p)), None)
    if not iscc:
        print("Inno Setup not found: no installer built (the app folder works on its own)")
        return
    subprocess.check_call([iscc, "/Q", "/DAppDir=" + app, "/DOutDir=" + OUT,
                           os.path.join(ROOT, "desktop", "windows", "ClassicVoices.iss")])
    print("installer:", os.path.join(OUT, "ClassicVoicesSetup.exe"))


if __name__ == "__main__":
    main()
