#!/usr/bin/env python3
"""
ipswtool - discover, download, decrypt, and extract Apple IPSWs (iPhone OS 1.0 -> current).

Format timeline handled automatically (detected per file by magic, not version):
  Root filesystem DMG:
    encrcdsa (vfdecrypt, 1.0-9.x)  ->  plain UDIF (iOS 10-17)  ->  .dmg.aea (iOS 18+)
  Ramdisks / boot images:
    8900/IMG2 (1.x, global 0x837)  ->  IMG3 (2.0b4-iOS9, per-file IV+Key)  ->  IM4P/IMG4 (iOS 10+)

Keys: AEA -> fetched from Apple via 'ipsw'; legacy (encrcdsa/IMG3/IM4P) -> The Apple Wiki.
Decrypted DMGs (and ramdisks, wrapped into real DMGs) are auto-mounted on macOS.

Engines: brew install blacktop/tap/ipsw   (AEA)
         pip3 install cryptography        (vfdecrypt/IMG3/IM4P; or a 'vfdecrypt' binary)
         hdiutil / pbcopy / ditto         (macOS)
"""

import argparse
import hashlib
import hmac
import json
import os
import platform
import plistlib
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request
import zipfile

UA = "ipswtool/2.1 (+local script; python-urllib)"
APPLEDB_OS_MAIN = "https://api.appledb.dev/ios/{ostype}/main.json"
IPSWME_DEVICES = "https://api.ipsw.me/v4/devices"
IPSWME_IPSW_VERSION = "https://api.ipsw.me/v4/ipsw/{version}"
WIKI_API_BASES = ["https://theapplewiki.com/api.php", "https://theapplewiki.com/w/api.php"]
KEY_0X837 = "188458a6d15034dfe386f23b61d43774"

PLATFORM_ALIASES = {
    "ios": "iOS", "iphone": "iOS", "ipod": "iOS",
    "ipados": "iPadOS", "ipad": "iPadOS",
    "macos": "macOS", "mac": "macOS", "osx": "macOS", "macosx": "macOS",
    "tvos": "tvOS", "appletv": "tvOS", "atv": "tvOS",
    "watchos": "watchOS", "watch": "watchOS",
    "visionos": "visionOS", "vision": "visionOS", "visionpro": "visionOS", "xros": "visionOS",
    "audioos": "audioOS", "homepod": "audioOS",
}
PLATFORM_OSTYPES = {
    "iOS": ["iOS"], "iPadOS": ["iPadOS"], "macOS": ["macOS"], "tvOS": ["tvOS"],
    "watchOS": ["watchOS"], "visionOS": ["visionOS"], "audioOS": ["HomePod Software", "audioOS"],
}
DEVICE_ID_RE = re.compile(
    r"^(iPhone|iPad|iPod|Watch|AppleTV|AudioAccessory|RealityDevice|"
    r"Mac|MacBook|VirtualMac|ADP|iBridge)\d+,\d+$")
VERSION_RE = re.compile(r"^\d+(\.\d+){0,3}$")
IS_MAC = platform.system() == "Darwin"


def eprint(*a):
    print(*a, file=sys.stderr, flush=True)


def human(n):
    n = float(n)
    for u in ("B", "KB", "MB", "GB", "TB"):
        if n < 1024 or u == "TB":
            return ("%.0f %s" % (n, u)) if u == "B" else ("%.2f %s" % (n, u))
        n /= 1024
    return "%.2f TB" % n


def human_time(s):
    if s is None or s != s or s < 0 or s == float("inf"):
        return "unknown"
    s = int(s); h, r = divmod(s, 3600); m, sec = divmod(r, 60)
    return ("%dh %02dm %02ds" % (h, m, sec)) if h else \
           ("%dm %02ds" % (m, sec)) if m else ("%ds" % sec)


def http_request(url, range_start=None):
    h = {"User-Agent": UA, "Accept": "*/*"}
    if range_start:
        h["Range"] = "bytes=%d-" % range_start
    return urllib.request.urlopen(urllib.request.Request(url, headers=h), timeout=60)


def http_get_json(url):
    with http_request(url) as r:
        return json.loads(r.read().decode("utf-8", "replace"))


def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, **kw)


def ask(prompt, assume_yes=False):
    if assume_yes:
        return ""
    try:
        return input(prompt)
    except (EOFError, KeyboardInterrupt):
        print(); eprint("Cancelled."); sys.exit(130)


def ask_yes_no(prompt, default=False, assume_yes=False):
    if assume_yes:
        return True
    while True:
        a = ask(prompt + (" [Y/n] " if default else " [y/N] ")).strip().lower()
        if a == "":
            return default
        if a in ("y", "yes"):
            return True
        if a in ("n", "no"):
            return False


