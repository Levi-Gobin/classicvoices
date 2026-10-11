#!/usr/bin/env python3
"""Download iOS voice MobileAssets of one FormatVersion from a catalog and verify
their SHA-1 (_Measurement). Usage: fetch_assets.py <catalog.xml> <format> <outdir>"""
import base64, hashlib, os, plistlib, sys, urllib.request
from concurrent.futures import ThreadPoolExecutor

cat, fmt, out = sys.argv[1], sys.argv[2], sys.argv[3]
os.makedirs(out, exist_ok=True)
assets = [a for a in plistlib.load(open(cat, "rb"))["Assets"] if a.get("FormatVersion") == fmt]

def fetch(a):
    lang = a.get("Language") or a.get("VoiceName", "?")
    url = a["__BaseURL"] + a["__RelativePath"]
    dest = os.path.join(out, f"{lang}.zip")
    want = a["_Measurement"]
    if os.path.exists(dest):
        h = hashlib.sha1(open(dest, "rb").read()).digest()
        if h == want:
            return f"{lang}: already have it"
    h = hashlib.sha1()
    with urllib.request.urlopen(url, timeout=60) as r, open(dest + ".part", "wb") as f:
        while True:
            b = r.read(1 << 20)
            if not b:
                break
            h.update(b); f.write(b)
    if h.digest() != want:
        return f"{lang}: SHA-1 MISMATCH"
    os.replace(dest + ".part", dest)
    return f"{lang}: ok {a['_DownloadSize'] / 1e6:.0f} MB"

with ThreadPoolExecutor(4) as ex:
    for msg in ex.map(fetch, assets):
        print(msg, flush=True)
print("done")
