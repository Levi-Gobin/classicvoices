"""Publishing the person's dictionary changes through GitHub's own API, where git isn't installed (gitsync.py is
used where it is). The person signs in once with GitHub's device flow (a code typed at github.com/login/device;
the Classic Voices GitHub App, no password here); the key is kept in <data root>/github.dat, encrypted for this
Windows user (DPAPI); elsewhere (the Linux app) in a file only this user can read, in TOKEN_DIR when it's set.

Publishing makes the same commits as gitsync.py, one per changed file with the same message, each on the
repository's latest copy of the file (contents API: read it, make the person's changes in it, write it back
with the sha it had; written meanwhile by someone else: read again once). Someone who can't write to the
repository is offered their own copy of it (a fork, made here; asking for its changes to be taken in, a pull
request, is theirs to do on GitHub), and later publishes go there.

Environment overrides (tests): CV_GITHUB_CLIENT_ID, CV_GITHUB_APP_SLUG, CV_GITHUB_API, CV_GITHUB_WEB."""
import base64
import json
import os
import threading
import time
import urllib.error
import urllib.parse
import urllib.request

from . import dicts
from . import gitsync

# The Classic Voices GitHub App (github.com/apps/classic-voices)
CLIENT_ID = "Iv23liprOaNWuXD30i5d"  # the Classic Voices GitHub App (Nico's); not secret
APP_SLUG = "classic-voices"

UA = "ClassicVoices"
REFRESH_BEFORE = 300  # seconds before the key runs out that a new one is asked for

_lock = threading.Lock()


def client_id():
    return os.environ.get("CV_GITHUB_CLIENT_ID", CLIENT_ID)


def app_slug():
    return os.environ.get("CV_GITHUB_APP_SLUG", APP_SLUG)


def api_base():
    return os.environ.get("CV_GITHUB_API", "https://api.github.com").rstrip("/")


def web_base():
    return os.environ.get("CV_GITHUB_WEB", "https://github.com").rstrip("/")


def configured():
    """Whether this build can sign in to GitHub at all."""
    return bool(client_id())


def install_url():
    """Where the person allows the app on their own account (needed to make their copy)."""
    return "%s/apps/%s/installations/new" % (web_base(), app_slug())


class GitHubError(Exception):
    def __init__(self, status, message):
        super().__init__(message)
        self.status, self.message = status, message


class NotSignedIn(Exception):
    pass


class NeedsFork(Exception):
    """The person can't write to the repository: their own copy is the way."""


class NeedsInstall(Exception):
    """GitHub needs the app allowed on the person's account first."""

    def __init__(self, url):
        super().__init__(url)
        self.url = url


def _request(method, url, token=None, json_body=None, form=None, timeout=30):
    """(status, parsed JSON or None); GitHubError for an HTTP error."""
    headers = {"User-Agent": UA, "Accept": "application/json"}
    data = None
    if url.startswith(api_base()):
        headers["Accept"] = "application/vnd.github+json"
        headers["X-GitHub-Api-Version"] = "2022-11-28"
    if token:
        headers["Authorization"] = "Bearer " + token
    if json_body is not None:
        data = json.dumps(json_body).encode("utf-8")
        headers["Content-Type"] = "application/json"
    elif form is not None:
        data = urllib.parse.urlencode(form).encode("ascii")
        headers["Content-Type"] = "application/x-www-form-urlencoded"
    req = urllib.request.Request(url, data=data, headers=headers, method=method)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            body = r.read()
            status = r.status
    except urllib.error.HTTPError as e:
        body = e.read() or b""
        try:
            msg = json.loads(body.decode("utf-8")).get("message") or e.reason
        except (ValueError, AttributeError):
            msg = str(e.reason)
        raise GitHubError(e.code, msg)
    except (urllib.error.URLError, OSError) as e:
        raise GitHubError(0, str(getattr(e, "reason", e)))
    try:
        return status, json.loads(body.decode("utf-8")) if body else None
    except ValueError:
        return status, None


# ---------------------------------------------------------------- the key, kept for this Windows user

