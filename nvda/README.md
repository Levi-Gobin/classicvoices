# Classic Voices for NVDA

The iOS 3–6 Vocalizer voices (Samantha, Daniel, Karen …) and the Mac OS X
Lion / Mountain Lion / Mavericks voices, as an NVDA add-on. The voices run on
Apple's original ARM engines, executed by the same bit-exact emulator the iOS
app uses — interpreter-only on Windows, since the recompiled fast paths are
generated from firmware and never distributed. The CPU hooks (`vx_set_native`)
remain, so a JIT backend can be added later for speed; today the interpreter
runs ~2× real time on a desktop CPU with ~80 ms to first audio, and premium
(concatenative) voices run far faster.

MacinTalk is deliberately out of scope: [Panthera Speech]
(https://github.com/tgeczy/panthera-speech) by Tamas Geczy already runs
MacinTalk natively on Windows, and this add-on follows its whole approach —
including "supply your own install media" and the refusal to bundle anything
of Apple's. Its MIT-licensed HFS+/UDIF/xar/cpio reader is vendored here as
`applereaders.py` (with one local change for iOS's HFSX root filesystems).

## How users get voices

1. Install the add-on. It contains no voices, only the emulator and tools.
2. NVDA menu → Tools → **Classic Voices data…** → *Import firmware (IPSW)*.
   Supply your own firmware for the builds you want (downloadable from Apple;
   the add-on never fetches firmware itself):
   - iPhone OS 3.1.3 (7E18, iPhone2,1) — the iOS 3 voices
   - iOS 4.3.5 (8L1, iPhone2,1) — the iOS 4 voices
   - iOS 5.1.1 (9B206, iPhone2,1) — the iOS 5 voices
   - iOS 6.1.6 (10B500, iPhone2,1) — the iOS 6 voices
   - iOS 7.1.2 (11D257, iPhone3,1) — engine only, for Mavericks compacts
   - iOS 10.3.4 (14G61, iPhone5,1) — engine only, for Mavericks premium

   The importer unzips the IPSW, fetches the root-filesystem key from The
   Apple Wiki, decrypts on the fly (Windows CNG AES; only the blocks actually
   read are decrypted, so an import takes ~30 s), reads the HFS+ volume, and
   copies out just the speech engine and voices — verified byte-identical to
   a Mac `hdiutil` extraction.
3. Once an engine is in place, the same dialog lists what can be downloaded
   from Apple's still-live servers: enhanced iOS voices (MobileAsset zips)
   and the Mac OS X voices (software-update packages; xar → gzip cpio →
   ROT13-named tar, unpacked in pure Python).

Data lives in `<NVDA user config>/classicvoices/` (`Voices/` from firmware,
`Downloads/` from Apple's servers), surviving add-on updates.

## Architecture (x86 and x64)

NVDA before 2026.1 is a 32-bit process; 2026.1+ is 64-bit. The add-on ships
both `cvengine32.dll` and `cvengine64.dll` and loads the one matching the
Python process. The 32-bit engine uses a 512 MB guest address space and
caches one engine (address space is scarce under WOW64), so the largest Mac
premium voices (~750 MB data) need 64-bit NVDA; everything else works on
both.

## Building

```
python nvda/build.py           # DLLs (MinGW-w64 gcc, x64 + i686) + .nvda-addon
build/nvda/cvtest64.exe <Voices root> <Downloads|-> ios6/en-US "Hello" out.wav
```

CI builds the add-on in `.github/workflows/nvda.yml`. `build.py` refuses to
package any Apple-derived file into the add-on.
