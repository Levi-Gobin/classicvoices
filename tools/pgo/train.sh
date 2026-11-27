#!/bin/sh
# Speed profiles for the fast code: clang's profile-guided optimization. The generated C (build/gen*/, from
# tools/recomp.py) is compiled with counters, every voice in DATA says its language's sample sentence, a line of
# numbers and tools/corpus.txt through cvsay, and each engine's counts become tools/pgo/<build>.profdata.gz
# (sparse, indexed; ~200-400 KB). Every platform compiles its fast code with its engine's profile
# (ios/project.yml through prepare.sh, the Mac app's FastCodeBuilder, Android's FastCode.java, fastcode.py,
# linux/classicvoices-data, nvda/build_native.py): the same output, sooner (first sound ~17% sooner on iOS 7).
#
# Train again after a change to recomp.py's output (the profiles are matched to each function's C; a changed
# function is compiled as without one, so a stale profile costs speed, never correctness).
#
#   tools/pgo/train.sh DATA [JOBS]
#
# DATA: a Classic Voices data folder (CLASSICVOICES_DATA) with every engine imported (Voices/ios3 ... ios10,
# mac107-109 with their common data) and, for each engine with them, an enhanced/premium voice in Downloads
# (ios5, ios6, ios70 en-US; mac109 Allison; a Siri voice), so unit selection is trained too. Needs build/gen*
# (recomp.py over each engine), Xcode's clang and llvm-profdata, and build/pcre2-* (tools/pcre2_build.py).
set -eu
cd "$(dirname "$0")/../.."
DATA=$(cd "$1" && pwd)
JOBS=${2:-8}
W=build/pgo-train
rm -rf "$W"; mkdir -p "$W/obj" "$W/raw" "$W/text" "$W/prof"
F="-O2 -Icore -w -fno-slp-vectorize -mllvm -enable-newgvn"

echo "compiling the fast code with counters"
for g in gen gen3 gen4 gen5 gen7 gen70 gen10; do
    mkdir -p "$W/obj/$g"
    ls build/$g/*.c | xargs -P "$(sysctl -n hw.ncpu)" -I{} sh -c \
        "clang $F -fprofile-instr-generate -c {} -o $W/obj/$g/\$(basename {} .c).o"
done

echo "building cvsay with them"
PC="-DCV_PCRE2_STATIC -DPCRE2_STATIC -DPCRE2_CODE_UNIT_WIDTH=8 -I$(ls -d build/pcre2-*/src | head -1)"
CF="-O2 -std=gnu11 -ffp-contract=off -w -pthread -Icore -Iports/host"
mkdir -p "$W/so"
for f in thumb vxengine vxstream vxphonemes vxrules vxlang; do clang $CF -include ports/posix/vx_posix.h -c core/$f.c -o "$W/so/$f.o"; done
for f in cv_catalog cv_ssml cv_dict cv_rules; do clang $CF $PC -c ports/host/$f.c -o "$W/so/$f.o"; done
sed '/^#define EMPTY_TABLE/,$d' nvda/src/mt_stub.c > "$W/mt_stub.c"  # MacinTalk left out; the tables are real
echo 'void cv_set_native_dir(const char *d) { (void)d; }' > "$W/nd_stub.c"
clang $CF -Icore/mt -c "$W/mt_stub.c" -o "$W/so/mt_stub.o"
clang $CF -c "$W/nd_stub.c" -o "$W/so/nd_stub.o"
python3 -c "
import sys; sys.path.insert(0, 'tools')
from pcre2_build import objects
print('\n'.join(objects('clang', '$W/pcre', 'b_')))" > "$W/pcre.txt"
clang $CF -fprofile-instr-generate -o "$W/cvsay" linux/cvsay.c "$W"/so/*.o "$W"/obj/*/*.o build/hosttest/logstub.c \
    $(tr '\n' ' ' < "$W/pcre.txt") -lm

echo "the training"
python3 - "$W" "$DATA" <<'PY'
import ast, os, re, subprocess, sys
W, DATA = sys.argv[1], sys.argv[2]
src = open('desktop/classicvoices/library.py', encoding='utf-8').read()
samples = ast.literal_eval(re.search(r'^SAMPLES = (\{.*?^\})', src, re.S | re.M).group(1))
corpus = open('tools/corpus.txt', encoding='utf-8').read()
out = subprocess.run([W + '/cvsay', '-l'], env=dict(os.environ, CLASSICVOICES_DATA=DATA),
                     capture_output=True, text=True).stdout
with open(W + '/jobs.txt', 'w') as jobs:
    for line in out.splitlines():
        f = line.split('\t')
        if len(f) < 3:
            continue
        lang, key = f[1], f[2]
        build = key.split('/')[0]
        # the engine each voice runs on (core/vxstream.c engine_build)
        if build.startswith('mac109'):
            eng = 'ios7' if key.endswith('Compact') else 'ios10'
        elif build.startswith('mac'):
            eng = 'ios6'
        elif build == 'siri7':
            eng = 'ios7'
        else:
            eng = build.rstrip('e')
        text = samples.get(lang.split('-')[0].lower(), samples.get('en', '')) \
            + '\n1 2 3 12 345 6789 2026 10:45 3.5 1,204\n' + corpus
        name = key.replace('/', '_')
        open('%s/text/%s.txt' % (W, name), 'w', encoding='utf-8').write(text)
        jobs.write('%s %s %s\n' % (eng, key, name))
PY
cat > "$W/one.sh" <<EOF
# one.sh ENGINE KEY NAME: a profile file per process (concurrent merging into one file crashes)
LLVM_PROFILE_FILE="$PWD/$W/raw/\$1-%p.profraw" CLASSICVOICES_DATA="$DATA" "$PWD/$W/cvsay" -v "\$2" -o /dev/null \
    "\$(cat "$PWD/$W/text/\$3.txt")" > /dev/null 2>&1 || echo "\$2 failed (\$?)"
EOF
# not iOS 3: its profile made it no faster (total +1.5%, first sound the same), so it has none
for eng in ios4 ios5 ios6 ios7 ios70 ios10; do
    grep "^$eng " "$W/jobs.txt" | xargs -P "$JOBS" -n 3 sh "$W/one.sh"
    if ls "$W/raw/$eng"-*.profraw > /dev/null 2>&1; then
        xcrun llvm-profdata merge --sparse -o "$W/prof/$eng.profdata" "$W/raw/$eng"-*.profraw
        rm -f "$W/raw/$eng"-*.profraw
        gzip -9 -n -c "$W/prof/$eng.profdata" > "tools/pgo/$eng.profdata.gz"
        echo "$eng: $(grep -c "^$eng " "$W/jobs.txt") voices, $(wc -c < "tools/pgo/$eng.profdata.gz") bytes"
    else
        echo "$eng: no voices in $DATA"
    fi
done
