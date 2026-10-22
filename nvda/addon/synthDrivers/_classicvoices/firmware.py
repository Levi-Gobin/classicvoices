# Import an iPhone firmware (IPSW) the user supplies: find the root
# filesystem, decrypt it (keys come from The Apple Wiki), read the HFS+
# volume inside, and copy out only the speech engine and its voices into
# Voices/<build>/. Nothing of Apple's is downloaded or redistributed here;
# the IPSW is the user's own. Follows tools/ipswtool.py from this repo.
import ctypes
import ctypes.util
import hashlib
import hmac
import io
import json
import os
import re
import shutil
import struct
import urllib.parse
import urllib.request
import zipfile

from . import applereaders

UA = "classicvoices-nvda/1.0 (python-urllib)"
WIKI_API_BASES = ["https://theapplewiki.com/api.php", "https://theapplewiki.com/w/api.php"]

# The firmware builds the engine recognizes (kBuilds in core/vxengine.c).
SUPPORTED = {
    "7E18": ("ios3", "iPhone2,1", "iPhone OS 3.1.3"),
    "8L1": ("ios4", "iPhone2,1", "iOS 4.3.5"),
    "9B206": ("ios5", "iPhone2,1", "iOS 5.1.1"),
    "10B500": ("ios6", "iPhone2,1", "iOS 6.1.6"),
    "11D257": ("ios7", "iPhone3,1", "iOS 7.1.2"),
    "14G61": ("ios10", "iPhone5,1", "iOS 10.3.4"),
}

# Mac OS X voice eras: common synthesis files assembled from firmware files
# (same table as tools/fetch_firmware.sh).
MAC_COMMON = {
    "mac107": (("ios5", "lid.dat"), ("ios6", "synth_med_fxd_bet2f22.dat")),
    "mac108": (("ios6", "lid.dat"), ("ios6", "synth_med_fxd_bet2f22.dat"),
               ("ios7", "synth_med_fxd_bet3f22.dat")),
    "mac109": (("ios7", "lid.dat"), ("ios7", "synth_med_fxd_bet3f22.dat")),
}


class ImportError_(Exception):
    pass


# ---------------------------------------------------------------- AES via CNG

class _CngAes:
    """AES-CBC decryption through Windows CNG (no third-party packages)."""

    def __init__(self, key):
        self.bc = ctypes.WinDLL("bcrypt")
        self.alg = ctypes.c_void_p()
        if self.bc.BCryptOpenAlgorithmProvider(ctypes.byref(self.alg), "AES", None, 0):
            raise ImportError_("CNG AES unavailable")
        mode = ctypes.create_unicode_buffer("ChainingModeCBC")
        self.bc.BCryptSetProperty(self.alg, "ChainingMode", mode,
                                  ctypes.sizeof(mode), 0)
        self.key = ctypes.c_void_p()
        if self.bc.BCryptGenerateSymmetricKey(self.alg, ctypes.byref(self.key), None, 0,
                                              key, len(key), 0):
            raise ImportError_("CNG key import failed")

    def decrypt(self, iv, data):
        n = len(data) - (len(data) % 16)
        out = ctypes.create_string_buffer(n)
        got = ctypes.c_ulong(0)
        ivbuf = ctypes.create_string_buffer(iv, 16)
        if self.bc.BCryptDecrypt(self.key, data, n, None, ivbuf, 16,
                                 out, n, ctypes.byref(got), 0):
            raise ImportError_("AES decrypt failed")
        return out.raw[:got.value] + data[n:]

    def close(self):
        if self.key:
            self.bc.BCryptDestroyKey(self.key)
        if self.alg:
            self.bc.BCryptCloseAlgorithmProvider(self.alg, 0)


