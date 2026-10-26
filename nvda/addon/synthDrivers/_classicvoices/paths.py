# Where the speech data lives: the Classic Voices app's folder when the app is
# installed and has voices, else <NVDA user configuration>/classicvoices.
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


def app_data_root():
    """The Classic Voices app's folder (HKLM\\SOFTWARE\\Classic Voices\\DataDir), if
    the app is installed and has voices there: the app, its SAPI voices and
    NVDA then share one copy."""
    try:
        import winreg
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\Classic Voices", 0,
                            winreg.KEY_READ | winreg.KEY_WOW64_64KEY) as k:
            d = winreg.QueryValueEx(k, "DataDir")[0]
    except (OSError, ImportError):
        return None
    v = os.path.join(d, "Voices")
    if os.path.isdir(v) and any(os.path.isfile(os.path.join(v, b, "voiced")) for b in os.listdir(v)):
        return d
    return None


def data_root():
    d = app_data_root()
    if d:
        return d
    import globalVars
    return os.path.join(globalVars.appArgs.configPath, "classicvoices")


def voices_root():
    return os.path.join(data_root(), "Voices")


def downloads_root():
    return os.path.join(data_root(), "Downloads")


def native_root():
    # cvnative_<build>_<bits>.dll, built from the user's firmware by
    # nvda/build_native.py; the engine falls back to the interpreter without.
    return os.path.join(data_root(), "Native")


def ensure_dirs():
    for p in (voices_root(), downloads_root()):
        os.makedirs(p, exist_ok=True)
