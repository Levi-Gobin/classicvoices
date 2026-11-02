# The Mac OS X compact voices of Lion, Mountain Lion and Mavericks, read out of
# Apple's installers with range requests (tools/mac_compacts.py made the
# recipe, MacCompacts.json, from local copies of them). Those releases
# installed their compacts with the system, so Apple's voice packages don't
# have them; only 25 to 40 MB of each installer is downloaded.
#
# The recipe is a chain of streams over the installer: the byte ranges to
# download ("fetch"), then disk images ("udif": zlib chunks) and files in them
# ("map": runs of bytes), ending at the part of Essentials.pkg's payload the
# voices are in. That part is entered in the middle of its compressed stream:
# Lion's gzip at a deflate block (with a dictionary the voices are proven not
# to use), Mountain Lion's and Mavericks' bzip2 at a block, wrapped as a
# stream of its own. Every voice is checked against its SHA-256.
#
# Shared by the NVDA add-on, the desktop app, the Linux tool and the build
# (tools/mac_compacts.py fetch). Mirrored by ios/MacApp/MacCompacts.swift and
# android/.../MacCompacts.java.
import bz2
import codecs
import hashlib
import io
import json
import os
import shutil
import tarfile
import threading
import urllib.request
import zlib

LION_URL = "https://updates.cdn-apple.com/2021/macos/041-7683-20210614-E610947E-C7CE-46EB-8860-D26D71F0D3EA/InstallMacOSX.dmg"
MOUNTAIN_LION_URL = "https://updates.cdn-apple.com/2021/macos/031-0627-20210614-90D11F33-1A65-42DD-BBEA-E1D9F43A6B3F/InstallMacOSX.dmg"
MAVERICKS_URL = "https://archive.org/download/macos-tutoriais-mac-os-x-mavericks-10-9-5/OS-X-Mavericks-10-9-5.iso"

UA = "classicvoices/1.0 (python-urllib)"
RAW, ZERO, IGNORE, ZLIB = 0x00000001, 0x00000000, 0x00000002, 0x80000005
BZ_END = 0x177245385090
ILLEGAL = set('<>:"/\\|?*\0')


class CompactsError(Exception):
    pass


_recipe = None


def recipe():
    global _recipe
    if _recipe is None:
        with open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "MacCompacts.json"), encoding="utf-8") as f:
            _recipe = json.load(f)["releases"]
    return _recipe


def eras():
    return sorted(recipe())


def voices(era):
    """Folder names the release's compacts get ("AgataCompact", ...)."""
    r = recipe()[era]
    return sorted(list(r["voices"]) + [f["voice"] for f in r["files"]])


def download_size(era):
    return sum(n for _o, n in recipe()[era]["fetch"])


def locale(era, voice):
    r = recipe()[era]
    info = r["voices"].get(voice) or next(f for f in r["files"] if f["voice"] == voice)
    return info["locale"]


def missing(era, root, keep=None):
    """The release's compacts (that `keep(locale)` wants) not in root/<era>/ yet."""
    return [v for v in voices(era) if (keep is None or keep(locale(era, v)))
            and not os.path.isfile(os.path.join(root, era, v, "broker.hdr"))]


MARKER = ".compacts"


def fetched_before(era, root):
    """Whether the release's compacts were fetched here once (voices removed
    since stay removed: nothing fetches them again by itself)."""
    return os.path.isfile(os.path.join(root, era, MARKER))


def _get(url, start, end):
    h = {"User-Agent": UA, "Range": "bytes=%d-%d" % (start, end)}
    return urllib.request.urlopen(urllib.request.Request(url, headers=h), timeout=120)


class _Fetched(object):
    """The recipe's byte ranges of the installer, downloaded into one file."""

    def __init__(self, url, spans, path, progress, total):
        self.spans, self.f = [], None
        at = 0
        for o, n in spans:
            self.spans.append((o, n, at))
            at += n
        with open(path, "wb") as f:
            f.truncate(at)
        done = [0]
        lock = threading.Lock()
        errors = []
        work = list(self.spans)

        def worker():
            while True:
                with lock:
                    if not work or errors:
                        return
                    o, n, local = work.pop(0)
                try:
                    got = 0
                    for attempt in range(4):
                        try:
                            with _get(url, o + got, o + n - 1) as r, open(path, "r+b") as out:
                                out.seek(local + got)
                                while got < n:
                                    c = r.read(min(1 << 20, n - got))
                                    if not c:
                                        break
                                    out.write(c)
                                    got += len(c)
                                    with lock:
                                        done[0] += len(c)
                                        d = done[0]
                                    progress(None, d / total)
                            if got == n:
                                break
                        except OSError:
                            if attempt == 3:
                                raise
                    if got != n:
                        raise CompactsError("the download was cut short")
                except BaseException as e:
                    with lock:
                        errors.append(e)
                    return

        threads = [threading.Thread(target=worker, daemon=True) for _ in range(min(6, len(work)))]
        for t in threads:
            t.start()
        for t in threads:
            t.join()
        if errors:
            raise errors[0]
        self.f = open(path, "rb")

    def read(self, off, n):
        out = bytearray()
        for o, ln, local in self.spans:
            a, b = max(off, o), min(off + n, o + ln)
            if a < b:
                self.f.seek(local + a - o)
                out += self.f.read(b - a)
        if len(out) != n:
            raise CompactsError("the recipe read outside what was downloaded")
        return bytes(out)

    def close(self):
        if self.f:
            self.f.close()


