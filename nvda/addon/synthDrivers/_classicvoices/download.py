# Download voices from Apple's own servers, once the matching engine firmware
# is in place:
#  - enhanced iOS voices: MobileAsset zips on appldnld.apple.com
#    (EnhancedCatalog.json carries the URLs and SHA-1s; format 3 = iOS 5,
#    format 4 = iOS 6, format 5 = iOS 7)
#  - iOS 7's Siri voices: the same kind of zips (SiriVoiceCatalog.json, built
#    by tools/siri_catalog.py), into Downloads/siri7/<Name>[Compact]
#  - Mac OS X Lion / Mountain Lion / Mavericks voices: flat packages on
#    swcdn.apple.com (MacVoiceCatalog.json, built by tools/mac_catalog.py).
#    A package is xar -> Payload (gzip) -> cpio -> <Name>.SpeechVoice whose
#    PCMWave is a tar with ROT13 member names.
import codecs
import hashlib
import io
import json
import os
import re
import shutil
import struct
import tarfile
import urllib.request
import zipfile
import zlib

from . import applereaders
from . import voices as cv_voices

UA = "classicvoices-nvda/1.0 (python-urllib)"


class DownloadError(Exception):
    pass


def _catalog(name):
    with open(os.path.join(os.path.dirname(__file__), name), encoding="utf-8") as f:
        return json.load(f)


def enhanced_catalog():
    return _catalog("EnhancedCatalog.json")


def mac_catalog():
    return _catalog("MacVoiceCatalog.json")["voices"]


# The firmware a Mac era's voices need before they can be downloaded: the
# engine, and the firmware their common files come from (firmware.MAC_COMMON).
MAC_FIRMWARE = {"mac107": ("ios5", "ios6"), "mac108": ("ios6", cv_voices.IOS7), "mac109": (cv_voices.IOS7,)}


def suggested(f):
    """The firmware to get for a requirement: of the two iOS 7 ones, 7.0.6,
    which also has the iOS 7 voices."""
    return "ios70" if f == cv_voices.IOS7 else f


def _listing(voices_root, downloads_root):
    """[(kind, entry, label, size, firmware keys still to get)] of everything
    not installed yet."""
    from . import firmware
    wanted = firmware.wanted_languages(os.path.dirname(os.path.normpath(voices_root)))
    out = []
    for e in enhanced_catalog():
        build, lang = e["build"], e["language"]
        if not firmware.language_wanted(lang, wanted):
            continue
        dest = os.path.join(downloads_root, build, lang)
        if os.path.isfile(os.path.join(dest, "broker.hdr.asset")):
            continue
        needs = [] if cv_voices.has_engine(voices_root, build) else [build]
        label = "Enhanced %s (%s)" % (lang, dict(cv_voices.IOS_BUILDS)[build])
        out.append(("enhanced", e, label, e["size"], needs))
    for v in mac_catalog():
        era = v["era"]
        if not firmware.language_wanted(v.get("locale", ""), wanted):
            continue
        if os.path.isfile(os.path.join(downloads_root, era, v["name"], "broker.hdr")):
            continue
        needs = [suggested(f) for f in MAC_FIRMWARE[era] if not cv_voices.has_engine(voices_root, f)]
        if not needs and not os.path.isdir(os.path.join(voices_root, era, "common")):
            continue  # an import assembles the common files
        era_label = dict(cv_voices.MAC_ERAS)[era]
        label = "%s (%s, Mac OS X %s)" % (v["name"], v["locale"], era_label)
        out.append(("mac", v, label, v["size"], needs))
    from . import maccompacts
    for era, era_label in cv_voices.MAC_ERAS:
        # the compacts the release installed with the system, all in one download
        keep = lambda loc: firmware.language_wanted(loc, wanted)  # noqa: E731
        left = maccompacts.missing(era, downloads_root, keep)
        if not left:
            continue
        needs = [suggested(f) for f in MAC_FIRMWARE[era] if not cv_voices.has_engine(voices_root, f)]
        if not needs and not os.path.isdir(os.path.join(voices_root, era, "common")):
            continue
        label = "Compact voices of Mac OS X %s (%d)" % (era_label, len(left))
        out.append(("compacts", {"era": era}, label, maccompacts.download_size(era), needs))
    for s in cv_voices.siri_catalog():
        if not firmware.language_wanted(s["locale"], wanted):
            continue
        if os.path.isfile(os.path.join(downloads_root, cv_voices.SIRI, cv_voices.siri_folder(s), "broker.hdr")):
            continue
        needs = [] if cv_voices.has_engine(voices_root, cv_voices.SIRI_ENGINE) else [cv_voices.SIRI_ENGINE]
        kind = "compact" if s["footprint"] == "compact" else "enhanced"
        label = "%s (%s, iOS 7 Siri, %s)" % (s["name"], s["locale"], kind)
        out.append(("siri", s, label, s["size"], needs))
    return out


