# Scan the data folders and list every voice the engine can speak with,
# as (key, display name, language) - key being vxstream's "<build>/<lang>"
# or "<era>/<VoiceFolder>" form.
import json
import os
import re

# "ios70" is iOS 7.0.6 (its own engine); "ios7" is 7.1.2, only an engine
# for the Mavericks compacts.
IOS_BUILDS = (("ios3", "iOS 3"), ("ios4", "iOS 4"), ("ios5", "iOS 5"), ("ios6", "iOS 6"), ("ios70", "iOS 7"))
ENHANCED_BUILDS = (("ios5", "iOS 5"), ("ios6", "iOS 6"), ("ios70", "iOS 7"))
MAC_ERAS = (("mac107", "Lion"), ("mac108", "Mountain Lion"), ("mac109", "Mavericks"))
# Either iOS 7 engine runs the Mavericks compacts: 7.1.2's when it was
# imported (the only one before 0.5.0), else 7.0.6's (vxstream.c open_engine).
IOS7 = ("ios7", "ios70")
# Engine firmware needed per Mac era (and for Mavericks, compact vs premium);
# a tuple is a choice of firmware.
MAC_ENGINES = {"mac107": ("ios6",), "mac108": ("ios6",), "mac109": (IOS7, "ios10")}
# iOS 7's Siri voices (SiriVoiceCatalog.json): Downloads/siri7/<Name>[Compact],
# spoken by 7.1.2's engine (core/vxstream.c open_engine; 7.0.6's runs those
# whose catalog entry doesn't say "needs": "ios7", where only it was imported).
SIRI = "siri7"
SIRI_ENGINE = "ios7"


def broker_voice_name(path):
    """The voice name from a broker.hdr / broker.hdr.asset file."""
    try:
        with open(path, "rb") as f:
            head = f.read(65536).decode("utf-8", "replace")
    except OSError:
        return None
    if head.startswith("defaultvoice:"):
        return head[13:head.find("\n")].strip() or None
    m = re.search(r"<voice>([^<]+)</voice>", head)
    return m.group(1).strip() if m else None


def _mac_catalog():
    path = os.path.join(os.path.dirname(__file__), "MacVoiceCatalog.json")
    try:
        with open(path, encoding="utf-8") as f:
            return json.load(f)["voices"]
    except (OSError, ValueError, KeyError):
        return []


def siri_catalog():
    path = os.path.join(os.path.dirname(__file__), "SiriVoiceCatalog.json")
    try:
        with open(path, encoding="utf-8") as f:
            return json.load(f)["voices"]
    except (OSError, ValueError, KeyError):
        return []


def siri_folder(entry):
    """A Siri catalog entry's folder (and key after "siri7/"): "Aaron", "AaronCompact"."""
    return entry["name"] + ("Compact" if entry["footprint"] == "compact" else "")


def siri_locale(downloads_root, folder):
    """A downloaded Siri voice's language: the locale file written beside it, else the catalog's."""
    try:
        with open(os.path.join(downloads_root, SIRI, folder, "locale"), encoding="utf-8") as f:
            tag = f.read().strip()
            if tag:
                return tag
    except OSError:
        pass
    return next((e["locale"] for e in siri_catalog() if siri_folder(e) == folder), "")


def _mac_locales():
    locales = {}
    for v in _mac_catalog():
        locales.setdefault((v["era"], v["name"].lower()), v.get("locale", ""))
    return locales


def _locale_file(voice_dir):
    """The language written beside a voice (Siri voices, the Mac compacts)."""
    try:
        with open(os.path.join(voice_dir, "locale"), encoding="utf-8") as f:
            return f.read().strip()
    except OSError:
        return ""


def has_engine(voices_root, build):
    """build: a firmware key, or a tuple of them of which any will do."""
    if isinstance(build, tuple):
        return any(has_engine(voices_root, b) for b in build)
    return os.path.isfile(os.path.join(voices_root, build, "voiced"))


class Voice:
    def __init__(self, key, name, language):
        self.key = key
        self.name = name
        self.language = language


def hidden_path(voices_root):
    """<data>/hidden.txt: voices taken out of the system's list (a key per line)."""
    return os.path.join(os.path.dirname(os.path.normpath(voices_root)), "hidden.txt")


def hidden_keys(voices_root):
    try:
        with open(hidden_path(voices_root), encoding="utf-8") as f:
            return {line.strip() for line in f if line.strip()}
    except OSError:
        return set()


def scan(voices_root, downloads_root, include_hidden=False):
    """-> [Voice], sorted for a sensible picker order; without the hidden ones
    unless include_hidden."""
    out = []
    for build, label in IOS_BUILDS:
        if not has_engine(voices_root, build):
            continue
        res = os.path.join(voices_root, build, "TTSResources")
        for lang in sorted(os.listdir(res)) if os.path.isdir(res) else []:
            if lang == "common":
                continue
            name = broker_voice_name(os.path.join(res, lang, "broker.hdr"))
            if name:
                out.append(Voice("%s/%s" % (build, lang), "%s (%s)" % (name, label), lang))
    for build, label in ENHANCED_BUILDS:
        if not has_engine(voices_root, build):
            continue
        dl = os.path.join(downloads_root, build)
        for lang in sorted(os.listdir(dl)) if os.path.isdir(dl) else []:
            name = broker_voice_name(os.path.join(dl, lang, "broker.hdr.asset"))
            if name:
                out.append(Voice("%se/%s" % (build, lang),
                                 "%s (%s, enhanced)" % (name, label), lang))
    locales = _mac_locales()
    for era, label in MAC_ERAS:
        if not os.path.isdir(os.path.join(voices_root, era, "common")):
            continue
        engines = MAC_ENGINES[era]
        dl = os.path.join(downloads_root, era)
        for folder in sorted(os.listdir(dl)) if os.path.isdir(dl) else []:
            if folder.startswith(".") or not os.path.isdir(os.path.join(dl, folder)):
                continue  # the compacts' marker, a download being unpacked
            compact = folder.endswith("Compact")
            base = folder[:-7] if compact else folder
            engine = engines[0] if (compact or era != "mac109") else engines[-1]
            if not has_engine(voices_root, engine):
                continue
            name = broker_voice_name(os.path.join(dl, folder, "broker.hdr")) or base
            lang = locales.get((era, base.lower()), "") or _locale_file(os.path.join(dl, folder))
            kind = "compact" if compact else "premium"
            out.append(Voice("%s/%s" % (era, folder),
                             "%s (Mac OS X %s, %s)" % (name, label, kind), lang))
    if has_engine(voices_root, IOS7):
        ios712 = has_engine(voices_root, SIRI_ENGINE)
        needs = {siri_folder(e) for e in siri_catalog() if e.get("needs")}
        dl = os.path.join(downloads_root, SIRI)
        for folder in sorted(os.listdir(dl)) if os.path.isdir(dl) else []:
            name = broker_voice_name(os.path.join(dl, folder, "broker.hdr"))
            if not name or (not ios712 and folder in needs):
                continue
            kind = "compact" if folder.endswith("Compact") else "enhanced"
            out.append(Voice("%s/%s" % (SIRI, folder), "%s (iOS 7 Siri, %s)" % (name, kind),
                             siri_locale(downloads_root, folder)))
    if not include_hidden:
        hidden = hidden_keys(voices_root)
        out = [v for v in out if v.key not in hidden]
    return out
