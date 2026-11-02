#!/usr/bin/env python3
"""Record the "Play sample" greeting of every voice the app downloads, so the
browser can play it before the voice is on the device (as iOS does with its
own voices). Each voice is downloaded from Apple, spoken once, encoded as AAC
and deleted again; already-made samples are kept, so the run can be resumed.

    make_samples.py <cvsay> <Voices folder> <work folder> [siri|enhanced|mac ...]
    make_samples.py alex <mtsay>

<cvsay> is linux/cvsay.c built for the Mac with PCRE2 linked in
(-DCV_PCRE2_STATIC, tools/pcre2_build.py): without it Apple's pronunciation
rules are skipped and "iOS" isn't spelled out as the app does. <Voices folder> the engines (the
installed app's Synth.appex/Contents/Resources/Voices). Samples go to
ios/Samples/<identifier without com.levigobin.ClassicVoices.>.m4a. The
greeting is VoiceItem.greeting of a row that isn't downloaded yet."""
import os
import shutil
import subprocess
import sys
import threading
from concurrent.futures import ThreadPoolExecutor

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "nvda", "addon", "synthDrivers"))
from _classicvoices import download as dl  # noqa: E402

OUT = os.path.join(ROOT, "ios", "Samples")
VERSION = {"ios5": "iOS 5", "ios6": "iOS 6", "ios70": "iOS 7", "siri7": "Siri in iOS 7",
           "mac107": "Mac OS X Lion", "mac108": "Mac OS X Mountain Lion", "mac109": "Mac OS X Mavericks"}
WORKERS = int(os.environ.get("WORKERS", "3"))


def greeting(name, enhanced, build):
    return "Hello, my name is %s. %s %s." % (name, "I am an enhanced voice from" if enhanced else "I am from",
                                             VERSION[build])


def jobs(kinds):
    """(sample name, kind, catalog entry, voice key, name or None, enhanced)"""
    out = []
    if "siri" in kinds:
        for v in dl._catalog("SiriVoiceCatalog.json")["voices"]:
            folder = v["name"] + ("Compact" if v["footprint"] == "compact" else "")
            out.append(("siri7." + folder, "siri", v, "siri7/" + folder, v["name"], v["footprint"] != "compact"))
    if "enhanced" in kinds:
        for a in dl.enhanced_catalog():
            # the row's name is the built-in compact's for that language: read once installed
            out.append(("%se.%s" % (a["build"], a["language"]), "enhanced", a,
                        "%se/%s" % (a["build"], a["language"]), None, True))
    if "mac" in kinds:
        for v in dl.mac_catalog():
            out.append(("%s.%s" % (v["era"], v["name"]), "mac", v, "%s/%s" % (v["era"], v["name"]), v["name"], True))
    return out


def compact_names(cvsay, data):
    """{key: name} of the bundled voices, from `cvsay -l` ("Samantha (iOS 6)\ten-US\tios6/en-US")."""
    env = dict(os.environ, CLASSICVOICES_DATA=data)
    lines = subprocess.run([cvsay, "-l"], env=env, capture_output=True, text=True).stdout.splitlines()
    return {l.split("\t")[2]: l.split("\t")[0].rsplit(" (", 1)[0] for l in lines if l.count("\t") == 2}


def alex(mtsay):
    """Alex (MacinTalk, from the install discs) has no download here: he is
    spoken from extracted/mtNNN/Voices/Alex.SpeechVoice with core/mt/mtsay.c.
    MacinTalk never had pronunciation rules."""
    for gen, release in (("mt105", "Leopard"), ("mt106", "Snow Leopard"), ("mt107", "Lion")):
        root = os.path.join(ROOT, "extracted", gen)
        if not os.path.isdir(os.path.join(root, "Voices", "Alex.SpeechVoice")):
            print("%s.Alex: no %s/Voices/Alex.SpeechVoice" % (gen, root))
            continue
        wav = os.path.join(OUT, gen + ".Alex.wav")
        text = "Hello, my name is Alex. I am from Mac OS X %s." % release
        subprocess.run([mtsay, root, "Alex", text, wav], check=True, capture_output=True)
        subprocess.run(["afconvert", "-f", "m4af", "-d", "aac", "-b", "40000", wav,
                        os.path.join(OUT, gen + ".Alex.m4a")], check=True)
        os.remove(wav)
        print("%s.Alex: %s" % (gen, text))


