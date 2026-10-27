#!/usr/bin/env python3
"""Copy the MacinTalk engines and voices of Mac OS X Tiger, Leopard and Snow
Leopard out of install discs kept on archive.org, into extracted/mt104,
mt105 and mt106 (the layout tools/fetch_macintalk.sh makes for Lion).

    tools/fetch_old_macintalk.py                   all three
    tools/fetch_old_macintalk.py mt106             only Snow Leopard
    tools/fetch_old_macintalk.py --image mt106=SnowLeopard.dmg   a copy you have
    tools/fetch_old_macintalk.py --out build/old   somewhere other than extracted/
    tools/fetch_old_macintalk.py --alex            Alex too (several hundred MB)

The Leopard and Snow Leopard discs (8 GB and 5.6 GB) aren't downloaded: the
disc reader (third_party/panthera/pantherahfs.py) asks for the bytes it
needs, which are fetched with HTTP range requests, about a tenth of each.
Tiger is an installed disk in a .tar.bz2, which can't be read in the middle,
so it is streamed once (1.4 GB) into a sparse file and read from there.

Every file written is checked against tools/profiles/oldmt.sha256, so the
engines are exactly the ones the fast engine code was made for. Needs only
Python 3 (no hdiutil), so it runs on Linux too.
"""
import argparse
import bz2
import gzip
import hashlib
import io
import os
import shutil
import struct
import sys
import tarfile
import threading
import time
import urllib.request
from concurrent.futures import ThreadPoolExecutor

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "third_party", "panthera"))
import pantherahfs as hfs  # noqa: E402

IA = "https://archive.org/download/"
SOURCES = {
    "mt104": {
        "name": "Tiger (10.4.1, build 8B1025)",
        "url": IA + "deadmoo-tiger-x86/tiger-x86.tar.bz2",
        "member": "tiger-x86/tiger-x86-flat.img",
        "lib": None,
    },
    "mt105": {
        "name": "Leopard (10.5 install DVD)",
        "url": IA + "Mac_OS_X_Leopard_Install_DVD_Version_10.5_2Z691-6037-A_Apple_Inc._2007/"
               "Mac%20OS%20X%20Leopard%20Install%20DVD%20Version%2010.5%20%282Z691-6037-A%29"
               "%20%28Apple%2C%20Inc.%29%282007%29.iso",
        "lib": "libstdc++.6.0.4.dylib",
    },
    "mt106": {
        "name": "Snow Leopard (10.6 install DVD)",
        "url": IA + "MacOSX10.6-Snow-Leopard-Retail-2Z691-6428-A/Mac%20OS%20X%20Install%20DVD.dmg",
        "lib": "libstdc++.6.0.9.dylib",
    },
}

ENGINE = "System/Library/Speech/Synthesizers/MacinTalk.SpeechSynthesizer/Contents/MacOS/MacinTalk"
DICTIONARY = "System/Library/PrivateFrameworks/SpeechDictionary.framework/Versions/A"
VOICES = "System/Library/Speech/Voices"
VOICE_PKGS = ("System/Installation/Packages/AdditionalSpeechVoices.pkg",
              "System/Installation/Packages/Essentials.pkg")
VOICE_PREFIX = "./" + VOICES + "/"
BIG = ("Alex.SpeechVoice",)
MANIFEST = os.path.join(ROOT, "tools", "profiles", "oldmt.sha256")


def say(msg):
    print(msg, flush=True)


# ---- reading a file on a web server -------------------------------------

