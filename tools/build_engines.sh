#!/bin/sh
# Generate the recompiled engines (build/gen, build/gen3 ... build/gen10) from
# the firmware in extracted/. Nothing here ships Apple code: the C is produced
# from your own copy of each engine, guided by tools/profiles/*.txt (lists of
# addresses the engine ran while speaking tools/corpus.txt).
#
# Stripped engines (iOS 3, 4, 5) take their function boundaries from their BL
# targets (recomp.py --scan-bl); the others have symbols.
set -e
cd "$(dirname "$0")/.."
PY=${PYTHON:-python3}
mkdir -p build

recomp() {  # <prefix> <engine binary> <outdir> [stripped]
    prefix=$1 bin=$2 out=$3 kind=$4
    if [ ! -f "$bin" ]; then echo "skip $prefix: $bin not found"; return; fi
    set --
    if [ "$kind" = stripped ]; then set -- --scan-bl; fi
    rm -rf "$out"
    "$PY" -I tools/recomp.py "$bin" "tools/profiles/$prefix.txt" "$out" --prefix "$prefix" "$@"
}

E=extracted
recomp ios3 $E/ios3/VoiceServices.framework/Support/voiced build/gen3 stripped
recomp ios4 $E/ios4/VoiceServices.framework/Support/voiced build/gen4 stripped
recomp ios5 $E/ios5/VoiceServices.framework/Support/voiced build/gen5 stripped
recomp ios6 $E/ios6/VoiceServices.framework/Support/voiced build/gen
recomp ios7 $E/ios7/VoiceServices.framework/Support/voiced build/gen7
recomp ios70 $E/ios70/VoiceServices.framework/Support/voiced build/gen70
recomp ios10 $E/ios10/TTSSpeechBundle.speechbundle/TTSSpeechBundle build/gen10
