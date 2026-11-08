"""The window: an iOS tab, a macOS tab and Settings, laid out like the iOS
and Mac apps (pages of buttons you drill into, each saying what it does
before you press it), with native controls only so screen readers read
everything."""
import os
import threading
import time
import webbrowser

import wx

from . import library as L
from . import system

APP = "Classic Voices"


class Page:
    """Builds one page's controls into a panel, top to bottom, in tab order."""

    def __init__(self, panel):
        self.panel = panel
        self.sizer = wx.BoxSizer(wx.VERTICAL)
        self.group = None
        self.first = None
        self.live = []  # (control, function giving its label or percent now): progress updates in place
        self.keyed = []  # (control, key): what a rebuilt page finds the focused control again by
        self.gauged = set()  # the jobs whose progress bar is on the page

    def _parent(self):
        return self.group.GetStaticBox() if self.group else self.panel

    def _add(self, ctrl, expand=True, key=None):
        """key: what the control stands for (a voice, a firmware), for one whose
        label changes as it works; others go by their kind and label."""
        (self.group or self.sizer).Add(ctrl, 0, (wx.EXPAND if expand else 0) | wx.ALL, 4)
        if self.first is None and ctrl.AcceptsFocus():
            self.first = ctrl
        if key is None:
            named = isinstance(ctrl, (wx.TextCtrl, wx.Choice, wx.Gauge))
            key = (type(ctrl).__name__, ctrl.GetName() if named else ctrl.GetLabel())
        key = (key, sum(1 for _, k in self.keyed if k[0] == key))  # the same label twice: which one
        self.keyed.append((ctrl, key))
        return ctrl

    def section(self, title):
        """A group; screen readers say its name when focus enters it."""
        self.end()
        self.group = wx.StaticBoxSizer(wx.VERTICAL, self.panel, title)
        return self

    def end(self):
        if self.group:
            self.sizer.Add(self.group, 0, wx.EXPAND | wx.ALL, 6)
            self.group = None

    def button(self, label, handler, enabled=True, live=None, key=None):
        """live: a function giving the label as progress changes."""
        b = wx.Button(self._parent(), label=live() if live else label, style=wx.BU_LEFT)
        b.Bind(wx.EVT_BUTTON, lambda e: handler())
        b.Enable(enabled)
        if live:
            self.live.append((b, live))
        return self._add(b, key=key)

    def gauge(self, name, percent):
        """A native progress bar, so the screen reader reports the progress its
        own way (NVDA's progress bar output, Orca's progress bar updates).
        percent: a function giving 0-100 now."""
        g = wx.Gauge(self._parent(), range=100, name=name)
        g.SetValue(percent())
        self.live.append((g, percent))
        return self._add(g)

    def text(self, label):
        # read-only text a screen reader can focus and read, unlike a label
        t = wx.TextCtrl(self._parent(), value=label, style=wx.TE_READONLY | wx.TE_MULTILINE | wx.TE_NO_VSCROLL
                        | wx.BORDER_NONE)
        t.SetName(label.split("\n")[0])
        t.SetMinSize((-1, t.GetCharHeight() * (label.count("\n") + 1) + 8))
        return self._add(t)

    def label(self, label):
        return self._add(wx.StaticText(self._parent(), label=label))

    def field(self, label, value="", multiline=False):
        self._add(wx.StaticText(self._parent(), label=label))
        t = wx.TextCtrl(self._parent(), value=value, style=wx.TE_MULTILINE if multiline else 0)
        t.SetName(label.rstrip(":"))
        return self._add(t)

    def check(self, label, value, handler):
        c = wx.CheckBox(self._parent(), label=label)
        c.SetValue(value)
        c.Bind(wx.EVT_CHECKBOX, lambda e: handler(c.GetValue()))
        return self._add(c)

    def choice(self, label, options, selected, handler):
        self._add(wx.StaticText(self._parent(), label=label))
        c = wx.Choice(self._parent(), choices=[o[1] for o in options])
        c.SetName(label.rstrip(":"))
        c.SetSelection(max(0, [o[0] for o in options].index(selected)) if selected in [o[0] for o in options] else 0)
        c.Bind(wx.EVT_CHOICE, lambda e: handler(options[c.GetSelection()][0]))
        return self._add(c, expand=False)

    def finish(self):
        self.end()
        self.panel.SetSizer(self.sizer)


def percent(job):
    return " %d%%" % job.percent if job.percent is not None else ""


def job_gauge(p, job, name):
    """The progress of a job, in a progress bar after its Stop button. Labels
    stay put while it runs: the bar carries the progress, every 1%."""
    p.gauged.add(job.key)
    p.gauge(name, lambda: job.percent or 0)


JOB_VERB = {"download": "Downloading", "update": "Downloading", "firmware": "Getting", "fast": "Building", "pronunciation": "Downloading",
            "copy": "Copying"}


def other_jobs(p, jobs):
    """Every job without a progress bar on this page gets one at its end, so
    the progress goes on being heard after leaving the page it started on."""
    others = [j for j in jobs if j.key not in p.gauged]
    if others:
        p.section("In progress")
        for j in others:
            job_gauge(p, j, "%s %s" % (JOB_VERB.get(j.kind, "Working on"), j.label))


