# Firmware straight from Apple: only the root filesystem of an IPSW is
# downloaded (read through the zip's central directory with range requests and
# inflated on the way), then the engine and voices are copied out of it as
# firmware.import_rootfs does for a file you have. Shared by the Linux tool and
# the desktop app.
import os
import shutil
import struct
import tempfile
import urllib.request
import zlib

from . import firmware

UA = "classicvoices/1.0 (python-urllib)"

# Apple's own links for the firmware Classic Voices runs (as listed by ipsw.me).
FIRMWARE = {  # build key: (build, URL, size)
    "ios3": ("7E18", "https://secure-appldnld.apple.com/iPhone/061-7472.20100202.8tugj/iPhone2,1_3.1.3_7E18_Restore.ipsw", 305122343),
    "ios4": ("8L1", "https://secure-appldnld.apple.com/iPhone4/041-1965.20110721.gxUB5/iPhone2,1_4.3.5_8L1_Restore.ipsw", 440577895),
    "ios5": ("9B206", "https://secure-appldnld.apple.com/iOS5.1.1/041-4347.20120427.o2yov/iPhone2,1_5.1.1_9B206_Restore.ipsw", 720391327),
    "ios6": ("10B500", "https://secure-appldnld.apple.com/iOS6.1/091-3457.20140221.Btt3e/iPhone2,1_6.1.6_10B500_Restore.ipsw", 822970962),
    "ios70": ("11B651", "https://secure-appldnld.apple.com/iOS7/031-3020.20140221.8j5GW/iPhone3,1_7.0.6_11B651_Restore.ipsw", 1201645522),
    "ios7": ("11D257", "https://secure-appldnld.apple.com/iOS7.1/031-4812.20140627.cq6y8/iPhone3,1_7.1.2_11D257_Restore.ipsw", 1204958925),
    "ios10": ("14G61", "https://updates.cdn-apple.com/2019/ios/091-25277-20190722-0C1B94DE-992C-11E9-A2EE-E2C9A77C2E40/iPhone_4.0_32bit_10.3.4_14G61_Restore.ipsw", 2000796302),
}
PURPOSE = {
    "ios3": "the iOS 3 voices", "ios4": "the iOS 4 voices", "ios5": "the iOS 5 voices and their enhanced versions",
    "ios6": "the iOS 6 voices and their enhanced versions; also the Lion and Mountain Lion voices",
    "ios70": "the iOS 7 voices and their enhanced versions; also the Mavericks compact voices",
    "ios7": "engine only, read from Apple's server (about 50 MB): the iOS 7 Siri voices and Mavericks compact voices",
    "ios10": "engine only: Mavericks premium voices",
}


def _get(url, start=None, end=None):
    h = {"User-Agent": UA}
    if start is not None:
        h["Range"] = "bytes=%d-%s" % (start, "" if end is None else end)
    return urllib.request.urlopen(urllib.request.Request(url, headers=h), timeout=120)