def main():
    if sys.argv[1] == "alex":  # make_samples.py alex <mtsay>
        return alex(os.path.abspath(sys.argv[2]))
    cvsay, voices = os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2])
    work = os.path.abspath(sys.argv[3])
    kinds = sys.argv[4:] or ["siri", "enhanced", "mac"]
    os.makedirs(OUT, exist_ok=True)
    slots = []
    for i in range(WORKERS):
        d = os.path.join(work, "d%d" % i)
        os.makedirs(os.path.join(d, "Downloads"), exist_ok=True)
        if not os.path.lexists(os.path.join(d, "Voices")):
            os.symlink(voices, os.path.join(d, "Voices"))
        slots.append(d)
    names = compact_names(cvsay, slots[0])
    free = list(slots)
    lock = threading.Lock()
    todo = [j for j in jobs(kinds) if not os.path.exists(os.path.join(OUT, j[0] + ".m4a"))]
    print("%d samples to make" % len(todo), flush=True)

    def one(job):
        sample, kind, entry, key, name, enhanced = job
        with lock:
            data = free.pop()
        downloads = os.path.join(data, "Downloads")
        try:
            dl.download(kind, entry, downloads, lambda *_: None)
            if name is None:
                build, lang = key.split("/")
                name = names.get("%s/%s" % (build[:-1], lang), lang)
            build = key.split("/")[0]
            # a package's folder can differ in case from the catalog's name (Sin-ji)
            d = os.path.join(downloads, build)
            if os.path.isdir(d) and not os.path.isdir(os.path.join(downloads, key)):
                key = next((build + "/" + f for f in os.listdir(d) if f.lower() == key.split("/")[1].lower()), key)
            text = greeting(name, enhanced, build[:-1] if kind == "enhanced" else build)
            wav = os.path.join(data, "sample.wav")
            env = dict(os.environ, CLASSICVOICES_DATA=data)
            r = subprocess.run([cvsay, "-q", "-v", key, "-o", wav, text], env=env, capture_output=True, text=True)
            if r.returncode or not os.path.exists(wav) or os.path.getsize(wav) < 8000:
                return "%s: FAILED %s" % (sample, (r.stderr or r.stdout).strip()[:200])
            m4a = os.path.join(OUT, sample + ".m4a")
            subprocess.run(["afconvert", "-f", "m4af", "-d", "aac", "-b", "40000", wav, m4a + ".part.m4a"], check=True)
            os.replace(m4a + ".part.m4a", m4a)
            os.remove(wav)
            return "%s: %s (%d KB)" % (sample, text, os.path.getsize(m4a) >> 10)
        except Exception as e:  # noqa: BLE001 - keep going, report it
            return "%s: FAILED %s" % (sample, e)
        finally:
            for f in os.listdir(downloads):
                p = os.path.join(downloads, f)
                shutil.rmtree(p) if os.path.isdir(p) and not os.path.islink(p) else os.remove(p)
            with lock:
                free.append(data)

    for attempt in range(3):  # Apple's servers time out now and then: try those again
        with ThreadPoolExecutor(WORKERS) as ex:
            for msg in ex.map(one, todo):
                print(msg, flush=True)
        todo = [j for j in todo if not os.path.exists(os.path.join(OUT, j[0] + ".m4a"))]
        if not todo:
            break
        print("%d to try again" % len(todo), flush=True)
    print("done", flush=True)


if __name__ == "__main__":
    main()
