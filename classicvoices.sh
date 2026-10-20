#!/bin/bash
# Build Classic Voices in your own GitHub account and download the app, from
# the Mac Terminal. Paste this into Terminal:
#
#   bash <(curl -fsSL https://raw.githubusercontent.com/Levi-Gobin/classicvoices/main/classicvoices.sh)
#
# It signs you in to GitHub (in your browser), makes or updates your copy
# (fork) of Classic Voices, runs the build there and waits for it. Then it
# installs the Mac app, and saves ClassicVoices.ipa in your Downloads folder
# and opens Sideloadly if you have it, so you can install the iPhone app. It
# can also build the Apple Vision Pro app.
# Running it again later updates your copy and builds the newest version.
#
# Everything it prints is plain lines, so it reads well with VoiceOver.
set -euo pipefail

UPSTREAM=Levi-Gobin/classicvoices
WORKFLOW=build.yml
HOME_DIR="$HOME/.classicvoices"
OUT="$HOME/Downloads"

say() { printf '%s\n' "$*"; }
fail() { say "Stopped: $*" >&2; exit 1; }

# --- GitHub's command line tool, kept in ~/.classicvoices ------------------
gh_tool() {
    if command -v gh >/dev/null 2>&1; then GH=$(command -v gh); return; fi
    GH="$HOME_DIR/gh/bin/gh"
    [ -x "$GH" ] && return
    say "Downloading GitHub's command line tool (one time)."
    local arch tag tmp
    arch=$(uname -m); [ "$arch" = x86_64 ] && arch=amd64
    tag=$(curl -fsSL https://api.github.com/repos/cli/cli/releases/latest | sed -n 's/.*"tag_name": *"v\([^"]*\)".*/\1/p')
    [ -n "$tag" ] || fail "couldn't reach GitHub. Check your internet connection."
    tmp=$(mktemp -d)
    curl -fsSL -o "$tmp/gh.zip" "https://github.com/cli/cli/releases/download/v$tag/gh_${tag}_macOS_${arch}.zip"
    unzip -q "$tmp/gh.zip" -d "$tmp"
    mkdir -p "$HOME_DIR"
    rm -rf "$HOME_DIR/gh"
    mv "$tmp/gh_${tag}_macOS_${arch}" "$HOME_DIR/gh"
    rm -rf "$tmp"
}

# CLASSICVOICES_TARGETS="iphone mac" skips the question.
TARGETS=${CLASSICVOICES_TARGETS:-}
if [ -z "$TARGETS" ]; then
    say "Which apps should be built?"
    say "  1. iPhone and Mac"
    say "  2. iPhone only"
    say "  3. Mac only"
    say "  4. Apple Vision Pro"
    read -r -p "Type the numbers you want, like 1 or 2 4, and press Return (Return alone means 1): " choice </dev/tty || choice=1
    [ -n "$choice" ] || choice=1
    TARGETS=""
    case "$choice" in *1*) TARGETS="iphone mac" ;; esac
    case "$choice" in *2*) TARGETS="$TARGETS iphone" ;; esac
    case "$choice" in *3*) TARGETS="$TARGETS mac" ;; esac
    case "$choice" in *4*) TARGETS="$TARGETS visionpro" ;; esac
    TARGETS=${TARGETS# }
    [ -n "$TARGETS" ] || TARGETS="iphone mac"
fi
has() { case " $TARGETS " in *" $1 "*) return 0 ;; esac; return 1; }

gh_tool
gh() { "$GH" "$@"; }

# --- Sign in -----------------------------------------------------------------
if ! gh auth status >/dev/null 2>&1; then
    say "Signing in to GitHub. Your browser will open, and a one-time code is copied"
    say "to the clipboard: paste it on the page, then approve."
    gh auth login --hostname github.com --git-protocol https --web --clipboard --scopes workflow </dev/tty
fi
ME=$(gh api user --jq .login) || fail "couldn't sign in to GitHub."
say "Signed in as $ME."

