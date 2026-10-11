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

You need a free GitHub account and a way to sideload apps (AltStore, SideStore
or Sideloadly; these work from Windows or a Mac). You don't need a Mac or Xcode.

1. Fork this repository (the Fork button at the top of the page).
2. In your fork, open "Settings › Secrets and variables › Actions" and add a
   repository secret named `IPA_PASSWORD` with a password of your choosing.
   The finished app is zipped with it, because anyone signed in to GitHub can
   download a public fork's build files.
3. Open the "Actions" tab. GitHub turns workflows off in new forks, so first
   press "I understand my workflows, go ahead and enable them". Then choose
   "Build Classic Voices" and press "Run workflow". The build takes roughly 30 to 60 minutes.
4. When it finishes, download "ClassicVoices" from the run's page, unzip it
   with your password, and sideload `ClassicVoices.ipa` with your sideloading
   tool.
5. Open Classic Voices once. The voices then appear in "Settings ›
   Accessibility › VoiceOver › Speech › Voice" and under "Spoken Content".

With a free Apple ID, sideloaded apps must be refreshed every 7 days (AltStore
and SideStore can do it automatically), and Classic Voices counts as two of
the three apps you may have installed at once (the app and its speech
extension).

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

### Mac OS X voices from your own copy

Apple doesn't host the older Mac OS X releases, so their built-in voices
(MacinTalk's Fred, Victoria, Alex and the rest, and each release's bundled
compact voices) come from installation media you already have. Classic Voices
was tested with these:

- Mac OS X 10.4.1 Tiger (8B1025). Later Tiger discs carry MacinTalk 3.4, which
  doesn't run here.
- Mac OS X 10.5 Leopard
- Mac OS X 10.6 Snow Leopard, retail disc 2Z691-6428-A
- OS X 10.7 Lion and OS X 10.8 Mountain Lion (Apple's InstallMacOSX downloads)
- OS X 10.9.5 Mavericks

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
