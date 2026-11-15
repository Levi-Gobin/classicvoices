# Classic Voices pronunciation dictionaries: NVDA menu -> Tools -> Classic Voices pronunciation dictionaries.
# The community's entries (fetched, synthDrivers/_classicvoices/dicts.py) and the person's own, in one list per
# file, as NVDA's own speech dictionary dialog shows them; an entry's replacement can hold phonemes (X-SAMPA in
# ~~...~~), picked from the voice's own sounds. While the dialog is open the voices use no dictionary at all, so
# the patterns are heard as the voice reads them by itself; the edits are used from the moment it closes.
import threading
import webbrowser

import api
import globalPluginHandler
import gui
import synthDriverHandler
import ui
import wx
from logHandler import log

from synthDrivers._classicvoices import dicts as cv_dicts
from synthDrivers._classicvoices import ghpublish as cv_gh
from synthDrivers._classicvoices import gitsync as cv_git
from synthDrivers._classicvoices import lib as cv_lib
from synthDrivers._classicvoices import paths as cv_paths

import addonHandler
try:
    addonHandler.initTranslation()
except Exception:
    pass

# Sounds offered when the repository has no phonemes/<language>.txt yet: each one the voice can say, heard on its
# own (no example word).
GENERIC = ("i y I Y e 2 E 9 { a A Q O o U u V @ 3 6 1 } M 7 8 & "
           "a~ E~ O~ 9~ o~ "
           "p b t d k g q G\\ ? c J\\ "
           "m n N J F "
           "f v T D s z S Z x G h X R C j\\ s\\ z\\ "
           "tS dZ ts dz "
           "l L 5 r r\\ 4 R j w H").split()


def _synth():
    """The Classic Voices driver when it's NVDA's voice, else None."""
    s = synthDriverHandler.getSynth()
    return s if s is not None and getattr(s, "name", "") == "classicvoices" else None


def _in_span(text, pos):
    """Whether pos is inside a ~~...~~ span of text."""
    return text[:pos].count("~~") % 2 == 1


class PhonemeDialog(wx.Dialog):
    """The voice's sounds, each with an example word from phonemes/<language>.txt: Space hears the sound and
    the word, Enter inserts it."""

    def __init__(self, parent, info):
        # Translators: title of the list of sounds to insert into a replacement.
        super().__init__(parent, title=_("Insert a phoneme"))
        self.info = info
        self.items = self._items()
        main = wx.BoxSizer(wx.VERTICAL)
        # Translators: explains the list of sounds.
        main.Add(wx.StaticText(self, label=_(
            "The current voice's sounds. Press Space to hear one and its example word, Enter to insert it.")),
            flag=wx.ALL, border=5)
        # Translators: label of the list of sounds.
        main.Add(wx.StaticText(self, label=_("&Sounds:")), flag=wx.LEFT | wx.TOP, border=5)
        self.list = wx.ListBox(self, choices=[self._label(x, e) for x, e, _t in self.items], size=(360, 300))
        if self.items:
            self.list.SetSelection(0)
        self.list.Bind(wx.EVT_KEY_DOWN, self.onKey)
        self.list.Bind(wx.EVT_LISTBOX_DCLICK, lambda evt: self.EndModal(wx.ID_OK))
        main.Add(self.list, proportion=1, flag=wx.EXPAND | wx.ALL, border=5)
        btns = wx.BoxSizer(wx.HORIZONTAL)
        # Translators: button that says the selected sound and its example.
        hear = wx.Button(self, label=_("&Hear"))
        hear.Bind(wx.EVT_BUTTON, lambda evt: self.hear())
        btns.Add(hear)
        # Translators: button that inserts the selected sound.
        ok = wx.Button(self, wx.ID_OK, label=_("&Insert"))
        ok.SetDefault()
        btns.Add(ok)
        btns.Add(wx.Button(self, wx.ID_CANCEL))
        main.Add(btns, flag=wx.ALL, border=5)
        self.SetSizerAndFit(main)
        self.list.SetFocus()

    @staticmethod
    def _label(xsampa, example):
        # Translators: a sound in the list: its symbol and a word that has it ("S, as in ship").
        return _("{symbol}, as in {word}").format(symbol=xsampa, word=example) if example else xsampa

    def _engine_text(self, ipa):
        key, lang, vroot, droot, _data = self.info
        return cv_lib.phoneme_text(vroot, droot, key, lang, ipa, False)

    def _items(self):
        """(X-SAMPA, example, engine text of the sound heard alone) for each sound the voice has, the language's
        list first, the same sound offered once."""
        key, lang, _v, _d, data = self.info
        listed = cv_dicts.phoneme_examples(data, lang)
        out, seen = [], set()
        for x, ex in listed or [(g, "") for g in GENERIC]:
            ipa = cv_lib.xsampa_to_ipa(x)
            if not ipa:
                continue
            own = self._engine_text(ipa)
            if not own or own in seen:
                continue  # the voice hasn't got it (or says it as one already in the list)
            seen.add(own)
            # a consonant is heard between two a's: alone, some voices say nothing
            heard = own if cv_lib.ipa_has_vowel(ipa) else (self._engine_text("a" + ipa + "a") or own)
            out.append((x, ex, heard))
        return out

    def hear(self):
        i = self.list.GetSelection()
        s = _synth()
        if i < 0 or s is None:
            return
        x, ex, heard = self.items[i]
        example = ex.replace("\x1b", "")
        s.try_out("", engine_text=heard + ("  " + example if example else ""))

    def onKey(self, evt):
        if evt.GetKeyCode() == wx.WXK_SPACE:
            self.hear()
        else:
            evt.Skip()

    def chosen(self):
        i = self.list.GetSelection()
        return self.items[i][0] if i >= 0 else None