class _OpenSslAes:
    """AES-CBC decryption through OpenSSL's libcrypto (present on every Linux
    distribution), for the Linux importer."""

    def __init__(self, key):
        name = ctypes.util.find_library("crypto")
        for cand in ([name] if name else []) + ["libcrypto.so.3", "libcrypto.so.1.1", "libcrypto.so"]:
            try:
                self.lib = ctypes.CDLL(cand)
                break
            except OSError:
                continue
        else:
            raise ImportError_("OpenSSL's libcrypto not found")
        L = self.lib
        L.EVP_CIPHER_CTX_new.restype = ctypes.c_void_p
        L.EVP_aes_128_cbc.restype = ctypes.c_void_p
        L.EVP_CIPHER_CTX_free.argtypes = [ctypes.c_void_p]
        L.EVP_DecryptInit_ex.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p,
                                         ctypes.c_char_p, ctypes.c_char_p]
        L.EVP_CIPHER_CTX_set_padding.argtypes = [ctypes.c_void_p, ctypes.c_int]
        L.EVP_DecryptUpdate.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.POINTER(ctypes.c_int),
                                        ctypes.c_char_p, ctypes.c_int]
        self.key = bytes(key)

    def decrypt(self, iv, data):
        L = self.lib
        n = len(data) - (len(data) % 16)
        out = ctypes.create_string_buffer(n + 16)
        got = ctypes.c_int(0)
        ctx = L.EVP_CIPHER_CTX_new()
        try:
            if (L.EVP_DecryptInit_ex(ctx, L.EVP_aes_128_cbc(), None, self.key, bytes(iv)) != 1
                    or L.EVP_CIPHER_CTX_set_padding(ctx, 0) != 1
                    or L.EVP_DecryptUpdate(ctx, out, ctypes.byref(got), bytes(data[:n]), n) != 1):
                raise ImportError_("AES decrypt failed")
        finally:
            L.EVP_CIPHER_CTX_free(ctx)
        return out.raw[:got.value] + data[n:]

    def close(self):
        pass


class _PyCaAes:
    """AES-CBC through the `cryptography` package, where it is installed."""

    def __init__(self, key):
        from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
        self._make = lambda iv: Cipher(algorithms.AES(bytes(key)), modes.CBC(bytes(iv))).decryptor()

    def decrypt(self, iv, data):
        n = len(data) - (len(data) % 16)
        d = self._make(iv)
        return d.update(bytes(data[:n])) + d.finalize() + data[n:]

    def close(self):
        pass


def _Aes(key):
    if os.name == "nt":
        return _CngAes(key)
    try:
        return _PyCaAes(key)
    except ImportError:
        return _OpenSslAes(key)


class EncrcdsaStream(io.RawIOBase):
    """Seekable plaintext view of an encrcdsa v2 image, decrypting only the
    chunks actually read (the HFS+ reader touches a few percent of it)."""

    def __init__(self, f, key_hex):
        self.f = f
        f.seek(0)
        hdr = f.read(72)
        if hdr[:8] != b"encrcdsa":
            raise ImportError_("not an encrcdsa image")
        self.blocksize = struct.unpack(">I", hdr[52:56])[0]
        self.size = struct.unpack(">Q", hdr[56:64])[0]
        self.dataoff = struct.unpack(">Q", hdr[64:72])[0]
        key = bytes.fromhex(key_hex)
        self.aes = _Aes(key[:16])
        self.hk = key[16:36]
        self.pos = 0
        self.cache = {}  # chunk index -> plaintext

    def _chunk(self, n):
        c = self.cache.get(n)
        if c is None:
            self.f.seek(self.dataoff + n * self.blocksize)
            data = self.f.read(self.blocksize)
            iv = hmac.new(self.hk, struct.pack(">I", n), hashlib.sha1).digest()[:16]
            c = self.aes.decrypt(iv, data)
            if len(self.cache) > 128:
                self.cache.clear()
            self.cache[n] = c
        return c

    def seek(self, off, whence=io.SEEK_SET):
        if whence == io.SEEK_SET:
            self.pos = off
        elif whence == io.SEEK_CUR:
            self.pos += off
        else:
            self.pos = self.size + off
        return self.pos

    def tell(self):
        return self.pos

    def readable(self):
        return True

    def seekable(self):
        return True

    def read(self, n=-1):
        if n < 0:
            n = self.size - self.pos
        n = max(0, min(n, self.size - self.pos))
        out = bytearray()
        while n:
            ci, co = divmod(self.pos, self.blocksize)
            piece = self._chunk(ci)[co:co + n]
            if not piece:
                break
            out += piece
            self.pos += len(piece)
            n -= len(piece)
        return bytes(out)


# ---------------------------------------------------------------- wiki keys

