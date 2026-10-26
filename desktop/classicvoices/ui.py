"""The window: an iOS tab, a macOS tab and Settings, laid out like the iOS
and Mac apps (pages of buttons you drill into, each saying what it does
before you press it), with native controls only so screen readers read
everything."""
import os

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

    def _parent(self):
        return self.group.GetStaticBox() if self.group else self.panel

    def _add(self, ctrl, expand=True):
        (self.group or self.sizer).Add(ctrl, 0, (wx.EXPAND if expand else 0) | wx.ALL, 4)
        if self.first is None and ctrl.AcceptsFocus():
            self.first = ctrl
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

    def button(self, label, handler, enabled=True, live=None):
        """live: a function giving the label as progress changes."""
        b = wx.Button(self._parent(), label=live() if live else label, style=wx.BU_LEFT)
        b.Bind(wx.EVT_BUTTON, lambda e: handler())
        b.Enable(enabled)
        if live:
            self.live.append((b, live))
        return self._add(b)

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
    p.gauge(name, lambda: job.percent or 0)


def voice_label(lib, item):
    """What a voice's button says: what pressing it does comes first when it
    doesn't simply open the voice (as in the iOS app)."""
    job = lib.job_for(item.id)
    if job:
        return "Stop downloading %s, %s" % (item.title, L.size_text(job.size))
    if item.state == "available":
        return "Download %s, %s" % (item.title, L.size_text(item.size))
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
        self.came_from = []  # per page opened: the index of the control that opened it, to return to
        self.live = []
        top = wx.BoxSizer(wx.HORIZONTAL)
        self.back = wx.Button(self, label="&Back")
        self.back.Bind(wx.EVT_BUTTON, lambda e: self.go_back())
        top.Add(self.back, 0, wx.ALL, 4)
        self.where = wx.StaticText(self, label="")
        top.Add(self.where, 1, wx.ALIGN_CENTER_VERTICAL | wx.ALL, 4)
        self.body = wx.ScrolledWindow(self, style=wx.VSCROLL)
        self.body.SetScrollRate(0, 20)
        outer = wx.BoxSizer(wx.VERTICAL)
        outer.Add(top, 0, wx.EXPAND)
        outer.Add(wx.StaticLine(self), 0, wx.EXPAND)
        outer.Add(self.body, 1, wx.EXPAND)
        self.SetSizer(outer)
        self.Bind(wx.EVT_CHAR_HOOK, self.on_key)
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

    def open(self, route):
        ctrls = list(self._controls(self.body))
        f = self.FindFocus()
        self.came_from.append(ctrls.index(f) if f in ctrls else None)
        self.stack.append(route)
        self.render()

    def go_back(self):
        if self.stack:
            self.stack.pop()
            index = self.came_from.pop() if self.came_from else None
            self.render(focus_index=index)

    def title(self, route):
        kind = route[0]
        if kind == "language":
            return L.language_name(route[1])
        if kind == "voice":
            it = self.lib.item(route[1])
            return it.title if it else "Voice"
        if kind == "search":
            return "Search: %s" % route[1]
        return {"diagnostics": "Diagnostics", "fast": "Speed up the voices", "firmware": "Get the voices"}[kind]

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

    def render(self, focus=True, keep=False, focus_index=None):
        """Rebuild the current page. keep: stay on the control that had focus
        (something changed under it); focus_index: focus that control (going
        back); else focus the page's first control."""
        had = self.FindFocus()
        had_index = focus_index
        if keep and had:
            ctrls = list(self._controls(self.body))
            had_index = ctrls.index(had) if had in ctrls else None
        self.body.Freeze()
        self.body.DestroyChildren()
        page = Page(self.body)
        route = self.stack[-1] if self.stack else None
        {None: self.root_page, "language": self.language_page, "voice": self.voice_page,
         "search": self.search_page, "diagnostics": self.diagnostics_page, "fast": self.fast_page,
         "firmware": self.firmware_page}[route[0] if route else None](page, route)
        page.finish()
        self.live = page.live
        self.body.FitInside()
        self.body.Thaw()
        self.back.Enable(bool(self.stack))
        v = L.VERSION.get(self.version)
        crumbs = [self.tab_name, "%s (%s)" % (v.name, v.number) if v else "All versions"] + \
            [self.title(r) for r in self.stack]
        self.where.SetLabel(" › ".join(crumbs))
        self.where.SetName("Current location: " + ", ".join(crumbs))
        self.Layout()
        target = None
        if had_index is not None:
            ctrls = list(self._controls(self.body))
            if had_index < len(ctrls):
                target = ctrls[had_index]
        if target is None and focus:
            target = page.first
        if target:
            # after the toolkit has settled the new page, or GTK's screen
            # readers miss the focus change
            wx.CallAfter(lambda: target and target.SetFocus())

    def _controls(self, w):
        for c in w.GetChildren():
            if isinstance(c, wx.StaticBox):
                yield from self._controls(c)
            elif c.AcceptsFocus():
                yield c

    # ---- pages
    def root_page(self, p, _route):
        lib = self.lib
        search = p.field("Search all voices:", self.query)
        search.SetWindowStyleFlag(search.GetWindowStyleFlag() | wx.TE_PROCESS_ENTER)
        search.Bind(wx.EVT_TEXT_ENTER, lambda e: self._search(search.GetValue()))
        versions = lib.versions(self.platform)
        p.choice("Version:", [("all", "All versions")] + [(v.id, "%s (%s)" % (v.name, v.number)) for v in versions],
                 self.version, self._set_version)
        shown = [v for v in versions if self.version in ("all", v.id)]
        missing = [v for v in shown if not lib.firmware_ready(v)]
        if missing:
            p.section("Get the voices")
            for v in missing:
                need = [L.FIRMWARE_NAMES[f] for f in v.firmware if not lib.firmware_imported(f)]
                p.text("%s voices need Apple's %s firmware, which Classic Voices takes them from."
                       % (v.name + ("'" if v.name.endswith("s") else "'s"), " and ".join(need) or "files"))
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
            p.button("", lambda i=it: self._press(i), live=lambda i=it: voice_label(self.lib, i))
            job = self.lib.job_for(it.id)
            if job:
                job_gauge(p, job, "Downloading " + it.title)

    def _press(self, item):
        if item.state in ("builtin", "installed"):
            self.open(("voice", item.id))
        else:
            self.lib.toggle_download(item)

    def language_page(self, p, route):
        items = self.lib.visible(self.platform, None if self.version == "all" else self.version, route[1])
        self._voice_buttons(p, items, self.version == "all", L.language_name(route[1]))

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

    def firmware_page(self, p, _route):
        lib = self.lib
        needed = []
        for v in lib.versions(self.platform):
            for f in v.firmware:
                if f not in needed:
                    needed.append(f)
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
            p.button("", lambda f=f: lib.import_firmware(f), live=lambda f=f: label(f))
            job = lib.job_for("fw:" + f)
            if job:
                job_gauge(p, job, "Getting " + L.FIRMWARE_NAMES[f])
        p.section("From a file")
        p.button("Import a firmware file (.ipsw)…", self.frame.import_ipsw)
        p.button("Import an extracted firmware folder…", self.frame.import_folder)
        for j in [j for j in lib.jobs if j.key.startswith("file:")]:
            p.button("", lambda j=j: lib.cancel(j.key), live=lambda j=j: "Stop importing %s, %s" % (j.label, j.message))
            job_gauge(p, j, "Importing " + j.label)

    def fast_page(self, p, _route):
        lib = self.lib
        p.text("On their own, the engines run on an emulated iPhone processor. Fast code is that same engine "
               "translated into this computer's own code, from your own firmware: it sounds identical and starts "
               "speaking several times sooner. Classic Voices builds it by itself after each firmware import. "
               "It takes a few minutes per firmware" +
               (", and the first time it downloads the free Zig C compiler from ziglang.org (about 100 MB)."
                if system.IS_WIN else ", with your clang, or the Zig C compiler it downloads (about 55 MB)."))
        imported = [f for f in L.FIRMWARE_NAMES if lib.firmware_imported(f)]
        p.section("Fast code")
        if not imported:
            p.text("Import firmware first.")
        for f in imported:
            p.text("%s: %s" % (L.FIRMWARE_NAMES[f], "fast code ready" if lib.fast_code(f) else "no fast code yet"))
        job = lib.job_for("fast")
        if job:
            p.button("", lambda: lib.cancel("fast"), live=lambda: "Stop building, %s" % job.message)
            job_gauge(p, job, "Building fast code")
        elif imported and system.recompiler():
            todo = [f for f in imported if not lib.fast_code(f)]
            if todo:
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

    def render(self):
        self.Freeze()
        self.DestroyChildren()
        p = Page(self)
        p.section("Speech")
        p.check("Apple’s pronunciation rules", system.get_setting("PronunciationRules"),
                lambda v: system.set_setting("PronunciationRules", v))
        p.check("Compact voice for pitch changes", system.get_setting("PitchFallback"),
                lambda v: system.set_setting("PitchFallback", v))
        p.text("Apple’s rules are the text fixes iOS made before the engine read it (“iOS”, "
               "“OS X” …). Enhanced and premium voices can’t change their pitch; iOS used the "
               "compact voice for those words, and so does Classic Voices unless this is off.")
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
            if not target or not target.IsShownOnScreen():
                target = self.book
        except RuntimeError:  # the control was destroyed (the page was rebuilt)
            target = self.book
        target.SetFocus()

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
        if self.lib.jobs and e.CanVeto():
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
    app.MainLoop()
