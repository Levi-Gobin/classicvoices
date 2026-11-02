#!/usr/bin/env python3
"""The Mac OS X compact voices (Lion, Mountain Lion, Mavericks), straight from
Apple's installers without downloading them whole.

Lion, Mountain Lion and Mavericks installed their Nuance compact voices with
the system: they are in the installer's Essentials.pkg (and four of Lion's in
its BaseSystem.dmg), not in the premium voice packages on Apple's update
server. Every Essentials.pkg keeps them together in 25 to 40 MB of a 1.7 to 3 GB
compressed payload, so only that stretch is downloaded: a gzip stream (Lion)
is entered at a deflate block found in front of the voices, a bzip2 stream
(Mountain Lion, Mavericks) at a block of its own.

    recipe <installers dir> <out.json>
        Works out, from local copies of the three installers, which byte
        ranges of each one hold the voices and how to unpack them, with a
        SHA-256 per voice: nvda/.../MacCompacts.json and ios/Shared/MacCompacts.json.
        The JSON names only offsets, lengths and hashes, nothing of Apple's.
    fetch <out root> [era ...]
        Follows the recipe over the network into <root>/<era>/<Name>Compact/,
        as extracted/ has them (what the build bundles). Same code as the apps.

The installers: Lion and Mountain Lion are the InstallMacOSX.dmg files Apple
still offers; Mavericks is a copy of the 10.9.5 install disk on archive.org.
"""
import bz2
import hashlib
import json
import os
import re
import struct
import sys
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "nvda", "addon", "synthDrivers"))
from _classicvoices import applereaders as ar  # noqa: E402
from _classicvoices import maccompacts  # noqa: E402

RELEASES = [
    # era, name, file in the installers folder, URL, path chain
    ("mac107", "Lion", "InstallMacOSX10.7.dmg", maccompacts.LION_URL, "pkg"),
    ("mac108", "Mountain Lion", "InstallMacOSX10.8.dmg", maccompacts.MOUNTAIN_LION_URL, "pkg"),
    ("mac109", "Mavericks", "OS X Mavericks 10.9.5.iso", maccompacts.MAVERICKS_URL, "app"),
]
VOICES = "System/Library/Speech/Voices/"
# Nuance language codes (broker.hdr <langcode>), as VoiceCatalog.nuanceLanguages
NUANCE = {
    "ARW": "ar-SA", "CAH": "zh-HK", "CZC": "cs-CZ", "DAD": "da-DK", "DUB": "nl-BE", "DUN": "nl-NL",
    "ENA": "en-AU", "ENE": "en-IE", "ENG": "en-GB", "ENS": "en-GB", "ENU": "en-US", "ENZ": "en-ZA",
    "ENI": "en-IN", "FIF": "fi-FI", "FRC": "fr-CA", "FRF": "fr-FR", "GED": "de-DE", "GRG": "el-GR",
    "HII": "hi-IN", "HUH": "hu-HU", "IDI": "id-ID", "ITI": "it-IT", "JPJ": "ja-JP", "KOK": "ko-KR",
    "MNC": "zh-CN", "MNT": "zh-TW", "NON": "no-NO", "PLP": "pl-PL", "PTB": "pt-BR", "PTP": "pt-PT",
    "ROR": "ro-RO", "RUR": "ru-RU", "SKS": "sk-SK", "SPE": "es-ES", "SPM": "es-MX", "SWS": "sv-SE",
    "THT": "th-TH", "TRT": "tr-TR",
}


def locale_of(pcmwave):
    import codecs
    import io
    import tarfile
    with tarfile.open(fileobj=io.BytesIO(pcmwave)) as t:
        for m in t.getmembers():
            if codecs.decode(os.path.basename(m.name), "rot13") == "broker.hdr":
                xml = t.extractfile(m).read().decode("latin-1")
                code = re.search(r"<langcode>([^<]*)</langcode>", xml).group(1)
                return NUANCE[code]
    raise SystemExit("a voice without broker.hdr")


# ---- streams that remember what they were asked for --------------------
class Local(object):
    """The installer itself (stream -1 in the recipe)."""

    def __init__(self, path):
        self.f = open(path, "rb")
        self.size = os.path.getsize(path)
        self.used = []

    def need(self, off, n):
        self.used.append((off, n))

    def read(self, off, n):
        self.f.seek(off)
        return self.f.read(n)


