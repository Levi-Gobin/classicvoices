# A few files out of an IPSW's root filesystem straight from Apple, by HTTP
# range requests, without downloading the image (remote.py downloads all of
# it, ~1.2 GB for iOS 7). Used for engine-only firmware: iOS 7.1.2's engine is
# 3 MB, and this fetches about 50 MB.
#
# The root filesystem is a zip member, deflated; it is encrypted (encrcdsa),
# so after a short Huffman-coded start deflate wrote it as stored blocks: a
# 5-byte header, then ~16.4 KB copied as is. Their lengths vary by a few
# bytes, so where a byte of the image sits in the zip is known only up to a
# few headers. Two things pin it:
# - Between two places whose positions are known exactly, the number of
#   headers is exact, and the count up to any point between them is off by a
#   small fraction of a header (a random walk of a few bytes per block), so
#   it rounds to the exact one.
# - Where the decrypted content can say (a deflated chunk of the disk image
#   inside starts with a zlib stream; text and code aren't noise), it must
#   agree, and it decides where the count can't.
# Reading on from a stretch already placed needs neither: the next header's
# position is known.
import bisect
import hashlib
import hmac
import io
import math
import struct
import zlib
from collections import Counter

from . import applereaders, firmware, remote

BLOCK = 16390  # typical stored-block length
WINDOW = 300000  # bytes fetched on either side of an estimate


def _entropy(b):
    c = Counter(b)
    n = len(b)
    return -sum(v / n * math.log2(v / n) for v in c.values())


def _chain(d, h):
    """Stored blocks from header h in d: [(header position, length)]."""
    out = []
    while h + 5 <= len(d):
        n, nn = struct.unpack("<HH", d[h + 1:h + 5])
        if d[h] not in (0, 1) or n ^ nn != 0xFFFF or n == 0:
            break
        out.append((h, n))
        h += 5 + n
    return out


def _find_chain(d, need=3):
    for h in range(len(d) - 5):
        c = _chain(d, h)
        if len(c) >= need:
            return c
    return []


class _Member(io.RawIOBase):
    """Seekable view of a deflated zip member whose body is stored blocks."""

    def __init__(self, url, start, csize, usize):
        self.url, self.start, self.csize, self.size = url, start, csize, usize
        self.check = None  # check(plain offset, bytes) -> mismatches, or None if it can't tell
        self.pos = 0
        self.fetched = 0
        self.spans = []  # (plain start, bytes), sorted
        self.starts = []  # their plain starts, for bisect
        self.anchors = []  # (compressed payload start, plain start), exact
        self.runs = []  # (plain end, compressed position of the next header)
        head = self._get(start, start + (4 << 20) - 1)
        c = _find_chain(head, 8)
        if not c:
            raise firmware.ImportError_("the root filesystem isn't stored the expected way")
        o = zlib.decompressobj(-15)
        plain = o.decompress(head[:c[0][0]])
        self._span(0, plain)
        self.mean = sum(n for _, n in c) / len(c)
        self._add(head, start, c, len(plain))
        # the member ends with a stored block: its end is the image's end
        tail = self._get(start + csize - BLOCK - 64, start + csize - 1)
        for h in range(len(tail) - 5):
            n, nn = struct.unpack("<HH", tail[h + 1:h + 5])
            if tail[h] in (0, 1) and n ^ nn == 0xFFFF and h + 5 + n == len(tail):
                bisect.insort(self.anchors, (start + csize - n, usize - n))
                self._span(usize - n, tail[h + 5:])
                break
        else:
            raise firmware.ImportError_("the root filesystem doesn't end the expected way")

    def _get(self, a, b):
        b = min(b, self.start + self.csize - 1)
        if a > b:
            return b""
        with remote._get(self.url, a, b) as r:
            d = r.read()
        self.fetched += len(d)
        return d

    def _span(self, at, data):
        i = bisect.bisect(self.starts, at)
        self.starts.insert(i, at)
        self.spans.insert(i, (at, data))

    def _add(self, d, d_at, c, plain):
        a = (d_at + c[0][0] + 5, plain)
        for h, n in c:
            self._span(plain, d[h + 5:h + 5 + n])
            plain += n
        h, n = c[-1]
        self.runs.append((plain, d_at + h + 5 + n))
        if a not in self.anchors:
            bisect.insort(self.anchors, a)

    def _piece(self, off, n):
        i = bisect.bisect(self.starts, off) - 1
        if i < 0:
            return None
        s, b = self.spans[i]
        return b[off - s:off - s + n] if off < s + len(b) else None

    def _gather(self, off, n):
        out = bytearray()
        while len(out) < n:
            piece = self._piece(off + len(out), n - len(out))
            if not piece:
                return None
            out += piece
        return bytes(out)

    def _fetch(self, off, n):
        for end, nxt in self.runs:
            if end == off:  # right after a placed stretch: the next header is known
                ratio = (self.mean + 5) / self.mean
                d = self._get(nxt, nxt + int(n * ratio) + 2 * BLOCK)
                c = _chain(d, 0)
                if c:
                    self._add(d, nxt, c, off)
                    return
                if d and self.start + self.csize - nxt < 1 << 20:
                    # the member ends in compressed blocks: inflate them, with
                    # the 32 KB before as their dictionary
                    window = self._gather(off - 32768, 32768) if off >= 32768 else None
                    if window is not None:
                        self._span(off, zlib.decompressobj(-15, zdict=window).decompress(self._get(nxt, nxt + (1 << 20))))
                        return
        self._place(off, n)

    def _place(self, off, n):
        below = [a for a in self.anchors if a[1] <= off]
        above = [a for a in self.anchors if a[1] > off]
        ca, pa = max(below, key=lambda a: a[1])
        n_ab, mean = 0, self.mean
        if above:
            cb, pb = min(above, key=lambda a: a[1])
            n_ab = ((cb - ca) - (pb - pa)) / 5
            if n_ab > 0:
                mean = (pb - pa) / n_ab
        ratio = (mean + 5) / mean
        est = ca + int((off - pa) * ratio)
        lo = max(self.start, est - WINDOW)
        d = self._get(lo, est + int(n * ratio) + WINDOW)
        c = _find_chain(d)
        if not c:
            raise firmware.ImportError_("no stored blocks near %d" % off)
        h0 = lo + c[0][0]
        kf = (h0 + 5 - ca) / (mean + 5)  # headers from the anchor, estimated
        k0 = round(kf)
        nt = abs(kf)
        sigma = ((nt * (1 - nt / n_ab)) ** 0.5 if n_ab > nt else nt ** 0.5 + 50) * 4.6 / mean
        body = b"".join(d[h + 5:h + 5 + m] for h, m in c)
        verdict = {}
        for k in range(k0 - 4, k0 + 5):
            v = self.check(pa + (h0 + 5 - ca) - 5 * k, body) if self.check else None
            if v is not None:
                verdict[k] = v
        good = [k for k, v in verdict.items() if v == 0]
        if sigma < 0.15:  # the count is certain; the content mustn't disagree
            k = k0
            if verdict.get(k0, 0) != 0 and good:
                raise firmware.ImportError_("couldn't place the data near %d" % off)
        elif len(good) == 1:
            k = good[0]
        else:
            raise firmware.ImportError_("couldn't place the data near %d" % off)
        self._add(d, lo, c, pa + (h0 + 5 - ca) - 5 * k)

    def read(self, n=-1):
        if n < 0:
            n = self.size - self.pos
        n = max(0, min(n, self.size - self.pos))
        out = bytearray()
        tries = 0
        while len(out) < n:
            piece = self._piece(self.pos + len(out), n - len(out))
            if piece:
                out += piece
                continue
            tries += 1
            if tries > 64:
                raise firmware.ImportError_("couldn't read the firmware at %d" % (self.pos + len(out)))
            self._fetch(self.pos + len(out), n - len(out))
        self.pos += n
        return bytes(out)

    def seek(self, off, whence=io.SEEK_SET):
        self.pos = off if whence == io.SEEK_SET else self.pos + off if whence == io.SEEK_CUR else self.size + off
        return self.pos

    def tell(self):
        return self.pos

    def readable(self):
        return True

    def seekable(self):
        return True


