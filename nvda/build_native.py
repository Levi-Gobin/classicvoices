#!/usr/bin/env python3
"""Build the recompiled (fast) engine DLLs for the Classic Voices NVDA add-on,
from firmware YOU supply. The output is derived from Apple's code and must
never be published; it belongs on your machine (or in your own private CI).

  python nvda/build_native.py [--source DIR] [--out DIR] [--builds ios6 ...]
                              [--bits 64 32]

source  where the engines are (default: the NVDA data folder,
        %%APPDATA%%/nvda/classicvoices/Voices, i.e. whatever you imported with
        the add-on's data manager). Also accepts tools/fetch_firmware.sh's
        extracted/ shape.
out     where the DLLs go (default: <NVDA data folder>/Native, where the
        add-on looks). Files are cvnative_<build>_<bits>.dll.

Pipeline per build: tools/recomp.py turns the engine's hot functions into C
that calls the interpreter's own instruction semantics with constant
encodings; an optimizing compiler folds that into real native code, so the
translation cannot drift from the interpreter. MSVC (found via vswhere) is
preferred: it optimizes these huge generated functions in seconds where GCC
takes hours. clang-cl / clang / gcc are fallbacks.

iOS 3/4/5 engines are stripped: recomp.py takes their function boundaries
from the engine's own BL targets (--scan-bl).
"""
import argparse
import os
import shutil
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "nvda", "src")
WORK = os.path.join(ROOT, "build", "native")

BUILDS = ("ios3", "ios4", "ios5", "ios6", "ios70", "ios7", "ios10")
STRIPPED = ("ios3", "ios4", "ios5")
VSWHERE = os.path.join(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)"),
                       "Microsoft Visual Studio", "Installer", "vswhere.exe")


def default_data_dir():
    return os.path.join(os.environ.get("APPDATA", ""), "nvda", "classicvoices")


def find_voiced(source, build):
    """Accept the Voices/<build> shape and fetch_firmware.sh's extracted/ shape."""
    for rel in (
        (build, "voiced"),
        (build, "VoiceServices.framework", "Support", "voiced"),
        (build, "TTSSpeechBundle.speechbundle", "TTSSpeechBundle"),
    ):
        p = os.path.join(source, *rel)
        if os.path.isfile(p):
            return p
    return None


# ---------------------------------------------------------------- compilers

def msvc_env(bits):
    """The environment after vcvarsall, or None."""
    if not os.path.isfile(VSWHERE):
        return None
    try:
        vs = subprocess.check_output(
            [VSWHERE, "-latest", "-products", "*",
             "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
             "-property", "installationPath"], text=True).strip()
    except subprocess.CalledProcessError:
        return None
    if not vs:
        return None
    vcvars = os.path.join(vs, "VC", "Auxiliary", "Build", "vcvarsall.bat")
    if not os.path.isfile(vcvars):
        return None
    arch = "x64" if bits == 64 else "x86"
    out = subprocess.check_output(
        'cmd /s /c ""%s" %s >nul 2>&1 && set"' % (vcvars, arch), shell=True, text=True)
    env = dict(os.environ)
    for line in out.splitlines():
        if "=" in line:
            k, _sep, v = line.partition("=")
            env[k] = v
    return env if "VCToolsInstallDir" in env else None


def find_clang(bits):
    """clang with a MinGW target: the recompiler's C is written for clang's
    constant folding (recomp.py's whole trick), and MSVC neither folds it nor
    finishes force-inlining it in reasonable time. llvm-mingw's prefixed
    wrappers are preferred; plain clang works when a MinGW GCC is installed
    for it to find its runtime libraries in."""
    prefixed = ("x86_64-w64-mingw32-clang" if bits == 64 else "i686-w64-mingw32-clang")
    found = shutil.which(prefixed) or shutil.which("clang")
    if found:
        return found
    for p in (os.path.expanduser(r"~\scoop\apps\llvm\current\bin\clang.exe"),
              r"C:\Program Files\LLVM\bin\clang.exe"):
        if os.path.isfile(p):
            return p
    return None


def gnu_cc(bits):
    for name in (("x86_64-w64-mingw32-clang", "x86_64-w64-mingw32-gcc") if bits == 64
                 else ("i686-w64-mingw32-clang", "i686-w64-mingw32-gcc")):
        if shutil.which(name):
            return name
    return None


def _compile_parallel(cmds):
    with ThreadPoolExecutor(max_workers=max(2, (os.cpu_count() or 4) - 2)) as ex:
        for res in ex.map(lambda c: subprocess.run(c, capture_output=True, text=True), cmds):
            if res.returncode:
                sys.exit("compile failed:\n%s" % (res.stderr or res.stdout))


