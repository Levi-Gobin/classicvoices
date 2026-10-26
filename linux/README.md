# Classic Voices for Linux

The iOS 3–6 VoiceOver voices, their enhanced versions, and the Mac OS X Lion,
Mountain Lion and Mavericks voices, as a Speech Dispatcher output module:
Orca, and anything else that speaks through Speech Dispatcher, can use them.
The voices run on Apple's original ARM engines through the same bit-exact
emulator as the iPhone app; the audio is byte-for-byte what the Mac and
iPhone versions produce.

MacinTalk (Fred, Victoria, Alex …) is not part of this: on Linux it is
[Panthera Speech](https://github.com/tgeczy/panthera-speech)'s job.

Nothing of Apple's comes with Classic Voices. `classicvoices-data` takes the
engines and voices out of iPhone firmware, streamed from Apple's own servers
or read from an IPSW you have, and downloads the other voices from Apple.

## Install

Needs Speech Dispatcher 0.11 or later (Debian 12+, Ubuntu 22.04+, Fedora 36+)
and Python 3. Everything installs into your home folder; no root needed.

```
tar -xzf classicvoices-linux-x86_64.tar.gz
classicvoices/install.sh
```

That puts `classicvoices-data` and `classicvoices-say` in `~/.local/bin` and
the module in `~/.local/libexec/speech-dispatcher-modules/sd_classicvoices`,
where Speech Dispatcher finds it by itself. (If your
`~/.config/speech-dispatcher/speechd.conf` lists modules with `AddModule`,
add `AddModule "classicvoices" "sd_classicvoices" "classicvoices.conf"` to it.)

## Get the voices

The easy way is the Classic Voices app (in your applications menu, or
`classicvoices`): the same voice browser as the iPhone and Mac apps, which
streams the firmware, downloads voices, builds the fast code and switches
voices in and out of Speech Dispatcher. It needs wxPython
(`sudo apt install python3-wxgtk4.0`); see
[desktop/README.md](../desktop/README.md). From a terminal:

```
classicvoices-data firmware        # what each firmware is for, and Apple's links
classicvoices-data import ios6     # stream iOS 6's speech files from Apple (~750 MB)
classicvoices-data import all      # every firmware, about 5.5 GB to stream
classicvoices-data import ~/iPhone2,1_6.1.6_10B500_Restore.ipsw   # or a file you have
classicvoices-data voices          # enhanced iOS and Mac OS X voices you can download
classicvoices-data download "Daniel" "enhanced en-US" all-enhanced
classicvoices-data status
```

Streaming downloads only the firmware's root filesystem, inflates it into a
temporary file, copies the engine and voices out (a few dozen MB per
firmware), and deletes the rest. Encrypted firmware (iOS 3–7) is decrypted
with the key from The Apple Wiki, using OpenSSL's libcrypto.

| Firmware | Gives you |
|---|---|
| ios3, ios4 | the iOS 3 and 4 voices |
| ios5, ios6 | the iOS 5 and 6 voices, and the engines for their enhanced voices |
| ios6 (with ios5) | the engine and common files for the Lion and Mountain Lion voices |
| ios7 | engine only: Mavericks compact voices |
| ios10 | engine only: Mavericks premium voices |

## Use them

```
spd-say -o classicvoices -L                       # list the voices
spd-say -o classicvoices -y "Samantha (iOS 6)" "Hello"
classicvoices-say -v Samantha -o hello.wav "Hello"   # straight to a WAV file
```

In Orca: Preferences, Voice, Speech synthesizer: Classic Voices, then pick a
voice by name.

## Fast engine code (recommended)

On its own the engine emulates the iPhone's processor; on a slow machine
that can be slower than real time. `classicvoices-data native` recompiles each
imported engine's hot code into native code with your compiler (clang
recommended: `sudo apt install clang`). The result is bit-identical and
typically six to ten times real time; it goes to
`~/.local/share/classicvoices/Native/<arch>/` and is picked up on the next
message. Each build takes a few minutes. It is derived from Apple's code:
keep it on your machine.

The same for an Android phone, from the firmware imported here (needs the
[Android NDK](https://developer.android.com/ndk/downloads); also works on a Mac):

```
classicvoices-data android-pack --ndk ~/android-ndk-r30 -o ClassicVoices-android-fast-code.zip
```

builds 64- and 32-bit ARM libraries for every imported engine (`--abi` picks
others) into a data pack: copy it to the phone and use **Import a data pack**
in the app.

## Settings

`~/.config/speech-dispatcher/modules/classicvoices.conf` (optional):

```
PronunciationRules 0   # don't apply Apple's text rules ("iOS" -> "I O S", "OS X" -> "OS ten")
PitchFallback 0        # let enhanced/premium voices ignore pitch changes
DataPath "/path"       # instead of ~/.local/share/classicvoices
Debug 1                # log to Speech Dispatcher's module log
```

Apple's pronunciation rules use PCRE2 (`libpcre2-8.so.0`, on practically
every system); without it they are skipped.

## How it works

- `sd_classicvoices` speaks Speech Dispatcher's module protocol through
  Speech Dispatcher's own helpers (vendored in `speechd/`, BSD licence) and
  sends audio to the server (server-side audio). One worker thread runs each
  message through the engine stream; every SSML `<mark>` ends a segment and
  is reported once the audio before it has gone out, so Orca's reading
  position follows the speech.
- Volume 100 (Speech Dispatcher's default) is the voice's own level; rate and
  pitch map onto the engine's 50–400 % and 50–200 % exponentially.
- The engine, the voice catalogue and the SSML parser are the shared C code in
  `core/` and `ports/` (the same as the Android app's).

## Building

```
linux/build.sh       # CC=clang by default; -> build/linux/classicvoices-linux-<arch>.tar.gz
```

No dependencies beyond a C compiler. `tests/speechd_module_test.py
build/linux/stage/libexec/sd_classicvoices` drives the module the way the
server does (voices, settings, marks, characters, keys, stopping) without a
server.
