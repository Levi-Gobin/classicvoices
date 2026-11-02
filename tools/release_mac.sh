#!/bin/sh
# Build the released Mac app (the ClassicVoicesMacPublic target: nothing of
# Apple's in it; it fetches the engines and builds their fast code on each
# Mac), have Apple notarize it, and package it:
#   build/release-mac/ClassicVoices-mac.dmg   for the first install
#   build/release-mac/ClassicVoices-mac.zip   what the app's updater installs
#
#   NOTARY_PROFILE=<name> tools/release_mac.sh
#
# NOTARY_PROFILE is the keychain profile `xcrun notarytool store-credentials`
# saved. Signs with the Developer ID Application certificate in the keychain.
# Universal (Apple silicon and Intel).
set -e
cd "$(dirname "$0")/.."
: "${NOTARY_PROFILE:?set NOTARY_PROFILE to the notarytool keychain profile}"
OUT=build/release-mac
DD=build/xcode-release
rm -rf "$OUT"; mkdir -p "$OUT"
(cd ios && xcodegen generate -q)
xcodebuild -project ios/ClassicVoices.xcodeproj -scheme ClassicVoicesMacPublic -configuration Release \
    -destination 'generic/platform=macOS' -derivedDataPath "$DD" ARCHS="arm64 x86_64" ONLY_ACTIVE_ARCH=NO \
    build > "$OUT/build.log" 2>&1 || { grep -E ': error: ' "$OUT/build.log" | sort -u; exit 1; }
APP="$DD/Build/Products/Release/Classic Voices.app"

# refuse to package anything of Apple's
if find "$APP" \( -name voiced -o -name '*.dat' -o -name 'broker.hdr*' -o -name TTSResources -o -name MacinTalk \
        -o -name '*.SpeechVoice' -o -name 'cvnative_*' -o -name 'cvmtnative_*' -o -name '*.ipsw' \) | grep -q .; then
    echo "refusing to package Apple-derived files:" >&2
    find "$APP" \( -name voiced -o -name '*.dat' -o -name 'broker.hdr*' -o -name TTSResources -o -name MacinTalk \
        -o -name '*.SpeechVoice' -o -name 'cvnative_*' -o -name 'cvmtnative_*' \) >&2
    exit 1
fi
codesign --verify --deep --strict "$APP"
codesign -dvv "$APP" 2>&1 | grep -q "Authority=Developer ID Application" || { echo "not signed with Developer ID" >&2; exit 1; }

echo "notarizing the app..."
ditto -c -k --keepParent "$APP" "$OUT/notarize.zip"
xcrun notarytool submit "$OUT/notarize.zip" --keychain-profile "$NOTARY_PROFILE" --wait | tee "$OUT/notary-app.log"
grep -q "status: Accepted" "$OUT/notary-app.log" || { echo "notarization failed (see $OUT/notary-app.log)" >&2; exit 1; }
xcrun stapler staple "$APP"
rm "$OUT/notarize.zip"
ditto -c -k --keepParent "$APP" "$OUT/ClassicVoices-mac.zip"

echo "making the disk image..."
STAGE="$OUT/dmg"
mkdir -p "$STAGE"
ditto "$APP" "$STAGE/Classic Voices.app"
ln -s /Applications "$STAGE/Applications"
hdiutil create -volname "Classic Voices" -srcfolder "$STAGE" -fs HFS+ -format UDZO -ov "$OUT/ClassicVoices-mac.dmg" >/dev/null
rm -rf "$STAGE"
codesign -s "Developer ID Application" --timestamp "$OUT/ClassicVoices-mac.dmg"
xcrun notarytool submit "$OUT/ClassicVoices-mac.dmg" --keychain-profile "$NOTARY_PROFILE" --wait | tee "$OUT/notary-dmg.log"
grep -q "status: Accepted" "$OUT/notary-dmg.log" || { echo "notarization of the disk image failed" >&2; exit 1; }
xcrun stapler staple "$OUT/ClassicVoices-mac.dmg"
echo "built $OUT/ClassicVoices-mac.dmg and $OUT/ClassicVoices-mac.zip"