def file_sha256(p):
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


# --------------------------------------------------------------------------- #
# Discovery + signing                                                         #
# --------------------------------------------------------------------------- #

def load_device_names():
    try:
        return {r.get("identifier"): r.get("name")
                for r in http_get_json(IPSWME_DEVICES) if r.get("identifier")}
    except Exception:
        return {}


def fetch_builds_for_platform(label):
    ostypes = PLATFORM_OSTYPES.get(label, [label])
    merged, seen, errors, ok = [], set(), [], False
    for ot in ostypes:
        try:
            data = http_get_json(APPLEDB_OS_MAIN.format(ostype=urllib.parse.quote(ot)))
            ok = True
        except Exception as e:
            errors.append("%s -> %s" % (ot, e)); continue
        for e in (data if isinstance(data, list) else []):
            k = (e.get("osStr"), e.get("build"))
            if k not in seen:
                seen.add(k); merged.append(e)
    if not ok:
        raise SystemExit("Could not load AppleDB data for %s. Errors: %s"
                         % (label, "; ".join(errors)))
    return merged


def base_version(e):
    return (e.get("version") or "").split()[0] if e.get("version") else ""


def classify_channel(e):
    v = (e.get("version") or "").lower()
    if e.get("internal"):
        return "internal"
    if "public beta" in v:
        return "public_beta"
    if e.get("rc") or re.search(r"\brc\b", v) or "release candidate" in v:
        return "rc"
    if e.get("beta") or "beta" in v:
        return "dev_beta"
    return "release"


def channel_set_from_flags(a):
    if a.all:
        return {"release", "dev_beta", "public_beta", "rc", "internal"}
    w = set()
    if a.beta:
        w |= {"release", "dev_beta", "public_beta", "rc"}
    if a.developer_beta:
        w |= {"dev_beta", "rc"}
    if a.public_beta:
        w |= {"public_beta"}
    return w or {"release"}


def signed_for_device(e, ident):
    s = e.get("signed")
    return True if s is True else (ident in s if isinstance(s, list) else False)


def pick_link(src):
    links = src.get("links") or []
    https = [l for l in links if l.get("active") and str(l.get("url", "")).startswith("https")]
    pref = [l for l in https if l.get("preferred")]
    c = pref or https or [l for l in links if l.get("active")] or links
    return c[0]["url"] if c else None


def build_candidates(builds, channels, names):
    out = []
    for e in builds:
        if classify_channel(e) not in channels:
            continue
        for s in e.get("sources") or []:
            if s.get("type") != "ipsw":
                continue
            url = pick_link(s)
            if not url:
                continue
            hsh = s.get("hashes") or {}
            for ident in s.get("deviceMap") or []:
                out.append({"ostype": e.get("osStr"), "version": e.get("version"),
                            "base_version": base_version(e), "build": e.get("build"),
                            "channel": classify_channel(e), "released": e.get("released") or "",
                            "identifier": ident, "name": names.get(ident, ident),
                            "signed": signed_for_device(e, ident), "signed_source": "AppleDB",
                            "url": url, "sha256": hsh.get("sha2-256"), "size": s.get("size")})
    return out


def refresh_release_signing(c):
    if c["channel"] != "release":
        return
    try:
        rows = http_get_json(IPSWME_IPSW_VERSION.format(version=c["base_version"]))
    except Exception:
        return
    for r in rows if isinstance(rows, list) else []:
        if r.get("identifier") == c["identifier"] and r.get("buildid") == c["build"]:
            c["signed"] = bool(r.get("signed")); c["signed_source"] = "ipsw.me (live)"
            return


# --------------------------------------------------------------------------- #
# Selection                                                                   #
# --------------------------------------------------------------------------- #

def classify_token(t):
    low = t.lower()
    if low in PLATFORM_ALIASES:
        return "platform", PLATFORM_ALIASES[low]
    if VERSION_RE.match(t):
        return "version", t
    return "device", t


def parse_positionals(tokens):
    platform_, version, device = None, None, None
    for t in tokens:
        k, v = classify_token(t)
        if k == "platform" and platform_ is None:
            platform_ = v
        elif k == "version" and version is None:
            version = v
        elif device is None:
            device = v
    return platform_, version, device


def resolve_platform(p):
    if p:
        return p
    opts = ["iOS", "iPadOS", "macOS", "tvOS", "watchOS", "visionOS", "audioOS"]
    print("Select a platform:")
    for i, o in enumerate(opts, 1):
        print("  %d. %s" % (i, o))
    while True:
        a = ask("Platform number or name: ").strip()
        if a.isdigit() and 1 <= int(a) <= len(opts):
            return opts[int(a) - 1]
        k, v = classify_token(a)
        if k == "platform":
            return v
        print("Not a recognized platform.")


