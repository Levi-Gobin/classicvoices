"""The pronunciation dictionary editor and publishing, as the NVDA add-on has them
(nvda/addon/globalPlugins/classicvoicesDicts.py), on the same shared code: the store
(_classicvoices/dicts.py: Dictionaries/Mine), publishing with git (gitsync.py) where git is
installed and may push to the repository, else through GitHub's API after signing in
(ghpublish.py). Settings > Edit the pronunciation dictionaries.

What differs from NVDA: the voice previews and the phoneme list use is chosen in the dialog
(any voice you have of the file's language), not NVDA's; the entries are a plain list (a list
box: native, so NVDA, JAWS and Orca read it), its context menu (Applications key) has the
entry's actions; and since the screen reader can't be told things directly, results come as
messages or as focus moving to what changed. While the dialog is open, the voices use no
dictionary at all (Dictionaries/editing, touched every 15 seconds: the system's voices, SAPI
and Speech Dispatcher, read it too), so the patterns are heard as the voices read them by
themselves; the edits are used from the moment it closes."""
import threading
import webbrowser

import wx

from . import system
from _classicvoices import dicts as cv_dicts  # noqa: E402 (system puts the library on the path)
from _classicvoices import ghpublish as cv_gh  # noqa: E402
from _classicvoices import gitsync as cv_git  # noqa: E402
from _classicvoices import voices as cv_voices  # noqa: E402

cv_gh.TOKEN_DIR = system.token_dir()

APP = "Classic Voices"
UA = "ClassicVoices"

# Sounds offered when the repository has no phonemes/<language>.txt yet (as the NVDA add-on): each one the voice
# can say, heard on its own (no example word).
GENERIC = ("i y I Y e 2 E 9 { a A Q O o U u V @ 3 6 1 } M 7 8 & "
           "a~ E~ O~ 9~ o~ "
           "p b t d k g q G\\ ? c J\\ "
           "m n N J F "
           "f v T D s z S Z x G h X R C j\\ s\\ z\\ "
           "tS dZ ts dz "
           "l L 5 r r\\ 4 R j w H").split()

TYPE_NAMES = {0: "Anywhere", 1: "Regular expression", 2: "Whole word"}
WHOSE = {"community": "Community", "added": "Yours", "changed": "Yours (changed)", "removed": "Removed by you"}


def _in_span(text, pos):
    """Whether pos is inside a ~~...~~ span of text."""
    return text[:pos].count("~~") % 2 == 1


def _alive(w):
    try:
        return bool(w)
    except RuntimeError:
        return False


class Voice:
    """The voice previews and the phoneme list use: (key, language, name), with the frame that plays it."""

    def __init__(self, frame, key, language, name):
        self.frame, self.key, self.language, self.name = frame, key, language, name
        self.phonetics = system.Phonetics(key, language)

    def say(self, text, entries=None, engine=False):
        self.frame.try_out(self.key, self.language, text, entries=entries, engine=engine)


def voices_for(name):
    """[(key, language, name)] of the voices you have that use a dictionary file (en-us.dic: American English
    voices; en.dic: every English voice; macintalk.dic: the MacinTalk voices)."""
    try:
        every = cv_voices.scan(system.voices_root(), system.downloads_root(), include_hidden=True)
    except Exception:
        return []
    return [(v.key, v.language, v.name) for v in every if name in cv_dicts.names(v.key, v.language)]


# ---------------------------------------------------------------- the voice's sounds


class PhonemeDialog(wx.Dialog):
    """The voice's sounds, each with an example word from phonemes/<language>.txt: Space hears the sound and
    the word, Enter inserts it."""

    def __init__(self, parent, voice, data):
        super().__init__(parent, title="Insert a phoneme")
        self.voice = voice
        self.items = self._items(data)
        main = wx.BoxSizer(wx.VERTICAL)
        main.Add(wx.StaticText(self, label=(
            "%s's sounds. Press Space to hear one and its example word, Enter to insert it." % voice.name
            if self.items else "%s's sounds couldn't be listed." % voice.name)), flag=wx.ALL, border=5)
        main.Add(wx.StaticText(self, label="&Sounds:"), flag=wx.LEFT | wx.TOP, border=5)
        self.list = wx.ListBox(self, choices=[self._label(x, e) for x, e, _t in self.items], size=(360, 300),
                               name="Sounds")
        if self.items:
            self.list.SetSelection(0)
        self.list.Bind(wx.EVT_KEY_DOWN, self.onKey)
        self.list.Bind(wx.EVT_LISTBOX_DCLICK, lambda evt: self.EndModal(wx.ID_OK))
        main.Add(self.list, proportion=1, flag=wx.EXPAND | wx.ALL, border=5)
        btns = wx.BoxSizer(wx.HORIZONTAL)
        hear = wx.Button(self, label="&Hear")
        hear.Bind(wx.EVT_BUTTON, lambda evt: self.hear())
        btns.Add(hear)
        ok = wx.Button(self, wx.ID_OK, label="&Insert")
        ok.SetDefault()
        btns.Add(ok)
        btns.Add(wx.Button(self, wx.ID_CANCEL))
        main.Add(btns, flag=wx.ALL, border=5)
        self.SetSizerAndFit(main)
        self.list.SetFocus()

    @staticmethod
    def _label(xsampa, example):
        return "{symbol}, as in {word}".format(symbol=xsampa, word=example) if example else xsampa

    def _items(self, data):
        """(X-SAMPA, example, engine text of the sound heard) for each sound the voice has, the language's
        list first, the same sound offered once."""
        listed = cv_dicts.phoneme_examples(data, self.voice.language) or [(g, "") for g in GENERIC]
        try:
            info = self.voice.phonetics.sounds([x for x, _e in listed])
        except Exception:
            return []
        out, seen = [], set()
        for (x, ex), (ipa, vowel, own, between) in zip(listed, info):
            if not ipa or not own or own in seen:
                continue  # the voice hasn't got it (or says it as one already in the list)
            seen.add(own)
            # a consonant is heard between two a's, the second stressed: alone, some voices say nothing
            out.append((x, ex, own if vowel else (between or own)))
        return out

    def hear(self):
        i = self.list.GetSelection()
        if i < 0 or i >= len(self.items):
            return
        _x, ex, heard = self.items[i]
        example = ex.replace("\x1b", "")
        self.voice.say(heard + (". " + example if example else ""), engine=True)

    def onKey(self, evt):
        if evt.GetKeyCode() == wx.WXK_SPACE:
            self.hear()
        else:
            evt.Skip()

    def chosen(self):
        i = self.list.GetSelection()
        return self.items[i][0] if 0 <= i < len(self.items) else None


