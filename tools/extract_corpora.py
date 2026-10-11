#!/usr/bin/env python3
"""Decode the speech corpus of every downloaded enhanced/premium voice to FLAC.

Inputs: downloads/assets/<build>_<lang>.zip (iOS MobileAssets) and
downloads/macvoices/<era>_<Name>.pkg (Mac voice packages). For each one the
synth_<voice>_full_*.dat voice font is pulled out, decoded with
build/vxcorpus_native (the engine's own decoder running in the emulator) and
encoded to out/corpus/<locale>/<build>_<voice>_<locale>.flac.
Duplicates are only recorded in out/corpus/MANIFEST.tsv, not kept: fonts that
are byte-identical to one already done, decodes whose PCM is bit-identical to
an existing one, and iOS CELP (155mrf22) corpora that null against a Mac ADPCM
(vssq5) corpus of the same voice (same recording, lossier coding). Mac
packages are processed before iOS assets so the ADPCM version is the keeper.

Usage: extract_corpora.py [-j N] [paths...]   (default: everything in downloads/)
"""
import codecs, glob, hashlib, json, os, re, subprocess, sys, tarfile, tempfile, threading, zipfile
from concurrent.futures import ThreadPoolExecutor
import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)
V = "extracted/ios6/VoiceServices.framework"
ENGINE = [f"{V}/Support/voiced", f"{V}/TTSResources/en-US", f"{V}/TTSResources/common"]
OUT = "out/corpus"
MANIFEST = f"{OUT}/MANIFEST.tsv"
os.makedirs(OUT, exist_ok=True)
os.makedirs("build/corpus", exist_ok=True)

MAC = {v["era"] + "_" + v["name"]: v for v in json.load(open("ios/Shared/MacVoiceCatalog.json"))["voices"]}
LOCK = threading.Lock()
COLS = ["key", "md5", "flac", "source", "coder", "seconds", "synth_file", "note", "pcm_md5"]


def log(msg):
    print(msg, flush=True)


def font_from_zip(path, tmp):
    with zipfile.ZipFile(path) as z:
        names = [n for n in z.namelist() if re.search(r"/synth_[^/]*_full_[^/]*\.dat$", n)]
        if len(names) != 1:
            raise RuntimeError(f"{path}: expected one synth file, got {names}")
        z.extract(names[0], tmp)
        return os.path.join(tmp, names[0])


def font_from_pkg(path, tmp):
    exp = os.path.join(tmp, "pkg")
    subprocess.run(["pkgutil", "--expand-full", path, exp], check=True, capture_output=True)
    waves = [p for p in glob.glob(f"{exp}/**/PCMWave", recursive=True) if "Compact.SpeechVoice" not in p]
    if len(waves) != 1:
        raise RuntimeError(f"{path}: expected one premium PCMWave, got {waves}")
    with tarfile.open(waves[0]) as t:
        for m in t.getmembers():
            name = codecs.decode(os.path.basename(m.name), "rot13")
            if m.isfile() and re.match(r"synth_.*_full_.*\.dat$", name):
                dest = os.path.join(tmp, name)
                with t.extractfile(m) as f, open(dest, "wb") as o:
                    while True:
                        b = f.read(1 << 24)
                        if not b:
                            break
                        o.write(b)
                return dest
    raise RuntimeError(f"{path}: no synth file in PCMWave")


def vinf(font):
    d = open(font, "rb").read(0x400)
    i = d.find(bytes(c ^ 0xA9 for c in b"VINF"))
    ln = int.from_bytes(d[i + 4:i + 8], "little")
    txt = bytes(c ^ 0xA9 for c in d[i + 8:i + 8 + ln]).decode("latin1").split("\0")
    return dict(zip(txt[0::2], txt[1::2]))


def md5(path):
    h = hashlib.md5()
    with open(path, "rb") as f:
        while True:
            b = f.read(1 << 24)
            if not b:
                break
            h.update(b)
    return h.hexdigest()


def manifest():
    rows = {}
    if os.path.exists(MANIFEST):
        for line in open(MANIFEST):
            f = line.rstrip("\n").split("\t")
            if len(f) >= 6 and f[0] != "key":
                rows[f[0]] = f + [""] * (len(COLS) - len(f))
    return rows


def record(fields):
    fields = [str(x) for x in fields] + [""] * (len(COLS) - len(fields))
    with open(MANIFEST, "a") as f:
        if f.tell() == 0:
            f.write("\t".join(COLS) + "\n")
        f.write("\t".join(fields) + "\n")


def decode_window(path, skip, n):
    raw = subprocess.check_output(["flac", "-d", "-c", "--silent", "--force-raw-format", "--endian=little", "--sign=signed",
                                   f"--skip={skip}", f"--until=+{n}", path])
    return np.frombuffer(raw, dtype="<i2").astype(np.float64)


