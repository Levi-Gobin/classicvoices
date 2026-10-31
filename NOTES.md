# Classic Voices — engine notes

Goal: run the original Nuance Vocalizer voices from old iPhone firmware
(iOS 3.1.3 and 6.1.6, iPhone2,1) and eventually expose them to VoiceOver on
modern iOS via an `AVSpeechSynthesisProviderAudioUnit` extension.

The app is **Classic Voices** everywhere: ClassicVoices.xcodeproj (targets
ClassicVoices, ClassicVoicesMac, Synth), bundle IDs com.levigobin.ClassicVoices(.Synth,
.mac, .mac.Synth), App Groups group.com.levigobin.ClassicVoices (iOS) and
7WV49AN2BX.com.levigobin.ClassicVoices (macOS), voice identifiers
com.levigobin.ClassicVoices.<build>.<voice>, log lines tagged CLASSICVOICES.

## Status

- **iOS 6 on iPhone: working.** `ios/` is an XcodeGen project: the `ClassicVoices`
  app plus a `Synth` speech-synthesizer audio-unit extension that offers all
  36 iOS 6 voices to the system. On an iPhone 17 Pro Max it reaches first
  audio in 5–13 ms and runs 14–19× faster than real time.
- **iOS 3 on iPhone: working.** All 34 iOS 3.1.3 voices (Samantha, Serena, Karen, …)
  ship alongside iOS 6's as "Name (iOS 3)". On device they reach first audio in
  6–17 ms and run 20–26× faster than real time.

## Architecture (iOS 6)

- `core/thumb_ops.h` holds the Thumb-2 integer semantics (`exec16`/`exec32`
  on a `Core`). The profile showed the engine never uses VFP, NEON, or ARM mode.
- `core/thumb.c` is the interpreter run loop. It also dispatches into
  recompiled code and calls traps for imports.
- `core/vxengine.c` loads the Mach-O, implements libc in C, provides the
  mmap-backed data-mapping interface, and drives Vocalizer.
- `core/vxstream.c` runs synthesis on a worker thread and feeds a lock-free
  ring buffer that the real-time render callback drains.
- `tools/recomp.py` is the static recompiler. For each hot function it emits
  C that calls exec16/exec32 with constant encodings, which clang inlines and
  folds. Registers live in a local `Core`. Direct calls are native, the
  division helpers are inlined, and anything unknown goes back to the
  interpreter. It's driven by a block-start profile (`build/profile` over
  `tools/corpus.txt`).
- Verification: `tools/verify_native.sh <lang>` checks that native and
  interpreted output are bit-identical over the corpus (it currently passes
  for en-US and en-GB). The C interpreter was checked bit-identical against
  the Unicorn/Python reference.

Rebuild everything:
```
build/profile build/profile6.bin            # (slow; only when coverage needs refreshing)
.venv/bin/python -I tools/recomp.py extracted/ios6/VoiceServices.framework/Support/voiced build/profile6.bin build/gen
tools/build_mac.sh                            # Mac CLIs
cd ios && xcodegen generate && xcodebuild ... # app (see project.yml)
```

## Firmware generations (all iPhone2,1)

| Build | Engine | Rate | Voices | Notes |
|---|---|---|---|---|
| iOS 3.1.3 7E18 | Vocalizer 5.2.0, stripped | 16 kHz | 34 | VFP; split broker (common + lang) |
| iOS 4.3.5 8L1 | Vocalizer 5.2.0, stripped | 16 kHz | 34 | new front-end data in most languages; new select DBs for es-MX, fi-FI, ja-JP, zh-HK; ru-RU Katerina became Milena; the lang broker lists only userdct + pipeline |
| iOS 5.1.1 9B206 | Vocalizer 5.2.3, symbols | 22 kHz | 36 | clc format; Samantha's (and Karen's, Zuzana's, Ida's, Julie's, Yuna's, Aylin's) select DB differs from iOS 6 |
| iOS 6.1.6 10B500 | Vocalizer 5.2.3, symbols | 22 kHz | 36 | |

