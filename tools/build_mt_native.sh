#!/bin/zsh
# Generate recompiled MacinTalk code (build/mtgen104 ... build/mtgen107) from
# the generations in extracted/mt104..mt107. Nothing here ships Apple code: the
# C is produced from your own copy of each engine, guided by
# tools/profiles/mtNNN.txt (lists of addresses the engine ran while speaking).
#
#   tools/build_mt_native.sh [mt105 ...]        generate (default: all present)
#   tools/build_mt_native.sh --profile mt105    re-record that profile first
#
# Each engine is loaded by the host harness (tests/mt_golden.c) with MT_DUMP,
# which writes the images exactly as MacinTalk's loader leaves them in guest
# memory; tools/recomp_x86.py translates from that.
set -e
cd "${0:a:h}/.."
PY=${PYTHON:-python3}
profile=0
if [ "$1" = --profile ]; then profile=1; shift; fi
gens=("$@")
[ ${#gens} -gt 0 ] || gens=(mt104 mt105 mt106 mt107)

H=build/mtn/mt_golden
mkdir -p build/mtn
SRC=(core/mt/*.c core/x86/*.c)
SRC=(${SRC:#*/(mtcli|mtimports|mtsay|mtswitch).c})
clang++ -O2 -Icore -w -std=c++17 -c third_party/glint/aac_decoder.cpp -o build/mtn/glint.o
clang++ -O2 -Icore -Icore/mt -w -std=c++17 -c core/mt/mt_glint.cpp -o build/mtn/mt_glint.o
clang -O2 -Icore -Icore/mt -Icore/x86 -w -o $H tests/mt_golden.c $SRC build/mtn/glint.o build/mtn/mt_glint.o \
    -lc++ -lz -lsqlite3 -lbz2 -framework CoreFoundation -framework AudioToolbox -framework Accelerate

for g in $gens; do
    root=extracted/$g
    if [ ! -f $root/MacinTalk ]; then echo "skip $g: $root/MacinTalk not found"; continue; fi
    if [ $profile = 1 ]; then
        rm -f tools/profiles/$g.txt.new
        MT_NATIVE=0 MT_PROFILE=tools/profiles/$g.txt.new MT_GOLDEN_CORPUS=tools/corpus.txt $H $root > /dev/null
        { echo "# Places MacinTalk $g ran (image, address before sliding) while speaking tests/mt_golden.c's"
          echo "# utterances and tools/corpus.txt with every voice. Addresses only; input to tools/recomp_x86.py."
          cat tools/profiles/$g.txt.new; } > tools/profiles/$g.txt
        rm -f tools/profiles/$g.txt.new
    fi
    rm -rf build/mtn/dump_$g build/mtgen${g#mt}
    MT_NATIVE=0 MT_DUMP=build/mtn/dump_$g $H $root NoSuchVoice > /dev/null 2>&1 || true
    "$PY" -I tools/recomp_x86.py build/mtn/dump_$g tools/profiles/$g.txt build/mtgen${g#mt} --prefix $g
    rm -rf build/mtn/dump_$g
done