def voice_label(lib, item):
    """What a voice's button says: what pressing it does comes first when it
    doesn't simply open the voice (as in the iOS app)."""
    job = lib.job_for(item.id)
    if job:
        return "Stop downloading %s, %s" % (item.title, L.size_text(job.size))
    if item.state == "available":
        return "Download %s, %s" % (item.title, L.size_text(item.size))
    if item.state == "needs":
        return "%s, requires the %s firmware" % (item.title, L.names_text(item.needs))
    extra = []
    if item.state == "installed":
        extra.append("using %s" % L.size_text(item.size))
    if lib.is_hidden(item):
        extra.append("not a system voice")
    return item.title + (", " + ", ".join(extra) if extra else "")


class Browser(wx.Panel):
    """One tab: a stack of pages (routes) with Back."""

    def __init__(self, parent, frame, platform):
        super().__init__(parent)
        self.frame, self.lib, self.platform = frame, frame.lib, platform
        self.version = "all"
        self.query = ""
        self.stack = []  # routes: ("language", code), ("voice", id), ("search", q), ("diagnostics",), ("fast",)
        self.came_from = []  # per page opened: the key of the control that opened it, to return to
        self.live = []
        self.keyed = []  # the page's (control, key)s
        self.focus_key = None  # the control last focused on this page, by key
        top = wx.BoxSizer(wx.HORIZONTAL)
        self.back = wx.Button(self, label="&Back")
        self.back.Bind(wx.EVT_BUTTON, lambda e: self.go_back())
        top.Add(self.back, 0, wx.ALL, 4)
        self.where = wx.StaticText(self, label="")
        top.Add(self.where, 1, wx.ALIGN_CENTER_VERTICAL | wx.ALL, 4)
        # The first page's search field and version list stay put while the
        # page under them is rebuilt: choosing a version rebuilt the page with
        # the list in it, and focus fell to the panel ("panel" instead of the
        # version) and out of the list.
        self.head = wx.Panel(self)
        head = wx.BoxSizer(wx.VERTICAL)
        head.Add(wx.StaticText(self.head, label="Search all voices:"), 0, wx.ALL, 4)
        self.search = wx.TextCtrl(self.head, value="", style=wx.TE_PROCESS_ENTER)
        self.search.SetName("Search all voices")
        self.search.Bind(wx.EVT_TEXT_ENTER, lambda e: self._search(self.search.GetValue()))
        head.Add(self.search, 0, wx.EXPAND | wx.ALL, 4)
        head.Add(wx.StaticText(self.head, label="Version:"), 0, wx.ALL, 4)
        self.versions = wx.Choice(self.head)
        self.versions.SetName("Version")
        self.version_ids = []
        self.versions.Bind(wx.EVT_CHOICE, lambda e: self._set_version(self.version_ids[self.versions.GetSelection()]))
        head.Add(self.versions, 0, wx.ALL, 4)
        self.head.SetSizer(head)
        self.body = wx.ScrolledWindow(self, style=wx.VSCROLL)
        self.body.SetScrollRate(0, 20)
        outer = wx.BoxSizer(wx.VERTICAL)
        outer.Add(top, 0, wx.EXPAND)
        outer.Add(wx.StaticLine(self), 0, wx.EXPAND)
        outer.Add(self.head, 0, wx.EXPAND)
        outer.Add(self.body, 1, wx.EXPAND)
        self.SetSizer(outer)
        self.Bind(wx.EVT_CHAR_HOOK, self.on_key)
        self.body.Bind(wx.EVT_CHILD_FOCUS, self.on_child_focus)
        self.render(focus=False)

    @property
    def tab_name(self):
        return "iOS" if self.platform == "ios" else "macOS"

    def on_key(self, e):
        if (e.GetKeyCode() == wx.WXK_LEFT and e.AltDown()) or \
                (e.GetKeyCode() == wx.WXK_BACK and not isinstance(self.FindFocus(), wx.TextCtrl)):
            self.go_back()
        else:
            e.Skip()

    def on_child_focus(self, e):
        key = self._key_of(self.FindFocus())
        if key:
            self.focus_key = key
        e.Skip()

    def _key_of(self, w):
        return next((k for c, k in self.keyed if c is w), None) if w else None

    def open(self, route):
        self.came_from.append(self._key_of(self.FindFocus()))
        self.stack.append(route)
        self.render()

    def go_back(self):
        if self.stack:
            self.stack.pop()
            key = self.came_from.pop() if self.came_from else None
            self.render(focus_key=key)

    def title(self, route):
        kind = route[0]
        if kind == "language":
            return L.language_name(route[1])
        if kind == "voice":
            it = self.lib.item(route[1])
            return it.title if it else "Voice"
        if kind == "search":
            return "Search: %s" % route[1]
        return {"diagnostics": "Diagnostics", "fast": "Speed up the voices", "firmware": "Get the voices",
                "languages": "Voices for other languages", "wanted": "Languages to get"}[kind]

    def update_live(self):
        """A progress tick: new values on the progress bars, new labels on the
        buttons whose step changed."""
        for ctrl, fn in self.live:
            value = fn()
            if not ctrl:
                continue
            if isinstance(ctrl, wx.Gauge):
                if ctrl.GetValue() != value:
                    ctrl.SetValue(value)
            elif ctrl.GetLabel() != value:
                ctrl.SetLabel(value)

    def render(self, focus=True, keep=False, focus_key=None):
        """Rebuild the current page. keep: stay on the control that had focus
        (something changed under it), found again by what it is, not where it
        is; focus_key: focus that control (going back); else focus the page's
        first control."""
        key = focus_key
        if keep:
            had = self.FindFocus()
            key = self._key_of(had)
            if key is None and (had is None or had in (self, self.body, self.frame, self.frame.root)):
                # focus is nowhere: the control was destroyed by a rebuild a moment
                # ago, whose own focusing hasn't happened yet (two downloads ending
                # together). Without this it went to the page's first control.
                key = self.focus_key
            elif key is None:
                focus = False  # focus is outside the page (the tabs): leave it there
        self.keyed = []
        self.body.Freeze()
        self.body.DestroyChildren()
        page = Page(self.body)
        route = self.stack[-1] if self.stack else None
        {None: self.root_page, "language": self.language_page, "voice": self.voice_page,
         "search": self.search_page, "diagnostics": self.diagnostics_page, "fast": self.fast_page,
         "firmware": self.firmware_page, "languages": self.languages_page,
         "wanted": self.wanted_page}[route[0] if route else None](page, route)
        other_jobs(page, self.lib.jobs)
        page.finish()
        self.live = page.live
        self.keyed = page.keyed
        self.body.FitInside()
        self.body.Thaw()
        self.back.Enable(bool(self.stack))
        self.head.Show(not self.stack)
        v = L.VERSION.get(self.version)
        crumbs = [self.tab_name, "%s (%s)" % (v.name, v.number) if v else "All versions"] + \
            [self.title(r) for r in self.stack]
        self.where.SetLabel(" › ".join(crumbs))
        self.where.SetName("Current location: " + ", ".join(crumbs))
        self.Layout()
        target = next((c for c, k in page.keyed if k == key), None) if key else None
        if target is None and focus:
            target = page.first if self.stack else self.search
        if target:
            self.focus_key = self._key_of(target)
            # after the toolkit has settled the new page, or GTK's screen
            # readers miss the focus change
            wx.CallAfter(lambda: target and target.SetFocus())

    def refocus(self):
        """Focus the control last focused on this page (after a rebuild that
        destroyed it); False if it isn't here any more."""
        target = next((c for c, k in self.keyed if k == self.focus_key), None) if self.focus_key else None
        if target:
            target.SetFocus()
        return bool(target)

    # ---- pages
    def root_page(self, p, _route):
        lib = self.lib
        versions = lib.versions(self.platform)
        ids = ["all"] + [v.id for v in versions]
        if ids != self.version_ids:  # only when the list changes: setting it again moves a screen reader
            self.version_ids = ids
            self.versions.Set(["All versions"] + ["%s (%s)" % (v.name, v.number) for v in versions])
        if self.version not in ids:
            self.version = "all"
        if self.versions.GetSelection() != ids.index(self.version):
            self.versions.SetSelection(ids.index(self.version))
        shown = [v for v in versions if self.version in ("all", v.id)]
        missing = [v for v in shown if not lib.firmware_ready(v)]
        if missing:
            p.section("Get the voices")
            for v in missing:
                p.text("%s voices need Apple's %s firmware, which Classic Voices takes them from."
                       % (v.name + ("'" if v.name.endswith("s") else "'s"),
                          L.names_text(lib.firmware_missing(v)) or "files"))
            p.button("Get the firmware…", lambda: self.open(("firmware",)))
        items = lib.visible(self.platform, None if self.version == "all" else self.version)
        failed = [(a, b) for a, b in lib.errors]
        if failed:
            p.section("Problems")
            for label, err in failed:
                p.text("%s: %s" % (label, err))
            p.button("Dismiss", lambda: (lib.dismiss_errors(), self.render()))
        langs = sorted({i.language for i in items}, key=L.language_name)
        if langs:
            p.section("Languages")
            for code in langs:
                n = sum(1 for i in items if i.language == code)
                p.button("%s, %d voice%s" % (L.language_name(code), n, "" if n == 1 else "s"),
                         lambda c=code: self.open(("language", c)))
        p.section("More")
        if not missing:
            p.button("Get the firmware…", lambda: self.open(("firmware",)))
        p.button("Voices for other languages", lambda: self.open(("languages",)))
        p.button("Speed up the voices", lambda: self.open(("fast",)))
        p.button("Diagnostics", lambda: self.open(("diagnostics",)))

    def _search(self, q):
        self.query = q
        if q.strip():
            self.open(("search", q))

    def _set_version(self, v):
        self.version = v
        self.render(focus=False)

    def _voice_buttons(self, p, items, group_by_version, where=""):
        """where: the page's subject, leading each group's name, which a
        screen reader says on arriving."""
        lead = where + ", " if where else ""
        if group_by_version:
            for v in L.VERSIONS:
                vs = [i for i in items if i.version is v]
                if vs:
                    p.section(lead + v.name)
                    self._rows(p, vs)
        else:
            p.section(lead + "voices" if lead else "Voices")
            self._rows(p, items)

    def _rows(self, p, items):
        for it in sorted(items, key=lambda i: (i.name.lower(), i.kind)):
            p.button("", lambda i=it: self._press(i), live=lambda i=it: voice_label(self.lib, i), key="voice:" + it.id)
            job = self.lib.job_for(it.id)
            if job:
                job_gauge(p, job, "Downloading " + it.title)

    def _press(self, item):
        if item.state in ("builtin", "installed"):
            self.open(("voice", item.id))
        elif item.state == "needs":
            self.frame.offer_firmware(item)
        else:
            self.lib.toggle_download(item)

    def language_page(self, p, route):
        items = self.lib.visible(self.platform, None if self.version == "all" else self.version, route[1])
        self._voice_buttons(p, items, self.version == "all", L.language_name(route[1]))
        from _classicvoices import languages
        lang = languages.choice_language(route[1])
        here = [i for i in self.lib.items if i.state in ("builtin", "installed")
                and languages.choice_language(i.language) == lang]
        if here:
            p.section("Remove")
            p.button("Remove every %s voice (%d)" % (L.language_name(lang), len(here)),
                     lambda: self.frame.remove_language(lang, len(here)))

    def wanted_page(self, p, _route):
        from _classicvoices import firmware
        p.text("The languages Classic Voices takes from Apple's firmware and offers to download. Voices of a "
               "language you turn off stay until you remove them (on the language's page); turning one back on "
               "brings its voices with the next firmware you get.")
        wanted = self.lib.wanted_languages()
        p.section("Languages")
        for lang in sorted(firmware.ALL_LANGUAGES, key=L.language_name):
            p.check(L.language_name(lang), wanted is None or lang in wanted,
                    lambda on, lang=lang: self.lib.set_language_wanted(lang, on))

    def search_page(self, p, route):
        found = self.lib.search(route[1])
        if not found:
            p.text("No voices match “%s”." % route[1])
        self._voice_buttons(p, found, True, "Found %d" % len(found))

    def voice_page(self, p, route):
        it = self.lib.item(route[1])
        if not it:
            p.text("This voice isn't here any more.")
            return
        frame = self.frame
        p.section("%s, %s" % (it.title, it.version.name))
        p.text("%s, %s voice, %s (%s %s)" % (it.name, it.kind, L.language_name(it.language), it.version.name,
                                             it.version.number))
        text = p.field("Text to speak:", L.sample_text(it.language, it.name))
        # a list, not a slider: a screen reader reads a slider as a percentage of its range
        p.choice("Speaking rate:", [(r, "%d%%" % r) for r in (50, 75, 100, 125, 150, 175, 200, 250, 300, 400)],
                 frame.rate, lambda v: setattr(frame, "rate", v))
        p.button("&Speak", lambda: frame.speak(it, text.GetValue(), frame.rate))
        p.button("S&top", frame.stop_speaking)
        p.check("Use as a system voice (for %s)" % ("Windows programs and NVDA" if system.IS_WIN
                                                    else "Speech Dispatcher and Orca"),
                not self.lib.is_hidden(it), lambda on: frame.set_system_voice(it, on))
        if it.removable:
            p.button("Remove this voice (%s)" % L.size_text(it.size), lambda: frame.remove(it))
            compact = self.lib.item(it.id + "Compact") if it.version.platform == "mac" and it.kind == "premium" else None
            if compact:
                p.button("Remove the premium voice, keep %s (frees %s)" % (compact.title, L.size_text(it.size)),
                         lambda: frame.remove_premium(it))
        if it.state in ("builtin", "installed") and not self.lib.is_hidden(it):
            self._language_rows(p, it)

    def languages_page(self, p, _route):
        p.text("When a screen reader switches language in the middle of what it reads (NVDA's automatic language "
               "switching, SSML in SAPI programs, Orca), the voice reading hands that part to a voice of the other "
               "language: by default the one nearest it, from the same release where there is one. Choose a voice "
               "here to use for a language whichever voice is reading; each voice's page can choose differently "
               "for that voice.")
        self._language_rows(p, None)

    def _language_rows(self, p, voice):
        """Automatic language switching: per language, the voice for text in
        it while `voice` reads (None: whichever voice reads), as kept in
        languages.txt."""
        from _classicvoices import languages
        lib = self.lib
        here = [i for i in lib.items if i.state in ("builtin", "installed") and not lib.is_hidden(i)]
        by = {}
        for i in here:
            by.setdefault(languages.choice_language(i.language), []).append(i)
        own = languages.choice_language(voice.language) if voice else None
        own_base = languages.base(voice.language) if voice else None
        # a voice reads its own language (any region) itself
        langs = sorted((l for l in by if l != own and languages.base(l) != own_base), key=L.language_name)
        if voice:
            p.section("Other languages")
        if not langs:
            p.text("There are no voices for other languages yet.")
            return
        if voice:
            p.text("When %s reads text in another language, it hands it to these voices." % voice.name)
        names = {i.id: "%s, %s" % (i.title, i.version.name) for i in here}
        choices = languages.load(system.data_root())
        # "-": the voice reading keeps reading it; "*" as the language: every language
        every = {l: k for l, k in choices.get("*", {}).items() if k in names or k == "-"}
        mine = {l: k for l, k in choices.get(voice.id, {}).items() if k in names or k == "-"} if voice else every
        every_off = every.get("*") == "-"
        keep = "Keep the voice reading"

        def switch(on):
            languages.set_choice(system.data_root(), "*", None if on else "-", voice.id if voice else None)
            self.render(keep=True)
        if voice and every_off:
            p.text("Switching voices when the text changes language is off for every voice (on the Languages "
                   "page); a voice chosen below for a language still reads it.")
        else:
            p.check("Switch voices when the text changes language" + (" (while %s reads)" % voice.name if voice else ""),
                    mine.get("*") != "-", switch)
        auto = self.frame.auto_voices(voice.id) if voice else {}
        for lang in langs:
            if not voice:
                first = "Off: keep the voice reading" if every_off else "Automatic: the voice nearest the one reading"
            elif lang in every or every_off:
                first = "As for every voice: " + (keep if every.get(lang, "-") == "-" else names[every[lang]])
            else:
                first = "Automatic: " + names.get(auto.get(lang), "none")
            opts = [("", first), ("-", keep)] + [(i.id, names[i.id]) for i in sorted(by[lang], key=lambda i: (
                [v.id for v in L.VERSIONS].index(i.version.id), i.name.lower(), i.kind))]
            p.choice(L.language_name(lang) + ":", opts, mine.get(lang, ""),
                     lambda key, lang=lang: languages.set_choice(system.data_root(), lang, key or None,
                                                                 voice.id if voice else None))

    def firmware_page(self, p, _route):
        lib = self.lib
        needed = []
        for v in lib.versions(self.platform):
            for f in v.firmware:
                # either iOS 7 firmware: 7.0.6 is offered, and 7.1.2 once imported
                for k in [L.suggested(f)] + (["ios7"] if f == L.IOS7 and lib.firmware_imported("ios7") else []):
                    if k not in needed:
                        needed.append(k)
        p.text("Classic Voices takes the voices out of Apple's iPhone firmware: each firmware only for its "
               "speech files. It can stream them from Apple, or read a firmware (.ipsw) file you have.")
        p.section("From Apple")
        from _classicvoices import remote

        def label(f):
            job = lib.job_for("fw:" + f)
            if job:
                return "Stop getting %s, %s" % (L.FIRMWARE_NAMES[f], job.message)
            if lib.firmware_imported(f):
                return "%s: imported. Get it again" % L.FIRMWARE_NAMES[f]
            return "Get %s from Apple (streams about %s), for %s" % (
                L.FIRMWARE_NAMES[f], L.size_text(remote.FIRMWARE[f][2]), remote.PURPOSE[f])
        for f in needed:
            p.button("", lambda f=f: lib.import_firmware(f), live=lambda f=f: label(f), key="fw:" + f)
            job = lib.job_for("fw:" + f)
            if job:
                job_gauge(p, job, "Getting " + L.FIRMWARE_NAMES[f])
        wanted = lib.wanted_languages()
        from _classicvoices import firmware as fw
        p.section("Languages")
        p.button("Languages to get: %s" % ("all" if wanted is None else "%d of %d" % (len(wanted), len(fw.ALL_LANGUAGES))),
                 lambda: self.open(("wanted",)))
        p.section("From a file")
        p.button("Import a firmware file (.ipsw)…", self.frame.import_ipsw)
        p.button("Import an extracted firmware folder…", self.frame.import_folder)
        for j in [j for j in lib.jobs if j.key.startswith("file:")]:
            p.button("", lambda j=j: lib.cancel(j.key), live=lambda j=j: "Stop importing %s, %s" % (j.label, j.message),
                     key=j.key)
            job_gauge(p, j, "Importing " + j.label)

    def fast_page(self, p, _route):
        lib = self.lib
        p.text("On their own, the engines run on an emulated iPhone processor. Fast code is that same engine "
               "translated into this computer's own code, from your own firmware: it sounds identical and starts "
               "speaking several times sooner. Classic Voices builds it by itself after each firmware import. "
               "It takes a few minutes per firmware" +
               ((", and the first time it downloads the free llvm-mingw C compiler from GitHub (186 MB)."
                 if system.arm_windows() else
                 ", and the first time it downloads the free Zig C compiler from ziglang.org (about 100 MB).")
                if system.IS_WIN else ", with your clang, or the Zig C compiler it downloads (about 55 MB)."))
        imported = [f for f in L.FIRMWARE_NAMES if lib.firmware_imported(f)]
        p.section("Fast code")
        if not imported:
            p.text("Import firmware first.")
        for f in imported:
            p.text("%s: %s" % (L.FIRMWARE_NAMES[f], "fast code ready" if lib.fast_code_current(f)
                               else "fast code from an older version" if lib.fast_code(f) else "no fast code yet"))
        job = lib.job_for("fast")
        if job:
            p.button("", lambda: lib.cancel("fast"), live=lambda: "Stop building, %s" % job.message, key="fast")
            job_gauge(p, job, "Building fast code")
        elif imported and system.recompiler():
            todo = [f for f in imported if not lib.fast_code_current(f)]
            if todo and all(lib.fast_code(f) for f in todo):
                p.button("Update the fast code",
                         lambda: (system.set_setting("FastCodeAuto", True), lib.build_fast_code(None)))
            elif todo:
                p.button("Build fast code for %s" % ("the new firmware" if len(todo) < len(imported)
                                                     else "all imported firmware"),
                         lambda: (system.set_setting("FastCodeAuto", True), lib.build_fast_code(None)))
            else:
                p.button("Build it again", lambda: lib.build_fast_code(imported))

    def diagnostics_page(self, p, _route):
        report = L.diagnostics(self.lib)
        t = p.field("Report:", report, multiline=True)
        t.SetMinSize((-1, 260))
        t.SetEditable(False)

        def copy():
            if wx.TheClipboard.Open():
                wx.TheClipboard.SetData(wx.TextDataObject(report))
                wx.TheClipboard.Close()
        p.button("&Copy the report", copy)


