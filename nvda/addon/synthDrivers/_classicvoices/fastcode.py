"""Builds the fast engine code on this computer, from the firmware imported
here: tools/recomp.py translates each engine's hot functions into C that
calls the interpreter's own instruction semantics with constant encodings,
and a C compiler folds that into native code, bit-identical to the
interpreter. The compiler is clang when there is one (Linux), else Zig,
downloaded from ziglang.org and checked against the SHA-256 in its own
index; on Windows on Arm, llvm-mingw (Zig can't build ARM64EC, the native
Arm code x64 programs such as NVDA load there), from its GitHub release,
checked against the SHA-256 pinned here. Used by the NVDA add-on and the
Windows and Linux app. What it builds
derives from Apple's code and stays on this computer.

The recompiler comes with the add-on (recompiler/: recomp.py, its profiles,
the instruction semantics headers and native_glue.c); the app passes where
its own copy is."""
import hashlib
import importlib.util
import json
import os
import platform
import re
import shutil
import subprocess
import sys
import tempfile
import time

IS_WIN = sys.platform == "win32"
HERE = os.path.dirname(os.path.abspath(__file__))
ZIG_INDEX = "https://ziglang.org/download/index.json"
# Windows on Arm's compiler: (version, URL, SHA-256, bytes) of llvm-mingw's
# release for Arm64 Windows (LLVM 23; ARM64EC since LLVM 21).
LLVM_MINGW = ("20261006",
              "https://github.com/mstorsjo/llvm-mingw/releases/download/20261006/llvm-mingw-20261006-ucrt-aarch64.zip",
              "9a835d5179c9f3a5a783c11a7f14062a249b4765dd11add64a66786864e82ab2", 185727081)
STRIPPED = ("ios3", "ios4", "ios5")  # function boundaries from the BL targets (recomp.py --scan-bl)
# Windows: no console window for each compiler run (it would take the focus)
NO_WINDOW = 0x08000000 if IS_WIN else 0
# Below normal priority (Windows) / nice 10: a screen reader speaking on the
# same computer goes first, so speech doesn't lag while fast code builds.
BELOW_NORMAL = 0x00004000 if IS_WIN else 0


def _run(cmd, **kw):
    """A build step, at low priority (see BELOW_NORMAL), with no window."""
    p = subprocess.Popen(cmd, creationflags=NO_WINDOW | BELOW_NORMAL, **kw)
    if not IS_WIN:
        try:
            os.setpriority(os.PRIO_PROCESS, p.pid, 10)
        except (OSError, AttributeError):
            pass
    return p


def _check(cmd, **kw):
    p = _run(cmd, **kw)
    err = p.communicate()[1]
    if p.returncode:
        raise OSError("compiling failed: " + (err or b"").decode("utf-8", "replace")[-2000:])
# The generated functions are huge: LLVM's SLP vectorizer spent 290 s on one
# iOS 7.1.2 file and made the code slower (iOS 6 sentences 17% faster
# without it), and its classic GVN 85% of an iOS 6 file's time (NewGVN: a
# sixth). Same output either way. -g0: Zig adds debug info otherwise.
FAST_FLAGS = ["-g0", "-fno-slp-vectorize", "-mllvm", "-enable-newgvn"]
# Bumped when the compiler flags change what's built (f2: FAST_FLAGS).
FLAGS_VERSION = "f2"
SEMANTICS = ("thumb.h", "thumb_ops.h", "recomp_rt.h")
BUILDS = ("ios3", "ios4", "ios5", "ios6", "ios70", "ios7", "ios10")  # a profile each


def cache_dir():
    if IS_WIN:
        return os.path.join(os.environ.get("LOCALAPPDATA", os.path.expanduser("~")), "Classic Voices")
    return os.path.join(os.environ.get("XDG_CACHE_HOME") or os.path.join(os.path.expanduser("~"), ".cache"),
                        "classicvoices")


def arm_windows():
    """Windows on Arm (x64 Python, NVDA's, says ARM64 here too): fast code for
    64-bit programs is ARM64EC, which they run natively."""
    return IS_WIN and platform.machine().lower() in ("arm64", "aarch64")


def find_compiler():
    """A C compiler for the fast code, as an argument list, or None. Linux
    prefers the system's clang; Windows on Arm llvm-mingw, otherwise a Zig
    downloaded before."""
    if arm_windows():
        c = _cached_llvm_mingw()
        return [c] if c else None
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


def _cached_llvm_mingw():
    p = os.path.join(cache_dir(), "llvm-mingw", LLVM_MINGW[0], "bin", "clang.exe")
    return p if os.path.isfile(p) else None


