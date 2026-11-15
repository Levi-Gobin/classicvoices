"""Pronunciation dictionaries (ports/host/cv_dict.h), in <data root>/Dictionaries:

- the community's, fetched from a GitHub repository (github.com/nicopn/classicvoices-dicts unless the person
  chose a fork; source()): the files a voice uses (en-us.dic and en.dic for an American English voice) are asked
  for in the background when it speaks, at most every 6 hours each, and used when the community switch is on. A
  file the repository no longer has is removed. Its phonemes/<language>.txt example lists come along (the phoneme
  picker's).
- the person's own, in Dictionaries/Mine: <file> holds the entries they added or changed (used even with the
  community switch off, before everything else), <file>.removed the fetched entries they took out (pattern TAB
  type). Fetching never touches Mine; tidy() drops what the repository has caught up with.

EDITING is set while the dictionary dialog is open: the voices then use none (the patterns are heard
uncorrected)."""
import json
import os
import threading
import time
import urllib.error
import urllib.request

DEFAULT_REPO, DEFAULT_BRANCH = "nicopn/classicvoices-dicts", "main"
EVERY = 6 * 3600
EDITING = False

_lock = threading.Lock()
_busy = set()
_checked = {}  # file name -> time asked (this session)


def folder(data_root):
    return os.path.join(data_root, "Dictionaries")


def mine_folder(data_root):
    return os.path.join(folder(data_root), "Mine")


def source(data_root):
    """(owner/repo, branch) the community dictionaries come from."""
    try:
        with open(os.path.join(folder(data_root), "source.json"), encoding="utf-8") as f:
            j = json.load(f)
        repo, branch = j.get("repo") or DEFAULT_REPO, j.get("branch") or DEFAULT_BRANCH
        if _valid_repo(repo):
            return repo, branch
    except (OSError, ValueError, AttributeError):
        pass
    return DEFAULT_REPO, DEFAULT_BRANCH


def _valid_repo(repo):
    parts = repo.split("/")
    return len(parts) == 2 and all(p and all(c.isalnum() or c in "-_." for c in p) for p in parts)


def parse_repo(text):
    """owner/repo from what the person typed ("owner/repo", a github.com URL, with .git or a trailing /), or
    None."""
    t = text.strip()
    for prefix in ("https://", "http://", "git@github.com:", "github.com/", "www.github.com/"):
        if t.lower().startswith(prefix):
            t = t[len(prefix):]
    if t.lower().startswith("github.com/"):
        t = t[len("github.com/"):]
    t = t.strip("/")
    if t.endswith(".git"):
        t = t[:-4]
    t = "/".join(t.split("/")[:2])
    return t if _valid_repo(t) else None


def set_source(data_root, repo, branch=DEFAULT_BRANCH):
    """Another repository (a fork): its files replace the fetched ones at the next refresh (now)."""
    d = folder(data_root)
    os.makedirs(d, exist_ok=True)
    with open(os.path.join(d, "source.json.part"), "w", encoding="utf-8") as f:
        json.dump({"repo": repo, "branch": branch or DEFAULT_BRANCH}, f)
    os.replace(os.path.join(d, "source.json.part"), os.path.join(d, "source.json"))
    try:
        os.remove(_state_path(d))  # nothing fetched from the old one counts
    except OSError:
        pass
    with _lock:
        _checked.clear()


def _raw(data_root):
    repo, branch = source(data_root)
    return "https://raw.githubusercontent.com/%s/%s/" % (repo, branch)


def _contents(data_root, path=""):
    repo, branch = source(data_root)
    return "https://api.github.com/repos/%s/contents/%s?ref=%s" % (repo, path, branch)


def names(voice_key, language):
    """The files a voice uses, narrowest first (as cv_dict_for_voice)."""
    if voice_key.startswith("mt"):
        return ["macintalk.dic"]
    lang = (language or "").replace("_", "-").lower()
    if not lang:
        return []
    out = [lang + ".dic"]
    if "-" in lang:
        out.append(lang.split("-")[0] + ".dic")
    return out


def _state_path(d):
    return os.path.join(d, "state.json")