def _wiki_api(params):
    params["format"] = "json"
    qs = urllib.parse.urlencode(params)
    last = None
    for base in WIKI_API_BASES:
        try:
            req = urllib.request.Request(base + "?" + qs, headers={"User-Agent": UA})
            with urllib.request.urlopen(req, timeout=30) as r:
                return json.load(r)
        except Exception as e:
            last = e
    raise ImportError_("The Apple Wiki is unreachable: %s" % last)


def fetch_rootfs_key(build, identifier, rootfs_name):
    """The root-filesystem VFDecrypt key for one firmware, from The Apple Wiki."""
    si = _wiki_api({"action": "query", "meta": "siteinfo", "siprop": "namespaces"})
    ns = next((n for n, i in si["query"]["namespaces"].items()
               if i.get("canonical") == "Keys" or i.get("*") == "Keys"), 0)
    res = _wiki_api({"action": "query", "list": "search",
                     "srsearch": "%s %s" % (build, identifier),
                     "srnamespace": ns, "srlimit": 10})
    titles = [h["title"] for h in res["query"]["search"]]
    flat = identifier.replace(" ", "")
    title = next((t for t in titles if build in t and flat in t.replace(" ", "")),
                 titles[0] if titles else None)
    if not title:
        raise ImportError_("no key page found for %s (%s)" % (build, identifier))
    html = _wiki_api({"action": "parse", "page": title, "prop": "text"})["parse"]["text"]["*"]
    text = re.sub(r"\s+", " ", re.sub(r"&[a-zA-Z#0-9]+;", " ", re.sub(r"<[^>]+>", " ", html)))
    for seg in re.split(r"Filename:", text)[1:]:
        mfn = re.search(r"(\S+\.dmg)", seg)
        mk = re.search(r"\bKey\b\s*:?\s*([0-9a-fA-F]{64,80})", seg)
        if mfn and mk and mfn.group(1) == rootfs_name:
            return mk.group(1).lower()
    # fall back to any 72-hex key on the page (the root FS key is the long one)
    mk = re.search(r"\b([0-9a-fA-F]{72})\b", text)
    if mk:
        return mk.group(1).lower()
    raise ImportError_("no VFDecrypt key for %s on %s" % (rootfs_name, title))


# ---------------------------------------------------------------- extraction

def _volume_from_stream(f, size, what):
    """applereaders.open_image, but over an arbitrary seekable stream."""
    def read_at(off, n):
        f.seek(off)
        return f.read(n)
    try:
        base = applereaders.find_hfs(read_at, size, what)
        return applereaders.Volume(f, base)
    except SystemExit:
        pass
    f.seek(size - 512)
    if f.read(4) != b"koly":
        raise ImportError_("%s: no HFS+ volume and no UDIF trailer" % what)
    udif = applereaders.UDIFReader(f, size, what)
    base = applereaders.find_hfs(lambda o, n: (udif.seek(o), udif.read(n))[1],
                                 udif.size, what)
    return applereaders.Volume(udif, base)


def _copy_entry(vol, entry, dest):
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    packed = vol.compressed(entry)  # iOS 10 keeps system files compressed
    if packed is not None:
        with open(dest, "wb") as o:
            o.write(packed)
        return
    r = applereaders.ForkReader(vol, entry)
    left = entry[3]  # logical size: extents are whole allocation blocks
    with open(dest, "wb") as o:
        while left:
            b = r.read(min(left, 1 << 20))
            if not b:
                break
            o.write(b)
            left -= len(b)


def _copy_tree(vol, entry, dest, progress=None):
    for child in vol.children(entry):
        name, is_dir = child[0], child[1]
        if set(name) & applereaders.ILLEGAL:
            continue
        sub = os.path.join(dest, name)
        if is_dir:
            _copy_tree(vol, child, sub, progress)
        else:
            if progress:
                progress("copying %s" % name, None)
            _copy_entry(vol, child, sub)


def assemble_mac_common(voices_root):
    """Create Voices/mac10x/common from firmware files, where present
    (tools/fetch_firmware.sh does the same on the Mac)."""
    for era, wants in MAC_COMMON.items():
        srcs = [(os.path.join(voices_root, b, "TTSResources", "common", f), f)
                for b, f in wants]
        if not all(os.path.isfile(p) for p, _f in srcs):
            continue
        out = os.path.join(voices_root, era, "common")
        os.makedirs(out, exist_ok=True)
        for p, f in srcs:
            dest = os.path.join(out, f)
            if not os.path.isfile(dest):
                shutil.copyfile(p, dest)