class EntryDialog(wx.Dialog):
    """Adds or edits one entry: pattern, replacement (with phonemes), case, type; Hear it says the test text with
    just this entry."""

    TYPES = ((0, "Anywhere"), (2, "Whole word"), (1, "Regular expression"))

    def __init__(self, parent, title, entry, info):
        super().__init__(parent, title=title)
        self.info = info
        main = wx.BoxSizer(wx.VERTICAL)
        # Translators: label of an entry's pattern.
        main.Add(wx.StaticText(self, label=_("&Pattern:")), flag=wx.LEFT | wx.TOP, border=5)
        self.pattern = wx.TextCtrl(self, value=entry.pattern if entry else "", size=(420, -1))
        main.Add(self.pattern, flag=wx.EXPAND | wx.ALL, border=5)
        # Translators: label of an entry's replacement.
        main.Add(wx.StaticText(self, label=_("&Replacement (phonemes go between ~~ and ~~):")),
                 flag=wx.LEFT | wx.TOP, border=5)
        self.replacement = wx.TextCtrl(self, value=entry.replacement if entry else "", size=(420, -1))
        main.Add(self.replacement, flag=wx.EXPAND | wx.ALL, border=5)
        row = wx.BoxSizer(wx.HORIZONTAL)
        # Translators: button that opens the list of the voice's sounds.
        phon = wx.Button(self, label=_("Insert p&honeme..."))
        phon.Bind(wx.EVT_BUTTON, self.onPhoneme)
        phon.Enable(_synth() is not None)
        row.Add(phon)
        # Translators: button turning the replacement's words into phonemes, as the voice says them.
        conv = wx.Button(self, label=_("Convert to phon&emes"))
        conv.Bind(wx.EVT_BUTTON, self.onConvert)
        conv.Enable(_synth() is not None and bool(info[0]) and not str(info[0]).startswith("mt"))
        row.Add(conv)
        main.Add(row, flag=wx.ALL, border=5)
        # Translators: check box: the pattern matches only with the same capitals.
        self.case = wx.CheckBox(self, label=_("Ca&se sensitive"))
        self.case.SetValue(bool(entry and entry.case))
        main.Add(self.case, flag=wx.ALL, border=5)
        # Translators: the kinds of pattern, as in NVDA's own speech dictionaries.
        names = {0: _("&Anywhere"), 2: _("&Whole word"), 1: _("Regular e&xpression")}
        # Translators: label of the choice of pattern kind.
        self.kind = wx.RadioBox(self, label=_("Type"), choices=[names[t] for t, _n in self.TYPES])
        self.kind.SetSelection([t for t, _n in self.TYPES].index(entry.type if entry else 2))
        main.Add(self.kind, flag=wx.ALL, border=5)
        # Translators: label of the text Preview says.
        main.Add(wx.StaticText(self, label=_("&Test text:")), flag=wx.LEFT | wx.TOP, border=5)
        self.test = wx.TextCtrl(self, value=entry.pattern if entry and entry.type != 1 else "", size=(420, -1))
        main.Add(self.test, flag=wx.EXPAND | wx.ALL, border=5)
        # (Without the entry, the text is simply read: the voices use no dictionary while the dialog is open.)
        # Translators: button saying the test text with this entry.
        preview = wx.Button(self, label=_("Pre&view"))
        preview.Bind(wx.EVT_BUTTON, lambda evt: self.hear(True))
        preview.Enable(_synth() is not None)
        main.Add(preview, flag=wx.ALL, border=5)
        main.Add(self.CreateButtonSizer(wx.OK | wx.CANCEL), flag=wx.ALL, border=5)
        self.Bind(wx.EVT_BUTTON, self.onOk, id=wx.ID_OK)
        self.SetSizerAndFit(main)
        self.pattern.SetFocus()

    def entry(self):
        return cv_dicts.Entry(self.pattern.GetValue(), self.replacement.GetValue().replace("\t", " "),
                              self.case.GetValue(), self.TYPES[self.kind.GetSelection()][0])

    def hear(self, with_entry):
        s = _synth()
        text = self.test.GetValue() or self.pattern.GetValue()
        if s is None or not text:
            return
        e = self.entry()
        s.try_out(text, entries=e.line() if with_entry and e.pattern else "")

    def onPhoneme(self, evt):
        d = PhonemeDialog(self, self.info)
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
        """The replacement's words as phonemes, as the voice says them (no stress marks: add them where needed)."""
        key, lang, vroot, droot, _data = self.info
        text, done, left = cv_dicts.to_phonemes(
            self.replacement.GetValue(), self.TYPES[self.kind.GetSelection()][0] == 1,
            lambda w: cv_lib.text_xsampa(vroot, droot, key, lang, w))
        self.replacement.SetValue(text)
        self.replacement.SetFocus()
        if done and not left:
            # Translators: after converting the replacement; stress isn't known, so it's added by hand.
            ui.message(_("Converted to phonemes. Add stress marks where needed."))
        elif done:
            # Translators: some words couldn't be converted.
            ui.message(_("Converted {done} words; {left} stayed as they were.").format(done=done, left=left))
        else:
            # Translators: nothing could be converted.
            ui.message(_("Nothing to convert."))

    def onOk(self, evt):
        e = self.entry()
        if not e.pattern:
            # Translators: an entry needs a pattern.
            gui.messageBox(_("Type the pattern to replace."), _("Classic Voices"), wx.OK, self)
            return
        if "\t" in e.pattern:
            e.pattern = e.pattern.replace("\t", " ")
            self.pattern.SetValue(e.pattern)
        if not e.replacement and gui.messageBox(
                # Translators: asked when an entry's replacement is empty.
                _("The replacement is empty: the voices will skip \"{pattern}\" entirely. That suits things like "
                  "rows of box-drawing characters, not words. Save it anyway?").format(pattern=e.pattern),
                _("Classic Voices"), wx.YES_NO | wx.NO_DEFAULT | wx.ICON_WARNING, self) != wx.YES:
            return
        if e.replacement.count("~~") % 2 and not e.replacement.rstrip().endswith("~~~"):
            # Translators: the phonemes in a replacement aren't closed.
            gui.messageBox(_("A phoneme span isn't closed: end it with ~~."), _("Classic Voices"), wx.OK, self)
            return
        evt.Skip()


