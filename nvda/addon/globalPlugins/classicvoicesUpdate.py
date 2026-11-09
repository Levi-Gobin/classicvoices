# Classic Voices: says once when a newer version is on GitHub and offers to
# install it: the add-on is downloaded, checked against the SHA-256 GitHub
# publishes for it, and handed to NVDA's own add-on installer (which asks, and
# restarts NVDA).
import os
import threading

import globalPluginHandler
import gui
import ui
import wx
from logHandler import log

from synthDrivers._classicvoices import paths as cv_paths
from synthDrivers._classicvoices import update as cv_update

import addonHandler
try:
    addonHandler.initTranslation()
except Exception:
    pass

ASSET = "classicvoices.nvda-addon"
STATE = "update-nvda.json"  # the app, when installed, keeps update.json beside it
_stop = threading.Event()


def _check():
    """30 s after NVDA starts (not while it's starting), then every hour while
    it runs: update.newer itself asks GitHub at most every 6 hours."""
    try:
        cv_update.remove_downloads(ASSET)
    except Exception:
        log.debugWarning("Classic Voices: removing old update files failed", exc_info=True)
    wait = 30
    while not _stop.wait(wait):
        wait = 3600
        try:
            found = cv_update.newer(cv_paths.data_root(), name=STATE)
        except Exception:
            log.debugWarning("Classic Voices: update check failed", exc_info=True)
            continue
        if found:
            wx.CallAfter(_offer, *found)


def _check_now():
    """The person asked: GitHub is asked at once and says either way."""
    try:
        found = cv_update.newer(cv_paths.data_root(), name=STATE, asked=True)
    except Exception as e:
        log.debugWarning("Classic Voices: update check failed", exc_info=True)
        # Translators: the update check failed; %s says why.
        wx.CallAfter(gui.messageBox, _("Classic Voices couldn't check for updates: %s.") % e,
                     _("Classic Voices update"), wx.OK | wx.ICON_ERROR)
        return
    if found:
        wx.CallAfter(_offer, *found)
    else:
        # Translators: no newer version; %s is this one.
        wx.CallAfter(gui.messageBox, _("Classic Voices %s is the newest version.") % cv_update.VERSION,
                     _("Classic Voices update"), wx.OK | wx.ICON_INFORMATION)


def _offer(version, url, assets):
    asset = assets.get(ASSET)
    if not asset:
        # Translators: asked when a newer Classic Voices is available; %s is its version.
        if gui.messageBox(_("Classic Voices %s is available. Open its download page?") % version,
                          # Translators: title of the new version message.
                          _("Classic Voices update"), wx.YES_NO | wx.ICON_INFORMATION) == wx.YES:
            os.startfile(url)
        return
    # Translators: asked when a newer Classic Voices is available; %s is its version.
    if gui.messageBox(_("Classic Voices %s is available. Download and install it now? NVDA restarts "
                        "afterwards.") % version,
                      _("Classic Voices update"), wx.YES_NO | wx.ICON_INFORMATION) != wx.YES:
        return
    # Translators: announced while the new version downloads; %s is its version.
    ui.message(_("Downloading Classic Voices %s") % version)
    threading.Thread(target=_download, args=(version, url, asset), name="ClassicVoicesUpdate", daemon=True).start()


def _download(version, url, asset):
    path = cv_update.download_path(version, ASSET)
    try:
        cv_update.download(asset, path)
    except Exception as e:
        log.warning("Classic Voices: update download failed", exc_info=True)
        wx.CallAfter(_failed, url, str(e))
        return
    wx.CallAfter(_install, path, url)


def _failed(url, error):
    # Translators: the update couldn't be downloaded; %s says why.
    if gui.messageBox(_("The new Classic Voices couldn't be downloaded: %s. Open its download page?") % error,
                      _("Classic Voices update"), wx.YES_NO | wx.ICON_ERROR) == wx.YES:
        os.startfile(url)


def _install(path, url):
    """NVDA's own add-on install: it confirms, replaces the old version and
    offers to restart."""
    try:
        from gui import addonGui
        if addonGui.installAddon(gui.mainFrame, path):
            addonGui.promptUserForRestart()
        return
    except Exception:
        log.warning("Classic Voices: NVDA's add-on installer failed", exc_info=True)
    try:
        os.startfile(path)  # an installed NVDA opens .nvda-addon files itself
    except OSError:
        _failed(url, _("NVDA couldn't install it"))


class GlobalPlugin(globalPluginHandler.GlobalPlugin):
    def __init__(self):
        super().__init__()
        _stop.clear()
        threading.Thread(target=_check, name="ClassicVoicesUpdate", daemon=True).start()
        tools = gui.mainFrame.sysTrayIcon.toolsMenu
        # Translators: the Tools menu item that asks GitHub for a newer Classic Voices.
        self.item = tools.Append(wx.ID_ANY, _("Check for Classic Voices &updates"),
                                 _("Ask GitHub now whether a newer Classic Voices is out"))
        gui.mainFrame.sysTrayIcon.Bind(wx.EVT_MENU, self.onCheck, self.item)

    def onCheck(self, evt):
        threading.Thread(target=_check_now, name="ClassicVoicesUpdate", daemon=True).start()

    def terminate(self):
        _stop.set()
        try:
            gui.mainFrame.sysTrayIcon.toolsMenu.Remove(self.item)
        except Exception:
            pass
        super().terminate()