# ---------------------------------------------------------------- one entry


class EntryDialog(wx.Dialog):
    """Adds or edits one entry: pattern, replacement (with phonemes), case, type; Preview says the test text with
    just this entry, with the entries before it, or with the whole file."""

    TYPES = (0, 2, 1)  # Anywhere, Whole word, Regular expression (the order NVDA's own dialog has)

    def __init__(self, parent, title, entry, voice, data, name, community):
        super().__init__(parent, title=title)
        self.voice, self.data, self.name, self.community = voice, data, name, community
        self.old_key = entry.key if entry else None
        main = wx.BoxSizer(wx.VERTICAL)
        main.Add(wx.StaticText(self, label="&Pattern:"), flag=wx.LEFT | wx.TOP, border=5)
        self.pattern = wx.TextCtrl(self, value=entry.pattern if entry else "", size=(420, -1), name="Pattern")
        main.Add(self.pattern, flag=wx.EXPAND | wx.ALL, border=5)
        main.Add(wx.StaticText(self, label="&Replacement (phonemes go between ~~ and ~~):"),
                 flag=wx.LEFT | wx.TOP, border=5)
        self.replacement = wx.TextCtrl(self, value=entry.replacement if entry else "", size=(420, -1),
                                       name="Replacement (phonemes go between ~~ and ~~)")
        main.Add(self.replacement, flag=wx.EXPAND | wx.ALL, border=5)
        row = wx.BoxSizer(wx.HORIZONTAL)
        phon = wx.Button(self, label="Insert p&honeme...")
        phon.Bind(wx.EVT_BUTTON, self.onPhoneme)
        phon.Enable(voice is not None)
        row.Add(phon)
        conv = wx.Button(self, label="Convert to phon&emes")
        conv.Bind(wx.EVT_BUTTON, self.onConvert)
        conv.Enable(voice is not None and not voice.key.startswith("mt"))
        row.Add(conv)
        main.Add(row, flag=wx.ALL, border=5)
        self.case = wx.CheckBox(self, label="Ca&se sensitive")
        self.case.SetValue(bool(entry and entry.case))
        main.Add(self.case, flag=wx.ALL, border=5)
        names = {0: "&Anywhere", 2: "&Whole word", 1: "Regular e&xpression"}
        self.kind = wx.RadioBox(self, label="Type", choices=[names[t] for t in self.TYPES])
        self.kind.SetSelection(self.TYPES.index(entry.type if entry else 2))
        main.Add(self.kind, flag=wx.ALL, border=5)
        main.Add(wx.StaticText(self, label="&Test text:"), flag=wx.LEFT | wx.TOP, border=5)
        self.test = wx.TextCtrl(self, value=entry.pattern if entry and entry.type != 1 else "", size=(420, -1),
                                name="Test text")
        main.Add(self.test, flag=wx.EXPAND | wx.ALL, border=5)
        # (Without the entry, the text is simply read: the voices use no dictionary while the dialog is open.)
        row = wx.BoxSizer(wx.HORIZONTAL)
        for label, scope in (("Pre&view", "alone"), ("Preview with ear&lier entries", "earlier"),
                             ("Preview with all e&ntries", "all")):
            b = wx.Button(self, label=label)
            b.Bind(wx.EVT_BUTTON, lambda evt, scope=scope: self.hear(scope))
            b.Enable(voice is not None)
            row.Add(b)
        main.Add(row, flag=wx.ALL, border=5)
        if voice is None:
            main.Add(wx.StaticText(self, label="None of your voices uses this file, so it can't be previewed."),
                     flag=wx.ALL, border=5)
        main.Add(self.CreateButtonSizer(wx.OK | wx.CANCEL), flag=wx.ALL, border=5)
        self.Bind(wx.EVT_BUTTON, self.onOk, id=wx.ID_OK)
        self.SetSizerAndFit(main)
        self.pattern.SetFocus()

    def entry(self):
        return cv_dicts.Entry(self.pattern.GetValue().replace("\t", " "), self.replacement.GetValue().replace("\t", " "),
                              self.case.GetValue(), self.TYPES[self.kind.GetSelection()])

    def hear(self, scope):
        text = self.test.GetValue() or self.pattern.GetValue()
        if self.voice is None or not text:
            return
        e = self.entry()
        entries = cv_dicts.preview_entries(self.data, self.name, e, self.old_key, self.community, scope) \
            if e.pattern else ""
        self.voice.say(text, entries=entries)

    def onPhoneme(self, evt):
        with wx.BusyCursor():
            d = PhonemeDialog(self, self.voice, self.data)
        if d.ShowModal() == wx.ID_OK and d.chosen():
            x = d.chosen()
            t, pos = self.replacement.GetValue(), self.replacement.GetInsertionPoint()
            if _in_span(t, pos):
                t, pos = t[:pos] + x + t[pos:], pos + len(x)
            elif t[:pos].endswith("~~") and _in_span(t, pos - 2):  # right after a span: into it
                t, pos = t[:pos - 2] + x + t[pos - 2:], pos - 2 + len(x)
            else:
                t, pos = t[:pos] + "~~" + x + "~~" + t[pos:], pos + 2 + len(x)
            self.replacement.SetValue(t)
            self.replacement.SetInsertionPoint(pos)
        d.Destroy()
        self.replacement.SetFocus()

    def onConvert(self, evt):
        """The replacement's words as phonemes, as the voice says them (no stress marks: add them where needed).
        The words go to the voice in one go (Linux asks classicvoices-say once)."""
        regex = self.TYPES[self.kind.GetSelection()] == 1
        words = []
        cv_dicts.to_phonemes(self.replacement.GetValue(), regex, lambda w: words.append(w))
        with wx.BusyCursor():
            try:
                said = dict(zip(words, self.voice.phonetics.words(words))) if words else {}
            except Exception:
                said = {}
        text, done, left = cv_dicts.to_phonemes(self.replacement.GetValue(), regex, lambda w: said.get(w))
        self.replacement.SetValue(text)
        # all converted: focus going back to the field reads its new text
        if done and left:
            wx.MessageBox("Converted {done} words; {left} stayed as they were.".format(done=done, left=left),
                          APP, wx.OK | wx.ICON_INFORMATION, self)
        elif not done:
            wx.MessageBox("Nothing to convert.", APP, wx.OK | wx.ICON_INFORMATION, self)
        self.replacement.SetFocus()

    def onOk(self, evt):
        e = self.entry()
        if not e.pattern:
            wx.MessageBox("Type the pattern to replace.", APP, wx.OK, self)
            self.pattern.SetFocus()
            return
        if e.pattern != self.pattern.GetValue():
            self.pattern.SetValue(e.pattern)
        if not e.replacement and wx.MessageBox(
                "The replacement is empty: the voices will skip \"{pattern}\" entirely. That suits things like "
                "rows of box-drawing characters, not words. Save it anyway?".format(pattern=e.pattern),
                APP, wx.YES_NO | wx.NO_DEFAULT | wx.ICON_WARNING, self) != wx.YES:
            return
        if e.replacement.count("~~") % 2 and not e.replacement.rstrip().endswith("~~~"):
            wx.MessageBox("A phoneme span isn't closed: end it with ~~.", APP, wx.OK, self)
            self.replacement.SetFocus()
            return
        evt.Skip()


