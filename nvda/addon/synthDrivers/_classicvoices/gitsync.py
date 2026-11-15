"""Publishing the person's dictionary changes (dicts.py, Dictionaries/Mine) to the repository they come from,
with git: only where git is installed and its stored credentials may push there. Never asks for anything (no
password prompt, no window): if it can't push, the changes simply stay on this computer.

Every 24 hours (and from the dialog's Publish now), one commit per changed file, made on the repository's latest
copy (the person's changes are applied to it afresh, so edits made meanwhile elsewhere never conflict):

    en.dic: add 3 entries - nico, faouzia, boosted; modify 1 entry - colour; remove 1 entry - fibre

The work copy is <data root>/Dictionaries/.git-work. Once pushed, the files are the fetched copies and the
person's published changes drop out of Mine (dicts.tidy)."""
import json
import os
import shutil
import subprocess
import sys
import threading
import time

from . import dicts

EVERY = 24 * 3600
MAX_NAMES = 5  # in a commit's first line; the rest in its body

_lock = threading.Lock()


def find_git():
    """The git program, or None. On a Mac, Apple's /usr/bin/git only counts when the Command Line Tools are
    there (else it would open Apple's install dialog)."""
    if sys.platform == "darwin":
        for g in ("/opt/homebrew/bin/git", "/usr/local/bin/git"):
            if os.path.isfile(g):
                return g
        try:
            dev = subprocess.run(["/usr/bin/xcode-select", "-p"], capture_output=True, text=True, timeout=10)
            if dev.returncode == 0 and os.path.isdir(dev.stdout.strip()):
                return "/usr/bin/git"
        except (OSError, subprocess.SubprocessError):
            pass
        return None
    g = shutil.which("git")
    if g:
        return g
    if os.name == "nt":
        for base in (os.environ.get("ProgramFiles"), os.environ.get("ProgramW6432"), os.environ.get("LOCALAPPDATA")):
            for rel in (r"Git\cmd\git.exe", r"Programs\Git\cmd\git.exe"):
                p = os.path.join(base or "", rel)
                if base and os.path.isfile(p):
                    return p
    return None


def _env():
    e = dict(os.environ)
    e.update({"GIT_TERMINAL_PROMPT": "0", "GCM_INTERACTIVE": "never", "GIT_ASKPASS": "",
              "SSH_ASKPASS": "", "GIT_SSH_COMMAND": "ssh -o BatchMode=yes"})
    return e


def _run(git, args, cwd=None, timeout=120):
    flags = 0x08000000 if os.name == "nt" else 0  # CREATE_NO_WINDOW
    p = subprocess.run([git] + args, cwd=cwd, env=_env(), capture_output=True, text=True, timeout=timeout,
                       stdin=subprocess.DEVNULL, creationflags=flags)
    return p.returncode, (p.stdout or "") + (p.stderr or "")


def work_dir(data_root):
    return os.path.join(dicts.folder(data_root), ".git-work")


def _state_path(data_root):
    return os.path.join(dicts.folder(data_root), "publish.json")


def _state(data_root):
    try:
        with open(_state_path(data_root), encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError):
        return {}


def _save_state(data_root, st):
    p = _state_path(data_root)
    with open(p + ".part", "w", encoding="utf-8") as f:
        json.dump(st, f)
    os.replace(p + ".part", p)


def pending(data_root):
    """{file name: (added, changed, removed)} for every file the person changed."""
    out = {}
    for name in dicts.file_names(data_root):
        a, c, r = dicts.changes(data_root, name)
        if a or c or r:
            out[name] = (a, c, r)
    return out


def message(name, added, changed, removed):
    """The commit message: a first line naming up to MAX_NAMES patterns per kind, the full lists in the body."""
    def part(verb, entries):
        n = len(entries)
        names = [e.pattern for e in entries]
        shown = ", ".join(names[:MAX_NAMES]) + (" and %d more" % (n - MAX_NAMES) if n > MAX_NAMES else "")
        return "%s %d entr%s - %s" % (verb, n, "y" if n == 1 else "ies", shown)
    parts = [part(v, l) for v, l in (("add", added), ("modify", changed), ("remove", removed)) if l]
    subject = "%s: %s" % (name, "; ".join(parts))
    body = []
    for v, l in (("Added", added), ("Modified", changed), ("Removed", removed)):
        if len(l) > MAX_NAMES:
            body.append("%s:\n%s" % (v, "\n".join("- " + e.pattern for e in l)))
    return subject + ("\n\n" + "\n\n".join(body) if body else "")


