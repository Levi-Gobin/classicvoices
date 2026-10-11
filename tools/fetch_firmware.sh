#!/bin/sh
# Download the iPhone firmware Classic Voices runs, from Apple's servers, and
# copy out only the speech engine and voices into extracted/<build>/.
#
#   tools/fetch_firmware.sh [ios3 ios4 ios5 ios6 ios7 ios10]   (default: all)
#
# Each IPSW is fetched by tools/ipswtool.py (keys for the encrypted ones come
# from The Apple Wiki), mounted read-only, copied from, and deleted again, so
# only one firmware is on disk at a time. Needs macOS (hdiutil) and Python with
# `cryptography`.
set -e
cd "$(dirname "$0")/.."
ROOT=$(pwd)
PY=${PYTHON:-python3}
WORK=${WORK:-$ROOT/build/firmware}

# build -> version, device. iPhone 3GS firmware for the classic voices; iOS 7
# and 10 are only engines for the Mac OS X Mavericks voices.
spec() {
    case $1 in
        ios3) echo "3.1.3 iPhone2,1" ;;
        ios4) echo "4.3.5 iPhone2,1" ;;
        ios5) echo "5.1.1 iPhone2,1" ;;
        ios6) echo "6.1.6 iPhone2,1" ;;
        ios7) echo "7.1.2 iPhone3,1" ;;
        ios10) echo "10.3.4 iPhone5,1" ;;
        *) echo "unknown build $1" >&2; exit 2 ;;
    esac
}

fetch() {
    b=$1
    set -- $(spec "$b")
    ver=$1 dev=$2
    dir=$WORK/$b
    rm -rf "$dir"
    mkdir -p "$dir"
    echo "== $b: iOS $ver for $dev"
    "$PY" -I tools/ipswtool.py ios "$ver" "$dev" -y --no-mount -o "$dir"
    rm -f "$dir"/*.ipsw
    # the root filesystem is the largest disk image in the firmware
    dmg=$(ls -S "$dir"/*_extracted/*.dmg | head -1)
    mnt=$dir/mnt
    mkdir -p "$mnt"
    hdiutil attach -readonly -nobrowse -noverify -mountpoint "$mnt" "$dmg" >/dev/null
    out=$ROOT/extracted/$b
    rm -rf "$out"
    mkdir -p "$out"
    if [ "$b" = ios10 ]; then
        ditto "$mnt/System/Library/TTSPlugins/TTSSpeechBundle.speechbundle" "$out/TTSSpeechBundle.speechbundle"
    else
        ditto "$mnt/System/Library/PrivateFrameworks/VoiceServices.framework" "$out/VoiceServices.framework"
    fi
    # iOS 7 is only the engine for the Mavericks voices; its own voices aren't used
    if [ "$b" = ios7 ]; then
        find "$out/VoiceServices.framework/TTSResources" -mindepth 1 -maxdepth 1 -type d ! -name common -exec rm -rf {} +
    fi
    hdiutil detach "$mnt" >/dev/null
    rm -rf "$dir"
    echo "   -> extracted/$b ($(du -sh "$out" | cut -f1))"
}

# The Mac OS X voices you can download (Lion, Mountain Lion and Mavericks
# premium voices, and the compacts in their packages) share a small common
# folder. Its synthesis files are the same as these firmwares'; lid.dat
# (language identification) differs but doesn't change the speech.
mac_common() {
    era=$1; shift
    out=$ROOT/extracted/$era/common
    mkdir -p "$out"
    for pair in "$@"; do
        b=${pair%%:*} f=${pair#*:}
        src=$ROOT/extracted/$b/VoiceServices.framework/TTSResources/common/$f
        [ -f "$src" ] || return 0
        [ -f "$out/$f" ] || cp "$src" "$out/$f"
    done
}

builds=${*:-ios3 ios4 ios5 ios6 ios7 ios10}
for b in $builds; do fetch "$b"; done
mac_common mac107 ios5:lid.dat ios6:synth_med_fxd_bet2f22.dat
mac_common mac108 ios6:lid.dat ios6:synth_med_fxd_bet2f22.dat ios7:synth_med_fxd_bet3f22.dat
mac_common mac109 ios7:lid.dat ios7:synth_med_fxd_bet3f22.dat