# ---------------------------------------------------------------- where the dictionaries come from


class SourceDialog(wx.Dialog):
    """The GitHub repository the community dictionaries come from (a fork, say)."""

    def __init__(self, parent, data):
        super().__init__(parent, title="Dictionary source")
        repo, branch = cv_dicts.source(data)
        main = wx.BoxSizer(wx.VERTICAL)
        main.Add(wx.StaticText(self, label="GitHub &repository (owner/name or its address):"),
                 flag=wx.LEFT | wx.TOP, border=5)
        self.repo = wx.TextCtrl(self, value=repo, size=(360, -1), name="GitHub repository (owner/name or its address)")
        main.Add(self.repo, flag=wx.EXPAND | wx.ALL, border=5)
        main.Add(wx.StaticText(self, label="&Branch:"), flag=wx.LEFT | wx.TOP, border=5)
        self.branch = wx.TextCtrl(self, value=branch, size=(360, -1), name="Branch")
        main.Add(self.branch, flag=wx.EXPAND | wx.ALL, border=5)
        row = wx.BoxSizer(wx.HORIZONTAL)
        default = wx.Button(self, label="&Default")
        default.Bind(wx.EVT_BUTTON, lambda evt: (self.repo.SetValue(cv_dicts.DEFAULT_REPO),
                                                 self.branch.SetValue(cv_dicts.DEFAULT_BRANCH)))
        row.Add(default)
        main.Add(row, flag=wx.ALL, border=5)
        main.Add(self.CreateButtonSizer(wx.OK | wx.CANCEL), flag=wx.ALL, border=5)
        self.Bind(wx.EVT_BUTTON, self.onOk, id=wx.ID_OK)
        self.SetSizerAndFit(main)
        self.repo.SetFocus()

    def onOk(self, evt):
        self.chosen = cv_dicts.parse_repo(self.repo.GetValue())
        if not self.chosen:
            wx.MessageBox("Type the repository as owner/name, for example nicopn/classicvoices-dicts.", APP,
                          wx.OK, self)
            self.repo.SetFocus()
            return
        self.chosen_branch = self.branch.GetValue().strip() or cv_dicts.DEFAULT_BRANCH
        evt.Skip()


# ---------------------------------------------------------------- publishing


def _window(w, frame):
    """w if it's still open, else the app's window (the dictionaries dialog may have closed meanwhile)."""
    return w if w is not None and _alive(w) else frame


def _box(parent, frame, text, title, style=wx.OK | wx.ICON_INFORMATION):
    wx.MessageBox(text, title, style, _window(parent, frame))


def _ask(parent, frame, text, title, yes, no):
    """A question with two buttons named yes and no; whether yes was chosen."""
    d = wx.MessageDialog(_window(parent, frame), text, title, wx.YES_NO | wx.ICON_QUESTION)
    d.SetYesNoLabels(yes, no)
    try:
        return d.ShowModal() == wx.ID_YES
    finally:
        d.Destroy()