class SettingsPanel(wx.ScrolledWindow):
    def __init__(self, parent, frame):
        super().__init__(parent, style=wx.VSCROLL)
        self.frame = frame
        self.SetScrollRate(0, 20)
        self.render()

    def set_newer(self, on):
        system.set_setting("NewerPronunciation", on)
        if on:
            self.frame.lib.get_pronunciation()

    def render(self):
        self.Freeze()
        self.DestroyChildren()
        p = Page(self)
        p.section("Speech")
        p.check("Apple’s pronunciation rules", system.get_setting("PronunciationRules"),
                lambda v: system.set_setting("PronunciationRules", v))
        p.check("Newer pronunciation rules", system.get_setting("NewerPronunciation"), self.set_newer)
        p.check("Compact voice for pitch changes", system.get_setting("PitchFallback"),
                lambda v: system.set_setting("PitchFallback", v))
        p.text("Apple’s rules are the text fixes each voice’s own iOS or Mac OS X release made "
               "before the engine read it (“iOS”, “OS X” …). The newer rules add Apple’s later "
               "pronunciation dictionary and rules to every voice; turning them on downloads them "
               "from Apple (about 5 MB). Enhanced and premium voices can’t change their pitch; iOS "
               "used the compact voice for those words, and so does Classic Voices unless this is off.")
        if system.IS_WIN:
            p.section("Windows programs")
            n = system.system_voice_count()
            p.text("%d voices are available to programs that use Windows’ speech (SAPI 5): JAWS, NVDA’s "
                   "SAPI 5 option, Balabolka and others." % n)
            p.button("Update the voices for Windows programs (asks for administrator permission)",
                     self.frame.update_system)
            nv = system.nvda_data_root()
            if nv and os.path.normcase(nv) != os.path.normcase(system.data_root()):
                p.button("Copy the voices from the NVDA add-on", lambda: self.frame.lib.copy_from(nv))
        else:
            p.section("Speech Dispatcher")
            p.text("The Speech Dispatcher module is %s." % (
                "installed: Orca and other programs can use the voices" if system.speechd_module_installed()
                else "not installed (run install.sh again)"))
        p.section("Data")
        p.text("The voices are in %s" % system.data_root())
        p.button("Open the data folder", lambda: system.open_folder(system.data_root()))
        p.finish()
        self.FitInside()
        self.Thaw()


