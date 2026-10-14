#!/bin/sh
# Copy Mac OS X Lion's MacinTalk engine and voices (Fred, Victoria, Bells and
# the rest; not Alex, who is several hundred megabytes) out of the Lion
# installer Apple offers as a free download, into extracted/mt107/.
#
#   tools/fetch_macintalk.sh
#
# LION_DMG=<path to InstallMacOSX.dmg> uses a copy you already have instead of
# downloading 4.7 GB. OUT=<dir> writes somewhere other than extracted/mt107.
# Needs macOS (hdiutil). The installer's own disk image is unpacked next to it
# while this runs (another 4.7 GB) and deleted afterwards.
set -e
cd "$(dirname "$0")/.."
ROOT=$(pwd)
PY=${PYTHON:-python3}
WORK=${WORK:-$ROOT/build/lion}
OUT=${OUT:-$ROOT/extracted/mt107}
URL=https://updates.cdn-apple.com/2021/macos/041-7683-20210614-E610947E-C7CE-46EB-8860-D26D71F0D3EA/InstallMacOSX.dmg

mkdir -p "$WORK"
cleanup() {
    hdiutil detach "$WORK/base" >/dev/null 2>&1 || true
    hdiutil detach "$WORK/outer" >/dev/null 2>&1 || true
    hdiutil detach "$WORK/esd" >/dev/null 2>&1 || true
    rm -rf "$WORK/InstallESD.dmg" "$WORK/tree"
}
trap cleanup EXIT

DMG=$LION_DMG
if [ -z "$DMG" ]; then
    DMG=$WORK/InstallMacOSX.dmg
    [ -f "$DMG" ] || curl -fL --retry 3 -o "$DMG" "$URL"
fi

# InstallMacOSX.dmg > InstallMacOSX.pkg (xar) > InstallESD.dmg > Packages/*.pkg
hdiutil attach -readonly -nobrowse -noverify -mountpoint "$WORK/outer" "$DMG" >/dev/null
"$PY" -I tools/xar_member.py "$WORK/outer/InstallMacOSX.pkg" InstallESD.dmg > "$WORK/InstallESD.dmg"
hdiutil detach "$WORK/outer" >/dev/null
[ -n "$LION_DMG" ] || rm -f "$DMG"
hdiutil attach -readonly -nobrowse -noverify -mountpoint "$WORK/esd" "$WORK/InstallESD.dmg" >/dev/null

T=$WORK/tree
rm -rf "$T"
mkdir -p "$T"
for p in BaseSystemBinaries Essentials AdditionalSpeechVoices; do
    echo "== $p"
    # Each package holds only some of these, and tar says so about the rest;
    # anything really missing stops the copy below.
    "$PY" -I tools/xar_member.py "$WORK/esd/Packages/$p.pkg" Payload | tar -xzf - -C "$T" \
        --exclude '*/Alex.SpeechVoice/*' \
        --include '*System/Library/Speech/Synthesizers/MacinTalk.SpeechSynthesizer/*' \
        --include '*System/Library/Speech/Voices/*' \
        --include '*System/Library/PrivateFrameworks/SpeechDictionary.framework/*' \
        --include '*usr/lib/libc++abi.dylib' \
        --include '*usr/lib/libstdc++.6.0.9.dylib' 2>/dev/null || true
done
# Fred and the dictionary's data are only on the installer's base system disk,
# whose programs are 64-bit only; so programs come from the packages above,
# and data from here.
hdiutil attach -readonly -nobrowse -noverify -mountpoint "$WORK/base" "$WORK/esd/BaseSystem.dmg" >/dev/null
BS=$WORK/base

# The layout the engine loads: MacinTalk, SpeechDictionary/, lib/, Voices/.
rm -rf "$OUT"
mkdir -p "$OUT/lib" "$OUT/Voices"
cp "$T/System/Library/Speech/Synthesizers/MacinTalk.SpeechSynthesizer/Contents/MacOS/MacinTalk" "$OUT/MacinTalk"
cp -R "$T/System/Library/PrivateFrameworks/SpeechDictionary.framework/Versions/A" "$OUT/SpeechDictionary"
cp -R "$BS/System/Library/PrivateFrameworks/SpeechDictionary.framework/Versions/A/Resources/" "$OUT/SpeechDictionary/Resources/"
cp "$T/usr/lib/libc++abi.dylib" "$T/usr/lib/libstdc++.6.0.9.dylib" "$OUT/lib/"
n=0
for v in "$T"/System/Library/Speech/Voices/*.SpeechVoice "$BS"/System/Library/Speech/Voices/*.SpeechVoice; do
    # MacinTalk voices only; the Nuance "Compact" ones run on another engine
    [ -f "$v/Contents/Resources/VoiceDescription" ] || continue
    [ -e "$OUT/Voices/$(basename "$v")" ] && continue
    cp -R "$v" "$OUT/Voices/"
    n=$((n + 1))
done
hdiutil detach "$WORK/base" >/dev/null
hdiutil detach "$WORK/esd" >/dev/null
rm -rf "$WORK/InstallESD.dmg" "$T"
echo "== Lion MacinTalk with $n voices in $OUT"
