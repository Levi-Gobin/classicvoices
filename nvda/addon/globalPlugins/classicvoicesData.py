# Classic Voices data manager: NVDA menu -> Tools -> Classic Voices data.
# Gets the speech files out of Apple's iPhone firmware (streamed from Apple,
# or from an IPSW the user has), downloads voices from Apple's servers once
# the matching engine is in place, and then builds the fast engine code on
# this computer. All slow work runs on a background thread, one job at a
# time; the dialog stays usable and announces progress.
import os
import threading

import globalPluginHandler
import globalVars
import gui
import languageHandler
import ui
import wx
from gui import nvdaControls
from logHandler import log

from synthDrivers._classicvoices import download as cv_download
from synthDrivers._classicvoices import fastcode as cv_fastcode
from synthDrivers._classicvoices import firmware as cv_firmware
from synthDrivers._classicvoices import languages as cv_languages
from synthDrivers._classicvoices import lib as cv_lib
from synthDrivers._classicvoices import paths as cv_paths
from synthDrivers._classicvoices import remote as cv_remote
from synthDrivers._classicvoices import voices as cv_voices

import addonHandler
try:
    addonHandler.initTranslation()
except Exception:
    pass


BUILDS = (("ios3", "iPhone OS 3.1.3"), ("ios4", "iOS 4.3.5"), ("ios5", "iOS 5.1.1"), ("ios6", "iOS 6.1.6"),
          ("ios70", "iOS 7.0.6"), ("ios7", "iOS 7.1.2 (engine only)"), ("ios10", "iOS 10.3.4 (engine only)"))

FIRMWARE_NAMES = {v[0]: v[2] for v in cv_firmware.SUPPORTED.values()}

# One job at a time, also while the dialog is closed (fast code builds on in the background).
_work = threading.Lock()
# What that job is doing now, for a dialog opened meanwhile: (message, fraction or None).
_now = ["", None]


def _has_fast_code(build):
    """Fast code is there, from this version or an older one."""
    return cv_fastcode.has_fast_code(cv_paths.native_root(), build)


def _fast_code_current(build):
    return cv_fastcode.up_to_date(cv_paths.native_root(), build)


def _fast_code_missing():
    """Imported engines without fast code, or with an older version's."""
    vr = cv_paths.voices_root()
    return [b for b, _label in BUILDS if cv_voices.has_engine(vr, b) and not _fast_code_current(b)]


def _fast_code_outdated():
    """Imported engines whose fast code an older version built."""
    return [b for b in _fast_code_missing() if _has_fast_code(b)]


def _engines_in_use():
    """The engines speaking NVDA's Classic Voices voice now, likeliest first:
    their fast code is built first."""
    import synthDriverHandler
    s = synthDriverHandler.getSynth()
    key = getattr(s, "voice", "") if s and s.name == "classicvoices" else ""
    era = (key or "").split("/")[0]
    if era == cv_voices.SIRI:
        return [cv_voices.SIRI_ENGINE, "ios70"]
    if era in cv_voices.MAC_ENGINES:
        if era == "mac109":  # compacts on an iOS 7 engine, premium voices on iOS 10's
            return list(cv_voices.IOS7) if key.endswith("Compact") else ["ios10"]
        return list(cv_voices.MAC_ENGINES[era])
    return [era[:-1] if era.endswith("e") else era] if era else []


def _build_fast_code(progress, todo=None):
    """Builds fast engine code for every imported engine without it (or todo):
    a few minutes each, and the compiler (Zig, about 100 MB; llvm-mingw, 186 MB,
    on Windows on Arm) is downloaded the first time. Each step has its own
    progress. Returns whether anything was built."""
    if todo is None:
        todo = _fast_code_missing()
    if not todo or not cv_fastcode.recompiler():
        return False
    try:
        first = _engines_in_use()
    except Exception:
        first = []
    todo = sorted(todo, key=lambda b: first.index(b) if b in first else len(first))  # the voice in use first
    cc = cv_fastcode.find_compiler() or cv_fastcode.download_compiler(progress)
    labels = dict(BUILDS)
    for i, b in enumerate(todo):
        def step(message, fraction, i=i, b=b):
            # Translators: a fast code build step; %(step)s is what it does, %(firmware)s the firmware,
            # then which of how many firmware this is.
            progress(message and _("%(step)s (%(firmware)s, %(n)d of %(of)d)") % {
                "step": message, "firmware": labels.get(b, b), "n": i + 1, "of": len(todo)}, fraction)
        cv_fastcode.build(b, cv_paths.voices_root(), cv_paths.native_root(), cc, step)
    return True