class HTTPFile(io.RawIOBase):
    """A seekable read-only file over HTTP range requests.

    Reads come in 4 MB blocks, kept in a small cache; a run of consecutive
    blocks (a package being streamed) makes it fetch ahead on several
    connections, which is what makes archive.org fast enough.
    """

    BLOCK = 4 << 20
    AHEAD = 16
    KEEP = 48

    def __init__(self, url):
        self.url = self._resolve(url)
        self.pos = 0
        self.got = 0
        self.cache = {}
        self.order = []
        self.pending = {}
        self.lock = threading.Lock()
        self.pool = ThreadPoolExecutor(8)
        self.last = -2

    def _resolve(self, url):
        # archive.org answers with a redirect to the server holding the item;
        # ask that one directly from then on.
        req = urllib.request.Request(url, headers={"Range": "bytes=0-0"})
        with urllib.request.urlopen(req, timeout=60) as r:
            rng = r.headers.get("Content-Range", "")
            if r.status != 206 or "/" not in rng:
                raise SystemExit("%s doesn't answer range requests" % url)
            self.size = int(rng.rsplit("/", 1)[1])
            return r.geturl()

    def _fetch(self, b):
        start = b * self.BLOCK
        end = min(start + self.BLOCK, self.size) - 1
        err = "a short read"
        for attempt in range(6):
            try:
                req = urllib.request.Request(self.url, headers={"Range": "bytes=%d-%d" % (start, end)})
                with urllib.request.urlopen(req, timeout=120) as r:
                    data = r.read()
                if len(data) == end - start + 1:
                    return data
            except OSError as e:
                err = e
            time.sleep(2 * (attempt + 1))
        raise SystemExit("couldn't read %s: %s" % (self.url, err))

    def _block(self, b):
        with self.lock:
            if b in self.cache:
                return self.cache[b]
            fut = self.pending.get(b)
            if fut is None:
                fut = self.pending[b] = self.pool.submit(self._fetch, b)
            if b == self.last + 1:
                for k in range(b + 1, min(b + 1 + self.AHEAD, (self.size - 1) // self.BLOCK + 1)):
                    if k not in self.cache and k not in self.pending:
                        self.pending[k] = self.pool.submit(self._fetch, k)
            self.last = b
        data = fut.result()
        with self.lock:
            self.pending.pop(b, None)
            if b not in self.cache:
                self.got += len(data)
                self.cache[b] = data
                self.order.append(b)
                while len(self.order) > self.KEEP:
                    self.cache.pop(self.order.pop(0), None)
        return data

    def readable(self):
        return True

    def seekable(self):
        return True

    def tell(self):
        return self.pos

    def seek(self, off, whence=io.SEEK_SET):
        self.pos = off if whence == io.SEEK_SET else self.pos + off if whence == io.SEEK_CUR else self.size + off
        return self.pos

    def read(self, n=-1):
        if n is None or n < 0:
            n = self.size - self.pos
        n = max(0, min(n, self.size - self.pos))
        out = bytearray()
        while n > 0:
            b, at = divmod(self.pos, self.BLOCK)
            piece = self._block(b)[at:at + n]
            out += piece
            self.pos += len(piece)
            n -= len(piece)
        return bytes(out)

    def readinto(self, buf):
        data = self.read(len(buf))
        buf[:len(data)] = data
        return len(data)

    def close(self):
        self.pool.shutdown(wait=False, cancel_futures=True)
        super().close()


def open_stream(where):
    if where.startswith(("http://", "https://")):
        return HTTPFile(where)
    return open(where, "rb")


# ---- finding the HFS+ volume --------------------------------------------

def volume_of(f, what):
    """-> an hfs.Volume for a raw disc (Apple partition map, PC partition
    table or a bare volume) or a compressed disk image."""
    size = f.seek(0, io.SEEK_END)

    def read_at(off, n):
        f.seek(off)
        return f.read(n)

    head = read_at(0, 512)
    if head[510:512] == b"\x55\xaa" and head[:2] != b"ER":
        for k in range(4):                        # a PC disk (VMware's)
            e = head[446 + 16 * k:462 + 16 * k]
            if e[4] == 0xAF:
                return hfs.Volume(f, struct.unpack("<I", e[8:12])[0] * 512)
        raise SystemExit("%s has no HFS+ partition" % what)
    if read_at(size - 512, 4) == b"koly":
        udif = hfs.UDIFReader(f, size, what)
        base = hfs.find_hfs(lambda o, n: (udif.seek(o), udif.read(n))[1], udif.size, what)
        return hfs.Volume(udif, base)
    return hfs.Volume(f, hfs.find_hfs(read_at, size, what))


def unpack_tiger(where, member, dest):
    """Read a .tar.bz2 once, front to back, and write one member as a sparse
    file."""
    raw = open_stream(where)        # several connections at once, ahead
    total = raw.seek(0, io.SEEK_END)
    raw.seek(0)

    class Counted(io.RawIOBase):
        n = 0

        def readable(self):
            return True

        def readinto(self, buf):
            got = raw.readinto(buf)
            Counted.n += got or 0
            return got

    seen = [0]
    with tarfile.open(fileobj=bz2.BZ2File(io.BufferedReader(Counted(), 1 << 20)), mode="r|") as tar:
        for m in tar:
            if m.name != member:
                continue
            src = tar.extractfile(m)
            zero = bytes(1 << 20)
            with open(dest, "wb") as out:
                left = m.size
                while left:
                    blk = src.read(min(left, 1 << 20))
                    if not blk:
                        raise SystemExit("%s ended early" % where)
                    if blk == zero[:len(blk)]:
                        out.seek(len(blk), io.SEEK_CUR)
                    else:
                        out.write(blk)
                    left -= len(blk)
                    pct = Counted.n * 100 // total if total else 0
                    if pct >= seen[0] + 5:
                        seen[0] = pct
                        say("   %d%% of %d MB" % (pct, total >> 20))
                out.truncate(m.size)
            return
    raise SystemExit("%s isn't in %s" % (member, where))


# ---- copying ------------------------------------------------------------

def write_tree(v, entry, dest):
    """Copy a folder of the volume, symlinks as symlinks."""
    os.makedirs(dest, exist_ok=True)
    for kid in v.children(entry):
        name = kid[0]
        if hfs.ILLEGAL & set(name) or name in (".", "..") or "/" in name:
            continue
        p = os.path.join(dest, name)
        if kid[1]:
            write_tree(v, kid, p)
        else:
            write_file(v, kid, p)


def write_file(v, entry, p):
    data = v.read(entry)
    if entry[5] & hfs.S_IFMT == hfs.S_IFLNK:
        os.symlink(data.decode("utf-8"), p)
    else:
        with open(p, "wb") as out:
            out.write(data)


def need(v, path):
    e = v.entry(path)
    if e is None:
        raise SystemExit("not on this disc: " + path)
    return e


def take_voices(v, pkg, out, alex):
    """Stream one package and keep what is under the voices folder. Its
    files are in path order, so it stops once it has gone past them."""
    entry = v.entry(pkg)
    if entry is None:
        return
    offset, length = hfs.payload_offset(v, entry)
    if offset is None:
        return
    say("   reading " + os.path.basename(pkg))
    stream = gzip.GzipFile(fileobj=io.BufferedReader(hfs.ForkReader(v, entry, skip=offset), 1 << 20))
    inside = False
    for name, mode, size, read in hfs.cpio_entries(stream):
        if not name.startswith(VOICE_PREFIX):
            if inside:
                break
            continue
        inside = True
        rel = name[len(VOICE_PREFIX):]
        if not rel or hfs.ILLEGAL & set(rel) or (not alex and rel.split("/")[0] in BIG):
            continue
        dest = hfs.safe_join(out, rel.replace("/", os.sep))
        if dest is None:
            continue
        kind = mode & hfs.S_IFMT
        if kind == hfs.S_IFDIR:
            os.makedirs(dest, exist_ok=True)
        elif kind == hfs.S_IFREG:
            os.makedirs(os.path.dirname(dest), exist_ok=True)
            with open(dest, "wb") as f:
                f.write(read() if size else b"")


def extract(v, gen, out, alex):
    src = SOURCES[gen]
    os.makedirs(out)
    write_file(v, need(v, ENGINE), os.path.join(out, "MacinTalk"))
    write_tree(v, need(v, DICTIONARY), os.path.join(out, "SpeechDictionary"))
    if src["lib"]:
        os.makedirs(os.path.join(out, "lib"))
        write_file(v, need(v, "usr/lib/" + src["lib"]), os.path.join(out, "lib", src["lib"]))
    voices = os.path.join(out, "Voices")
    os.makedirs(voices)
    for kid in v.children(need(v, VOICES)):     # the voices live on the disc
        if kid[1] and kid[0].endswith(".SpeechVoice") and (alex or kid[0] not in BIG):
            write_tree(v, kid, os.path.join(voices, kid[0]))
    for pkg in VOICE_PKGS:                      # and the rest in packages
        take_voices(v, pkg, voices, alex)
    # MacinTalk voices only: a VoiceDescription says it is one
    for name in os.listdir(voices):
        if not os.path.isfile(os.path.join(voices, name, "Contents", "Resources", "VoiceDescription")):
            shutil.rmtree(os.path.join(voices, name))


def digest_tree(top):
    out = {}
    for d, dirs, files in os.walk(top, followlinks=True):
        dirs.sort()
        for name in files:
            p = os.path.join(d, name)
            rel = os.path.relpath(p, top).replace(os.sep, "/")
            if name == ".DS_Store":
                continue
            if os.path.islink(p):
                out[rel] = "link:" + os.readlink(p)
            else:
                with open(p, "rb") as f:
                    out[rel] = hashlib.sha256(f.read()).hexdigest()
    return out


def load_manifest():
    want = {}
    with open(MANIFEST) as f:
        for line in f:
            line = line.rstrip("\n")
            if line and not line.startswith("#"):
                h, path = line.split("  ", 1)
                want[path] = h
    return want


def check(gen, out, alex):
    want = {p[len(gen) + 1:]: h for p, h in load_manifest().items() if p.startswith(gen + "/")}
    if not alex:
        want = {p: h for p, h in want.items() if p.split("/")[:2] != ["Voices", "Alex.SpeechVoice"]}
    have = digest_tree(out)
    bad = sorted(p for p in set(want) | set(have) if want.get(p) != have.get(p))
    if bad:
        for p in bad[:20]:
            say("   differs: %s (%s, expected %s)" % (p, have.get(p, "missing"), want.get(p, "nothing")))
        raise SystemExit("%s: %d files aren't the ones the fast code was made for" % (gen, len(bad)))


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("gens", nargs="*", default=sorted(SOURCES))
    ap.add_argument("--image", action="append", default=[], metavar="GEN=PATH",
                    help="read a disc you already have (for mt104, the .tar.bz2 or the -flat.img)")
    ap.add_argument("--out", default=os.path.join(ROOT, "extracted"))
    ap.add_argument("--work", default=os.path.join(ROOT, "build", "oldmt"))
    ap.add_argument("--alex", action="store_true", help="also copy Alex (Leopard 670 MB, Snow Leopard 380 MB)")
    ap.add_argument("--no-check", action="store_true", help="don't compare with tools/profiles/oldmt.sha256")
    ap.add_argument("--manifest", action="store_true", help="write the manifest from what is in --out instead")
    ap.add_argument("--keep-going", action="store_true",
                    help="if a disc can't be read (gone from archive.org, say), go on without that release")
    a = ap.parse_args(argv)

    if a.manifest:
        with open(MANIFEST, "w") as f:
            f.write("# SHA-256 of the files tools/fetch_old_macintalk.py writes (links as link:<target>)\n")
            for gen in a.gens:
                for p, h in sorted(digest_tree(os.path.join(a.out, gen)).items()):
                    f.write("%s  %s/%s\n" % (h, gen, p))
        return
    images = dict(i.split("=", 1) for i in a.image)
    for gen in a.gens:
        if gen not in SOURCES:
            raise SystemExit("unknown generation %s (one of %s)" % (gen, ", ".join(sorted(SOURCES))))
        src = SOURCES[gen]
        dest = os.path.join(a.out, gen)
        if os.path.lexists(dest):
            say("== %s is already there: %s" % (src["name"], dest))
            continue
        where = images.get(gen, src["url"])
        say("== %s from %s" % (src["name"], where))
        try:
            fetch(gen, src, where, dest, a)
        except (SystemExit, OSError, EOFError, ValueError) as e:
            if not a.keep_going:
                raise
            # GitHub shows this line as a warning on the run's page
            say("::warning::%s isn't in this build: %s" % (src["name"], e))


def fetch(gen, src, where, dest, a):
    t0 = time.time()
    os.makedirs(a.work, exist_ok=True)
    os.makedirs(a.out, exist_ok=True)
    tmp = os.path.join(a.work, gen + ".partial")
    shutil.rmtree(tmp, ignore_errors=True)
    disk = None
    try:
        if gen == "mt104" and not where.endswith(".img"):
            disk = os.path.join(a.work, "tiger-flat.img")
            say("   unpacking the disk (it can't be read in place)")
            unpack_tiger(where, src["member"], disk)
            where = disk
        f = open_stream(where)
        v = volume_of(f, where)
        extract(v, gen, tmp, a.alex)
        fetched = getattr(f, "got", 0)
        f.close()
        if not a.no_check:
            check(gen, tmp, a.alex)
        os.rename(tmp, dest)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
        if disk and os.path.exists(disk):
            os.remove(disk)
    say("   done in %d s%s" % (time.time() - t0, ", %d MB read" % (fetched >> 20) if fetched else ""))


if __name__ == "__main__":
    main()
