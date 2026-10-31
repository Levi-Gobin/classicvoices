"""What went wrong, written down: Python errors anywhere (any thread, any
button) and crashes in native code, which a windowed app has nowhere else to
show. log.txt is this run, log-old.txt the one before (like NVDA's
nvda-old.log), both in the user's own folder; Diagnostics shows them."""
import faulthandler
import os
import platform
import sys
import threading
import time
import traceback

_file = None
CLEAN_EXIT = "Classic Voices closed"


def folder():
    if sys.platform == "win32":
        return os.path.join(os.environ.get("LOCALAPPDATA") or os.path.expanduser("~"), "Classic Voices")
    base = os.environ.get("XDG_STATE_HOME") or os.path.join(os.path.expanduser("~"), ".local", "state")
    return os.path.join(base, "classicvoices")


def path(old=False):
    return os.path.join(folder(), "log-old.txt" if old else "log.txt")


def write(text):
    if _file:
        try:
            _file.write("%s %s\n" % (time.strftime("%H:%M:%S"), text))
        except (OSError, ValueError):
            pass


def _excepthook(kind, value, tb):
    write("Error:\n" + "".join(traceback.format_exception(kind, value, tb)).rstrip())
    if sys.stderr:
        sys.__excepthook__(kind, value, tb)


def _thread_excepthook(args):
    if args.exc_type is SystemExit:
        return
    write("Error in thread %s:\n%s" % (args.thread.name if args.thread else "?", "".join(
        traceback.format_exception(args.exc_type, args.exc_value, args.exc_traceback)).rstrip()))


def start():
    global _file
    try:
        os.makedirs(folder(), exist_ok=True)
        if os.path.exists(path()):
            os.replace(path(), path(old=True))
        _file = open(path(), "w", encoding="utf-8", buffering=1)
    except OSError:
        return
    write("Classic Voices started: Python %s, %s, %s" % (platform.python_version(), platform.platform(),
                                                         platform.machine()))
    # a crash in native code (an engine, the toolkit) writes where Python was
    faulthandler.enable(_file, all_threads=True)
    sys.excepthook = _excepthook  # wxPython reports a button's error through this
    threading.excepthook = _thread_excepthook
    if sys.stderr is None:  # a windowed app: whatever else is printed, here
        sys.stderr = _file


def stop():
    write(CLEAN_EXIT)


def report():
    """For Diagnostics: where the logs are, and what they say beyond the
    start line — the last run's too, which says if it ended in a crash."""
    lines = ["Log: %s" % path()]
    for old, name in ((True, "Last time"), (False, "This time")):
        try:
            with open(path(old), encoding="utf-8", errors="replace") as f:
                text = f.read().splitlines()
        except OSError:
            continue
        body = [t for t in text[1:] if not t.endswith(CLEAN_EXIT)]
        if old and text and not text[-1].endswith(CLEAN_EXIT):
            body.append("(Classic Voices didn't close normally: it crashed or was ended)")
        if body:
            lines.append("%s:" % name)
            lines += ["  " + t for t in body[-80:]]
    return "\n".join(lines)