class SourceDialog(wx.Dialog):
    """The GitHub repository the community dictionaries come from (a fork, say)."""

    def __init__(self, parent, data):
        # Translators: title of the dialog choosing where the community dictionaries come from.
        super().__init__(parent, title=_("Dictionary source"))
        repo, branch = cv_dicts.source(data)
        main = wx.BoxSizer(wx.VERTICAL)
        # Translators: label of the repository field.
        main.Add(wx.StaticText(self, label=_("GitHub &repository (owner/name or its address):")),
                 flag=wx.LEFT | wx.TOP, border=5)
        self.repo = wx.TextCtrl(self, value=repo, size=(360, -1))
        main.Add(self.repo, flag=wx.EXPAND | wx.ALL, border=5)
        # Translators: label of the branch field.
        main.Add(wx.StaticText(self, label=_("&Branch:")), flag=wx.LEFT | wx.TOP, border=5)
        self.branch = wx.TextCtrl(self, value=branch, size=(360, -1))
        main.Add(self.branch, flag=wx.EXPAND | wx.ALL, border=5)
        row = wx.BoxSizer(wx.HORIZONTAL)
        # Translators: button going back to the usual repository.
        default = wx.Button(self, label=_("&Default"))
        default.Bind(wx.EVT_BUTTON, lambda evt: (self.repo.SetValue(cv_dicts.DEFAULT_REPO),
                                                 self.branch.SetValue(cv_dicts.DEFAULT_BRANCH)))
        row.Add(default)
        main.Add(row, flag=wx.ALL, border=5)
        main.Add(self.CreateButtonSizer(wx.OK | wx.CANCEL), flag=wx.ALL, border=5)
        self.Bind(wx.EVT_BUTTON, self.onOk, id=wx.ID_OK)
        self.SetSizerAndFit(main)

    def onOk(self, evt):
        self.chosen = cv_dicts.parse_repo(self.repo.GetValue())
        if not self.chosen:
            # Translators: the repository typed isn't one.
            gui.messageBox(_("Type the repository as owner/name, for example nicopn/classicvoices-dicts."),
                           _("Classic Voices"), wx.OK, self)
            return
        self.chosen_branch = self.branch.GetValue().strip() or cv_dicts.DEFAULT_BRANCH
        evt.Skip()


def _window(w):
    """w if it's still open, else NVDA's main frame (the dictionaries dialog may have closed meanwhile)."""
    try:
        if w:
            return w
    except RuntimeError:
        pass
    return gui.mainFrame


def _popup(parent, show):
    """show(window) with the window to put a message on, in front even when the dialog is gone."""
    w = _window(parent)
    if w is gui.mainFrame:
        gui.mainFrame.prePopup()
        try:
            return show(w)
        finally:
            gui.mainFrame.postPopup()
    return show(w)


def _box(parent, text, title, style=wx.OK | wx.ICON_INFORMATION):
    _popup(parent, lambda w: gui.messageBox(text, title, style, w))