def summarize_versions(cs):
    by = {}
    for c in cs:
        r = by.setdefault(c["base_version"] or "?", {"ch": set(), "rel": ""})
        r["ch"].add(c["channel"])
        if c["released"] > r["rel"]:
            r["rel"] = c["released"]
    return sorted(by.items(), key=lambda kv: (kv[1]["rel"], kv[0]), reverse=True)


def resolve_version(version, cs):
    if version:
        return version
    print("\nEnter a version number (for example 26.0, 3.0, or 1.0).")
    print("Leave blank to list the most recent available versions.")
    while True:
        a = ask("Version: ").strip()
        if a:
            return a
        for v, r in summarize_versions(cs)[:25]:
            print("  %-14s %-12s (%s)" % (v, r["rel"], ", ".join(sorted(r["ch"]))))
        print("")


def match_version(cs, v):
    if "." in v:
        return [c for c in cs if c["base_version"] == v]
    return [c for c in cs if c["base_version"].split(".")[0] == v]


def filter_device(cs, dev):
    if not dev:
        return cs
    if DEVICE_ID_RE.match(dev) or "," in dev:
        return [c for c in cs if c["identifier"].lower() == dev.lower()]
    n = dev.lower()
    return [c for c in cs if n in (c["name"] or "").lower()]


def choose_candidates(cs, dev):
    cs = filter_device(cs, dev)
    if not cs:
        raise SystemExit("No matching IPSW for that platform/version/device.")
    for c in cs:
        refresh_release_signing(c)
    cs.sort(key=lambda c: (c["released"], c["name"]), reverse=True)
    if len(cs) == 1:
        c = cs[0]
        print("\nOnly one match: %s %s (%s), %s, %s"
              % (c["ostype"], c["version"], c["build"], c["name"],
                 "signed" if c["signed"] else "not signed"))
        return cs
    print("\nMatching firmware:")
    for i, c in enumerate(cs, 1):
        print("  %2d. %s %s (%s), %s" % (i, c["ostype"], c["version"], c["build"], c["name"]))
        print("        device %s, %s, channel %s" % (
            c["identifier"], "signed" if c["signed"] else "not signed", c["channel"]))
    print("Enter numbers separated by commas, or 'a' for all.")
    while True:
        a = ask("Selection: ").strip().lower()
        if a in ("a", "all"):
            return cs
        try:
            idx = [int(x) for x in re.split(r"[,\s]+", a) if x]
            ch = [cs[i - 1] for i in idx if 1 <= i <= len(cs)]
            if ch:
                return ch
        except ValueError:
            pass
        print("Invalid selection.")


# --------------------------------------------------------------------------- #
# Download (single self-updating status line, comma+space separated columns)  #
# --------------------------------------------------------------------------- #

def render_progress(name, dl, total, speed, eta, final=False):
    cols = [name]
    if total:
        cols += ["%d%%" % int(dl * 100 / total), "%s of %s" % (human(dl), human(total))]
    else:
        cols.append(human(dl))
    cols.append("%s/s" % human(speed) if speed > 0 else "-- B/s")
    if total:
        cols.append("ETA %s" % ("done" if final else human_time(eta)))
    sys.stdout.write("\r" + ", ".join(cols) + "\033[K")
    sys.stdout.flush()
    if final:
        sys.stdout.write("\n"); sys.stdout.flush()


def download(url, dest_dir, expected_sha256=None, total_hint=None):
    os.makedirs(dest_dir, exist_ok=True)
    fname = url.split("/")[-1].split("?")[0] or "firmware.ipsw"
    dest = os.path.join(dest_dir, fname)
    existing = os.path.getsize(dest) if os.path.exists(dest) else 0
    if existing and expected_sha256 and (total_hint is None or existing == total_hint):
        if file_sha256(dest) == expected_sha256.lower():
            print("Already downloaded and verified: %s" % fname)
            return dest
    resume = existing if (existing and total_hint and existing < total_hint) else 0
    try:
        resp = http_request(url, range_start=resume or None)
    except urllib.error.HTTPError:
        resp = http_request(url); resume = 0
    status = getattr(resp, "status", resp.getcode())
    hasher = hashlib.sha256() if expected_sha256 else None
    if resume and status == 206:
        mode, dl = "ab", resume
        if hasher:
            with open(dest, "rb") as f:
                for b in iter(lambda: f.read(1 << 20), b""):
                    hasher.update(b)
        cr = resp.headers.get("Content-Range")
        total = int(cr.split("/")[-1]) if cr and "/" in cr else (total_hint or 0)
        print("Resuming %s at %s" % (fname, human(resume)))
    else:
        mode, dl = "wb", 0
        total = int(resp.headers.get("Content-Length") or (total_hint or 0))
    start_bytes, t0, last = dl, time.time(), 0.0
    with resp, open(dest, mode) as fh:
        while True:
            b = resp.read(1 << 20)
            if not b:
                break
            fh.write(b)
            if hasher:
                hasher.update(b)
            dl += len(b)
            now = time.time()
            if now - last >= 1.0:
                el = now - t0
                sp = (dl - start_bytes) / el if el > 0 else 0
                render_progress(fname, dl, total, sp, (total - dl) / sp if (total and sp > 0) else None)
                last = now
    el = max(time.time() - t0, 1e-6)
    render_progress(fname, dl, total or dl, (dl - start_bytes) / el, 0, final=True)
    if expected_sha256:
        d = hasher.hexdigest() if hasher else file_sha256(dest)
        print("sha256 verified." if d.lower() == expected_sha256.lower()
              else "WARNING: sha256 mismatch (%s)" % d)
    return dest


