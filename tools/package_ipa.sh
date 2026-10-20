#!/bin/sh
# Build Classic Voices for iPhone (default), Apple TV or Apple Vision Pro
# without a developer account and package it as an .ipa for AltStore,
# SideStore or Sideloadly to sign with your own Apple ID:
#
#   tools/package_ipa.sh            -> build/ClassicVoices.ipa        (iPhone and iPad)
#   tools/package_ipa.sh appletv    -> build/ClassicVoicesTV.ipa      (Apple TV)
#   tools/package_ipa.sh visionpro  -> build/ClassicVoicesVision.ipa  (Apple Vision Pro)
#
# Run tools/fetch_firmware.sh and tools/build_engines.sh first.
#
# The app is signed ad hoc only so it carries its App Group entitlement; the
# sideloading tool replaces that signature with yours.
set -e
cd "$(dirname "$0")/.."
ROOT=$(pwd)

case "${1:-iphone}" in
iphone)    SCHEME=ClassicVoices;       SDK=iphoneos;    PLATFORM=iOS;      NAME=ClassicVoices ;;
appletv)   SCHEME=ClassicVoicesTV;     SDK=appletvos;   PLATFORM=tvOS;     NAME=ClassicVoicesTV ;;
visionpro) SCHEME=ClassicVoicesVision; SDK=xros;        PLATFORM=visionOS; NAME=ClassicVoicesVision ;;
*) echo "usage: $0 [iphone|appletv|visionpro]" >&2; exit 2 ;;
esac
DD=$ROOT/build/ipa-derived
OUT=$ROOT/build/$NAME.ipa
LOG=$ROOT/build/$NAME-build.log

(cd ios && xcodegen generate -q)
xcodebuild -project ios/ClassicVoices.xcodeproj -scheme "$SCHEME" -configuration Release \
    -sdk "$SDK" -destination "generic/platform=$PLATFORM" -derivedDataPath "$DD" \
    CODE_SIGNING_ALLOWED=NO CODE_SIGNING_REQUIRED=NO CODE_SIGN_IDENTITY="" build > "$LOG" 2>&1 \
    || { grep -E ': error: |^error:' "$LOG" | sort -u; exit 1; }

APP=$DD/Build/Products/Release-$SDK/ClassicVoices.app
codesign -f -s - --entitlements ios/Synth/Synth.entitlements "$APP/PlugIns/Synth.appex"
codesign -f -s - --entitlements ios/App/ClassicVoices.entitlements "$APP"

STAGE=$(mktemp -d)
mkdir "$STAGE/Payload"
ditto "$APP" "$STAGE/Payload/ClassicVoices.app"
rm -f "$OUT"
(cd "$STAGE" && zip -qry "$OUT" Payload)
rm -rf "$STAGE"
echo "built $OUT ($(du -h "$OUT" | cut -f1))"