def _status_lines():
    vr, dr = cv_paths.voices_root(), cv_paths.downloads_root()
    lines = []
    for build, label in BUILDS:
        if not cv_voices.has_engine(vr, build):
            lines.append("%s: not imported" % label)
            continue
        lines.append("%s: imported, %s" % (label, "fast engine code ready" if _fast_code_current(build)
                                           else "fast engine code from an older version" if _has_fast_code(build)
                                           else "no fast engine code yet"))
    n = len(cv_voices.scan(vr, dr))
    lines.append("Voices available to NVDA: %d" % n)
    return lines


def _reload_synth():
    """Classic Voices picks up new voices and fast code when it's loaded again."""
    import synthDriverHandler
    s = synthDriverHandler.getSynth()
    if s and s.name == "classicvoices":
        synthDriverHandler.setSynth("classicvoices")


class DataDialog(wx.Dialog):
    _instance = None

    def __init__(self, parent):
        super().__init__(parent, title="Classic Voices data")
        DataDialog._instance = self
        self._busy = self._building = False
        main = wx.BoxSizer(wx.VERTICAL)
        # Translators: label of the firmware/voices status list.
        main.Add(wx.StaticText(self, label=_("&Status:")), flag=wx.ALL, border=5)
        self.status = wx.ListBox(self, size=(480, 150))
        main.Add(self.status, flag=wx.EXPAND | wx.ALL, border=5)
        # Translators: label of the downloadable-voices list.
        main.Add(wx.StaticText(self, label=_("Downloadable &voices:")),
                 flag=wx.ALL, border=5)
        # CustomCheckListBox is NVDA's accessible checkbox list: check state
        # is announced properly, unlike a bare wx.CheckListBox.
        self.dl = nvdaControls.CustomCheckListBox(self, size=(480, 200))
        main.Add(self.dl, flag=wx.EXPAND | wx.ALL, border=5)
        self.progress = wx.StaticText(self, label="")
        main.Add(self.progress, flag=wx.EXPAND | wx.ALL, border=5)
        # A native progress bar, moved by 1%: NVDA reports it its own way
        # (beeps or speech, as set in its progress bar output).
        # Translators: name of the progress bar of an import or download.
        self.gauge = wx.Gauge(self, range=100, name=_("Progress"))
        main.Add(self.gauge, flag=wx.EXPAND | wx.ALL, border=5)
        btns = wx.BoxSizer(wx.HORIZONTAL)
        # Translators: button that streams every firmware not imported yet from Apple's servers.
        self.appleBtn = wx.Button(self, label=_("Get firmware from &Apple..."))
        self.appleBtn.Bind(wx.EVT_BUTTON, self.onApple)
        btns.Add(self.appleBtn)
        # Translators: button that imports an IPSW firmware file.
        self.importBtn = wx.Button(self, label=_("&Import firmware (IPSW)..."))
        self.importBtn.Bind(wx.EVT_BUTTON, self.onImport)
        btns.Add(self.importBtn)
        # Translators: button that imports an already-extracted firmware folder.
        self.importDirBtn = wx.Button(self, label=_("Import extracted &folder..."))
        self.importDirBtn.Bind(wx.EVT_BUTTON, self.onImportFolder)
        btns.Add(self.importDirBtn)
        # Translators: button that downloads the checked voices.
        self.dlBtn = wx.Button(self, label=_("&Download checked voices"))
        self.dlBtn.Bind(wx.EVT_BUTTON, self.onDownload)
        btns.Add(self.dlBtn)
        # Translators: button that builds fast engine code for the imported firmware.
        self.fastBtn = wx.Button(self, label=_("Build fast &code"))
        self.fastBtn.Bind(wx.EVT_BUTTON, lambda evt: self._start(None))
        btns.Add(self.fastBtn)
        # Translators: button that opens the choice of languages firmware imports take.
        wantBtn = wx.Button(self, label=_("Languages to &get..."))
        wantBtn.Bind(wx.EVT_BUTTON, lambda evt: (WantedDialog(self).ShowModal(), self.refresh()))
        btns.Add(wantBtn)
        # Translators: button that opens the choice of voices for text in other languages.
        langBtn = wx.Button(self, label=_("Voices for other &languages..."))
        langBtn.Bind(wx.EVT_BUTTON, lambda evt: LanguagesDialog(self).ShowModal())
        btns.Add(langBtn)
        # Translators: button that opens the data folder in Explorer.
        openBtn = wx.Button(self, label=_("&Open data folder"))
        openBtn.Bind(wx.EVT_BUTTON, lambda evt: os.startfile(cv_paths.data_root()))
        btns.Add(openBtn)
        closeBtn = wx.Button(self, wx.ID_CLOSE)
        btns.Add(closeBtn)
        self.Bind(wx.EVT_BUTTON, lambda evt: self.Close(), id=wx.ID_CLOSE)
        # Enter and Escape both land on Close.
        closeBtn.SetDefault()
        self.SetAffirmativeId(wx.ID_CLOSE)
        self.SetEscapeId(wx.ID_CLOSE)
        self.Bind(wx.EVT_CLOSE, self.onClose)
        main.Add(btns, flag=wx.ALL, border=5)
        self.SetSizerAndFit(main)
        cv_paths.ensure_dirs()
        self.refresh()
        if _work.locked():  # fast code building in the background, as after an update
            self._busy = self._building = True
            self._enable(False)
            self._show(*_now)

    def onClose(self, evt):
        if self._busy and not self._building:
            # Translators: message when closing while a download runs.
            gui.messageBox(_("Please wait for the current operation to finish."),
                           _("Classic Voices"), wx.OK, self)
            return
        DataDialog._instance = None
        self.Destroy()

    def refresh(self):
        self.status.Set(_status_lines())
        vr, dr = cv_paths.voices_root(), cv_paths.downloads_root()
        # voices whose firmware isn't here are listed too, saying what they need
        self._downloads = ([x + ([],) for x in cv_download.available(vr, dr)]
                           + cv_download.needing(vr, dr))
        # Translators: a downloadable voice: its name, size and (after a comma) the firmware it needs.
        self.dl.Set(["%s - %d MB" % (label, size >> 20)
                     + (_(", requires the %s firmware") % " and ".join(FIRMWARE_NAMES[f] for f in needs)
                        if needs else "")
                     for _k, _e, label, size, needs in self._downloads])
        self.fastBtn.Enable(not self._busy and bool(_fast_code_missing()))

    @staticmethod
    def _progress(message, fraction):
        # A message starts a step, with its own progress from 0: the
        # fraction is how far that step is.
        if message:
            _now[:] = [message, fraction]
        elif fraction is not None:
            _now[1] = fraction

        def update():
            d = DataDialog._instance
            if d:
                d._show(message, fraction)
        wx.CallAfter(update)

    def _show(self, message, fraction):
        if message:
            self.progress.SetLabel(message)
        if fraction is not None or message:
            percent = int(min(max(fraction or 0.0, 0.0), 1.0) * 100)
            if self.gauge.GetValue() != percent:
                self.gauge.SetValue(percent)

    def _enable(self, on):
        for b in (self.appleBtn, self.importBtn, self.importDirBtn, self.dlBtn):
            b.Enable(on)
        self.fastBtn.Enable(on and bool(_fast_code_missing()))

    def _start(self, fn):
        """Runs fn (None: nothing), then builds fast engine code for whatever
        lacks it. The dialog can be closed while fast code builds."""
        if not _work.acquire(blocking=False):
            message, fraction = _now
            if fraction is not None:
                message = "%s, %d%%" % (message, int(fraction * 100))
            # Translators: message when another job (such as building fast code) is still running;
            # %s says what it is doing.
            gui.messageBox(_("Classic Voices is still busy: %s. It says when it's done.") % message,
                           _("Classic Voices"), wx.OK, self)
            return
        _now[:] = ["", None]
        self._busy = True
        self._building = False
        self.gauge.SetValue(0)
        self._enable(False)

        def building():
            self._building = True
            # Translators: shown while fast engine code builds.
            self._progress(_("Building fast engine code. You can close this window; "
                             "NVDA says when it's ready."), 0.0)

        def run():
            error = None
            built = False
            try:
                if fn:
                    fn()
                if _fast_code_missing():
                    wx.CallAfter(building)
                    built = _build_fast_code(self._progress)
            except Exception as e:
                log.exception("Classic Voices data operation failed")
                error = str(e)
            finally:
                _work.release()

            def finish():
                if fn or built:
                    _reload_synth()
                d = DataDialog._instance
                if d:
                    d._busy = d._building = False
                    d._enable(True)
                    d.refresh()
                if error:
                    if d:
                        gui.messageBox(error, _("Classic Voices"), wx.OK | wx.ICON_ERROR, d)
                    else:
                        ui.message(_("Classic Voices: %s") % error)
                    return
                if built:
                    # Translators: announced when fast engine code has been built.
                    done = _("Classic Voices: fast engine code is ready.")
                else:
                    # Translators: announced when an import/download finishes.
                    done = _("Done.")
                if d:
                    d.progress.SetLabel(done)
                ui.message(done)
            wx.CallAfter(finish)
        threading.Thread(target=run, daemon=True).start()

    def onApple(self, evt):
        vr = cv_paths.voices_root()
        # iOS 7.1.2 too: only its engine, read in place (about 50 MB), for the Siri voices
        todo = [k for k, _label in BUILDS if k in cv_remote.FIRMWARE and not cv_voices.has_engine(vr, k)]
        if not todo:
            # Translators: message when every firmware is imported already.
            gui.messageBox(_("Every firmware is imported already."), _("Classic Voices"), wx.OK, self)
            return
        d = FirmwareDialog(self, todo)
        ok = d.ShowModal() == wx.ID_OK
        todo = d.chosen()
        d.Destroy()
        if not ok or not todo:
            return

        def work():
            for k in todo:
                cv_remote.import_from_apple(k, cv_paths.voices_root(), self._progress)
        self._start(work)

    def onImport(self, evt):
        # Translators: title of the IPSW file picker.
        with wx.FileDialog(self, _("Choose an iPhone firmware file"),
                           wildcard="iPhone firmware (*.ipsw)|*.ipsw|All files (*.*)|*.*",
                           style=wx.FD_OPEN | wx.FD_FILE_MUST_EXIST) as fd:
            if fd.ShowModal() != wx.ID_OK:
                return
            path = fd.GetPath()
        self._start(lambda: cv_firmware.import_ipsw(path, cv_paths.voices_root(),
                                                    self._progress))

    def onImportFolder(self, evt):
        # Translators: title of the extracted-firmware folder picker.
        with wx.DirDialog(self, _("Choose an extracted firmware folder "
                                  "(e.g. extracted/ or extracted/ios6)"),
                          style=wx.DD_DEFAULT_STYLE | wx.DD_DIR_MUST_EXIST) as dd:
            if dd.ShowModal() != wx.ID_OK:
                return
            path = dd.GetPath()
        self._start(lambda: cv_firmware.import_extracted(path, cv_paths.voices_root(),
                                                         self._progress))

    def onDownload(self, evt):
        checked = [self._downloads[i] for i in self.dl.GetCheckedItems()]
        if not checked:
            # Translators: message when no voices are checked for download.
            gui.messageBox(_("Check at least one voice to download."),
                           _("Classic Voices"), wx.OK, self)
            return

        firmware = []
        for _k, _e, _label, _size, needs in checked:
            firmware += [f for f in needs if f not in firmware]
        if firmware and gui.messageBox(
                # Translators: asks before getting the firmware checked voices need; %s names it.
                _("The checked voices need Apple's %s firmware, which isn't here yet. Get it from Apple now "
                  "(only its speech files are streamed, which takes a while), then download the voices?")
                % " and ".join(FIRMWARE_NAMES[f] for f in firmware),
                _("Classic Voices"), wx.YES_NO | wx.ICON_QUESTION, self) != wx.YES:
            return

        def work():
            for k in firmware:
                cv_remote.import_from_apple(k, cv_paths.voices_root(), self._progress)
            for kind, entry, label, _size, _needs in checked:
                self._progress(label, None)
                cv_download.download(kind, entry, cv_paths.downloads_root(), self._progress)
        self._start(work)


