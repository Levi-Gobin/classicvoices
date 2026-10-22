#!/bin/sh
# Build Classic Voices for Linux and package it:
#   linux/build.sh            -> build/linux/classicvoices-linux-<arch>.tar.gz
# Needs a C compiler (CC=clang by default; gcc works too) and nothing else:
# the Speech Dispatcher module carries its own copy of Speech Dispatcher's
# module helpers. Nothing of Apple's is built in or packaged; users import
# firmware afterwards with classicvoices-data.
set -e
cd "$(dirname "$0")/.."
CC=${CC:-$(command -v clang || command -v cc)}
ARCH=${ARCH:-$(uname -m)}
OUT=build/linux
rm -rf "$OUT/stage"
mkdir -p "$OUT/stage/bin" "$OUT/stage/libexec" "$OUT/stage/lib/classicvoices"
CFLAGS="-O2 -std=gnu11 -ffp-contract=off -fPIC -w -pthread -Icore -Iports/host -Iports/posix/compat -Ilinux/speechd -DVX_DYNAMIC_NATIVE ${EXTRA_CFLAGS}"
CORE="core/thumb.c core/vxengine.c core/vxstream.c"
HOST="ports/host/cv_catalog.c ports/host/cv_ssml.c ports/host/cv_rules.c ports/posix/vx_posix.c ports/posix/vx_native_dl.c nvda/src/mt_stub.c"
OBJ=$OUT/obj
mkdir -p "$OBJ"
for f in $CORE; do $CC $CFLAGS -include ports/posix/vx_posix.h -c "$f" -o "$OBJ/$(basename "$f" .c).o"; done
for f in $HOST linux/speechd/module_main.c linux/speechd/module_process.c linux/speechd/module_readline.c; do
    $CC $CFLAGS -c "$f" -o "$OBJ/$(basename "$f" .c).o"
done
LIBS="-lm -ldl -pthread"
COMMON="$OBJ/thumb.o $OBJ/vxengine.o $OBJ/vxstream.o $OBJ/cv_catalog.o $OBJ/cv_ssml.o $OBJ/cv_rules.o $OBJ/vx_posix.o $OBJ/vx_native_dl.o $OBJ/mt_stub.o"
$CC $CFLAGS -o "$OUT/stage/libexec/sd_classicvoices" linux/sd_classicvoices.c $COMMON \
    "$OBJ/module_main.o" "$OBJ/module_process.o" "$OBJ/module_readline.o" $LIBS
$CC $CFLAGS -o "$OUT/stage/bin/classicvoices-say" linux/cvsay.c $COMMON $LIBS
cp linux/classicvoices-data "$OUT/stage/bin/"
# the importer and the fast-engine recompiler (pure Python)
L=$OUT/stage/lib/classicvoices
cp -R nvda/addon/synthDrivers/_classicvoices "$L/"
rm -rf "$L/_classicvoices/__pycache__" "$L/_classicvoices"/*.dll
mkdir -p "$L/tools/profiles" "$L/core"
cp tools/recomp.py "$L/tools/"
cp tools/profiles/*.txt "$L/tools/profiles/"
cp core/thumb.h core/thumb_ops.h core/recomp_rt.h "$L/core/"
cp nvda/src/native_glue.c "$L/"
cp linux/install.sh linux/README.md LICENSE "$OUT/stage/"
# refuse to package anything of Apple's
if find "$OUT/stage" \( -name voiced -o -name '*.dat' -o -name 'broker.hdr*' -o -name '*.ipsw' -o -name 'cvnative_*' \) | grep -q .; then
    echo "refusing to package Apple-derived files" >&2; exit 1
fi
TAR=$OUT/classicvoices-linux-$ARCH.tar.gz
tar -czf "$TAR" -C "$OUT" --transform "s,^stage,classicvoices," stage 2>/dev/null || \
    (cd "$OUT" && rm -rf classicvoices && cp -R stage classicvoices && tar -czf "classicvoices-linux-$ARCH.tar.gz" classicvoices && rm -rf classicvoices)
echo "built $TAR"