# --------------------------------------------------------------------------- #
# The Apple Wiki key lookup (api.php; parses rendered output)                 #
# --------------------------------------------------------------------------- #

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
    raise last


def fetch_legacy_keys(build, identifier):
    """Return ({filename: {'key':hex, 'iv':hex}}, page_title)."""
    if not build or not identifier or build == "?" or identifier == "?":
        return {}, None
    try:
        si = _wiki_api({"action": "query", "meta": "siteinfo", "siprop": "namespaces"})
        ns = next((n for n, i in si["query"]["namespaces"].items()
                   if i.get("canonical") == "Keys" or i.get("*") == "Keys"), 0)
        res = _wiki_api({"action": "query", "list": "search",
                         "srsearch": "%s %s" % (build, identifier),
                         "srnamespace": ns, "srlimit": 10})
        titles = [h["title"] for h in res["query"]["search"]]
    except Exception as e:
        eprint("Wiki search failed: %s" % e)
        return {}, None
    flat = identifier.replace(" ", "")
    title = next((t for t in titles if build in t and flat in t.replace(" ", "")),
                 titles[0] if titles else None)
    if not title:
        return {}, None
    try:
        html = _wiki_api({"action": "parse", "page": title, "prop": "text"})["parse"]["text"]["*"]
    except Exception as e:
        eprint("Wiki fetch failed: %s" % e)
        return {}, title
    text = re.sub(r"\s+", " ", re.sub(r"&[a-zA-Z#0-9]+;", " ", re.sub(r"<[^>]+>", " ", html)))
    keys = {}
    for seg in re.split(r"Filename:", text)[1:]:
        mfn = re.search(r"(\S+\.dmg|\S+\.img\d?|\S+\.dfu|\S+)", seg)
        if not mfn:
            continue
        rec = {}
        miv = re.search(r"\bIV\b\s*:?\s*([0-9a-fA-F]{16,})", seg)
        mk = re.search(r"\bKey\b\s*:?\s*([0-9a-fA-F]{16,})", seg)
        if miv:
            rec["iv"] = miv.group(1).lower()
        if mk:
            rec["key"] = mk.group(1).lower()
        if rec:
            keys[mfn.group(1)] = rec
    return keys, title


# --------------------------------------------------------------------------- #
# Decryption primitives                                                       #
# --------------------------------------------------------------------------- #

def _aes_cbc(key, iv, data):
    from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
    n = len(data) - (len(data) % 16)
    return Cipher(algorithms.AES(key), modes.CBC(iv)).decryptor().update(data[:n]) + data[n:]


def vfdecrypt_inline(in_path, out_path, key_hex):
    """encrcdsa v2: encrypted region is whole `blocksize` blocks (last padded);
    decrypt all, then truncate plaintext to datasize so the koly trailer survives."""
    key = bytes.fromhex(key_hex); aes_key, hk = key[:16], key[16:36]
    with open(in_path, "rb") as f:
        hdr = f.read(72)
        if hdr[:8] != b"encrcdsa":
            raise ValueError("not encrcdsa")
        blocksize = struct.unpack(">I", hdr[52:56])[0]
        datasize = struct.unpack(">Q", hdr[56:64])[0]
        dataoff = struct.unpack(">Q", hdr[64:72])[0]
        nchunks = (datasize + blocksize - 1) // blocksize
        f.seek(dataoff)
        written = 0
        with open(out_path, "wb") as o:
            for n in range(nchunks):
                chunk = f.read(blocksize)
                if not chunk:
                    break
                iv = hmac.new(hk, struct.pack(">I", n), hashlib.sha1).digest()[:16]
                dec = _aes_cbc(aes_key, iv, chunk)
                if written + len(dec) > datasize:
                    dec = dec[:datasize - written]
                o.write(dec); written += len(dec)


