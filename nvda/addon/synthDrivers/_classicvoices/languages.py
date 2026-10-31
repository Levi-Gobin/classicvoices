"""Which voice speaks text in another language (automatic language
switching). By default the voice nearest the one in use (core/vxlang.c);
people can choose a voice for a language for every voice, or for one voice.
The choices are kept in <data>/languages.txt, shared by the NVDA add-on, the
Windows and Linux app, SAPI and Speech Dispatcher (vxlang.h vx_lang_choose):

    # <for> <language> <voice key>
    * de ios6/de-DE
    ios70/en-US de ios70e/de-DE

<for> is "*" (whichever voice is in use) or a voice key; languages are bare
codes ("de"), so a choice covers de-DE and de-AT text alike."""
import os

from . import lib

FILE = "languages.txt"
generation = 0  # changes when this process saves choices (the voice's cache of picks follows)
HEADER = ("# Classic Voices: the voice for each language when the text changes language.\n"
          "# <for> <language> <voice key>; <for> is * (every voice) or a voice key.\n")


def _path(data_root):
    return os.path.join(data_root, FILE)


def base(language):
    return (language or "").replace("_", "-").split("-")[0].lower()


def text(data_root):
    """languages.txt as it is ("" when there are no choices)."""
    try:
        with open(_path(data_root), encoding="utf-8") as f:
            return f.read()
    except OSError:
        return ""


def load(data_root):
    """{for: {language: voice key}}, "*" for every voice."""
    out = {}
    for line in text(data_root).splitlines():
        f = line.split("#", 1)[0].split()
        if len(f) == 3:
            out.setdefault(f[0], {})[base(f[1])] = f[2]
    return out


def save(data_root, choices):
    os.makedirs(data_root, exist_ok=True)
    lines = [HEADER]
    for who in sorted(choices, key=lambda w: (w != "*", w)):
        for lang, key in sorted(choices[who].items()):
            lines.append("%s %s %s\n" % (who, lang, key))
    tmp = _path(data_root) + ".tmp"
    with open(tmp, "w", encoding="utf-8", newline="\n") as f:
        f.write("".join(lines))
    os.replace(tmp, _path(data_root))
    global generation
    generation += 1


def set_choice(data_root, language, voice_key, for_voice=None):
    """voice_key None: back to automatic. for_voice: only while that voice
    is the one in use."""
    c = load(data_root)
    table = c.setdefault(for_voice or "*", {})
    if voice_key:
        table[base(language)] = voice_key
    else:
        table.pop(base(language), None)
    if not table:
        c.pop(for_voice or "*", None)
    save(data_root, c)


def pick(choices_text, current_key, language, voices):
    """The key of the voice for `language` while current_key speaks.
    voices: [(key, language)] available now; a chosen voice that isn't there
    any more falls back to automatic."""
    return lib.lang_pick(current_key, language, voices, choices_text)
