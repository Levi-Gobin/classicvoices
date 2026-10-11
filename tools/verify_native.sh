#!/bin/sh
# Compare recompiled vs interpreted output over the corpus. Prints mismatches.
#   verify_native.sh <lang> [build]            built-in voice, e.g. en-US ios3
#   verify_native.sh <lang_dir> <build>        any voice directory (e.g. extracted/ios6enh/en-US)
cd "$(dirname "$0")/.."
BUILD=${2:-ios6}
R=extracted/$BUILD/VoiceServices.framework
case "$1" in */*) LDIR=$1 ;; *) LDIR=$R/TTSResources/$1 ;; esac
T=$(mktemp -d)
n=0; bad=0
while IFS= read -r line; do
  n=$((n+1))
  VX_INTERP=1 ./build/vxcli_native $R/Support/voiced $LDIR $R/TTSResources/common "$line" $T/a.wav >/dev/null 2>$T/ea || echo "interp failed: $line: $(cat $T/ea)"
  ./build/vxcli_native $R/Support/voiced $LDIR $R/TTSResources/common "$line" $T/b.wav >/dev/null 2>$T/eb || echo "native failed: $line: $(cat $T/eb)"
  cmp -s $T/a.wav $T/b.wav || { bad=$((bad+1)); echo "MISMATCH: $line"; }
done < tools/corpus.txt
echo "$BUILD $LDIR: $n lines, $bad mismatches"
rm -rf $T
