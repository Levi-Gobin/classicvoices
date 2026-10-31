#!/bin/sh
# Build the Classic Voices Android app without Gradle:
#   android/build.sh            -> build/android/ClassicVoices.apk
# Needs the Android SDK (build-tools, platforms;android-34), the NDK and a JDK.
# Paths: ANDROID_HOME, ANDROID_NDK, JAVA_HOME (defaults below are this Mac's).
# ABIS="arm64-v8a" builds just one ABI (faster while developing).
# Nothing of Apple's is built in: voices are imported on the phone.
set -e
cd "$(dirname "$0")/.."
ROOT=$(pwd)
SDK=${ANDROID_HOME:-/opt/homebrew/share/android-commandlinetools}
NDK=${ANDROID_NDK:-$HOME/Library/Android/android-ndk-r30}
[ -n "$JAVA_HOME" ] || JAVA_HOME=$HOME/Library/Android/jdk/Contents/Home
export JAVA_HOME
BT=$(ls -d "$SDK"/build-tools/* | sort -V | tail -1)
JAR=$SDK/platforms/android-34/android.jar
MIN=21
ABIS=${ABIS:-"arm64-v8a armeabi-v7a x86_64 x86"}
OUT=build/android  # relative: the checkout path may contain spaces
HOSTTAG=$(ls "$NDK/toolchains/llvm/prebuilt" | head -1)
TC=$NDK/toolchains/llvm/prebuilt/$HOSTTAG/bin
rm -rf "$OUT/apk" "$OUT/classes" "$OUT/gen"
mkdir -p "$OUT/apk" "$OUT/classes" "$OUT/gen" "$OUT/obj"

# ---- native library, per ABI
CFLAGS="-O2 -std=gnu11 -ffp-contract=off -fPIC -w -fvisibility=hidden -Icore -Iports/host -Iports/posix/compat -DVX_DYNAMIC_NATIVE"
CORE="core/thumb.c core/vxengine.c core/vxstream.c core/vxphonemes.c core/vxrules.c"
HOST="ports/host/cv_catalog.c ports/host/cv_ssml.c ports/host/cv_rules.c ports/posix/vx_posix.c ports/posix/vx_native_dl.c nvda/src/mt_stub.c android/jni/cv_jni.c"
# the .xz decoder, for the Zig compiler the phone downloads to build fast code
XZ="third_party/xz-embedded/xz_crc32.c third_party/xz-embedded/xz_crc64.c third_party/xz-embedded/xz_dec_lzma2.c third_party/xz-embedded/xz_dec_stream.c third_party/xz-embedded/xz_dec_bcj.c android/jni/cv_untar.c"
XZFLAGS="-Ithird_party/xz-embedded -DXZ_USE_CRC64 -DXZ_DEC_ANY_CHECK -DXZ_DEC_X86 -DXZ_DEC_ARM -DXZ_DEC_ARMTHUMB -DXZ_DEC_ARM64"
for abi in $ABIS; do
    case $abi in
        arm64-v8a) T=aarch64-linux-android; X="" ;;
        armeabi-v7a) T=armv7a-linux-androideabi; X="-DGUEST_MEM_SIZE=0x20000000u -DMAX_ENGINES=1 -mthumb" ;;
        x86_64) T=x86_64-linux-android; X="" ;;
        x86) T=i686-linux-android; X="-DGUEST_MEM_SIZE=0x20000000u -DMAX_ENGINES=1" ;;
    esac
    CC="$TC/clang --target=$T$MIN"
    O=$OUT/obj/$abi
    mkdir -p "$O" "$OUT/apk/lib/$abi"
    objs=""
    for f in $CORE; do
        $CC $CFLAGS $X -include ports/posix/vx_posix.h -c "$f" -o "$O/$(basename "$f" .c).o" &
        objs="$objs $O/$(basename "$f" .c).o"
    done
    for f in $HOST; do
        $CC $CFLAGS $X -c "$f" -o "$O/$(basename "$f" .c).o" &
        objs="$objs $O/$(basename "$f" .c).o"
    done
    for f in $XZ; do
        $CC $CFLAGS $X $XZFLAGS -c "$f" -o "$O/$(basename "$f" .c).o" &
        objs="$objs $O/$(basename "$f" .c).o"
    done
    wait
    # 16 KB pages: Android 15+ devices may use them
    $CC -shared -o "$OUT/apk/lib/$abi/libclassicvoices.so" $objs -llog -ldl -lm \
        -Wl,-z,max-page-size=16384 -Wl,--build-id=sha1 -Wl,-soname,libclassicvoices.so
    "$TC/llvm-strip" --strip-unneeded "$OUT/apk/lib/$abi/libclassicvoices.so"
    # libcvzig.so: a program, not a library, that runs the downloaded Zig compiler (cv_zigrun.c);
    # 64-bit only, the phones that can build fast code
    case $abi in arm64-v8a|x86_64)
        $CC -O2 -fPIE -pie -Wall -o "$OUT/apk/lib/$abi/libcvzig.so" android/jni/cv_zigrun.c \
            -Wl,-z,max-page-size=16384
        "$TC/llvm-strip" "$OUT/apk/lib/$abi/libcvzig.so" ;;
    esac
    echo "native: $abi"
done

# ---- resources and manifest
rm -rf "$OUT/assets"
mkdir -p "$OUT/assets/fastcode/profiles"
cp nvda/addon/synthDrivers/_classicvoices/EnhancedCatalog.json nvda/addon/synthDrivers/_classicvoices/MacVoiceCatalog.json "$OUT/assets/"
# what the phone needs to build fast code itself (FastCode.java): the recompiler's
# profiles (addresses only) and the instruction semantics the generated C includes
cp tools/profiles/ios3.txt tools/profiles/ios4.txt tools/profiles/ios5.txt tools/profiles/ios6.txt \
    tools/profiles/ios7.txt tools/profiles/ios70.txt tools/profiles/ios10.txt "$OUT/assets/fastcode/profiles/"
cp core/thumb.h core/thumb_ops.h core/recomp_rt.h nvda/src/native_glue.c "$OUT/assets/fastcode/"
"$BT/aapt2" compile --dir android/res -o "$OUT/res.zip"
# DEBUG=1: a debuggable build, so adb can reach the app's files (run-as) while developing
DBG=""
[ "${DEBUG:-0}" = 1 ] && DBG="--debug-mode"
"$BT/aapt2" link $DBG -I "$JAR" --manifest android/AndroidManifest.xml --min-sdk-version $MIN --target-sdk-version 34 \
    --java "$OUT/gen" -A "$OUT/assets" -o "$OUT/base.apk" "$OUT/res.zip" --auto-add-overlay

# ---- Java
"$JAVA_HOME/bin/javac" -nowarn -Xlint:-options -source 8 -target 8 -encoding UTF-8 -bootclasspath "$JAR" \
    -d "$OUT/classes" $(find android/src "$OUT/gen" -name '*.java')
"$BT/d8" --min-api $MIN --release --lib "$JAR" --output "$OUT/apk" $(find "$OUT/classes" -name '*.class')

# ---- package: native libraries stored uncompressed and page-aligned. Android extracts them when
# installing (extractNativeLibs), since libcvzig.so must be a file it can start.
cp "$OUT/base.apk" "$OUT/unaligned.apk"
(cd "$OUT/apk" && zip -q -X ../unaligned.apk classes.dex && zip -q -X -0 -r ../unaligned.apk lib)
"$BT/zipalign" -f -P 16 4 "$OUT/unaligned.apk" "$OUT/aligned.apk"
KS=${KEYSTORE:-$HOME/.android/classicvoices-debug.keystore}
if [ ! -f "$KS" ]; then
    "$JAVA_HOME/bin/keytool" -genkeypair -keystore "$KS" -storepass android -keypass android -alias classicvoices \
        -keyalg RSA -keysize 2048 -validity 10000 -dname "CN=Classic Voices debug" >/dev/null 2>&1
fi
"$BT/apksigner" sign --ks "$KS" --ks-pass pass:${KEYSTORE_PASS:-android} --ks-key-alias classicvoices \
    --out "$OUT/ClassicVoices.apk" "$OUT/aligned.apk"
rm -f "$OUT/unaligned.apk" "$OUT/aligned.apk" "$OUT/base.apk"
# refuse to package anything of Apple's
if unzip -l "$OUT/ClassicVoices.apk" | grep -E -q 'voiced|\.dat$|broker\.hdr|cvnative_'; then
    echo "refusing: Apple-derived files in the APK" >&2; exit 1
fi
echo "built $ROOT/$OUT/ClassicVoices.apk ($(du -h "$OUT/ClassicVoices.apk" | cut -f1))"
