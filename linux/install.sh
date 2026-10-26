#!/bin/sh
# Install Classic Voices for the current user (no root needed):
#   ~/.local/bin/classicvoices                the app (and a menu entry for it)
#   ~/.local/bin/classicvoices-data, classicvoices-say
#   ~/.local/lib/classicvoices/            the app, importer and recompiler
#   ~/.local/libexec/speech-dispatcher-modules/sd_classicvoices
# Speech Dispatcher (0.11 and later) finds the module there by itself.
set -e
cd "$(dirname "$0")"
P=${PREFIX:-$HOME/.local}
mkdir -p "$P/bin" "$P/lib" "$P/libexec/speech-dispatcher-modules"
rm -rf "$P/lib/classicvoices"
cp -R lib/classicvoices "$P/lib/"
# copy, then rename into place: a running module (Orca speaking) can't be overwritten
put() { cp "$1" "$2.new" && chmod 755 "$2.new" && mv -f "$2.new" "$2"; }
for f in classicvoices classicvoices-data classicvoices-say; do put "bin/$f" "$P/bin/$f"; done
put libexec/sd_classicvoices "$P/libexec/speech-dispatcher-modules/sd_classicvoices"
# the app's menu entry
A=${XDG_DATA_HOME:-$HOME/.local/share}/applications
mkdir -p "$A"
cat > "$A/classicvoices.desktop" <<DESKTOP
[Desktop Entry]
Type=Application
Name=Classic Voices
Comment=Get and manage the classic iOS and Mac OS X voices
Exec=$P/bin/classicvoices
Terminal=false
Categories=Utility;Accessibility;
Keywords=speech;voices;tts;Speech Dispatcher;
DESKTOP
command -v update-desktop-database >/dev/null && update-desktop-database "$A" 2>/dev/null || true
# restart Speech Dispatcher so it runs the new module (the kernel keeps 15
# characters of a process name: "speech-dispatch")
pkill -u "$(id -u)" -x speech-dispatch 2>/dev/null || true
echo "Installed into $P."
if ! python3 -c "import wx" 2>/dev/null; then
    echo
    echo "The Classic Voices app needs wxPython. Install it with one of:"
    echo "  sudo apt install python3-wxgtk4.0      (Debian, Ubuntu)"
    echo "  sudo dnf install python3-wxpython4     (Fedora)"
    echo "  sudo pacman -S python-wxpython         (Arch)"
fi
case ":$PATH:" in *":$P/bin:"*) ;; *) echo "Add $P/bin to your PATH to run classicvoices-data." ;; esac
echo
echo "Next, get the voices (nothing of Apple's comes with Classic Voices): open Classic Voices from"
echo "your applications menu, or run classicvoices; or from a terminal:"
echo "  classicvoices-data import ios6        # streams iOS 6's speech files from Apple"
echo "  classicvoices-data import all        # every firmware (about 5.5 GB to stream)"
echo "  classicvoices-data native            # optional: fast engine code, needs clang"
echo "  spd-say -o classicvoices 'Hello from Classic Voices'"