def compile_dll(build, bits, gen_dir, out_dll):
    sources = sorted(os.path.join(gen_dir, f) for f in os.listdir(gen_dir) if f.endswith(".c"))
    glue = os.path.join(SRC, "native_glue.c")
    obj_dir = os.path.join(WORK, "obj_%s_%d" % (build, bits))
    shutil.rmtree(obj_dir, ignore_errors=True)
    os.makedirs(obj_dir)
    clang = find_clang(bits)
    if clang:
        target = "x86_64-w64-windows-gnu" if bits == 64 else "i686-w64-windows-gnu"
        objs, cmds = [], []
        for s in sources + [glue]:
            o = os.path.join(obj_dir, os.path.basename(s) + ".o")
            cmds.append([clang, "--target=" + target, "-O2", "-std=gnu11",
                         "-ffp-contract=off", "-w", "-I", os.path.join(ROOT, "core"),
                         "-DCV_BUILD=" + build, "-c", s, "-o", o])
            objs.append(o)
        _compile_parallel(cmds)
        subprocess.check_call([clang, "--target=" + target, "-shared", "-static",
                               "-o", out_dll] + objs)
        return "clang"
    env = msvc_env(bits)
    if env:
        print("WARNING: building with MSVC; it does not constant-fold the "
              "generated code, so these DLLs will be no faster than the "
              "interpreter. Install clang (e.g. `scoop install llvm`).")
        # CreateProcess resolves the exe against the parent's PATH, so point
        # at the toolchain directly.
        tooldir = os.path.join(env["VCToolsInstallDir"], "bin", "Hostx64",
                               "x64" if bits == 64 else "x86")
        # /MP compiles the generated files in parallel; /MT keeps the DLL
        # self-contained; default /arch has no FMA, so floats stay bit-exact.
        cmd = [os.path.join(tooldir, "cl.exe"), "/nologo", "/O2", "/MT", "/MP", "/std:c17", "/w",
               "/FI", os.path.join(SRC, "vx_msvc.h"), "/I", os.path.join(ROOT, "core"),
               "/DCV_BUILD=" + build, "/Fo" + obj_dir + os.sep,
               "/c"] + sources + [glue]
        subprocess.check_call(cmd, env=env)
        objs = [os.path.join(obj_dir, f) for f in os.listdir(obj_dir) if f.endswith(".obj")]
        subprocess.check_call([os.path.join(tooldir, "link.exe"), "/nologo", "/DLL",
                               "/OUT:" + out_dll] + objs, env=env)
        for ext in (".lib", ".exp"):  # import-library litter next to the DLL
            p = os.path.splitext(out_dll)[0] + ext
            if os.path.exists(p):
                os.remove(p)
        return "msvc"
    cc = gnu_cc(bits)
    if not cc:
        sys.exit("no compiler: need VS Build Tools (vswhere) or MinGW clang/gcc for %d-bit" % bits)
    if cc.endswith("gcc"):
        print("WARNING: gcc optimizes this generated code extremely slowly; "
              "this may take hours. Install VS Build Tools or clang for a fast build.")
    objs = []
    for s in sources + [glue]:
        o = os.path.join(obj_dir, os.path.basename(s) + ".o")
        subprocess.check_call([cc, "-O2", "-std=gnu11", "-ffp-contract=off", "-w",
                               "-I", os.path.join(ROOT, "core"),
                               "-include", os.path.join(SRC, "vx_win.h"),
                               "-DCV_BUILD=" + build, "-c", s, "-o", o])
        objs.append(o)
    subprocess.check_call([cc, "-shared", "-static", "-o", out_dll] + objs)
    return cc


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--source", default=os.path.join(default_data_dir(), "Voices"))
    ap.add_argument("--out", default=os.path.join(default_data_dir(), "Native"))
    ap.add_argument("--builds", nargs="+", default=list(BUILDS))
    ap.add_argument("--bits", nargs="+", type=int, default=[64, 32], choices=[32, 64])
    a = ap.parse_args()
    os.makedirs(WORK, exist_ok=True)
    os.makedirs(a.out, exist_ok=True)
    done, skipped = [], []
    for build in a.builds:
        voiced = find_voiced(a.source, build)
        if not voiced:
            skipped.append((build, "no engine under %s" % a.source))
            continue
        gen_dir = os.path.join(WORK, "gen_" + build)
        stamp = os.path.join(gen_dir, ".src")
        if not (os.path.isdir(gen_dir)
                and os.path.isfile(stamp)
                and open(stamp).read() == voiced + repr(os.path.getmtime(voiced))):
            shutil.rmtree(gen_dir, ignore_errors=True)
            cmd = [sys.executable, "-I", os.path.join(ROOT, "tools", "recomp.py"),
                   voiced, os.path.join(ROOT, "tools", "profiles", build + ".txt"),
                   gen_dir, "--prefix", build]
            if build in STRIPPED:  # function boundaries from its BL targets
                cmd.append("--scan-bl")
            print("recompiling", build)
            subprocess.check_call(cmd)
            with open(stamp, "w") as fh:
                fh.write(voiced + repr(os.path.getmtime(voiced)))
        for bits in a.bits:
            dll = os.path.join(a.out, "cvnative_%s_%d.dll" % (build, bits))
            print("compiling %s (%d-bit)" % (build, bits))
            cc = compile_dll(build, bits, gen_dir, dll)
            print("  -> %s (%s, %.1f MB)" % (dll, cc, os.path.getsize(dll) / 1e6))
            done.append(dll)
    for build, why in skipped:
        print("skipped %s: %s" % (build, why))
    if not done:
        sys.exit("nothing built")


if __name__ == "__main__":
    main()
