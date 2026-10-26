"""PCRE2, compiled into the Windows engine DLLs (the NVDA add-on's cvengine and
the SAPI engine), which run Apple's pronunciation rules with it. Linux and
Android use the system's copy instead. The source is fetched from the PCRE2
project's release (checked against its SHA-256) into build/, never committed.

    from pcre2_build import objects
    objs = objects(cc, out_dir, prefix)   # list of .o files, PCRE2_STATIC
"""
import hashlib
import os
import shutil
import subprocess
import tarfile
import urllib.request

VERSION = "10.45"
URL = "https://github.com/PCRE2Project/pcre2/releases/download/pcre2-%s/pcre2-%s.tar.bz2" % (VERSION, VERSION)
SHA256 = "21547f3516120c75597e5b30a992e27a592a31950b5140e7b8bfde3f192033c4"
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "build", "pcre2-" + VERSION)

# The library proper: not the JIT, the test programs or the table generator.
# (pcre2_printint.c and pcre2_ucptables.c are #included by other files.)
FILES = ["auto_possess", "chkdint", "compile", "compile_class", "config", "context", "convert", "dfa_match",
         "error", "extuni", "find_bracket", "maketables", "match", "match_data", "newline", "ord2utf",
         "pattern_info", "script_run", "serialize", "string_utils", "study", "substitute", "substring",
         "tables", "ucd", "valid_utf", "xclass", "chartables"]
DEFINES = ["-DHAVE_CONFIG_H", "-DPCRE2_CODE_UNIT_WIDTH=8", "-DPCRE2_STATIC", "-DSUPPORT_UNICODE",
           "-DHAVE_MEMMOVE", "-DHEAP_LIMIT=20000000", "-DLINK_SIZE=2", "-DMATCH_LIMIT=10000000",
           "-DMATCH_LIMIT_DEPTH=10000000", "-DMAX_NAME_COUNT=10000", "-DMAX_NAME_SIZE=128",
           "-DMAX_VARLOOKBEHIND=255", "-DNEWLINE_DEFAULT=2", "-DPARENS_NEST_LIMIT=250"]


def source():
    """The unpacked source with its generated headers in place; fetched once."""
    src = os.path.join(SRC, "src")
    if os.path.isfile(os.path.join(src, "pcre2.h")):
        return src
    os.makedirs(os.path.dirname(SRC), exist_ok=True)
    archive = SRC + ".tar.bz2"
    if not os.path.isfile(archive):
        print("fetching", URL, flush=True)
        with urllib.request.urlopen(URL, timeout=120) as r, open(archive + ".part", "wb") as f:
            shutil.copyfileobj(r, f)
        os.replace(archive + ".part", archive)
    digest = hashlib.sha256(open(archive, "rb").read()).hexdigest()
    if digest != SHA256:
        os.remove(archive)
        raise SystemExit("PCRE2 download doesn't match its checksum (%s)" % digest)
    with tarfile.open(archive) as t:
        t.extractall(os.path.dirname(SRC), filter="data")
    shutil.copy(os.path.join(src, "config.h.generic"), os.path.join(src, "config.h"))
    shutil.copy(os.path.join(src, "pcre2_chartables.c.dist"), os.path.join(src, "pcre2_chartables.c"))
    shutil.copy(os.path.join(src, "pcre2.h.generic"), os.path.join(src, "pcre2.h"))
    return src


def include_dir():
    return source()


def objects(cc, out_dir, prefix, extra=()):
    """Compile PCRE2 with cc (a command name) into out_dir; returns the objects."""
    src = source()
    os.makedirs(out_dir, exist_ok=True)
    objs = []
    for name in FILES:
        c = os.path.join(src, "pcre2_%s.c" % name)
        o = os.path.join(out_dir, "%s_pcre2_%s.o" % (prefix, name))
        if not os.path.isfile(o) or os.path.getmtime(o) < os.path.getmtime(c):
            subprocess.check_call([cc, "-O2", "-w", "-I", src] + DEFINES + list(extra) + ["-c", c, "-o", o])
        objs.append(o)
    return objs