def _ask(parent, text, title, yes, no):
    """A question with two buttons named yes and no; whether yes was chosen."""
    def show(w):
        d = wx.MessageDialog(w, text, title, wx.YES_NO | wx.ICON_QUESTION)
        d.SetYesNoLabels(yes, no)
        try:
            return d.ShowModal() == wx.ID_YES
        finally:
            d.Destroy()
    return _popup(parent, show)


def _summary(todo):
    """ "3 entries added, 1 changed and 1 removed in en.dic" for {file: (added, changed, removed)}."""
    counts = [sum(len(v[i]) for v in todo.values()) for i in range(3)]
    # Translators: kinds of change in "3 entries added, 1 changed and 1 removed in en.dic".
    verbs = (_("added"), _("changed"), _("removed"))
    parts = []
    for n, verb in zip(counts, verbs):
        if not n:
            continue
        if not parts:
            # Translators: the first kind of change, e.g. "3 entries added" or "1 entry changed".
            parts.append((_("{n} entry {verb}") if n == 1 else _("{n} entries {verb}")).format(n=n, verb=verb))
        else:
            parts.append("{n} {verb}".format(n=n, verb=verb))
    # Translators: joins the last kind of change, e.g. "3 entries added and 1 removed".
    text = parts[0] if len(parts) == 1 else _("{list} and {last}").format(list=", ".join(parts[:-1]), last=parts[-1])
    if len(todo) == 1:
        # Translators: where the changes were published, e.g. "... in en.dic".
        return _("{changes} in {file}").format(changes=text, file=next(iter(todo)))
    # Translators: e.g. "... across 2 files".
    return _("{changes} across {n} files").format(changes=text, n=len(todo))


class SignInDialog(wx.Dialog):
    """GitHub's device sign-in: the code to type at github.com/login/device; closes by itself once the person
    has signed in there."""

    def __init__(self, parent, data):
        # Translators: title of the GitHub sign-in dialog.
        super().__init__(parent, title=_("Sign in to GitHub"))
        self.data = data
        self.code = None
        self.stop = threading.Event()  # set when the dialog goes: the waiting thread gives up
        main = wx.BoxSizer(wx.VERTICAL)
        # Translators: label of the text explaining the GitHub sign-in.
        main.Add(wx.StaticText(self, label=_("What to do:")), flag=wx.LEFT | wx.TOP, border=5)
        # Translators: shown while the sign-in code is asked for.
        self.text = wx.TextCtrl(self, value=_("Asking GitHub for a sign-in code..."),
                                style=wx.TE_MULTILINE | wx.TE_READONLY, size=(480, 100))
        main.Add(self.text, flag=wx.EXPAND | wx.ALL, border=5)
        btns = wx.BoxSizer(wx.HORIZONTAL)
        # Translators: button copying the sign-in code and opening GitHub's page for it.
        self.copyBtn = wx.Button(self, label=_("&Copy code and open GitHub"))
        self.copyBtn.Bind(wx.EVT_BUTTON, self.onCopy)
        self.copyBtn.Disable()
        btns.Add(self.copyBtn)
        # Translators: button giving up on signing in.
        cancel = wx.Button(self, wx.ID_CANCEL, label=_("Cancel"))
        cancel.Bind(wx.EVT_BUTTON, self.onCancel)
        btns.Add(cancel)
        self.SetEscapeId(wx.ID_CANCEL)
        main.Add(btns, flag=wx.ALL, border=5)
        self.SetSizerAndFit(main)
        self.text.SetFocus()
        threading.Thread(target=self._work, daemon=True).start()

    def _work(self):
        try:
            code = cv_gh.start_sign_in()
            wx.CallAfter(self._show_code, code)
            who = cv_gh.finish_sign_in(self.data, code, cancelled=self.stop.is_set)
            if who is not None:
                wx.CallAfter(self._done, who)
        except Exception as e:
            wx.CallAfter(self._failed, getattr(e, "message", None) or str(e) or e.__class__.__name__)

    def _alive(self):
        if self.stop.is_set():
            return False
        try:
            return bool(self)
        except RuntimeError:
            return False

    def _show_code(self, code):
        if not self._alive():
            return
        self.code = code
        # Translators: the GitHub sign-in instructions; {url} is GitHub's page, {code} the code to type there.
        self.text.SetValue(_("Go to {url} and enter the code {code}. The Copy code and open GitHub button "
                             "copies the code and opens that page. This window closes by itself once you've "
                             "signed in.").format(url=code["verification_uri"], code=code["user_code"]))
        self.copyBtn.Enable()
        self.copyBtn.SetFocus()
        # Translators: announced when the sign-in code arrives.
        ui.message(_("Your GitHub sign-in code is {code}").format(code=" ".join(code["user_code"])))

    def onCopy(self, evt):
        if self.code:
            api.copyToClip(self.code["user_code"])
            webbrowser.open(self.code["verification_uri"])

    def _done(self, who):
        if not self._alive():
            return
        # Translators: announced once signed in to GitHub; {login} is the GitHub account.
        ui.message(_("Signed in as {login}").format(login=who))
        self.EndModal(wx.ID_OK)

    def _failed(self, why):
        if not self._alive():
            return
        # Translators: the sign-in didn't work; {why} is GitHub's reason.
        msg = _("Couldn't sign in: {why}").format(why=why)
        self.text.SetValue(msg)
        self.copyBtn.Disable()
        ui.message(msg)

    def onCancel(self, evt):
        self.stop.set()
        self.EndModal(wx.ID_CANCEL)