def same_recording(a, b, seconds):
    """True when FLACs a and b are the same recording (possibly different codecs):
    three 20 s windows each correlate > 0.8 after a lag search of +-4000 samples."""
    sr, w, ml = 22050, 20 * 22050, 4000
    total = int(seconds * sr)
    corrs = []
    for i in range(3):
        start = int((i + 0.5) * total / 3)
        x = decode_window(a, start, w + 2 * ml)
        y = decode_window(b, start, w + 2 * ml)
        if len(x) < w + 2 * ml or len(y) < w + 2 * ml:
            return False
        c = np.correlate(y[: w // 2 + 2 * ml], x[ml:ml + w // 2], mode="valid")
        lag = int(np.argmax(c)) - ml
        xs, ys = x[ml:ml + w], y[ml + lag:ml + lag + w]
        corrs.append(float(np.corrcoef(xs, ys)[0, 1]))
    return min(corrs) > 0.8


def voice_of(flac):
    m = re.match(r"([^_]+)_([^_]+)_(.+)\.flac$", os.path.basename(flac))
    return m.group(2), m.group(3)


def one(src):
    base = os.path.basename(src)
    key = re.sub(r"\.(zip|pkg)$", "", base)
    done = manifest()
    if key in done:
        return f"{key}: already in manifest"
    if base.endswith(".pkg") and key not in MAC:
        return f"{key}: not in MacVoiceCatalog, skipped"
    tmp = tempfile.mkdtemp(prefix=key + "_", dir="build/corpus")
    try:
        font = font_from_zip(src, tmp) if src.endswith(".zip") else font_from_pkg(src, tmp)
        info = vinf(font)
        digest = md5(font)
        if base.endswith(".pkg"):
            v = MAC[key]
            name, locale, build = v["name"].lower(), v["locale"], v["era"]
        else:
            build, locale = key.split("_", 1)
            name = info.get("Voice", "voice")
        flac = f"{locale}/{build}_{name}_{locale}.flac"
        os.makedirs(f"{OUT}/{locale}", exist_ok=True)
        dup = [r for r in done.values() if r[1] == digest]
        if dup:
            with LOCK:
                record([key, digest, dup[0][2], src, info.get("Coder"), dup[0][5], os.path.basename(font), f"identical font to {dup[0][0]}", dup[0][8]])
            return f"{key}: identical font to {dup[0][0]} ({dup[0][2]}), not decoded again"
        raw = os.path.join(tmp, "pcm.raw")
        logf = open(os.path.join("build/corpus", key + ".log"), "w")
        subprocess.run(["build/vxcorpus_native", *ENGINE, font, raw], check=True, stdout=logf, stderr=logf)
        logf.close()
        secs = os.path.getsize(raw) / 2 / int(info.get("Freq", "22050"))
        pcm_md5 = md5(raw)
        subprocess.run(["flac", "--best", "--force", "--silent", "--endian=little", "--sign=signed", "--channels=1",
                        "--bps=16", f"--sample-rate={info.get('Freq', '22050')}",
                        "-T", f"TITLE={name.capitalize()} ({locale}, {build}) unit-selection corpus",
                        "-T", "ARTIST=Nuance Vocalizer 5.2 voice corpus",
                        "-T", f"DESCRIPTION=Decoded {info.get('Coder')} SPCH stream of {os.path.basename(font)} with the engine's own decoder",
                        "-T", f"SOURCE={base}", "-o", f"{OUT}/{flac}", raw], check=True)
        with LOCK:
            done = manifest()
            same = [r for r in done.values() if r[8] == pcm_md5 and r[2] != flac]
            if same:
                os.remove(f"{OUT}/{flac}")
                record([key, digest, same[0][2], src, info.get("Coder"), f"{secs:.2f}", os.path.basename(font),
                        f"same decoded audio as {same[0][0]} (FLAC deleted)", pcm_md5])
                return f"{key}: decoded audio identical to {same[0][0]} ({same[0][2]}), FLAC deleted"
            if info.get("Coder") == "155mrf22":
                for r in done.values():
                    if r[4] == "vssq5" and r[2] and os.path.exists(f"{OUT}/{r[2]}") and voice_of(r[2]) == (name, locale) \
                            and abs(float(r[5]) - secs) < 2 and same_recording(f"{OUT}/{r[2]}", f"{OUT}/{flac}", secs):
                        os.remove(f"{OUT}/{flac}")
                        record([key, digest, r[2], src, info.get("Coder"), f"{secs:.2f}", os.path.basename(font),
                                f"same recording as {r[2]} (27 kbps CELP copy, zero-lag null; FLAC deleted)", pcm_md5])
                        return f"{key}: same recording as {r[0]} ({r[2]}), CELP FLAC deleted"
            record([key, digest, flac, src, info.get("Coder"), f"{secs:.2f}", os.path.basename(font), "", pcm_md5])
        return f"{key}: {flac} ({secs / 3600:.2f} h, {info.get('Coder')})"
    except Exception as e:
        return f"{key}: FAILED {e}"
    finally:
        subprocess.run(["rm", "-rf", tmp])


def main():
    args = sys.argv[1:]
    jobs = 3
    if args[:1] == ["-j"]:
        jobs, args = int(args[1]), args[2:]
    srcs = args or sorted(glob.glob("downloads/assets/*.zip") + glob.glob("downloads/macvoices/mac10*.pkg"))
    srcs = sorted(srcs, key=lambda p: (not p.endswith(".pkg"), p))  # Mac ADPCM first
    with ThreadPoolExecutor(jobs) as ex:
        for msg in ex.map(one, srcs):
            log(msg)
    log("done")


if __name__ == "__main__":
    main()
