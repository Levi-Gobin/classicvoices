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
CORE="core/thumb.c core/vxengine.c core/vxstream.c"
HOST="ports/host/cv_catalog.c ports/host/cv_ssml.c ports/host/cv_rules.c ports/posix/vx_posix.c ports/posix/vx_native_dl.c nvda/src/mt_stub.c android/jni/cv_jni.c"
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
    wait
    # 16 KB pages: Android 15+ devices may use them
    $CC -shared -o "$OUT/apk/lib/$abi/libclassicvoices.so" $objs -llog -ldl -lm \
        -Wl,-z,max-page-size=16384 -Wl,--build-id=sha1 -Wl,-soname,libclassicvoices.so
    "$TC/llvm-strip" --strip-unneeded "$OUT/apk/lib/$abi/libclassicvoices.so"
    echo "native: $abi"
done

# ---- resources and manifest
mkdir -p "$OUT/assets"
cp nvda/addon/synthDrivers/_classicvoices/EnhancedCatalog.json nvda/addon/synthDrivers/_classicvoices/MacVoiceCatalog.json "$OUT/assets/"
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

# ---- package: native libraries stored uncompressed and page-aligned (loaded straight from the APK)
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