# --- Your copy of Classic Voices --------------------------------------------
if [ "$(echo "$ME" | tr A-Z a-z)" = "$(echo "${UPSTREAM%%/*}" | tr A-Z a-z)" ]; then
    REPO=$UPSTREAM
else
    REPO=$ME/classicvoices
    make_copy() {
        say "Making your copy of Classic Voices."
        gh repo fork "$UPSTREAM" --clone=false --default-branch-only >/dev/null 2>&1 || fail "couldn't make your copy."
        for _ in $(seq 30); do gh repo view "$REPO" >/dev/null 2>&1 && break; sleep 2; done
    }
    if gh repo view "$REPO" >/dev/null 2>&1; then
        # A copy whose history shares nothing with Classic Voices itself is left
        # over from an older history and a normal update is refused; it only
        # ever held build files, so reset it to match. (A plain commit lookup
        # won't do: GitHub finds a fork's commits through the shared network.)
        HEAD=$(gh api "repos/$REPO/commits/HEAD" --jq .sha 2>/dev/null)
        RELATION=$(gh api "repos/$UPSTREAM/compare/$HEAD...HEAD" --jq .status 2>/dev/null) || RELATION=""
        say "Updating your copy of Classic Voices."
        if [ -n "$HEAD" ] && [ -z "$RELATION" ]; then
            gh repo sync "$REPO" --source "$UPSTREAM" --force >/dev/null || fail "couldn't replace the contents of $REPO."
        else
            gh repo sync "$REPO" --source "$UPSTREAM" >/dev/null || fail "couldn't update $REPO from $UPSTREAM."
        fi
    else
        make_copy
    fi
fi

# GitHub turns workflows off in new copies; turn this one on.
gh api -X PUT "repos/$REPO/actions/permissions" -F enabled=true -f allowed_actions=all >/dev/null 2>&1 || true
gh workflow enable "$WORKFLOW" -R "$REPO" >/dev/null 2>&1 || true

# The build is zipped with a password only this script knows, because anyone
# signed in to GitHub can download a public repository's build files.
PASSWORD=$(openssl rand -hex 16)
printf '%s' "$PASSWORD" | gh secret set IPA_PASSWORD -R "$REPO" >/dev/null || fail "couldn't set the build password on $REPO."

# --- Build -------------------------------------------------------------------
STARTED=$(date -u +%Y-%m-%dT%H:%M:%SZ)
gh workflow run "$WORKFLOW" -R "$REPO" -f targets="$TARGETS" >/dev/null 2>&1 \
    || fail "couldn't start the build. Open https://github.com/$REPO/actions, turn workflows on if asked, and run this again."
say "Build started. It takes about half an hour; you'll hear each step as it finishes."
RUN=""
for _ in $(seq 60); do
    RUN=$(gh run list -R "$REPO" -w "$WORKFLOW" -e workflow_dispatch -L 5 --json databaseId,createdAt \
          --jq "[.[] | select(.createdAt >= \"$STARTED\")] | last | .databaseId // empty")
    [ -n "$RUN" ] && break
    sleep 5
done
[ -n "$RUN" ] || fail "the build didn't appear. See https://github.com/$REPO/actions."

# One line per finished step, read from GitHub every 30 seconds.
STEPS='.jobs[].steps[] | select(.status == "completed") | {"Fetch firmware from Apple": "Firmware downloaded from Apple", "Generate the fast engines": "Engines generated", "Build the app": "iPhone app built", "Build the Vision Pro app": "Vision Pro app built", "Build the Mac app": "Mac app built", "Run actions/upload-artifact@v4": "Upload finished"}[.name] as $n | select($n) | $n + (if .conclusion == "success" then "" else " (" + .conclusion + ")" end)'
seen=""
while true; do
    steps=$(gh run view "$RUN" -R "$REPO" --json jobs --jq "$STEPS" 2>/dev/null) || { sleep 20; continue; }
    while IFS= read -r line; do
        [ -z "$line" ] && continue
        case "$seen" in *"|$line|"*) ;; *) say "  $line"; seen="$seen|$line|" ;; esac
    done <<< "$steps"
    [ "$(gh run view "$RUN" -R "$REPO" --json status --jq .status 2>/dev/null)" = completed ] && break
    sleep 30
done
CONCLUSION=$(gh run view "$RUN" -R "$REPO" --json conclusion --jq .conclusion)
[ "$CONCLUSION" = success ] || fail "the build didn't finish ($CONCLUSION). Details: https://github.com/$REPO/actions/runs/$RUN"

# --- Download ----------------------------------------------------------------
say "Downloading."
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
if has mac; then
    gh run download "$RUN" -R "$REPO" -n ClassicVoicesMac -D "$TMP" || fail "couldn't download the Mac app."
    unzip -q -o -P "$PASSWORD" "$TMP/ClassicVoices-mac-locked.zip" -d "$TMP" || fail "couldn't open the Mac app download."
    APPS=/Applications
    [ -w "$APPS" ] || { APPS="$HOME/Applications"; mkdir -p "$APPS"; }
    APP="$APPS/Classic Voices.app"
    # Replacing a copy that is open stops its voices until the new one opens.
    # If Classic Voices is already installed, ask instead of just doing it.
    choice=1
    if [ -d "$APP" ]; then
        if pgrep -f "Classic Voices.app/" >/dev/null 2>&1; then
            say "Classic Voices is already installed and open."
            say "Replacing it now closes it and its voices for a moment: if VoiceOver is speaking"
            say "with one of them, speech stops until the new copy opens. macOS may then ask to"
            say "reopen Classic Voices and to open Classic Voices Synthesizer: choose Reopen and Open Anyway."
        else
            say "Classic Voices is already installed."
        fi
        say "  1. Replace it with the new build now"
        say "  2. Save the new build in Downloads as Classic Voices.app, to install later"
        say "  3. Leave the installed copy alone"
        read -r -p "Type 1, 2 or 3 and press Return (Return alone means 1): " choice </dev/tty || choice=1
    fi
    case "$choice" in
    2)
        rm -rf "$OUT/Classic Voices.app"
        ditto -x -k "$TMP/ClassicVoices-mac.zip" "$OUT" || fail "couldn't save the Mac app."
        say "Saved the new build as Classic Voices.app in your Downloads folder."
        say "To install it later: quit Classic Voices, drag the new one into $APPS replacing the old, and open it."
        ;;
    3)
        say "Left the installed Mac app as it is."
        ;;
    *)
        LS=/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister
        osascript -e 'quit app "Classic Voices"' >/dev/null 2>&1 || true
        # The speech part keeps running after the app quits; stop it before its files go.
        pkill -f "Classic Voices.app/Contents/PlugIns/Synth.appex" >/dev/null 2>&1 || true
        rm -rf "$APP"
        ditto -x -k "$TMP/ClassicVoices-mac.zip" "$APPS" || fail "couldn't install the Mac app."
        "$LS" -f "$APP" >/dev/null 2>&1 || true
        # macOS keeps a list of speech voices; restart it so the new ones appear.
        killall axassetsd >/dev/null 2>&1 || true
        sleep 2
        open "$APP"
        say "Installed the Mac app in $APPS and opened it."
        say "If macOS asks whether to open Classic Voices Synthesizer, choose Open Anyway:"
        say "that is its speech part, which the build signed without an Apple developer account."
        say "The voices then appear in System Settings, Accessibility, Spoken Content and VoiceOver."
        ;;
    esac
fi

if has iphone; then
    gh run download "$RUN" -R "$REPO" -n ClassicVoices -D "$TMP" || fail "couldn't download the iPhone app."
    unzip -q -o -P "$PASSWORD" "$TMP/ClassicVoices-ipa.zip" -d "$TMP" || fail "couldn't open the iPhone app download."
    mv -f "$TMP/ClassicVoices.ipa" "$OUT/ClassicVoices.ipa"
    say "Saved $OUT/ClassicVoices.ipa"
    printf '%s' "$OUT/ClassicVoices.ipa" | pbcopy
    if open -a Sideloadly 2>/dev/null; then
        say "Sideloadly is open. Connect your iPhone, choose ClassicVoices.ipa from your"
        say "Downloads folder (its full path is on the clipboard), and press Start."
    else
        say "Install it on your iPhone with Sideloadly (sideloadly.io), AltStore or SideStore:"
        say "choose ClassicVoices.ipa from your Downloads folder."
    fi
    say "Then open Classic Voices once, and its voices appear in VoiceOver and Spoken Content."
fi

if has visionpro; then
    gh run download "$RUN" -R "$REPO" -n ClassicVoicesVision -D "$TMP" || fail "couldn't download the Vision Pro app."
    unzip -q -o -P "$PASSWORD" "$TMP/ClassicVoicesVision-ipa.zip" -d "$TMP" || fail "couldn't open the Vision Pro app download."
    mv -f "$TMP/ClassicVoicesVision.ipa" "$OUT/ClassicVoicesVision.ipa"
    say "Saved $OUT/ClassicVoicesVision.ipa"
    say "Install it on your Apple Vision Pro with Xcode (turn on Developer Mode on the headset, then"
    say "Window, Devices and Simulators, and drag the file onto it), or with a sideloading tool that"
    say "supports Vision Pro, such as SideStore."
    say "Then open Classic Voices once on the headset, and its voices appear in VoiceOver."
fi
