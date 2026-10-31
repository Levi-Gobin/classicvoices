# Classic Voices

Have you ever wished you could use Samantha from iOS 6, or take a trip back in time to hear the voices VoiceOver originally had? Have you ever wished you could have Mac exclusive voices that never made it to the iPhone, like Jill, Emily and more, on modern devices? Classic Voices allows you to do this and more.

They run on the original engines. Classic Voices emulates each engine, and
recompiles the busy parts of it for speed, so the voices sound exactly as they
did and respond as quickly as the built-in system voices you may be used to.

## Contents of this repo

The repo contains only the app and the tools to build it. No Apple software or voice data is
stored here. The engines and voices are downloaded when you build, or by the app after you install it from a release: iPhone firmware (IPSW files) and the voice downloads Apple still offers come from Apple's own servers, and the older Mac OS X install discs (Tiger, Leopard and Snow Leopard) come from archive.org.
The fast engine code (code which allows for quicker response times) is generated from that firmware on your own device or during your build, and is
never published.

## Available platforms

Classic Voices runs on iOS, iPadOS, macOS, visionOS (untested), Windows (both as a SAPI 5 voice and an NVDA add-on), Android, and Linux.

The iOS and Mac OS X voices (Samantha, Daniel, Karen, Jill, Emily and the rest) work on every platform. The old MacinTalk voices (Fred, Victoria, Vicki, Alex and the rest) are only in the iPhone, iPad, Mac and Vision Pro apps. On Windows, Android and Linux, [Panthera Speech](https://github.com/tgeczy/panthera-speech) is the way to get those.


## Installing
### From a release

Navigate to the [releases page](https://github.com/Levi-Gobin/classicvoices/releases) and download the file for your platform.
You can install on to the following platforms with a release:
#### Windows
- [ClassicVoicesSetup.exe](https://github.com/Levi-Gobin/classicvoices/releases/latest/download/ClassicVoicesSetup.exe): The Classic Voices app, where you download and manage the voices, plus the voices for SAPI 5 apps like JAWS, Balabolka and NVDA's SAPI 5 option, both 32 bit and 64 bit. The installer isn't code signed yet, so Windows may say the publisher is unknown. If it does, choose "More info", then "Run anyway".
- [classicvoices.nvda-addon](https://github.com/Levi-Gobin/classicvoices/releases/latest/download/classicvoices.nvda-addon): Used for NVDA 2023.1 or later, both 64 bit and 32 bit versions. Open it with NVDA running to install it. Then go to the NVDA menu, Tools, "Classic Voices data", and choose "Get firmware from Apple" to get the voices. If the Windows app is installed too, the add-on uses the app's voices. As of now I don't have a Windows computer, but it has been tested in a VM. If you have any issues with it please let me know.

#### Android

- [ClassicVoices.apk](https://github.com/Levi-Gobin/classicvoices/releases/latest/download/ClassicVoices.apk): For Android 5 or later. Works as a system TTS engine so it can be used with screen readers like TalkBack or BackTalk. You may need to allow your browser or files app to install apps. Then open Classic Voices, get the firmware, pick your voices, and choose Classic Voices as your text to speech engine. The fast engine code is only on 64 bit phones.

#### Linux
- [classicvoices-linux-x86_64.tar.gz](https://github.com/Levi-Gobin/classicvoices/releases/latest/download/classicvoices-linux-x86_64.tar.gz) (most PCs) and [classicvoices-linux-aarch64.tar.gz](https://github.com/Levi-Gobin/classicvoices/releases/latest/download/classicvoices-linux-aarch64.tar.gz) (64 bit ARM, like a Raspberry Pi 4 or 5): Acts as a Speech Dispatcher module and can be used with Orca. You need Speech Dispatcher 0.11 or later (Debian 12, Ubuntu 22.04, Fedora 36 or newer). Unpack it and run `classicvoices/install.sh`. It installs into your home folder, so you don't need root. Then open Classic Voices from your applications menu to get the voices (it needs wxPython: `sudo apt install python3-wxgtk4.0`), or run `classicvoices-data import ios6` in a terminal. After that, pick Classic Voices as Orca's speech synthesizer. More in [linux/README.md](linux/README.md).



### iOS/iPadOS
The iPhone, iPad, Mac and Vision Pro apps have Apple's code in them, so they're never in a release. You build your own on GitHub instead, and the scripts below do that for you.

For iOS, you need a free GitHub account and a way to sideload apps ([Sideloadly](https://sideloadly.io), [AltStore](https://altstore.io) or [SideStore](https://sidestore.io). These work from Windows or a Mac). You don't need a Mac
or Xcode.

### Building on GitHub

#### Windows
Paste the following into PowerShell:


```powershell
irm https://raw.githubusercontent.com/Levi-Gobin/classicvoices/main/classicvoices.ps1 | iex
```


#### macOS
Open Terminal and paste/run the following command:


```sh
bash <(curl -fsSL https://raw.githubusercontent.com/Levi-Gobin/classicvoices/main/classicvoices.sh)
```

It asks what to build. On Windows you can build the iPhone app and the NVDA add-on (with its voices already in it), or just one of them. On a Mac, you can build the iPhone app and the Mac app, or just one of them. On either, you can also build the Apple Vision Pro app (untested). It signs you in to GitHub in your
browser (the one-time code is copied for you), makes your own copy of this
repository, builds there, and reads out each step as it finishes. It takes about an hour and a quarter (another hour for the Apple Vision Pro app).

## When the build finishes

### Windows

#### NVDA

The voices go into NVDA's settings folder and NVDA offers to
  install the add-on. Choose Yes, restart NVDA, and pick Classic Voices as the
  synthesizer.
#### SAPI
The script doesn't build the SAPI voices. Install ClassicVoicesSetup.exe from the release instead, then navigate to the Classic Voices app in the Start menu, and you can download voices from there.


### macOS

 The app is installed in Applications and opened. If Classic Voices is
  already installed, the script asks whether to replace it now, save the new
  build in Downloads for later, or leave it: replacing an open copy stops its
  voices until the new one opens. Because it's built without an Apple developer
  account, macOS may ask whether to open "Classic Voices Synthesizer": choose
  Open Anyway.
### iOS/iPadOS
 `ClassicVoices.ipa` is saved in your Downloads folder and
  Sideloadly opens, if you have it, so you can install it.

### visionOS

`ClassicVoicesVision.ipa` is saved in Downloads.
  Install it with Xcode (Developer Mode on in the headset's settings) or a
  sideloading tool that supports Vision Pro, such as SideStore.


### By hand on GitHub

1. Fork this repository (the Fork button at the top of the page).
2. In your fork, open "Settings › Secrets and variables › Actions" and add a
   repository secret named `IPA_PASSWORD` with a password of your choosing.
   The finished app is zipped with it, because anyone signed in to GitHub can
   download a public fork's build files.
3. Open the "Actions" tab. GitHub turns workflows off in new forks, so first
   press "I understand my workflows, go ahead and enable them". Then choose
   "Build Classic Voices" and press "Run workflow". Leaving "targets" as it is
   builds the iPhone app, the Mac app and the NVDA add-on. Add `visionpro` to
   it if you want the Apple Vision Pro app too. The build takes about an hour
   and a quarter, and another hour with Apple Vision Pro.
4. When it finishes, download from the run's page and unzip with your
   password: "ClassicVoices" holds the iPhone app (sideload
   `ClassicVoices.ipa`), "ClassicVoicesVision" the Vision Pro app, "ClassicVoicesMac" the Mac app, "ClassicVoicesNVDA" is the add-on, and
   "ClassicVoicesNVDAData" holds its voices, which go in
   `%APPDATA%\nvda\classicvoices`.

### Using the voices
#### iOS/iPadOS/visionOS and macOS

Open the Classic Voices app once. On iOS and iPadOS, the voices then appear in "Settings > Accessibility > VoiceOver > Speech > Voice" and in "Spoken Content > Voices". On the Mac, pick them for VoiceOver in VoiceOver Utility under Speech, and for Spoken Content under "System voice". The enhanced voices, the Mac OS X voices and Alex are downloaded in the app first. If they don't speak,
open "Diagnostics" at the bottom of the app's first page and send the report.


### If you have a paid Apple developer account

With an Apple Developer Program membership, Classic Voices keeps working for a
year instead of a week, and the three-app limit doesn't apply:

1. Build `ClassicVoices.ipa` as above (the one-line script or the Actions
   run). Nothing changes in the build.
2. In Sideloadly (or AltStore or SideStore), sign in with the Apple ID that
   holds your developer membership, not a free one, and install the .ipa as
   usual.
3. The install now lasts one year, or until your membership ends if that's
   sooner. Nothing needs refreshing in between. Install a new build the same
   way whenever you update, and before the year is up.

If you build in Xcode with your own team instead, put your Team ID in
`DEVELOPMENT_TEAM` and your own prefix in `bundleIdPrefix` (ios/project.yml).
Then change the App Group `group.com.levigobin.ClassicVoices` to one of
yours: in ios/project.yml, ios/App/ClassicVoices.entitlements and
ios/Synth/Synth.entitlements. App Group names are unique across all of Apple's
developers, so you can't register that one.

## What the build includes

| From | Voices |
|---|---|
| iPhone OS 3.1.3 (7E18, iPhone 3GS) | The iOS 3 voices, as VoiceOver spoke when it first came to the iPhone |
| iOS 4.3.5 (8L1, iPhone 3GS) | The iOS 4 voices |
| iOS 5.1.1 (9B206, iPhone 3GS) | The iOS 5 voices, plus enhanced variants downloaded in the app |
| iOS 6.1.6 (10B500, iPhone 3GS) | The iOS 6 voices, and their enhanced voices (downloaded in the app) |
| iOS 7.0.6 (11B651, iPhone 4) | The iOS 7 voices, on iOS 7.0's own engine, and their enhanced voices (downloaded in the app) |
| iOS 7.1.2 (11D257, iPhone 4) | Only its engine, for the Mac OS X Mavericks voices |
| iOS 10.3.4 (14G61, iPhone 5) | Only its engine, for the Mavericks premium voices |

The app can also download the voices Mac OS X Lion, Mountain Lion and
Mavericks offered in System Preferences, from Apple's software update servers.

The voices speak the way they did then. Each voice gets the pronunciation rules
its own release had: the iOS 3 voices iOS 3's, the iOS 6 voices iOS 6's ("iOS"
is spelled out, "OS X" is "OS ten", and so on), and each Mac OS X voice its own.
"Newer pronunciation rules", off by default, adds Apple's later pronunciation
dictionary and rules to any voice, the iOS 3 and 4 voices included; turning it
on downloads them from Apple (about 5 MB for every language). When VoiceOver
changes the pitch, a voice that can't change pitch
itself (the iOS 5 and 6 enhanced voices, and the Lion and Mountain Lion premium
voices) hands that part to its compact version, as iOS did. The iOS 7 enhanced
voices and the Mavericks premium voices change pitch on their own and keep doing so. The rules and the compact voice are on for every voice. All three switches are in the app's Settings tab ("Apple's pronunciation rules", "Newer pronunciation rules" and "Compact voice for pitch changes") and on each voice's page. A search field at the top of each tab finds any voice in
the catalog.

### Mac OS X voices

The build also takes the MacinTalk voices (Fred, Victoria, Vicki and the rest,
not Alex) of four Mac OS X releases: Lion's from the Lion installer Apple
offers as a free download, and Tiger's, Leopard's and Snow Leopard's from the
install discs kept on archive.org (tools/fetch_old_macintalk.py reads only the
parts of each disc it needs, about a tenth). Their fast engine code is made in
the same run.

Alex, several hundred megabytes for each release, isn't in the build: each
Leopard, Snow Leopard and Lion Alex is a voice you download in the app's macOS tab, which
takes only his package from the same disc (or from the Lion installer on
Apple's servers), in the background like any other download.

Anything else from installation media you have can be added in the app's
macOS tab: choose "Add from Files" and pick the install disc's image
(.dmg or .iso; it's read in place, on iPhone too), the disc itself, a speech
data folder like panthera-speech's, or single .SpeechVoice voices. The page
says which files each release still needs. Classic Voices was tested with
these:

- Mac OS X 10.4.1 Tiger (8B1025). Later Tiger discs carry MacinTalk 3.4, which
  doesn't run here.
- Mac OS X 10.5 Leopard
- Mac OS X 10.6 Snow Leopard, retail disc 2Z691-6428-A
- OS X 10.7 Lion and OS X 10.8 Mountain Lion (Apple's InstallMacOSX downloads)
- OS X 10.9.5 Mavericks

## Windows, Android and Linux

On **Windows**, the Classic Voices app (in the Start menu, from
`ClassicVoicesSetup.exe`) gets and manages the voices the way the iPhone and
Mac apps do, and installs them as Windows speech (SAPI 5) voices for 64- and
32-bit programs: JAWS, NVDA's SAPI 5 option, Balabolka and the rest. NVDA
users can also use the add-on, which shares the app's voices. See
[desktop/README.md](desktop/README.md) and [windows/sapi/README.md](windows/sapi/README.md).

Classic Voices also runs as an **Android** text-to-speech engine (for
TalkBack and every app that speaks, Android 5.0 and later) and as a
**Speech Dispatcher** module on **Linux** (for Orca), where the same app
manages the voices. None of these needs a Mac: the apps (or
`classicvoices-data`) stream the firmware from Apple, copy out only the
engines and voices, and download the enhanced and Mac OS X voices the same
way. MacinTalk on these platforms is Panthera Speech's job. See
[android/README.md](android/README.md) and [linux/README.md](linux/README.md).

## Building (macOS)

```sh
python3 -m venv .venv && .venv/bin/pip install cryptography
brew install xcodegen
PYTHON=.venv/bin/python tools/fetch_firmware.sh   # firmware from Apple -> extracted/
tools/fetch_macintalk.sh                          # Lion's MacinTalk from Apple -> extracted/mt107
python3 tools/fetch_old_macintalk.py              # Tiger, Leopard and Snow Leopard from archive.org -> extracted/mt104-106
PYTHON=.venv/bin/python tools/build_engines.sh    # fast engine code -> build/gen*
PYTHON=.venv/bin/python tools/build_mt_native.sh  # MacinTalk fast code -> build/mtgen*
tools/package_ipa.sh                              # build/ClassicVoices.ipa
tools/install_mac.sh                              # the Mac app (needs your own signing team in ios/project.yml)
```

[NOTES.md](NOTES.md) describes how the engines are emulated.

## Credits

- MacinTalk support follows [panthera-speech](https://github.com/tgeczy/panthera-speech)
  by Tamas Geczy, which showed the way for running Apple's MacinTalk engines.
- AAC decoding uses [Glint](third_party/glint) (MIT).

Classic Voices isn't affiliated with or endorsed by Apple Inc. or Cerence.
Apple, iPhone, iOS, Mac, Mac OS X, macOS, VoiceOver and MacinTalk are
trademarks of Apple Inc. Vocalizer is a trademark of Cerence Operating Company.