def remote_url(data_root):
    return "https://github.com/%s.git" % dicts.source(data_root)[0]


def _prepare(git, data_root):
    """The work copy, cloned or brought to the repository's latest; (ok, text)."""
    branch = dicts.source(data_root)[1]
    url = remote_url(data_root)
    w = work_dir(data_root)
    if os.path.isdir(os.path.join(w, ".git")):
        rc, out = _run(git, ["remote", "get-url", "origin"], cwd=w)
        if rc or out.strip() != url:
            shutil.rmtree(w, ignore_errors=True)  # another repository was chosen
    if not os.path.isdir(os.path.join(w, ".git")):
        shutil.rmtree(w, ignore_errors=True)
        rc, out = _run(git, ["clone", "--quiet", "--branch", branch, url, w], timeout=300)
        if rc:
            return False, out
    for args in (["fetch", "--quiet", "origin", branch], ["checkout", "--quiet", "-B", branch, "origin/" + branch],
                 ["reset", "--quiet", "--hard", "origin/" + branch]):
        rc, out = _run(git, args, cwd=w)
        if rc:
            return False, out
    return True, ""


def can_push(data_root):
    """Whether git is here and may push to the chosen repository (a push that sends nothing)."""
    git = find_git()
    if not git:
        return False
    with _lock:
        ok, _out = _prepare(git, data_root)
        if not ok:
            return False
        rc, _out = _run(git, ["push", "--dry-run", "--quiet", "origin", "HEAD"], cwd=work_dir(data_root))
        return rc == 0


def publish(data_root, force=False):
    """Commits and pushes the person's changes, if it's time (or force). Returns (published file count, None)
    or (0, why it couldn't)."""
    st = _state(data_root)
    if not force and time.time() - st.get("tried", 0) < EVERY:
        return 0, None
    todo = pending(data_root)
    if not todo:
        return 0, None
    git = find_git()
    if not git:
        return 0, "git isn't installed"
    with _lock:
        st["tried"] = time.time()
        _save_state(data_root, st)
        for attempt in range(2):  # pushed meanwhile by someone else: once more on the new copy
            ok, out = _prepare(git, data_root)
            if not ok:
                return 0, out.strip()
            w = work_dir(data_root)
            made = 0
            for name, (a, c, r) in sorted(todo.items()):
                path = os.path.join(w, name)
                lines = dicts.read_lines(path)
                # the person's changes, made on the repository's latest copy of the file
                new = dicts.apply_changes(lines, a, c, r)
                if new == lines:
                    continue
                with open(path, "w", encoding="utf-8", newline="\n") as f:
                    f.write("\n".join(new) + "\n")
                rc, out = _run(git, ["add", "--", name], cwd=w)
                if not rc:
                    rc, out = _run(git, ["commit", "--quiet", "-m", message(name, a, c, r)], cwd=w)
                if rc:
                    return 0, out.strip()
                made += 1
            if not made:
                break
            rc, out = _run(git, ["push", "--quiet", "origin", "HEAD"], cwd=w, timeout=300)
            if rc == 0:
                # what was pushed is now the fetched copy; the person's published changes drop out of Mine
                for name in todo:
                    src = os.path.join(w, name)
                    if os.path.isfile(src):
                        shutil.copyfile(src, os.path.join(dicts.folder(data_root), name))
                dicts.tidy(data_root)
                st["published"] = time.time()
                _save_state(data_root, st)
                return made, None
            if attempt == 1 or "rejected" not in out and "fetch first" not in out:
                return 0, out.strip()
        return 0, None


def publish_in_background(data_root, force=False, done=None):
    """publish() on a thread; done(count, error) on it at the end."""
    def run():
        try:
            n, err = publish(data_root, force)
        except Exception as e:  # never in NVDA's way
            n, err = 0, str(e) or e.__class__.__name__
        if done:
            done(n, err)
    threading.Thread(target=run, daemon=True).start()