def summary(todo):
    """ "3 entries added, 1 changed and 1 removed in en.dic" for {file: (added, changed, removed)}."""
    counts = [sum(len(v[i]) for v in todo.values()) for i in range(3)]
    parts = []
    for n, verb in zip(counts, ("added", "changed", "removed")):
        if not n:
            continue
        if not parts:
            parts.append(("{n} entry {verb}" if n == 1 else "{n} entries {verb}").format(n=n, verb=verb))
        else:
            parts.append("{n} {verb}".format(n=n, verb=verb))
    text = parts[0] if len(parts) == 1 else "{list} and {last}".format(list=", ".join(parts[:-1]), last=parts[-1])
    if len(todo) == 1:
        return "{changes} in {file}".format(changes=text, file=next(iter(todo)))
    return "{changes} across {n} files".format(changes=text, n=len(todo))


# Whether git is here and may push to the chosen repository: {source repo: True/False}, checked in the background
# when the dialog opens (a push that sends nothing) and before publishing. Git is used when it can push; otherwise
# GitHub's API, after signing in (unlike NVDA, which uses git whenever it's installed: on Linux git nearly always is,
# but rarely with the right to push).
_git_ok = {}
_git_lock = threading.Lock()


def git_can_push(data, cached=False):
    """True/False; with cached, None when it hasn't been checked yet (never waits)."""
    source = cv_dicts.source(data)[0]
    if source in _git_ok or cached:
        return _git_ok.get(source)
    with _git_lock:
        if source not in _git_ok:
            try:
                ok = cv_git.find_git() is not None and cv_git.can_push(data)
            except Exception:
                ok = False
            _git_ok[source] = ok
    return _git_ok[source]


def check_git_in_background(data):
    if cv_git.find_git() is None:
        _git_ok[cv_dicts.source(data)[0]] = False
        return
    threading.Thread(target=git_can_push, args=(data,), name="git check", daemon=True).start()


class SignInDialog(wx.Dialog):
    """GitHub's device sign-in: the code to type at github.com/login/device; closes by itself once the person
    has signed in there."""

    def __init__(self, parent, data):
        super().__init__(parent, title="Sign in to GitHub")
        self.data = data
        self.code = None
        self.who = None
        self.stop = threading.Event()  # set when the dialog goes: the waiting thread gives up
        main = wx.BoxSizer(wx.VERTICAL)
        main.Add(wx.StaticText(self, label="What to do:"), flag=wx.LEFT | wx.TOP, border=5)
        self.text = wx.TextCtrl(self, value="Asking GitHub for a sign-in code...",
                                style=wx.TE_MULTILINE | wx.TE_READONLY, size=(480, 100), name="What to do")
        main.Add(self.text, flag=wx.EXPAND | wx.ALL, border=5)
        main.Add(wx.StaticText(self, label="Your c&ode:"), flag=wx.LEFT | wx.TOP, border=5)
        # its own field, so it can be read letter by letter
        self.codeField = wx.TextCtrl(self, value="", style=wx.TE_READONLY, size=(200, -1), name="Your code")
        main.Add(self.codeField, flag=wx.ALL, border=5)
        btns = wx.BoxSizer(wx.HORIZONTAL)
        self.copyBtn = wx.Button(self, label="&Copy code and open GitHub")
        self.copyBtn.Bind(wx.EVT_BUTTON, self.onCopy)
        self.copyBtn.Disable()
        btns.Add(self.copyBtn)
        cancel = wx.Button(self, wx.ID_CANCEL, label="Cancel")
        cancel.Bind(wx.EVT_BUTTON, self.onCancel)
        btns.Add(cancel)
        self.SetEscapeId(wx.ID_CANCEL)
        main.Add(btns, flag=wx.ALL, border=5)
        self.SetSizerAndFit(main)
        self.text.SetFocus()
        threading.Thread(target=self._work, name="GitHub sign-in", daemon=True).start()

    def _work(self):
        try:
            code = cv_gh.start_sign_in()
            wx.CallAfter(self._show_code, code)
            who = cv_gh.finish_sign_in(self.data, code, cancelled=self.stop.is_set)
            if who is not None:
                wx.CallAfter(self._done, who)
        except Exception as e:
            wx.CallAfter(self._failed, getattr(e, "message", None) or str(e) or e.__class__.__name__)

    def _here(self):
        return not self.stop.is_set() and _alive(self)

    def _show_code(self, code):
        if not self._here():
            return
        self.code = code
        self.text.SetValue("Go to {url} and enter the code {code}. The Copy code and open GitHub button copies "
                           "the code and opens that page. This window closes by itself once you've signed in."
                           .format(url=code["verification_uri"], code=code["user_code"]))
        self.codeField.SetValue(code["user_code"])
        self.copyBtn.Enable()
        # the field with the code: the screen reader reads it now, and it can be read letter by letter
        self.codeField.SetFocus()

    def onCopy(self, evt):
        if self.code:
            if wx.TheClipboard.Open():
                wx.TheClipboard.SetData(wx.TextDataObject(self.code["user_code"]))
                wx.TheClipboard.Flush()
                wx.TheClipboard.Close()
            webbrowser.open(self.code["verification_uri"])

    def _done(self, who):
        if not self._here():
            return
        self.who = who
        self.EndModal(wx.ID_OK)

    def _failed(self, why):
        if not self._here():
            return
        self.text.SetValue("Couldn't sign in: {why}".format(why=why))
        self.copyBtn.Disable()
        self.text.SetFocus()

    def onCancel(self, evt):
        self.stop.set()
        self.EndModal(wx.ID_CANCEL)