def _checker(enc, deflated_starts):
    """check(plain offset, body) for _Member: how many places in body disagree
    with the encrypted image's structure there, or None if none can tell."""
    bs, data = enc.blocksize, enc.dataoff

    def decrypt(body, plain0, n, count=1):
        out = b""
        for i in range(n, n + count):
            o = data + i * bs - plain0
            iv = hmac.new(enc.hk, struct.pack(">I", i), hashlib.sha1).digest()[:16]
            out += enc.aes.decrypt(iv, body[o:o + bs])
        return out

    def check(plain0, body):
        end = plain0 + len(body)
        # a deflated chunk of the disk image begins here: a zlib stream
        i, j = bisect.bisect_left(deflated_starts, plain0 - data), bisect.bisect_left(deflated_starts, end - data - 2 * bs)
        tried = bad = 0
        for st in deflated_starts[i:j][:16]:
            n = st // bs
            if data + n * bs < plain0:
                continue
            p = decrypt(body, plain0, n, 2)[st % bs:]
            tried += 1
            try:
                zlib.decompressobj().decompress(p[:bs])
            except zlib.error:
                bad += 1
        if tried:
            return bad
        # elsewhere: text, code and empty space decrypt to something that
        # isn't noise (a wrong candidate is off by 5-40 bytes, never a whole
        # number of AES blocks, so it decrypts to noise throughout)
        first, last = max(0, -(-(plain0 - data) // bs)), (end - data) // bs
        step = max(1, (last - first) // 96)
        for n in range(first, last, step):
            if _entropy(decrypt(body, plain0, n)) < 6.5:
                return 0
        return None

    return check


def open_volume(url, build, progress=None):
    """(HFS+ volume, bytes fetched so far) of a remote IPSW's root filesystem."""
    name, method, csize, lho, usize = remote._remote_rootfs(url)
    if method != 8:
        raise firmware.ImportError_("the root filesystem isn't deflated")
    with remote._get(url, lho, lho + 29) as r:
        h = r.read()
    nl, el = struct.unpack("<HH", h[26:30])
    m = _Member(url, lho + 30 + nl + el, csize, usize)
    _key, device, _nice = firmware.SUPPORTED[build]
    if progress:
        progress("Fetching the decryption key from The Apple Wiki", None)
    enc = firmware.EncrcdsaStream(m, firmware.fetch_rootfs_key(build, device, name.split("/")[-1]))
    m.check = _checker(enc, [])
    udif = applereaders.UDIFReader(enc, enc.size, name)
    m.check = _checker(enc, sorted(c[3] for c in udif.chunks if c[0] == applereaders.ZLIB))
    base = applereaders.find_hfs(lambda o, n: (udif.seek(o), udif.read(n))[1], udif.size, name)
    return applereaders.Volume(udif, base), m