def available(voices_root, downloads_root):
    """Downloads whose engine firmware is present and that aren't installed
    yet: [(kind, entry, label, size)]."""
    return [x[:4] for x in _listing(voices_root, downloads_root) if not x[4]]


def needing(voices_root, downloads_root):
    """Downloads that wait for firmware: [(kind, entry, label, size, firmware
    keys to get first)]."""
    return [x for x in _listing(voices_root, downloads_root) if x[4]]


def _fetch(url, dest, size_hint, progress, sha1=None):
    h = hashlib.sha1() if sha1 else None
    req = urllib.request.Request(url, headers={"User-Agent": UA})
    with urllib.request.urlopen(req, timeout=60) as r, open(dest, "wb") as f:
        total = int(r.headers.get("Content-Length") or size_hint or 0)
        done = 0
        while True:
            b = r.read(1 << 20)
            if not b:
                break
            f.write(b)
            if h:
                h.update(b)
            done += len(b)
            progress(None, done / total if total else None)
    if h and sha1 and h.hexdigest().lower() != sha1.lower():
        os.remove(dest)
        raise DownloadError("SHA-1 mismatch for %s" % os.path.basename(url))


def _download_asset(entry, dest, broker, what, downloads_root, progress):
    """One MobileAsset zip -> dest (its AssetData, flattened), checked by SHA-1."""
    tmp = os.path.join(downloads_root, "_dl.zip")
    os.makedirs(downloads_root, exist_ok=True)
    progress("Downloading %s (%d MB)" % (what, entry["size"] >> 20), 0.0)
    try:
        _fetch(entry["url"], tmp, entry["size"], progress, sha1=entry.get("sha1"))
        progress("Unpacking %s" % what, None)
        if os.path.isdir(dest):
            shutil.rmtree(dest)
        os.makedirs(dest)
        with zipfile.ZipFile(tmp) as z:
            for info in z.infolist():
                parts = info.filename.replace("\\", "/").split("/")
                if info.is_dir() or "AssetData" not in parts:
                    continue
                name = parts[-1]
                if not name or set(name) & applereaders.ILLEGAL or ".." in name:
                    continue
                with z.open(info) as src, open(os.path.join(dest, name), "wb") as out:
                    shutil.copyfileobj(src, out, 1 << 20)
        if not os.path.isfile(os.path.join(dest, broker)):
            shutil.rmtree(dest)
            raise DownloadError("the archive had no " + broker)
    finally:
        if os.path.exists(tmp):
            os.remove(tmp)
    progress("%s%s installed" % (what[0].upper(), what[1:]), 1.0)


def download_enhanced(entry, downloads_root, progress):
    """One MobileAsset zip -> Downloads/<build>/<language>/ (flattened)."""
    dest = os.path.join(downloads_root, entry["build"], entry["language"])
    _download_asset(entry, dest, "broker.hdr.asset", "enhanced " + entry["language"], downloads_root, progress)


def download_siri(entry, downloads_root, progress):
    """An iOS 7 Siri voice -> Downloads/siri7/<Name>[Compact]/, with its language beside it."""
    dest = os.path.join(downloads_root, cv_voices.SIRI, cv_voices.siri_folder(entry))
    kind = "compact" if entry["footprint"] == "compact" else "enhanced"
    _download_asset(entry, dest, "broker.hdr", "%s (Siri, %s)" % (entry["name"], kind), downloads_root, progress)
    with open(os.path.join(dest, "locale"), "w", encoding="utf-8") as f:
        f.write(entry["locale"] + "\n")


def download_compacts(era, downloads_root, progress):
    """A Mac OS X release's compact voices (maccompacts) -> Downloads/<era>/<Name>Compact/,
    in the languages firmware imports take."""
    from . import firmware, maccompacts
    wanted = firmware.wanted_languages(os.path.dirname(os.path.normpath(downloads_root)))
    return maccompacts.fetch(era, downloads_root, progress, keep=lambda loc: firmware.language_wanted(loc, wanted))


def download(kind, entry, downloads_root, progress):
    """A download from available()/needing(), by its kind."""
    if kind == "enhanced":
        download_enhanced(entry, downloads_root, progress)
    elif kind == "siri":
        download_siri(entry, downloads_root, progress)
    elif kind == "compacts":
        download_compacts(entry["era"], downloads_root, progress)
    else:
        download_mac_voice(entry, downloads_root, progress)