def _language_name(code):
    """"German" for "de", "Chinese (Taiwan)" for "zh-TW" (in NVDA's language)."""
    try:
        return languageHandler.getLanguageDescription(code.replace("-", "_")) or code
    except Exception:
        return code


class FirmwareDialog(wx.Dialog):
    """Which of the firmware not imported yet to get from Apple."""

    def __init__(self, parent, todo):
        # Translators: title of the dialog choosing which firmware to get from Apple.
        super().__init__(parent, title=_("Get firmware from Apple"))
        self.todo = todo
        labels = dict(BUILDS)
        main = wx.BoxSizer(wx.VERTICAL)
        # Translators: explains the firmware checklist.
        main.Add(wx.StaticText(self, label=_(
            "Only each firmware's speech files are streamed from Apple, which takes a while; then fast engine "
            "code is built on this computer.")), flag=wx.ALL, border=5)
        # Translators: label of the firmware checklist.
        main.Add(wx.StaticText(self, label=_("&Firmware to get:")), flag=wx.LEFT | wx.TOP, border=5)
        # Translators: a firmware in the checklist: its name, then what it brings.
        self.list = nvdaControls.CustomCheckListBox(self, choices=[
            _("%(firmware)s: %(purpose)s") % {"firmware": labels.get(k, k), "purpose": cv_remote.PURPOSE.get(k, "")}
            for k in todo], size=(520, 220))
        self.list.SetCheckedItems(range(len(todo)))
        self.list.SetSelection(0)
        main.Add(self.list, proportion=1, flag=wx.EXPAND | wx.ALL, border=5)
        btns = wx.BoxSizer(wx.HORIZONTAL)
        # Translators: button getting the checked firmware.
        get = wx.Button(self, wx.ID_OK, label=_("&Get the checked firmware"))
        get.SetDefault()
        btns.Add(get)
        btns.Add(wx.Button(self, wx.ID_CANCEL))
        main.Add(btns, flag=wx.ALL, border=5)
        self.SetSizerAndFit(main)
        self.list.SetFocus()

    def chosen(self):
        return [self.todo[i] for i in self.list.GetCheckedItems()]


