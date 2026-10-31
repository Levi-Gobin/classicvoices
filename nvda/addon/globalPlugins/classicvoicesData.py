# Classic Voices data manager: NVDA menu -> Tools -> Classic Voices data.
# Gets the speech files out of Apple's iPhone firmware (streamed from Apple,
# or from an IPSW the user has), downloads voices from Apple's servers once
# the matching engine is in place, and then builds the fast engine code on
# this computer. All slow work runs on a background thread, one job at a
# time; the dialog stays usable and announces progress.
import os
import threading

import globalPluginHandler
import gui
import ui
import wx
from gui import nvdaControls
from logHandler import log

from synthDrivers._classicvoices import download as cv_download
from synthDrivers._classicvoices import fastcode as cv_fastcode
from synthDrivers._classicvoices import firmware as cv_firmware
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

# One job at a time, also while the dialog is closed (fast code builds on in the background).
_work = threading.Lock()


def _has_fast_code(build):
    nr = cv_paths.native_root()
    return all(os.path.isfile(os.path.join(nr, f)) for f in cv_fastcode.native_files(build))


def _fast_code_missing():
    vr = cv_paths.voices_root()
    return [b for b, _label in BUILDS if cv_voices.has_engine(vr, b) and not _has_fast_code(b)]


def _build_fast_code(progress):
    """Builds fast engine code for every imported engine without it: a few
    minutes each, and the Zig compiler (about 100 MB) is downloaded the first
    time. Returns whether anything was built."""
    todo = _fast_code_missing()
    if not todo or not cv_fastcode.recompiler():
        return False
    cc = cv_fastcode.find_compiler()
    if not cc:
        cv_fastcode.download_zig(progress)
        cc = cv_fastcode.find_compiler()
    for b in todo:
        cv_fastcode.build(b, cv_paths.voices_root(), cv_paths.native_root(), cc, progress)
    return True


def _status_lines():
    vr, dr = cv_paths.voices_root(), cv_paths.downloads_root()
    lines = []
    for build, label in BUILDS:
        if not cv_voices.has_engine(vr, build):
            lines.append("%s: not imported" % label)
            continue
        lines.append("%s: imported, %s" % (label, "fast engine code ready" if _has_fast_code(build)
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
        main.Add(wx.StaticText(self, label=_("Downloadable &voices (needs the matching firmware first):")),
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
        self.appleBtn = wx.Button(self, label=_("Get firmware from &Apple"))
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
        self._downloads = cv_download.available(cv_paths.voices_root(),
                                                cv_paths.downloads_root())
        self.dl.Set(["%s - %d MB" % (label, size >> 20)
                     for _k, _e, label, size in self._downloads])
        self.fastBtn.Enable(not self._busy and bool(_fast_code_missing()))

    @staticmethod
    def _progress(message, fraction):
        def update():
            d = DataDialog._instance
            if not d:
                return
            if message:
                d.progress.SetLabel(message)
            if fraction is not None:
                percent = int(min(max(fraction, 0.0), 1.0) * 100)
                if d.gauge.GetValue() != percent:
                    d.gauge.SetValue(percent)
        wx.CallAfter(update)

    def _enable(self, on):
        for b in (self.appleBtn, self.importBtn, self.importDirBtn, self.dlBtn):
            b.Enable(on)
        self.fastBtn.Enable(on and bool(_fast_code_missing()))

    def _start(self, fn):
        """Runs fn (None: nothing), then builds fast engine code for whatever
        lacks it. The dialog can be closed while fast code builds."""
        if not _work.acquire(blocking=False):
            # Translators: message when another job (such as building fast code) is still running.
            gui.messageBox(_("Classic Voices is still busy; it says when it's done."),
                           _("Classic Voices"), wx.OK, self)
            return
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
        todo = [k for k, _label in BUILDS if k in cv_remote.FIRMWARE and not cv_voices.has_engine(vr, k)]
        if not todo:
            # Translators: message when every firmware is imported already.
            gui.messageBox(_("Every firmware is imported already."), _("Classic Voices"), wx.OK, self)
            return
        # Translators: asks before streaming firmware from Apple; %d is how many.
        if gui.messageBox(_("Get %d firmware from Apple? Only their speech files are streamed, which takes a "
                            "while; then fast engine code is built on this computer.") % len(todo),
                          _("Classic Voices"), wx.YES_NO | wx.ICON_QUESTION, self) != wx.YES:
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

        def work():
            for kind, entry, label, _size in checked:
                self._progress(label, None)
                if kind == "enhanced":
                    cv_download.download_enhanced(entry, cv_paths.downloads_root(),
                                                  self._progress)
                else:
                    cv_download.download_mac_voice(entry, cv_paths.downloads_root(),
                                                   self._progress)
        self._start(work)


class GlobalPlugin(globalPluginHandler.GlobalPlugin):
    def __init__(self):
        super().__init__()
        tools = gui.mainFrame.sysTrayIcon.toolsMenu
        # Translators: the Tools menu item that opens the data manager.
        self.item = tools.Append(wx.ID_ANY, _("Classic &Voices data..."),
                                 _("Import firmware and download Classic Voices"))
        gui.mainFrame.sysTrayIcon.Bind(wx.EVT_MENU, self.onOpen, self.item)

    def onOpen(self, evt):
        gui.mainFrame.prePopup()
        d = DataDialog._instance or DataDialog(gui.mainFrame)
        d.Raise()
        d.Show()
        gui.mainFrame.postPopup()

    def terminate(self):
        try:
            gui.mainFrame.sysTrayIcon.toolsMenu.Remove(self.item)
        except Exception:
            pass