def decrypt_encrcdsa(in_path, out_path, key_hex):
    if shutil.which("vfdecrypt"):
        run(["vfdecrypt", "-i", in_path, "-o", out_path, "-k", key_hex])
        if os.path.exists(out_path) and os.path.getsize(out_path) > 0:
            return
    vfdecrypt_inline(in_path, out_path, key_hex)


def unwrap_8900(data, key_hex=None):
    size = struct.unpack("<I", data[0x0C:0x10])[0]
    payload = data[0x800:0x800 + size]
    if payload[0x400:0x402] in (b"H+", b"HX"):
        return payload, "plaintext-hfs"
    if key_hex:
        return _aes_cbc(bytes.fromhex(key_hex), b"\x00" * 16, payload), "decrypted"
    return payload, "encrypted-skip"


def img3_decrypt(data, iv_hex, key_hex):
    """Parse IMG3, decrypt the DATA tag (AES-128/192/256 by key length)."""
    if data[:4] != b"3gmI":
        raise ValueError("not img3")
    off, tags = 20, {}
    while off + 12 <= len(data):
        magic = data[off:off + 4]
        total, dlen = struct.unpack("<II", data[off + 4:off + 12])
        if total < 12:
            break
        tags[magic[::-1].decode("latin1", "replace")] = data[off + 12:off + 12 + dlen]
        off += total
    typ = tags.get("TYPE", b"")[:4][::-1].decode("latin1", "replace")
    payload = tags.get("DATA", b"")
    if iv_hex and key_hex:
        payload = _aes_cbc(bytes.fromhex(key_hex), bytes.fromhex(iv_hex), payload)
        return payload, typ, True
    return payload, typ, False


def _der_tlv(buf, off):
    tag = buf[off]; off += 1
    l = buf[off]; off += 1
    if l & 0x80:
        nb = l & 0x7f
        l = int.from_bytes(buf[off:off + nb], "big"); off += nb
    return tag, buf[off:off + l], off + l


def im4p_extract(data, iv_hex=None, key_hex=None):
    """Best-effort IM4P: return (type, payload, had_kbag). Decrypts if key given."""
    tag, seq, _ = _der_tlv(data, 0)
    if tag != 0x30:
        raise ValueError("not im4p")
    items, off = [], 0
    while off < len(seq):
        t, v, off = _der_tlv(seq, off)
        items.append((t, v))
    if not items or items[0][1][:4] != b"IM4P":
        raise ValueError("no IM4P magic")
    typ = items[1][1].decode("latin1", "replace") if len(items) > 1 else ""
    payload, had_kbag = b"", False
    for t, v in items:
        if t == 0x04 and not payload:
            payload = v
        elif t == 0x04 and payload:
            had_kbag = True
    if had_kbag and iv_hex and key_hex:
        payload = _aes_cbc(bytes.fromhex(key_hex), bytes.fromhex(iv_hex), payload)
    return typ, payload, had_kbag


def classify_dmg(path):
    with open(path, "rb") as f:
        head = f.read(0x1000)
        try:
            f.seek(-512, 2); tail = f.read()
        except OSError:
            tail = b""
    if head[:8] == b"encrcdsa":
        return "encrcdsa"
    if head[:4] == b"8900":
        return "8900"
    if head[:4] == b"3gmI":
        return "img3"
    if head[:1] == b"\x30" and b"IM4P" in head[:32]:
        return "im4p"
    if b"koly" in tail:
        return "udif"
    if head[0x400:0x402] in (b"H+", b"HX"):
        return "rawhfs"
    return "unknown"


def looks_like_dmg(b):
    return b[0x400:0x402] in (b"H+", b"HX") or b"koly" in b[-512:]


# --------------------------------------------------------------------------- #
# macOS helpers                                                               #
# --------------------------------------------------------------------------- #

def convert_raw_to_dmg(raw_path, dmg_base):
    dmg = dmg_base + ".dmg"
    if not (IS_MAC and shutil.which("hdiutil")):
        eprint("  (hdiutil unavailable; left raw HFS+ at %s)" % os.path.basename(raw_path))
        return None
    r = run(["hdiutil", "convert", "-format", "UDZO",
             "-imagekey", "diskimage-class=CRawDiskImage", "-o", dmg_base, raw_path])
    if r.returncode == 0 and os.path.exists(dmg):
        return dmg
    att = run(["hdiutil", "attach", "-nomount", "-readonly",
               "-imagekey", "diskimage-class=CRawDiskImage", raw_path])
    dev = next((w for w in att.stdout.split() if w.startswith("/dev/disk")), None)
    if not dev:
        eprint("  could not wrap into a DMG: %s" % (att.stderr or att.stdout).strip())
        return None
    try:
        run(["hdiutil", "create", "-srcdevice", dev, "-format", "UDZO", "-o", dmg_base])
    finally:
        run(["hdiutil", "detach", dev])
    return dmg if os.path.exists(dmg) else None


