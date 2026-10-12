#!/bin/bash
# Build Classic Voices in your own GitHub account and download the app, from
# the Mac Terminal. Paste this into Terminal:
#
#   bash <(curl -fsSL https://raw.githubusercontent.com/Levi-Gobin/classicvoices/main/classicvoices.sh)
#
# It signs you in to GitHub (in your browser), makes or updates your copy
# (fork) of Classic Voices, runs the build there, waits for it, and saves
# ClassicVoices.ipa in your Downloads folder. Then it opens Sideloadly if you
# have it, so you can install the app on your iPhone. Running it again later
# updates your copy and builds the newest version.
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
elif gh repo view "$ME/classicvoices" >/dev/null 2>&1; then
    REPO=$ME/classicvoices
    say "Updating your copy of Classic Voices."
    gh repo sync "$REPO" --source "$UPSTREAM" >/dev/null || fail "couldn't update $REPO from $UPSTREAM."
else
    say "Making your copy of Classic Voices."
    gh repo fork "$UPSTREAM" --clone=false --default-branch-only >/dev/null 2>&1 || fail "couldn't make your copy."
    REPO=$ME/classicvoices
    for _ in $(seq 30); do gh repo view "$REPO" >/dev/null 2>&1 && break; sleep 2; done
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
gh workflow run "$WORKFLOW" -R "$REPO" >/dev/null 2>&1 \
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
STEPS='.jobs[].steps[] | select(.status == "completed") | {"Fetch firmware from Apple": "Firmware downloaded from Apple", "Generate the fast engines": "Engines generated", "Build the app": "App built", "Run actions/upload-artifact@v4": "Upload finished"}[.name] as $n | select($n) | $n + (if .conclusion == "success" then "" else " (" + .conclusion + ")" end)'
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
say "Downloading the app."
TMP=$(mktemp -d)
gh run download "$RUN" -R "$REPO" -n ClassicVoices -D "$TMP" || fail "couldn't download the build."
unzip -q -o -P "$PASSWORD" "$TMP/ClassicVoices-ipa.zip" -d "$TMP" || fail "couldn't open the download."
mv -f "$TMP/ClassicVoices.ipa" "$OUT/ClassicVoices.ipa"
rm -rf "$TMP"
say "Saved $OUT/ClassicVoices.ipa"

# --- Install -----------------------------------------------------------------
printf '%s' "$OUT/ClassicVoices.ipa" | pbcopy
if open -a Sideloadly 2>/dev/null; then
    say "Sideloadly is open. Connect your iPhone, choose ClassicVoices.ipa from your"
    say "Downloads folder (its full path is on the clipboard), and press Start."
else
    say "Install it on your iPhone with Sideloadly (sideloadly.io), AltStore or SideStore:"
    say "choose ClassicVoices.ipa from your Downloads folder."
fi
say "Then open Classic Voices once, and its voices appear in VoiceOver and Spoken Content."
