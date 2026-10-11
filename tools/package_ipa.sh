#!/bin/sh
# Build Classic Voices for iPhone without a developer account and package it
# as build/ClassicVoices.ipa, for AltStore, SideStore or Sideloadly to sign
# with your own Apple ID. Run tools/fetch_firmware.sh and
# tools/build_engines.sh first.
#
# The app is signed ad hoc only so it carries its App Group entitlement; the
# sideloading tool replaces that signature with yours.
set -e
cd "$(dirname "$0")/.."
ROOT=$(pwd)
DD=$ROOT/build/ipa-derived
OUT=$ROOT/build/ClassicVoices.ipa

(cd ios && xcodegen generate -q)
xcodebuild -project ios/ClassicVoices.xcodeproj -scheme ClassicVoices -configuration Release \
    -sdk iphoneos -destination 'generic/platform=iOS' -derivedDataPath "$DD" \
    CODE_SIGNING_ALLOWED=NO CODE_SIGNING_REQUIRED=NO CODE_SIGN_IDENTITY="" build > build/ipa-build.log 2>&1 \
    || { grep -E ': error: |^error:' build/ipa-build.log | sort -u; exit 1; }

APP=$DD/Build/Products/Release-iphoneos/ClassicVoices.app
codesign -f -s - --entitlements ios/Synth/Synth.entitlements "$APP/PlugIns/Synth.appex"
codesign -f -s - --entitlements ios/App/ClassicVoices.entitlements "$APP"

STAGE=$(mktemp -d)
mkdir "$STAGE/Payload"
ditto "$APP" "$STAGE/Payload/ClassicVoices.app"
rm -f "$OUT"
(cd "$STAGE" && zip -qry "$OUT" Payload)
rm -rf "$STAGE"
echo "built $OUT ($(du -h "$OUT" | cut -f1))"