def mount_dmg(dmg_path, raw=False):
    if not (IS_MAC and shutil.which("hdiutil")):
        return None
    cmd = ["hdiutil", "attach", "-nobrowse", "-readonly"]
    if raw:
        cmd += ["-imagekey", "diskimage-class=CRawDiskImage"]
    cmd.append(dmg_path)
    r = run(cmd)
    if r.returncode != 0:
        eprint("  mount failed: %s" % (r.stderr or r.stdout).strip())
        return None
    m = re.findall(r"(/Volumes/[^\n\t]+)", r.stdout)
    return m[-1].strip() if m else "(attached, no mountable volume)"


def copy_to_clipboard(text):
    if not (IS_MAC and shutil.which("pbcopy")):
        return False
    try:
        return subprocess.run(["pbcopy"], input=text.encode()).returncode == 0
    except Exception:
        return False


# --------------------------------------------------------------------------- #
# AEA (modern) decryption                                                     #
# --------------------------------------------------------------------------- #

def decrypt_aea(aea_path, out_path, pem_db=None):
    if shutil.which("ipsw"):
        with tempfile.TemporaryDirectory() as tmp:
            cmd = ["ipsw", "fw", "aea", aea_path, "--output", tmp]
            if pem_db:
                cmd += ["--pem-db", pem_db]
            r = run(cmd)
            if r.returncode == 0:
                for root, _d, files in os.walk(tmp):
                    for fn in files:
                        if not fn.lower().endswith(".aea"):
                            shutil.move(os.path.join(root, fn), out_path)
                            return True, ""
            return False, (r.stderr or r.stdout).strip()
    if shutil.which("aea"):
        aeota = os.environ.get("AEOTA_DIR")
        gk = os.path.join(aeota, "get_key.py") if aeota else None
        if gk and os.path.exists(gk):
            kp = run([sys.executable, gk, aea_path])
            m = re.search(r"[A-Za-z0-9+/]{40,}={0,2}", kp.stdout)
            if m:
                r = run(["aea", "decrypt", "-i", aea_path, "-o", out_path,
                         "-key-value", "base64:" + m.group(0)])
                return (r.returncode == 0), (r.stderr or r.stdout).strip()
    return False, "no AEA engine (install: brew install blacktop/tap/ipsw)"


# --------------------------------------------------------------------------- #
# Extraction orchestration                                                    #
# --------------------------------------------------------------------------- #

def read_restore_plist(zf):
    for n in zf.namelist():
        if n.endswith("Restore.plist"):
            try:
                return plistlib.loads(zf.read(n))
            except Exception:
                return {}
    return {}


def _collect(d, key):
    cur = d.get(key, {})
    if isinstance(cur, dict):
        return {os.path.basename(v) for v in cur.values() if isinstance(v, str)}
    return {os.path.basename(cur)} if isinstance(cur, str) else set()


def extract_entry(zf, name, dest_dir):
    out = os.path.join(dest_dir, os.path.basename(name))
    with zf.open(name) as src, open(out, "wb") as dst:
        shutil.copyfileobj(src, dst, length=1 << 20)
    return out


def _finish_image(produced, used_keys, work, base, role, out_path, opts):
    """Given a decrypted file at out_path, wrap raw HFS+ into a DMG and queue."""
    kind = classify_dmg(out_path)
    if kind == "rawhfs":
        dmg = convert_raw_to_dmg(out_path, out_path.rsplit(".", 1)[0] if "." in os.path.basename(out_path) else out_path)
        if dmg:
            if not opts.keep_encrypted and os.path.exists(out_path) and dmg != out_path:
                os.remove(out_path)
            produced.append((dmg, False, role)); return
        produced.append((out_path, True, role)); return
    produced.append((out_path, False, role))