def sign_in(parent, frame, data):
    """The sign-in dialog; the account signed in to, or None."""
    d = SignInDialog(_window(parent, frame), data)
    try:
        return d.who if d.ShowModal() == wx.ID_OK else None
    finally:
        d.stop.set()
        d.Destroy()


class AskSignInDialog(wx.Dialog):
    """On closing the dictionaries with changes nothing here can publish: sign in to GitHub?"""

    DONT = wx.NewIdRef()

    def __init__(self, parent):
        super().__init__(parent, title="Publish your changes?")
        main = wx.BoxSizer(wx.VERTICAL)
        main.Add(wx.StaticText(self, label="Sign in to GitHub to publish your changes?"), flag=wx.ALL, border=10)
        btns = wx.BoxSizer(wx.HORIZONTAL)
        for i, label in ((wx.ID_YES, "&Yes"), (wx.ID_NO, "&No"), (self.DONT, "&Don't show again")):
            b = wx.Button(self, i, label=label)
            b.Bind(wx.EVT_BUTTON, lambda evt, i=i: self.EndModal(i))
            btns.Add(b)
        self.SetEscapeId(wx.ID_NO)
        main.Add(btns, flag=wx.ALL, border=5)
        self.SetSizerAndFit(main)
        self.FindWindowById(wx.ID_YES, self).SetFocus()


class Publisher:
    """Publish now: git where it can push, else GitHub's API (signing in first when needed; offering the person's
    own copy when they can't write to the repository), then a message saying how it went. after() runs once it's
    over (the dialog refreshes); busy(on) while it runs (the button says Publishing...)."""

    def __init__(self, frame, data):
        self.frame, self.data = frame, data
        self.running = False

    def publish(self, parent, after=None, busy=None):
        data, frame = self.data, self.frame
        todo = cv_git.pending(data)
        if not todo:
            _box(parent, frame, "You have no changes of your own that aren't published yet.", "Nothing to Publish")
            return
        if self.running:
            return
        self.running = True
        if busy:
            busy(True)

        def run():
            git = git_can_push(data)
            wx.CallAfter(self._go, parent, todo, git, after, busy)
        threading.Thread(target=run, name="publish", daemon=True).start()

    def _go(self, parent, todo, git, after, busy):
        data = self.data
        if not git:
            if not cv_gh.configured():
                self._end(busy)
                _box(parent, self.frame, "Publishing needs git, and publishing through GitHub isn't set up in this "
                     "build yet. Your changes stay on this computer.", "Couldn't Publish", wx.OK | wx.ICON_WARNING)
                return
            if not cv_gh.signed_in(data):
                self._end(busy)
                if sign_in(parent, self.frame, data):
                    if after:
                        after()
                    self.publish(parent, after, busy)
                return

        def run():
            kind, n, err, target, url = "done", 0, None, None, None
            try:
                if git:
                    n, err = cv_git.publish(data, force=True)
                    target = cv_dicts.source(data)[0]
                else:
                    n, err, target = cv_gh.publish(data, force=True)
            except cv_gh.NotSignedIn:
                kind = "signin"
            except cv_gh.NeedsFork:
                kind = "fork"
            except cv_gh.NeedsInstall as e:
                kind, url = "install", e.url
            except Exception as e:
                n, err = 0, str(e) or e.__class__.__name__
            wx.CallAfter(self._published, parent, todo, kind, n, err, target, url, after, busy)
        threading.Thread(target=run, name="publish", daemon=True).start()

    def _end(self, busy):
        self.running = False
        if busy:
            busy(False)

    def _published(self, parent, todo, kind, n, err, target, url, after, busy):
        self._end(busy)
        if after:
            after()
        data, frame = self.data, self.frame
        source = cv_dicts.source(data)[0]
        if kind == "signin":
            if sign_in(parent, frame, data):
                self.publish(parent, after, busy)
        elif kind == "fork":
            if _ask(parent, frame, "You can't publish to {repo}. Classic Voices can make your own copy of it (a fork) "
                    "on GitHub and publish your changes there. To offer them for everyone, open a pull request from "
                    "your copy on GitHub.".format(repo=source),
                    "Publish to your own copy?", "&Make my copy", "Cancel"):
                if busy:
                    busy(True)

                def make():
                    try:
                        cv_gh.make_fork(data)
                        wx.CallAfter(self._forked, parent, after, busy, None, None)
                    except cv_gh.NeedsInstall as e:
                        wx.CallAfter(self._forked, parent, after, busy, e.url, None)
                    except Exception as e:
                        wx.CallAfter(self._forked, parent, after, busy, None, getattr(e, "message", None) or str(e))
                threading.Thread(target=make, name="fork", daemon=True).start()
        elif kind == "install":
            self._install(parent, url)
        elif err:
            _box(parent, frame, err + "\n\nYour changes stay on this computer.", "Couldn't Publish",
                 wx.OK | wx.ICON_WARNING)
        elif not n:
            if target and target != source:
                _box(parent, frame, "Your copy, {repo}, has all your changes already.".format(repo=target),
                     "Nothing to Publish")
            else:
                _box(parent, frame, "You have no changes of your own that aren't published yet.", "Nothing to Publish")
        else:
            text = summary(todo) + "."
            if target and target != source:
                text += " " + "Published to your copy, {repo}.".format(repo=target)
            _box(parent, frame, text, "Dictionaries Published")

    def _forked(self, parent, after, busy, install, err):
        if busy:
            busy(False)
        if install:
            self._install(parent, install)
        elif err:
            _box(parent, self.frame, err + "\n\nYour changes stay on this computer.", "Couldn't Publish",
                 wx.OK | wx.ICON_WARNING)
        else:
            self.publish(parent, after, busy)

    def _install(self, parent, url):
        if _ask(parent, self.frame, "To make your copy, GitHub needs Classic Voices allowed on your account. Choose "
                "All repositories, then publish again.", "Allow Classic Voices on GitHub", "&Open GitHub", "Cancel"):
            webbrowser.open(url)