Build profiles (stub address → interface tables and API addresses) are in
`kBuilds` in vxengine.c. iOS 4's API addresses: Init 0xC9618, Open 0xC90F0,
SetParamList 0xCA98C, SetOutDevice 0xC83B0, Process 0xCA738. IPSWs are
downloaded and decrypted with `tools/ipswtool.py ios <ver> iPhone2,1 -y --no-mount`
(the user's script; keys come from The Apple Wiki).

## Enhanced voices (MobileAsset)

- Asset type `com.apple.MobileAsset.VoiceServicesVocalizerVoice`. The catalog
  is still live at mesu.apple.com/assets/com_apple_MobileAsset_VoiceServicesVocalizerVoice/…xml
  and the zips are still on appldnld.apple.com.
- `TTSResources/voice_format_version.plist` says which format a firmware reads:
  **iOS 5 = 3, iOS 6 = 4**. Each format has an enhanced voice for **all 36
  languages**, not just English (the format 3 and 4 Samanthas differ).
- An archive holds `AssetData/<lang>.tmp/`: `broker.hdr.asset` (XML only, no
  defaultvoice line), `clc_*_cfg3.dat`, `uselect_<voice>_full.dat`, and
  `synth_<voice>_full_155mrf22_*.dat` (about 107 MB, needing a 512 MB guest
  address space).
- The app downloads them on demand (EnhancedDownloads.swift: SHA-1 check, zip
  extraction) and hands them to the speech extension's own store (see "Voice
  store" below) at <store>/<build>/<lang>. Engine keys are "ios6e/en-US".
- `tools/fetch_assets.py <catalog> <format> <dir>` mirrors them on the Mac
  (downloads/assets5, downloads/assets6).

## iOS 3 (7E18) differences

- It's the same Vocalizer 5.2 family (5.2.0.8035), but `voiced` is stripped.
  The API was found by call pattern: Initialize 0xC0510, Open 0xBF914,
  SetParamList 0xBECE0, SetOutDevice 0xBDBB0, ProcessText2Speech 0xBF61C.
  The interfaces are heap 0x190C54, critsec 0x190C44, stream 0x190C28 and
  mapping 0x190BFC. These are in the `kBuilds` table in vxengine.c.
- The broker data is `common/broker.hdr` joined with `<lang>/broker.hdr`, and
  the language name comes from `pipeline/<Language>/...`. Audio is 16 kHz,
  resampled to 22.05 kHz in vxstream.c with a 32-tap windowed sinc.
- It uses scalar **VFP** (iOS 6 doesn't). See `exec_vfp` in thumb_ops.h, which
  is verified against Unicorn per encoding by `tests/vfp_diff.py`. VMLA must
  not be fused, hence `#pragma clang fp contract(off)`.
- Inline commands: `ESC\pause=N\` works, but `ESC\eos=1\` and `ESC\pitch=N\`
  are ignored.
- **How the original iOS 3 VoiceOver spoke** (VoiceOverTouch.app/vot, reverse
  engineered): `addString:` makes one output action per piece (label, value,
  each trait, hint). `_handlePerformAction:` sends each piece to
  VSSpeechSynthesizer as its own `startSpeakingString:`. So the break between
  pieces is the engine's end-of-utterance silence, with utterance-final
  intonation and no added punctuation. Its only inline pause for this was
  `ESC\pause=800\` in place of a `....HINT....` marker before hints; the other
  pauses are in its time, phone-number and spelling formatters.
  vxstream's `speak_segmented` reproduces this: each VoiceOver `<s>`
  (ESC\eos=1\) becomes a separate engine call with the natural silence kept.
  Pitch changes inside a sentence also split the call, with the parts joined
  tightly (at most 100 ms).
- It needs more libc than iOS 6 (strncmp/strstr/strtol/…, `_DefaultRuneLocale`
  for ctype).
- Recompiler: function boundaries come from BL targets (`--dis voiced3.dis`).

## Debugging the recompiler

- `VX_TRACE=1` logs every import call. Diff the interpreted run (`VX_INTERP=1`)
  against the native one to find the first divergence.
- Generating with `--debug-calls --debug-budget` gives a build where
  `VX_NATIVE_RANGE=lo-hi` limits which native entries run and `VX_BUDGET=N`
  hands back to the interpreter after N native instructions. Bisecting both
  pinpoints a bad translation down to a single instruction. This is how the
  "computed jump to the next address" bug was found.

## Latency

- The system pulls provider audio faster than real time (256-frame pulls
  milliseconds apart), so any silence we return gets queued in front of the
  speech. The render block now waits (up to 500 ms) for real samples instead
  of padding. The first pull now returns speech about 20–30 ms after the
  request.
- Leading and trailing engine silence is trimmed in vxstream.c, keeping 5 ms
  before speech and 30 ms after.

## Voice store (no App Group needed)

Downloaded and added voices used to live in the App Group. Sideloading tools
(Sideloadly with a free Apple ID at least) strip that entitlement: the iPad
copy reported `App Group ... (NOT available)`, every download failed with
"couldn't create the file", and testers saw the same. macOS also warns that an
ad hoc signed app "tried to access data from other apps" when it opens the
team-prefixed group. So, since 2026-10-18:

- The **extension owns the voices**: `<Synth container>/Library/Application
  Support/EnhancedVoices/` (`VoiceCatalog.extensionStore`), same layout as the
  group had. Its container belongs to the extension bundle, so what it writes
  while the app hosts it is there when VoiceOver hosts it. It survives app
  updates (the container UUID changes, the data stays).
- The **app hosts its own audio unit** (`AVAudioUnit.instantiate` with
  'ausp'/'cvox'/'ClVo'; the component manager lists 0 matches but instantiate
  works, 200-900 ms) and talks to it over `messageChannel(for:
  "com.levigobin.ClassicVoices.files")`. Messages are dictionaries with an
  "op": put (4 MB chunks into .incoming), commit, delete, write/read (small
  files), tree, catalog (the extension's VoiceCatalog.enhanced, JSON), test.
  `Shared/StoreOps.swift` implements them; `Synth/FileChannel.swift` is the
  extension side; `App/VoiceStore.swift` the app side. Measured 600-760 MB/s
  on an iPad Air 3; the extension writes to Application Support, Caches,
  Documents and tmp of its container (not its root).
- **Speaking is untouched**: the channel is only used while the app hosts the
  unit. First audio for a store voice equals a bundled one (80 ms on that
  iPad through AVSpeechSynthesizer; the first use of a voice adds its load
  time wherever the files live).
- **Old voices stay**: at every start the extension links whatever the App
  Group holds into the store (`VoiceCatalog.adoptLegacy`, symlinks refreshed
  because the group path can change). Deleting a linked voice deletes the
  target too. The app never touches the group itself any more (no macOS
  prompt), except `VoiceStore`'s fallback when it can't host the unit.
- The app's view of what's installed is `VoiceStore.snapshot` (tree +
  catalog from the extension); `VoiceCatalog.remoteEnhanced` /
  `remoteMacinTalkEngines` / `remoteHidden` make the shared code read it.
  Hidden voices go to `<store>/hidden.json` (old group defaults still read).
- Downloads finishing in the background can't host the unit, so installs are
  recorded as jobs in the app's Application Support/PendingInstalls and run
  when the app is next in front (`VoiceStore.flush`, on launch and
  scenePhase .active). Rows show 90 % until then.
- Dev flags run off the main thread now (`VoiceStore` blocks): `-report`,
  `-import <folder or Documents-relative path>`, `-latency [a.b,c.d]`,
  `-storetest key,key`. devicectl passes app args after `--`.
- A copy without any group can be built for testing with
  `CODE_SIGN_ENTITLEMENTS=tools/nogroup.entitlements` (an empty plist) on both
  targets; that is what the iPad verification used.

## Apple's pronunciation rules and the compact fallback (2026-10-19)

- iOS 5 and 6 ship `TTSResources/<lang>/user_rules.txt` (29 languages; iOS 3
  and 4 have fewer, without the "iOS" rule): regex rules VoiceServices ran
  over the text before the engine, e.g. `/\biOS\b/ --> " I O S "`,
  `/OS X\b/ --> "OS ten "`, St/Dr before a line end -> Street/Drive, 49er,
  ETA, degree signs, German date ordinals. The engine (voiced) contains the
  string but never asks for the file (VX_FILES trace: it maps `userdct/enu`
  only), so with our engine "iOS" and "ios" rendered identically.
  `ios/Shared/PronunciationRules.swift` parses the file (`/re/flags --> "repl"`,
  `$1`, `\xHH`, trailing `# comment`) into NSRegularExpression and applies it
  to the SSML text tokens in `SpeechJob.from(ssml:rules:)` and to samples in
  the app. iOS 6's file is used for every voice of that language (Mac Nuance,
  MacinTalk, iOS 3-5 included), falling back to the voice's firmware or iOS 5.
  Apple's own quirks are kept (German `06_MTH_` leaves "Juni_").
- iOS 6 VoiceOver (`vot`) has `_useCompactVoice`, `setFootprint:`,
  `_handleUsePitchChange:`; VoiceServices' `-[VSSpeechSynthesizer setFootprint:]`
  picks compact vs premium. An enhanced voice ignores pitch, so pitched speech
  used the compact one. `vxs_start_with_fallback()` does the same: with a
  fallback key, pitch commands always split the text (inline pitch off) and
  every segment whose engine pitch != 50 is spoken by the fallback engine
  (same sample rate required; rate/volume copied). The fallback loads on
  first use (never evicting the primary: `engine_for_opt(..., keep)`) and is
  warmed after each utterance without evicting anything (3 GB devices simply
  get no fallback). Verified on the Mac with `build/fbtest/fbtest`
  (scratch program): the pitched segments go to the compact engine and a
  whole-utterance pitch renders byte-identical to the compact voice itself.
  `VoiceCatalog.compactCounterpart(of:)`: ios6e/<lang> -> ios6/<lang>,
  mac107/Karen -> mac107/KarenCompact; MacinTalk none.
- Which voices actually ignore pitch (`Entry.fixedPitch`), measured 2026-10-20
  with fbtest at 100% vs 140% (F0 by autocorrelation on the float output):
  ios5e/ios6e enhanced, mac107 and mac108 premium -> byte-identical, F0
  unchanged (Vocalizer 5.2.3 unit selection has no pitch control). Every
  compact voice and the mac109 premium voices (iOS 10 engine) -> F0 x1.4.
  So Mavericks premium voices get no fallback (they changed pitch themselves
  and the compact would have spoken for nothing); the fallback's own pitched
  output equals the compact voice at that pitch.

## Heap overflow in speak_segmented (fixed 2026-10-20)

- 13 Synth crashes on the phone in one afternoon, all "BUG IN CLIENT OF
  LIBMALLOC: memory corruption of free block", detected at random malloc
  sites (dumpRenderLog, NSXPC decoding, the ios10 recompiled code, vx_open).
  Cause (pre-existing since the Mavericks voices, 85b1cf4 2026-10-11): for
  the percent-pitch engines (ios7/ios10 = Mavericks voices) speak_segmented
  rewrites inline `ESC\pitch=N\` commands to percent values, which can be a
  byte or two longer than the originals, but the segment buffer was sized to
  the input text and only grew when the command itself didn't fit at that
  moment, so the trailing NUL (and later characters) went past the end. Every
  Mavericks utterance with a capital letter corrupted the heap a little.
  Found in seconds with `tests/fbstress.sh` (Address Sanitizer build of a
  VoiceOver-like stress: fallbacks, pitch tags, cancels, preloads); clean for
  3 x 400 iterations after the fix (buffer tracks its capacity and grows
  before every write). Also fixed: dumpRenderLog could trap on arithmetic
  overflow when a render was logged between the count reset and the new start
  time (Mac crash 2026-10-17).

## Apple Vision Pro, and why not Apple TV (2026-10-20)

- Targets ClassicVoicesVision/SynthVision (visionOS 1.0) share the iOS
  sources through `targetTemplates` in project.yml (same bundle ids,
  entitlements, PRODUCT_NAME ClassicVoices / Synth so packaging and the
  `Synth.appex` lookups don't change). `tools/package_ipa.sh visionpro` ->
  build/ClassicVoicesVision.ipa; the workflow's iphone job builds it when the
  targets input has `visionpro`; the one-line scripts offer it as choice 4
  (several numbers may be typed). Guards are `canImport(UIKit)` /
  `!os(macOS)` rather than `os(iOS)`, which is false on visionOS.
- **tvOS is out.** The tvOS SDK declares AVSpeechSynthesisProviderAudioUnit
  for tvos(16.0) and a tvOS build compiled, but installing it in the tvOS 27
  simulator failed: "NSExtensionPointIdentifier com.apple.AudioUnit ... does
  not correspond to a known extension point". The runtime's own definition,
  `System/Library/ExtensionKit/ExtensionPoints/com.apple.audio.units.appexpt`,
  lists `EXSupportedPlatforms = [iOS, xrOS]` for `com.apple.AudioUnit`: audio
  unit extensions, and so third-party voices, don't exist on Apple TV. (tvOS
  also lacks AUMessageChannel, a Files picker, pasteboard, Slider/Stepper,
  textSelection, and persists only Caches, had it mattered.) The tvOS targets
  and guards were removed the same day.
- Sideloading to Vision Pro: Xcode (Developer Mode on the headset) or
  SideStore; Sideloadly doesn't list it.

## NVDA importer: iOS 7 and 10 engines (fixed 2026-10-23, add-on 0.1.1)

- The add-on's "Import firmware (IPSW)" produced empty engines for iOS 7
  and 10: their `voiced` / `TTSSpeechBundle` are HFS+-compressed (empty data
  fork, com.apple.decmpfs type 4, zlib chunks in the resource fork), and
  iOS 10.3.4's rootfs is HFSX inside a GPT (not an Apple partition map, and
  not APFS). applereaders.py now reads GPT and decmpfs types 3/4; firmware.py
  gained an OpenSSL/cryptography AES path besides CNG and a reusable
  import_rootfs(). Verified with import_ipsw on the real IPSWs: iOS 3, 7 and
  10 engines and voice folders byte-identical to the Mac's hdiutil
  extraction; DLLs and the add-on still build (zig cc for MinGW).
## NVDA on Windows on Arm (tested 2026-10-25)

- World Designer's Windows 11 ARM64 VM (~/VMs/windows; ask that session
  first; work only in C:\classic-voices). NVDA 2026.2 portable there runs as
  ARM64EC and loads cvengine64.dll (x64, emulated) without complaint.
- The VM has no sound card, so NVDA can't open a WavePlayer (our driver,
  like NVDA's own, fails to load and NVDA falls back to OneCore). Tested
  with a test-only global plugin that swaps nvwave.WavePlayer for a
  real-time fake and calls setSynth again: 69 voices, speech, cancel, and
  IndexCommands each reached exactly when their segment's audio had played.
  extensionPoints hold handlers weakly: register module-level functions,
  not lambdas.
- Interpreter under x64 emulation: first sound 105-370 ms, about 1x real
  time. With fast code (cvnative_<build>_64.dll in <data>/Native): 17-55 ms,
  19-24 ms after an interrupt; cvtest output is byte-identical either way.
- The DLLs build on the Mac with zig cc (-target x86_64-windows-gnu /
  x86-windows-gnu) through a wrapper that drops -static, adds
  -Wl,--out-implib for the DLL, and links cvtest against that .lib. The fast
  code cross-compiles the same way from build/gen* (about 90 s per build and
  bitness, 32-bit with -DGUEST_MEM_SIZE=0x20000000u).

## SAPI 5 engine (windows/sapi, 2026-10-25)

- cvsapi64.dll / cvsapi32.dll: in-process COM engines (ISpTTSEngine +
  ISpObjectWithToken) in C over vxstream; README there has the mapping.
  Registering each DLL with its own regsvr32 puts its CLSID and tokens in its
  own registry view, so 64-bit and 32-bit programs each see all voices. On
  Windows on Arm, System32's (ARM64) regsvr32 registers the x64 DLL fine.
- Tested on the VM through SAPI itself (cvsapitest, WAV output), through
  System.Speech in 32-bit PowerShell (bookmarks at 880/1501/2020 ms), and
  through NVDA's sapi5 driver (the WavePlayer stand-in; it calls
  feed(pointer, size)). Output is byte-identical to the interpreter (cvtest)
  with fast code. Fast code, first audio: 64-bit 14-20 ms short, 40-45 ms
  long; 32-bit 21-27 / 44-60 ms; interrupt to next first audio 21-34 ms,
  engine stop 1-5 ms; SAPI itself adds about 0.2 ms.
- SAPI creates the engine object at the first Speak, not at SetVoice, so the
  first utterance also loads the voice (100-330 ms).
- Waiting for samples uses a high-resolution waitable timer: Sleep(1) can be
  a 15.6 ms tick.
- My C client got no events back from GetEvents even for Microsoft's voices
  (a client bug, not the engine); System.Speech receives them, so the bookmark
  test is PowerShell.
- The catalogue scan uses ANSI opendir/fopen, so registration scans through
  the data folder's 8.3 name when it has non-ASCII characters (tested with a
  junction named Données).

## Word positions, rules, Arm, OneCore (2026-10-26)

- **Word markers.** The engine already writes markers into the buffer
  vxengine hands it (VE_OUTDATA's pMrkList): VE_MARKINFO is **40 bytes**
  {?, type, src pos, src len (bytes of the UTF-16 input), dest pos (samples
  into the call), dest len, phoneme, ...}; type 1 sentence, 2 word, 4
  phoneme. The buffer was allotted 0x500 x 32 bytes while the engine was told
  0x500 markers: now 0x500 x 40. Output is byte-identical to before on every
  engine (iOS 3-6, 5/6 enhanced, Lion, Mavericks on iOS 7 and 10).
  iOS 3 and 4 report no word at a sentence's start, only the sentence: the
  stream adds its first word. vxstream maps each marker to an output frame
  through its trimming, held silent runs, resampling and segment joins, and
  to a byte of the request through the pitch/eos segment split
  (vxs_read_words). SAPI places an event where the audio written so far
  ends, so cvsapi writes up to each word, then adds its event.
- **recomp.py --scan-bl** finds a stripped engine's function boundaries (BL
  targets) itself, matching llvm-objdump's set exactly for iOS 3, 4 and 5:
  objdump prints an IT block's conditional BLs as "blne" etc., which the old
  regex skipped, so the scan skips them too. Generated code is identical to
  the --dis output and to build/gen3. llvm-objdump is no longer needed
  anywhere.
- **Apple's rules on Windows.** tools/pcre2_build.py fetches PCRE2 10.45
  (SHA-256 pinned) into build/ and compiles it into cvengine and cvsapi
  (CV_PCRE2_STATIC). cv_rules_apply_mapped keeps a byte map through the
  rewrites (PCRE2_SUBSTITUTE_REPLACEMENT_ONLY|MATCHED, 10.40+). The NVDA
  add-on gained a "pronunciation rules" setting and applies them per string
  before adding ESC commands; SAPI reads HKCU\Software\Classic Voices
  PronunciationRules / PitchFallback.
- **Hidden voices**: <data>/hidden.txt (a key per line), honoured by
  cv_scan_voices (SAPI registration, speechd, Android, cvsay) and the add-on's
  voices.scan; the app's "Use as a system voice".
- **Narrator/OneCore**: Windows.Media.SpeechSynthesis lists only its own
  voices. A token copied into Speech_OneCore\Voices\Tokens (64- or 32-bit
  view), even with Version/DataVersion/Vendor=Microsoft and Microsoft's own
  engine CLSID, never appeared (no reboot tried).
- **ARM64X forwarder (not working)**: x64 and ARM64 programs share COM's
  registration on Windows on Arm. lld 22 (zig lld-link) links /machine:arm64x
  /defarm64native pure forwarders. Without load configs the native view
  works (ARM64 System.Speech spoke) but x64 programs get the native exports
  (error 193 resolving cvsapi_arm64). With load configs (LLVM's
  loadconfig-arm64ec.s / loadconfig-arm64.s test inputs) LoadLibrary fails
  with 193 in both. MSVC 14.44's link on the VM rejects every forwarded .def
  export in /machine:arm64x ("unresolved external symbol Foo (EC Symbol)").
  The pieces are in the scratchpad's arm64x/; the x64 DLL stays registered on
  Arm. cvsapi_arm64.dll and cvnative_*_arm64.dll (loader suffix "arm64") build.

## The desktop app (desktop/, 2026-10-26)

- wxPython, native controls only; desktop/classicvoices/{system,library,ui}.py
  over the shared _classicvoices modules (now with remote.py, the Apple
  streaming moved out of linux/classicvoices-data). Windows: PyInstaller +
  Inno Setup (desktop/build_windows.py, desktop/windows/ClassicVoices.iss,
  .github/workflows/windows.yml); Linux: linux/install.sh installs it as
  `classicvoices` with a .desktop file.
- Tested with real screen readers: on the Windows VM a test add-on logs what
  NVDA speaks (patching speech.speech.speak) and keys go in through QMP
  sendkey (SendKeys from a scheduled task reaches the task's own hidden
  console instead); on the Linux VM Orca's --debug-file "SPEECH OUTPUT" lines
  and AT-SPI generateKeyboardEvent. UIA from PowerShell on the Arm VM shows
  every Win32 control as "Pane" (no client-side proxies there): use NVDA.
- Found that way: re-activating the window left focus on the wx.Frame, where
  Tab does nothing (now a panel holds the notebook and the frame passes focus
  on); a page rebuilt while its tab was hidden let Tab wander into it (only
  the page on screen is rebuilt); rebuilding the page every second for
  progress would re-announce focus (labels now update in place, in 5%
  steps); GTK screen readers missed focus set during a rebuild (CallAfter);
  Ctrl+Tab doesn't change GTK notebook tabs (handled); a slider read as
  "14" (a percentage of its range), now a list of rates.
- NVDA on the Arm VM froze the (emulated) app for ~9 s on focusing a text
  field holding Arabic; English is fine. Not seen elsewhere.
- linux/install.sh never restarted Speech Dispatcher: pkill -x
  speech-dispatcher can't match (process names keep 15 characters); now
  -x speech-dispatch. It also copies over a running module by renaming.

## Downloaded voices after a restart (2026-10-26)

- The user restarted the iPhone with VoiceOver on Tom (mac107, downloaded):
  every downloaded voice, Alex included, failed and VoiceOver showed the
  voice as "(null)" and spoke with Apple's Alex, until the app was opened.
  Cause: VoiceOver starts the extension before the first unlock, when files
  of the default protection class can't be read; scanEnhanced found nothing,
  and the scan was cached by folder modification times, which the unlock
  doesn't change. Opening the app asks the system to update the voices,
  whose speechVoices getter clears the cache.
- Fix: the extension's store (and new files in it) has no data protection,
  and unprotectVoices takes it off older files, each process its own (the
  extension can't change the App Group files the app wrote: 663 "don't have
  permission"). A scan made while a class-C probe file is unreadable is not
  cached; the first complete one afterwards calls
  AVSpeechSynthesisProviderVoice.updateSpeechVoices(). Not on macOS.

## Fast code built where the voices are (2026-10-26)

The user wants fast code everywhere ("so that everyone has an equal
experience"), never shipping Apple's code, as frictionless as possible.

- Windows/Linux app and NVDA add-on: one builder,
  `_classicvoices/fastcode.py` (moved out of desktop/system.py). It runs
  after every firmware import, and the app also at launch for firmware
  imported before; a failed or stopped build turns that off (app setting
  FastCodeAuto, Android pref fast_auto_off) until the user starts one from
  "Speed up the voices". Compiler: clang if there is one (Linux), else Zig
  from ziglang.org, SHA-256 from its index.json. Windows runs every compiler
  with CREATE_NO_WINDOW (a console per compile took the focus). The add-on
  carries recomp.py, the six Vocalizer profiles, thumb.h/thumb_ops.h/
  recomp_rt.h and native_glue.c in `_classicvoices/recompiler/` (added by
  nvda/build.py) and runs recomp.py in NVDA's own process
  (recomp.main(argv)); its dialog gained "Get firmware from Apple" (all
  missing firmware, streamed as the app does) and "Build fast code", can be
  closed while fast code builds, and reloads the synth when done.
- Android builds it on the phone: `Recompiler.java` is recomp.py in Java
  (byte-identical output for all six engines, checked on the Mac with the
  JDK, also with --per-file 1500, which the phone uses to keep compiles
  small); Zig 0.17.0 (aarch64-linux / x86_64-linux tarball, unpacked by
  android/jni/cv_untar.c with XZ Embedded, keeping only zig, lib/include and
  the musl headers: 194 MB) compiles with `-target <arch>-linux-musl` and
  links `-shared -nostdlib` against a stand-in libc.so so the library gets
  DT_NEEDED libc.so (without it dlopen fails on memcpy, RTLD_LOCAL; the
  stand-in must define what's used and the link needs --no-as-needed, or
  the dependency is dropped). Musl and bionic agree on everything the
  generated code touches (Cpu holds only a jmp_buf pointer).
- Android forbids exec of app data files for targetSdk 29+
  (execute_no_trans), but allows mmap PROT_EXEC of them (the avc log says
  "granted { execute }"), and allows exec from the APK's lib folder. So
  `libcvzig.so` (cv_zigrun.c, a PIE program in lib/<abi>/, which needs
  extractNativeLibs=true) maps Zig's static ET_EXEC segments at their link
  addresses (MAP_FIXED_NOREPLACE, 0x1000000 up), builds the stack (argv,
  envp, auxv with AT_PHDR/AT_RANDOM/AT_HWCAP/AT_SYSINFO_EHDR) and jumps to
  the entry point. Zig re-runs itself through /proc/self/exe (the loader),
  which finds Zig again through CV_ZIG; ZIG_LIB_DIR points at its lib.
- Measured on the emulator (4 cores, 3 GB: one compile at a time): iOS 6 in
  7 min, iOS 3 in 5; output WAV identical to the interpreter's; iOS 6 first
  audio 103 ms against 1.5 s, iOS 3 12x real time against 1.6x. A compile
  of the biggest generated file takes ~900 MB, so FastCode runs
  (free memory - 400 MB) / 1.2 GB compiles at once, at most 4. The engine
  is restarted at the next utterance (Native.reload, never mid-speech; a
  rwlock keeps cancel/preload off a stream being replaced). Zig is deleted
  when the build ends. 32-bit Android: no (memory, and the soft-float
  calling convention untested); data packs still work there.
- GitHub's iPhone/Mac build now makes all four MacinTalk generations and
  their fast code: Lion from Apple as before, and Tiger, Leopard and Snow
  Leopard from archive.org with tools/fetch_old_macintalk.py, which reads
  the discs through Panthera Speech's disc reader
  (third_party/panthera/pantherahfs.py, MIT, unchanged) over HTTP range
  requests (4 MB blocks, 8 connections, 64 MB read ahead while a package
  streams; archive.org gives ~1 MB/s per connection, ~10 MB/s in all here).
  It stops reading a package's cpio once it has passed
  ./System/Library/Speech/Voices/ (members are in path order), skips Alex
  unless --alex, and checks every file against tools/profiles/oldmt.sha256
  (made from extracted/ with --manifest). Results, all identical to
  extracted/: Snow Leopard (MacOSX10.6-Snow-Leopard-Retail-2Z691-6428-A,
  UDIF zlib) 1.1 GB read of 5.6, 7 min at 2.7 MB/s before the bigger blocks;
  Leopard (Mac_OS_X_Leopard_Install_DVD_Version_10.5_2Z691-6037-A_Apple_Inc._2007,
  raw APM .iso, so this is mt105's exact source) 1.4 GB of 8.1 in 3 min.
  Tiger (deadmoo-tiger-x86/tiger-x86.tar.bz2) can't be read in the middle:
  the whole 1.4 GB is read once (still in parallel ranges) and the 6.4 GB
  VMware disk inside written as a sparse file, then read (MBR, type 0xAF)
  and deleted.

- Alex (Leopard, Snow Leopard, Lion) is a download in the app
  (ios/App/DiscDownloads.swift): plan in front (read the disc's tables over
  HTTP through DiskImage/HFSVolume, now readers over any RandomAccess with
  `sources()` mapping a range down to bytes of the file on the server;
  11-39 MB, kept in blocks/), then download only the runs
  AdditionalSpeechVoices.pkg's payload is made from (Leopard 674 MB in 2
  runs, Snow Leopard 398 MB in 2, Lion 418 MB in 1; the Lion stack is
  dmg > HFS > InstallMacOSX.pkg > stored xar member InstallESD.dmg > HFS >
  Packages/AdditionalSpeechVoices.pkg) in 64 MB parts, 8 at a time, then
  read the package from blocks + parts (SparseSource) and keep only
  Alex.SpeechVoice. All three were byte-identical to the Alex copies on
  the Mac; Snow Leopard took 45 s, Leopard and Lion 110 s together.
  **Not the background URLSession:** on macOS nsurlsessiond fetched
  archive.org at ~8 KB/s (a plain session: ~1 MB/s per connection). Parts
  that finished are kept, and `resume()` (launch, app back in front) goes
  on from them. Test: `-download disc:mt106/Alex -disc-scratch <dir>`
  (voice to <dir> instead of the store).

## One layout, and progress the screen reader reports (2026-10-26)

- Every app has the same tabs and pages: iOS, macOS, Settings; a version
  menu and languages, a language's voices grouped by version, a voice's page
  (try it, system voice, remove), and on the non-Apple platforms "Get the
  firmware" and "Speed up the voices". Android's MainActivity is now that
  browser (Library.java is the model; the old Voices/Downloads screens are
  gone): tabs are buttons with TalkBack's role extra
  ("AccessibilityNodeInfo.roleDescription") and selected state, pages keep a
  stack per tab with Back and the system back key, and TalkBack's focus goes
  to the location line on a new page and back to the row that opened it.
- Progress (the user's rule): pass it on every 1% and let the screen reader
  decide how to report it. iOS/Mac: a downloading row's label stays "Stop
  downloading X" and the amount is its accessibilityValue with the
  updatesFrequently trait, so VoiceOver polls it at its own pace. wx (Windows,
  Linux, the NVDA add-on's dialog) and Android: a native progress bar under
  the Stop button (wx.Gauge, ProgressBar), set at each whole percent; the
  labels don't change with progress (a focused button's name change would be
  spoken every time).
- Android: a stopped enhanced download left its `<lang>.downloading` folder,
  which the catalogue then listed as a voice; partial folders are now removed
  whenever a download stops or fails. Debuggable builds log every text the
  engine is asked to speak (`adb logcat -s CVSPEAK`): that's how TalkBack's
  wording was checked on the emulator. (TalkBack 14 there ignores injected
  keyboard shortcuts and swipes; restart TalkBack after reinstalling the
  engine, or it stops speaking.)

## Android and Linux (2026-10-22)

- One shared C layer: `ports/posix/` (os_log shim, Darwin rune table,
  dlopen loader for `cvnative_<build>.so`, QoS no-op, force-included
  `vx_posix.h`) and `ports/host/` (voice catalogue after the NVDA add-on's
  voices.py with the iOS app's compact fallbacks, SSML parser after the
  Swift SSMLParser (marks as inline bookmarks since 2026-10-28), PCRE2 pronunciation rules
  loaded by dlopen). MacinTalk is stubbed (nvda/src/mt_stub.c): Panthera
  Speech covers it on both platforms (its Android APK runs Tiger-Lion via
  Box64; its Linux host is a CLI, no speechd module).
- Data layout everywhere: Voices/<build>, Downloads/<build|era>, the NVDA
  add-on's; Mac voice folders get a `locale` file (else the broker's
  langcode is mapped). Native/<abi>/cvnative_<build>.so.
- Output is byte-identical across Mac (arm64), Linux (x86_64, zig cc) and
  Android (arm64 emulator, through the TextToSpeech API), interpreter and
  fast code alike.
- **Firmware facts the importers rely on** (all six fetched and checked):
  every IPSW deflates its root filesystem (no random access into the zip,
  so it is streamed and inflated into a temp file; the IPSW itself is never
  stored). iOS 3-7 rootfs are encrcdsa, and the plaintext is a **UDIF
  (zlib) image**, not a bare volume: the decrypted header starts 78 01.
  iOS 10.3.4's rootfs is unencrypted UDIF holding **HFSX in a GPT** (EFI
  partition + "disk image" Apple_HFSX); it is not APFS (that conversion
  happens on the device). iOS 7's `voiced` and iOS 10's `TTSSpeechBundle`
  are **HFS+-compressed** (empty data fork, com.apple.decmpfs type 4: zlib
  chunks in the resource fork). The NVDA reader (applereaders.py) gained
  GPT and decmpfs support, so iOS 7 and 10 imports work on Windows too
  (before, they produced empty engines). Keys: The Apple Wiki API.
- Enhanced voice URLs are http://appldnld.apple.com; the same files are at
  https://secure-appldnld.apple.com (Android blocks cleartext).
- Linux: `sd_classicvoices` vendors speechd's BSD module helpers
  (linux/speechd/, upstream 5f44f03) with tiny type headers, so it needs no
  dev package. Its own server-audio writer (module_tts_output_server reads
  stdin between chunks, unsafe off the main thread). speechd sends
  `<speak>` SSML with `<mark name="__spd_N"/>` after every sentence; volume
  100 is speechd's default and maps to the voice's own level (80). The
  server then scales samples itself (x0.925 at volume 100). Autodetected in
  ~/.local/libexec/speech-dispatcher-modules (speechd 0.11+). Tested with
  speechd 0.12.0 unpacked into $HOME (SPEECHD_PLUGIN_DIR, -C, -m) and ALSA's
  file plugin as the sink: 138 voices listed, audio = engine output x0.925.
- Android: no Gradle (android/build.sh: NDK clang, aapt2, javac -source 8,
  d8 --min-api 21, zipalign -P 16, apksigner); minSdk 21 is the NDK r30
  floor, targetSdk 34. 16 KB pages: max-page-size=16384, libs stored
  uncompressed. `dlopen` of fast code from the app's data dir works with
  targetSdk 34 on Android 13 when the file is read-only (pack import marks
  it so). 32-bit ABIs: GUEST_MEM_SIZE 512 MB, MAX_ENGINES 1, and the fast
  code for them must be compiled with the same GUEST_MEM_SIZE (the bounds
  checks are inlined). Data in device-protected storage, service
  directBootAware.
- Speed (emulator on M-series, arm64): interpreter 1.1-1.9x real time; iOS 6
  fast code 4.7-8.6x (first audio 42-80 ms); Lion premium 12x. Linux VM
  (2 vCPU x86_64): 0.9x interpreted, 6.1x with fast code.

## Devices with 3 GB of RAM (iPad Air 3, iPhone XR/XS/11, iPad 7-9, mini 5)

A process there may map only ~1.3 GB of anonymous memory (mmap fails with
ENOMEM beyond that, PROT_NONE reservations included; measured on an iPad Air 3
in both the extension and the app). The engines' flat guest spaces hit this:

- Vocalizer reserves 1 GB per engine (`GUEST_MEM_SIZE`), so a second engine
  (another language or firmware) failed to open and VoiceOver went silent on
  every voice switch. `engine_for` (vxstream.c) now closes every other engine,
  MacinTalk included, and retries once when an open fails. Switching costs a
  reopen (milliseconds) on such devices; unchanged elsewhere.
- MacinTalk reserved a fixed 2 GB (`X86_MEM_SIZE`), so `mte_open` failed with
  "cannot reserve guest memory". `mt_create` now tries 2 GB, then 1.25 GB
  (heap to 0x20000000, 736 MB of file mappings: Leopard's 669 MB Alex fits),
  then 1 GB (heap to 0x18000000). The layout lives in `Mt` (heap_hi, map_lo,
  map_hi, stack_top) and `X86.mem_size`; `CLASSICVOICES_MT_GUEST_MB=1280`
  forces a smaller layout for testing (`build/mtsay`). Fred and Alex render
  bit-identically in all three.
- `-speaktime a.b,c.d` (speak + didFinish timing) is the reliable device check;
  `AVSpeechSynthesizer.write` returns a lone 256-frame buffer for some engines
  even when speech is fine.

## macOS

- XcodeGen targets `ClassicVoicesMac` and `SynthMac` share all the code. The Mac
  App Group is team-prefixed (`7WV49AN2BX.com.levigobin.ClassicVoices`). The app
  and extension are both sandboxed, and the app has network.client.
- Install with `tools/install_mac.sh`. macOS only takes providers from
  /Applications. After a reinstall, **axassetsd can keep a stale audio
  component** and fail every load with `TextToSpeech.MultiError error 1`
  without launching the extension. `killall axassetsd` fixes it, and the
  script does this.
- Voice files live in Synth.appex/Contents/Resources/Voices
  (`Bundle(url: appex).resourceURL`).
- In this zsh, `log` is a shell builtin: use `/usr/bin/log`.

## iOS integration gotchas

- The extension must use `NSExtensionPointIdentifier` = `com.apple.AudioUnit`
  (no UI) with a plain `AUAudioUnitFactory` NSObject as its principal class.
  With `com.apple.AudioUnit-UI` the unit still loads, but axassetsd never
  asks it for voices, so nothing shows up in Settings.
- iOS prefixes voice identifiers with the extension's bundle ID
  (`com.levigobin.ClassicVoices.Synth.<ours>`), so match them by suffix.
- VoiceOver sends SSML like `<speak><prosody rate="189.99995%" volume="+0.0dB"><s><lang xml:lang="en-US">…</lang></s></prosody></speak>`.
  The rate is a percent of normal; the engine's own limit is 400.
- Device logs: `.venv/bin/pymobiledevice3 syslog live` (needs USB; over Wi-Fi
  only `sudo log collect` works, and it failed with "Device not configured").

## Speech parameters (VE_PARAM ids, iOS 6)

2 = volume (0–100, default 80), 3 = rate (% of normal, 50–400),
4 = pitch (1–100, **50 = normal**; F0 ratio 0.56×–1.58×, mapped in vxstream.c), 7 = language name, 8 = voice name,
1 = sample rate in kHz, 12 = input char type (1 = UTF-16).

```
.venv/bin/python -I tools/vocalizer6.py "text" out.wav [--lang en-GB] [--trace]
```

## Where things are

- Firmware rootfs DMGs (already decrypted) live in `~/ipsw-downloads/*_extracted/`.
- `extracted/ios6/VoiceServices.framework/` was copied out of the 6.1.6 rootfs.
  - `Support/voiced` is a 32-bit armv7 Mach-O daemon. **The whole Vocalizer
    engine is statically linked into it**, and the iOS 6 build keeps about
    2,100 symbol names.
  - `TTSResources/<lang>/` holds `broker.hdr`, `clc_*`, `select_<voice>_bet2f22.dat`
    and `userdct_*`. `TTSResources/common/` holds `lid.dat` and `synth_med_fxd_bet2f22.dat`.
- iOS 3.1.3 uses the same idea with an older engine (`VoiceServicesVocalizerEngine-29`),
  older data files (`depes_`, `sysdct_`, `tokentn_`, `bet2f16`, 16 kHz), and a stripped `voiced`.

## iOS 6 engine API (Vocalizer 5.2, "vauto" = Vocalizer Automotive)

All of these are Thumb functions in `voiced`, which loads at its preferred
base 0x1000 with no rebasing needed.

| fn | addr | call |
|---|---|---|
| `vauto_ttsInitialize` | 0xF82B0 | `(VE_INSTALL*, VE_HSPEECH* out[2 words])` |
| `vauto_ttsOpen` | 0xF7424 | `(hSpeech w0, w1, hHeap=0, hLog=0, [sp]=VE_HINSTANCE* out, [sp+4]=0)` |
| `vauto_ttsSetParamList` | 0xF6620 | `(hInst w0, w1, VE_PARAM*, count)` |
| `vauto_ttsSetOutDevice` | 0xF542C | `(hInst w0, w1, {pUserData, pfCallback}*)` |
| `vauto_ttsProcessText2Speech` | 0xF70B4 | `(hInst w0, w1, {format=0, byteLen, UTF-16LE text}*)` |

**VE_INSTALL** (built by Apple at voiced 0x5c7e):
`+0 u16 0x520 | +4 broker XML char* | +8 heap iface | +0x10 critsec iface |
+0x18 stream iface | +0x1c mapping iface | +0x20 hDataClass`

- The interface tables are in `__DATA`: heap 0x208B08, critsec 0x208B18,
  stream 0x208B28, mapping 0x208B44.
- I replaced the mapping interface with a Python version, because Apple's
  caches mappings in CFDictionary. The Python version does
  open/close/map(h, off, *len, **ptr)/unmap/freeze.
- The broker XML is `broker.hdr` with its first line (`defaultvoice:<Name>\n`)
  stripped.

**hDataClass** (Apple object, 0x1c bytes): `+0x14` = directory count and
`+0x18` = `char*[]` of search directories. Resource names like
`clc/enu/cfg1` become `clc_enu_cfg1.dat`: `/` turns into `_`, and `.dat` is
appended when the name has no dot.

**VE_PARAM** is 0x84 bytes: `u32 id` plus a union of a u16 and `char[128]`.
voiced sets: 7 = language name (e.g. "American English"), 8 = voice,
1 = 22 (kHz), 12 = 1 (UTF-16 input).

**Callback** is `(hInst w0, w1, pUserData, VE_CALLBACKMSG* {u32 msg, ?, pParam})`.
- msg 0x8 means OUTBUFREQ: fill `VE_OUTDATA {fmt, cntPcmBufLen @+4,
  pOutPcmBuf @+8, cntMrkListLen @+0xc, pMrkList @+0x10}`.
- msg 0x10 means OUTBUFDONE: PCM is at `+8`, and its byte count is at `+4`.
- Output is 16-bit mono at 22050 Hz.

## libc surface the engine needs

malloc family, mem*/str*, fopen/fread/fseek/ftell/fclose, open/close/stat/fstat,
mmap/munmap, pthread mutex/once/tls, setjmp/longjmp, qsort (calls back into
emulated code), soft-div helpers (`__divsi3` …), `floor`/`roundf`/`__fixdfdi`/
`__floatundidf` (softfp, so doubles are passed in r0:r1), and `*printf_chk`.
CoreFoundation is never touched once the mapping interface is replaced.

## Next steps

1. Get the iOS 3 engine running. Its `voiced` is stripped, so find the
   `ttsInitialize` call site by matching against the iOS 6 code and the strings.
2. Move from Python to native: either a small ARMv7/Thumb-2 *interpreter*
   (iOS forbids JIT, so Unicorn's TCG won't work in an extension) or static
   recompilation. Either one gets wrapped in an `AVSpeechSynthesisProviderAudioUnit`.
3. Keep firmware-derived files out of anything distributed. The app should
   import them from the user's own IPSW.

## Mac OS X Nuance voices (Lion 10.7, Mountain Lion 10.8, Mavericks 10.9)

The Mac's MultiLingual/Polyglot synthesizers are x86, but their voice data is
the same Vocalizer 5.2.3 family as iOS, so the iOS ARM engines run them.

| Era | Data | Engine we use | Notes |
|---|---|---|---|
| Lion 10.7 (`mac107`) | 5.2.3.102xx, bet2, old layout (depes/pp/tokentn/sysdct) | iOS 6 | Lion's own `lid.dat` (from BaseSystem.dmg) |
| Mountain Lion 10.8 (`mac108`) | 5.2.3.12xxx, bet2, clc layout | iOS 6 | common data = iOS 6's |
| Mavericks 10.9 (`mac109`) | bet3 + new pipeline (pp/text_parser, fe_voice_switch, featextract) | **iOS 7** | iOS 6 rejects these (SetParamList 0x8000000d) |

- Voice bundles: `<Name>.SpeechVoice/Contents/Resources/PCMWave` is a ustar
  archive whose member names are ROT13 (`tools/unpack_macvoice.py`); the
  synthesizer's `CommonData` is the same format. Unpacked compacts are in
  `extracted/mac107|mac108|mac109/<Name>Compact/` with `common/`. Lion's
  Kyoko/Ting-Ting/Virginie/Yannick compacts and CommonData are only in the
  installer's BaseSystem.dmg, not its packages.
- **iOS 7.1.2 (11D257, iPhone3,1)** is used only as an engine; the user
  doesn't want its own voices. `voiced` is a standalone armv7 binary exporting
  `ve_tts*` (same signatures as iOS 6's `vauto_tts*`); VE_INSTALL is still
  0x520. Interfaces: heap 0x281018, critsec 0x281028, stream 0x281038,
  mapping 0x281054; stubs at 0x27F968. It is built with NEON: VLD1/VST1
  (multiple), VMOV/VORR/VBIC immediate, VORR register, VADD/VSUB/VMUL.F32 and
  VCVT F32<->S32/U32 are implemented in thumb_ops.h (flush-to-zero, default
  NaN) and all 954 VFP/NEON encodings in the binary match Unicorn
  (`tests/vfp_diff.py build/voiced7.dis 30 <voiced>`). It also needs
  `__divmodsi4`/`__udivmodsi4`/`__divdi3`, and its pp/text_parser rejects a
  whole input that contains a surrogate pair, so non-BMP characters become
  spaces for that engine (vx_speak_utf8).
- Downloadable voices (System Preferences): Apple's software-update catalog
  (`index-10.9-mountainlion-lion-snowleopard-leopard.merged-1.sucatalog`)
  still lists them and swcdn.apple.com still serves them over https.
  `tools/mac_catalog.py` builds `ios/Shared/MacVoiceCatalog.json` (53 Lion,
  55 Mountain Lion, 76 Mavericks voices) from each product's .dist: Lion/ML
  dists carry `VOICEID:…;VERSION:…;LOCALE:…` and a minimum OS of 10.7 or
  10.8; Mavericks dists only name the voice, so locale/name come from the
  package file name. MLV_<ll>_<CC>_<name> and MLV_<ll>_<name>2.0.0SU are the
  same build posted twice.
- A package is xar → Payload (gzip, stored in the xar heap) → odc cpio → both
  `<Name>.SpeechVoice` (premium, `full_vssq5f22`, up to ~750 MB) and
  `<Name>Compact.SpeechVoice`. `ios/App/MacVoicePackage.swift` streams that
  whole chain (output byte-identical to pkgutil + unpack_macvoice.py).
- Lion and Mountain Lion premium voices run on the iOS 6 engine (Karen premium
  ~47× real time). Mavericks premium voices need the new pipeline (iOS 7+)
  *and* the vssq5 decoder, which iOS 7 dropped (it has `gesmc_vssq5` but not
  `gsmc_vssq5`; error 0x8150600d on `decoder/vssq5`), while iOS 6's front end
  can't read their clc data. **iOS 10.3.4 (14G61, iPhone5,1, unencrypted)**
  has both: `/System/Library/TTSPlugins/TTSSpeechBundle.speechbundle/
  TTSSpeechBundle` is a standalone armv7s MH_BUNDLE with `ve_tts*` exported
  (only its engine is used, as `Voices/ios10/voiced`). It loads at its
  preferred base 0 (no rebasing; the loader now copies any segment with file
  data). Interfaces: heap 0x264958, critsec 0x264968, stream 0x264978,
  mapping 0x264994; stubs 0x263AA4. No armv7s-only instructions are used.
  Its text path (Gryphon `FeatureExtractorOriginalText`) needs libc++
  `std::string`, which is HLE'd for the 32-bit libc++ layout, plus C++
  new/delete. Allison premium: 78× real time recompiled on the Mac, output
  identical to the interpreter.
- **Pitch scale differs by engine**: iOS 3–6 take `pitch` 1–100 with 50 =
  normal; the iOS 7 and iOS 10 engines take percent of normal, 50–200, with
  100 = normal (F0 measured linear; out-of-range values are ignored). Sending
  50 to iOS 7 dropped Mavericks voices an octave. vxstream converts the
  1–100 values (param and inline `ESC\pitch=N\`) for those engines.
- Engine per key (vxstream.c `engine_build`): mac107/mac108 → ios6;
  mac109/*Compact → ios7; other mac109 (premium) → ios10.
- Guest heap (vxengine.c, 40 MB): the iOS 7/10 engines allocate
  phrase-sized buffers, so a large-block free list that only reused exact
  sizes filled the heap after a few hundred varied phrases (live data stays
  ~0.6 MB); from then on every ProcessText2Speech returned 0x80000011 and the
  voice was silent until VoiceOver restarted. Large blocks are now best fit,
  split, and merged with free neighbours (plateaus ~9 MB; output identical).
- Lion Karen/Lee compacts (en-AU, FE 5.2.3.10238 cfg2) mispronounce on
  purpose: words found in `sysdct_ena_cfg2` lose `A`/`A+` (car, park, hot)
  and keep a final `R+` (here, door). Letter-to-sound, phonmap and the voice
  are fine (`ESC\toi=lhp\` input speaks correctly); real Lion did the same,
  so it stays. Lion Lee's select/synth files are identical to Mountain Lion
  Lee's; Lion Karen's select file is iOS 5 Karen's (ML Karen = iOS 6 Karen).
  The voice page says so (VoiceDetail.note).
- Korean/Chinese Mavericks compacts reject an input that is only "(" or ")"
  (pp/sent_parser 0x8d402000); the utterance is silent, nothing crashes.
- Guest address space is 1 GB (MAP region 0x04000000–0x40000000) so the
  largest premium synth files (Mikko, Maged, Ting-Ting, Melina, Anna …) fit.
- Voice keys: `mac107/KarenCompact`, `mac108/Karen` … The voice folder is
  `<root>/<era>/<folder>` when bundled, else `<group>/EnhancedVoices/<era>/
  <folder>`; common data is always `<root>/<era>/common`. The engine is iOS 6
  for mac107/mac108 and iOS 7 for mac109 (vxstream.c `engine_build`).
- Debug aids: `VX_FILES=1` logs every resource the engine maps (and whether it
  was found); `VX_DEBUG=1` also prints the engine's formatted error records
  (`vsnprintf`), e.g. `component{*}synth/bet1{*}lherror{*}8150600d`.

## MacinTalk (Tiger 10.4 / Leopard 10.5 / Snow Leopard 10.6)

Apple's i386 MacinTalk engines run on our own x86 interpreter (iOS allows no
JIT, so Unicorn is out), with the Mac OS APIs they import implemented in
core/mt. Behaviour, struct layouts and loader details follow the user's
reference, panthera-speech by Tamas Geczy (MIT, ~/panthera-speech), which
runs these engines natively on Windows/Linux and under Unicorn on Android.
Its shims assume guest address == host address (it maps the engine into the
low 4 GB); iOS reserves the low 4 GB of a 64-bit process, so this is a
re-implementation over a 2 GB guest space at `m->mem`.

- **core/x86**: i386 interpreter (integer, x87 subset, SSE/SSE2/SSE3,
  pshufb). `tests/x86_diff.py` runs every distinct encoding of the Leopard
  engine, SpeechDictionary and libstdc++ (44,492) against Unicorn: all match
  except where Unicorn is wrong or the comparison is meaningless (#DE on
  divide, movaps alignment faults, segment ops in data). NB: Unicorn keeps
  stale translated code after `mem_write` at the same address --
  `uc.ctl_flush_tb()` each trial (vfp_diff.py too).
- **core/mt**: loader (mt.c: fat slices, classic relocations, indirect
  symbols, self-modifying __IMPORT jump tables, external relocations,
  initializers; mt_dyldinfo.c: 10.6+ rebase/bind streams), and HLE modules:
  mt_libc.c (libSystem, printf, files, mmap, pthreads), mt_mp.c (UpTime /
  Durations on a clock running 128x real time, MP queues/tasks/critical
  regions as host threads -- each guest thread is an MtThread with its own
  CPU over shared memory), mt_cf.c (CoreFoundation in guest memory; bundles
  and URLs are paths; XML plists), mt_audio.c (AUGraph; scheduled audio
  slices collected at their sample times by a pacer thread that also
  completes them -- the engine's clock), mt_aac.c (AudioConverter bridged to
  the host's AudioToolbox, so Alex/Vicki AAC is decoded by Apple's own
  decoder), mt_misc.c (BLAS/vDSP in C, sgesvd via LAPACK, regex, sqlite).
- `build/mtcli <MacinTalk> <SpeechDictionary.framework/Versions/A>
  <Voice.SpeechVoice> text out.wav [dylibs...]`; MT_RATE=wpm, MT_VERBOSE=1/2.
- **Leopard: all 24 voices speak.** Fred "Hello there." at 180 wpm is
  byte-identical to panthera's reference (sha256 ec4be821..., 17360 frames,
  which panthera documents as identical to native x86). Vicki/Alex differ by
  a few frames from panthera's (different AAC decoders; ours is Apple's).
  Interpreter speed on the Mac: 15-30x real time; first audible sample 9 ms
  (Fred) to 79 ms (Alex).
- **Generations** (extracted/mtNNN = {MacinTalk, SpeechDictionary/, lib/,
  Voices/}; the app bundles them minus Alex):
  - mt104 Tiger: **MacinTalk 3.3** from the 10.4.1 developer build 8B1025
    (archive.org `deadmoo-tiger-x86`, an installed raw HFS+ disk; read with
    panthera's pantherahfs.py since macOS won't mount its journal). Every
    retail/restore Intel disc tried, including the original iMac 10.4.4
    (8G1165), carries MacinTalk 3.4, which calls `___commpage_dsmos` (Apple's
    "Don't Steal Mac OS X"); like panthera, we don't fake that. 23 voices.
    Fred "Hello there." 180 wpm is byte-identical to panthera's reference
    (sha256 cef98214..., 15792 frames). SpeechDictionary is prebound and
    split-segment; prebound imports are N_PBUD symbols, which the lookup must
    skip. Vicki decodes through the Sound Manager's SoundConverter (extended
    VBR records, `wave`/esds cookie), bridged to AudioToolbox.
  - mt105 Leopard (from the Leopard installer, + libstdc++.6.0.4): 24 voices.
  - mt106 Snow Leopard (archive.org MacOSX10.6-Snow-Leopard-Retail-
    2Z691-6428-A, md5-checked; engine/dictionary/libstdc++.6.0.9 from the live
    filesystem, voices from Essentials + AdditionalSpeechVoices): 24 voices.
    Uses dyld-info binding, `stat$INODE64` (108-byte stat), gettimeofday on
    the sped-up clock, and GCD timer sources (mt_gcd.c) instead of MP tasks.
- **AAC decoding uses Glint** (third_party/glint, MIT, CrispStrobe/glint at
  the commit panthera pins; bridge in core/mt/mt_glint.cpp, ADTS header added
  per access unit), not AudioToolbox: inside the speech extension iOS routes
  AAC to an out-of-process decoder the extension can't connect to
  ("AudioConverterOOP ... connection severed", NewSpecific doesn't avoid it),
  so Alex/Vicki were silent in VoiceOver while working in the app. Glint +
  2112-sample trim matches Apple's decode (correlation 1.000000, max diff 2).
- A stuck engine thread used to leak its 2 GB reservation ("cannot reserve
  guest memory" for every later MacinTalk voice). The stream now releases its
  cached Vocalizer engines before loading MacinTalk, and engine-thread faults
  go to os_log via mt_log (extension stderr is discarded).
- Embedded commands ([[rate 300]], [[pbas]], [[slnc]] ...) pass through to the
  engine, as with the original voices (the user wants them).
- **Out-of-range command values wrap inside the engine** (measured by PCM hash
  on Fred, Vicki and Alex, 10.4-10.7, all alike). rate is 16.16 fixed: 32768+
  speaks at the floor (Lion: already 32767.99). pbas becomes a 16-bit
  frequency: plateau from about 80 to 155, then it wraps to the bottom (200 =
  0). pmod levels at 200 and volm at 2, but both wrap past 2^32 as parsed.
  Floors don't wrap (rate 0-20, pbas 0-20, -N relative). mtengine's
  clamp_commands sets a value past the top to the top (32767, 127, 200, 2)
  and cuts a relative +N that would pass it; rate and pbas are tracked
  through the text (and across utterances) for that. Relative pbas is not
  quite additive ([[pbas 30; pbas +10]] differs from [[pbas 40]]), so
  relatives are left alone unless they'd overflow. No hex ($28 is 0); names
  are case-insensitive; `pbas+10` (no space) is ignored by the engine.
- **Leopard reads 24-hour times as 12-hour** (SpeechDictionary 3.5, every
  voice): 13:45 "one forty-five PM", 00:30 "twelve thirty AM", 18:05:45
  "eighteen hours five minutes ...". Tiger, Snow Leopard and Lion read them
  as written. mtengine respells times that can only be 24-hour (hour 13-24 or
  a leading zero) to "13 45", "18 oh 5", "0 0 30", "oh 7 30", which give
  Leopard Snow Leopard's exact phonemes. Detected from version.plist (3.4-3.9).
  `mte_phonemes` (SETextToPhonemes / 10.7 SECopyPhonemesFromText) shows what
  an engine makes of text; it is the way to check readings.
- **Rate curve.** The system sends rate as a percent: AVSpeechUtterance rate 0
  -> 12.5%, 0.5 -> 100%, 1.0 -> 400% (linear on each side). Today's Alex speaks
  48.7 + 1.737p wpm up to 100% (222 wpm) and 2.708p - 48.3 above (1035 at
  400%); exactly 0.5 is his default 173. Classic Voices used to speak base
  rate x p (Alex 175 x 0.5-4). "Same as today's Alex" (default) uses Alex's
  curve for every MacinTalk voice (mte_set_modern_rate); measured with
  AVSpeechSynthesizer.write on the Mac.
- **`say` turns embedded commands into SSML** before an Audio Unit voice sees
  them: [[pbas P]] arrives as prosody pitch +(2^((P - 51.18)/12) - 1)*100%
  (pbas 127 = +7883%), [[rate 200]] as rate 112%. "+N%" is a plain multiple;
  read as a power of two it overflowed past pbas 171 and fell back to the
  normal pitch, and Vocalizer's 0.56-1.58 cap held MacinTalk to ~166 Hz.
  MacinTalk keys now get the exact percent (ESC\pitchpct=N\, linear reading)
  and Tiger Fred tracks today's Fred (760 Hz at pbas 127). Vocalizer keeps the
  power-of-two reading of VoiceOver's changes, capped. VoiceOver itself spaces
  out "[[" so it never sends commands.
- A command `say` doesn't know (inpt, cmnt ...) at the very start of the text
  loses its first bracket (" [inpt TUNE]]ppp", read aloud as "inpt tune");
  anywhere else it arrives whole (hence the user's leading-quote trick).
  mtengine's restore_command puts the bracket back.
- **Lion names every sung ([[inpt TUNE]]) word U+266B** (beamed eighth notes).
  Our 10.7 CFStrings are MacRoman bytes and made it "?"; Alex's
  MTMBSelectUnits::FindWordCandidates then found "?" among his recorded words
  and sang "question" for any run of TUNE consonants. Characters MacRoman
  lacks now get spare control bytes that read back as themselves
  (mt_lion.c), and mtengine turns control characters in input into spaces.
- **A voice's rate is not always words per minute** (tools/mt_rate_table.py
  measures it, core/mt/mt_rates.h): Tiger's formant voices run away above
  ~250 (asked 500, Fred speaks 1281; top 2200), Leopard-Lion's compress
  (asked 1000: 553-703), Tiger Vicki/Victoria/Agnes/Bruce level off at
  370-600, Tiger Cellos/Organ/Good/Bad News drop to 32 wpm above 180. The
  modern curve asks each voice for the rate that gives the target speed
  (within ~1% where the voice can reach it, else its own ceiling). Classic
  and embedded [[rate]] are left as the engine takes them.
- Testing the extension path: app launch arg `-voicetest mt105/Alex,...`
  speaks via AVSpeechSynthesizer (= the extension); watch with
  `.venv/bin/pymobiledevice3 syslog live -pn Synth`.
- **AAC priming**: the engines ask for kConverterPrimeMethod_None, which the
  current decoder rejects; without trimming the 2112-frame codec delay every
  Alex/Vicki grain lands 96 ms late and sounds choppy (the user heard it).
  The converter is created with PrimeInfo leading=2112; SoundConverter units
  decode to exactly frameCount*1024-2112 samples.
- MacinTalk 3 divides by zero above ~320 wpm (harmless on PowerPC); the
  interpreter retries with divisor 1 (`div0_as_one`), as panthera does.
- Alex is not bundled (Leopard 669 MB, Snow Leopard 382 MB). iPhone: copy the
  .SpeechVoice into the app's Documents/MacinTalk/<gen>/ (`devicectl device
  copy to --domain-type appDataContainer --domain-identifier
  com.levigobin.ClassicVoices`); the app hands it to the extension's store at
  launch (MacinTalkImport.swift; file sharing is on too). Mac: Add from Files,
  or the same Documents/MacinTalk/<gen>/ inbox in the app's container.
- Engine API: core/mt/mtengine.h (mte_open/speak/close; one generation
  loaded at a time; shutdown stops all guest threads); vxstream keys
  `mt104/Fred` etc.; build/mtsay <gen dir> <voice> text out.wav [rate%].
- iPhone speed: Fred 35-41x real time (first audio ~15 ms), Tiger Vicki 13x,
  Alex 24-27x (first audio 65-120 ms).
- mt107 Lion (from the Lion installer; + libc++abi, libstdc++.6.0.9): 24 voices
  (Alex 402 MB, not bundled; the folder's Alex-sequoia.SpeechVoice is a
  modern Alex and is excluded). Imports in core/mt/mt_lion.c: `_kSpeech*`
  keys as CFStrings, SESpeakCFString/SESetSpeechProperty, blocks, synchronous
  dispatch_async/sync/once, vDSP FFT on the host's Accelerate. End of speech:
  arming the `_MTBEAudioUnitDeferredStopAudioGraph` timer (never fired).
  dispatch_after must run its block: Lion spins on a flag it sets before
  restarting the graph after a recent stop (the old no-op hung voice changes).
  iPhone: Fred 35x, Victoria 42x, Alex 28-32x, first audio 16-63 ms.
- Engine-instance state: anything caching a guest address (Lion's key
  objects, global queue, locale, setlocale's string, FFT setups) is cleared in
  mt_reset_globals. A stale key object once made Lion store the rate as its
  speech-done callback (jump to 0xb5).
- AudioUnitReset waits for the pacer to deliver every queued slice. 10.6/10.7
  reset the unit when their wait for queued slices times out (guest time runs
  128x, so a busy phone hits it) and then retire the slices themselves; the
  pacer completing them too drove the count negative and the retire loop ran
  off guest memory (Snow Leopard Alex cut at 2.2 s on iPhone, engine leaked).
- build/mtswitch <gen dir>:<voice> ... loads engines in turn in one process,
  keeping a generation loaded across consecutive entries like vxstream
  (MT_EXTRA=<group>/EnhancedVoices for the app-group Alex layout). The
  benchmark takes `-bench-only mt107` to run a subset.
- The user does not want iOS 10's ARM MacinTalk used (different sound).
- **Recompiled MacinTalk (2026-10-25).** Same idea as the Vocalizer fast code:
  `tools/build_mt_native.sh` loads each extracted/mtNNN with the host harness
  (tests/mt_golden.c) under `MT_DUMP=<dir>` (the images exactly as the loader
  left them: relocated, imports bound), and `tools/recomp_x86.py` turns the
  hottest functions (`--coverage 0.995` of tools/profiles/mtNNN.txt, recorded
  with `--profile`) into build/mtgenNNN/*.c. Instruction semantics are shared:
  core/x86/x86_ops.h works on an `X86Core` (registers, flags, SSE, x87) and
  takes the instruction's bytes as arguments; each guest instruction becomes
  a small static function calling `x86_exec_<group>` with constant bytes
  (group = m0..mf by opcode>>4, 0f0..0ff, sse, x87, so each compiles only its
  part of the interpreter), which clang simplifies alone and then inlines.
  The guest stack and return addresses are kept as on x86. At run time a set
  registers itself (constructor) and mt_native_attach uses it only if the
  `__text` ranges it was made from hash identically in the loaded engine.
  `MT_NATIVE=0` turns it off. Mac speed-up 3-6x per utterance (Leopard Alex
  long sentence 1112 -> 182 ms), mostly limited by the 128x playback pacing.
- Recompiled-code pitfalls found: (1) guest recursion now uses the C stack,
  so engine threads have 16 MB stacks and functions hand over to the
  interpreter below `X86.stack_limit`; (2) `ret n` (callee pops, e.g. functions
  returning a struct) must count as returning, or every such call leaves a
  nested interpreter frame and the stack runs out (Agnes hung); (3) the
  dispatcher must interpret an instruction when native code hands back without
  moving, or (1) livelocks; (4) loops get a compiler barrier so a wait on
  another thread's store re-reads memory; (5) the prefix loop had to be
  unrolled and the interpreter split by opcode group, or one file took >10 min
  and 5 GB to compile; Xcode's C debug info is off for the engine for the same
  reason.
  (6) clang needs about 1.5 MB per guest instruction in a file and more for
  one long function: Lion's 10000-instruction function took 10 GB, and the
  first GitHub build with all four generations (2026-10-27) swapped on the
  7 GB runner until the 150-minute limit. A function over `--per-file` (600)
  instructions is now split into parts that pass control to one another by
  `musttail` calls (one router per function picks the part by address), each
  free to go in its own file; no file has more than ~900 instructions.
  The C engines and generated code are a static library per platform
  (EngineCodeiOS/Vision/Mac) that the app and extension force-load, so they
  compile once: GitHub build 2:25 -> 1:11 for the Mac, iPhone app 2:16 ->
  1:09 (Vision Pro another hour, in the same job).
- Verification: tests/mt_golden.c prints a hash per utterance (every voice,
  rates 60-250, pitch 40-160, MT_GOLDEN_CORPUS=tools/corpus.txt adds the corpus);
  tests/x86_native_diff.py runs every generated instruction against the
  interpreter from random states (13091 for Leopard, all identical). Tiger:
  all 1104 utterances identical. Elsewhere Alex, Vicki and (in long runs)
  Bells/Boing/Lion voices differ, by pause lengths (e.g. 89 vs 72 ms after a
  semicolon) and +-2 frames at the end: MacinTalk sizes some pauses against
  its audio clock, so the output depends on how far it runs ahead of playback.
  The interpreter does the same when playback is slowed (MT_SPEED test: Alex
  287390 -> 287698 frames at 4x); with random delays the native build matches
  the interpreter exactly. A virtual audio clock would make it deterministic.
- Debugging recompiled code: `--debug-calls` (every call through the
  dispatcher) + `MT_NATIVE_RANGE=lo-hi,lo-hi` (hex) to bisect; `-DX86_TRACE`
  builds print each guest thread's last import and position when the engine
  gives up waiting.
- **Interrupting (2026-10-25).** `vxs_latency_bench` (app `-latbench key,...`,
  `LATBENCH_DETAIL=1` for each run) measures first sound after cancelling a
  long utterance. What it found:
  - Leopard and Snow Leopard Alex/Vicki don't honour a stop until they have
    synthesized the rest (a stop left to finish took 600-800 ms), so a cancel
    restarts the engine after MT_STOP_LIMIT (12 ms, now covering the stop and
    the drain after it). A voice whose last stop was cut off restarts
    straight away next time (every eighth tries a stop again): 30 -> 13 ms
    for Leopard Alex on the Mac.
  - Opening a channel parsed SpeechDictionary's Homophones plist (2086 keys)
    into an O(n^2) CFDictionary: 5-9 ms of every engine open. CF dictionaries
    now keep a hash index (mt_cf.c); open 9.4 -> 0.9 ms.
  - Lion after Snow Leopard in one process: mt_gcd cached the address of
    MTBEAudioUnitDeferredStopAudioGraph keyed on the Mt pointer, and the new
    Mt can reuse the old one's address, so Lion's graph-stop timer went
    unrecognised and every stop waited ~40 ms of synthesis (the phone's
    44 ms). Reset per engine now.
  - pthread keys were numbered across engines: Snow Leopard (a key per open)
    could not open again after 63 restarts in one process. Reset per engine.
  - After a restart with nothing queued, the stream reopens the engine and
    loads the voice at once (mte_prepare), not on the next utterance.
  - All of the above is MacinTalk, which the Windows, Linux and Android ports
    stub out. Their Vocalizer path, measured on an LG V60 (Android 13,
    Snapdragon 865, fast code): first sound 15-47 ms, and an interrupt adds
    about 3 ms over a fresh start (ios6 24.7 vs 21.0 ms for "Settings.
    Button."), so no stop wait there. Glue: Android and speechd poll every
    1 ms and stop at once. The NVDA driver held back each whole chunk until
    the next one came (for the index callback) and polled every 4 ms: 4-8 ms
    of every first sound. It now keeps back only the last 10 ms and polls
    every 1 ms.

## Speech commands inline, bookmarks, phonemes (2026-10-28)

- **What the engines take inside the text** (measured on every build):
  `ESC\tn=spell\ ... ESC\tn=normal\` everywhere (about 250 ms of pause after
  the letters); `ESC\rate=N\` and `ESC\vol=N\` on iOS 5 and later, on the
  parameters' own scale (iOS 3 and 4 ignore them: speak_segmented splits
  there and sets the parameters, joining with at most 100 ms, as it already
  did for pitch); `ESC\toi=lhp\ ... ESC\toi=orth\` (L&H+ phonemes) everywhere
  but iOS 4, which reads the symbols out. iOS 3 says nothing for a lone
  consonant in L&H+, but is fine with it right after a word.
- **Bookmarks.** `ESC\mrk=N\` gives VE_MARKINFO type 8 (src pos/len, dest pos,
  the id at +28) and leaves the audio byte-identical. The engines drop some:
  iOS 3 and 4 one at the very start of a call and the second of two in a
  row, iOS 5-7 one at the very end. So vxstream places each bookmark by its
  place in the text: the first engine marker (word, sentence or bookmark)
  at or past it reaches it, one right after a reported bookmark or sentence
  start (nothing but spaces and commands between) comes with it, and any
  left at the end of a call come at its end. All are reported, in order,
  before their audio (vxs_read_marks). MacinTalk can't say where it is
  inside an utterance, so mte_speak splits there and reports between the
  pieces (mte_set_mark_callback).
- **One utterance everywhere.** The NVDA driver writes each IndexCommand as a
  bookmark into one engine call (engine.py cuts the PCM it feeds at each
  bookmark's frame; a bookmark where the fed audio already ends rides on one
  sample of silence); SAPI speaks everything from the first text fragment to
  the last as one call (bookmarks, silence, spelling, rate/pitch/volume
  inline; leading and trailing silence still written directly, since the
  stream trims silence around speech); cv_ssml (Linux, Android) and the
  Swift SSMLParser put marks, prosody, `say-as` spelling and phonemes
  inline. With indexes or marks the audio is byte-identical to the same text
  without them (NVDA on the VM, SAPI via System.Speech, speechd module).
- **MacinTalk** gets `[[rate]]`, `[[volm]]`, `[[char LTRL]]`/`[[char NORM]]`
  for the same commands, drops L&H+ phonemes, and takes its own phonemes
  (`[[inpt PHON]] ... [[inpt TEXT]]`).
- **Phonemes** (core/vxphonemes.c, shared by every platform): IPA -> L&H+
  checked against the voice's own symbol table (the STMC/STMF key/value
  tables, chunk names XORed with 0xA9, in phonmap_*.dat or the voice's
  clc_*_cfg*.dat; the one whose keys hold ' and #), nearest sound when the
  language lacks one, Apple's iOS 10 phonemeMap-<locale>.json (imported, never
  shipped) first for tones and as a fallback; IPA -> MacinTalk's American
  phonemes for MacinTalk voices. SAPI `<pron>` arrives as phone ids of
  SAPI's American English set (1 - 2 ! 3 & ... 10 aa ... 49 zh, checked with
  SpPhoneConverter for 0x409). Android hands engines text, not SSML: verbatim
  TtsSpans become `say-as characters`.

## iOS 7–12 voices compared; pronunciation packs (2026-10-30)

Root filesystems of 7.0 (11A465), 7.0.6 (11B651), 7.1 (11D169), 7.1.2
(11D257) for iPhone3,1, 8.4.1 (12H321), 9.3.5 (13G36), 10.3.4 (14G61) for
iPhone5,1 and 12.5.7 (16H81, APFS, mounted with hdiutil) for iPhone6,1,
compared file by file (compact voices: TTSResources/<lang>/select_*_bet3f22.dat):

- 7.0 = 7.0.6. 7.1 changed 16 compacts; 11 of them (Samantha, Daniel, Karen,
  Thomas, Kyoko, Ting-Ting, Zuzana, Mariska, Luciana, Joana, Mei-Jia) are
  already the recording iOS 12 has. Tarik, Alice, Xander, Zosia and Alva have
  a 7.1-only version. 7.1.2 = 7.1 (voices and rules), and its voiced is the
  engine we already use.
- iOS 8: 19 more reach their final recording (Hebrew/Carmit new, Tarik ->
  Maged); iOS 9 the last 6 (Paulina, Satu, Alice, Nora, Milena, Carmit).
  10.3.4 = 12.5.7 byte for byte, rules included.
- iOS 7.0 compacts are the Mavericks compacts (select and clc identical)
  except Karen, Zosia and Mei-Jia (Mavericks Mei-Jia = 7.1's); iOS adds a
  per-voice <voice>_userdct_<lang>.dat and user_rules.txt.
- Enhanced (Samantha): format 5 (iOS 7) = iOS 6's synth + select with new clc;
  format 6 (iOS 8) new select DB; format 7 (iOS 9) = iOS 10+ premium,
  byte-identical. voice_format_version.plist: 7.x = 5, 8 = 6, 9-12 = "7,6".
  The catalog keeps one posting per format (format 5 is 7.1's, 2014-03-10).

Pronunciation packs ("Newer pronunciation rules"): VoiceServices.VoiceResources
MobileAssets, one per language. iOS 10 preinstalls compatibility 3, iOS 12
compatibility 8 (in /System/Library/PreinstalledAssets/*.cpio; the catalog's
compat-8 assets are later content, 39 languages, 4.6 MB). voice_configs.plist
loads, in order: vocalizer-user-dict.dat (application/edct-bin-dictionary, the
same format as iOS 6's userdct), vocalizer-user-rules.txt, domain rules (SMS,
music, address: Siri's, left out), vocalizer-override-rules.txt (both
application/x-vocalizer-rettt+text, the user_rules.txt syntax with \e/+...\e/+
phonetic spans).

- `ve_ttsResourceLoad(hInst, mime, size, data, &hRes)` (vx_load_resource):
  exported by iOS 5/6 (vauto_), 7 and 10 (ve_); iOS 3 at 0xBE438, iOS 4 at
  0xCA85C (stripped; same frame as iOS 5's: sub sp,#0xb0, data at [sp+0xc4]).
  All five engines take every file; the engine keeps using the data.
- The old phonetic switch `ESC/+ ... ESC/+` works on iOS 3-6, identical to
  `ESC\toi=lhp\`; iOS 4 ignores toi=lhp but takes ESC/+, so vx_phoneme_text
  now gives iOS 4 phonemes that way.
- Compiling the rule sets used to fill the 40 MB guest heap (fr-FR 44 MB,
  ja-JP > 168 MB high water) though only 3-5 MB stays live: realloc always
  moved, and small exact-size free lists never served other sizes. Now
  realloc stays put when it fits or grows the last block in place, and small
  requests take (and split) large free blocks: 0.7-5.6 MB for every language;
  40 corpus sentences byte-identical on iOS 3/4/5/6/7/10, fbstress clean.
- Loaded into the engine, the rule sets cost 20-137 ms of first-audio latency
  on every utterance (fast code, Mac: en-US 12 -> 33 ms, ja-JP 15 -> 137 ms;
  the rettt matcher isn't in the fast-code profiles) and 0.3-1.2 s per engine
  open; the dictionary alone costs nothing. So only the dictionary goes into
  the engine (vxs_set_rules_pack) and the rule files are applied to the text
  with the voice's own rules (cv_rules_for_voice_pack, PronunciationRules.swift,
  Rules.java), each compiled only when needed, behind a pre-check
  (core/vxrules.c: the literal every match must contain, found in the text
  with a two-byte index). No rule was skipped that matches, over all 211 rule
  files (39 packs x 2, every iOS release's, every Mac voice's) against PCRE2;
  15-66 of thousands run per phrase. Cost: 0.01-0.05 ms per phrase in C
  (worst 0.33), 0.035-0.086 Swift, ~0.05-0.065 Java; loading 3-90 ms once.
- Order: the engine runs the rule set loaded last first. iOS 12 loaded
  user-dict, user-rules, (domain rules), override-rules, so outside the
  engine the overrides go before the user rules ("Cinco de Mayo" proves it);
  then 46 of 46 test lines are byte-identical to loading them into the engine.
  The voice's own user_rules come first (VoiceServices ran them before the
  engine saw the text).
- Default rules are now each voice's own: an iOS voice its release's
  user_rules.txt only (no more iOS 6's for everyone), a Mac voice the
  user_rules.dat in its folder (113 of 154 have one), MacinTalk none
  (cv_rules_for_voice, PronunciationRules.swift, Rules.java).

## iOS 7 voices (iOS 7.0.6, 11B651) on their own engine (2026-10-31)

- Keys: `ios70/<lang>` (compact, bundled or imported) and `ios70e/<lang>`
  (enhanced, downloaded into `<downloads>/ios70/<lang>`), shown as "iOS 7"
  (7.0.6). `ios7` stays iOS 7.1.2, only the engine for the Mavericks compacts.
- 7.0.6's voiced is VoiceServices-225.1 (7.1.2's is 228, which added the
  Chinese DAG segmenter, maxent homographs, FE_PROSODY_WORD ...), so it is a
  build of its own in kBuilds: stubs 0x259978, heap/critsec/stream interfaces
  0x25B008/0x25B018/0x25B028 (found by the same function-pointer deltas as
  7.1.2's 0x281018 table), API by symbol (`_ve_tts*`). With the iOS 7.0
  voices both engines gave byte-identical audio for 20 corpus lines each in
  en-US, zh-CN, ja-JP and de-DE, but the voices run on their own release's
  engine anyway.
- Pitch: percent of normal like 7.1.2 (50 -> 90 Hz, 100 -> 181, 200 -> 361
  for Samantha; 25 is ignored), so vxstream's `percent` path covers `ios70`.
- Enhanced voices: format 5 (`voice_format_version.plist` in 7.0.6 says 5),
  36 languages on appldnld.apple.com/iOS7/031-3752.20140310.35GMf (7.1's
  posting; the catalog keeps one per format). They speak on the 7.0.6 engine
  and change pitch themselves (Samantha 178 -> 350 Hz at 200), unlike the
  iOS 5/6 enhanced voices, so they get no compact fallback (cv_catalog.c,
  VoiceCatalog.fixedPitch).
- Firmware: iPhone3,1_7.0.6_11B651_Restore.ipsw on secure-appldnld
  (1,201,645,522 bytes); the root filesystem is encrypted and its key is on
  The Apple Wiki. remote.import_from_apple("ios70") streams 1111 MB and
  imports in ~2.5 min (64 MB kept), byte-identical to the mounted IPSW's files.
- Fast code: tools/profiles/ios70.txt (corpus over en-US, en-GB, ja-JP and
  all 36 enhanced voices), build/gen70, `vx_native_ios70`. With only enhanced
  Samantha profiled, every other enhanced voice ran 19 M instructions per
  sentence in the interpreter (Tessa: 96-106 ms to first audio on an iPhone 17
  Pro Max against iOS 6 Tessa's 62-72); their unit-selection data takes paths
  Samantha's doesn't. Profiling each enhanced voice (13,254 -> 16,038 block
  starts) brought Tessa to 0.0 M interpreted, output bit-identical. The iOS
  5/6 enhanced voices were checked the same way and are covered.
