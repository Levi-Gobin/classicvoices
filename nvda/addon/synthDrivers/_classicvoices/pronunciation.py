# Apple's later pronunciation packs ("Newer pronunciation rules"): for each
# language a MobileAsset (VoiceServices.VoiceResources, compatibility version
# 8) holding a user dictionary and rule sets, which the engine loads on top of
# a voice's own data (vxs_set_rules_pack). Apple still serves them; all 39
# languages are about 5 MB, fetched together the first time the setting is
# turned on.
#
#   <data root>/Pronunciation/<language>/vocalizer-user-dict.dat
#                                        vocalizer-user-rules.txt
#                                        vocalizer-override-rules.txt
import hashlib
import os
import plistlib
import shutil
import urllib.request
import zipfile

UA = "classicvoices/1.0 (python-urllib)"
CATALOG = ("https://mesu.apple.com/assets/com_apple_MobileAsset_VoiceServices_VoiceResources/"
           "com_apple_MobileAsset_VoiceServices_VoiceResources.xml")
COMPATIBILITY = 8
FILES = ("vocalizer-user-dict.dat", "vocalizer-user-rules.txt", "vocalizer-override-rules.txt")


class PackError(Exception):
    pass


def packs_root(data_root):
    return os.path.join(data_root, "Pronunciation")


def pack_dir(data_root, language):
    """The pack folder for a language, or None when it isn't there."""
    if not language:
        return None
    d = os.path.join(packs_root(data_root), language)
    return d if os.path.isfile(os.path.join(d, "vocalizer-user-rules.txt")) else None


def have_packs(data_root):
    root = packs_root(data_root)
    return os.path.isdir(root) and any(pack_dir(data_root, l) for l in os.listdir(root))


def _get(url):
    return urllib.request.urlopen(urllib.request.Request(url, headers={"User-Agent": UA}), timeout=60)


def download_all(data_root, progress):
    """Fetch every language's pack from Apple. progress(message, fraction)."""
    progress("Getting the list of pronunciation rules from Apple", 0.0)
    with _get(CATALOG) as r:
        assets = [a for a in plistlib.loads(r.read()).get("Assets", [])
                  if a.get("_CompatibilityVersion") == COMPATIBILITY and a.get("Languages")]
    if not assets:
        raise PackError("Apple's list has no pronunciation rules")
    root = packs_root(data_root)
    os.makedirs(root, exist_ok=True)
    total = sum(a.get("_DownloadSize", 0) for a in assets) or len(assets)
    done = 0
    for a in assets:
        lang = a["Languages"][0]
        progress("Downloading pronunciation rules: %s" % lang, done / total)
        tmp = os.path.join(root, "_pack.zip")
        try:
            h = hashlib.sha1()
            with _get(a["__BaseURL"] + a["__RelativePath"]) as r, open(tmp, "wb") as f:
                for b in iter(lambda: r.read(1 << 16), b""):
                    f.write(b)
                    h.update(b)
            want = a.get("_Measurement")
            if want and h.digest() != bytes(want):
                raise PackError("SHA-1 mismatch for %s" % lang)
            staging = os.path.join(root, "_" + lang)
            shutil.rmtree(staging, ignore_errors=True)
            os.makedirs(staging)
            with zipfile.ZipFile(tmp) as z:
                for info in z.infolist():
                    name = info.filename.replace("\\", "/").split("/")[-1]
                    if name in FILES and "AssetData/" in info.filename.replace("\\", "/"):
                        with z.open(info) as src, open(os.path.join(staging, name), "wb") as out:
                            shutil.copyfileobj(src, out)
            dest = os.path.join(root, lang)
            shutil.rmtree(dest, ignore_errors=True)
            os.replace(staging, dest)
        finally:
            if os.path.exists(tmp):
                os.remove(tmp)
        done += a.get("_DownloadSize", 0) or 1
    progress("Pronunciation rules installed", 1.0)