class Changes:
    """What a refresh changed in the dictionaries (entries by pattern and type; phoneme lists don't count)."""

    def __init__(self):
        self.added = self.changed = self.removed = 0
        self.languages = set()

    def compare(self, old, new, name):
        def entries(text):
            out = {}
            for line in (text or "").splitlines():
                e = parse_line(line)
                if e:
                    out[e.key] = e.line()
            return out
        o, n = entries(old), entries(new)
        a = sum(1 for k in n if k not in o)
        r = sum(1 for k in o if k not in n)
        c = sum(1 for k in n if k in o and o[k] != n[k])
        if a + r + c:
            self.added += a
            self.removed += r
            self.changed += c
            self.languages.add(name[:-4].split("-")[0])

    def text(self):
        """"3 entries added, 1 changed and 2 removed across 2 languages" (what didn't happen left out), or None."""
        parts = []
        for n, verb in ((self.added, "added"), (self.changed, "changed"), (self.removed, "removed")):
            if n:
                parts.append("%d %s %s" % (n, "entry" if n == 1 else "entries", verb) if not parts else "%d %s" % (n, verb))
        if not parts:
            return None
        joined = parts[0] if len(parts) == 1 else ", ".join(parts[:-1]) + " and " + parts[-1]
        n = max(len(self.languages), 1)
        return "%s across %d %s" % (joined, n, "language" if n == 1 else "languages")


last_changes = Changes()  # what the last refresh_all changed


def _read_text(path):
    try:
        with open(path, encoding="utf-8-sig") as f:
            return f.read()
    except OSError:
        return None


def _fetch(d, name, ua, changes=None):
    path = os.path.join(d, *name.split("/"))
    try:
        with open(_state_path(d), encoding="utf-8") as f:
            state = json.load(f)
    except (OSError, ValueError):
        state = {}
    entry = state.get(name, {})
    if time.time() - entry.get("checked", 0) < EVERY:
        return
    headers = {"User-Agent": ua}
    if entry.get("etag") and os.path.exists(path):
        headers["If-None-Match"] = entry["etag"]
    try:
        base = _raw(os.path.dirname(d))
        with urllib.request.urlopen(urllib.request.Request(base + name, headers=headers), timeout=30) as r:
            data = r.read()
            if changes is not None and name.endswith(".dic"):
                changes.compare(_read_text(path), data.decode("utf-8-sig", "replace"), name)
            os.makedirs(os.path.dirname(path), exist_ok=True)
            tmp = path + ".part"
            with open(tmp, "wb") as f:
                f.write(data)
            os.replace(tmp, path)
            entry["etag"] = r.headers.get("ETag")
    except urllib.error.HTTPError as e:
        if e.code == 404:  # not (or no longer) in the repository
            try:
                os.remove(path)
            except OSError:
                pass
            entry.pop("etag", None)
        elif e.code != 304:
            return  # try again later
    except Exception:
        return
    entry["checked"] = time.time()
    state[name] = entry
    try:
        with open(_state_path(d) + ".part", "w", encoding="utf-8") as f:
            json.dump(state, f)
        os.replace(_state_path(d) + ".part", _state_path(d))
    except OSError:
        pass


def refresh(data_root, voice_key, language, ua="ClassicVoices"):
    """Ask for this voice's files in the background, if it's time."""
    d = folder(data_root)
    now = time.time()
    lang = (language or "").replace("_", "-").lower()
    wanted = names(voice_key, language) + (["phonemes/%s.txt" % lang] if lang else []) + \
        (["phonemes/%s.txt" % lang.split("-")[0]] if "-" in lang else [])
    with _lock:
        todo = [n for n in wanted
                if n not in _busy and now - _checked.get(n, 0) >= EVERY]
        for n in todo:
            _busy.add(n)
            _checked[n] = now
    if not todo:
        return

    def run():
        try:
            for n in todo:
                _fetch(d, n, ua)
            tidy(data_root)
        finally:
            with _lock:
                _busy.difference_update(todo)

    threading.Thread(target=run, daemon=True).start()