def sign_in(parent, data):
    """The sign-in dialog; whether the person is signed in at the end."""
    def show(w):
        d = SignInDialog(w, data)
        try:
            return d.ShowModal() == wx.ID_OK
        finally:
            d.stop.set()
            d.Destroy()
    return _popup(parent, show)


class AskSignInDialog(wx.Dialog):
    """On closing the dictionaries with changes nothing here can publish: sign in to GitHub?"""

    DONT = wx.NewIdRef()

    def __init__(self, parent):
        # Translators: title of the question asked on closing the dictionaries with unpublished changes.
        super().__init__(parent, title=_("Publish your changes?"))
        main = wx.BoxSizer(wx.VERTICAL)
        # Translators: the question asked on closing the dictionaries with unpublished changes.
        main.Add(wx.StaticText(self, label=_("Sign in to GitHub to publish your changes?")), flag=wx.ALL, border=10)
        btns = wx.BoxSizer(wx.HORIZONTAL)
        # Translators: answers to "Sign in to GitHub to publish your changes?".
        for i, label in ((wx.ID_YES, _("&Yes")), (wx.ID_NO, _("&No")), (self.DONT, _("&Don't show again"))):
            b = wx.Button(self, i, label=label)
            b.Bind(wx.EVT_BUTTON, lambda evt, i=i: self.EndModal(i))
            btns.Add(b)
        self.SetEscapeId(wx.ID_NO)
        main.Add(btns, flag=wx.ALL, border=5)
        self.SetSizerAndFit(main)
        self.FindWindowById(wx.ID_YES, self).SetFocus()


_publishing = False


def publish_with_ui(parent, data, after=None):
    """Publish now: git where it's installed, else GitHub's API (signing in first when needed; offering the
    person's own copy when they can't write to the repository), then a message saying how it went. after() runs
    once it's over (the dialog refreshes its list)."""
    global _publishing
    todo = cv_git.pending(data)
    if not todo:
        # Translators: title and text when there's nothing of the person's to publish.
        _box(parent, _("You have no changes of your own that aren't published yet."), _("Nothing to Publish"))
        return
    git = cv_git.find_git() is not None
    if not git:
        if not cv_gh.configured():
            # Translators: no git and no GitHub sign-in in this build.
            _box(parent, _("Publishing needs git, and publishing through GitHub isn't set up in this build yet. "
                           "Your changes stay on this computer."), _("Couldn't Publish"), wx.OK | wx.ICON_WARNING)
            return
        if not cv_gh.signed_in(data):
            if sign_in(parent, data):
                if after:
                    after()
                publish_with_ui(parent, data, after)
            return
    if _publishing:
        # Translators: Publish now pressed while publishing.
        ui.message(_("Still publishing..."))
        return
    _publishing = True
    # Translators: announced while the changes are published.
    ui.message(_("Publishing..."))
    source = cv_dicts.source(data)[0]

    def run():
        kind, n, err, target, url = "done", 0, None, None, None
        try:
            if git:
                n, err = cv_git.publish(data, force=True)
                target = source
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
        wx.CallAfter(_published, parent, data, todo, kind, n, err, target, url, after)
    threading.Thread(target=run, daemon=True).start()


def _published(parent, data, todo, kind, n, err, target, url, after):
    global _publishing
    _publishing = False
    if after:
        after()
    source = cv_dicts.source(data)[0]
    if kind == "signin":
        if sign_in(parent, data):
            publish_with_ui(parent, data, after)
    elif kind == "fork":
        # Translators: offered to someone who can't write to the dictionaries' repository; {repo} is it.
        if _ask(parent, _("You can't publish to {repo}. Classic Voices can make your own copy of it (a fork) on "
                          "GitHub and publish your changes there. To offer them for everyone, open a pull request "
                          "from your copy on GitHub.").format(repo=source),
                _("Publish to your own copy?"), _("&Make my copy"), _("Cancel")):
            # Translators: announced while the person's copy of the repository is made.
            ui.message(_("Making your copy..."))

            def make():
                try:
                    cv_gh.make_fork(data)
                    wx.CallAfter(publish_with_ui, parent, data, after)
                except cv_gh.NeedsInstall as e:
                    wx.CallAfter(_install, parent, e.url)
                except Exception as e:
                    wx.CallAfter(_box, parent, (getattr(e, "message", None) or str(e)) + "\n\n" +
                                 _("Your changes stay on this computer."), _("Couldn't Publish"),
                                 wx.OK | wx.ICON_WARNING)
            threading.Thread(target=make, daemon=True).start()
    elif kind == "install":
        _install(parent, url)
    elif err:
        # Translators: publishing failed; the reason comes first.
        _box(parent, err + "\n\n" + _("Your changes stay on this computer."), _("Couldn't Publish"),
             wx.OK | wx.ICON_WARNING)
    elif not n:
        if target and target != source:
            # Translators: the person's copy of the repository has their changes already.
            _box(parent, _("Your copy, {repo}, has all your changes already.").format(repo=target),
                 _("Nothing to Publish"))
        else:
            _box(parent, _("You have no changes of your own that aren't published yet."), _("Nothing to Publish"))
    else:
        text = _summary(todo) + "."
        if target and target != source:
            # Translators: added when the changes went to the person's own copy of the repository.
            text += " " + _("Published to your copy, {repo}.").format(repo=target)
        # Translators: title of the message after publishing.
        _box(parent, text, _("Dictionaries Published"))


