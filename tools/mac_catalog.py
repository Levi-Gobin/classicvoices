#!/usr/bin/env python3
"""Build ios/Shared/MacVoiceCatalog.json: the downloadable Nuance voices that
Mac OS X 10.7-10.9 offered, from Apple's software-update catalog.

Each product's .dist names the voice (VOICEID tag) and the OS range it was
for (10.7 = Lion, 10.8 = Mountain Lion, 10.9 = Mavericks). A package holds the
premium voice and its compact sibling.

Usage: mac_catalog.py <out.json> [catalog-cache-dir]
"""
import concurrent.futures
import json
import os
import plistlib
import re
import sys
import urllib.request

CATALOG = ("https://swscan.apple.com/content/catalogs/others/"
           "index-10.9-mountainlion-lion-snowleopard-leopard.merged-1.sucatalog")
DISPLAY = {"sinji": "Sin-Ji", "meijia": "Mei-Jia", "tingting": "Ting-Ting", "yaling": "Ya-Ling"}
ERAS = {"10.7": "mac107", "10.8": "mac108", "10.9": "mac109"}


def fetch(url):
    with urllib.request.urlopen(url.replace("http://", "https://"), timeout=120) as r:
        return r.read()


def describe(item):
    pid, prod = item
    pkgs = prod.get("Packages", [])
    if len(pkgs) != 1:
        return None
    pkg = pkgs[0]
    name = pkg["URL"].rsplit("/", 1)[1]
    if not re.match(r"(MLV|MultiLingualVoice)_", name):
        return None
    dist = fetch(prod["Distributions"]["English"]).decode("utf-8", "replace")
    tag = re.search(r"<tag>(VOICEID:[^<]*;VERSION:[^<]*)</tag>", dist)
    mins = re.findall(r"compareVersions\(plistKeyValue, '([\d.]+)'\) &lt; 0", dist)
    maxs = re.findall(r"compareVersions\(plistKeyValue, '([\d.]+)'\) &gt;= 0", dist)
    if tag and mins:
        f = dict(kv.split(":", 1) for kv in tag.group(1).split(";") if ":" in kv)
    else:
        # Mavericks: MultiLingualVoice_<ll>_<CC>_<name>.pkg, version in BundleCheck
        m = re.match(r"MultiLingualVoice_([a-z]+)_([A-Za-z]+)_([a-z]+)\.pkg", name)
        vid = re.search(r"<tag>VOICEID:([^<]*premium[^<]*)</tag>", dist)
        ver = re.search(r"compareVersions\(bundleKeyValue, '([\d.]+)'\)", dist)
        mins = re.findall(r"ProductVersion, '([\d.]+)'\) &lt; 0", dist)[:1]
        maxs = re.findall(r"ProductVersion, '([\d.]+)'\) >= 0", dist)[:1]
        if not (m and vid and ver and mins):
            return None
        f = {"NAME": DISPLAY.get(m.group(3), m.group(3).capitalize()), "LOCALE": f"{m.group(1)}_{m.group(2)}",
             "VOICEID": vid.group(1), "VERSION": ver.group(1)}
    era = ERAS.get(mins[0])
    if not era:
        return None
    return {
        "product": pid, "era": era, "name": f["NAME"], "locale": f["LOCALE"].replace("_", "-"),
        "voiceID": f["VOICEID"], "version": f["VERSION"], "gender": f.get("GENDER", ""),
        "minOS": mins[0], "maxOS": maxs[0] if maxs else "",
        "url": pkg["URL"].replace("http://", "https://"), "size": pkg["Size"],
        "date": prod["PostDate"].strftime("%Y-%m-%d"),
    }


def main():
    out = sys.argv[1]
    cache = sys.argv[2] if len(sys.argv) > 2 else None
    path = cache and os.path.join(cache, os.path.basename(CATALOG))
    if path and os.path.exists(path):
        raw = open(path, "rb").read()
    else:
        raw = fetch(CATALOG)
        if path:
            open(path, "wb").write(raw)
    products = plistlib.loads(raw)["Products"]
    with concurrent.futures.ThreadPoolExecutor(16) as ex:
        rows = [r for r in ex.map(describe, products.items()) if r]
    # One entry per (era, voice): the same build is often posted twice
    # (e.g. MLV_en_AU_karen and MLV_en_karen2.0.0SU); keep the newest posting.
    best = {}
    for r in rows:
        k = (r["era"], re.sub(r"_\d$", "", r["voiceID"]))
        if k not in best or (r["version"], r["date"]) > (best[k]["version"], best[k]["date"]):
            best[k] = r
    voices = sorted(best.values(), key=lambda r: (r["era"], r["locale"], r["name"]))
    json.dump({"voices": voices}, open(out, "w"), indent=1)
    for era in ERAS.values():
        sel = [v for v in voices if v["era"] == era]
        print(f"{era}: {len(sel)} voices, {sum(v['size'] for v in sel) / 1e9:.1f} GB")


if __name__ == "__main__":
    main()
