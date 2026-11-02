"""What the app shows: the voices of each version, what each needs, and the
work that gets them (firmware imports, downloads, fast code), one job at a
time on a worker thread."""
import json
import os
import shutil
import threading
import traceback

from . import crashlog, system
from _classicvoices import download as cv_download
from _classicvoices import firmware as cv_firmware
from _classicvoices import maccompacts as cv_compacts
from _classicvoices import pronunciation as cv_pron
from _classicvoices import remote as cv_remote
from _classicvoices import voices as cv_voices


class Version:
    def __init__(self, id, name, platform, firmware, number):
        self.id, self.name, self.platform, self.firmware, self.number = id, name, platform, firmware, number

    @property
    def spoken(self):
        return self.name


# The firmware each version needs: its own voices' (iOS), or the engine and
# common files the Mac voices run on (firmware.MAC_COMMON, voices.MAC_ENGINES).
# IOS7: either iOS 7 firmware; 7.0.6 is the one suggested, as it also has the
# iOS 7 voices (7.1.2 was the only one before 0.4.1, and still works).
IOS7 = cv_voices.IOS7
VERSIONS = [
    Version("ios3", "iOS 3", "ios", ["ios3"], "3.1.3"),
    Version("ios4", "iOS 4", "ios", ["ios4"], "4.3.5"),
    Version("ios5", "iOS 5", "ios", ["ios5"], "5.1.1"),
    Version("ios6", "iOS 6", "ios", ["ios6"], "6.1.6"),
    Version("ios70", "iOS 7", "ios", ["ios70"], "7.0.6"),
    Version("siri7", "iOS 7 Siri", "ios", ["ios7"], "7.1.2"),
    Version("mac107", "Lion", "mac", ["ios5", "ios6"], "10.7"),
    Version("mac108", "Mountain Lion", "mac", ["ios6", IOS7], "10.8"),
    Version("mac109", "Mavericks", "mac", [IOS7, "ios10"], "10.9"),
]
VERSION = {v.id: v for v in VERSIONS}

FIRMWARE_NAMES = {"ios3": "iPhone OS 3.1.3", "ios4": "iOS 4.3.5", "ios5": "iOS 5.1.1", "ios6": "iOS 6.1.6",
                  "ios70": "iOS 7.0.6", "ios7": "iOS 7.1.2", "ios10": "iOS 10.3.4"}


def suggested(f):
    """The firmware to get for a requirement (a key, or IOS7)."""
    return "ios70" if f == IOS7 else f


def firmware_name(f):
    return FIRMWARE_NAMES[suggested(f)]


def names_text(keys):
    return " and ".join(FIRMWARE_NAMES[k] for k in keys)

LANGUAGES = {
    "ar-SA": "Arabic (Saudi Arabia)", "cs-CZ": "Czech", "da-DK": "Danish", "de-DE": "German",
    "el-GR": "Greek", "en-AU": "English (Australia)", "en-GB": "English (United Kingdom)",
    "en-IE": "English (Ireland)", "en-IN": "English (India)", "en-US": "English (United States)",
    "en-ZA": "English (South Africa)", "en-scotland": "English (Scotland)", "es-AR": "Spanish (Argentina)",
    "es-CO": "Spanish (Colombia)", "es-ES": "Spanish (Spain)", "es-MX": "Spanish (Mexico)",
    "fi-FI": "Finnish", "fr-CA": "French (Canada)", "fr-FR": "French (France)", "he-IL": "Hebrew",
    "hi-IN": "Hindi", "hu-HU": "Hungarian", "id-ID": "Indonesian", "it-IT": "Italian", "ja-JP": "Japanese",
    "ko-KR": "Korean", "nl-BE": "Dutch (Belgium)", "nl-NL": "Dutch (Netherlands)", "no-NO": "Norwegian",
    "nb-NO": "Norwegian", "pl-PL": "Polish", "pt-BR": "Portuguese (Brazil)", "pt-PT": "Portuguese (Portugal)",
    "ro-RO": "Romanian", "ru-RU": "Russian", "sk-SK": "Slovak", "sv-SE": "Swedish", "th-TH": "Thai",
    "tr-TR": "Turkish", "zh-CN": "Chinese (China mainland)", "zh-HK": "Chinese (Hong Kong)",
    "zh-TW": "Chinese (Taiwan)",
}