# Where the key goes, if not the data root: the Linux app keeps it in its config folder (~/.config/classicvoices)
TOKEN_DIR = None


def _token_path(data_root):
    return os.path.join(TOKEN_DIR or data_root, "github.dat")


if os.name == "nt":
    import ctypes
    from ctypes import wintypes

    class _Blob(ctypes.Structure):
        _fields_ = [("cbData", wintypes.DWORD), ("pbData", ctypes.POINTER(ctypes.c_char))]

    def _dpapi(data, protect):
        buf = ctypes.create_string_buffer(data, len(data))
        src = _Blob(len(data), ctypes.cast(buf, ctypes.POINTER(ctypes.c_char)))
        out = _Blob()
        crypt32 = ctypes.windll.crypt32
        fn = crypt32.CryptProtectData if protect else crypt32.CryptUnprotectData
        # CRYPTPROTECT_UI_FORBIDDEN: never a window
        if not fn(ctypes.byref(src), None, None, None, None, 0x1, ctypes.byref(out)):
            raise OSError("DPAPI failed")
        try:
            return ctypes.string_at(out.pbData, out.cbData)
        finally:
            ctypes.windll.kernel32.LocalFree(out.pbData)

    def _protect(b):
        return b"CVD1" + _dpapi(b, True)

    def _unprotect(b):
        if not b.startswith(b"CVD1"):
            raise ValueError("not ours")
        return _dpapi(b[4:], False)
else:  # Linux (and tests on a Mac): a file only this user can read
    def _protect(b):
        return b"CVP1" + b

    def _unprotect(b):
        if not b.startswith(b"CVP1"):
            raise ValueError("not ours")
        return b[4:]


def _load(data_root):
    try:
        with open(_token_path(data_root), "rb") as f:
            return json.loads(_unprotect(f.read()).decode("utf-8"))
    except (OSError, ValueError):
        return None


def _save(data_root, tok):
    p = _token_path(data_root)
    os.makedirs(os.path.dirname(p), mode=0o700, exist_ok=True)
    raw = _protect(json.dumps(tok).encode("utf-8"))
    fd = os.open(p + ".part", os.O_WRONLY | os.O_CREAT | os.O_TRUNC | getattr(os, "O_BINARY", 0), 0o600)
    if os.name != "nt":
        os.fchmod(fd, 0o600)  # a .part left over from before keeps its mode otherwise
    with os.fdopen(fd, "wb") as f:
        f.write(raw)
    os.replace(p + ".part", p)


def signed_in(data_root):
    return _load(data_root) is not None


def login(data_root):
    """The GitHub account signed in, or None."""
    t = _load(data_root)
    return t.get("login") if t else None


def sign_out(data_root):
    try:
        os.remove(_token_path(data_root))
    except OSError:
        pass
    st = gitsync._state(data_root)
    if st.pop("fork", None) is not None:
        gitsync._save_state(data_root, st)


def _store_grant(data_root, j, user=None):
    """Keeps a token answer (access key, its expiry, the refresh key)."""
    now = time.time()
    tok = {"access": j["access_token"], "login": user}
    if j.get("expires_in"):
        tok["expires"] = now + int(j["expires_in"])
    if j.get("refresh_token"):
        tok["refresh"] = j["refresh_token"]
    _save(data_root, tok)
    return tok


def access_token(data_root):
    """A key that works now (a new one asked for when it is about to run out); NotSignedIn when there is none or
    GitHub won't renew it."""
    with _lock:
        t = _load(data_root)
        if not t:
            raise NotSignedIn()
        if t.get("expires") and time.time() > t["expires"] - REFRESH_BEFORE:
            if not t.get("refresh"):
                sign_out(data_root)
                raise NotSignedIn()
            try:
                _s, j = _request("POST", web_base() + "/login/oauth/access_token",
                                 form={"client_id": client_id(), "grant_type": "refresh_token",
                                       "refresh_token": t["refresh"]})
            except GitHubError as e:
                if e.status and e.status < 500:
                    sign_out(data_root)
                    raise NotSignedIn()
                raise
            if not j or "access_token" not in j:  # bad_refresh_token and the like (sent with status 200)
                sign_out(data_root)
                raise NotSignedIn()
            t = _store_grant(data_root, j, t.get("login"))
        return t["access"]


