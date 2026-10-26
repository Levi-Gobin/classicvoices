# Classic Voices for Android

The iOS 3–6 VoiceOver voices, their enhanced versions, and the Mac OS X Lion,
Mountain Lion and Mavericks voices as an Android text-to-speech engine, for
TalkBack and every app that speaks. Android 5.0 and later; ARM (64- and
32-bit) and x86 phones, tablets and emulators. The voices run on Apple's
original engines through the same bit-exact emulator as the iPhone app, so
the audio is byte-for-byte the same.

MacinTalk (Fred, Victoria, Alex …) comes from
[Panthera Speech](https://github.com/tgeczy/panthera-speech), a separate app.

## Getting voices

Nothing of Apple's ships in the app. The app is laid out like the iPhone,
Mac, Windows and Linux apps: an **iOS** tab, a **macOS** tab and
**Settings**. A tab goes from a version and a language down to each voice,
one page at a time under a Back button (Android's back works too). Every row
says what pressing it does first ("Download Samantha (enhanced), 123.2 MB");
a download shows a progress bar under its Stop button that moves by 1%, and
TalkBack reads it its own way.

1. **Get the firmware** (on a tab's first page): tap a firmware to stream
   just its root filesystem from Apple; the engine and voices are copied out
   (a few dozen MB stay on the phone). Use Wi-Fi. You can also import an
   IPSW already on the phone. Each version says which firmware it needs:
   Lion's voices need iOS 5 and 6, for example.
2. **Download voices**: the enhanced iOS 5 and 6 voices and the Mac OS X
   voices appear in their languages once their engine is imported; tap one
   to download it, tap again to stop.
3. **A voice's page**: hear it with your own text and speed, choose whether
   TalkBack and other apps see it ("Use as a system voice"), make it the
   voice for its language, or remove it.
4. Set Classic Voices as the preferred engine in Android's text-to-speech
   settings (Settings tab, **Open text-to-speech settings**).

Imports run in the background with a notification. Everything is kept in
device-protected storage, so the voices also speak on the lock screen after
a restart, before the phone is first unlocked.

## Fast engine code

The engine emulates the iPhone's processor. On a recent phone that is about
real time or a little faster; on older phones it can fall behind. Fast engine
code, recompiled from your own firmware into native code, is several times
faster and bit-identical.

The app builds it on the phone, by itself, after each firmware import (and
when it opens, for firmware imported before): **Speed up the voices** shows
it and its progress bar, and has a Stop button. It downloads the free
[Zig](https://ziglang.org) C compiler from ziglang.org (about 53 MB, checked
against the SHA-256 ziglang.org publishes) and deletes it when it's done; it
takes a few minutes per firmware, depending on the phone. `Recompiler.java` is tools/recomp.py in Java (it writes the same
files), and Android doesn't let an app start a program it downloaded, so Zig
runs through `libcvzig.so` (android/jni/cv_zigrun.c), a small program in the
app's lib folder that maps Zig into memory and starts it. 64-bit phones
(arm64-v8a, x86_64) only.

If a build fails or you stop it, it starts again only from that page. A
data pack still works too, and is the way for 32-bit phones:

```
classicvoices-data import all
classicvoices-data android-pack --ndk ~/android-ndk-r30
```

(`classicvoices-data` is the Linux tool, see [../linux/README.md](../linux/README.md);
it runs on a Mac too), copy `ClassicVoices-android-fast-code.zip` to the
phone, and import it with **Speed up the voices**, **Import a data pack (.zip)**.
Fast code is derived from Apple's code, so it never ships with the app: what
the phone builds stays on it.

## Building

```
android/build.sh                 # -> build/android/ClassicVoices.apk
ABIS=arm64-v8a DEBUG=1 android/build.sh   # one ABI, debuggable (adb run-as)
```

No Gradle: aapt2, javac, d8, zipalign and apksigner from the SDK, and the
NDK's clang. Paths: `ANDROID_HOME`, `ANDROID_NDK`, `JAVA_HOME`. Signed with
`~/.android/classicvoices-debug.keystore` (created on first build) unless
`KEYSTORE`/`KEYSTORE_PASS` say otherwise; every update must be signed with the
same key. The APK refuses to build with any Apple-derived file in it.

Test hooks (adb):

```
adb shell am start -S -n com.levigobin.classicvoices/.MainActivity --es import ios6,ios3
adb shell am start -S -n com.levigobin.classicvoices/.MainActivity --es download "mac:mac107:Serena"
adb shell am start -S -n com.levigobin.classicvoices/.MainActivity --es selftest "ios6/en-US,ios6e/en-GB"
adb logcat -s CVSELFTEST ClassicVoices
adb logcat -s CVSPEAK          # debuggable builds: every text the engine is asked to speak (TalkBack's wording)
```

## How it works

- `jni/cv_jni.c` wraps the shared C engine (`core/`), the voice catalogue and
  SSML parser (`ports/host/`) and the POSIX port layer (`ports/posix/`).
- `TtsService` is the `TextToSpeechService`: languages, the Android 5+ voice
  API, rate and pitch (Android's 100 = normal), Apple's pronunciation rules
  (`Rules.java`, java.util.regex) and the compact voice for pitch changes.
- The importer is a Java port of the NVDA add-on's: `Source` (files and HTTP
  ranges), `Firmware` (zip directory, streaming inflate, The Apple Wiki keys),
  `Encrcdsa` (AES-CBC chunks), `Udif` (zlib disk images), `Hfs` (HFS+/HFSX
  catalog, extents overflow, GPT and Apple partition maps, HFS+ compression),
  and `Downloads` (MobileAsset zips; xar → gzip → cpio → ROT13 tar packages),
  all streamed.
- 32-bit builds give the emulated iPhone a 512 MB address space and keep one
  engine open; the largest Mac premium voices need a 64-bit phone.
