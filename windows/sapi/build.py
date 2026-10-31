#!/usr/bin/env python3
"""Build the Classic Voices SAPI 5 engine for Windows.

  python windows/sapi/build.py [--cc-x64 NAME] [--cc-x86 NAME] [--cc-arm64 NAME]

Writes build/sapi/cvsapi64.dll (64-bit programs) and cvsapi32.dll (32-bit
programs), plus cvsapitest64/32.exe, with MinGW-w64 gcc or clang (the same
compilers nvda/build.py uses; CI cross-compiles from Linux). --cc-arm64 adds
cvsapi_arm64.dll for native Arm programs. It isn't installed yet: on Windows on
Arm, x64 and Arm programs share one registration, which needs an ARM64X
forwarder (see NOTES.md); the x64 build serves x64 programs and ARM64EC ones
(NVDA) there. Like the NVDA
add-on, nothing of Apple's is built in: the engine reads the user's own data
folder, and the optional fast code (cvnative_<build>_<bits>.dll) is the same
as the add-on's.
"""
import argparse
import os
import shutil
import subprocess
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))), "tools"))
import pcre2_build  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
HERE = os.path.join(ROOT, "windows", "sapi")
NVDA_SRC = os.path.join(ROOT, "nvda", "src")
OUT = os.path.join(ROOT, "build", "sapi")

CORE = [os.path.join(ROOT, "core", f) for f in ("thumb.c", "vxengine.c", "vxstream.c", "vxphonemes.c", "vxrules.c")]
SHIM = [os.path.join(NVDA_SRC, f) for f in ("vx_win.c", "mt_stub.c", "vx_native_load.c")]
HOST = [os.path.join(ROOT, "ports", "host", "cv_catalog.c"), os.path.join(HERE, "cvsapi.c")]
RULES = os.path.join(ROOT, "ports", "host", "cv_rules.c")  # with UTF-8 paths and PCRE2 built in

CFLAGS = [
    "-O2", "-std=gnu11", "-ffp-contract=off", "-fvisibility=hidden",
    "-I" + os.path.join(ROOT, "core"), "-I" + NVDA_SRC, "-I" + os.path.join(NVDA_SRC, "compat"),
    "-I" + os.path.join(ROOT, "ports", "host"),
    "-D_WIN32_WINNT=0x0601", "-DVX_DYNAMIC_NATIVE",
]
CORE_EXTRA = ["-include", os.path.join(NVDA_SRC, "vx_win.h")]
LIBS = ["-lole32", "-loleaut32", "-luuid", "-ladvapi32"]
LDFLAGS = ["-static", "-lwinpthread"]


def run(cmd):
    print("+", " ".join(cmd), flush=True)
    subprocess.check_call(cmd)


def find_cc(explicit, candidates):
    for name in ([explicit] if explicit else []) + candidates:
        if name and shutil.which(name):
            return name
    return None


def build(cc, bits, defines):
    os.makedirs(OUT, exist_ok=True)
    objs = pcre2_build.objects(cc, os.path.join(OUT, "pcre2"), str(bits))
    pcre = ["-DCV_PCRE2_STATIC", "-DPCRE2_CODE_UNIT_WIDTH=8", "-DPCRE2_STATIC", "-I" + pcre2_build.include_dir()]
    for src in CORE + SHIM + HOST + [RULES]:
        extra = CORE_EXTRA if src in CORE or src == RULES else []
        if src in (RULES, HOST[1]):
            extra = extra + pcre
        obj = os.path.join(OUT, "%s_%s.o" % (bits, os.path.splitext(os.path.basename(src))[0]))
        run([cc] + CFLAGS + defines + extra + ["-c", src, "-o", obj])
        objs.append(obj)
    dll = os.path.join(OUT, "cvsapi_arm64.dll" if bits == "arm64" else "cvsapi%s.dll" % bits)
    run([cc, "-shared", "-o", dll, os.path.join(HERE, "cvsapi.def")] + objs + LIBS + LDFLAGS)
    test = os.path.join(OUT, "cvsapitest%s.exe" % bits)
    run([cc] + CFLAGS + ["-o", test, os.path.join(HERE, "cvsapitest.c")] + LIBS + LDFLAGS)
    print("built", dll)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cc-x64")
    ap.add_argument("--cc-x86")
    ap.add_argument("--cc-arm64")
    args = ap.parse_args()
    cc64 = find_cc(args.cc_x64, ["x86_64-w64-mingw32-gcc", "x86_64-w64-mingw32-clang"])
    cc86 = find_cc(args.cc_x86, ["i686-w64-mingw32-gcc", "i686-w64-mingw32-clang"])
    if not cc64 and not cc86:
        sys.exit("no compiler found (want x86_64-w64-mingw32-gcc / i686-w64-mingw32-gcc or clang)")
    if cc64:
        build(cc64, 64, ["-DMAX_ENGINES=2"])
    if cc86:
        # 32-bit programs: a 512 MB guest and one cached engine, as cvengine32.dll
        build(cc86, 32, ["-DGUEST_MEM_SIZE=0x20000000u", "-DMAX_ENGINES=1"])
    cca = find_cc(args.cc_arm64, ["aarch64-w64-mingw32-clang"])
    if cca:
        build(cca, "arm64", ["-DMAX_ENGINES=2"])


if __name__ == "__main__":
    main()
