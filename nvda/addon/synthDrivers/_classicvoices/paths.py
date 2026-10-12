# Where the speech data lives: <NVDA user configuration>/classicvoices.
# It sits outside the add-on folder because updating an add-on deletes and
# recreates its directory (the same reasoning as Panthera Speech's).
#
#   classicvoices/
#     Voices/              engines + built-in voices from your firmware
#       ios6/voiced
#       ios6/TTSResources/<lang>/...
#       mac107/common/     assembled from firmware files at import time
#     Downloads/           voices fetched from Apple's servers
#       ios6/<lang>/       enhanced voices (broker.hdr.asset ...)
#       mac107/<Voice>/    Mac OS X voices (broker.hdr ...)
import os


def data_root():
    import globalVars
    return os.path.join(globalVars.appArgs.configPath, "classicvoices")


def voices_root():
    return os.path.join(data_root(), "Voices")


def downloads_root():
    return os.path.join(data_root(), "Downloads")


def ensure_dirs():
    for p in (voices_root(), downloads_root()):
        os.makedirs(p, exist_ok=True)
