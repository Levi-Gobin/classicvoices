"""Is there a newer Classic Voices? GitHub is asked at most once a day, in the
background, and each new version is announced once. Nothing is downloaded or
installed: the announcement offers the release page."""
import json
import os
import re
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
    """(version, release page) of a release newer than this one that hasn't
    been announced yet, else None (also when GitHub can't be reached)."""
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
            found = (v, rel.get("html_url") or PAGE)
    if found:
        state["announced"] = found[0]
    try:
        os.makedirs(state_dir, exist_ok=True)
        with open(path, "w", encoding="utf-8") as f:
            json.dump(state, f)
    except OSError:
        pass
    return found
