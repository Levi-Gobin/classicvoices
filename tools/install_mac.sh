#!/bin/sh
# Build the Mac app (Release), install it in /Applications and make macOS pick
# up its voices. axassetsd caches the audio-component list; after a reinstall it
# can keep a stale entry and fail every voice load (TextToSpeech.MultiError), so
# it is restarted (launchd relaunches it immediately).
set -e
cd "$(dirname "$0")/../ios"
xcodegen generate -q
xcodebuild -project ClassicVoices.xcodeproj -scheme ClassicVoicesMac -configuration Release \
  -destination 'platform=macOS' -derivedDataPath ../build/xcode -allowProvisioningUpdates build > ../build/xcbmac.log 2>&1 \
  || { grep -E ': error: ' ../build/xcbmac.log | sort -u; exit 1; }
APP="../build/xcode/Build/Products/Release/Classic Voices.app"
LS=/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister
osascript -e 'quit app "Classic Voices"' 2>/dev/null || true
$LS -u "$APP" 2>/dev/null || true
rm -rf "/Applications/Classic Voices.app"
ditto "$APP" "/Applications/Classic Voices.app"
$LS -f "/Applications/Classic Voices.app"
killall axassetsd 2>/dev/null || true
sleep 2
open -g "/Applications/Classic Voices.app"
echo "installed; voices appear in System Settings › Accessibility › Spoken Content within ~30 s"