def _xar_payload(f):
    """-> (offset, length) of the Payload member of a flat package file."""
    head = f.read(28)
    if head[:4] != b"xar!":
        raise DownloadError("not a flat package (no xar header)")
    hsize, _ver, toc_c, _toc_u, _ck = struct.unpack(">HHQQI", head[4:28])
    f.seek(hsize)
    toc = zlib.decompress(f.read(toc_c)).decode("utf-8", "replace")
    heap = hsize + toc_c
    for block in re.findall(r"<file\b.*?</file>", toc, re.S):
        if "<name>Payload</name>" not in block:
            continue
        off = re.search(r"<offset>(\d+)</offset>", block)
        length = re.search(r"<length>(\d+)</length>", block)
        if off and length:
            return heap + int(off.group(1)), int(length.group(1))
    raise DownloadError("the package has no Payload")


class _Slice(io.RawIOBase):
    """Sequential view of f[off:off+length]."""

    def __init__(self, f, off, length):
        self.f = f
        self.f.seek(off)
        self.left = length

    def readable(self):
        return True

    def read(self, n=-1):
        if n < 0 or n > self.left:
            n = self.left
        b = self.f.read(n)
        self.left -= len(b)
        return b

    def readinto(self, b):
        data = self.read(len(b))
        b[:len(data)] = data
        return len(data)


def _unpack_pcmwave(pcmwave_path, voice_dir):
    """PCMWave is a tar whose member names are ROT13 (tools/unpack_macvoice.py)."""
    os.makedirs(voice_dir, exist_ok=True)
    with tarfile.open(pcmwave_path) as t:
        for m in t.getmembers():
            if not m.isfile():
                continue
            name = codecs.decode(os.path.basename(m.name), "rot13")
            if not name or set(name) & applereaders.ILLEGAL or ".." in name:
                continue
            with t.extractfile(m) as f, open(os.path.join(voice_dir, name), "wb") as o:
                shutil.copyfileobj(f, o, 1 << 20)


def download_mac_voice(entry, downloads_root, progress):
    """One swcdn package -> Downloads/<era>/<Name>/ and <Name>Compact/."""
    era, name = entry["era"], entry["name"]
    tmp_pkg = os.path.join(downloads_root, "_dl.pkg")
    os.makedirs(downloads_root, exist_ok=True)
    progress("Downloading %s (%d MB)" % (name, entry["size"] >> 20), 0.0)
    installed = []
    try:
        _fetch(entry["url"], tmp_pkg, entry["size"], progress)
        progress("Unpacking %s" % name, None)
        with open(tmp_pkg, "rb") as f:
            off, length = _xar_payload(f)
            gz = zlib.decompressobj(16 + zlib.MAX_WBITS)
            sl = _Slice(f, off, length)

            class _Gunzip(io.RawIOBase):
                def __init__(self):
                    self.buf = b""

                def readable(self):
                    return True

                def read(self, n=-1):
                    while (n < 0 or len(self.buf) < n):
                        chunk = sl.read(1 << 20)
                        if not chunk:
                            self.buf += gz.flush()
                            break
                        self.buf += gz.decompress(chunk)
                    out = self.buf if n < 0 else self.buf[:n]
                    self.buf = self.buf[len(out):]
                    return out

                def readinto(self, b):
                    data = self.read(len(b))
                    b[:len(data)] = data
                    return len(data)

            stream = _Gunzip()
            want = re.compile(r"(?:^|/)([^/]+)\.SpeechVoice/Contents/Resources/PCMWave$")
            for member, _mode, size, read in applereaders.cpio_entries(stream):
                m = want.search(member)
                if not m:
                    continue
                folder = m.group(1)
                if set(folder) & applereaders.ILLEGAL or ".." in folder:
                    continue
                progress("Extracting %s (%d MB)" % (folder, size >> 20), None)
                tmp_tar = os.path.join(downloads_root, "_dl.tar")
                left = size
                with open(tmp_tar, "wb") as o:
                    while left:
                        b = read(min(left, 1 << 20))
                        if not b:
                            break
                        o.write(b)
                        left -= len(b)
                voice_dir = os.path.join(downloads_root, era, folder)
                if os.path.isdir(voice_dir):
                    shutil.rmtree(voice_dir)
                _unpack_pcmwave(tmp_tar, voice_dir)
                os.remove(tmp_tar)
                if os.path.isfile(os.path.join(voice_dir, "broker.hdr")):
                    installed.append(folder)
                else:
                    shutil.rmtree(voice_dir)
        if not installed:
            raise DownloadError("no voices found inside the package")
    finally:
        for p in (tmp_pkg, os.path.join(downloads_root, "_dl.tar")):
            if os.path.exists(p):
                os.remove(p)
    progress("%s installed" % " and ".join(installed), 1.0)
    return installed
