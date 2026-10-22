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

Nothing of Apple's ships in the app. In the app:

1. **Voices from iPhone firmware**: tap a firmware to import it. Only its
   root filesystem is downloaded from Apple, inflated into a temporary file,
   and the engine and voices copied out (a few dozen MB stay on the phone).
   Use Wi-Fi: the sizes shown are what is downloaded. iOS 6 first.
   You can also pick an IPSW already on the phone.
2. **Download voices**: the enhanced iOS 5 and 6 voices and the Mac OS X
   voices, from Apple's servers, once their engine is imported.
3. **Voices and defaults**: pick each language's voice and hear it.
4. Set Classic Voices as the preferred engine in Android's text-to-speech
   settings (the app's first button opens them).

Imports run in the background with a notification. Everything is kept in
device-protected storage, so the voices also speak on the lock screen after
a restart, before the phone is first unlocked.

## Fast engine code

The engine emulates the iPhone's processor. On a recent phone that is about
real time or a little faster; on older phones it can fall behind. Fast engine
code, recompiled from your own firmware into native code, is several times
faster and bit-identical. It can't be built on the phone: build it on a
Linux or Mac computer with the Android NDK,

```
classicvoices-data import all
classicvoices-data android-pack --ndk ~/android-ndk-r30
```

(`classicvoices-data` is the Linux tool, see [../linux/README.md](../linux/README.md);
it runs on a Mac too), copy `ClassicVoices-android-fast-code.zip` to the
phone, and import it with **Import a data pack (.zip)**. It is derived from
Apple's code, so it never ships with the app: keep it for your own phone.

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
