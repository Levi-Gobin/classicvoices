"""Which voice speaks text in another language (automatic language
switching). By default the voice nearest the one in use (core/vxlang.c);
people can choose a voice for a language for every voice, or for one voice.
The choices are kept in <data>/languages.json, shared by the NVDA add-on and
the Windows app:

    {"all": {"de": "ios6/de-DE"},
     "voices": {"ios70/en-US": {"de": "ios70e/de-DE"}}}

Languages are keyed by their bare code ("de"), so a choice covers de-DE and
de-AT text alike."""
import json
import os

from . import lib

FILE = "languages.json"


def _path(data_root):
    return os.path.join(data_root, FILE)


def load(data_root):
    try:
        with open(_path(data_root), encoding="utf-8") as f:
            d = json.load(f)
    except (OSError, ValueError):
        d = {}
    return {"all": dict(d.get("all") or {}), "voices": {k: dict(v) for k, v in (d.get("voices") or {}).items()}}


def save(data_root, choices):
    os.makedirs(data_root, exist_ok=True)
    tmp = _path(data_root) + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(choices, f, indent=1, sort_keys=True)
    os.replace(tmp, _path(data_root))


def base(language):
    return (language or "").replace("_", "-").split("-")[0].lower()


def set_choice(data_root, language, voice_key, for_voice=None):
    """voice_key None: back to automatic. for_voice: only while that voice
    is the one in use."""
    c = load(data_root)
    table = c["voices"].setdefault(for_voice, {}) if for_voice else c["all"]
    if voice_key:
        table[base(language)] = voice_key
    else:
        table.pop(base(language), None)
    if for_voice and not table:
        c["voices"].pop(for_voice, None)
    save(data_root, c)


def pick(choices, current_key, language, voices):
    """The key of the voice for `language` while current_key speaks.
    voices: [(key, language)] available now; a chosen voice that isn't
    there any more falls back to automatic."""
    auto = lib.lang_pick(current_key, language, voices)
    if auto is not None and auto == current_key:
        return auto  # the voice in use speaks it: a choice never replaces it
    have = {k for k, _ in voices}
    b = base(language)
    for table in (choices["voices"].get(current_key) or {}, choices["all"]):
        k = table.get(b)
        if k and k in have:
            return k
    return auto