def _listing(data_root, path, ext, ua):
    """The names of the files with ext in a folder of the repository ([] when it has no such folder)."""
    req = urllib.request.Request(_contents(data_root, path),
                                 headers={"User-Agent": ua, "Accept": "application/vnd.github+json"})
    try:
        with urllib.request.urlopen(req, timeout=30) as r:
            items = json.load(r)
    except urllib.error.HTTPError as e:
        if e.code == 404 and path:
            return []
        raise
    return [i["name"] for i in items if isinstance(i, dict) and i.get("type", "file") == "file"
            and i.get("name", "").endswith(ext) and "/" not in i["name"] and not i["name"].startswith(".")]


def refresh_all(data_root, ua="ClassicVoices", force=False, done=None):
    """Every .dic file of the repository (the Classic Voices app: its voices'
    languages aren't known ahead), in the background, at most every 6 hours
    (force: now); files the repository no longer has are removed. done(ok),
    if given, is called at the end from the background thread."""
    d = folder(data_root)

    def run():
        global last_changes
        ok = False
        changes = Changes()
        try:
            os.makedirs(d, exist_ok=True)
            try:
                with open(_state_path(d), encoding="utf-8") as f:
                    state = json.load(f)
            except (OSError, ValueError):
                state = {}
            if not force and time.time() - state.get("_all", 0) < EVERY:
                ok = True
                return
            names = _listing(data_root, "", ".dic", ua) + ["phonemes/" + n for n in
                                                           _listing(data_root, "phonemes", ".txt", ua)]
            with _lock:
                for n in names:
                    _checked.pop(n, None)
            for n in names:
                entry = state.get(n, {})
                entry.pop("checked", None)  # asked for now
                state[n] = entry
            with open(_state_path(d) + ".part", "w", encoding="utf-8") as f:
                json.dump(state, f)
            os.replace(_state_path(d) + ".part", _state_path(d))
            for n in names:
                _fetch(d, n, ua, changes)
            for sub, ext in (("", ".dic"), ("phonemes", ".txt")):
                here = os.path.join(d, sub)
                for f in os.listdir(here) if os.path.isdir(here) else ():
                    if f.endswith(ext) and (sub + "/" + f if sub else f) not in names:
                        if ext == ".dic":
                            changes.compare(_read_text(os.path.join(here, f)), None, f)
                        try:
                            os.remove(os.path.join(here, f))
                        except OSError:
                            pass
            tidy(data_root)
            try:
                with open(_state_path(d), encoding="utf-8") as f:
                    state = json.load(f)
            except (OSError, ValueError):
                state = {}
            state["_all"] = time.time()
            with open(_state_path(d) + ".part", "w", encoding="utf-8") as f:
                json.dump(state, f)
            os.replace(_state_path(d) + ".part", _state_path(d))
            ok = True
        except Exception:
            ok = False
        finally:
            last_changes = changes
            if done:
                done(ok)

    threading.Thread(target=run, daemon=True).start()


# ---------------------------------------------------------------- entries (the dictionary dialog)

class Entry:
    """One line: pattern, replacement, case sensitive, type (0 anywhere, 1 regular expression, 2 whole word)."""
    __slots__ = ("pattern", "replacement", "case", "type")

    def __init__(self, pattern, replacement, case=False, type=0):
        self.pattern, self.replacement, self.case, self.type = pattern, replacement, bool(case), int(type)

    @property
    def key(self):
        return (self.pattern, self.type)

    def line(self):
        return "%s\t%s\t%d\t%d" % (self.pattern, self.replacement, 1 if self.case else 0, self.type)

    def same(self, other):
        return other is not None and self.line() == other.line()


def parse_line(line):
    """An Entry from a .dic line, or None (comments, blank lines, bad lines)."""
    line = line.rstrip("\r\n")
    if not line or line.startswith("#"):
        return None
    f = line.split("\t")
    if len(f) < 2 or not f[0]:
        return None
    case = len(f) > 2 and f[2] in ("1", "True")
    try:
        t = int(f[3]) if len(f) > 3 else 0
    except ValueError:
        t = 0
    return Entry(f[0], f[1], case, t)


def read_lines(path):
    try:
        with open(path, encoding="utf-8-sig") as f:
            return f.read().splitlines()
    except OSError:
        return []


def read_entries(path):
    return [e for e in (parse_line(l) for l in read_lines(path)) if e]