class Map(object):
    """A file inside a volume: (offset in file, length, offset in parent) runs."""

    def __init__(self, parent, parts, size):
        self.parent, self.parts, self.size, self.used = parent, sorted(parts), size, set()

    def _runs(self, off, n):
        for i, (v, ln, p) in enumerate(self.parts):
            a, b = max(off, v), min(off + n, v + ln)
            if a < b:
                yield i, p + (a - v), b - a

    def need(self, off, n):
        for i, p, ln in self._runs(off, n):
            self.used.add(i)
            self.parent.need(p, ln)

    def read(self, off, n):
        return b"".join(self.parent.read(p, ln) for _i, p, ln in self._runs(off, n))


class Udif(object):
    """A compressed disk image: (kind, output offset, length, source offset, length) chunks."""

    def __init__(self, parent, chunks, size):
        self.parent, self.chunks, self.size, self.used = parent, sorted(chunks, key=lambda c: c[1]), size, set()
        self.starts = [c[1] for c in self.chunks]
        self._cache = (-1, b"")

    def _hits(self, off, n):
        import bisect
        i = max(0, bisect.bisect_right(self.starts, off) - 1)
        while i < len(self.chunks) and self.chunks[i][1] < off + n:
            _k, o, ln, _s, _sl = self.chunks[i]
            if off < o + ln:
                yield i
            i += 1

    def need(self, off, n):
        for i in self._hits(off, n):
            if i not in self.used:
                self.used.add(i)
                k, _o, _ln, s, sl = self.chunks[i]
                if k not in (ar.ZERO, ar.IGNORE):
                    self.parent.need(s, sl)

    def _chunk(self, i):
        if self._cache[0] != i:
            k, _o, ln, s, sl = self.chunks[i]
            if k in (ar.ZERO, ar.IGNORE):
                data = b"\0" * ln
            elif k == ar.RAW:
                data = self.parent.read(s, sl)
            elif k == ar.ZLIB:
                data = zlib.decompress(self.parent.read(s, sl))
            else:
                raise SystemExit("chunk kind %#x" % k)
            self._cache = (i, data.ljust(ln, b"\0"))
        return self._cache[1]

    def read(self, off, n):
        out = bytearray()
        for i in self._hits(off, n):
            _k, o, ln, _s, _sl = self.chunks[i]
            d = self._chunk(i)
            a, b = max(off, o), min(off + n, o + ln)
            out += d[a - o:b - o]
        return bytes(out)


class Adapter(object):
    """A stream as the file-like object applereaders wants."""

    def __init__(self, s):
        self.s, self.pos = s, 0

    def seek(self, off, whence=0):
        self.pos = off if whence == 0 else self.pos + off if whence == 1 else self.s.size + off
        return self.pos

    def tell(self):
        return self.pos

    def read(self, n=-1):
        if n is None or n < 0:
            n = self.s.size - self.pos
        n = max(0, min(n, self.s.size - self.pos))
        d = self.s.read(self.pos, n)
        self.pos += len(d)
        return d


def udif_of(parent, what):
    u = ar.UDIFReader(Adapter(parent), parent.size, what)
    return Udif(parent, [tuple(c) for c in u.chunks], u.size)


def volume_of(stream, what):
    a = Adapter(stream)
    base = ar.find_hfs(lambda o, n: (a.seek(o), a.read(n))[1], stream.size, what)
    return ar.Volume(a, base)


def file_of(vol, parent, path, start=0, length=None):
    """A file of an HFS+ volume (from byte `start`, `length` long) as a Map over `parent`."""
    e = vol.entry(path)
    if e is None or e[4] is None:
        raise SystemExit("%s not found" % path)
    parts, v = [], 0
    for blk, cnt in e[4]:
        parts.append((v, cnt * vol.bs, vol.base + blk * vol.bs))
        v += cnt * vol.bs
    size = e[3] if length is None else length
    if start:
        parts = [(pv - start, ln, p) for pv, ln, p in parts]
        shifted = []
        for pv, ln, p in parts:  # cut the part that starts before `start`
            if pv < 0:
                if pv + ln <= 0:
                    continue
                p, ln, pv = p - pv, ln + pv, 0
            shifted.append((pv, ln, p))
        parts = shifted
    return Map(parent, [(pv, min(ln, size - pv), p) for pv, ln, p in parts if pv < size], size)


