#!/usr/bin/env python3
"""Drive a Speech Dispatcher output module the way the server does, without a
server: INIT, AUDIO (server audio), LIST VOICES, SET, SPEAK, STOP, QUIT.
Checks the replies and events, decodes the audio frames, and writes what was
spoken to a WAV per message.

  tests/speechd_module_test.py path/to/sd_classicvoices [out_dir]
"""
import os
import select
import subprocess
import sys
import time
import wave

mod = sys.argv[1]
out_dir = sys.argv[2] if len(sys.argv) > 2 else "."
p = subprocess.Popen([mod, "/nonexistent.conf"], stdin=subprocess.PIPE, stdout=subprocess.PIPE)
buf = b""


def send(s):
    p.stdin.write(s.encode())
    p.stdin.flush()


def read_until(pred, timeout=30):
    """Read stdout lines (decoding audio frames) until pred(line) is true."""
    global buf
    lines, audio = [], bytearray()
    end = time.time() + timeout
    while time.time() < end:
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            if line.startswith(b"705-AUDIO\x00"):
                raw = line[len(b"705-AUDIO\x00"):]
                out, i = bytearray(), 0
                while i < len(raw):
                    if raw[i] == 0x7d:
                        out.append(raw[i + 1] ^ 0x20)
                        i += 2
                    else:
                        out.append(raw[i])
                        i += 1
                audio += out
                continue
            text = line.decode("utf-8", "replace")
            if not text.startswith("705"):
                lines.append(text)
            if pred(text):
                return lines, bytes(audio)
        r, _, _ = select.select([p.stdout], [], [], 0.2)
        if r:
            chunk = os.read(p.stdout.fileno(), 65536)
            if not chunk:
                break
            buf += chunk
    raise SystemExit("timeout waiting; got %r" % lines[-5:])


def wav(name, audio):
    with wave.open(os.path.join(out_dir, name), "wb") as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(22050); w.writeframes(audio)


send("INIT\n")
lines, _ = read_until(lambda l: l.startswith("299 ") or l.startswith("399 "))
print("INIT:", lines)
assert lines[-1].startswith("299")
send("AUDIO\naudio_output_method=server\n.\n")
print("AUDIO:", read_until(lambda l: l[:3] in ("203", "300", "303"))[0])
send("LIST VOICES\n")
lines, _ = read_until(lambda l: l.startswith("200 "))
voices = [l[4:].split("\t") for l in lines if l.startswith("200-")]
print("voices:", len(voices), voices[:3])
assert voices

def speak(label, settings, text, kind="SPEAK"):
    send("SET\n" + "".join("%s=%s\n" % kv for kv in settings.items()) + ".\n")
    read_until(lambda l: l.startswith("203"))
    send(kind + "\n")
    read_until(lambda l: l.startswith("202"))
    send(text + "\n.\n")
    t0 = time.time()
    lines, audio = read_until(lambda l: l.startswith("702 END") or l.startswith("703 STOP") or l.startswith("301"))
    events = [l for l in lines if l[:3] in ("200", "700", "701", "702", "703", "301")]
    print("%s: %d ms audio in %.2f s, events %s" % (label, len(audio) / 44.1, time.time() - t0, events))
    wav(label + ".wav", audio)
    return events, audio

ev, a = speak("plain", {"synthesis_voice": "Samantha (iOS 6)", "rate": "0", "pitch": "0"}, "Hello from speech dispatcher.")
assert any(e.startswith("701") for e in ev) and ev[-1].startswith("702") and len(a) > 22050
ev, a = speak("ssml_marks", {"rate": "30"},
              '<speak>First part <mark name="one"/> second part <mark name="two"/> end.</speak>')
marks = [e[4:] for e in ev if e.startswith("700-")]
assert marks == ["one", "two"], marks
ev, a = speak("char", {"cap_let_recogn": "pitch"}, "A", "CHAR")
ev, a = speak("key", {}, "shift_a", "KEY")
ev, a = speak("enhanced_lang", {"synthesis_voice": "NULL", "language": "en-GB"}, "Choosing by language.")

# STOP in the middle of a long message
send("SET\nsynthesis_voice=Samantha (iOS 6)\n.\n"); read_until(lambda l: l.startswith("203"))
send("SPEAK\n"); read_until(lambda l: l.startswith("202"))
send(("This is a long sentence that will be interrupted part way through. " * 6) + "\n.\n")
time.sleep(0.4)
send("STOP\n")
lines, audio = read_until(lambda l: l.startswith("703 STOP") or l.startswith("702 END"))
print("stop:", [l for l in lines if l[:3] in ("701", "702", "703")], "%d ms audio before stop" % (len(audio) / 44.1))
assert lines[-1].startswith("703")

# a quick STOP then a new message (what Orca does while arrowing)
for i in range(5):
    send("SPEAK\n"); read_until(lambda l: l.startswith("202"))
    send("Item number %d in a list\n.\n" % i)
    time.sleep(0.05)
    send("STOP\n")
    read_until(lambda l: l.startswith("703 STOP") or l.startswith("702 END"))
ev, a = speak("after_stops", {}, "Still speaking after interruptions.")
assert ev[-1].startswith("702") and len(a) > 22050
send("QUIT\n")
p.wait(timeout=10)
print("ok, exit", p.returncode)