class WantedDialog(wx.Dialog):
    """The languages firmware imports take and downloads offer
    (<data>/languages-wanted.txt), and removing the voices of the others."""

    def __init__(self, parent):
        # Translators: title of the dialog choosing which languages to get.
        super().__init__(parent, title=_("Languages to get"))
        self.langs = sorted(cv_firmware.ALL_LANGUAGES, key=_language_name)
        wanted = cv_firmware.wanted_languages(cv_paths.data_root())
        main = wx.BoxSizer(wx.VERTICAL)
        # Translators: explains the languages list.
        main.Add(wx.StaticText(self, label=_(
            "Firmware you import from now on brings the checked languages' voices, and only their voices are "
            "offered for download. Voices you have of the others stay until you remove them.")), flag=wx.ALL, border=5)
        # Translators: label of the languages checklist.
        main.Add(wx.StaticText(self, label=_("&Languages:")), flag=wx.LEFT | wx.TOP, border=5)
        self.list = nvdaControls.CustomCheckListBox(self, choices=[_language_name(l) for l in self.langs],
                                                    size=(360, 300))
        self.list.SetCheckedItems([i for i, l in enumerate(self.langs) if wanted is None or l in wanted])
        self.list.Bind(wx.EVT_CHECKLISTBOX, self.onCheck)
        main.Add(self.list, proportion=1, flag=wx.EXPAND | wx.ALL, border=5)
        btns = wx.BoxSizer(wx.HORIZONTAL)
        # Translators: button removing every voice of the unchecked languages.
        rm = wx.Button(self, label=_("&Remove the unchecked languages' voices"))
        rm.Bind(wx.EVT_BUTTON, self.onRemove)
        btns.Add(rm)
        close = wx.Button(self, wx.ID_CLOSE)
        self.Bind(wx.EVT_BUTTON, lambda evt: self.EndModal(wx.ID_CLOSE), id=wx.ID_CLOSE)
        self.SetEscapeId(wx.ID_CLOSE)
        btns.Add(close)
        main.Add(btns, flag=wx.ALL, border=5)
        self.SetSizerAndFit(main)

    def _checked(self):
        return {self.langs[i] for i in self.list.GetCheckedItems()}

    def onCheck(self, evt):
        got = self._checked()
        cv_firmware.set_wanted_languages(cv_paths.data_root(), None if len(got) == len(self.langs) else got)

    def onRemove(self, evt):
        drop = [l for l in self.langs if l not in self._checked()]
        if not drop:
            return
        # Translators: asks before removing voices; %s lists the languages.
        if gui.messageBox(_("Remove every voice of these languages: %s?") % ", ".join(_language_name(l) for l in drop),
                          _("Classic Voices"), wx.YES_NO | wx.NO_DEFAULT | wx.ICON_QUESTION, self) != wx.YES:
            return
        for l in drop:
            cv_firmware.remove_language(cv_paths.voices_root(), cv_paths.downloads_root(), l)
        self.onCheck(None)  # remove_language narrows the list; the checks are what was asked for
        _reload_synth()
        # Translators: announced when voices were removed.
        ui.message(_("Removed."))