def _copy_build_dir(src, voices_root, build_key, progress):
    """One extracted build -> Voices/<build>/. `src` may be shaped like
    tools/fetch_firmware.sh output (VoiceServices.framework/... or
    TTSSpeechBundle.speechbundle/...) or already like Voices/<build>."""
    fw = os.path.join(src, "VoiceServices.framework")
    bundle = os.path.join(src, "TTSSpeechBundle.speechbundle", "TTSSpeechBundle")
    if os.path.isfile(os.path.join(src, "voiced")):
        voiced, res = os.path.join(src, "voiced"), os.path.join(src, "TTSResources")
    elif os.path.isfile(bundle):
        voiced, res = bundle, None
    elif os.path.isdir(fw):
        voiced = os.path.join(fw, "Support", "voiced")
        res = os.path.join(fw, "TTSResources")
    else:
        raise ImportError_("%s has neither VoiceServices.framework, a TTSSpeechBundle, "
                           "nor a voiced binary" % src)
    if not os.path.isfile(voiced):
        raise ImportError_("no voiced binary under %s" % src)
    progress("Copying %s" % build_key, None)
    dest = os.path.join(voices_root, build_key)
    if os.path.isdir(dest):
        shutil.rmtree(dest)
    os.makedirs(dest)
    shutil.copyfile(voiced, os.path.join(dest, "voiced"))
    if res and os.path.isdir(res):
        shutil.copytree(res, os.path.join(dest, "TTSResources"))
        if build_key == "ios7":  # engine only; its own voices are not wanted
            res_dir = os.path.join(dest, "TTSResources")
            for name in os.listdir(res_dir):
                p = os.path.join(res_dir, name)
                if name != "common" and os.path.isdir(p):
                    shutil.rmtree(p)


def import_extracted(path, voices_root, progress):
    """Import a folder in the shape tools/fetch_firmware.sh leaves behind:
    either extracted/ itself (ios3 ... ios10, and optionally mac10x/common),
    or a single build folder named after its build (e.g. .../ios6).
    Returns the list of builds imported."""
    builds = [v[0] for v in SUPPORTED.values()]
    name = os.path.basename(os.path.normpath(path))
    done = []
    os.makedirs(voices_root, exist_ok=True)
    if name in builds:
        _copy_build_dir(path, voices_root, name, progress)
        done.append(name)
    else:
        for build in sorted(builds):
            sub = os.path.join(path, build)
            if os.path.isdir(sub):
                _copy_build_dir(sub, voices_root, build, progress)
                done.append(build)
        for era in MAC_COMMON:  # the fetch script also leaves mac10x/common
            src = os.path.join(path, era, "common")
            if os.path.isdir(src):
                dest = os.path.join(voices_root, era, "common")
                if os.path.isdir(dest):
                    shutil.rmtree(dest)
                os.makedirs(os.path.dirname(dest), exist_ok=True)
                shutil.copytree(src, dest)
    if not done:
        raise ImportError_(
            "%s has no firmware builds. Expected a folder named after a build "
            "(%s) or a folder containing them, in the shape "
            "tools/fetch_firmware.sh produces." % (path, ", ".join(sorted(builds))))
    assemble_mac_common(voices_root)
    progress("Imported %s" % ", ".join(done), 1.0)
    return done


def identify_ipsw(path):
    """-> (build_key, build_number, device, nice_name). Raises on unsupported."""
    with zipfile.ZipFile(path) as z:
        names = z.namelist()
        manifest = next((n for n in ("BuildManifest.plist", "Restore.plist") if n in names), None)
        if not manifest:
            raise ImportError_("no BuildManifest/Restore.plist: not an IPSW?")
        import plistlib
        pl = plistlib.loads(z.read(manifest))
        build = pl.get("ProductBuildVersion")
        devices = pl.get("SupportedProductTypes") or [pl.get("ProductType", "?")]
    if build not in SUPPORTED:
        wanted = ", ".join("%s (%s, %s)" % (v[2], k, v[1]) for k, v in SUPPORTED.items())
        raise ImportError_(
            "build %s is not one Classic Voices runs. Supply one of: %s" % (build, wanted))
    key, device, nice = SUPPORTED[build]
    if device not in devices:
        raise ImportError_("this IPSW is for %s; %s needs the %s build"
                           % (", ".join(devices), nice, device))
    return key, build, device, nice


