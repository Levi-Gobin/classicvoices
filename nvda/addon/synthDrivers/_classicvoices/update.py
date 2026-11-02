"""Is there a newer Classic Voices? GitHub is asked at most once a day, in the
background, and each new version is offered once. If the person says yes, its
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
VERSION = "0.4.0"
RELEASES = "https://api.github.com/repos/Levi-Gobin/classicvoices/releases?per_page=10"
PAGE = "https://github.com/Levi-Gobin/classicvoices/releases"
DAY = 24 * 3600


def _number(v):
    return tuple(int(x) for x in re.findall(r"\d+", v or "")[:3])


def newer(state_dir, timeout=10):
    """(version, release page, {file name: asset}) of a release newer than this
    one that hasn't been announced yet, else None (also when GitHub can't be
    reached). An asset is {"url", "size", "sha256" (or None)}."""
    path = os.path.join(state_dir, "update.json")
    try:
        with open(path, encoding="utf-8") as f:
            state = json.load(f)
    except (OSError, ValueError):
        state = {}
    if time.time() - state.get("checked", 0) < DAY:
        return None
    try:
        req = urllib.request.Request(RELEASES, headers={"Accept": "application/vnd.github+json",
                                                        "User-Agent": "ClassicVoices/" + VERSION})
        with urllib.request.urlopen(req, timeout=timeout) as r:
            releases = json.load(r)
    except Exception:
        return None
    state["checked"] = time.time()
    found = None
    for rel in releases:  # the releases are marked pre-release, so not .../releases/latest
        v = (rel.get("tag_name") or "").lstrip("v")
        if rel.get("draft") or _number(v) <= _number(VERSION) or _number(v) <= _number(state.get("announced")):
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