class LanguagesDialog(wx.Dialog):
    """Automatic language switching: per language, the voice that reads text
    in it, for every voice or while one voice reads (languages.txt)."""

    def __init__(self, parent):
        # Translators: title of the dialog choosing voices for other languages.
        super().__init__(parent, title=_("Voices for other languages"))
        self.data = cv_paths.data_root()
        self.voices = cv_voices.scan(cv_paths.voices_root(), cv_paths.downloads_root())
        self.names = {v.key: v.name for v in self.voices}
        main = wx.BoxSizer(wx.VERTICAL)
        # Translators: explains automatic language switching choices.
        main.Add(wx.StaticText(self, label=_(
            "With NVDA's automatic language switching on, text in another language is read by a voice of that "
            "language: by default the one nearest the voice reading. Choose voices here for every voice, or for "
            "one voice, or keep the voice reading for a language or for all of them.")), flag=wx.ALL, border=5)
        # Translators: label of the list choosing whose choices are shown.
        main.Add(wx.StaticText(self, label=_("&For:")), flag=wx.LEFT | wx.TOP, border=5)
        self.whos = [None] + [v.key for v in self.voices]
        # Translators: the first entry of the "For" list.
        self.who = wx.Choice(self, choices=[_("Every voice")] + [v.name for v in self.voices])
        self.who.SetSelection(0)
        self.who.Bind(wx.EVT_CHOICE, lambda evt: self.fill())
        main.Add(self.who, flag=wx.ALL, border=5)
        # Translators: checkbox turning Classic Voices' language switching on or off (for every voice or one).
        self.switch = wx.CheckBox(self, label=_("&Switch voices when the text changes language"))
        self.switch.Bind(wx.EVT_CHECKBOX, self.onSwitch)
        main.Add(self.switch, flag=wx.ALL, border=5)
        self.rows = wx.ScrolledWindow(self, size=(520, 360), style=wx.VSCROLL)
        self.rows.SetScrollRate(0, 20)
        self.grid = wx.FlexGridSizer(2, 5, 5)
        self.rows.SetSizer(self.grid)
        main.Add(self.rows, proportion=1, flag=wx.EXPAND | wx.ALL, border=5)
        close = wx.Button(self, wx.ID_CLOSE)
        self.Bind(wx.EVT_BUTTON, lambda evt: self.EndModal(wx.ID_CLOSE), id=wx.ID_CLOSE)
        self.SetEscapeId(wx.ID_CLOSE)
        main.Add(close, flag=wx.ALL, border=5)
        self.SetSizerAndFit(main)
        self.fill()

    def onSwitch(self, evt):
        who = self.whos[self.who.GetSelection()]
        cv_languages.set_choice(self.data, "*", None if self.switch.GetValue() else "-", who)
        self.fill()

    def fill(self):
        self.rows.DestroyChildren()
        self.grid.Clear()
        who = self.whos[self.who.GetSelection()]
        by = {}
        for v in self.voices:
            by.setdefault(cv_languages.choice_language(v.language), []).append(v)
        own = next((v.language for v in self.voices if v.key == who), None)
        langs = sorted((l for l in by if not own or cv_languages.base(l) != cv_languages.base(own)), key=_language_name)
        choices = cv_languages.load(self.data)
        # "-": the voice reading keeps reading it; "*" as the language: every language
        every = {l: k for l, k in choices.get("*", {}).items() if k in self.names or k == "-"}
        mine = {l: k for l, k in choices.get(who, {}).items() if k in self.names or k == "-"} if who else every
        every_off = every.get("*") == "-"
        self.switch.SetValue(mine.get("*") != "-" and not every_off)
        self.switch.Enable(not (who and every_off))
        # Translators: the entry in a language's list keeping the voice that is reading.
        keep = _("Keep the voice reading")
        pairs = [(v.key, v.language) for v in self.voices]
        for lang in langs:
            if not who:
                # Translators: the automatic entry in a language's list, for every voice.
                first = _("Automatic: the voice nearest the one reading") if not every_off else \
                    _("Off: keep the voice reading")
            elif lang in every or every_off:
                # Translators: %s is the voice chosen for that language for every voice.
                first = _("As for every voice: %s") % (keep if every.get(lang, "-") == "-"
                                                       else self.names[every[lang]])
            else:
                auto = cv_lib.lang_pick(who, lang, pairs)
                # Translators: %s is the voice automatic language switching picks.
                first = _("Automatic: %s") % self.names.get(auto, _("none"))
            keys = [None, "-"] + [v.key for v in by[lang]]
            label = wx.StaticText(self.rows, label="%s:" % _language_name(lang))
            c = wx.Choice(self.rows, choices=[first, keep] + [v.name for v in by[lang]], name=_language_name(lang))
            c.SetSelection(keys.index(mine[lang]) if lang in mine and mine[lang] in keys else 0)
            c.Bind(wx.EVT_CHOICE, lambda evt, lang=lang, keys=keys, c=c: cv_languages.set_choice(
                self.data, lang, keys[c.GetSelection()], who))
            self.grid.Add(label, flag=wx.ALIGN_CENTER_VERTICAL)
            self.grid.Add(c)
        self.rows.Layout()
        self.rows.FitInside()