# ---------------------------------------------------------------- signing in (device flow)

def start_sign_in():
    """Asks GitHub for a code: {device_code, user_code, verification_uri, interval, expires_in}."""
    _s, j = _request("POST", web_base() + "/login/device/code", form={"client_id": client_id()})
    if not j or "device_code" not in j:
        raise GitHubError(0, (j or {}).get("error_description") or "GitHub didn't give a code")
    return j


def finish_sign_in(data_root, code, cancelled=lambda: False):
    """Waits for the person to enter the code at GitHub; the account name, or None when cancelled. GitHubError
    when it was refused or the code ran out."""
    interval = max(1, int(code.get("interval", 5)))
    deadline = time.time() + int(code.get("expires_in", 900))
    while time.time() < deadline:
        for _i in range(interval * 10):
            if cancelled():
                return None
            time.sleep(0.1)
        try:
            _s, j = _request("POST", web_base() + "/login/oauth/access_token",
                             form={"client_id": client_id(), "device_code": code["device_code"],
                                   "grant_type": "urn:ietf:params:oauth:grant-type:device_code"})
        except GitHubError as e:
            if e.status == 0:  # the connection dropped (Wi-Fi, sleep): GitHub still has the code; ask again
                continue
            raise
        j = j or {}
        err = j.get("error")
        if err == "authorization_pending":
            continue
        if err == "slow_down":
            interval = int(j.get("interval", interval + 5))
            continue
        if err:
            raise GitHubError(0, j.get("error_description") or err)
        tok = _store_grant(data_root, j)
        for attempt in range(10):  # the key is good; a dropped connection mustn't lose it
            try:
                _s, user = _request("GET", api_base() + "/user", token=tok["access"])
                break
            except GitHubError as e:
                if e.status != 0 or attempt == 9:
                    raise
                time.sleep(2)
        tok["login"] = (user or {}).get("login")
        _save(data_root, tok)
        return tok["login"]
    raise GitHubError(0, "The code ran out before it was entered.")


# ---------------------------------------------------------------- publishing

def _fork(data_root):
    """The person's copy of the chosen repository, if they made one here."""
    f = gitsync._state(data_root).get("fork")
    if f and f.get("source") == dicts.source(data_root)[0] and f.get("login") == login(data_root):
        return f.get("repo")
    return None


def fork_name(data_root):
    return _fork(data_root)


def _get_file(token, repo, branch, name):
    """(sha, lines) of a file in the repository; (None, []) when it hasn't got it."""
    url = "%s/repos/%s/contents/%s?ref=%s" % (api_base(), repo, urllib.parse.quote(name),
                                              urllib.parse.quote(branch))
    try:
        _s, j = _request("GET", url, token=token)
    except GitHubError as e:
        if e.status == 404:
            return None, []
        raise
    if j.get("content"):
        text = base64.b64decode(j["content"]).decode("utf-8")
    else:  # over 1 MB: its content comes from download_url
        req = urllib.request.Request(j["download_url"], headers={"User-Agent": UA})
        with urllib.request.urlopen(req, timeout=60) as r:
            text = r.read().decode("utf-8")
    if text.startswith("﻿"):
        text = text[1:]
    return j["sha"], text.splitlines()


def _publish_file(token, repo, branch, name, a, c, r):
    """Writes one file's changes as one commit; the new text, or None when the repository has them already."""
    for attempt in range(2):
        sha, lines = _get_file(token, repo, branch, name)
        new = dicts.apply_changes(lines, a, c, r)
        if new == lines:
            return None
        text = "\n".join(new) + "\n"
        body = {"message": gitsync.message(name, a, c, r), "branch": branch,
                "content": base64.b64encode(text.encode("utf-8")).decode("ascii")}
        if sha:
            body["sha"] = sha
        try:
            _request("PUT", "%s/repos/%s/contents/%s" % (api_base(), repo, urllib.parse.quote(name)),
                     token=token, json_body=body)
            return text
        except GitHubError as e:
            # written meanwhile by someone else: once more on the new copy
            if attempt == 0 and (e.status == 409 or (e.status == 422 and "sha" in e.message.lower())):
                continue
            raise
    return None


