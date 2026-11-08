#!/bin/sh
# Build Classic Voices for iPhone (default) or Apple Vision Pro
# without a developer account and package it as an .ipa for AltStore,
# SideStore or Sideloadly to sign with your own Apple ID:
#
#   tools/package_ipa.sh            -> build/ClassicVoices.ipa        (iPhone and iPad)
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
visionpro) SCHEME=ClassicVoicesVision; SDK=xros;        PLATFORM=visionOS; NAME=ClassicVoicesVision ;;
*) echo "usage: $0 [iphone|visionpro]" >&2; exit 2 ;;
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
# Sideloadly 0.71 signs every Mach-O file in the app and fails on one that has
# no signature yet ("macho sign/edit failed (-11)"): Tiger's MacinTalk and
# SpeechDictionary. Sign those ad hoc so it only replaces a signature. Only
# their header and the end of __LINKEDIT change, not the code fast code checks.
# Each is signed on its own in a temporary folder: signed in place, the
# SpeechDictionary folder would be taken for a bundle and get a _CodeSignature.
SIGN=$(mktemp -d)
find "$APP/PlugIns/Synth.appex/Voices" -type f | while IFS= read -r F; do
    case $(head -c 4 "$F" | od -An -tx1 | tr -d ' \n') in
    cafebabe|feedface|cefaedfe|feedfacf|cffaedfe) ;;
    *) continue ;;
    esac
    codesign -dv "$F" 2>&1 | grep -q "not signed at all" || continue
    cp "$F" "$SIGN/$(basename "$F")"
    codesign -s - "$SIGN/$(basename "$F")" || exit 1
    mv "$SIGN/$(basename "$F")" "$F"
done
rm -rf "$SIGN"
codesign -f -s - --entitlements ios/Synth/Synth.entitlements "$APP/PlugIns/Synth.appex"
codesign -f -s - --entitlements ios/App/ClassicVoices.entitlements "$APP"

STAGE=$(mktemp -d)
mkdir "$STAGE/Payload"
ditto "$APP" "$STAGE/Payload/ClassicVoices.app"
rm -f "$OUT"
(cd "$STAGE" && zip -qry "$OUT" Payload)
rm -rf "$STAGE"
echo "built $OUT ($(du -h "$OUT" | cut -f1))"