def _update_fast_code():
    """Fast code an older version of the add-on built still runs, but slower:
    build it again in the background, as the voices keep working meanwhile.
    When the Classic Voices app is installed, its data is shared and the app
    does this."""
    try:
        if globalVars.appArgs.secure or cv_paths.app_data_root() or not cv_fastcode.recompiler():
            return
        todo = _fast_code_outdated()
    except Exception:
        log.exception("Classic Voices: checking the fast engine code failed")
        return
    if not todo or not _work.acquire(blocking=False):
        return
    # Translators: shown while fast code an older version built is built again in the background.
    _now[:] = [_("Updating the fast engine code; the voices keep working meanwhile, a little slower."), 0.0]

    def run():
        built = False
        try:
            built = _build_fast_code(DataDialog._progress, todo)
        except Exception:
            log.exception("Classic Voices: updating the fast engine code failed")
        finally:
            _work.release()

        def finish():
            if built:
                _reload_synth()
            d = DataDialog._instance
            if d:
                d._busy = d._building = False
                d._enable(True)
                d.refresh()
                d.gauge.SetValue(0)
                # Translators: shown when updating the fast code failed (the log says why).
                d.progress.SetLabel(_("Classic Voices: fast engine code is updated.") if built
                                    else _("Updating the fast engine code failed; Build fast code tries again."))
            if built:
                # Translators: announced when fast engine code built by an older version has been built again.
                ui.message(_("Classic Voices: fast engine code is updated."))
        wx.CallAfter(finish)
    threading.Thread(target=run, daemon=True).start()


class GlobalPlugin(globalPluginHandler.GlobalPlugin):
    def __init__(self):
        super().__init__()
        tools = gui.mainFrame.sysTrayIcon.toolsMenu
        # Translators: the Tools menu item that opens the data manager.
        self.item = tools.Append(wx.ID_ANY, _("Classic &Voices data..."),
                                 _("Import firmware and download Classic Voices"))
        gui.mainFrame.sysTrayIcon.Bind(wx.EVT_MENU, self.onOpen, self.item)
        # after an update, once NVDA has settled
        self._later = wx.CallLater(30000, _update_fast_code)

    def onOpen(self, evt):
        gui.mainFrame.prePopup()
        d = DataDialog._instance or DataDialog(gui.mainFrame)
        d.Raise()
        d.Show()
        gui.mainFrame.postPopup()

    def terminate(self):
        try:
            self._later.Stop()
        except Exception:
            pass
        try:
            gui.mainFrame.sysTrayIcon.toolsMenu.Remove(self.item)
        except Exception:
            pass
