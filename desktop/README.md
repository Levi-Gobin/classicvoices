# Classic Voices for Windows and Linux (the app)

The same voice browser as the iPhone, Mac and Android apps: an **iOS** tab, a
**macOS** tab and **Settings**. You go from a version and a language down to
each voice. A voice you have opens: try it with your own text and speed,
choose whether it's a system voice, or remove it. A voice you don't have
downloads when you press it, and pressing it again stops the download.
Every row says what pressing it does before you press it. Only native
controls are used, so screen readers read everything: NVDA and JAWS on
Windows, Orca on Linux. A download or import shows a progress bar under its
Stop button, moving by 1%; the screen reader reports it its own way (NVDA's
progress bar output, Orca's progress bar updates).

What the phone and Mac apps get built in, this app gets for you:

- **Get the firmware**: streams the speech files out of Apple's iPhone
  firmware (only the root filesystem, copying out just the engines and
  voices), or reads a firmware file or extracted folder you have. Each
  version says which firmware it needs: Lion's voices need iOS 5 and 6, for
  example.
- **Speed up the voices**: the fast engine code, built from your firmware on
  this computer, by itself after each firmware import (and when the app
  opens, for firmware imported before). It's bit-identical to the slow
  interpreter, and starts speaking several times sooner. Windows downloads
  the Zig C compiler for it (checked against ziglang.org's checksum); Linux
  uses clang if you have it, else Zig too. A firmware takes one to three
  minutes. If a build fails or you stop it, it starts again only from this
  page.
- **Settings**: Apple's pronunciation rules and the compact voice for pitch
  changes, as on iOS. On Windows, re-registering the Windows voices; on
  Linux, the Speech Dispatcher module's status.

## Windows

`ClassicVoicesSetup.exe` installs:

- the app, in the Start menu;
- the SAPI 5 engine for 64- and 32-bit programs
  ([windows/sapi](../windows/sapi/README.md));
- the data folder `%ProgramData%\Classic Voices`, which every user of the PC
  can add voices to.

It's built by the `windows.yml` workflow, or by `desktop/build_windows.py`
(PyInstaller and Inno Setup). Without NVDA you get the voices from the app.
With NVDA, the add-on uses the app's voices when the app has some, and
Settings offers to copy over voices the add-on imported before.

Changing which voices are system voices updates the Windows registration,
which asks for administrator permission once each time.

## Linux

`linux/install.sh` installs the app as `classicvoices`, with a menu entry. It
needs wxPython from your distribution (`sudo apt install python3-wxgtk4.0`).
Voice samples play through `classicvoices-say` and PipeWire or PulseAudio.
Speech Dispatcher sees new or hidden voices without a restart; the settings
go to `<data>/settings.conf`, which the module reads at every message.

## Running from the repository

    python3 -m classicvoices        (from desktop/, with wxPython installed)

The app finds the shared library (`nvda/addon/synthDrivers/_classicvoices`)
and the recompiler (`tools/recomp.py`) in the repository.