def xar_member(stream, name):
    head = stream.read(0, 1 << 16)
    hsize, _v, toc_c, _u, _ck = struct.unpack(">HHQQI", head[4:28])
    toc = zlib.decompress(stream.read(hsize, toc_c)).decode("utf-8", "replace")
    for block in re.findall(r"<file\b.*?</file>", toc, re.S):
        if "<name>%s</name>" % name in block:
            return (hsize + toc_c + int(re.search(r"<offset>(\d+)</offset>", block).group(1)),
                    int(re.search(r"<length>(\d+)</length>", block).group(1)))
    raise SystemExit("no %s in the package" % name)


def sub(m, off, n):
    """Bytes off..off+n of a Map, as a Map over the same parent."""
    parts = []
    for v, ln, p in m.parts:
        a, b = max(off, v), min(off + n, v + ln)
        if a < b:
            parts.append((a - off, b - a, p + (a - v)))
    return Map(m.parent, parts, n)


# ---- finding the voices in a payload --------------------------------------
def cpio_voices(data, at):
    """{folder: PCMWave bytes} for the *Compact.SpeechVoice entries from `at`
    on, and whether they ended (at an entry that isn't one of them)."""
    out = {}
    while at + 76 <= len(data) and data[at:at + 6] == b"070707":
        nl, fs = int(data[at + 59:at + 65], 8), int(data[at + 65:at + 76], 8)
        if at + 76 + nl > len(data):
            break
        name = data[at + 76:at + 75 + nl].decode("utf-8", "replace")
        m = re.match(r"\./" + VOICES + r"([^/]+Compact)\.SpeechVoice(/|$)", name)
        if not m:
            return out, True
        if at + 76 + nl + fs > len(data):
            break
        if name.endswith("/Contents/Resources/PCMWave"):
            out[m.group(1)] = data[at + 76 + nl:at + 76 + nl + fs]
        at += 76 + nl + fs
    return out, False


def first_compact(data):
    m = re.search(rb"070707[0-7]{70}\./" + VOICES.encode() + rb"[^/\0]+Compact\.SpeechVoice\0", data)
    return m.start() if m else -1


def gzip_entry(payload, approx):
    """Lion: a deflate block in front of the voices, entered with a dictionary
    of unknowns; proven by decoding with two different ones."""
    lo = max(0, approx - (512 << 10))
    hi = min(payload.size, approx + (32 << 20))
    raw = payload.read(lo, hi - lo)
    big = int.from_bytes(raw, "little")
    shifted = [(big >> s).to_bytes(len(raw), "little") for s in range(8)]
    d1 = bytes((i * 131 + 7) & 0xFF for i in range(32768))
    d2 = bytes(x ^ 0xFF for x in d1)
    for i in range(0, 2 << 20):
        for s in range(8):
            b = shifted[s]
            if (b[i] >> 1) & 3 not in (1, 2):
                continue
            try:
                o = zlib.decompressobj(-15, zdict=d1)
                a = o.decompress(b[i:i + (4 << 20)])
            except zlib.error:
                continue
            if len(a) < (1 << 20):
                continue
            p = first_compact(a)
            if p < 0:
                continue
            a2 = zlib.decompressobj(-15, zdict=d2).decompress(b[i:i + (4 << 20)])
            if a[p:] != a2[p:]:
                continue
            # decode on until the voices end, and note how much input that took
            o1, o2 = zlib.decompressobj(-15, zdict=d1), zlib.decompressobj(-15, zdict=d2)
            out1 = out2 = b""
            used = i
            while used < len(b):
                c = b[used:used + 65536]
                out1 += o1.decompress(c)
                out2 += o2.decompress(c)
                used += len(c)
                if cpio_voices(out1, p)[1]:
                    break
            if out1[p:] != out2[p:]:
                continue
            return {"codec": "gzip", "from": lo + i, "to": lo + used + 1, "shift": s, "skip": p}, out1
    raise SystemExit("no deflate block in front of the voices")


BZ_MAGIC = 0x314159265359
BZ_END = 0x177245385090