def _remote_rootfs(url):
    """(name, method, compressed size, local header offset) of the largest
    .dmg in a remote zip, read from its central directory with range requests."""
    with _get(url, 0, 0) as r:
        size = int(r.headers["Content-Range"].split("/")[1])
    tail_at = max(0, size - 65536)
    with _get(url, tail_at, size - 1) as r:
        tail = r.read()
    i = tail.rfind(b"PK\x05\x06")
    if i < 0:
        raise firmware.ImportError_("not a zip: " + url)
    cdsize, cdoff = struct.unpack("<II", tail[i + 12:i + 20])
    if cdoff == 0xFFFFFFFF:  # zip64
        j = tail.rfind(b"PK\x06\x07")
        off64 = struct.unpack("<Q", tail[j + 8:j + 16])[0]
        with _get(url, off64, off64 + 55) as r:
            z = r.read()
        cdsize, cdoff = struct.unpack("<QQ", z[40:56])
    if cdoff >= tail_at:
        cd = tail[cdoff - tail_at:cdoff - tail_at + cdsize]
    else:
        with _get(url, cdoff, cdoff + cdsize - 1) as r:
            cd = r.read()
    best, p = None, 0
    while cd[p:p + 4] == b"PK\x01\x02":
        method = struct.unpack("<H", cd[p + 10:p + 12])[0]
        csz, usz = struct.unpack("<II", cd[p + 20:p + 28])
        nl, el, cl = struct.unpack("<HHH", cd[p + 28:p + 34])
        lho = struct.unpack("<I", cd[p + 42:p + 46])[0]
        name = cd[p + 46:p + 46 + nl].decode("utf-8", "replace")
        extra = cd[p + 46 + nl:p + 46 + nl + el]
        if 0xFFFFFFFF in (csz, usz, lho):  # zip64 extra field
            q = 0
            while q + 4 <= len(extra):
                hid, hl = struct.unpack("<HH", extra[q:q + 4])
                if hid == 1:
                    vals = list(struct.unpack("<%dQ" % (hl // 8), extra[q + 4:q + 4 + hl]))
                    if usz == 0xFFFFFFFF: usz = vals.pop(0)
                    if csz == 0xFFFFFFFF: csz = vals.pop(0)
                    if lho == 0xFFFFFFFF: lho = vals.pop(0)
                q += 4 + hl
        if name.lower().endswith(".dmg") and (best is None or usz > best[4]):
            best = (name, method, csz, lho, usz)
        p += 46 + nl + el + cl
    if not best:
        raise firmware.ImportError_("no disk images in the firmware")
    return best


def stream_rootfs(url, dest, progress):
    """Download only the root filesystem out of a remote IPSW, inflating it on
    the way into `dest`. Returns the image's name inside the IPSW."""
    name, method, csize, lho, usize = _remote_rootfs(url)
    with _get(url, lho, lho + 29) as r:
        h = r.read()
    nl, el = struct.unpack("<HH", h[26:30])
    start = lho + 30 + nl + el
    progress("Downloading the root filesystem (%d MB of the firmware)" % (csize >> 20), 0.0)
    inflater = zlib.decompressobj(-15) if method == 8 else None
    got = 0
    with _get(url, start, start + csize - 1) as r, open(dest, "wb") as out:
        while True:
            b = r.read(1 << 20)
            if not b:
                break
            got += len(b)
            out.write(inflater.decompress(b) if inflater else b)
            progress(None, got / max(1, csize))
        if inflater:
            out.write(inflater.flush())
    if got != csize:
        raise firmware.ImportError_("download ended early (%d of %d bytes)" % (got, csize))
    return os.path.basename(name)



def _import_engine_only(key, voices_root, progress):
    """iOS 7.1.2 is only an engine here (Siri voices, Mavericks compacts):
    read its voiced and common data out of the IPSW on Apple's server
    (rangefs) instead of downloading the whole root filesystem."""
    from . import rangefs
    build, url, _size = FIRMWARE[key]
    build_key, _device, nice = firmware.SUPPORTED[build]
    progress("Reading the %s engine from Apple's server" % nice, None)
    vol, member = rangefs.open_volume(url, build, progress)
    fw = "System/Library/PrivateFrameworks/VoiceServices.framework/"
    voiced, common = vol.entry(fw + "Support/voiced"), vol.entry(fw + "TTSResources/common")
    if not voiced or not common:
        raise firmware.ImportError_("voiced/TTSResources missing from the firmware")
    dest = os.path.join(voices_root, build_key)
    tmp = dest + ".part"
    if os.path.isdir(tmp):
        shutil.rmtree(tmp)
    firmware._copy_entry(vol, voiced, os.path.join(tmp, "voiced"))
    progress(None, 0.5)
    firmware._copy_tree(vol, common, os.path.join(tmp, "TTSResources", "common"))
    if os.path.isdir(dest):
        shutil.rmtree(dest)
    os.rename(tmp, dest)
    firmware.assemble_mac_common(voices_root)
    progress("%s imported (%d MB read from Apple)" % (nice, member.fetched >> 20), 1.0)
    return build_key


ENGINE_ONLY = ("ios7",)


def import_from_apple(key, voices_root, progress):
    """Stream firmware `key` (ios3 ... ios10, ios70) from Apple and import it."""
    if key in ENGINE_ONLY:
        try:
            return _import_engine_only(key, voices_root, progress)
        except Exception as e:  # anything unexpected: the whole image, as before
            progress("Reading it in place didn't work (%s); downloading the whole firmware" % e, None)
            part = os.path.join(voices_root, key + ".part")
            if os.path.isdir(part):
                shutil.rmtree(part)
    build, url, _size = FIRMWARE[key]
    progress("Streaming %s from Apple" % firmware.SUPPORTED[build][2], None)
    os.makedirs(voices_root, exist_ok=True)
    fd, tmp = tempfile.mkstemp(prefix="cv_rootfs_", suffix=".dmg", dir=voices_root)
    os.close(fd)
    try:
        name = stream_rootfs(url, tmp, progress)
        return firmware.import_rootfs(tmp, name, build, voices_root, progress)
    finally:
        if os.path.exists(tmp):
            os.remove(tmp)
