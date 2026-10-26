# Classic Voices data manager: NVDA menu -> Tools -> Classic Voices data.
# Imports the user's own iPhone firmware (IPSW) and downloads voices from
# Apple's servers once the matching engine is in place. All slow work runs on
# a background thread; the dialog stays usable and announces progress.
import os
import struct
import threading

import globalPluginHandler
import gui
import ui
import wx
from gui import nvdaControls
from logHandler import log

from synthDrivers._classicvoices import download as cv_download
from synthDrivers._classicvoices import firmware as cv_firmware
from synthDrivers._classicvoices import paths as cv_paths
from synthDrivers._classicvoices import voices as cv_voices

import addonHandler
try:
    addonHandler.initTranslation()
except Exception:
    pass


def _status_lines():
    vr, dr = cv_paths.voices_root(), cv_paths.downloads_root()
    nr = cv_paths.native_root()
    bits = 64 if struct.calcsize("P") == 8 else 32
    lines = []
    for build, label in (("ios3", "iPhone OS 3.1.3"), ("ios4", "iOS 4.3.5"),
                         ("ios5", "iOS 5.1.1"), ("ios6", "iOS 6.1.6"),
                         ("ios7", "iOS 7.1.2 (engine only)"),
                         ("ios10", "iOS 10.3.4 (engine only)")):
        if not cv_voices.has_engine(vr, build):
            lines.append("%s: not imported" % label)
            continue
        fast = os.path.isfile(os.path.join(nr, "cvnative_%s_%d.dll" % (build, bits)))
        lines.append("%s: imported%s" % (
            label,
            ", with fast engine code" if fast
            else " (interpreter; build fast code with nvda/build_native.py)"))
    n = len(cv_voices.scan(vr, dr))
    lines.append("Voices available to NVDA: %d" % n)
    return lines


class DataDialog(wx.Dialog):
    _instance = None

    def __init__(self, parent):
        super().__init__(parent, title="Classic Voices data")
        DataDialog._instance = self
        self._busy = False
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
        if self._busy:
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

    def _progress(self, message, fraction):
        def update():
            if not DataDialog._instance:
                return
            if message:
                self.progress.SetLabel(message)
            if fraction is not None:
                percent = int(min(max(fraction, 0.0), 1.0) * 100)
                if self.gauge.GetValue() != percent:
                    self.gauge.SetValue(percent)
        wx.CallAfter(update)

    def _start(self, fn):
        self._busy = True
        self.gauge.SetValue(0)
        self.importBtn.Disable()
        self.importDirBtn.Disable()
        self.dlBtn.Disable()

        def run():
            error = None
            try:
                fn()
            except Exception as e:
                log.exception("Classic Voices data operation failed")
                error = str(e)

            def finish():
                if not DataDialog._instance:
                    return
                self._busy = False
                self.importBtn.Enable()
                self.importDirBtn.Enable()
                self.dlBtn.Enable()
                self.refresh()
                if error:
                    gui.messageBox(error, _("Classic Voices"), wx.OK | wx.ICON_ERROR, self)
                else:
                    # Translators: announced when an import/download finishes.
                    done = _("Done. New voices appear after reselecting the synthesizer.")
                    self.progress.SetLabel(done)
                    ui.message(done)
            wx.CallAfter(finish)
        threading.Thread(target=run, daemon=True).start()

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