SAMPLES = {
    "ar": "مرحبا، هذا صوتي.", "cs": "Dobrý den, toto je můj hlas.", "da": "Hej, dette er min stemme.",
    "de": "Hallo, das ist meine Stimme.", "el": "Γεια σας, αυτή είναι η φωνή μου.", "es": "Hola, esta es mi voz.",
    "fi": "Hei, tämä on minun ääneni.", "fr": "Bonjour, voici ma voix.", "he": "שלום, זה הקול שלי.",
    "hi": "नमस्ते, यह मेरी आवाज़ है।", "hu": "Helló, ez az én hangom.", "id": "Halo, ini suara saya.",
    "it": "Ciao, questa è la mia voce.", "ja": "こんにちは、これは私の声です。", "ko": "안녕하세요, 제 목소리입니다.",
    "nb": "Hei, dette er stemmen min.", "no": "Hei, dette er stemmen min.", "nl": "Hallo, dit is mijn stem.",
    "pl": "Cześć, to jest mój głos.", "pt": "Olá, esta é a minha voz.", "ro": "Bună, aceasta este vocea mea.",
    "ru": "Здравствуйте, это мой голос.", "sk": "Dobrý deň, toto je môj hlas.", "sv": "Hej, det här är min röst.",
    "th": "สวัสดี นี่คือเสียงของฉัน", "tr": "Merhaba, bu benim sesim.", "zh": "你好，这是我的声音。",
}


def language_name(code):
    if code in LANGUAGES:
        return LANGUAGES[code]
    # a bare language ("de", as language switching keeps choices): its name without a region
    for k, name in LANGUAGES.items():
        if k.split("-")[0] == code:
            return name.split(" (")[0]
    return code


def sample_text(code, name):
    lang = (code or "en").split("-")[0]
    return SAMPLES.get(lang) or "Hello, my name is %s." % name


def size_text(n):
    for unit, d in (("GB", 1e9), ("MB", 1e6), ("KB", 1e3)):
        if n >= d:
            return "%.1f %s" % (n / d, unit)
    return "%d bytes" % n


def folder_size(path):
    total = 0
    for root, _dirs, files in os.walk(path):
        for f in files:
            try:
                total += os.path.getsize(os.path.join(root, f))
            except OSError:
                pass
    return total


class Item:
    """One voice as the app shows it. state: "builtin" (from firmware),
    "installed", "available" (can be downloaded), "needs" (can be downloaded
    once the firmware in `needs` is imported), "downloading"."""

    def __init__(self, id, version, name, kind, language, state, size=0, entry=None, kind_of=None, needs=()):
        self.id, self.version, self.name, self.kind, self.language = id, version, name, kind, language
        self.state, self.size, self.entry, self.kind_of = state, size, entry, kind_of
        self.needs = list(needs)  # firmware keys to get
        self.job = None

    @property
    def title(self):
        return "%s (%s)" % (self.name, self.kind)

    @property
    def removable(self):
        return self.state == "installed"


class Cancelled(Exception):
    pass


class Job:
    def __init__(self, kind, key, label, size, run):
        self.kind, self.key, self.label, self.size, self.run = kind, key, label, size, run
        self.fraction = None
        self.message = label
        self.cancelled = False
        self.error = None
        self.notify = None  # called (on the worker thread) when the percent or the step changes

    @property
    def percent(self):
        return int(self.fraction * 100) if self.fraction is not None else None

    def progress(self, message, fraction):
        if self.cancelled:
            raise Cancelled()
        before = (self.message, self.percent)
        if message:
            self.message = message
        if fraction is not None:
            self.fraction = min(max(fraction, 0.0), 1.0)
        if self.notify and (self.message, self.percent) != before:
            self.notify()

    @property
    def done_bytes(self):
        return int((self.fraction or 0) * self.size)