def _install(parent, url):
    # Translators: GitHub needs the Classic Voices app allowed on the person's account to make their copy.
    if _ask(parent, _("To make your copy, GitHub needs Classic Voices allowed on your account. Choose All "
                      "repositories, then publish again."), _("Allow Classic Voices on GitHub"),
            _("&Open GitHub"), _("Cancel")):
        webbrowser.open(url)


class DictsDialog(wx.Dialog):
    _instance = None

    def __init__(self, parent):
        # Translators: title of the pronunciation dictionaries dialog.
        super().__init__(parent, title=_("Classic Voices pronunciation dictionaries"))
        DictsDialog._instance = self
        cv_dicts.EDITING = True  # the patterns are heard uncorrected meanwhile
        s = _synth()
        self.data = cv_paths.data_root()
        self.info = s.voice_info() if s is not None else (None, None, cv_paths.voices_root(),
                                                            cv_paths.downloads_root(), self.data)
        main = wx.BoxSizer(wx.VERTICAL)
        # Translators: explains the dialog.
        main.Add(wx.StaticText(self, label=_(
            "While this dialog is open the voices use no dictionary, so you hear words as they read them by "
            "themselves. Your entries work even with the community dictionaries turned off.")), flag=wx.ALL, border=5)
        # Translators: label of the choice of dictionary file.
        main.Add(wx.StaticText(self, label=_("Dictionary &file:")), flag=wx.LEFT | wx.TOP, border=5)
        self.names = self._names()
        self.file = wx.Choice(self, choices=self.names)
        self.file.SetSelection(self._default_name())
        self.file.Bind(wx.EVT_CHOICE, lambda evt: self.fill())
        main.Add(self.file, flag=wx.EXPAND | wx.ALL, border=5)
        # Translators: label of the list of entries.
        main.Add(wx.StaticText(self, label=_("&Entries:")), flag=wx.LEFT | wx.TOP, border=5)
        self.list = wx.ListCtrl(self, style=wx.LC_REPORT | wx.LC_SINGLE_SEL, size=(620, 300))
        # Translators: column headers of the entries list.
        for i, (h, w) in enumerate(((_("Pattern"), 150), (_("Replacement"), 200), (_("Type"), 110),
                                    (_("Case"), 50), (_("Whose"), 110))):
            self.list.InsertColumn(i, h, width=w)
        self.list.Bind(wx.EVT_LIST_ITEM_SELECTED, lambda evt: self.update_buttons())
        self.list.Bind(wx.EVT_LIST_ITEM_ACTIVATED, lambda evt: self.onEdit(None))
        main.Add(self.list, proportion=1, flag=wx.EXPAND | wx.ALL, border=5)
        btns = wx.BoxSizer(wx.HORIZONTAL)
        # Translators: buttons of the dictionary dialog.
        self.addBtn = wx.Button(self, label=_("&Add..."))
        self.editBtn = wx.Button(self, label=_("E&dit..."))
        self.removeBtn = wx.Button(self, label=_("&Remove"))
        self.restoreBtn = wx.Button(self, label=_("Res&tore the community's"))
        self.sourceBtn = wx.Button(self, label=_("&Source..."))
        # Translators: button fetching the newest community dictionaries now.
        self.fetchBtn = wx.Button(self, label=_("Download the &latest dictionaries"))
        # Translators: button publishing the person's changes now (with git, or through GitHub).
        self.publishBtn = wx.Button(self, label=_("&Publish now"))
        # Without git: signing in to GitHub (its API publishes); its label says which way it goes
        self.githubBtn = wx.Button(self, label=_("Sign in to &GitHub"))
        self.githubBtn.Show(cv_git.find_git() is None and cv_gh.configured())
        self._update_github()
        for b, h in ((self.addBtn, self.onAdd), (self.editBtn, self.onEdit), (self.removeBtn, self.onRemove),
                     (self.restoreBtn, self.onRestore), (self.sourceBtn, self.onSource), (self.fetchBtn, self.onFetch),
                     (self.publishBtn, self.onPublish), (self.githubBtn, self.onGitHub)):
            b.Bind(wx.EVT_BUTTON, h)
            btns.Add(b)
        close = wx.Button(self, wx.ID_CLOSE)
        btns.Add(close)
        self.Bind(wx.EVT_BUTTON, lambda evt: self.Close(), id=wx.ID_CLOSE)
        self.SetEscapeId(wx.ID_CLOSE)
        self.Bind(wx.EVT_CLOSE, self.onClose)
        main.Add(btns, flag=wx.ALL, border=5)
        self.SetSizerAndFit(main)
        self.rows = []
        self.fill()
        self.list.SetFocus()

    def _names(self):
        key, lang, _v, _d, _data = self.info
        out = []
        if key and lang:
            out = cv_dicts.names(key, lang)[::-1]  # the language's file first, then the region's
        for n in cv_dicts.file_names(self.data):
            if n not in out:
                out.append(n)
        return out or ["en.dic"]

    def _default_name(self):
        return 0  # the language's file: the community's entries for every voice of it are there

    @property
    def name(self):
        return self.names[self.file.GetSelection()]

    def fill(self, select=None):
        self.rows = cv_dicts.rows(self.data, self.name)
        self.list.DeleteAllItems()
        kinds = {0: _("Anywhere"), 1: _("Regular expression"), 2: _("Whole word")}
        # Translators: who an entry is from, in the entries list.
        whose = {"community": _("Community"), "added": _("Yours"), "changed": _("Yours (changed)"),
                 "removed": _("Removed by you")}
        for i, r in enumerate(self.rows):
            e = r.entry
            self.list.InsertItem(i, e.pattern)
            self.list.SetItem(i, 1, e.replacement)
            self.list.SetItem(i, 2, kinds.get(e.type, str(e.type)))
            # Translators: whether an entry is case sensitive.
            self.list.SetItem(i, 3, _("Yes") if e.case else _("No"))
            self.list.SetItem(i, 4, whose[r.origin])
        if self.rows:
            at = next((i for i, r in enumerate(self.rows) if r.entry.key == select), 0) if select else 0
            self.list.Select(at)
            self.list.Focus(at)
        self.update_buttons()

    def selected(self):
        i = self.list.GetFirstSelected()
        return self.rows[i] if 0 <= i < len(self.rows) else None

    def update_buttons(self):
        r = self.selected()
        self.editBtn.Enable(r is not None)
        self.removeBtn.Enable(r is not None and r.origin != "removed")
        self.restoreBtn.Enable(r is not None and r.origin in ("changed", "removed"))

    def onAdd(self, evt):
        # Translators: title of the dialog adding an entry.
        d = EntryDialog(self, _("Add an entry"), None, self.info)
        if d.ShowModal() == wx.ID_OK:
            e = d.entry()
            cv_dicts.put(self.data, self.name, e)
            self.fill(e.key)
        d.Destroy()

    def onEdit(self, evt):
        r = self.selected()
        if r is None:
            return
        # Translators: title of the dialog editing an entry.
        d = EntryDialog(self, _("Edit the entry"), r.entry, self.info)
        if d.ShowModal() == wx.ID_OK:
            e = d.entry()
            cv_dicts.put(self.data, self.name, e, old_key=r.entry.key)
            self.fill(e.key)
        d.Destroy()

    def onRemove(self, evt):
        r = self.selected()
        if r is None:
            return
        cv_dicts.remove(self.data, self.name, r.entry.key)
        self.fill(r.entry.key)
        # Translators: announced when an entry was taken out.
        ui.message(_("Removed"))

    def onRestore(self, evt):
        r = self.selected()
        if r is None:
            return
        cv_dicts.restore(self.data, self.name, r.entry.key)
        self.fill(r.entry.key)
        # Translators: announced when an entry went back to the community's.
        ui.message(_("Restored"))

    def onSource(self, evt):
        d = SourceDialog(self, self.data)
        if d.ShowModal() == wx.ID_OK and (d.chosen, d.chosen_branch) != cv_dicts.source(self.data):
            cv_dicts.set_source(self.data, d.chosen, d.chosen_branch)

            def done(ok):
                wx.CallAfter(self.fill)
                # Translators: after the dictionaries were fetched from another repository (or not).
                wx.CallAfter(ui.message, _("Dictionaries fetched.") if ok else
                             _("The dictionaries couldn't be fetched from there."))
            cv_dicts.refresh_all(self.data, force=True, done=done)
        d.Destroy()

    def onFetch(self, evt):
        """The newest files from the source now (they're otherwise fetched every few hours)."""
        self.fetchBtn.Disable()
        # Translators: announced while the dictionaries are downloaded.
        ui.message(_("Downloading the latest dictionaries..."))

        def done(ok):
            def show():
                if not self:
                    return
                self.fetchBtn.Enable()
                key = self.selected().entry.key if self.selected() else None
                self.fill(key)
                # what changed, as the phones' notification says it (Dictionaries Updated, with OK)
                text = cv_dicts.last_changes.text() if ok else None
                if not ok:
                    # Translators: the dictionaries couldn't be downloaded.
                    gui.messageBox(_("The dictionaries couldn't be downloaded. Check the connection and try again."),
                                   _("Classic Voices"), wx.OK | wx.ICON_WARNING, self)
                elif text:
                    # Translators: title of the message saying what a download changed.
                    gui.messageBox(text, _("Dictionaries Updated"), wx.OK | wx.ICON_INFORMATION, self)
                else:
                    # Translators: a download changed nothing.
                    gui.messageBox(_("Nothing has changed since the last download."), _("Dictionaries Up to Date"),
                                   wx.OK | wx.ICON_INFORMATION, self)
            wx.CallAfter(show)
        cv_dicts.refresh_all(self.data, force=True, done=done)

    def _update_github(self):
        # Translators: the GitHub button: sign in, or (signed in) sign out.
        self.githubBtn.SetLabel(_("Sign out of &GitHub") if cv_gh.signed_in(self.data) else _("Sign in to &GitHub"))

    def _after_publish(self):
        try:
            if self:
                self.fill()
                self._update_github()
        except RuntimeError:
            pass  # closed meanwhile

    def onPublish(self, evt):
        publish_with_ui(self, self.data, after=self._after_publish)

    def onGitHub(self, evt):
        if cv_gh.signed_in(self.data):
            who = cv_gh.login(self.data)
            cv_gh.sign_out(self.data)
            # Translators: announced after signing out of GitHub; {login} is the account.
            ui.message(_("Signed out of GitHub ({login})").format(login=who) if who else _("Signed out of GitHub"))
        else:
            sign_in(self, self.data)
        self._update_github()
        self.githubBtn.SetFocus()

    def onClose(self, evt):
        cv_dicts.EDITING = False
        DictsDialog._instance = None
        data = self.data
        # Nothing here can publish the person's changes: offer to sign in to GitHub (unless they said not to)
        answer = None
        try:
            if (cv_git.find_git() is None and cv_gh.configured() and not cv_gh.signed_in(data)
                    and not cv_gh.dont_ask(data) and cv_git.pending(data)):
                d = AskSignInDialog(self)
                answer = d.ShowModal()
                d.Destroy()
                if answer == AskSignInDialog.DONT:
                    cv_gh.set_dont_ask(data)
        except Exception:
            log.debugWarning("Classic Voices: the sign-in question failed", exc_info=True)
        self.Destroy()
        if answer == wx.ID_YES:
            wx.CallAfter(publish_with_ui, None, data)


