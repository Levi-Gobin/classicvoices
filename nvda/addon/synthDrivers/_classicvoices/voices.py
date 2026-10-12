# Scan the data folders and list every voice the engine can speak with,
# as (key, display name, language) - key being vxstream's "<build>/<lang>"
# or "<era>/<VoiceFolder>" form.
import json
import os
import re

IOS_BUILDS = (("ios3", "iOS 3"), ("ios4", "iOS 4"), ("ios5", "iOS 5"), ("ios6", "iOS 6"))
ENHANCED_BUILDS = (("ios5", "iOS 5"), ("ios6", "iOS 6"))
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


def scan(voices_root, downloads_root):
    """-> [Voice], sorted for a sensible picker order."""
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
    return out
