"""What differs between Windows and Linux: where the data lives, where
settings go, how a voice is heard, how the system's own speech (SAPI,
Speech Dispatcher) learns about the voices, and how fast code is built."""
import ctypes
import json
import os
import platform
import shutil
import subprocess
import sys
import threading
import time

IS_WIN = sys.platform == "win32"
HERE = os.path.dirname(os.path.abspath(__file__))
FROZEN = getattr(sys, "frozen", False)


def bundle_dir():
    """The app's own files: next to the frozen exe, or the repository."""
    if FROZEN:
        return getattr(sys, "_MEIPASS", os.path.dirname(sys.executable))
    return os.path.dirname(os.path.dirname(HERE))


def install_dir():
    """Where the installer put the app (and, on Windows, the SAPI DLLs)."""
    return os.path.dirname(sys.executable) if FROZEN else os.path.join(bundle_dir(), "build", "sapi")


def _lib_paths():
    root = bundle_dir()
    # frozen app (lib/), repository, Linux install (beside this package in lib/classicvoices)
    for cand in (os.path.join(root, "lib"), os.path.join(root, "nvda", "addon", "synthDrivers"),
                 os.path.dirname(HERE)):
        if os.path.isdir(os.path.join(cand, "_classicvoices")):
            return os.path.realpath(cand)
    raise SystemExit("Classic Voices: its _classicvoices library is missing")


sys.path.insert(0, _lib_paths())
from _classicvoices import fastcode as cv_fastcode  # noqa: E402
from _classicvoices import voices as cv_voices  # noqa: E402

# ---------------------------------------------------------------- data folder

if IS_WIN:
    import winreg

    def data_root():
        try:
            with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\Classic Voices", 0,
                                winreg.KEY_READ | winreg.KEY_WOW64_64KEY) as k:
                d = winreg.QueryValueEx(k, "DataDir")[0]
                if d:
                    return d
        except OSError:
            pass
        return os.path.join(os.environ.get("ProgramData", r"C:\ProgramData"), "Classic Voices")
else:
    def data_root():
        d = os.environ.get("CLASSICVOICES_DATA")
        if d:
            return d
        base = os.environ.get("XDG_DATA_HOME") or os.path.join(os.path.expanduser("~"), ".local", "share")
        return os.path.join(base, "classicvoices")


def voices_root():
    return os.path.join(data_root(), "Voices")


def downloads_root():
    return os.path.join(data_root(), "Downloads")


def linux_abi():
    m = platform.machine().lower()
    return {"x86_64": "x86_64", "amd64": "x86_64", "aarch64": "arm64-v8a", "arm64": "arm64-v8a",
            "armv7l": "armeabi-v7a", "i686": "x86", "i386": "x86"}.get(m, m)


def native_dir():
    return os.path.join(data_root(), "Native") if IS_WIN else os.path.join(data_root(), "Native", linux_abi())


def native_files(build):
    return cv_fastcode.native_files(build)


def nvda_data_root():
    """The NVDA add-on's own data folder (installed NVDA), if it has voices."""
    if not IS_WIN:
        return None
    d = os.path.join(os.environ.get("APPDATA", ""), "nvda", "classicvoices")
    return d if os.path.isdir(os.path.join(d, "Voices")) else None


