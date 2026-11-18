#!/usr/bin/env python3
"""The Windows and Linux app's dictionary publishing (desktop/classicvoices/dicteditor.py), end to end against a
stand-in GitHub on 127.0.0.1 and a local bare git repository: nothing goes to github.com.

    python3 tests/desktop_dicts_test.py        (needs wxPython; on a Mac or Linux)

Checks: the device-flow sign-in through the real SignInDialog (the key kept in TOKEN_DIR, 0600), publishing
through the contents API to a repository the person can't write to (the fork offer, then their copy), to one they
can (the fetched copy updated, Mine tidied), publishing with git to a local bare repository (git rewrites
https://github.com/ to it), the closing question's conditions and the daily publish."""
import base64
import json
import os
import shutil
import stat
import subprocess
import sys
import tempfile
import threading
import time
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WORK = tempfile.mkdtemp(prefix="cv-dicts-test-")
os.environ["CLASSICVOICES_DATA"] = os.path.join(WORK, "data")
os.environ["XDG_CONFIG_HOME"] = os.path.join(WORK, "config")
os.environ["CV_GITHUB_CLIENT_ID"] = "test-client"

# ---------------------------------------------------------------- the stand-in GitHub

ME = "tester"
repos = {}  # "owner/name" -> {path: text}
shas = {}  # ("owner/name", path) -> sha
polls = {"n": 0}
lock = threading.Lock()


def _sha(text):
    import hashlib
    return hashlib.sha1(text.encode("utf-8")).hexdigest()