def extract_ipsw(ipsw_path, opts):
    print("\n=== Extracting %s ===" % os.path.basename(ipsw_path))
    work = os.path.join(opts.output, os.path.splitext(os.path.basename(ipsw_path))[0] + "_extracted")
    os.makedirs(work, exist_ok=True)
    with zipfile.ZipFile(ipsw_path) as zf:
        plist = read_restore_plist(zf)
        identifier = plist.get("ProductType", "?")
        build = plist.get("ProductBuildVersion", "?")
        version = plist.get("ProductVersion", "?")
        rootfs = _collect(plist, "SystemRestoreImages")
        ramdisks = _collect(plist, "RestoreRamDisks")
        entries = [n for n in zf.namelist() if n.lower().endswith((".dmg", ".aea"))]
        if identifier != "?":
            print("Device %s, version %s, build %s" % (identifier, version, build))

        wiki_keys, title = ({}, None)
        if any(not n.lower().endswith(".aea") for n in entries):
            wiki_keys, title = fetch_legacy_keys(build, identifier)
        keys = {os.path.basename(k): v for k, v in wiki_keys.items()}
        for fn, hx in (opts.key or {}).items():
            keys.setdefault(fn, {})["key"] = hx.lower()

        produced, used_keys = [], []
        for name in entries:
            base = os.path.basename(name)
            role = ("root filesystem" if base in rootfs else
                    "ramdisk" if base in ramdisks else "disk image")
            src = extract_entry(zf, name, work)

            if name.lower().endswith(".aea"):
                out = src[:-4]
                print("\n%s  [%s, AEA]" % (base, role))
                ok, err = decrypt_aea(src, out, pem_db=opts.pem_db)
                if ok:
                    print("  decrypted -> %s" % os.path.basename(out))
                    if not opts.keep_encrypted:
                        os.remove(src)
                    produced.append((out, False, role))
                else:
                    eprint("  AEA decryption failed: %s" % err)
                continue

            kind = classify_dmg(src)
            rec = keys.get(base, {})
            print("\n%s  [%s, %s]" % (base, role, kind))

            if kind == "encrcdsa":
                if not rec.get("key"):
                    print("  encrypted, no key (supply --key %s=HEX); skipped." % base)
                    if not opts.keep_encrypted:
                        os.remove(src)
                    continue
                out = os.path.join(work, base)
                tmp = src + ".enc"; os.replace(src, tmp)
                try:
                    decrypt_encrcdsa(tmp, out, rec["key"])
                finally:
                    if not opts.keep_encrypted and os.path.exists(tmp):
                        os.remove(tmp)
                print("  decrypted -> %s (key %s)" % (os.path.basename(out), rec["key"]))
                used_keys.append((base, rec["key"], None))
                _finish_image(produced, used_keys, work, base, role, out, opts)

            elif kind == "8900":
                payload, status = unwrap_8900(open(src, "rb").read(),
                                              key_hex=rec.get("key") or KEY_0X837 if rec else None)
                if status == "encrypted-skip":
                    open(os.path.join(work, base + ".payload"), "wb").write(payload)
                    print("  8900 payload encrypted; wrote raw payload.")
                    print("    decrypt with: xpwntool %s out.bin -k %s" % (base, KEY_0X837))
                else:
                    raw = os.path.join(work, base.rsplit(".", 1)[0] + ".hfs")
                    open(raw, "wb").write(payload)
                    print("  unwrapped 8900 (%s)" % status)
                    _finish_image(produced, used_keys, work, base, role, raw, opts)
                if not opts.keep_encrypted and os.path.exists(src):
                    os.remove(src)

            elif kind == "img3":
                if not (rec.get("iv") and rec.get("key")):
                    print("  IMG3, no IV/Key (supply via wiki or --key); skipped.")
                    continue
                payload, typ, _ = img3_decrypt(open(src, "rb").read(), rec["iv"], rec["key"])
                print("  IMG3 type '%s', decrypted with IV+Key" % typ)
                used_keys.append((base, rec["key"], rec["iv"]))
                if looks_like_dmg(payload):
                    raw = os.path.join(work, base.rsplit(".", 1)[0] + ".hfs")
                    open(raw, "wb").write(payload)
                    _finish_image(produced, used_keys, work, base, role, raw, opts)
                else:
                    open(os.path.join(work, base + ".dec"), "wb").write(payload)
                    print("  -> %s.dec (boot component, not a filesystem)" % base)
                if not opts.keep_encrypted and os.path.exists(src):
                    os.remove(src)

            elif kind == "im4p":
                try:
                    typ, payload, kbag = im4p_extract(open(src, "rb").read(),
                                                      rec.get("iv"), rec.get("key"))
                except Exception as e:
                    print("  IM4P parse failed (%s); left as-is." % e)
                    continue
                if kbag and not rec.get("key"):
                    print("  IM4P type '%s' is encrypted, no key; left as-is." % typ)
                    continue
                if rec.get("key"):
                    used_keys.append((base, rec["key"], rec.get("iv")))
                if looks_like_dmg(payload):
                    raw = os.path.join(work, base.rsplit(".", 1)[0] + ".hfs")
                    open(raw, "wb").write(payload)
                    print("  IM4P type '%s' -> filesystem" % typ)
                    _finish_image(produced, used_keys, work, base, role, raw, opts)
                else:
                    open(os.path.join(work, base + ".payload"), "wb").write(payload)
                    print("  IM4P type '%s' -> %s.payload (boot component)" % (typ, base))
                if not opts.keep_encrypted and os.path.exists(src):
                    os.remove(src)

            elif kind == "rawhfs":
                _finish_image(produced, used_keys, work, base, role, src, opts)
            elif kind == "udif":
                produced.append((src, False, role))   # plain DMG (iOS 10-17), mounts directly
            else:
                print("  unrecognized format; left as-is")

        if used_keys:
            print("\nDecryption key(s)%s:" % ((" from " + title) if title else ""))
            clip = []
            for fn, k, iv in used_keys:
                line = "%s  Key=%s%s" % (fn, k, ("  IV=" + iv) if iv else "")
                print("  " + line)
                clip.append(line)
            if copy_to_clipboard("\n".join(clip)):
                print("  (copied to clipboard)")

        if produced and not opts.no_mount:
            print("\nMounting:")
            for dmg, raw, role in produced:
                mp = mount_dmg(dmg, raw=raw)
                print("  %s  [%s]  ->  %s" % (os.path.basename(dmg), role, mp or "not mounted"))
        if opts.extract_files:
            for dmg, raw, role in produced:
                dest = os.path.join(work, os.path.basename(dmg) + "_files")
                mp = mount_dmg(dmg, raw=raw)
                if mp and mp.startswith("/Volumes"):
                    os.makedirs(dest, exist_ok=True)
                    run(["ditto", mp, dest]); run(["hdiutil", "detach", mp])
                    print("  files -> %s" % dest)
        print("\nExtraction output: %s" % work)