class Frame(wx.Frame):
    def __init__(self):
        super().__init__(None, title=APP, size=(720, 640))
        self.rate = 100
        self.player = None
        self.lib = L.Library(lambda: wx.CallAfter(self.changed), lambda: wx.CallAfter(self.changed, progress=True))
        # A frame's controls belong in a panel: it handles Tab and gives focus
        # back when the window is activated again.
        self.root = wx.Panel(self)
        self.book = wx.Notebook(self.root)
        box = wx.BoxSizer(wx.VERTICAL)
        box.Add(self.book, 1, wx.EXPAND)
        self.root.SetSizer(box)
        self._last_focus = None
        self.Bind(wx.EVT_ACTIVATE, self.on_activate)
        self.Bind(wx.EVT_CHILD_FOCUS, self.on_child_focus)
        # Switching back to the window (Alt+Tab) can leave focus on the frame
        # itself, where Tab does nothing: hand it on.
        self.Bind(wx.EVT_SET_FOCUS, self.on_frame_focus)
        self.root.Bind(wx.EVT_SET_FOCUS, self.on_frame_focus)
        self.tabs = [Browser(self.book, self, "ios"), Browser(self.book, self, "mac")]
        self.book.AddPage(self.tabs[0], "iOS")
        self.book.AddPage(self.tabs[1], "macOS")
        self.settings = SettingsPanel(self.book, self)
        self.book.AddPage(self.settings, "Settings")
        self.book.Bind(wx.EVT_NOTEBOOK_PAGE_CHANGED, self.on_page)
        self.Bind(wx.EVT_CHAR_HOOK, self.on_key)
        self.status = self.CreateStatusBar()
        self.Bind(wx.EVT_CLOSE, self.on_close)
        self.updating = False  # the setup program may close the app without asking
        self.Centre()

    def on_key(self, e):
        """Ctrl+Tab / Ctrl+Shift+Tab change tabs everywhere (GTK's own keys
        are Ctrl+Page Down / Page Up)."""
        if e.GetKeyCode() == wx.WXK_TAB and e.ControlDown():
            self.book.AdvanceSelection(not e.ShiftDown())
            self.book.SetFocus()
        else:
            e.Skip()

    def on_page(self, e):
        """Only the page on screen is kept up to date (a page rebuilt while
        hidden would let Tab wander into it), so bring the new one up to date."""
        page = self.book.GetPage(e.GetSelection())
        if isinstance(page, Browser):
            page.render(focus=False, keep=False)
        elif page is self.settings:
            self.settings.render()
        e.Skip()

    def on_child_focus(self, e):
        w = self.FindFocus()
        if w and w not in (self, self.root):
            self._last_focus = w
        e.Skip()

    def _restore_focus(self):
        target = self._last_focus
        try:
            if target and target.IsShownOnScreen():
                return target.SetFocus()
        except RuntimeError:  # the control was destroyed (the page was rebuilt)
            pass
        tab = self.book.GetCurrentPage()
        if not (isinstance(tab, Browser) and tab.refocus()):
            self.book.SetFocus()

    def on_frame_focus(self, e):
        wx.CallAfter(self._restore_focus)
        e.Skip()

    def on_activate(self, e):
        if e.GetActive():
            wx.CallAfter(self._restore_focus)
        e.Skip()

    def changed(self, progress=False):
        """The library changed: redraw the current tab, keeping focus where it
        is. A progress tick (each 1% of a job) only updates the progress bars
        and the labels that show a job's step."""
        if progress and not self.lib.jobs:
            return
        tab = self.book.GetCurrentPage()
        if isinstance(tab, Browser):
            tab.update_live() if progress else tab.render(keep=True)
        job = self.lib.jobs[0] if self.lib.jobs else None
        self.status.SetStatusText("%s%s" % (job.message, percent(job)) if job else "")
        while self.lib.finished:
            self.status.SetStatusText("Done: " + self.lib.finished.pop(0))
        if not progress and tab is self.settings:
            self.settings.render()

    def speak(self, item, text, rate):
        try:
            if self.player is None:
                self.player = system.make_player()
            self.player.speak(item.id, text, rate, item.language)
        except Exception as e:
            wx.MessageBox("Classic Voices can't play the voice here: %s" % e, APP, wx.OK | wx.ICON_ERROR, self)

    def stop_speaking(self):
        if self.player:
            self.player.stop()

    def set_system_voice(self, item, on):
        system.set_hidden(item.id, not on)
        if system.IS_WIN:
            self.update_system()
        self.changed()

    def offer_firmware(self, item):
        """A voice whose firmware isn't here: offer to get it, then the voice."""
        from _classicvoices import remote
        size = sum(remote.FIRMWARE[k][2] for k in item.needs)
        if wx.MessageBox("%s needs Apple's %s firmware, which isn't here yet. Get it from Apple now (streams "
                         "about %s), then download %s (%s)?"
                         % (item.title, L.names_text(item.needs), L.size_text(size), item.name,
                            L.size_text(item.size)), APP, wx.YES_NO | wx.ICON_QUESTION, self) == wx.YES:
            self.lib.get_with_firmware(item)

    def auto_voices(self, key):
        """system.auto_voices, kept until the voices change."""
        stamp = (key, tuple(sorted(i.id for i in self.lib.items if i.state in ("builtin", "installed"))),
                 frozenset(system.hidden_keys()))
        if getattr(self, "_auto", (None, None))[0] != stamp:
            self._auto = (stamp, system.auto_voices(key))
        return self._auto[1]

    def remove_premium(self, item):
        if wx.MessageBox("Remove %s and keep the compact voice?" % item.title, APP,
                         wx.YES_NO | wx.NO_DEFAULT | wx.ICON_QUESTION, self) != wx.YES:
            return
        self.stop_speaking()
        self.lib.remove_premium(item)
        tab = self.book.GetCurrentPage()
        if isinstance(tab, Browser):
            tab.go_back()
        if system.IS_WIN:
            self.update_system()

    def remove_language(self, lang, count):
        if wx.MessageBox("Remove every %s voice (%d)? Firmware you get from now on leaves %s out; turn it back on "
                         "under Get the voices, Languages to get." % (L.language_name(lang), count,
                                                                       L.language_name(lang)),
                         APP, wx.YES_NO | wx.NO_DEFAULT | wx.ICON_QUESTION, self) != wx.YES:
            return
        self.stop_speaking()
        self.lib.remove_language(lang)
        tab = self.book.GetCurrentPage()
        if isinstance(tab, Browser):
            tab.go_back()
        if system.IS_WIN:
            self.update_system()

    def remove(self, item):
        if wx.MessageBox("Remove %s? You can download it again later." % item.title, APP,
                         wx.YES_NO | wx.NO_DEFAULT | wx.ICON_QUESTION, self) != wx.YES:
            return
        self.stop_speaking()
        self.lib.remove(item)
        tab = self.book.GetCurrentPage()
        if isinstance(tab, Browser):
            tab.go_back()
        if system.IS_WIN:
            self.update_system()

    def update_system(self):
        ok, msg = system.update_system_voices()
        self.status.SetStatusText(msg)
        if not ok:
            wx.MessageBox(msg, APP, wx.OK | wx.ICON_WARNING, self)
        if self.book.GetCurrentPage() is self.settings:
            self.settings.render()

    def import_ipsw(self):
        with wx.FileDialog(self, "Choose an iPhone firmware file",
                           wildcard="iPhone firmware (*.ipsw)|*.ipsw|All files|*",
                           style=wx.FD_OPEN | wx.FD_FILE_MUST_EXIST) as d:
            if d.ShowModal() == wx.ID_OK:
                self.lib.import_file(d.GetPath())

    def import_folder(self):
        with wx.DirDialog(self, "Choose an extracted firmware folder", style=wx.DD_DIR_MUST_EXIST) as d:
            if d.ShowModal() == wx.ID_OK:
                self.lib.import_file(d.GetPath())

    def on_close(self, e):
        if self.lib.jobs and e.CanVeto() and not self.updating:
            if wx.MessageBox("Something is still downloading or importing. Quit anyway? It will stop.", APP,
                             wx.YES_NO | wx.NO_DEFAULT, self) != wx.YES:
                e.Veto()
                return
        self.stop_speaking()
        e.Skip()