def publish_if_due(data):
    """The daily publish (when the person turned it on): git where it can push, else GitHub's API when signed in;
    quietly, on a thread. Called hourly by the app's window; each way publishes at most once a day."""
    def run():
        try:
            if not cv_git.pending(data):
                return
            if git_can_push(data):
                cv_git.publish(data)
            elif cv_gh.configured() and cv_gh.signed_in(data):
                cv_gh.publish_in_background(data)
        except Exception:
            pass
    threading.Thread(target=run, name="daily publish", daemon=True).start()


# ---------------------------------------------------------------- the dictionaries


class DictsDialog(wx.Dialog):
    _last_voice = {}  # file name -> the voice key chosen for it (this session)

    def __init__(self, frame):
        super().__init__(frame, title="Classic Voices pronunciation dictionaries",
                         style=wx.DEFAULT_DIALOG_STYLE | wx.RESIZE_BORDER)
        self.frame = frame
        self.data = system.data_root()
        self.publisher = Publisher(frame, self.data)
        self.community = system.get_setting("CommunityDictionaries")
        self.start_editing()
        check_git_in_background(self.data)
        main = wx.BoxSizer(wx.VERTICAL)
        main.Add(wx.StaticText(self, label=(
            "While this dialog is open the voices use no dictionary, so you hear words as they read them by "
            "themselves. Your entries work even with the community dictionaries turned off.")), flag=wx.ALL, border=5)
        main.Add(wx.StaticText(self, label="Dictionary &file:"), flag=wx.LEFT | wx.TOP, border=5)
        self.names = self._names()
        self.file = wx.Choice(self, choices=self.names, name="Dictionary file")
        self.file.SetSelection(self._default_name())
        self.file.Bind(wx.EVT_CHOICE, lambda evt: self.file_changed())
        main.Add(self.file, flag=wx.EXPAND | wx.ALL, border=5)
        main.Add(wx.StaticText(self, label="&Voice for previews:"), flag=wx.LEFT | wx.TOP, border=5)
        self.voiceChoice = wx.Choice(self, name="Voice for previews")
        self.voiceChoice.Bind(wx.EVT_CHOICE, lambda evt: self.voice_changed())
        main.Add(self.voiceChoice, flag=wx.EXPAND | wx.ALL, border=5)
        main.Add(wx.StaticText(self, label="&Entries:"), flag=wx.LEFT | wx.TOP, border=5)
        self.list = wx.ListBox(self, size=(620, 300), name="Entries")
        self.list.Bind(wx.EVT_LISTBOX, lambda evt: self.update_buttons())
        self.list.Bind(wx.EVT_LISTBOX_DCLICK, lambda evt: self.onEdit(None))
        self.list.Bind(wx.EVT_CONTEXT_MENU, self.onMenu)
        main.Add(self.list, proportion=1, flag=wx.EXPAND | wx.ALL, border=5)
        btns = wx.BoxSizer(wx.HORIZONTAL)  # the entry's actions
        more = wx.BoxSizer(wx.HORIZONTAL)  # the dictionaries'
        self.addBtn = wx.Button(self, label="&Add...")
        self.editBtn = wx.Button(self, label="E&dit...")
        self.removeBtn = wx.Button(self, label="&Remove")
        self.restoreBtn = wx.Button(self, label="Res&tore the community's")
        self.sourceBtn = wx.Button(self, label="&Source...")
        self.fetchBtn = wx.Button(self, label="Download the &latest dictionaries")
        self.publishBtn = wx.Button(self, label="&Publish now")
        self.githubBtn = wx.Button(self, label="Sign in to &GitHub")
        self.githubBtn.Show(cv_gh.configured())
        self._update_github()
        for b, h in ((self.addBtn, self.onAdd), (self.editBtn, self.onEdit), (self.removeBtn, self.onRemove),
                     (self.restoreBtn, self.onRestore), (self.sourceBtn, self.onSource), (self.fetchBtn, self.onFetch),
                     (self.publishBtn, self.onPublish), (self.githubBtn, self.onGitHub)):
            b.Bind(wx.EVT_BUTTON, h)
            (btns if b in (self.addBtn, self.editBtn, self.removeBtn, self.restoreBtn) else more).Add(
                b, flag=wx.ALL, border=2)
        main.Add(btns, flag=wx.ALL, border=3)
        main.Add(more, flag=wx.ALL, border=3)
        self.daily = wx.CheckBox(self, label="Publish my changes a&utomatically once a day")
        self.daily.SetValue(system.get_setting("PublishDictionaries"))
        self.daily.Bind(wx.EVT_CHECKBOX, lambda evt: system.set_setting("PublishDictionaries", self.daily.GetValue()))
        main.Add(self.daily, flag=wx.ALL, border=5)
        close = wx.Button(self, wx.ID_CLOSE)
        main.Add(close, flag=wx.ALL, border=5)
        self.Bind(wx.EVT_BUTTON, lambda evt: self.EndModal(wx.ID_CLOSE), id=wx.ID_CLOSE)
        self.SetEscapeId(wx.ID_CLOSE)
        self.Bind(wx.EVT_CHAR_HOOK, self.onCharHook)
        self.SetSizerAndFit(main)
        self.rows = []
        self.fill_voices()
        self.fill()
        self.list.SetFocus()

    # ---- the "no dictionary" flag, held while the dialog is open
    def start_editing(self):
        cv_dicts.EDITING = True  # the app's own previews (Windows: in this process)
        system.hold_editing()  # every voice, the system's included (cv_dict.c)
        self.timer = wx.Timer(self)
        self.Bind(wx.EVT_TIMER, lambda evt: system.hold_editing(), self.timer)
        self.timer.Start(15 * 1000)

    def stop_editing(self):
        self.timer.Stop()
        cv_dicts.EDITING = False
        system.release_editing()

    # ---- files and voices
    def _names(self):
        """The files of the languages of your voices, and every other file there is (fetched or yours)."""
        out = set(cv_dicts.file_names(self.data))
        try:
            for v in cv_voices.scan(system.voices_root(), system.downloads_root(), include_hidden=True):
                out.update(cv_dicts.names(v.key, v.language))
        except Exception:
            pass
        return sorted(out) or ["en.dic"]

    def _default_name(self):
        """The language file of the voice last heard in the app (its community entries are for every voice of the
        language), else English's."""
        last = getattr(self.frame, "last_voice", None)
        wanted = cv_dicts.names(*last)[-1:] if last else []
        for n in wanted + ["en.dic"]:
            if n in self.names:
                return self.names.index(n)
        return 0

    @property
    def name(self):
        return self.names[self.file.GetSelection()]

    def file_changed(self):
        self.fill_voices()
        self.fill()

    def fill_voices(self):
        self.voices = voices_for(self.name)
        self.voiceChoice.Clear()
        if not self.voices:
            self.voiceChoice.Append("None of your voices uses this file")
            self.voiceChoice.SetSelection(0)
            return
        for _k, _l, n in self.voices:
            self.voiceChoice.Append(n)
        keys = [k for k, _l, _n in self.voices]
        last = getattr(self.frame, "last_voice", None)
        want = self._last_voice.get(self.name) or (last[0] if last else None)
        self.voiceChoice.SetSelection(keys.index(want) if want in keys else 0)

    def voice_changed(self):
        v = self.voice()
        if v is not None:
            self._last_voice[self.name] = v.key

    def voice(self):
        i = self.voiceChoice.GetSelection()
        if not self.voices or not 0 <= i < len(self.voices):
            return None
        k, l, n = self.voices[i]
        return Voice(self.frame, k, l, n)

    # ---- the entries
    @staticmethod
    def _row_label(r):
        e = r.entry
        kind = TYPE_NAMES.get(e.type, str(e.type)).lower()
        return "{pattern}: {replacement}; {kind}{case}; {whose}".format(
            pattern=e.pattern, replacement=e.replacement if e.replacement else "(nothing)", kind=kind,
            case=", case sensitive" if e.case else "", whose=WHOSE[r.origin].lower())

    def fill(self, select=None):
        self.rows = cv_dicts.rows(self.data, self.name)
        self.list.Set([self._row_label(r) for r in self.rows])
        if self.rows:
            at = next((i for i, r in enumerate(self.rows) if r.entry.key == select), 0) if select else 0
            self.list.SetSelection(at)
            self.list.EnsureVisible(at)
        self.update_buttons()

    def selected(self):
        i = self.list.GetSelection()
        return self.rows[i] if 0 <= i < len(self.rows) else None

    def update_buttons(self):
        r = self.selected()
        self.editBtn.Enable(r is not None)
        self.removeBtn.Enable(r is not None and r.origin != "removed")
        self.restoreBtn.Enable(r is not None and r.origin in ("changed", "removed"))

    def onCharHook(self, evt):
        """Enter on an entry edits it, Delete removes it (only there: focusing an entry never does anything)."""
        if self.FindFocus() is self.list and not evt.HasAnyModifiers():
            k = evt.GetKeyCode()
            if k in (wx.WXK_RETURN, wx.WXK_NUMPAD_ENTER):
                return self.onEdit(None)
            if k == wx.WXK_DELETE:
                return self.onRemove(None)
        evt.Skip()

    def onMenu(self, evt):
        """The entry's actions (Applications key, Shift+F10 or a right click)."""
        r = self.selected()
        if r is None:
            return
        menu = wx.Menu()
        items = [("E&dit...", lambda: self.onEdit(None), True),
                 ("&Remove", lambda: self.onRemove(None), r.origin != "removed"),
                 ("Res&tore the community's", lambda: self.onRestore(None), r.origin in ("changed", "removed")),
                 None,
                 ("Pre&view", lambda: self.preview_row(r, "alone"), True),
                 ("Preview with ear&lier entries", lambda: self.preview_row(r, "earlier"), True),
                 ("Preview with all e&ntries", lambda: self.preview_row(r, "all"), True)]
        can_preview = self.voices and r.origin != "removed" and r.entry.type != 1
        for it in items:
            if it is None:
                menu.AppendSeparator()
                continue
            label, fn, ok = it
            mi = menu.Append(wx.ID_ANY, label)
            mi.Enable(bool(ok and (can_preview if label.startswith("Pre") else True)))
            menu.Bind(wx.EVT_MENU, lambda e, fn=fn: wx.CallAfter(fn), mi)
        self.list.PopupMenu(menu)
        menu.Destroy()

    def preview_row(self, r, scope):
        """An entry of the list said with its pattern as the test text: alone, with the entries before it, or with
        the whole file."""
        v = self.voice()
        if v is None:
            return
        v.say(r.entry.pattern, entries=cv_dicts.preview_entries(self.data, self.name, r.entry, None, self.community,
                                                                 scope))

    def _entry_dialog(self, title, entry):
        return EntryDialog(self, title, entry, self.voice(), self.data, self.name, self.community)

    def onAdd(self, evt):
        d = self._entry_dialog("Add an entry", None)
        if d.ShowModal() == wx.ID_OK:
            e = d.entry()
            cv_dicts.put(self.data, self.name, e)
            self.fill(e.key)
            self.list.SetFocus()
        d.Destroy()

    def onEdit(self, evt):
        r = self.selected()
        if r is None:
            return
        d = self._entry_dialog("Edit the entry", r.entry)
        if d.ShowModal() == wx.ID_OK:
            e = d.entry()
            cv_dicts.put(self.data, self.name, e, old_key=r.entry.key)
            self.fill(e.key)
            self.list.SetFocus()
        d.Destroy()

    def onRemove(self, evt):
        r = self.selected()
        if r is None or r.origin == "removed":
            return
        at = self.list.GetSelection()
        cv_dicts.remove(self.data, self.name, r.entry.key)
        self.fill(r.entry.key)
        if self.rows and not any(x.entry.key == r.entry.key for x in self.rows):  # yours: gone from the list
            self.list.SetSelection(min(at, len(self.rows) - 1))
            self.update_buttons()
        # focus on the list reads the entry as it is now ("removed by you") or the one after it
        self.list.SetFocus()

    def onRestore(self, evt):
        r = self.selected()
        if r is None:
            return
        cv_dicts.restore(self.data, self.name, r.entry.key)
        self.fill(r.entry.key)
        self.list.SetFocus()

    def onSource(self, evt):
        d = SourceDialog(self, self.data)
        if d.ShowModal() == wx.ID_OK and (d.chosen, d.chosen_branch) != cv_dicts.source(self.data):
            cv_dicts.set_source(self.data, d.chosen, d.chosen_branch)
            check_git_in_background(self.data)
            self._fetch(True)
        d.Destroy()

    def onFetch(self, evt):
        """The newest files from the source now (they're otherwise fetched every few hours)."""
        self._fetch(False)

    def _fetch(self, other_source):
        if getattr(self, "fetching", False):
            return
        self.fetching = True
        self.fetchBtn.SetLabel("Downloading the latest dictionaries...")

        def done(ok):
            def show():
                if not _alive(self):
                    return
                self.fetching = False
                self.fetchBtn.SetLabel("Download the &latest dictionaries")
                r = self.selected()
                self.fill(r.entry.key if r else None)
                # what changed, as the phones' notification says it (Dictionaries Updated, with OK)
                text = cv_dicts.last_changes.text() if ok else None
                if not ok:
                    wx.MessageBox("The dictionaries couldn't be fetched from there." if other_source else
                                  "The dictionaries couldn't be downloaded. Check the connection and try again.",
                                  APP, wx.OK | wx.ICON_WARNING, self)
                elif text:
                    wx.MessageBox(text, "Dictionaries Updated", wx.OK | wx.ICON_INFORMATION, self)
                elif other_source:
                    wx.MessageBox("Dictionaries fetched.", APP, wx.OK | wx.ICON_INFORMATION, self)
                else:
                    wx.MessageBox("Nothing has changed since the last download.", "Dictionaries Up to Date",
                                  wx.OK | wx.ICON_INFORMATION, self)
            wx.CallAfter(show)
        cv_dicts.refresh_all(self.data, ua=UA, force=True, done=done)

    # ---- publishing
    def _update_github(self):
        who = cv_gh.login(self.data) if cv_gh.signed_in(self.data) else None
        self.githubBtn.SetLabel("Sign out of &GitHub ({login})".format(login=who) if who else
                                "Sign out of &GitHub" if cv_gh.signed_in(self.data) else "Sign in to &GitHub")
        self.Layout()

    def _after_publish(self):
        if _alive(self):
            r = self.selected()
            self.fill(r.entry.key if r else None)
            self._update_github()

    def _busy(self, on):
        if _alive(self):
            self.publishBtn.SetLabel("Publishing..." if on else "&Publish now")

    def onPublish(self, evt):
        self.publisher.publish(self, after=self._after_publish, busy=self._busy)

    def onGitHub(self, evt):
        if cv_gh.signed_in(self.data):
            cv_gh.sign_out(self.data)
        else:
            who = sign_in(self, self.frame, self.data)
            if who:
                wx.MessageBox("Signed in as {login}".format(login=who), "GitHub", wx.OK | wx.ICON_INFORMATION, self)
        self._update_github()
        self.githubBtn.SetFocus()

    def finish(self):
        """After the dialog closed: the dictionaries are used again; with changes nothing here can publish, the
        offer to sign in to GitHub (unless the person said not to). Whether to publish now."""
        self.stop_editing()
        data = self.data
        try:
            if (cv_gh.configured() and not cv_gh.signed_in(data) and not cv_gh.dont_ask(data)
                    and git_can_push(data, cached=True) is not True and cv_git.pending(data)):
                d = AskSignInDialog(self.frame)
                answer = d.ShowModal()
                d.Destroy()
                if answer == AskSignInDialog.DONT:
                    cv_gh.set_dont_ask(data)
                return answer == wx.ID_YES
        except Exception:
            pass
        return False


def open_editor(frame):
    """Settings > Edit the pronunciation dictionaries: the dialog, then (if asked) publishing."""
    try:
        d = DictsDialog(frame)
    except Exception:
        cv_dicts.EDITING = False
        system.release_editing()
        raise
    try:
        d.ShowModal()
        frame.stop_speaking()
        publish = d.finish()
    finally:
        cv_dicts.EDITING = False
        system.release_editing()
        d.Destroy()
    if publish:
        Publisher(frame, system.data_root()).publish(None)
