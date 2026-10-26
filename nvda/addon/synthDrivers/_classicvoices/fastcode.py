"""Builds the fast engine code on this computer, from the firmware imported
here: tools/recomp.py translates each engine's hot functions into C that
calls the interpreter's own instruction semantics with constant encodings,
and a C compiler folds that into native code, bit-identical to the
interpreter. The compiler is clang when there is one (Linux), else Zig,
downloaded from ziglang.org and checked against the SHA-256 in its own
index. Used by the NVDA add-on and the Windows and Linux app. What it builds
derives from Apple's code and stays on this computer.

The recompiler comes with the add-on (recompiler/: recomp.py, its profiles,
the instruction semantics headers and native_glue.c); the app passes where
its own copy is."""
import importlib.util
import json
import os
import platform
import shutil
import subprocess
import sys
import tempfile
import time

IS_WIN = sys.platform == "win32"
HERE = os.path.dirname(os.path.abspath(__file__))
ZIG_INDEX = "https://ziglang.org/download/index.json"
STRIPPED = ("ios3", "ios4", "ios5")  # function boundaries from the BL targets (recomp.py --scan-bl)
# Windows: no console window for each compiler run (it would take the focus)
NO_WINDOW = 0x08000000 if IS_WIN else 0


def cache_dir():
    if IS_WIN:
        return os.path.join(os.environ.get("LOCALAPPDATA", os.path.expanduser("~")), "Classic Voices")
    return os.path.join(os.environ.get("XDG_CACHE_HOME") or os.path.join(os.path.expanduser("~"), ".cache"),
                        "classicvoices")


def find_compiler():
    """A C compiler for the fast code, as an argument list, or None. Linux
    prefers the system's clang; otherwise a Zig downloaded before."""
    if not IS_WIN:
        for c in ("clang", "clang-19", "clang-18", "clang-17", "clang-16", "clang-15"):
            if shutil.which(c):
                return [shutil.which(c)]
    z = _cached_zig()
    return [z, "cc"] if z else None


def _cached_zig():
    root = os.path.join(cache_dir(), "zig")
    exe = "zig.exe" if IS_WIN else "zig"
    if os.path.isdir(root):
        for d in sorted(os.listdir(root), reverse=True):
            p = os.path.join(root, d, exe)
            if os.path.isfile(p):
                return p
    return None


def _zig_target():
    m = platform.machine().lower()
    arch = "aarch64" if m in ("arm64", "aarch64") else "x86_64" if m in ("amd64", "x86_64") else "x86"
    return "%s-%s" % (arch, "windows" if IS_WIN else "linux")


def download_zig(progress):
    """Fetch Zig (its C compiler builds the fast code) from ziglang.org,
    checked against the SHA-256 in its own index. Returns the zig path."""
    import hashlib
    import tarfile
    import urllib.request
    import zipfile
    progress("Finding the Zig compiler", None)
    with urllib.request.urlopen(ZIG_INDEX, timeout=60) as r:
        index = json.load(r)
    version = next(k for k in index if k != "master")
    entry = index[version][_zig_target()]
    root = os.path.join(cache_dir(), "zig")
    os.makedirs(root, exist_ok=True)
    archive = os.path.join(root, os.path.basename(entry["tarball"]))
    h = hashlib.sha256()
    with urllib.request.urlopen(entry["tarball"], timeout=120) as r, open(archive + ".part", "wb") as f:
        total = int(r.headers.get("Content-Length") or entry.get("size") or 0)
        got = 0
        progress("Downloading the Zig compiler (%d MB)" % (total >> 20), 0.0)
        while True:
            b = r.read(1 << 20)
            if not b:
                break
            f.write(b)
            h.update(b)
            got += len(b)
            progress(None, got / total if total else None)
    if h.hexdigest() != entry["shasum"]:
        os.remove(archive + ".part")
        raise OSError("the Zig download doesn't match its checksum")
    os.replace(archive + ".part", archive)
    progress("Unpacking the Zig compiler", None)
    dest = os.path.join(root, version)
    tmp = dest + ".tmp"
    shutil.rmtree(tmp, ignore_errors=True)
    if archive.endswith(".zip"):
        with zipfile.ZipFile(archive) as z:
            z.extractall(tmp)
    else:
        with tarfile.open(archive) as t:
            t.extractall(tmp, filter="data")
    inner = os.path.join(tmp, os.listdir(tmp)[0])
    shutil.rmtree(dest, ignore_errors=True)
    os.replace(inner, dest)
    shutil.rmtree(tmp, ignore_errors=True)
    os.remove(archive)
    return _cached_zig()


