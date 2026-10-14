#!/bin/zsh
# Builds tests/mt_stress (the MacinTalk stream stress test) for this Mac.
#   tests/mt_stress.sh [extra clang flags, e.g. -fsanitize=address]
set -e
cd "${0:a:h}/.."
mkdir -p build/mt_stress
SRC=(core/*.c core/mt/*.c core/x86/*.c)
SRC=(${SRC:#*/(vxcli|mtcli|mtimports|mtsay|mtswitch).c})
clang++ -O2 -g -Icore -w -std=c++17 -c third_party/glint/aac_decoder.cpp -o build/mt_stress/glint.o
clang++ -O2 -g -Icore -Icore/mt -w -std=c++17 -c core/mt/mt_glint.cpp -o build/mt_stress/mt_glint.o
# The recompiled Vocalizer engines are optional (weak references).
U=()
for b in ios3 ios4 ios5 ios6 ios7 ios10; do U+=(-Wl,-U,_vx_native_$b -Wl,-U,_vx_native_${b}_count); done
clang -O2 -g "$@" -Icore -Icore/mt -Icore/x86 -w -o build/mt_stress/mt_stress tests/mt_stress.c $SRC \
    build/mt_stress/glint.o build/mt_stress/mt_glint.o -lc++ -lz -lsqlite3 -lbz2 \
    -framework CoreFoundation -framework AudioToolbox -framework Accelerate $U
echo "built build/mt_stress/mt_stress: run it as  build/mt_stress/mt_stress extracted mt106/Alex 3000"
