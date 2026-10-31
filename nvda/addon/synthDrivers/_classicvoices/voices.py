# Scan the data folders and list every voice the engine can speak with,
# as (key, display name, language) - key being vxstream's "<build>/<lang>"
# or "<era>/<VoiceFolder>" form.
import json
import os
import re

# "ios70" is iOS 7.0.6 (its own engine); "ios7" is 7.1.2, only the engine
# for the Mavericks compacts.
IOS_BUILDS = (("ios3", "iOS 3"), ("ios4", "iOS 4"), ("ios5", "iOS 5"), ("ios6", "iOS 6"), ("ios70", "iOS 7"))
ENHANCED_BUILDS = (("ios5", "iOS 5"), ("ios6", "iOS 6"), ("ios70", "iOS 7"))
MAC_ERAS = (("mac107", "Lion"), ("mac108", "Mountain Lion"), ("mac109", "Mavericks"))
# Engine firmware needed per Mac era (and for Mavericks, premium vs compact).
MAC_ENGINES = {"mac107": ("ios6",), "mac108": ("ios6",), "mac109": ("ios7", "ios10")}


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


def _mac_locales():
    locales = {}
    for v in _mac_catalog():
        locales.setdefault((v["era"], v["name"].lower()), v.get("locale", ""))
    return locales


def has_engine(voices_root, build):
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
            compact = folder.endswith("Compact")
            base = folder[:-7] if compact else folder
            engine = engines[0] if (compact or era != "mac109") else engines[-1]
            if not has_engine(voices_root, engine):
                continue
            name = broker_voice_name(os.path.join(dl, folder, "broker.hdr")) or base
            lang = locales.get((era, base.lower()), "")
            kind = "compact" if compact else "premium"
            out.append(Voice("%s/%s" % (era, folder),
                             "%s (Mac OS X %s, %s)" % (name, label, kind), lang))
    if not include_hidden:
        hidden = hidden_keys(voices_root)
        out = [v for v in out if v.key not in hidden]
    return out
