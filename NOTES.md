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
