#!/bin/zsh
# Builds tests/fbstress (VoiceOver-like use of vxstream: compact fallbacks,
# pitch changes, interruptions, preloads) with Address Sanitizer, for this Mac.
#   tests/fbstress.sh            then
#   build/fbstress/fbstress_asan "<Voices root>" "<downloaded voices root>" 400 <seed>
# Voices root: Classic Voices.app/Contents/PlugIns/Synth.appex/Contents/Resources/Voices;
# downloaded voices: ~/Library/Group Containers/7WV49AN2BX.com.levigobin.ClassicVoices/EnhancedVoices.
# Needs the generated engines in build/obj*/ (tools/build_engines.sh).
set -e
cd "${0:a:h}/.."
mkdir -p build/fbstress
[ -f build/fbstress/glint.o ] || clang++ -O2 -g -Icore -w -std=c++17 -c third_party/glint/aac_decoder.cpp -o build/fbstress/glint.o
[ -f build/fbstress/mt_glint.o ] || clang++ -O2 -g -Icore -Icore/mt -w -std=c++17 -c core/mt/mt_glint.cpp -o build/fbstress/mt_glint.o
SRC=(core/*.c core/mt/*.c core/x86/*.c)
SRC=(${SRC:#*/(vxcli|mtcli|mtimports|mtsay|mtswitch).c})
OBJ=(build/obj/ios*.o build/obj3/ios*.o build/obj4/ios*.o build/obj5/ios*.o build/obj7/ios*.o build/obj10/ios*.o)
clang -O1 -g -fsanitize=address -fno-omit-frame-pointer "$@" -Icore -Icore/mt -Icore/x86 -w \
    -o build/fbstress/fbstress_asan tests/fbstress.c $SRC $OBJ build/fbstress/glint.o build/fbstress/mt_glint.o \
    -lc++ -lz -lsqlite3 -lbz2 -framework CoreFoundation -framework AudioToolbox -framework Accelerate
echo "built build/fbstress/fbstress_asan"