class _Stream(object):
    def __init__(self, spec, parent):
        self.parent = parent
        self.map = spec.get("map")
        self.udif = spec.get("udif")
        self._cache = (-1, b"")

    def _chunk(self, i):
        if self._cache[0] != i:
            kind, _o, ln, src, slen = self.udif[i]
            if kind in (ZERO, IGNORE):
                data = b"\0" * ln
            elif kind == RAW:
                data = self.parent.read(src, slen)
            elif kind == ZLIB:
                data = zlib.decompress(self.parent.read(src, slen))
            else:
                raise CompactsError("disk image chunk type %#x" % kind)
            self._cache = (i, data.ljust(ln, b"\0"))
        return self._cache[1]

    def read(self, off, n):
        out = bytearray()
        if self.map is not None:
            for v, ln, p in self.map:
                a, b = max(off, v), min(off + n, v + ln)
                if a < b:
                    out += self.parent.read(p + a - v, b - a)
        else:
            for i, (_k, o, ln, _s, _sl) in enumerate(self.udif):
                a, b = max(off, o), min(off + n, o + ln)
                if a < b:
                    out += self._chunk(i)[a - o:b - o]
        if len(out) != n:
            raise CompactsError("the recipe doesn't cover what it reads")
        return bytes(out)


def _streams(r, fetched):
    out = []
    for spec in r["streams"]:
        out.append(_Stream(spec, fetched if spec["of"] < 0 else out[spec["of"]]))
    return out


def _bits(data, a, b):
    """Bits a..b (MSB first) of `data` as an int."""
    big = int.from_bytes(data, "big")
    n = len(data) * 8
    return (big >> (n - b)) & ((1 << (b - a)) - 1)


def _payload(e, raw):
    """The voices' part of the payload, decompressed."""
    if e["codec"] == "gzip":
        s = e["shift"]
        shifted = (int.from_bytes(raw, "little") >> s).to_bytes(len(raw), "little") if s else raw
        z = zlib.decompressobj(-15, zdict=b"\0" * 32768)
        return z.decompress(shifted)
    a, b = e["bits"]
    body = (_bits(raw, a, b) << 80) | (BZ_END << 32) | e["crc"]
    n = (b - a) + 80
    pad = (-n) % 8
    stream = b"BZh" + bytes([0x30 + e["level"]]) + (body << pad).to_bytes((n + pad) // 8, "big")
    return bz2.decompress(stream)


def _cpio_pcmwaves(data, at):
    """{folder: PCMWave bytes} of the *Compact.SpeechVoice entries from `at` on."""
    out = {}
    while at + 76 <= len(data) and data[at:at + 6] == b"070707":
        nl, fs = int(data[at + 59:at + 65], 8), int(data[at + 65:at + 76], 8)
        if at + 76 + nl + fs > len(data):
            break
        name = data[at + 76:at + 75 + nl].decode("utf-8", "replace")
        parts = name.split("/")
        if len(parts) < 6 or not parts[5].endswith("Compact.SpeechVoice"):
            break
        if name.endswith("/Contents/Resources/PCMWave"):
            out[parts[5][:-len(".SpeechVoice")]] = data[at + 76 + nl:at + 76 + nl + fs]
        at += 76 + nl + fs
    return out


def _unpack(pcmwave, dest, locale):
    """PCMWave is a tar whose member names are ROT13."""
    tmp = dest + ".part"
    shutil.rmtree(tmp, ignore_errors=True)
    os.makedirs(tmp)
    with tarfile.open(fileobj=io.BytesIO(pcmwave)) as t:
        for m in t.getmembers():
            if not m.isfile():
                continue
            name = codecs.decode(os.path.basename(m.name), "rot13")
            if not name or set(name) & ILLEGAL or ".." in name:
                continue
            with t.extractfile(m) as f, open(os.path.join(tmp, name), "wb") as o:
                shutil.copyfileobj(f, o, 1 << 20)
    with open(os.path.join(tmp, "locale"), "w", encoding="utf-8") as f:
        f.write(locale + "\n")
    shutil.rmtree(dest, ignore_errors=True)
    os.replace(tmp, dest)


def fetch(era, root, progress=lambda msg, frac: None, keep=None):
    """Every compact voice of `era` ("mac107"...) whose language `keep(locale)`
    wants into root/<era>/<Name>Compact/. Returns the folders written."""
    r = recipe()[era]
    os.makedirs(os.path.join(root, era), exist_ok=True)
    tmp = os.path.join(root, era, ".compacts.download")
    total = download_size(era)
    progress("Downloading the %s compact voices (%d MB)" % (r["name"], total // 1000000), 0.0)
    fetched = _Fetched(r["url"], r["fetch"], tmp, progress, total)
    try:
        streams = _streams(r, fetched)
        e = r["essentials"]
        progress("Unpacking the %s compact voices" % r["name"], None)
        raw = streams[e["stream"]].read(e["from"], e["to"] - e["from"])
        found = _cpio_pcmwaves(_payload(e, raw), e["skip"])
        for f in r["files"]:
            found[f["voice"]] = streams[f["stream"]].read(0, f["size"])
        want = dict(r["voices"])
        want.update({f["voice"]: {"sha256": f["sha256"], "locale": f["locale"]} for f in r["files"]})
        for v, info in want.items():
            if v not in found or hashlib.sha256(found[v]).hexdigest() != info["sha256"]:
                raise CompactsError("%s from the %s installer isn't the one expected" % (v, r["name"]))
        want = {v: i for v, i in want.items() if keep is None or keep(i["locale"])}
        for v in sorted(want):
            if set(v) & ILLEGAL or ".." in v:
                continue
            _unpack(found[v], os.path.join(root, era, v), want[v]["locale"])
    finally:
        fetched.close()
        try:
            os.remove(tmp)
        except OSError:
            pass
    with open(os.path.join(root, era, MARKER), "w", encoding="utf-8") as f:
        f.write("\n".join(sorted(want)) + "\n")
    progress("The %s compact voices are ready" % r["name"], 1.0)
    return sorted(want)