def import_ipsw(path, voices_root, progress):
    """Import one IPSW. progress(message, fraction_or_None)."""
    build_key, build, device, nice = identify_ipsw(path)
    progress("Reading %s (%s)" % (nice, build), None)
    tmp = os.path.join(voices_root, "_tmp_rootfs.dmg")
    os.makedirs(voices_root, exist_ok=True)
    try:
        with zipfile.ZipFile(path) as z:
            dmgs = [i for i in z.infolist() if i.filename.lower().endswith(".dmg")]
            if not dmgs:
                raise ImportError_("no disk images inside the IPSW")
            rootfs = max(dmgs, key=lambda i: i.file_size)  # the root FS is the largest
            progress("Unpacking the root filesystem (%d MB)" % (rootfs.file_size >> 20), 0.0)
            with z.open(rootfs) as src, open(tmp, "wb") as out:
                done = 0
                while True:
                    b = src.read(1 << 22)
                    if not b:
                        break
                    out.write(b)
                    done += len(b)
                    progress(None, done / max(1, rootfs.file_size))
        return import_rootfs(tmp, os.path.basename(rootfs.filename), build, voices_root, progress)
    finally:
        if os.path.exists(tmp):
            os.remove(tmp)


def import_rootfs(tmp, rootfs_name, build, voices_root, progress):
    """Copy the engine and voices out of an (encrypted or plain) root
    filesystem image already on disk: the second half of import_ipsw, also
    used by importers that stream the image straight from Apple."""
    build_key, device, nice = SUPPORTED[build]
    try:
        f = open(tmp, "rb")
        try:
            magic = f.read(8)
            if magic == b"encrcdsa":
                progress("Fetching the decryption key from The Apple Wiki", None)
                key_hex = fetch_rootfs_key(build, device, rootfs_name)
                progress("Reading the encrypted filesystem", None)
                stream = EncrcdsaStream(f, key_hex)
                vol = _volume_from_stream(stream, stream.size, rootfs_name)
            else:
                vol = _volume_from_stream(f, os.path.getsize(tmp), rootfs_name)
            dest = os.path.join(voices_root, build_key)
            if os.path.isdir(dest):
                shutil.rmtree(dest)
            if build_key == "ios10":
                entry = vol.entry("System/Library/TTSPlugins/"
                                  "TTSSpeechBundle.speechbundle/TTSSpeechBundle")
                if not entry:
                    raise ImportError_("TTSSpeechBundle not found in the firmware")
                progress("Copying the iOS 10 engine", None)
                _copy_entry(vol, entry, os.path.join(dest, "voiced"))
            else:
                fw = vol.entry("System/Library/PrivateFrameworks/VoiceServices.framework")
                if not fw:
                    raise ImportError_("VoiceServices.framework not found in the firmware")
                voiced = vol.entry("System/Library/PrivateFrameworks/"
                                   "VoiceServices.framework/Support/voiced")
                res = vol.entry("System/Library/PrivateFrameworks/"
                                "VoiceServices.framework/TTSResources")
                if not voiced or not res:
                    raise ImportError_("voiced/TTSResources missing from the firmware")
                progress("Copying the %s engine and voices" % nice, None)
                _copy_entry(vol, voiced, os.path.join(dest, "voiced"))
                _copy_tree(vol, res, os.path.join(dest, "TTSResources"), progress)
                if build_key == "ios7":
                    # iOS 7 is only an engine for the Mavericks voices; its own
                    # voices are not wanted (and are another firmware's anyway).
                    res_dir = os.path.join(dest, "TTSResources")
                    for name in os.listdir(res_dir):
                        if name != "common":
                            p = os.path.join(res_dir, name)
                            if os.path.isdir(p):
                                shutil.rmtree(p)
        finally:
            f.close()
    finally:
        if os.path.exists(tmp):
            os.remove(tmp)
    assemble_mac_common(voices_root)
    progress("%s imported" % nice, 1.0)
    return build_key
