#!/usr/bin/env python3
"""CI driver for the fast-engine build (see .github/workflows/nvda-native.yml):
download each firmware from Apple, extract its engine with the add-on's own
importer, and hand the result to build_native.py. Windows only (the importer
decrypts through CNG). Nothing here publishes Apple's code: the workflow that
calls this encrypts its artifact with a password only the repository owner
knows, exactly like the iOS app's IPA build.

  python nvda/ci_native.py [--builds ios6 ...] [--work DIR]
"""
import argparse
import json
import os
import subprocess
import sys
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "nvda", "addon", "synthDrivers"))
from _classicvoices import firmware  # noqa: E402

VERSIONS = {  # build key -> (version for ipsw.me, device)
    "ios3": ("3.1.3", "iPhone2,1"),
    "ios4": ("4.3.5", "iPhone2,1"),
    "ios5": ("5.1.1", "iPhone2,1"),
    "ios6": ("6.1.6", "iPhone2,1"),
    "ios7": ("7.1.2", "iPhone3,1"),
    "ios10": ("10.3.4", "iPhone5,1"),
}


def ipsw_url(version, device):
    req = urllib.request.Request("https://api.ipsw.me/v4/ipsw/" + version,
                                 headers={"User-Agent": "classicvoices-ci"})
    with urllib.request.urlopen(req, timeout=60) as r:
        for fw in json.load(r):
            if fw["identifier"] == device:
                return fw["url"]
    sys.exit("no IPSW for %s %s on ipsw.me" % (device, version))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--builds", nargs="+", default=list(VERSIONS))
    ap.add_argument("--work", default=os.path.join(ROOT, "build", "ci-native"))
    a = ap.parse_args()
    voices = os.path.join(a.work, "Voices")
    out = os.path.join(a.work, "Native")
    os.makedirs(voices, exist_ok=True)
    for build in a.builds:
        version, device = VERSIONS[build]
        ipsw = os.path.join(a.work, "%s.ipsw" % build)
        if not os.path.exists(ipsw):
            url = ipsw_url(version, device)
            print("downloading", url, flush=True)
            urllib.request.urlretrieve(url, ipsw)
        firmware.import_ipsw(ipsw, voices,
                             lambda m, f: print(m, flush=True) if m else None)
        os.remove(ipsw)  # one firmware on disk at a time
    subprocess.check_call([sys.executable, os.path.join(ROOT, "nvda", "build_native.py"),
                           "--source", voices, "--out", out, "--builds"] + a.builds)


if __name__ == "__main__":
    main()
