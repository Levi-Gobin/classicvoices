#!/usr/bin/env python3
"""Download the enhanced/premium voices of one language from ios/Shared/EnhancedCatalog.json
(iOS MobileAssets, SHA-1 checked) or ios/Shared/MacVoiceCatalog.json (Mac packages, size checked).
Usage: fetch_voices.py ios|mac <locale-prefix>   e.g. fetch_voices.py mac es"""
import hashlib, json, os, sys, urllib.request
from concurrent.futures import ThreadPoolExecutor

def get(url, dest, size=None, sha1=None):
    if os.path.exists(dest) and (size is None or os.path.getsize(dest) == size):
        if sha1 is None or hashlib.sha1(open(dest, "rb").read()).hexdigest() == sha1:
            return f"{os.path.basename(dest)}: already have it"
    h = hashlib.sha1()
    with urllib.request.urlopen(url.replace("http://swcdn", "https://swcdn"), timeout=120) as r, open(dest + ".part", "wb") as f:
        while True:
            b = r.read(1 << 20)
            if not b: break
            h.update(b); f.write(b)
    if sha1 and h.hexdigest() != sha1: return f"{os.path.basename(dest)}: SHA-1 MISMATCH"
    if size and os.path.getsize(dest + ".part") != size: return f"{os.path.basename(dest)}: SIZE MISMATCH"
    os.replace(dest + ".part", dest)
    return f"{os.path.basename(dest)}: ok {os.path.getsize(dest) / 1e6:.0f} MB"

jobs = []
prefix = sys.argv[2]
if sys.argv[1] == "ios":
    for a in json.load(open("ios/Shared/EnhancedCatalog.json")):
        if a["language"].startswith(prefix):
            jobs.append((a["url"], f"downloads/assets/{a['build']}_{a['language']}.zip", a["size"], a["sha1"]))
else:
    for v in json.load(open("ios/Shared/MacVoiceCatalog.json"))["voices"]:
        if v["locale"].startswith(prefix):
            jobs.append((v["url"], f"downloads/macvoices/{v['era']}_{v['name']}.pkg", v["size"], None))
with ThreadPoolExecutor(3) as ex:
    for msg in ex.map(lambda j: get(*j), jobs): print(msg, flush=True)
print("done")
