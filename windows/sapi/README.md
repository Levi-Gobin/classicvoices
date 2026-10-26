# Classic Voices for SAPI 5 (Windows)

The same voices as the NVDA add-on (iOS 3–6 Samantha, Daniel, Karen …, the Mac
OS X Lion–Mavericks voices) as ordinary Windows SAPI 5 voices, for every
program that speaks through SAPI: JAWS, NVDA's "Microsoft Speech API version
5" driver, Balabolka, System.Speech/.NET programs, and the rest. There are
two DLLs, `cvsapi64.dll` for 64-bit programs and `cvsapi32.dll` for 32-bit
ones, and both are installed.

Narrator and the modern Windows apps (Microsoft's OneCore speech) can't use
them: OneCore ignored a Classic Voices entry in its own voice list, even
with Microsoft's attributes and engine CLSID copied in (tested on Windows 11).

On Windows on Arm, the x64 DLL serves x64 programs and ARM64EC ones such as
NVDA. Native Arm programs would need an ARM64X forwarder, since x64 and Arm
programs share one COM registration; `--cc-arm64` builds the Arm engine, but
a working forwarder isn't done yet (see NOTES.md).

## Installing

The Classic Voices app installs all this for you
([desktop/README.md](../../desktop/README.md)). By hand:

1. Get voice data: with the app, or with the NVDA add-on (NVDA menu → Tools
   → Classic Voices data…), whose folder is `%APPDATA%\nvda\classicvoices`.
   Or put a folder with the same layout anywhere and pass it with
   `-DataDir`.
2. In an administrator PowerShell, in the folder with the DLLs:

       powershell -ExecutionPolicy Bypass -File install.ps1 [-DataDir <folder>]

   This copies the DLLs to `Program Files\Classic Voices` and registers them.
   The voice list is read at this point, so run it again after importing or
   downloading more voices.
3. `uninstall.ps1` removes the voices from every program and deletes the
   DLLs; your data folder stays.

The data folder is saved in `HKLM\SOFTWARE\Classic Voices\DataDir`. If it's
inside one user's profile, other Windows accounts can list the voices but
can't read them.

## Fast code

Programs get the fast engine code from `<data folder>\Native`, the same
`cvnative_<build>_64.dll` / `_32.dll` files the NVDA add-on uses (built from
your own firmware with `nvda/build_native.py`). Without them the engine
interprets, which sounds identical but is slower. Measured through SAPI on a
Windows 11 on Arm VM, where the x64 and x86 code runs emulated: first audio for
short phrases 14–27 ms, and 21–34 ms from an interruption to the next phrase's
first audio. The first utterance after a program picks a voice also loads it
(about 100–330 ms).

## Building

    python windows/sapi/build.py [--cc-x64 NAME] [--cc-x86 NAME]

MinGW-w64 gcc or clang, as for the NVDA add-on; output in `build/sapi/`.
MinGW has no `sapiddk.h`, so the engine side of SAPI is declared in
`sapi_engine.h`. `cvsapitest64/32.exe` drives the voices through SAPI itself
(`list`, `speak <voice> <out.wav> <text>`, `bench <voice> <dir>`, `register`)
and writes WAV files, so it works without a sound card. Set
`CLASSICVOICES_SAPI_LOG=<file>` to have the engine log first-audio and stop
times.

## How it maps SAPI onto the engine

- Each voice is a token `ClassicVoices_<key>` under `Speech\Voices\Tokens`,
  holding `CVKey` (the stream key, e.g. `ios6/en-US`) and `CVFallback` (the
  compact voice that speaks pitch-changed text, as on iOS 5 and 6).
- Consecutive text fragments with the same prosody become one engine
  utterance. Rate −10…+10 maps to 50–400 % (+10 is the engine's fastest),
  pitch ±10 to an octave either way, and volume 100 to the voice's own level.
- Bookmarks become `SPEI_TTS_BOOKMARK` events at the exact audio offset,
  and each word a `SPEI_WORD_BOUNDARY` event where its audio begins (the
  engine's own word and sentence markers, carried through the stream; iOS 3
  and 4 mark sentences, not their first words, so those are added).
- Apple's pronunciation rules apply unless `PronunciationRules` (DWORD) is 0
  in `HKCU\Software\Classic Voices`; word positions still point into the
  original text. `PitchFallback` 0 there lets enhanced and premium voices
  ignore pitch changes instead of handing those words to the compact voice.
  `<silence>` writes silence, and spelled-out text is read letter by letter.
- Stops are checked after every chunk of audio: the engine stops within a few
  milliseconds of SAPI asking it to.