def _write(path, lines):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    if not lines:
        try:
            os.remove(path)
        except OSError:
            pass
        return
    with open(path + ".part", "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines) + "\n")
    os.replace(path + ".part", path)


def _removed_path(data_root, name):
    return os.path.join(mine_folder(data_root), name + ".removed")


def read_removed(data_root, name):
    out = set()
    for l in read_lines(_removed_path(data_root, name)):
        p, _, t = l.partition("\t")
        if p:
            try:
                out.add((p, int(t or 0)))
            except ValueError:
                pass
    return out


def _write_removed(data_root, name, keys):
    _write(_removed_path(data_root, name), ["%s\t%d" % k for k in sorted(keys)])


class Row:
    """An entry as the dialog lists it. origin: "community" (as fetched), "added" (the person's, not in the
    repository), "changed" (the person's, replacing a fetched one), "removed" (fetched, taken out)."""
    __slots__ = ("entry", "origin", "community")

    def __init__(self, entry, origin, community=None):
        self.entry, self.origin, self.community = entry, origin, community


def rows(data_root, name):
    """The file's entries for the dialog: the fetched file's in order (with the person's changes and removals
    shown on them), then the person's additions."""
    community = read_entries(os.path.join(folder(data_root), name))
    mine = read_entries(os.path.join(mine_folder(data_root), name))
    removed = read_removed(data_root, name)
    mine_by_key = {e.key: e for e in mine}
    out, shown = [], set()
    for c in community:
        if c.key in shown:
            continue
        shown.add(c.key)
        m = mine_by_key.get(c.key)
        if m is not None:
            out.append(Row(m, "changed", c))
        elif c.key in removed:
            out.append(Row(c, "removed", c))
        else:
            out.append(Row(c, "community", c))
    out += [Row(m, "added") for m in mine if m.key not in shown]
    return out


def _save_mine(data_root, name, entries):
    _write(os.path.join(mine_folder(data_root), name), [e.line() for e in entries])


def put(data_root, name, entry, old_key=None):
    """Adds an entry or changes one (old_key: the one it replaces, when the pattern or type changed). A changed
    fetched entry becomes the person's (the fetched one with an old pattern counts as removed)."""
    every = read_entries(os.path.join(mine_folder(data_root), name))
    # a changed entry keeps its place: entries apply in order, and a regular expression can depend on the one before
    at = next((i for i, e in enumerate(every) if e.key in (entry.key, old_key)), None)
    mine = [e for e in every if e.key != entry.key and e.key != old_key]
    community = {e.key: e for e in read_entries(os.path.join(folder(data_root), name))}
    removed = read_removed(data_root, name)
    if old_key and old_key != entry.key and old_key in community:
        removed.add(old_key)
    removed.discard(entry.key)
    if not entry.same(community.get(entry.key)):
        if at is None:
            mine.append(entry)
        else:
            mine.insert(at, entry)
    _save_mine(data_root, name, mine)
    _write_removed(data_root, name, removed)


def remove(data_root, name, key):
    """Takes an entry out: the person's goes; a fetched one is listed as removed."""
    mine = [e for e in read_entries(os.path.join(mine_folder(data_root), name)) if e.key != key]
    _save_mine(data_root, name, mine)
    if any(e.key == key for e in read_entries(os.path.join(folder(data_root), name))):
        removed = read_removed(data_root, name)
        removed.add(key)
        _write_removed(data_root, name, removed)


def restore(data_root, name, key):
    """Back to the fetched entry: the person's change or removal of it is dropped."""
    mine = [e for e in read_entries(os.path.join(mine_folder(data_root), name)) if e.key != key]
    _save_mine(data_root, name, mine)
    removed = read_removed(data_root, name)
    removed.discard(key)
    _write_removed(data_root, name, removed)


def tidy(data_root):
    """After a fetch: the person's entries the repository now has as they are, and removals of entries it no
    longer has, are dropped (what was published has arrived)."""
    m = mine_folder(data_root)
    if not os.path.isdir(m):
        return
    for f in os.listdir(m):
        if not f.endswith(".dic"):
            continue
        cpath = os.path.join(folder(data_root), f)
        if not os.path.exists(cpath):
            continue  # nothing fetched for it (yet): everything stays the person's
        community = {e.key: e for e in read_entries(cpath)}
        mine = read_entries(os.path.join(m, f))
        keep = [e for e in mine if not e.same(community.get(e.key))]
        if len(keep) != len(mine):
            _save_mine(data_root, f, keep)
        removed = read_removed(data_root, f)
        still = {k for k in removed if k in community}
        if still != removed:
            _write_removed(data_root, f, still)


def changes(data_root, name):
    """What the person changed in a file, against the fetched copy: (added, changed, removed), lists of
    Entry (removed: the fetched entries)."""
    community = {e.key: e for e in read_entries(os.path.join(folder(data_root), name))}
    added, changed = [], []
    for e in read_entries(os.path.join(mine_folder(data_root), name)):
        if e.key in community:
            if not e.same(community[e.key]):
                changed.append(e)
        else:
            added.append(e)
    removed = [community[k] for k in sorted(read_removed(data_root, name)) if k in community]
    return added, changed, removed


def apply_changes(lines, added, changed, removed):
    """A fetched file's lines with the person's changes made in them: changed lines in place, removed ones
    gone, added ones at the end; comments and order kept (what a commit writes)."""
    by_key = {e.key: e for e in changed}
    gone = {e.key for e in removed}
    out = []
    for l in lines:
        e = parse_line(l)
        if e is None:
            out.append(l)
        elif e.key in gone:
            continue
        elif e.key in by_key:
            out.append(by_key.pop(e.key).line())
        else:
            out.append(l)
    out += [e.line() for e in added] + [e.line() for e in by_key.values()]
    return out


def file_names(data_root):
    """Every dictionary file there is, fetched or the person's."""
    out = set()
    for d in (folder(data_root), mine_folder(data_root)):
        if os.path.isdir(d):
            out.update(f for f in os.listdir(d) if f.endswith(".dic"))
    return sorted(out)


# ---------------------------------------------------------------- phoneme examples (the phoneme picker)

def phoneme_examples(data_root, language):
    """[(X-SAMPA, example word)] for a language: phonemes/<lang>.txt, its lines replaced and added to by the
    narrower phonemes/<lang-region>.txt."""
    lang = (language or "").replace("_", "-").lower()
    files = []
    if "-" in lang:
        files.append(lang.split("-")[0])
    files.append(lang)
    order, by = [], {}
    for f in files:
        for l in read_lines(os.path.join(folder(data_root), "phonemes", f + ".txt")):
            if not l.strip() or l.startswith("#") or "\t" not in l:
                continue
            ph, _, ex = l.partition("\t")
            ph, ex = ph.strip(), ex.strip()
            if not ph:
                continue
            if ph not in by:
                order.append(ph)
            by[ph] = ex
    return [(p, by[p]) for p in order]


# ---------------------------------------------------------------- Convert to Phonemes (the dictionary editors)

def to_phonemes(replacement, regex, convert):
    """A replacement with its words as phonemes: each word becomes ~~X-SAMPA~~ as convert(word) gives it (None:
    the word stays). Phonemes already there are kept, and so, in a regular expression's replacement, are its
    groups (\\1, \\g<name>). Returns (new replacement, words converted, words left)."""
    import re
    out, i, done, left = [], 0, 0, 0
    s = replacement
    while i < len(s):
        if s.startswith("~~", i) and not s.startswith("~~~", i):
            m = re.compile(r"~~+").search(s, i + 2)  # the span's close: the first run of two or more tildes
            end = m.end() if m else len(s)
            out.append(s[i:end])
            i = end
            continue
        if regex and s[i] == "\\":
            m = re.match(r"\\(\d+|g<[^>]*>)", s[i:])
            n = len(m.group(0)) if m else min(2, len(s) - i)
            out.append(s[i:i + n])
            i += n
            continue
        m = re.match(r"[^\W\d_]+(?:'[^\W\d_]+)*", s[i:])
        if m:
            word = m.group(0)
            x = convert(word)
            if x:
                out.append("~~%s~~" % x)
                done += 1
            else:
                out.append(word)
                left += 1
            i += len(word)
            continue
        out.append(s[i])
        i += 1
    return "".join(out), done, left