class Library:
    def __init__(self, on_change, on_progress=None):
        self.on_change = on_change  # called on the worker thread; the UI marshals it
        self.on_progress = on_progress  # a job's percent changed (every 1%); also marshalled
        self.items = []
        self.jobs = []      # queued and running
        self.errors = []    # (label, message) of failed jobs, until dismissed
        self.finished = []  # labels of jobs that just finished, for announcing
        self._lock = threading.Lock()
        self._wake = threading.Condition(self._lock)
        threading.Thread(target=self._worker, daemon=True).start()
        self.refresh()
        self.get_compacts()  # firmware imported before the app got the Mac compacts
        self.start_fast_code()  # firmware imported before the app built fast code by itself
        if system.get_setting("NewerPronunciation"):
            self.get_pronunciation()  # turned on before, and the download didn't finish

    # ---- what's there
    def refresh(self):
        vr, dr = system.voices_root(), system.downloads_root()
        have = {v.key: v for v in cv_voices.scan(vr, dr, include_hidden=True)}
        items = []
        compact_names = {}
        for key, v in have.items():
            build, rest = key.split("/", 1)
            name = v.name.split(" (")[0]
            if build in ("ios3", "ios4", "ios5", "ios6", "ios70"):
                compact_names[(build, rest)] = name
                items.append(Item(key, VERSION[build], name, "compact", v.language, "builtin"))
            elif build in ("ios5e", "ios6e", "ios70e"):
                b = build[:-1]
                items.append(Item(key, VERSION[b], name, "enhanced", v.language, "installed",
                                  folder_size(os.path.join(dr, b, rest)), kind_of=(b, rest)))
            elif build == cv_voices.SIRI:  # siri7/<Voice>[Compact]
                items.append(Item(key, VERSION[build], name, "compact" if rest.endswith("Compact") else "enhanced",
                                  v.language, "installed", folder_size(os.path.join(dr, build, rest)),
                                  kind_of=(build, rest)))
            else:  # mac10x/<Voice>[Compact]
                compact = rest.endswith("Compact")
                items.append(Item(key, VERSION[build], name, "compact" if compact else "premium", v.language,
                                  "installed", folder_size(os.path.join(dr, build, rest)),
                                  kind_of=(build, rest[:-7] if compact else rest)))
        # voices whose firmware isn't here are listed too, saying what they need;
        # languages left out (Get the voices, Languages) aren't
        wanted = self.wanted_languages()
        for e in cv_download.enhanced_catalog():
            b, lang = e["build"], e["language"]
            if "%se/%s" % (b, lang) in have or not cv_firmware.language_wanted(lang, wanted):
                continue
            needs = [] if cv_voices.has_engine(vr, b) else [b]
            name = compact_names.get((b, lang)) or language_name(lang)
            items.append(Item("%se/%s" % (b, lang), VERSION[b], name, "enhanced", lang,
                              "needs" if needs else "available", e["size"], entry=("enhanced", e), needs=needs))
        for e in cv_download.mac_catalog():
            era = e["era"]
            if "%s/%s" % (era, e["name"]) in have or not cv_firmware.language_wanted(e.get("locale", ""), wanted):
                continue
            ready = self.firmware_ready(VERSION[era])
            needs = [] if ready else self.firmware_missing(VERSION[era])
            if not ready and not needs:
                continue  # firmware here but not its common files: an import will assemble them
            items.append(Item("%s/%s" % (era, e["name"]), VERSION[era], e["name"], "premium and compact",
                              e.get("locale", ""), "needs" if needs else "available", e["size"],
                              entry=("mac", e), needs=needs))
        # the compacts each Mac OS X release installed with the system: one download per release
        for era, _label in cv_voices.MAC_ERAS:
            left = cv_compacts.missing(era, dr, lambda loc: cv_firmware.language_wanted(loc, wanted))
            ready = self.compacts_ready(era)
            needs = [suggested(f) for f in cv_download.MAC_FIRMWARE[era] if not self.firmware_imported(f)]
            if not left or not ready and not needs:
                continue
            for folder in left:
                items.append(Item("%s/%s" % (era, folder), VERSION[era], folder[:-7], "compact",
                                  cv_compacts.locale(era, folder), "needs" if needs else "available",
                                  cv_compacts.download_size(era), entry=("compacts", {"era": era}), needs=needs))
        for e in cv_voices.siri_catalog():
            folder = cv_voices.siri_folder(e)
            key = "%s/%s" % (cv_voices.SIRI, folder)
            if key in have or not cv_firmware.language_wanted(e["locale"], wanted):
                continue
            needs = self.firmware_missing(VERSION[cv_voices.SIRI])
            items.append(Item(key, VERSION[cv_voices.SIRI], e["name"],
                              "compact" if e["footprint"] == "compact" else "enhanced", e["locale"],
                              "needs" if needs else "available", e["size"], entry=("siri", e), needs=needs))
        with self._lock:
            for it in items:
                it.job = next((j for j in self.jobs if j.key == self._job_key(it)), None)
                if it.job:
                    it.state = "downloading"
            self.items = items

    def firmware_imported(self, key):
        return cv_voices.has_engine(system.voices_root(), key)

    # ---- languages
    def wanted_languages(self):
        """What firmware imports take (firmware.wanted_languages); None: all."""
        return cv_firmware.wanted_languages(system.data_root())

    def set_language_wanted(self, lang, on):
        wanted = self.wanted_languages()
        wanted = set(cv_firmware.ALL_LANGUAGES) if wanted is None else set(wanted)
        (wanted.add if on else wanted.discard)(lang)
        cv_firmware.set_wanted_languages(system.data_root(),
                                         None if wanted >= set(cv_firmware.ALL_LANGUAGES) else wanted)
        self.refresh()
        self.on_change()

    def remove_language(self, lang):
        """Every voice of the language, and imports leave it out from now on."""
        cv_firmware.remove_language(system.voices_root(), system.downloads_root(), lang)
        self.refresh()
        self.on_change()

    def firmware_missing(self, version):
        """The firmware to get (suggested keys) before a version's voices speak."""
        return [suggested(f) for f in version.firmware if not self.firmware_imported(f)]

    def firmware_ready(self, version):
        if not all(self.firmware_imported(f) for f in version.firmware):
            return False
        if version.platform == "mac":
            return os.path.isdir(os.path.join(system.voices_root(), version.id, "common"))
        return True

    def compacts_ready(self, era):
        """A Mac OS X release's compacts can speak: their engine and common files are here."""
        return (all(self.firmware_imported(f) for f in cv_download.MAC_FIRMWARE[era])
                and os.path.isdir(os.path.join(system.voices_root(), era, "common")))

    def item(self, id):
        return next((i for i in self.items if i.id == id), None)

    def versions(self, platform):
        return [v for v in VERSIONS if v.platform == platform]

    def visible(self, platform, version_id=None, language=None):
        return [i for i in self.items if i.version.platform == platform
                and (not version_id or i.version.id == version_id)
                and (language is None or i.language == language)]

    def search(self, query):
        q = query.strip().lower()
        return [i for i in self.items if q in i.name.lower() or q in language_name(i.language).lower()
                or q in i.version.name.lower()] if q else []

    def is_hidden(self, item):
        return item.id in system.hidden_keys()

    def fast_code(self, build):
        return all(os.path.isfile(os.path.join(system.native_dir(), f)) for f in system.native_files(build))

    # ---- work
    def job_for(self, key):
        with self._lock:
            return next((j for j in self.jobs if j.key == key), None)

    def _queue(self, job):
        with self._lock:
            if any(j.key == job.key for j in self.jobs):
                return None
            job.notify = self.on_progress
            self.jobs.append(job)
            self._wake.notify()
        self.refresh()
        self.on_change()
        return job

    def cancel(self, key):
        with self._lock:
            for j in list(self.jobs):
                if j.key == key:
                    j.cancelled = True
                    if j is not self.jobs[0]:  # not started yet
                        self.jobs.remove(j)
        self.refresh()
        self.on_change()

    @staticmethod
    def _job_key(item):
        """A release's compacts come in one download, whichever of them is chosen."""
        if item.entry and item.entry[0] == "compacts":
            return "compacts:" + item.entry[1]["era"]
        return item.id

    def toggle_download(self, item):
        key = self._job_key(item)
        if self.job_for(key):
            self.cancel(key)
            return
        kind, entry = item.entry
        if kind == "compacts":
            self._queue_compacts(entry["era"])
            return
        dr = system.downloads_root()
        run = lambda job: cv_download.download(kind, entry, dr, job.progress)  # noqa: E731
        self._queue(Job("download", item.id, item.title, item.size, run))

    def _queue_compacts(self, era):
        label = "Mac OS X %s compact voices" % VERSION[era].name
        self._queue(Job("compacts", "compacts:" + era, label, cv_compacts.download_size(era),
                        lambda job: cv_download.download_compacts(era, system.downloads_root(), job.progress)))

    def get_compacts(self):
        """The compacts of every Mac OS X release whose firmware is here, as
        the Mac had them with the system; once (voices removed stay removed)."""
        dr = system.downloads_root()
        for era, _label in cv_voices.MAC_ERAS:
            if (self.compacts_ready(era) and not cv_compacts.fetched_before(era, dr)
                    and not self.job_for("compacts:" + era)):
                self._queue_compacts(era)

    def import_firmware(self, key):
        """Stream a firmware's speech files from Apple."""
        if self.job_for("fw:" + key):
            self.cancel("fw:" + key)
            return
        self._queue_firmware(key)

    def _queue_firmware(self, key):
        size = cv_remote.FIRMWARE[key][2]
        self._queue(Job("firmware", "fw:" + key, FIRMWARE_NAMES[key], size,
                        lambda job: cv_remote.import_from_apple(key, system.voices_root(), job.progress)))

    def get_with_firmware(self, item):
        """A voice that needs firmware: get the firmware from Apple, then the
        voice (the jobs run one after another)."""
        for key in item.needs:
            if not self.job_for("fw:" + key):
                self._queue_firmware(key)
        if not self.job_for(self._job_key(item)):
            item.state = "available"
            self.toggle_download(item)

    def get_update(self, version, asset, on_ready):
        """Download a new version's file (a job with its progress bar), then
        on_ready(path) on the worker thread."""
        from _classicvoices import update
        path = update.download_path(version, system.update_asset())

        def run(job):
            update.download(asset, path, lambda f: job.progress(None, f))
            on_ready(path)
        self._queue(Job("update", "update", "Classic Voices %s" % version, asset.get("size") or 0, run))

    def import_file(self, path):
        def run(job):
            if os.path.isdir(path):
                cv_firmware.import_extracted(path, system.voices_root(), job.progress)
            else:
                cv_firmware.import_ipsw(path, system.voices_root(), job.progress)
        self._queue(Job("firmware", "file:" + path, os.path.basename(path), 0, run))

    def fast_code_missing(self):
        return [f for f in FIRMWARE_NAMES if self.firmware_imported(f) and not self.fast_code(f)]

    def start_fast_code(self):
        """Builds fast code for imported firmware that has none, unless a build
        failed or was stopped (then the Speed up the voices page starts it)."""
        if system.recompiler() and system.get_setting("FastCodeAuto") and self.fast_code_missing():
            self.build_fast_code(None)

    def build_fast_code(self, builds):
        """builds=None: whatever has no fast code when the job starts."""
        def run(job):
            todo = builds if builds is not None else self.fast_code_missing()
            if not todo:
                return
            cc = system.find_compiler()
            if not cc:
                system.download_zig(job.progress)
                cc = system.find_compiler()
            for b in todo:
                system.build_fast_code(b, cc, job.progress)
        self._queue(Job("fast", "fast", "Fast engine code", 0, run))

    def get_pronunciation(self):
        """Apple's newer pronunciation packs (about 5 MB), unless they're here."""
        if cv_pron.have_packs(system.data_root()):
            return
        self._queue(Job("pronunciation", "pronunciation", "Newer pronunciation rules", 4700000,
                        lambda job: cv_pron.download_all(system.data_root(), job.progress)))

    def copy_from(self, src):
        """Copy another data folder's voices (the NVDA add-on's) into ours."""
        def run(job):
            for sub in ("Voices", "Downloads", "Native", "Pronunciation"):
                s = os.path.join(src, sub)
                if not os.path.isdir(s):
                    continue
                job.progress("Copying %s" % sub, None)
                shutil.copytree(s, os.path.join(system.data_root(), sub), dirs_exist_ok=True)
        self._queue(Job("copy", "copy", "Voices from the NVDA add-on", 0, run))

    def remove(self, item):
        """Delete a downloaded voice (a Mac voice's premium and compact go together)."""
        dr = system.downloads_root()
        if item.version.platform == "ios":
            b, lang = item.kind_of
            shutil.rmtree(os.path.join(dr, b, lang), ignore_errors=True)
        else:
            era, base = item.kind_of
            for folder in (base, base + "Compact"):
                shutil.rmtree(os.path.join(dr, era, folder), ignore_errors=True)
        self.refresh()
        self.on_change()

    def remove_premium(self, item):
        """A Mac OS X premium voice: delete it and keep its compact (the
        package brought both)."""
        era, base = item.kind_of
        shutil.rmtree(os.path.join(system.downloads_root(), era, base), ignore_errors=True)
        self.refresh()
        self.on_change()

    def dismiss_errors(self):
        self.errors = []
        self.on_change()

    def _worker(self):
        while True:
            with self._lock:
                while not self.jobs:
                    self._wake.wait()
                job = self.jobs[0]
            try:
                if not job.cancelled:
                    job.run(job)
            except Cancelled:
                pass
            except Exception as e:  # shown to the user, who can try again
                job.error = str(e) or e.__class__.__name__
                self.errors.append((job.label, job.error))
                crashlog.write("%s failed:\n%s" % (job.label, traceback.format_exc().rstrip()))
            with self._lock:
                self.jobs.remove(job)
            # Anything going wrong from here on must not end this thread: every
            # download after it would wait for ever (until the app was opened again).
            try:
                ok = not job.cancelled and not job.error
                if ok:
                    self.finished.append(job.label)
                if job.kind == "fast":
                    try:
                        system.set_setting("FastCodeAuto", ok)
                    except OSError:
                        pass
                elif ok and job.kind in ("firmware", "copy"):
                    self.get_compacts()
                    self.start_fast_code()
                self.refresh()
            except Exception as e:
                self.errors.append(("After %s" % job.label, str(e) or e.__class__.__name__))
                crashlog.write("After %s:\n%s" % (job.label, traceback.format_exc().rstrip()))
            self.on_change()


def diagnostics(lib):
    from _classicvoices import update
    lines = ["Classic Voices %s diagnostics" % update.VERSION, "Data folder: %s" % system.data_root()]
    for key, name in FIRMWARE_NAMES.items():
        state = "imported" if lib.firmware_imported(key) else "not imported"
        if lib.firmware_imported(key):
            state += ", fast code" if lib.fast_code(key) else ", no fast code"
        lines.append("  %s (%s): %s" % (name, key, state))
    lines.append("Voices: %d (%d hidden from the system)" % (
        sum(1 for i in lib.items if i.state in ("builtin", "installed")), len(system.hidden_keys())))
    lines.append("Voices the system can use: %d" % system.system_voice_count())
    lines.append("Settings: " + ", ".join("%s %s" % (k, "on" if system.get_setting(k) else "off")
                                          for k in system.SETTINGS))
    for label, err in lib.errors:
        lines.append("Error: %s: %s" % (label, err))
    lines.append(crashlog.report())
    return "\n".join(lines)


def save_json(path, obj):
    with open(path, "w", encoding="utf-8") as f:
        json.dump(obj, f, indent=1)
