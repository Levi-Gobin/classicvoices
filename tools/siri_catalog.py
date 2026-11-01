#!/usr/bin/env python3
"""Build SiriVoiceCatalog.json: iOS 7's Siri voices, from Apple's MobileAsset
catalog (com.apple.MobileAsset.VoiceServices.CustomVoice, the asset type
iOS 7's voiced downloads them as).

Only the iOS 7 postings (compatibility 1: iOS 7.0's US English, French and
German voices; 2: the voices iOS 7.1 added, British and Australian English,
Japanese, Chinese), Vocalizer 5.2.3, on 7.0.6's engine. Of the 7.1 voices
only the compacts open there: their premium voices' 2014 pmk data fails
ttsSetParamList (0x80000019) on 7.0.6 and speaks on 7.1.2, and the Mandarin
pair (Li-mu, Yu-shu) uses fe/fe_pw, the word segmenter 7.1 added. Those are
left out, as are the iOS 8 and iOS 10 postings.

Usage: siri_catalog.py <out.json> [catalog.xml]
"""
import json
import plistlib
import sys
import urllib.request

CATALOG = ("https://mesu.apple.com/assets/com_apple_MobileAsset_VoiceServices_CustomVoice/"
           "com_apple_MobileAsset_VoiceServices_CustomVoice.xml")


def main():
    out = sys.argv[1]
    if len(sys.argv) > 2:
        raw = open(sys.argv[2], "rb").read()
    else:
        with urllib.request.urlopen(CATALOG, timeout=120) as r:
            raw = r.read()
    voices = []
    for a in plistlib.loads(raw)["Assets"]:
        compat = a.get("_CompatibilityVersion")
        if compat not in ("1", "2") or a["Languages"][0] == "zh-CN" or (compat == "2" and a["Footprint"] != "compact"):
            continue
        m = a.get("_Measurement")
        voices.append({
            "name": a["Name"],
            "locale": a["Languages"][0],
            "gender": a.get("Gender", ""),
            "footprint": a["Footprint"],
            "url": a["__BaseURL"] + a["__RelativePath"],
            "size": a["_DownloadSize"],
            "unarchivedSize": a["_UnarchivedSize"],
            "sha1": m.hex() if isinstance(m, bytes) else "",
        })
    voices.sort(key=lambda v: (v["locale"], v["name"], v["footprint"]))
    json.dump({"voices": voices}, open(out, "w"), indent=1)
    print(f"{len(voices)} voices, {sum(v['size'] for v in voices) / 1e9:.1f} GB")


if __name__ == "__main__":
    main()
