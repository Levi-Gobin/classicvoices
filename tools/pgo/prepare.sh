#!/bin/sh
# Speed profiles for the fast code (tools/pgo/<build>.profdata.gz, made by tools/pgo/train.sh): each engine's,
# unpacked into OUTDIR as <build>.profdata for clang's -fprofile-instr-use (ios/project.yml compiles each
# build/gen*/ folder with its engine's). A profile this toolchain can't read (an older Xcode) or a missing one
# becomes an empty profile: the code is then compiled as it was without one, never left unbuilt.
#   tools/pgo/prepare.sh OUTDIR
set -u
here=$(cd "$(dirname "$0")" && pwd)
out=$1
mkdir -p "$out"
tool=""
if command -v xcrun >/dev/null 2>&1 && xcrun --find llvm-profdata >/dev/null 2>&1; then tool="xcrun llvm-profdata"
elif command -v llvm-profdata >/dev/null 2>&1; then tool="llvm-profdata"
fi
for b in ios3 ios4 ios5 ios6 ios7 ios70 ios10; do
    gz="$here/$b.profdata.gz"
    dst="$out/$b.profdata"
    # CV_NO_PGO (a build setting): empty profiles, to compare against (then built in a derived data of its own)
    if [ -n "${CV_NO_PGO:-}" ]; then rm -f "$dst" "$dst.from"; gz=/nonexistent; fi
    # unchanged since the last build: keep it (Xcode then sees nothing new to compile)
    if [ -f "$dst" ] && [ -f "$gz" ] && [ "$dst.from" -nt "$gz" ] 2>/dev/null; then continue; fi
    ok=0
    if [ -f "$gz" ] && gunzip -c "$gz" > "$dst.tmp" 2>/dev/null; then
        if [ -z "$tool" ] || $tool show "$dst.tmp" >/dev/null 2>&1; then ok=1; fi
    fi
    if [ $ok = 1 ]; then
        mv -f "$dst.tmp" "$dst"
    else
        rm -f "$dst.tmp"
        : > "$out/empty.proftext"
        if [ -n "$tool" ]; then $tool merge -o "$dst" "$out/empty.proftext"; fi
        [ "$b" = ios3 ] || echo "note: no speed profile for $b (compiled without one)"
    fi
    [ -n "${CV_NO_PGO:-}" ] || touch "$dst.from"  # (an empty one made for CV_NO_PGO isn't kept)
done
exit 0