def publish(data_root, force=False):
    """Publishes the person's changes, if it's time (or force): (files published, None, where to) or (0, why it
    couldn't, None). NotSignedIn, NeedsFork (they can't write there and have no copy yet) and NeedsInstall
    (their copy won't take it) are raised."""
    st = gitsync._state(data_root)
    if not force and time.time() - st.get("tried", 0) < gitsync.EVERY:
        return 0, None, None
    todo = gitsync.pending(data_root)
    if not todo:
        return 0, None, None
    token = access_token(data_root)
    source, branch = dicts.source(data_root)
    target = _fork(data_root) or source
    with gitsync._lock:
        st["tried"] = time.time()
        gitsync._save_state(data_root, st)
        made = 0
        try:
            for name, (a, c, r) in sorted(todo.items()):
                try:
                    text = _publish_file(token, target, branch, name, a, c, r)
                except GitHubError as e:
                    if e.status == 401:  # the key was refused: once more with a new one
                        token = access_token(data_root)
                        text = _publish_file(token, target, branch, name, a, c, r)
                    elif e.status in (403, 404) and not made:
                        if target == source:
                            raise NeedsFork()
                        raise NeedsInstall(install_url())
                    else:
                        raise
                if text is None:
                    continue
                made += 1
                if target == source:  # now the fetched copy; the person's published changes drop out of Mine
                    p = os.path.join(dicts.folder(data_root), name)
                    with open(p + ".part", "w", encoding="utf-8", newline="\n") as f:
                        f.write(text)
                    os.replace(p + ".part", p)
        except GitHubError as e:
            return made, e.message or "GitHub refused it (%d)" % e.status, target
        finally:
            if made and target == source:
                dicts.tidy(data_root)
        if made:
            st["published"] = time.time()
            gitsync._save_state(data_root, st)
        return made, None, target


def make_fork(data_root):
    """Makes the person's copy of the chosen repository and remembers it: its owner/name. NeedsInstall when GitHub
    needs the app allowed on their account first."""
    token = access_token(data_root)
    source, branch = dicts.source(data_root)
    try:
        _s, j = _request("POST", "%s/repos/%s/forks" % (api_base(), source), token=token,
                         json_body={"default_branch_only": True})
    except GitHubError as e:
        if e.status in (403, 404):
            raise NeedsInstall(install_url())
        raise
    repo = j["full_name"]
    # made in the background by GitHub: ready once its files can be read
    for _i in range(30):
        try:
            _request("GET", "%s/repos/%s/contents/?ref=%s" % (api_base(), repo, urllib.parse.quote(branch)),
                     token=token)
            break
        except GitHubError as e:
            if e.status not in (404, 409):
                raise
            time.sleep(2)
    st = gitsync._state(data_root)
    st["fork"] = {"source": source, "repo": repo, "login": login(data_root)}
    gitsync._save_state(data_root, st)
    return repo


def publish_in_background(data_root, force=False, done=None):
    """publish() on a thread (the daily one); done(count, error, target) on it at the end."""
    def run():
        try:
            n, err, target = publish(data_root, force)
        except NotSignedIn:
            n, err, target = 0, "not signed in to GitHub", None
        except (NeedsFork, NeedsInstall):
            n, err, target = 0, "no place to publish to on GitHub yet", None
        except Exception as e:  # never in NVDA's way
            n, err, target = 0, str(e) or e.__class__.__name__, None
        if done:
            done(n, err, target)
    threading.Thread(target=run, daemon=True).start()


def dont_ask(data_root):
    """Whether the person chose "Don't show again" for the sign-in question when closing the dialog."""
    return bool(gitsync._state(data_root).get("dontAskSignIn"))


def set_dont_ask(data_root):
    st = gitsync._state(data_root)
    st["dontAskSignIn"] = True
    gitsync._save_state(data_root, st)
