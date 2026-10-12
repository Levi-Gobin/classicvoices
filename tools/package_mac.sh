#!/bin/sh
# Build the Classic Voices Mac app without a developer account and zip it as
# build/ClassicVoices-mac.zip. Run tools/fetch_firmware.sh and
# tools/build_engines.sh first.
#
# Apple silicon only by default: the fast engine code takes as long again to
# compile for Intel. MAC_ARCHS="arm64 x86_64" builds both.
#
# The app and its speech extension are signed ad hoc, with their sandbox and
# App Group entitlements, which a Mac runs when the app didn't come from a
# browser download (the build scripts fetch it with GitHub's tool).
set -e
cd "$(dirname "$0")/.."
ROOT=$(pwd)
DD=$ROOT/build/mac-derived
OUT=$ROOT/build/ClassicVoices-mac.zip

(cd ios && xcodegen generate -q)
xcodebuild -project ios/ClassicVoices.xcodeproj -scheme ClassicVoicesMac -configuration Release \
    -destination 'generic/platform=macOS' -derivedDataPath "$DD" ARCHS="${MAC_ARCHS:-arm64}" ONLY_ACTIVE_ARCH=NO \
    CODE_SIGNING_ALLOWED=NO CODE_SIGNING_REQUIRED=NO CODE_SIGN_IDENTITY="" build > build/mac-build.log 2>&1 \
    || { grep -E ': error: |^error:' build/mac-build.log | sort -u; exit 1; }

APP="$DD/Build/Products/Release/Classic Voices.app"
codesign -f -s - --entitlements ios/Synth/SynthMac.entitlements "$APP/Contents/PlugIns/Synth.appex"
codesign -f -s - --entitlements ios/App/ClassicVoicesMac.entitlements "$APP"
rm -f "$OUT"
ditto -c -k --keepParent "$APP" "$OUT"
echo "built $OUT ($(du -h "$OUT" | cut -f1))"