class GitHub(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def _send(self, code, body):
        data = json.dumps(body).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def _body(self):
        n = int(self.headers.get("Content-Length") or 0)
        raw = self.rfile.read(n).decode("utf-8") if n else ""
        if self.headers.get("Content-Type", "").startswith("application/json"):
            return json.loads(raw or "{}")
        return dict(urllib.parse.parse_qsl(raw))

    def _authed(self):
        return self.headers.get("Authorization") == "Bearer key-1"

    def do_POST(self):
        u = urllib.parse.urlparse(self.path)
        b = self._body()
        if u.path == "/login/device/code":
            return self._send(200, {"device_code": "dev-1", "user_code": "ABCD-1234", "interval": 1,
                                    "expires_in": 60, "verification_uri": "http://127.0.0.1/login/device"})
        if u.path == "/login/oauth/access_token":
            if b.get("grant_type", "").endswith("device_code"):
                polls["n"] += 1
                if polls["n"] < 2:
                    return self._send(200, {"error": "authorization_pending"})
                return self._send(200, {"access_token": "key-1", "expires_in": 28800, "refresh_token": "r-1"})
            return self._send(200, {"error": "bad_refresh_token"})
        parts = u.path.strip("/").split("/")
        if len(parts) == 4 and parts[0] == "repos" and parts[3] == "forks":
            if not self._authed():
                return self._send(401, {"message": "Bad credentials"})
            src = parts[1] + "/" + parts[2]
            dst = ME + "/" + parts[2]
            with lock:
                repos[dst] = dict(repos[src])
                for p, t in repos[dst].items():
                    shas[(dst, p)] = _sha(t)
            return self._send(202, {"full_name": dst})
        self._send(404, {"message": "Not Found"})

    def do_GET(self):
        u = urllib.parse.urlparse(self.path)
        if u.path == "/user":
            return self._send(200, {"login": ME}) if self._authed() else self._send(401, {"message": "Bad credentials"})
        parts = u.path.strip("/").split("/", 4)
        if len(parts) >= 4 and parts[0] == "repos" and parts[3] == "contents":
            repo = parts[1] + "/" + parts[2]
            path = urllib.parse.unquote(parts[4]) if len(parts) > 4 else ""
            with lock:
                files = repos.get(repo)
                if files is None:
                    return self._send(404, {"message": "Not Found"})
                if not path:
                    return self._send(200, [{"name": p, "type": "file"} for p in files])
                if path not in files:
                    return self._send(404, {"message": "Not Found"})
                return self._send(200, {"sha": shas[(repo, path)],
                                        "content": base64.b64encode(files[path].encode("utf-8")).decode("ascii")})
        self._send(404, {"message": "Not Found"})

    def do_PUT(self):
        u = urllib.parse.urlparse(self.path)
        parts = u.path.strip("/").split("/", 4)
        b = self._body()
        if not self._authed():
            return self._send(401, {"message": "Bad credentials"})
        repo = parts[1] + "/" + parts[2]
        path = urllib.parse.unquote(parts[4])
        if parts[1] != ME:
            return self._send(403, {"message": "Resource not accessible by integration"})
        with lock:
            if path in repos[repo] and b.get("sha") != shas[(repo, path)]:
                return self._send(409, {"message": "sha mismatch"})
            text = base64.b64decode(b["content"]).decode("utf-8")
            repos[repo][path] = text
            shas[(repo, path)] = _sha(text)
            repos.setdefault("_commits", {})[repo + ":" + str(len(repos["_commits"]))] = b["message"]
        self._send(200, {"content": {"sha": shas[(repo, path)]}})


server = ThreadingHTTPServer(("127.0.0.1", 0), GitHub)
threading.Thread(target=server.serve_forever, daemon=True).start()
BASE = "http://127.0.0.1:%d" % server.server_address[1]
os.environ["CV_GITHUB_API"] = BASE
os.environ["CV_GITHUB_WEB"] = BASE

# ---------------------------------------------------------------- the app's code

sys.path.insert(0, os.path.join(ROOT, "desktop"))
import wx  # noqa: E402
from classicvoices import system  # noqa: E402
from classicvoices import dicteditor as de  # noqa: E402
from _classicvoices import dicts as cv_dicts  # noqa: E402

failures = []


def check(what, ok):
    print(("ok   " if ok else "FAIL ") + what)
    if not ok:
        failures.append(what)


data = system.data_root()
D = cv_dicts.folder(data)
os.makedirs(os.path.join(D, "Mine"), exist_ok=True)
BASEFILE = "colour\tcolor\t0\t2\nfibre\tfiber\t0\t2\nnico\tneeko\t0\t2\n"


def reset(source):
    """A fetched en.dic and three changes of the person's (one added, one changed, one removed)."""
    shutil.rmtree(D)
    os.makedirs(os.path.join(D, "Mine"))
    cv_dicts.set_source(data, source)
    with open(os.path.join(D, "en.dic"), "w", encoding="utf-8") as f:
        f.write(BASEFILE)
    cv_dicts.put(data, "en.dic", cv_dicts.Entry("youtu.be", "you tube", False, 0))
    cv_dicts.put(data, "en.dic", cv_dicts.Entry("colour", "kolor", False, 2), old_key=("colour", 2))
    cv_dicts.remove(data, "en.dic", ("nico", 2))


boxes, asks = [], []
de._box = lambda parent, frame, text, title, style=0: boxes.append((title, text))


def fake_ask(parent, frame, text, title, yes, no):
    asks.append(title)
    return True


de._ask = fake_ask
sign_ins = []
real_sign_in = de.sign_in


def counted_sign_in(parent, frame, d):
    sign_ins.append(1)
    return real_sign_in(parent, frame, d)


de.sign_in = counted_sign_in


class Frame(wx.Frame):
    last_voice = None

    def try_out(self, *a, **k):
        pass

    def stop_speaking(self):
        pass


app = wx.App()
frame = Frame(None)


def wait(cond, secs=30):
    end = time.time() + secs
    while time.time() < end and not cond():
        wx.Yield()
        time.sleep(0.02)
    return cond()


def run_publish(parent=None):
    done = {"n": len(boxes)}
    de.Publisher(frame, data).publish(parent)
    return wait(lambda: len(boxes) > done["n"], 60)


def steps():
    # 1. The API, to a repository the person can't write to: signing in (the real dialog, the stand-in GitHub
    #    authorises on the second poll), the fork offer, their copy.
    de.cv_git.find_git = lambda: None
    de._git_ok.clear()
    repos["upstream/dicts"] = {"en.dic": BASEFILE}
    shas[("upstream/dicts", "en.dic")] = _sha(BASEFILE)
    reset("upstream/dicts")
    ok = run_publish()
    check("API: sign-in asked for once", len(sign_ins) == 1)
    tok = os.path.join(os.environ["XDG_CONFIG_HOME"], "classicvoices", "github.dat")
    check("key kept in the config folder", os.path.isfile(tok))
    if os.path.isfile(tok):
        check("key file is 0600", stat.S_IMODE(os.stat(tok).st_mode) == 0o600)
        check("key folder is 0700", stat.S_IMODE(os.stat(os.path.dirname(tok)).st_mode) == 0o700)
        with open(tok, "rb") as f:
            check("key file has the CVP1 marker", f.read(4) == b"CVP1")
    check("not in the data folder", not os.path.exists(os.path.join(data, "github.dat")))
    check("signed in as tester", de.cv_gh.login(data) == ME)
    check("fork offered", asks == ["Publish to your own copy?"])
    check("published message", ok and boxes[-1][0] == "Dictionaries Published")
    print("     ", boxes[-1] if boxes else None)
    check("says it went to the copy", bool(boxes) and "Published to your copy, tester/dicts." in boxes[-1][1])
    fork = repos.get("tester/dicts", {}).get("en.dic", "")
    check("the copy has the changes",
          fork == "colour\tkolor\t0\t2\nfibre\tfiber\t0\t2\nyoutu.be\tyou tube\t0\t0\n")
    check("upstream untouched", repos["upstream/dicts"]["en.dic"] == BASEFILE)
    check("Mine kept (the source hasn't got them)", bool(de.cv_git.pending(data)))
    # publishing again: the copy has them
    ok = run_publish()
    check("again: nothing to publish to the copy", ok and boxes[-1] == (
        "Nothing to Publish", "Your copy, tester/dicts, has all your changes already."))

    # 2. The API, to a repository the person can write to: the fetched copy updated, Mine tidied.
    repos["tester/own"] = {"en.dic": BASEFILE}
    shas[("tester/own", "en.dic")] = _sha(BASEFILE)
    reset("tester/own")
    asks.clear()
    ok = run_publish()
    check("own repo: published", ok and boxes[-1] == ("Dictionaries Published",
                                                     "1 entry added, 1 changed and 1 removed in en.dic."))
    check("own repo: no fork offer", not asks)
    check("own repo: commit message", any(m == "en.dic: add 1 entry - youtu.be; modify 1 entry - colour; "
                                          "remove 1 entry - nico" for m in repos["_commits"].values()))
    with open(os.path.join(D, "en.dic"), encoding="utf-8") as f:
        check("own repo: fetched copy updated", f.read() == repos["tester/own"]["en.dic"])
    check("own repo: nothing pending", not de.cv_git.pending(data))
    ok = run_publish()
    check("nothing left: Nothing to Publish", ok and boxes[-1][0] == "Nothing to Publish")

    # 3. The closing question: only with changes, no git that can push, not signed in, not "don't ask".
    reset("tester/own")
    d = de.DictsDialog(frame)
    check("editing flag while open", os.path.exists(os.path.join(D, "editing")) and cv_dicts.EDITING)
    asked = []
    real_ask = de.AskSignInDialog.ShowModal
    de.AskSignInDialog.ShowModal = lambda self: asked.append(1) or wx.ID_NO
    d.finish()
    check("signed in: no question", not asked)
    check("flag gone after closing", not os.path.exists(os.path.join(D, "editing")) and not cv_dicts.EDITING)
    d.Destroy()
    de.cv_gh.sign_out(data)
    check("signed out: key removed", not de.cv_gh.signed_in(data))
    d = de.DictsDialog(frame)
    d.finish()
    d.Destroy()
    check("not signed in, changes: the question", len(asked) == 1)
    de.AskSignInDialog.ShowModal = lambda self: asked.append(1) or de.AskSignInDialog.DONT
    d = de.DictsDialog(frame)
    d.finish()
    d.Destroy()
    d = de.DictsDialog(frame)
    d.finish()
    d.Destroy()
    check("Don't show again holds", len(asked) == 2 and de.cv_gh.dont_ask(data))
    de.AskSignInDialog.ShowModal = real_ask

    # 4. Git, to a local bare repository standing in for github.com/tester/viagit.
    import importlib
    importlib.reload(de.cv_git)
    if de.cv_git.find_git() is None:
        check("git installed (for the git test)", False)
        return
    git = de.cv_git.find_git()
    bare = os.path.join(WORK, "remote", "tester", "viagit.git")
    seed = os.path.join(WORK, "seed")
    subprocess.check_call([git, "init", "-q", "--bare", "-b", "main", bare])
    subprocess.check_call([git, "init", "-q", "-b", "main", seed])
    with open(os.path.join(seed, "en.dic"), "w") as f:
        f.write(BASEFILE)
    env = dict(os.environ, GIT_AUTHOR_NAME="t", GIT_AUTHOR_EMAIL="t@t", GIT_COMMITTER_NAME="t",
               GIT_COMMITTER_EMAIL="t@t")
    subprocess.check_call([git, "-C", seed, "add", "en.dic"], env=env)
    subprocess.check_call([git, "-C", seed, "commit", "-qm", "seed"], env=env)
    subprocess.check_call([git, "-C", seed, "push", "-q", bare, "main"], env=env)
    # git sends https://github.com/... to the bare repository; nothing reaches GitHub
    os.environ.update({"GIT_CONFIG_COUNT": "3",
                       "GIT_CONFIG_KEY_0": "url.%s/.insteadOf" % os.path.join(WORK, "remote"),
                       "GIT_CONFIG_VALUE_0": "https://github.com",
                       "GIT_CONFIG_KEY_1": "user.name", "GIT_CONFIG_VALUE_1": "t",
                       "GIT_CONFIG_KEY_2": "user.email", "GIT_CONFIG_VALUE_2": "t@t"})
    de._git_ok.clear()
    reset("tester/viagit")
    check("git: can push to the stand-in", de.git_can_push(data) is True)
    ok = run_publish()
    check("git: published", ok and boxes[-1] == ("Dictionaries Published",
                                                "1 entry added, 1 changed and 1 removed in en.dic."))
    log = subprocess.run([git, "-C", bare, "log", "-1", "--format=%s"], capture_output=True, text=True).stdout.strip()
    check("git: commit message", log == "en.dic: add 1 entry - youtu.be; modify 1 entry - colour; remove 1 entry - nico")
    shown = subprocess.run([git, "-C", bare, "show", "main:en.dic"], capture_output=True, text=True).stdout
    check("git: file", shown == "colour\tkolor\t0\t2\nfibre\tfiber\t0\t2\nyoutu.be\tyou tube\t0\t0\n")
    check("git: nothing pending", not de.cv_git.pending(data))
    check("git: no sign-in asked", len(sign_ins) == 1)

    # 5. The daily publish (the switch): once a day, quietly.
    cv_dicts.put(data, "en.dic", cv_dicts.Entry("gif", "jif", False, 2))
    st = de.cv_git._state(data)
    st["tried"] = 0
    de.cv_git._save_state(data, st)
    de.publish_if_due(data)
    wait(lambda: not de.cv_git.pending(data), 60)
    shown = subprocess.run([git, "-C", bare, "show", "main:en.dic"], capture_output=True, text=True).stdout
    check("daily: published by itself", "gif\tjif\t0\t2" in shown)
    cv_dicts.put(data, "en.dic", cv_dicts.Entry("png", "ping", False, 2))
    de.publish_if_due(data)
    time.sleep(3)
    shown = subprocess.run([git, "-C", bare, "show", "main:en.dic"], capture_output=True, text=True).stdout
    check("daily: not twice a day", "png" not in shown)


try:
    steps()
finally:
    frame.Destroy()
    server.shutdown()
    shutil.rmtree(WORK, ignore_errors=True)
print("%d failed" % len(failures) if failures else "all passed")
sys.exit(1 if failures else 0)
