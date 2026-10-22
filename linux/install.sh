#!/bin/sh
# Install Classic Voices for the current user (no root needed):
#   ~/.local/bin/classicvoices-data, classicvoices-say
#   ~/.local/lib/classicvoices/            importer and recompiler
#   ~/.local/libexec/speech-dispatcher-modules/sd_classicvoices
# Speech Dispatcher (0.11 and later) finds the module there by itself.
set -e
cd "$(dirname "$0")"
P=${PREFIX:-$HOME/.local}
mkdir -p "$P/bin" "$P/lib" "$P/libexec/speech-dispatcher-modules"
rm -rf "$P/lib/classicvoices"
cp -R lib/classicvoices "$P/lib/"
cp bin/classicvoices-data bin/classicvoices-say "$P/bin/"
cp libexec/sd_classicvoices "$P/libexec/speech-dispatcher-modules/"
chmod 755 "$P/bin/classicvoices-data" "$P/bin/classicvoices-say" "$P/libexec/speech-dispatcher-modules/sd_classicvoices"
pkill -u "$(id -u)" -x speech-dispatcher 2>/dev/null || true
echo "Installed into $P."
case ":$PATH:" in *":$P/bin:"*) ;; *) echo "Add $P/bin to your PATH to run classicvoices-data." ;; esac
echo
echo "Next, get the voices (nothing of Apple's comes with Classic Voices):"
echo "  classicvoices-data import ios6        # streams iOS 6's speech files from Apple"
echo "  classicvoices-data import all        # every firmware (about 5.5 GB to stream)"
echo "  classicvoices-data native            # optional: fast engine code, needs clang"
echo "  spd-say -o classicvoices 'Hello from Classic Voices'"
