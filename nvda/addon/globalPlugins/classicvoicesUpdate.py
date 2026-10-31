# Classic Voices: says once when a newer version is on GitHub, and offers to
# open its release page. Nothing is downloaded or installed here.
import os
import threading
import time

import globalPluginHandler
import gui
import wx
from logHandler import log

from synthDrivers._classicvoices import paths as cv_paths
from synthDrivers._classicvoices import update as cv_update

import addonHandler
try:
    addonHandler.initTranslation()
except Exception:
    pass


def _check():
    time.sleep(30)  # not while NVDA is starting
    try:
        found = cv_update.newer(cv_paths.data_root())
    except Exception:
        log.debugWarning("Classic Voices: update check failed", exc_info=True)
        return
    if found:
        wx.CallAfter(_offer, *found)


def _offer(version, url):
    # Translators: asked when a newer Classic Voices is available; %s is its version.
    if gui.messageBox(_("Classic Voices %s is available. Open its download page?") % version,
                      # Translators: title of the new version message.
                      _("Classic Voices update"), wx.YES_NO | wx.ICON_INFORMATION) == wx.YES:
        os.startfile(url)


class GlobalPlugin(globalPluginHandler.GlobalPlugin):
    def __init__(self):
        super().__init__()
        threading.Thread(target=_check, name="ClassicVoicesUpdate", daemon=True).start()