def recompiler(candidates=()):
    """(tools dir, core dir, native_glue.c) of the recompiler: the add-on's own
    copy, else the first of candidates (the same triples) that's there."""
    own = os.path.join(HERE, "recompiler")
    for tools, core, glue in ((own, own, os.path.join(own, "native_glue.c")),) + tuple(candidates):
        if os.path.isfile(os.path.join(tools, "recomp.py")) and os.path.isfile(glue):
            return tools, core, glue
    return None


def native_files(build):
    """The fast-code files a build needs here (Windows: both bitnesses, for
    32- and 64-bit programs)."""
    if IS_WIN:
        return ["cvnative_%s_64.dll" % build, "cvnative_%s_32.dll" % build]
    return ["cvnative_%s.so" % build]


def native_targets():
    """(output file name pattern, extra compiler args) per file a build needs."""
    if IS_WIN:
        return [("cvnative_%s_64.dll", ["-target", "x86_64-windows-gnu"]),
                ("cvnative_%s_32.dll", ["-target", "x86-windows-gnu", "-DGUEST_MEM_SIZE=0x20000000u"])]
    return [("cvnative_%s.so", ["-fPIC"])]


def build(build, voices_root, native_dir, cc, progress, candidates=(), python=None):
    """Recompile one imported engine and compile it into this system's
    fast-code files in native_dir. progress(message, fraction) may raise to
    cancel. python: run the recompiler with that interpreter (else in this
    process, as in NVDA or a frozen app)."""
    tools, core, glue = recompiler(candidates)
    voiced = os.path.join(voices_root, build, "voiced")
    os.makedirs(cache_dir(), exist_ok=True)
    work = tempfile.mkdtemp(prefix="cv_fast_", dir=cache_dir())
    try:
        gen = os.path.join(work, "gen")
        progress("Translating the %s engine" % build, None)
        args = [voiced, os.path.join(tools, "profiles", build + ".txt"), gen, "--prefix", build]
        if build in STRIPPED:
            args.append("--scan-bl")
        if python:
            subprocess.run([python, "-I", os.path.join(tools, "recomp.py")] + args, check=True,
                           stdout=subprocess.DEVNULL, creationflags=NO_WINDOW)
        else:
            _recomp_module(tools).main(args)
        sources = sorted(os.path.join(gen, f) for f in os.listdir(gen) if f.endswith(".c")) + [glue]
        targets = native_targets()
        steps, finished = len(sources) * len(targets), 0
        os.makedirs(native_dir, exist_ok=True)
        jobs = max(1, min(os.cpu_count() or 2, 8))
        for n, (pattern, extra) in enumerate(targets):
            objdir = os.path.join(work, "obj%d" % n)
            os.makedirs(objdir)
            objs, running = [], []
            for src in sources + [None]:
                while running and (src is None or len(running) >= jobs):
                    finished += _reap(running)
                    progress("Compiling the %s engine" % build, finished / steps)
                if src is None:
                    break
                o = os.path.join(objdir, os.path.basename(src) + ".o")
                objs.append(o)
                running.append(subprocess.Popen(cc + extra + ["-O2", "-std=gnu11", "-ffp-contract=off", "-w",
                                                              "-I", core, "-DCV_BUILD=" + build, "-c", src, "-o", o],
                                                stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
                                                creationflags=NO_WINDOW))
            out = os.path.join(native_dir, pattern % build)
            tmp = out + ".new"
            subprocess.run(cc + extra + ["-shared", "-s", "-o", tmp] + objs, check=True,
                           stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, creationflags=NO_WINDOW)
            if os.path.exists(out):
                try:
                    os.remove(out)
                except OSError:  # a program has it loaded: the new one is used next time
                    os.replace(out, out + ".old")
            os.replace(tmp, out)
            if not IS_WIN:
                os.chmod(out, 0o444)
        # Zig leaves the DLL's import library (named after the first object) beside it
        for f in os.listdir(native_dir):
            if f.endswith(".c.lib"):
                os.remove(os.path.join(native_dir, f))
    finally:
        shutil.rmtree(work, ignore_errors=True)


def _reap(running):
    """Wait for one compile to end; raise if it failed. Returns 1."""
    while True:
        for p in running:
            if p.poll() is not None:
                running.remove(p)
                if p.returncode:
                    err = p.stderr.read().decode("utf-8", "replace")[-2000:]
                    for q in running:
                        q.kill()
                    raise OSError("compiling failed: " + err)
                return 1
        time.sleep(0.05)


def _recomp_module(tools):
    """tools/recomp.py, loaded by path (no sys.path change inside NVDA)."""
    spec = importlib.util.spec_from_file_location("classicvoices_recomp", os.path.join(tools, "recomp.py"))
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m