# --------------------------------------------------------------------------- #
# Main                                                                        #
# --------------------------------------------------------------------------- #

def build_parser():
    p = argparse.ArgumentParser(
        prog="ipswtool", formatter_class=argparse.RawDescriptionHelpFormatter,
        description="Discover, download, decrypt, and extract Apple IPSWs (1.0 - current).",
        epilog=("Channels (union when combined): none=release, -d=dev betas+RC, "
                "-p=public betas, -b=release+all betas, -a=everything.\n"
                "Pass a local .ipsw path to extract it without downloading.\n"))
    p.add_argument("tokens", nargs="*", help="platform / version / device, or a local .ipsw path")
    p.add_argument("-b", "--beta", action="store_true")
    p.add_argument("-d", "--developer-beta", action="store_true")
    p.add_argument("-p", "--public-beta", action="store_true")
    p.add_argument("-a", "--all", action="store_true")
    p.add_argument("-o", "--output", default="./ipsw-downloads")
    p.add_argument("--no-extract", action="store_true", help="download only")
    p.add_argument("--no-mount", action="store_true", help="don't auto-mount decrypted DMGs")
    p.add_argument("--extract-files", action="store_true", help="also copy files out of each DMG")
    p.add_argument("--keep-encrypted", action="store_true", help="keep encrypted intermediates")
    p.add_argument("--pem-db", help="offline AEA FCS key database for ipsw")
    p.add_argument("--key", action="append", metavar="NAME=HEX",
                   help="supply/override a key for a filename (repeatable)")
    p.add_argument("-y", "--yes", action="store_true")
    return p


def main(argv=None):
    args = build_parser().parse_args(argv)
    args.key = dict(kv.split("=", 1) for kv in (args.key or []) if "=" in kv)

    locals_ = [t for t in args.tokens if t.lower().endswith(".ipsw") and os.path.isfile(t)]
    if locals_:
        for p in locals_:
            extract_ipsw(p, args)
        print("\nDone.")
        return

    platform_, version, device = parse_positionals(args.tokens)
    platform_ = resolve_platform(platform_)
    channels = channel_set_from_flags(args)
    print("\nPlatform: %s, channels: %s" % (platform_, ", ".join(sorted(channels))))
    print("Loading firmware catalog from AppleDB ...")
    catalog = build_candidates(fetch_builds_for_platform(platform_), channels, load_device_names())
    if not catalog:
        raise SystemExit("No IPSWs found for %s in the selected channel(s)." % platform_)
    version = resolve_version(version, catalog)
    matched = match_version(catalog, version)
    if not matched:
        raise SystemExit("No IPSWs found for %s %s." % (platform_, version))
    selected = choose_candidates(matched, device)
    for c in selected:
        print("\n=== %s %s (%s) for %s ===" % (c["ostype"], c["version"], c["build"], c["name"]))
        print("  signing: %s (%s)" % ("signed" if c["signed"] else "not signed", c["signed_source"]))
        path = download(c["url"], args.output, expected_sha256=c.get("sha256"), total_hint=c.get("size"))
        if not args.no_extract:
            extract_ipsw(path, args)
    print("\nDone.")


if __name__ == "__main__":
    main()