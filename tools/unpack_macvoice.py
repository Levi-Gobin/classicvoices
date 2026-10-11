#!/usr/bin/env python3
"""Unpack a Mac OS X 10.7-10.9 Nuance voice (<Name>.SpeechVoice/Contents/Resources/PCMWave:
a tar archive with ROT13-encoded member names) into a plain directory.
Usage: unpack_macvoice.py <PCMWave> <outdir>"""
import codecs, os, sys, tarfile
src, out = sys.argv[1], sys.argv[2]
os.makedirs(out, exist_ok=True)
with tarfile.open(src) as t:
    for m in t.getmembers():
        if not m.isfile():
            continue
        name = codecs.decode(os.path.basename(m.name), "rot13")
        with t.extractfile(m) as f, open(os.path.join(out, name), "wb") as o:
            o.write(f.read())
        print(f"{name:45} {m.size}")