def main():
    if not system.IS_WIN:
        import sys
        sys.argv[0] = "classicvoices"  # GTK names the application (for screen readers) after it
    app = wx.App()
    app.SetAppName("classicvoices")
    app.SetAppDisplayName(APP)
    f = Frame()
    f.Show()
    threading.Thread(target=check_for_update, args=(f,), name="update", daemon=True).start()
    app.MainLoop()


def check_for_update(frame):
    """Once a day at most: a newer Classic Voices is offered once, to install
    (or, when this copy can't update itself, with its release page)."""
    from _classicvoices import update
    found = update.newer(system.data_root())
    if found:
        wx.CallAfter(offer_update, frame, *found)
    for _ in range(6):  # the setup that opened this version may still be ending
        if not system.update_asset() or not update.remove_downloads(system.update_asset()):
            break
        time.sleep(10)


def offer_update(frame, version, url, assets):
    if not frame:
        return
    name = system.update_asset()
    asset = assets.get(name) if name else None
    if not asset:
        if wx.MessageBox("Classic Voices %s is available. Open its download page?" % version, APP,
                         wx.YES_NO | wx.ICON_INFORMATION, frame) == wx.YES:
            webbrowser.open(url)
        return
    if wx.MessageBox("Classic Voices %s is available. Download and install it now? Classic Voices opens again "
                     "when it's done%s." % (version, " (Windows asks for permission first)" if system.IS_WIN else ""),
                     APP, wx.YES_NO | wx.ICON_INFORMATION, frame) != wx.YES:
        return

    def ready(path):
        wx.CallAfter(install_update, frame, path, url)
    frame.lib.get_update(version, asset, ready)


def install_update(frame, path, url):
    try:
        setup = system.install_update(path)  # Linux doesn't return: the new app takes over
    except Exception as e:
        if wx.MessageBox("Classic Voices couldn't install the update: %s. Open its download page?" % e, APP,
                         wx.YES_NO | wx.ICON_ERROR, frame) == wx.YES:
            webbrowser.open(url)
        return
    # Windows: once permission is given the setup closes this app (it never
    # asks: frame.updating), replaces it and opens the new one; until then
    # this app stays, so a No leaves it running
    frame.updating = True
    threading.Thread(target=lambda: wx.CallAfter(update_ended, frame, setup.wait(), path, url),
                     name="update", daemon=True).start()


def update_ended(frame, code, path, url):
    """The setup ended and this app is still here: it was installed but didn't
    close this one, or permission wasn't given, or it failed."""
    if not frame:
        return
    frame.updating = False
    if code == 0:
        frame.Close(True)  # the new version is open
        return
    if wx.MessageBox("Classic Voices wasn't updated: Windows didn't give the setup permission, or it stopped "
                     "(code %d). Try again?" % code, APP, wx.YES_NO | wx.ICON_WARNING, frame) == wx.YES:
        install_update(frame, path, url)