def _speech_dicts_menu():
    """NVDA's Preferences > Speech dictionaries submenu (its only submenu there, 2023.1 on), or None."""
    try:
        prefs = getattr(gui.mainFrame.sysTrayIcon, "preferencesMenu", None)
        if prefs is None:
            return None
        for item in prefs.GetMenuItems():
            sub = item.GetSubMenu()
            if sub is not None:
                return sub
    except Exception:
        log.debugWarning("Classic Voices: no Speech dictionaries menu", exc_info=True)
    return None


class GlobalPlugin(globalPluginHandler.GlobalPlugin):
    def __init__(self):
        super().__init__()
        # In Preferences > Speech dictionaries, next to NVDA's own (its access keys there are D, V and T); the
        # Tools menu if that can't be found
        self.menu = _speech_dicts_menu()
        if self.menu is not None:
            # Translators: the item in NVDA's Preferences > Speech dictionaries menu that opens the Classic Voices
            # pronunciation dictionaries (NVDA's own items there use D, V and T as access keys).
            self.item = self.menu.Append(wx.ID_ANY, _("&Classic Voices pronunciation dictionaries..."),
                                         _("Edit the pronunciation dictionaries of the Classic Voices"))
        else:
            self.menu = gui.mainFrame.sysTrayIcon.toolsMenu
            # Translators: the Tools menu item that opens the pronunciation dictionaries.
            self.item = self.menu.Append(wx.ID_ANY, _("Classic Voices pronunciation &dictionaries..."),
                                         _("Edit the pronunciation dictionaries of the Classic Voices"))
        gui.mainFrame.sysTrayIcon.Bind(wx.EVT_MENU, self.onOpen, self.item)
        # The person's changes are published once a day where git can push them (gitsync.py), or without git
        # through GitHub's API once signed in (ghpublish.py); checked hourly, the first time a few minutes after
        # NVDA starts.
        self._publish = wx.Timer()
        self._publish.Bind(wx.EVT_TIMER, lambda evt: self._publish_if_due())
        self._publish.StartOnce(3 * 60 * 1000)

    def _publish_if_due(self):
        try:
            data = cv_paths.data_root()
            if cv_git.pending(data):
                if cv_git.find_git():
                    cv_git.publish_in_background(data)
                elif cv_gh.configured() and cv_gh.signed_in(data):
                    cv_gh.publish_in_background(data)  # git isn't here: GitHub's API
        except Exception:
            pass
        self._publish.StartOnce(3600 * 1000)

    def onOpen(self, evt):
        gui.mainFrame.prePopup()
        d = DictsDialog._instance or DictsDialog(gui.mainFrame)
        d.Raise()
        d.Show()
        gui.mainFrame.postPopup()

    def terminate(self):
        cv_dicts.EDITING = False
        try:
            self._publish.Stop()
        except Exception:
            pass
        try:
            self.menu.Remove(self.item)
        except Exception:
            pass