def bz_blocks(raw):
    """Bit offsets of every bzip2 block header in `raw`."""
    big = int.from_bytes(raw, "big")
    nbits = len(raw) * 8
    out = []
    for s in range(8):
        # the 48-bit magic at bit offset 8k+s: find its middle bytes, then check
        pat = ((BZ_MAGIC << (8 - s)) & ((1 << 56) - 1)) if s else BZ_MAGIC << 8
        key = pat.to_bytes(7, "big")[1:6]
        j = raw.find(key)
        while j >= 0:
            bit = (j - 1) * 8 + s
            if bit >= 0 and bit + 48 <= nbits and (big >> (nbits - bit - 48)) & ((1 << 48) - 1) == BZ_MAGIC:
                out.append(bit)
            j = raw.find(key, j + 1)
    return sorted(set(out))


def bz_wrap(raw, a, b, level):
    """A bzip2 stream of the blocks between bit offsets a and b of `raw`."""
    big = int.from_bytes(raw, "big")
    nbits = len(raw) * 8
    blocks = (big >> (nbits - b)) & ((1 << (b - a)) - 1)
    crc = 0
    for bit in bz_blocks(raw):
        if a <= bit < b:
            c = (big >> (nbits - bit - 80)) & 0xFFFFFFFF
            crc = (((crc << 1) | (crc >> 31)) & 0xFFFFFFFF) ^ c
    body = (blocks << 80) | (BZ_END << 32) | crc
    n = (b - a) + 80
    pad = (-n) % 8
    return b"BZh" + bytes([0x30 + level]) + (body << pad).to_bytes((n + pad) // 8, "big"), crc


def bzip2_entry(payload, approx):
    lo = max(0, approx - (3 << 20))
    hi = min(payload.size, approx + (48 << 20))
    level = payload.read(0, 4)[3] - 0x30
    raw = payload.read(lo, hi - lo)
    marks = bz_blocks(raw)
    for k, start in enumerate(marks):
        # the first block whose output holds the start of the voices
        stream, _crc = bz_wrap(raw, start, marks[k + 1], level)
        if first_compact(bz2.decompress(stream)) >= 0 or k + 2 >= len(marks):
            break
    k = max(0, k - 1)  # one block earlier, so a header split across blocks is whole
    for m in range(k + 2, len(marks)):
        stream, crc = bz_wrap(raw, marks[k], marks[m], level)
        out = bz2.decompress(stream)
        p = first_compact(out)
        if p >= 0 and cpio_voices(out, p)[1]:
            return {"codec": "bzip2", "level": level, "from": lo + marks[k] // 8, "to": lo + (marks[m] + 7) // 8 + 1,
                    "bits": [marks[k] % 8 + 0, marks[k] % 8 + (marks[m] - marks[k])], "crc": crc, "skip": p}, out
    raise SystemExit("the voices run past the end of what was read")


def approx_start(payload, codec):
    """Where in the compressed payload the voices start: read through it once."""
    z = zlib.decompressobj(31) if codec == "gzip" else bz2.BZ2Decompressor()
    tail, pos, step = b"", 0, 1 << 20
    key = b"./" + VOICES.encode()
    while pos < payload.size:
        c = payload.read(pos, min(step, payload.size - pos))
        pos += len(c)
        tail = (tail[-4096:] + z.decompress(c))
        if re.search(re.escape(key) + rb"[^/\0]+Compact\.SpeechVoice\0", tail):
            return pos - len(c)
    raise SystemExit("no compact voices in Essentials.pkg")


# ---- the recipe -------------------------------------------------------------
def recipe_for(era, name, path, url, chain):
    local = Local(path)
    streams = [local]
    if chain == "pkg":
        outer = udif_of(local, name)
        streams.append(outer)
        vol = volume_of(outer, name)
        pkg = file_of(vol, outer, "InstallMacOSX.pkg")
        off, ln = xar_member(pkg, "InstallESD.dmg")
        esd_file = sub(pkg, off, ln)
    else:
        vol = volume_of(local, name)
        esd_file = file_of(vol, local, "Install OS X Mavericks.app/Contents/SharedSupport/InstallESD.dmg")
    streams.append(esd_file)
    esd = udif_of(esd_file, "InstallESD.dmg")
    streams.append(esd)
    esd_vol = volume_of(esd, "InstallESD.dmg")
    ess = file_of(esd_vol, esd, "Packages/Essentials.pkg")
    off, ln = xar_member(ess, "Payload")
    payload = sub(ess, off, ln)
    streams.append(payload)
    codec = "gzip" if payload.read(0, 2) == b"\x1f\x8b" else "bzip2"
    approx = approx_start(payload, codec)
    entry, out = (gzip_entry if codec == "gzip" else bzip2_entry)(payload, approx)
    voices, ended = cpio_voices(out, entry["skip"])
    assert ended
    payload.need(entry["from"], entry["to"] - entry["from"])
    files = []
    if chain == "pkg":
        # Lion keeps four of its compacts only in BaseSystem.dmg
        bs_file = file_of(esd_vol, esd, "BaseSystem.dmg")
        bs = udif_of(bs_file, "BaseSystem.dmg")
        bs_vol = volume_of(bs, "BaseSystem.dmg")
        folder = bs_vol.entry(VOICES.rstrip("/"))
        streams += [bs_file, bs]
        for kid in bs_vol.children(folder):
            m = re.match(r"(.+Compact)\.SpeechVoice$", kid[0])
            if not m or m.group(1) in voices:
                continue
            f = file_of(bs_vol, bs, VOICES + kid[0] + "/Contents/Resources/PCMWave")
            data = f.read(0, f.size)
            f.need(0, f.size)
            streams.append(f)
            files.append({"voice": m.group(1), "stream": len(streams) - 1, "size": f.size,
                          "sha256": hashlib.sha256(data).hexdigest(), "locale": locale_of(data)})
    if not files:
        streams = [x for x in streams if x is not bs_file and x is not bs] if chain == "pkg" else streams
    # number the streams; write out only the parts and chunks that were used
    index = {id(s): i - 1 for i, s in enumerate(streams)}
    out_streams = []
    for s in streams[1:]:
        if isinstance(s, Map):
            parts = [list(s.parts[i]) for i in sorted(s.used)]
            out_streams.append({"of": index[id(s.parent)], "size": s.size, "map": parts})
        else:
            chunks = [list(s.chunks[i]) for i in sorted(s.used)]
            out_streams.append({"of": index[id(s.parent)], "size": s.size, "udif": chunks})
    # files' stream numbers were positions in `streams`; make them recipe indices
    for f in files:
        f["stream"] -= 1
    spans = []
    for o, n in sorted(local.used):
        if spans and o <= spans[-1][0] + spans[-1][1]:
            spans[-1][1] = max(spans[-1][1], o + n - spans[-1][0])
        else:
            spans.append([o, n])
    entry["stream"] = index[id(payload)]
    return {
        "name": name, "url": url, "size": local.size, "fetch": spans, "streams": out_streams,
        "essentials": entry,
        "voices": {v: {"sha256": hashlib.sha256(d).hexdigest(), "locale": locale_of(d)}
                   for v, d in sorted(voices.items())},
        "files": files,
    }


def main():
    if len(sys.argv) >= 4 and sys.argv[1] == "recipe":
        src, out = sys.argv[2], sys.argv[3]
        recipe = {}
        for era, name, fn, url, chain in RELEASES:
            r = recipe_for(era, name, os.path.join(src, fn), url, chain)
            n = len(r["voices"]) + len(r["files"])
            mb = sum(s[1] for s in r["fetch"]) / 1e6
            print("%s %s: %d voices, %.1f MB to download in %d ranges" % (era, name, n, mb, len(r["fetch"])))
            recipe[era] = r
        with open(out, "w") as f:
            json.dump({"version": 1, "releases": recipe}, f, separators=(",", ":"))
    elif len(sys.argv) >= 3 and sys.argv[1] == "fetch":
        root = sys.argv[2]
        eras = sys.argv[3:] or None

        def progress(msg, frac):
            print(("%s %s" % (msg or "", "" if frac is None else "%d%%" % (frac * 100))).strip(), flush=True)
        for era in eras or maccompacts.eras():
            maccompacts.fetch(era, root, progress)
    else:
        print(__doc__)
        sys.exit(2)


if __name__ == "__main__":
    main()