def download_compiler(progress):
    """Fetch the compiler find_compiler wants (llvm-mingw on Windows on Arm,
    else Zig). Returns its argument list."""
    if arm_windows():
        download_llvm_mingw(progress)
    else:
        download_zig(progress)
    return find_compiler()


def _download(url, path, sha256, size, what, progress):
    """url into path, checked against sha256; progress per MB."""
    import urllib.request
    h = hashlib.sha256()
    with urllib.request.urlopen(url, timeout=120) as r, open(path + ".part", "wb") as f:
        total = int(r.headers.get("Content-Length") or size or 0)
        got = 0
        progress("Downloading the %s compiler (%d MB)" % (what, total >> 20), 0.0)
        while True:
            b = r.read(1 << 20)
            if not b:
                break
            f.write(b)
            h.update(b)
            got += len(b)
            progress(None, got / total if total else None)
    if h.hexdigest() != sha256:
        os.remove(path + ".part")
        raise OSError("the %s download doesn't match its checksum" % what)
    os.replace(path + ".part", path)


def _llvm_mingw_needed(rel):
    """Whether a file of llvm-mingw (path inside its folder) is needed for the
    fast code: the compiler and linker, ARM64EC and x86 (32-bit programs)
    libraries and the headers; not the Arm 32-bit or x64 ones, the
    debugger, Python, the sanitizers."""
    top, _, rest = rel.partition("/")
    if top in ("include", "generic-w64-mingw32", "aarch64-w64-mingw32", "i686-w64-mingw32"):
        return True
    if top == "lib":
        return rest.startswith("clang/") and ("/include/" in rest or "/libclang_rt.builtins" in rest)
    if top == "bin" and "/" not in rest:
        if rest.endswith(".dll"):
            return not rest.startswith(("liblldb", "libpython"))
        # clang.exe and the target-named ones start clang-<version>.exe, the compiler
        return rest.endswith(".cfg") or re.match(r"clang-\d+\.exe$", rest) is not None or rest in (
            "clang.exe", "ld.lld.exe", "arm64ec-w64-mingw32-clang.exe", "i686-w64-mingw32-clang.exe")
    return False


def download_llvm_mingw(progress):
    """llvm-mingw for Windows on Arm, checked against LLVM_MINGW's SHA-256;
    only what the fast code needs is unpacked (about 400 of its 735 MB)."""
    import zipfile
    version, url, sha256, size = LLVM_MINGW
    root = os.path.join(cache_dir(), "llvm-mingw")
    os.makedirs(root, exist_ok=True)
    archive = os.path.join(root, os.path.basename(url))
    _download(url, archive, sha256, size, "llvm-mingw", progress)
    dest = os.path.join(root, version)
    tmp = dest + ".tmp"
    shutil.rmtree(tmp, ignore_errors=True)
    os.makedirs(tmp)
    with zipfile.ZipFile(archive) as z:
        members = [m for m in z.infolist()
                   if not m.is_dir() and "/" in m.filename and _llvm_mingw_needed(m.filename.split("/", 1)[1])]
        progress("Unpacking the llvm-mingw compiler", 0.0)
        for n, m in enumerate(members):
            out = os.path.join(tmp, *m.filename.split("/")[1:])
            os.makedirs(os.path.dirname(out), exist_ok=True)
            with z.open(m) as src, open(out, "wb") as f:
                shutil.copyfileobj(src, f, 1 << 20)
            if n % 50 == 0:
                progress(None, n / len(members))
    progress(None, 1.0)
    shutil.rmtree(dest, ignore_errors=True)
    os.replace(tmp, dest)
    os.remove(archive)
    # Zig, which an older version fetched here, isn't used on Arm any more
    shutil.rmtree(os.path.join(cache_dir(), "zig"), ignore_errors=True)
    return _cached_llvm_mingw()


