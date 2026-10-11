#!/bin/sh
# Build the Mac test CLIs: build/vxcli (interpreter only) and build/vxcli_native (recompiled).
set -e
cd "$(dirname "$0")/.."
CFLAGS="-O2 -Icore -Wno-unused-label -Wno-unused-variable -Wno-unused-function"
mkdir -p build/obj
mkdir -p build/obj3
ls build/gen/*.c | xargs -P "$(sysctl -n hw.ncpu)" -I{} sh -c "clang $CFLAGS -c {} -o build/obj/\$(basename {} .c).o"
ls build/gen3/*.c | xargs -P "$(sysctl -n hw.ncpu)" -I{} sh -c "clang $CFLAGS -c {} -o build/obj3/\$(basename {} .c).o"
for g in 4 5 7 10; do mkdir -p build/obj$g; ls build/gen$g/*.c | xargs -P "$(sysctl -n hw.ncpu)" -I{} sh -c "clang $CFLAGS -c {} -o build/obj$g/\$(basename {} .c).o"; done
clang $CFLAGS -o build/vxcli core/thumb.c core/vxengine.c core/vxcli.c
clang $CFLAGS -DVX_HAVE_NATIVE -o build/vxcli_native core/thumb.c core/vxengine.c core/vxcli.c build/obj/ios6_*.o build/obj3/ios3_*.o build/obj4/ios4_*.o build/obj5/ios5_*.o build/obj7/ios7_*.o build/obj10/ios10_*.o
