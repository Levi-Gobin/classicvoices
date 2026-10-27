# Classic Voices

The nostalgic VoiceOver voices such as Samantha from iOS 6, Karen and Daniel, the iOS 3 voices,
Lion's and Mountain Lion's premium voices, Fred and Alex from MacinTalk, and more. Use them with VoiceOver, Spoken Content, or any app that speaks.

They run on the original engines. Classic Voices emulates each engine, and
recompiles the busy parts of it for speed, so the voices sound exactly as they
did and respond as quickly as the built-in ones.

## Contents of this repo

Only the app and the tools to build it. No Apple software or voice data is
stored here. The engines and voices come from Apple's own servers when you
build: iPhone firmware (IPSW files) and the voice downloads Apple still offers.
The fast engine code is generated from that firmware during your build and is
never published.

## installing

You need a free GitHub account and a way to sideload apps (Sideloadly,
AltStore or SideStore; these work from Windows or a Mac). You don't need a Mac
or Xcode.

### The easy way: one line

On Windows, open PowerShell (Start menu, type PowerShell, press Enter) and
paste:

```powershell
irm https://raw.githubusercontent.com/Levi-Gobin/classicvoices/main/classicvoices.ps1 | iex
```

On a Mac, open Terminal and paste:

```sh
bash <(curl -fsSL https://raw.githubusercontent.com/Levi-Gobin/classicvoices/main/classicvoices.sh)
```

It asks what to build: on Windows, the iPhone app and the NVDA add-on; on a
Mac, the iPhone app and the Mac app; on either, also the Apple Vision Pro
app. Then it signs you in to GitHub in your
browser (the one-time code is copied for you), makes your own copy of this
repository, builds there, and reads out each step as it finishes. About half
an hour later:

- **NVDA**: the voices go into NVDA's settings folder and NVDA offers to
  install the add-on. Choose Yes, restart NVDA, and pick Classic Voices as the
  synthesizer.
- **Mac**: the app is installed in Applications and opened. If Classic Voices is
  already installed, the script asks whether to replace it now, save the new
  build in Downloads for later, or leave it: replacing an open copy stops its
  voices until the new one opens. Because it's built without an Apple developer
  account, macOS may ask whether to open "Classic Voices Synthesizer": choose
  Open Anyway.
- **iPhone**: `ClassicVoices.ipa` is saved in your Downloads folder and
  Sideloadly opens, if you have it, so you can install it.
- **Apple Vision Pro**: `ClassicVoicesVision.ipa` is saved in Downloads.
  Install it with Xcode (Developer Mode on in the headset's settings) or a
  sideloading tool that supports Vision Pro, such as SideStore. (There is no
  Apple TV app: tvOS doesn't let apps add speech voices.)

Run the same line again any time to update.

### By hand on GitHub

1. Fork this repository (the Fork button at the top of the page).
2. In your fork, open "Settings › Secrets and variables › Actions" and add a
   repository secret named `IPA_PASSWORD` with a password of your choosing.
   The finished app is zipped with it, because anyone signed in to GitHub can
   download a public fork's build files.
3. Open the "Actions" tab. GitHub turns workflows off in new forks, so first
   press "I understand my workflows, go ahead and enable them". Then choose
   "Build Classic Voices" and press "Run workflow". Leave "targets" as it is
   to build everything. The build takes about half an hour.
4. When it finishes, download from the run's page and unzip with your
   password: "ClassicVoices" holds the iPhone app (sideload
   `ClassicVoices.ipa`), "ClassicVoicesMac" the Mac app, "ClassicVoicesNVDA" is the add-on, and
   "ClassicVoicesNVDAData" holds its voices, which go in
   `%APPDATA%\nvda\classicvoices`.

### Then

Open Classic Voices once. The voices then appear in "Settings › Accessibility ›
VoiceOver › Speech › Voice" and under "Spoken Content". If they don't speak,
open "Diagnostics" at the bottom of the app's first page and send the report.

With a free Apple ID, sideloaded apps must be refreshed every 7 days (AltStore
and SideStore can do it automatically), and Classic Voices counts as two of
the three apps you may have installed at once (the app and its speech
extension).

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
| iOS 5.1.1 (9B206, iPhone 3GS) | The iOS 5 voices, plus enhanced varients downloaded in the app.|
| iOS 6.1.6 (10B500, iPhone 3GS) | The iOS 6 voices, and their enhanced voices (downloaded in the app) |
| iOS 7.1.2 (11D257, iPhone 4) | Only its engine, for the Mac OS X Mavericks voices |
| iOS 10.3.4 (14G61, iPhone 5) | Only its engine, for the Mavericks premium voices |

The app can also download the voices Mac OS X Lion, Mountain Lion and
Mavericks offered in System Preferences, from Apple's software update servers.

The voices speak the way they did then. Apple's pronunciation rules from iOS 5
and 6 are applied to the text ("iOS" is spelled out, "OS X" is "OS ten", and so
on), and when VoiceOver changes the pitch, a voice that can't change pitch
itself (the iOS 5 and 6 enhanced voices, and the Lion and Mountain Lion premium
voices) hands that part to its compact version, as iOS did. Mavericks premium
voices change pitch on their own and keep doing so. Both are on for every voice and can be turned off in Speech
Settings or per voice. A search field at the top of each tab finds any voice in
the catalog.

### Mac OS X voices

The build also takes the MacinTalk voices (Fred, Victoria, Bells and the rest,
not Alex) of four Mac OS X releases: Lion's from the Lion installer Apple
offers as a free download, and Tiger's, Leopard's and Snow Leopard's from the
install discs kept on archive.org (tools/fetch_old_macintalk.py reads only the
parts of each disc it needs, about a tenth). Their fast engine code is made in
the same run.

Alex, several hundred megabytes for each release, isn't in the build: each
Leopard, Snow Leopard and Lion Alex is a voice you download in the app, which
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
PYTHON=.venv/bin/python tools/build_engines.sh    # fast engine code -> build/gen*
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