def download_zig(progress):
    """Fetch Zig (its C compiler builds the fast code) from ziglang.org,
    checked against the SHA-256 in its own index. Returns the zig path."""
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
    _download(entry["tarball"], archive, entry["shasum"], int(entry.get("size") or 0), "Zig", progress)
    progress("Unpacking the Zig compiler", 0.0)
    dest = os.path.join(root, version)
    tmp = dest + ".tmp"
    shutil.rmtree(tmp, ignore_errors=True)
    if archive.endswith(".zip"):
        with zipfile.ZipFile(archive) as z:
            members = z.infolist()
            for n, m in enumerate(members):
                z.extract(m, tmp)
                if n % 50 == 0:
                    progress(None, n / len(members))
    else:
        with tarfile.open(archive) as t:
            members = t.getmembers()
            for n, m in enumerate(members):
                t.extract(m, tmp, filter="data")
                if n % 50 == 0:
                    progress(None, n / len(members))
    progress(None, 1.0)
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
    32- and 64-bit programs; on Arm the 64-bit one is ARM64EC)."""
    if arm_windows():
        return ["cvnative_%s_arm64ec.dll" % build, "cvnative_%s_32.dll" % build]
    if IS_WIN:
        return ["cvnative_%s_64.dll" % build, "cvnative_%s_32.dll" % build]
    return ["cvnative_%s.so" % build]


def has_fast_code(native_dir, build):
    """Fast code is there, from this version or an older one. On Arm, x64
    fast code from before ARM64EC counts: the engine runs it until it's built
    again."""
    sets = [native_files(build)]
    if arm_windows():
        sets.append(["cvnative_%s_64.dll" % build, "cvnative_%s_32.dll" % build])
    return any(all(os.path.isfile(os.path.join(native_dir, f)) for f in names) for names in sets)


def stamp(candidates=()):
    """What fast code is built from here besides the engine: the recompiler,
    its profiles, the instruction semantics (the interpreter's must match
    them), the glue and the flags. Fast code with another stamp was built by
    another version, and is rebuilt. None: no recompiler here."""
    r = recompiler(candidates)
    if r is None:
        return None
    tools, core, glue = r
    if r not in _stamps:
        h = hashlib.sha256(FLAGS_VERSION.encode())
        profiles = os.path.join(tools, "profiles")
        # every build's profile (each a few KB), so one stamp serves all, as on Android
        files = [("recomp.py", os.path.join(tools, "recomp.py")), ("native_glue.c", glue)]
        files += [(n, os.path.join(core, n)) for n in SEMANTICS]
        files += [("profiles/%s.txt" % b, os.path.join(profiles, b + ".txt")) for b in BUILDS]
        for name, path in files:
            with open(path, "rb") as f:
                h.update(name.encode() + b"\0" + f.read().replace(b"\r\n", b"\n") + b"\0")
        _stamps[r] = h.hexdigest()[:16]
    return _stamps[r]


_stamps = {}


def stamp_file(native_dir, build):
    return os.path.join(native_dir, "cvnative_%s.stamp" % build)


def write_stamp(native_dir, build, value):
    with open(stamp_file(native_dir, build), "w", encoding="utf-8") as f:
        f.write(value + "\n")


def up_to_date(native_dir, build, candidates=()):
    """Whether build's fast code in native_dir was built by this version (its
    files are there and the stamp matches). Without a recompiler here,
    whatever is there counts: nothing could rebuild it."""
    if not all(os.path.isfile(os.path.join(native_dir, f)) for f in native_files(build)):
        return False
    want = stamp(candidates)
    if want is None:
        return True
    try:
        with open(stamp_file(native_dir, build), encoding="utf-8") as f:
            return f.read().strip() == want
    except OSError:  # built before 0.5.0
        return False


def native_targets(cc):
    """(output file name pattern, compiler command, what it's for or None) per
    file a build needs, for the compiler cc."""
    small = ["-DGUEST_MEM_SIZE=0x20000000u"]  # 32-bit programs: a 512 MB guest
    if arm_windows():  # llvm-mingw's target-named clangs
        bindir = os.path.dirname(cc[0])
        return [("cvnative_%s_arm64ec.dll", [os.path.join(bindir, "arm64ec-w64-mingw32-clang.exe")], "64-bit programs"),
                ("cvnative_%s_32.dll", [os.path.join(bindir, "i686-w64-mingw32-clang.exe")] + small, "32-bit programs")]
    if IS_WIN:
        return [("cvnative_%s_64.dll", cc + ["-target", "x86_64-windows-gnu"], "64-bit programs"),
                ("cvnative_%s_32.dll", cc + ["-target", "x86-windows-gnu"] + small, "32-bit programs")]
    return [("cvnative_%s.so", cc + ["-fPIC"], None)]


def build(build, voices_root, native_dir, cc, progress, candidates=(), python=None):
    """Recompile one imported engine and compile it into this system's
    fast-code files in native_dir. progress(message, fraction) may raise to
    cancel. python: run the recompiler with that interpreter (else in this
    process, as in NVDA or a frozen app)."""
    tools, core, glue = recompiler(candidates)
    value = stamp(candidates)
    voiced = os.path.join(voices_root, build, "voiced")
    os.makedirs(cache_dir(), exist_ok=True)
    work = tempfile.mkdtemp(prefix="cv_fast_", dir=cache_dir())
    try:
        gen = os.path.join(work, "gen")
        progress("Translating the %s engine" % build, 0.0)
        args = [voiced, os.path.join(tools, "profiles", build + ".txt"), gen, "--prefix", build]
        if build in STRIPPED:
            args.append("--scan-bl")
        _translate(tools, args, python, lambda f: progress(None, f))
        sources = sorted(os.path.join(gen, f) for f in os.listdir(gen) if f.endswith(".c")) + [glue]
        os.makedirs(native_dir, exist_ok=True)
        jobs = max(1, min((os.cpu_count() or 2) - 1, 8))  # a core left for speech
        # each step its own progress: every file of each kind compiled
        for n, (pattern, cmd, what) in enumerate(native_targets(cc)):
            objdir = os.path.join(work, "obj%d" % n)
            os.makedirs(objdir)
            objs, running, finished = [], [], 0
            progress(("Compiling the %s engine for %s" % (build, what)) if what else
                     ("Compiling the %s engine" % build), 0.0)
            for src in sources + [None]:
                while running and (src is None or len(running) >= jobs):
                    finished += _reap(running)
                    progress(None, finished / len(sources))
                if src is None:
                    break
                o = os.path.join(objdir, os.path.basename(src) + ".o")
                objs.append(o)
                running.append(_run(cmd + ["-O2"] + FAST_FLAGS + ["-std=gnu11", "-ffp-contract=off", "-w",
                                                     "-I", core, "-DCV_BUILD=" + build, "-c", src, "-o", o],
                                    stdout=subprocess.DEVNULL, stderr=subprocess.PIPE))
            out = os.path.join(native_dir, pattern % build)
            tmp = out + ".new"
            _check(cmd + ["-shared", "-s", "-o", tmp] + objs, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
            if os.path.exists(out):
                try:
                    os.remove(out)
                except OSError:  # a program has it loaded: the new one is used next time
                    remove_old(native_dir)
                    os.replace(out, out + ".old")
            os.replace(tmp, out)
            if not IS_WIN:
                os.chmod(out, 0o444)
        # Zig leaves the DLL's import library (named after the first object) beside it
        for f in os.listdir(native_dir):
            if f.endswith(".c.lib"):
                os.remove(os.path.join(native_dir, f))
        remove_old(native_dir)
        write_stamp(native_dir, build, value)
    finally:
        shutil.rmtree(work, ignore_errors=True)


def remove_old(native_dir):
    """The fast code a build replaced while a program had it loaded, once no
    program has it any more."""
    try:
        names = os.listdir(native_dir)
    except OSError:
        return
    for f in names:
        if f.endswith(".old"):
            try:
                os.remove(os.path.join(native_dir, f))
            except OSError:
                pass


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


def _count_progress(m, report):
    """Have recomp.py (module m) report how far it is, without changing it
    (the stamp hashes it): exploring each function is the first 40% of its
    time, writing each the rest. report(fraction) at each whole percent."""
    starts = set()
    base = m.Function

    class Counted(base):
        def __init__(self, lo, *a, **k):
            starts.add(lo)
            base.__init__(self, lo, *a, **k)

    done, last = [0, 0], [-1]

    def tell():
        f = (0.4 * done[0] + 0.6 * done[1]) / max(1, len(starts))
        if int(f * 100) != last[0]:
            last[0] = int(f * 100)
            report(min(f, 1.0))

    explore, emit = m.explore, m.emit_function

    def counted_explore(*a, **k):
        r = explore(*a, **k)
        done[0] += 1
        tell()
        return r

    def counted_emit(*a, **k):
        r = emit(*a, **k)
        done[1] += 1
        tell()
        return r

    m.Function, m.explore, m.emit_function = Counted, counted_explore, counted_emit


def _translate(tools, args, python, report):
    """Run recomp.py with args, reporting its progress. python: in that
    interpreter (this file runs it, see the end), else in this process."""
    if python:
        p = _run([python, "-I", os.path.abspath(__file__), "--translate", tools] + args,
                 stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        other = []
        for line in p.stdout:
            text = line.decode("utf-8", "replace").rstrip()
            if text.startswith("progress "):
                report(float(text[9:]))
            else:
                other.append(text)
        if p.wait():
            raise OSError("translating failed: " + "\n".join(other)[-2000:])
        return
    # In this process (NVDA's): hand Python's lock back sooner (5 ms by
    # default), so NVDA's own threads aren't kept waiting meanwhile.
    m = _recomp_module(tools)
    _count_progress(m, report)
    interval = sys.getswitchinterval()
    sys.setswitchinterval(0.0005)
    try:
        m.main(args)
    finally:
        sys.setswitchinterval(interval)


if __name__ == "__main__" and sys.argv[1:2] == ["--translate"]:
    # _translate in another interpreter: progress on stdout
    _m = _recomp_module(sys.argv[2])
    _count_progress(_m, lambda f: print("progress %.3f" % f, flush=True))
    _m.main(sys.argv[3:])
