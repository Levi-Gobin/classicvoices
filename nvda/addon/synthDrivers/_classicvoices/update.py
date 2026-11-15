"""Is there a newer Classic Voices? GitHub is asked at most every 6 hours, in
the background (on the dev channel every hour), and each new version is
offered once (asked for, it is offered again). Two channels (channel.txt in the
state folder): stable, the numbered releases; dev, the build of the newest
commit on the dev branch (the pre-release tagged dev-latest, which CI replaces
on every push, with a dev.json naming its commit and time; this build's own are
in buildinfo.json beside this file). If the person says yes, its
file for this platform is downloaded (and checked against the SHA-256 GitHub
publishes for it) and handed to the platform's own installer: NVDA's add-on
installer, the Windows setup program, the Linux install script."""
import hashlib
import json
import os
import re
import tempfile
import time
import urllib.request

# The add-on's manifest, the Windows installer and the Android manifest carry
# the same number (nvda/build.py checks the manifest).
VERSION = "0.5.3"
RELEASES = "https://api.github.com/repos/Levi-Gobin/classicvoices/releases?per_page=10"
DEV_RELEASE = "https://api.github.com/repos/Levi-Gobin/classicvoices/releases/tags/dev-latest"
PAGE = "https://github.com/Levi-Gobin/classicvoices/releases"
EVERY = 6 * 3600
EVERY_DEV = 3600


def build_info():
    """{"commit", "time"} of this build (written by the build), or None (run from the repository)."""
    try:
        with open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "buildinfo.json"), encoding="utf-8") as f:
            info = json.load(f)
        return info if info.get("commit") else None
    except (OSError, ValueError):
        return None


def channel(state_dir):
    """"stable" or "dev". A build from the dev branch, the first time it runs, sets dev (someone who installs one
    wants its updates); the person's choice holds after that, until another dev build is put in."""
    info = build_info() or {}
    if info.get("channel") == "dev":
        mark = os.path.join(state_dir, "channel-build.txt")
        try:
            with open(mark, encoding="utf-8") as f:
                seen = f.read().strip()
        except OSError:
            seen = ""
        if seen != info["commit"]:
            try:
                set_channel(state_dir, "dev")
                with open(mark, "w", encoding="utf-8") as f:
                    f.write(info["commit"])
            except OSError:
                pass
    try:
        with open(os.path.join(state_dir, "channel.txt"), encoding="utf-8") as f:
            return "dev" if f.read().strip() == "dev" else "stable"
    except OSError:
        return "stable"


def set_channel(state_dir, value):
    os.makedirs(state_dir, exist_ok=True)
    with open(os.path.join(state_dir, "channel.txt"), "w", encoding="utf-8") as f:
        f.write("dev" if value == "dev" else "stable")


def _number(v):
    return tuple(int(x) for x in re.findall(r"\d+", v or "")[:3])


def newer(state_dir, timeout=10, name="update.json", asked=False):
    """(version, release page, {file name: asset}) of a release newer than this
    one that hasn't been announced yet, else None (also when GitHub can't be
    reached). An asset is {"url", "size", "sha256" (or None)}.
    name: the state file; the NVDA add-on and the app share state_dir when the
    app is installed, and each offers an update itself.
    asked: the person asked: GitHub is asked now, a version already announced
    is offered again, and GitHub not answering raises UpdateError."""
    path = os.path.join(state_dir, name)
    try:
        with open(path, encoding="utf-8") as f:
            state = json.load(f)
    except (OSError, ValueError):
        state = {}
    dev = channel(state_dir) == "dev"
    if not asked and time.time() - state.get("checked", 0) < (EVERY_DEV if dev else EVERY):
        return None
    if dev:
        return _newer_dev(path, state, timeout, asked)
    try:
        req = urllib.request.Request(RELEASES, headers={"Accept": "application/vnd.github+json",
                                                        "User-Agent": "ClassicVoices/" + VERSION})
        with urllib.request.urlopen(req, timeout=timeout) as r:
            releases = json.load(r)
    except Exception as e:
        if asked:
            raise UpdateError("GitHub couldn't be reached") from e
        return None
    state["checked"] = time.time()
    found = None
    for rel in releases:  # the releases are marked pre-release, so not .../releases/latest
        v = (rel.get("tag_name") or "").lstrip("v")
        if rel.get("draft") or _number(v) <= _number(VERSION) or (
                not asked and _number(v) <= _number(state.get("announced"))):
            continue
        if not found or _number(v) > _number(found[0]):
            assets = {}
            for a in rel.get("assets") or []:
                digest = a.get("digest") or ""
                assets[a.get("name")] = {"url": a.get("browser_download_url"), "size": a.get("size") or 0,
                                         "sha256": digest[7:].lower() if digest.startswith("sha256:") else None}
            found = (v, rel.get("html_url") or PAGE, assets)
    if found:
        state["announced"] = found[0]
    try:
        os.makedirs(state_dir, exist_ok=True)
        with open(path, "w", encoding="utf-8") as f:
            json.dump(state, f)
    except OSError:
        pass
    return found