def open_folder(path):
    os.makedirs(path, exist_ok=True)
    if IS_WIN:
        os.startfile(path)
    else:
        subprocess.Popen(["xdg-open", path], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

# ---------------------------------------------------------------- settings

# The two settings the engines read, named as they read them, and the app's
# own: FastCodeAuto is turned off when a fast-code build fails or is stopped,
# so it then starts only from the Speed up the voices page.
SETTINGS = {"PronunciationRules": True, "PitchFallback": True, "FastCodeAuto": True}

if IS_WIN:
    def get_setting(name):
        try:
            with winreg.OpenKey(winreg.HKEY_CURRENT_USER, r"Software\Classic Voices") as k:
                return bool(winreg.QueryValueEx(k, name)[0])
        except OSError:
            return SETTINGS[name]

    def set_setting(name, value):
        with winreg.CreateKey(winreg.HKEY_CURRENT_USER, r"Software\Classic Voices") as k:
            winreg.SetValueEx(k, name, 0, winreg.REG_DWORD, 1 if value else 0)
else:
    def _settings_path():
        return os.path.join(data_root(), "settings.conf")

    def _read_settings():
        out = {}
        try:
            with open(_settings_path(), encoding="utf-8") as f:
                for line in f:
                    parts = line.split()
                    if len(parts) == 2 and not line.startswith("#"):
                        out[parts[0]] = parts[1]
        except OSError:
            pass
        return out

    def get_setting(name):
        v = _read_settings().get(name)
        return SETTINGS[name] if v is None else v != "0"

    def set_setting(name, value):
        s = _read_settings()
        s[name] = "1" if value else "0"
        os.makedirs(data_root(), exist_ok=True)
        with open(_settings_path(), "w", encoding="utf-8") as f:
            f.write("# Classic Voices settings (the app writes this; the speech module reads it)\n")
            for k, v in sorted(s.items()):
                f.write("%s %s\n" % (k, v))


def hidden_keys():
    return cv_voices.hidden_keys(voices_root())


def set_hidden(key, hidden):
    keys = hidden_keys()
    keys.discard(key) if not hidden else keys.add(key)
    path = cv_voices.hidden_path(voices_root())
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        f.write("".join(k + "\n" for k in sorted(keys)))

# ---------------------------------------------------------------- hearing a voice


class Player:
    """Speaks a sample with a voice, through the same engine as everything else."""

    def speak(self, key, text, rate=100, language=""):
        raise NotImplementedError

    def stop(self):
        raise NotImplementedError


class WinPlayer(Player):
    """The engine DLL (the NVDA add-on's cvengine) and the wave-out API."""

    def __init__(self):
        from _classicvoices import lib
        self._lib = lib.load()
        self._lib_mod = lib
        self._s = None
        self._gen = 0
        self._lock = threading.Lock()

    def _stream(self):
        if self._s is None:
            self._lib.cv_set_native_dir(native_dir().encode("utf-8"))
            self._s = self._lib.vxs_create(voices_root().encode("utf-8"))
            self._lib.vxs_set_asset_root(self._s, downloads_root().encode("utf-8"))
        return self._s

    def speak(self, key, text, rate=100, language=""):
        self.stop()
        if get_setting("PronunciationRules"):
            text = self._lib_mod.rules_text(voices_root(), key, language, text)
        with self._lock:
            self._gen += 1
            gen = self._gen
        threading.Thread(target=self._run, args=(gen, key, text, rate), daemon=True).start()

    def stop(self):
        with self._lock:
            self._gen += 1
        if self._s is not None:
            self._lib.vxs_cancel(self._s)

    def _run(self, gen, key, text, rate):
        winmm = ctypes.windll.winmm

        class WAVEFORMATEX(ctypes.Structure):
            _fields_ = [("wFormatTag", ctypes.c_ushort), ("nChannels", ctypes.c_ushort),
                        ("nSamplesPerSec", ctypes.c_uint), ("nAvgBytesPerSec", ctypes.c_uint),
                        ("nBlockAlign", ctypes.c_ushort), ("wBitsPerSample", ctypes.c_ushort),
                        ("cbSize", ctypes.c_ushort)]

        class WAVEHDR(ctypes.Structure):
            _fields_ = [("lpData", ctypes.c_void_p), ("dwBufferLength", ctypes.c_uint),
                        ("dwBytesRecorded", ctypes.c_uint), ("dwUser", ctypes.c_void_p),
                        ("dwFlags", ctypes.c_uint), ("dwLoops", ctypes.c_uint),
                        ("lpNext", ctypes.c_void_p), ("reserved", ctypes.c_void_p)]

        s = self._stream()
        self._lib.vxs_start(s, key.encode("utf-8"), text.encode("utf-8"), int(rate), 100, -1, 0)
        fmt = WAVEFORMATEX(1, 1, 22050, 44100, 2, 16, 0)
        h = ctypes.c_void_p()
        if winmm.waveOutOpen(ctypes.byref(h), 0xFFFFFFFF, ctypes.byref(fmt), None, None, 0):
            self._lib.vxs_cancel(s)
            return
        queued = []
        buf = (ctypes.c_int16 * 2205)()
        done = ctypes.c_int(0)
        try:
            while gen == self._gen:
                n = self._lib.cv_read_s16(s, buf, 2205, ctypes.byref(done))
                if n:
                    data = ctypes.create_string_buffer(ctypes.string_at(buf, n * 2), n * 2)
                    hdr = WAVEHDR(ctypes.cast(data, ctypes.c_void_p), n * 2, 0, None, 0, 0, None, None)
                    winmm.waveOutPrepareHeader(h, ctypes.byref(hdr), ctypes.sizeof(hdr))
                    winmm.waveOutWrite(h, ctypes.byref(hdr), ctypes.sizeof(hdr))
                    queued.append((hdr, data))
                elif done.value:
                    break
                else:
                    time.sleep(0.002)
                while queued and queued[0][0].dwFlags & 1:  # WHDR_DONE
                    hdr, _ = queued.pop(0)
                    winmm.waveOutUnprepareHeader(h, ctypes.byref(hdr), ctypes.sizeof(hdr))
            while queued and gen == self._gen and not all(q[0].dwFlags & 1 for q in queued):
                time.sleep(0.01)
        finally:
            winmm.waveOutReset(h)
            for hdr, _ in queued:
                winmm.waveOutUnprepareHeader(h, ctypes.byref(hdr), ctypes.sizeof(hdr))
            winmm.waveOutClose(h)


class CliPlayer(Player):
    """Linux: classicvoices-say (the same engine) into the sound server."""

    def __init__(self):
        self._procs = []

    @staticmethod
    def _say():
        for c in (os.path.join(os.path.dirname(sys.argv[0]), "classicvoices-say"),
                  os.path.join(HERE, "..", "..", "..", "bin", "classicvoices-say"),  # ~/.local/lib/classicvoices/...
                  os.path.join(bundle_dir(), "build", "linux", "stage", "bin", "classicvoices-say"),
                  shutil.which("classicvoices-say") or ""):
            if c and os.path.isfile(c) and os.access(c, os.X_OK):
                return c
        return None

    @staticmethod
    def _sink():
        for c in (["pw-play", "-"], ["paplay"], ["aplay", "-q"]):
            if shutil.which(c[0]):
                return c
        return None

    def available(self):
        return bool(self._say() and self._sink())

    def speak(self, key, text, rate=100, language=""):
        self.stop()
        say, sink = self._say(), self._sink()
        if not say or not sink:
            raise OSError("needs classicvoices-say and pw-play, paplay or aplay")
        args = [say, "-v", key, "-r", str(int(rate)), "-o", "-"]
        if not get_setting("PronunciationRules"):
            args.append("-n")
        p1 = subprocess.Popen(args + ["--", text], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        p2 = subprocess.Popen(sink, stdin=p1.stdout, stderr=subprocess.DEVNULL)
        p1.stdout.close()
        self._procs = [p1, p2]

    def stop(self):
        for p in self._procs:
            if p.poll() is None:
                p.kill()
        self._procs = []


def make_player():
    return WinPlayer() if IS_WIN else CliPlayer()

# ---------------------------------------------------------------- the system's own speech


def system_voice_count():
    """How many voices Windows programs (SAPI) or Speech Dispatcher can see."""
    if IS_WIN:
        n = 0
        for view in (winreg.KEY_WOW64_64KEY, winreg.KEY_WOW64_32KEY):
            try:
                with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\Microsoft\Speech\Voices\Tokens", 0,
                                    winreg.KEY_READ | view) as k:
                    i = 0
                    found = 0
                    while True:
                        try:
                            name = winreg.EnumKey(k, i)
                        except OSError:
                            break
                        found += name.startswith("ClassicVoices_")
                        i += 1
                    n = max(n, found)
            except OSError:
                pass
        return n
    return len(cv_voices.scan(voices_root(), downloads_root())) if speechd_module_installed() else 0


def sapi_dlls():
    """The SAPI engine DLLs the installer put next to the app: (dll, regsvr32)."""
    windir = os.environ.get("SystemRoot", r"C:\Windows")
    out = []
    d = install_dir()
    if os.path.isfile(os.path.join(d, "cvsapi64.dll")):
        out.append((os.path.join(d, "cvsapi64.dll"), os.path.join(windir, "System32", "regsvr32.exe")))
    if os.path.isfile(os.path.join(d, "cvsapi32.dll")):
        wow = os.path.join(windir, "SysWOW64", "regsvr32.exe")
        out.append((os.path.join(d, "cvsapi32.dll"),
                    wow if os.path.isfile(wow) else os.path.join(windir, "System32", "regsvr32.exe")))
    return out


def register_system_voices_elevated():
    """In the elevated helper (--register-sapi): register the SAPI DLLs, which
    rescans the voices. Returns a process exit code."""
    rc = 0
    for dll, regsvr in sapi_dlls():
        p = subprocess.run([regsvr, "/s", "/n", "/i:%s" % data_root(), dll])
        rc = rc or p.returncode
    return rc


def update_system_voices(wait=True):
    """Tell the system about the voices as they are now. Windows: register
    the SAPI engine again, as administrator (one UAC prompt). Linux: Speech
    Dispatcher's module rescans by itself. Returns (ok, message)."""
    if not IS_WIN:
        if not speechd_module_installed():
            return False, "The Speech Dispatcher module isn't installed."
        return True, "Speech Dispatcher sees the change by itself."
    if not sapi_dlls():
        return False, "The voices for Windows programs aren't installed with this copy of the app."

    class SHELLEXECUTEINFO(ctypes.Structure):
        _fields_ = [("cbSize", ctypes.c_uint), ("fMask", ctypes.c_ulong), ("hwnd", ctypes.c_void_p),
                    ("lpVerb", ctypes.c_wchar_p), ("lpFile", ctypes.c_wchar_p),
                    ("lpParameters", ctypes.c_wchar_p), ("lpDirectory", ctypes.c_wchar_p),
                    ("nShow", ctypes.c_int), ("hInstApp", ctypes.c_void_p), ("lpIDList", ctypes.c_void_p),
                    ("lpClass", ctypes.c_wchar_p), ("hkeyClass", ctypes.c_void_p),
                    ("dwHotKey", ctypes.c_ulong), ("hIcon", ctypes.c_void_p), ("hProcess", ctypes.c_void_p)]

    if FROZEN:
        exe, params = sys.executable, "--register-sapi"
    else:
        exe, params = sys.executable, '-m classicvoices --register-sapi'
    sei = SHELLEXECUTEINFO()
    sei.cbSize = ctypes.sizeof(sei)
    sei.fMask = 0x40  # SEE_MASK_NOCLOSEPROCESS
    sei.lpVerb, sei.lpFile, sei.lpParameters = "runas", exe, params
    sei.lpDirectory = os.path.dirname(HERE)
    sei.nShow = 0
    if not ctypes.windll.shell32.ShellExecuteExW(ctypes.byref(sei)):
        return False, "Windows didn't allow it (administrator permission is needed)."
    if wait and sei.hProcess:
        ctypes.windll.kernel32.WaitForSingleObject(ctypes.c_void_p(sei.hProcess), 0xFFFFFFFF)
        code = ctypes.c_ulong()
        ctypes.windll.kernel32.GetExitCodeProcess(ctypes.c_void_p(sei.hProcess), ctypes.byref(code))
        ctypes.windll.kernel32.CloseHandle(ctypes.c_void_p(sei.hProcess))
        if code.value:
            return False, "Registering the voices failed (code %d)." % code.value
    return True, "%d voices are available to Windows programs." % system_voice_count()


def speechd_module_path():
    return os.path.join(os.path.expanduser("~"), ".local", "libexec", "speech-dispatcher-modules",
                        "sd_classicvoices")


def speechd_module_installed():
    return not IS_WIN and os.path.exists(speechd_module_path())

# ---------------------------------------------------------------- fast code
# (built by the add-on package's fastcode module, which NVDA's add-on uses too)


def _recompiler_candidates():
    root, lib = bundle_dir(), os.path.dirname(HERE)  # repo or frozen app; Linux install (lib/classicvoices)
    return ((os.path.join(root, "tools"), os.path.join(root, "core"), os.path.join(root, "nvda", "src", "native_glue.c")),
            (os.path.join(lib, "tools"), os.path.join(lib, "core"), os.path.join(lib, "native_glue.c")))


def find_compiler():
    return cv_fastcode.find_compiler()


def download_zig(progress):
    return cv_fastcode.download_zig(progress)


def recompiler():
    """(tools dir, core dir, native_glue.c) of the recompiler bundled with the app."""
    return cv_fastcode.recompiler(_recompiler_candidates())


def build_fast_code(build, cc, progress):
    """Recompile one imported engine and compile it into this system's
    fast-code files. progress(message, fraction) may raise to cancel."""
    cv_fastcode.build(build, voices_root(), native_dir(), cc, progress, _recompiler_candidates(),
                      python=None if FROZEN else sys.executable)
