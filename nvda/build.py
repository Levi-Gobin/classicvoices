#!/usr/bin/env python3
"""Build the Classic Voices NVDA add-on.

  python nvda/build.py [dll|addon|all|test] [--cc-x64 NAME] [--cc-x86 NAME]

dll    compile cvengine32.dll (x86, for NVDA before 2026.1) and cvengine64.dll
       (x64, NVDA 2026.1+) with MinGW-w64 gcc or clang, plus cvtest.exe.
addon  zip nvda/addon + the DLLs into build/classicvoices-<version>.nvda-addon,
       refusing to package any Apple-derived file.
all    both (default).

The engine is interpreter-only here: the recompiled tables (build/gen*) are
derived from Apple firmware and are never distributed. The CPU emulation
hooks (vx_set_native) stay in place, so a JIT/dynarec backend can be added
later without touching this packaging. 32-bit builds use a 512 MB guest and a
single cached engine (address space is scarce under WOW64); the largest Mac
premium voices need the 64-bit build.
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
NVDA = os.path.join(ROOT, "nvda")
SRC = os.path.join(NVDA, "src")
OUT = os.path.join(ROOT, "build", "nvda")

CORE = [os.path.join(ROOT, "core", f) for f in ("thumb.c", "vxengine.c", "vxstream.c")]
SHIM = [os.path.join(SRC, f) for f in ("vx_win.c", "mt_stub.c", "cvnvda.c", "vx_native_load.c")]
# Apple's pronunciation rules, with PCRE2 built in (tools/pcre2_build.py fetches it)
HOST = [os.path.join(ROOT, "ports", "host", f) for f in ("cv_catalog.c", "cv_rules.c")]
sys.path.insert(0, os.path.join(ROOT, "tools"))
import pcre2_build  # noqa: E402

CFLAGS = [
    "-O2", "-std=gnu11", "-ffp-contract=off", "-fvisibility=hidden",
    "-I" + os.path.join(ROOT, "core"), "-I" + SRC, "-I" + os.path.join(SRC, "compat"),
    "-I" + os.path.join(ROOT, "ports", "host"),
    "-D_WIN32_WINNT=0x0601", "-DVX_DYNAMIC_NATIVE",
]
# Force the POSIX->Windows redirections into the unmodified core sources only.
CORE_EXTRA = ["-include", os.path.join(SRC, "vx_win.h")]
LDFLAGS = ["-static", "-lwinpthread"]

APPLE_FILE_RE = re.compile(
    r"(voiced$|\.dat$|broker\.hdr|\.dmg$|\.ipsw$|\.pkg$|\.SpeechVoice|TTSResources)", re.I)


def run(cmd):
    print("+", " ".join(cmd), flush=True)
    subprocess.check_call(cmd)


def find_cc(explicit, candidates):
    for name in ([explicit] if explicit else []) + candidates:
        if name and shutil.which(name):
            return name
    return None


def build_dll(cc, bits, defines):
    os.makedirs(OUT, exist_ok=True)
    dll = os.path.join(OUT, "cvengine%d.dll" % bits)
    # core objects (with the force-include), then shims (without it)
    objs = pcre2_build.objects(cc, os.path.join(OUT, "pcre2"), str(bits))
    pcre = ["-DCV_PCRE2_STATIC", "-DPCRE2_CODE_UNIT_WIDTH=8", "-DPCRE2_STATIC", "-I" + pcre2_build.include_dir()]
    for src in CORE + SHIM + HOST:
        extra = [] if src.endswith("vx_win.c") else CORE_EXTRA
        if src.endswith(("cv_rules.c", "cvnvda.c")):
            extra = extra + pcre
        obj = os.path.join(OUT, "%d_%s.o" % (bits, os.path.splitext(os.path.basename(src))[0]))
        run([cc] + CFLAGS + defines + extra + ["-c", src, "-o", obj])
        objs.append(obj)
    run([cc, "-shared", "-o", dll, os.path.join(SRC, "cvengine.def")] + objs + LDFLAGS)
    run([cc] + CFLAGS + defines + ["-c", os.path.join(SRC, "cvtest.c"),
         "-o", os.path.join(OUT, "%d_cvtest.o" % bits)])
    run([cc, "-o", os.path.join(OUT, "cvtest%d.exe" % bits),
         os.path.join(OUT, "%d_cvtest.o" % bits), dll] + LDFLAGS)
    print("built", dll)
    return dll


def build_dlls(cc64, cc86):
    built = []
    if cc64:
        built.append(build_dll(cc64, 64, ["-DMAX_ENGINES=2"]))
    else:
        print("WARNING: no x86_64 compiler found; skipping cvengine64.dll")
    if cc86:
        built.append(build_dll(cc86, 32, ["-DGUEST_MEM_SIZE=0x20000000u", "-DMAX_ENGINES=1"]))
    else:
        print("WARNING: no i686 compiler found; skipping cvengine32.dll")
    if not built:
        sys.exit("no compiler found (want x86_64-w64-mingw32-gcc / i686-w64-mingw32-gcc or clang)")
    return built


def addon_version():
    manifest = open(os.path.join(NVDA, "addon", "manifest.ini"), encoding="utf-8").read()
    return re.search(r"^version\s*=\s*(\S+)", manifest, re.M).group(1)


def package():
    version = addon_version()
    os.makedirs(OUT, exist_ok=True)
    bundle = os.path.join(ROOT, "build", "classicvoices-%s.nvda-addon" % version)
    addon_dir = os.path.join(NVDA, "addon")
    with zipfile.ZipFile(bundle, "w", zipfile.ZIP_DEFLATED) as z:
        for base, _dirs, files in os.walk(addon_dir):
            for f in files:
                # DLLs are added from the build output below, even if a test
                # left copies in the addon tree
                if f.endswith((".pyc", ".pyo", ".dll")) or f.startswith("."):  # .DS_Store
                    continue
                p = os.path.join(base, f)
                rel = os.path.relpath(p, addon_dir)
                if APPLE_FILE_RE.search(rel.replace("\\", "/")):
                    sys.exit("refusing to package Apple-derived file: %s" % rel)
                z.write(p, rel)
        # the recompiler, so the add-on can build fast engine code from the user's own firmware
        # (_classicvoices/fastcode.py): recomp.py, its profiles (addresses only), the instruction
        # semantics its C includes, and the glue
        rc = "synthDrivers/_classicvoices/recompiler/"
        z.write(os.path.join(ROOT, "tools", "recomp.py"), rc + "recomp.py")
        for b in ("ios3", "ios4", "ios5", "ios6", "ios7", "ios10"):
            z.write(os.path.join(ROOT, "tools", "profiles", b + ".txt"), rc + "profiles/%s.txt" % b)
        for h in ("thumb.h", "thumb_ops.h", "recomp_rt.h"):
            z.write(os.path.join(ROOT, "core", h), rc + h)
        z.write(os.path.join(NVDA, "src", "native_glue.c"), rc + "native_glue.c")
        for bits in (32, 64):
            dll = os.path.join(OUT, "cvengine%d.dll" % bits)
            if not os.path.exists(dll):
                sys.exit("missing %s (run the dll step first)" % dll)
            z.write(dll, "synthDrivers/_classicvoices/cvengine%d.dll" % bits)
    print("packaged", bundle)
    return bundle


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("step", nargs="?", default="all", choices=["dll", "addon", "all"])
    ap.add_argument("--cc-x64")
    ap.add_argument("--cc-x86")
    args = ap.parse_args()
    cc64 = find_cc(args.cc_x64, ["x86_64-w64-mingw32-gcc", "x86_64-w64-mingw32-clang", "gcc"])
    cc86 = find_cc(args.cc_x86, ["i686-w64-mingw32-gcc", "i686-w64-mingw32-clang"])
    if args.step in ("dll", "all"):
        build_dlls(cc64, cc86)
    if args.step in ("addon", "all"):
        package()


if __name__ == "__main__":
    main()