def _assets(rel):
    assets = {}
    for a in rel.get("assets") or []:
        digest = a.get("digest") or ""
        assets[a.get("name")] = {"url": a.get("browser_download_url"), "size": a.get("size") or 0,
                                 "sha256": digest[7:].lower() if digest.startswith("sha256:") else None}
    return assets


def _newer_dev(path, state, timeout, asked):
    """The dev channel: the dev-latest build, when its commit is newer than this build's."""
    headers = {"Accept": "application/vnd.github+json", "User-Agent": "ClassicVoices/" + VERSION}
    try:
        with urllib.request.urlopen(urllib.request.Request(DEV_RELEASE, headers=headers), timeout=timeout) as r:
            rel = json.load(r)
        assets = _assets(rel)
        info_asset = assets.get("dev.json")
        if not info_asset:
            raise ValueError("no dev.json")
        with urllib.request.urlopen(urllib.request.Request(info_asset["url"], headers=headers), timeout=timeout) as r:
            info = json.load(r)
    except Exception as e:
        if asked:
            raise UpdateError("GitHub couldn't be reached") from e
        return None
    state["checked"] = time.time()
    mine = build_info() or {}
    found = None
    if (info.get("commit") and info.get("commit") != mine.get("commit")
            and float(info.get("time") or 0) > float(mine.get("time") or 0)
            and (asked or state.get("announced_dev") != info["commit"])):
        version = "%s-dev.%s" % (VERSION, info["commit"][:7])
        found = (version, rel.get("html_url") or PAGE, assets)
        state["announced_dev"] = info["commit"]
    try:
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w", encoding="utf-8") as f:
            json.dump(state, f)
    except OSError:
        pass
    return found


def download_path(version, name):
    """Where an update's file is downloaded: the temp folder."""
    return os.path.join(tempfile.gettempdir(), "classicvoices-%s-%s" % (version, name))


def remove_downloads(name):
    """Delete the update files earlier versions downloaded: installed or
    declined by now. One still in use (the setup that just opened this
    version is still ending) stays: True if any did. By extension: 0.4.0
    named the add-on classicvoices-<version>.nvda-addon."""
    ext = name[name.index("."):]
    left = False
    for f in os.listdir(tempfile.gettempdir()):
        if f.startswith("classicvoices-") and f.endswith((ext, ext + ".part")):
            try:
                os.remove(os.path.join(tempfile.gettempdir(), f))
            except OSError:
                left = True
    return left


class UpdateError(Exception):
    pass


def download(asset, dest, progress=None, timeout=60):
    """Fetch an asset (as newer gives it) to dest, checking its size and
    SHA-256; progress(fraction) as it goes."""
    h = hashlib.sha256()
    req = urllib.request.Request(asset["url"], headers={"User-Agent": "ClassicVoices/" + VERSION})
    tmp = dest + ".part"
    done = 0
    with urllib.request.urlopen(req, timeout=timeout) as r, open(tmp, "wb") as f:
        total = asset.get("size") or int(r.headers.get("Content-Length") or 0)
        while True:
            b = r.read(1 << 16)
            if not b:
                break
            f.write(b)
            h.update(b)
            done += len(b)
            if progress and total:
                progress(done / total)
    if asset.get("size") and done != asset["size"]:
        os.remove(tmp)
        raise UpdateError("the download was cut short")
    if asset.get("sha256") and h.hexdigest() != asset["sha256"]:
        os.remove(tmp)
        raise UpdateError("the download doesn't match what GitHub published")
    os.replace(tmp, dest)
    return dest
