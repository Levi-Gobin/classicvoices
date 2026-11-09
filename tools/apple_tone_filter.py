#!/usr/bin/env python3
"""The coefficients of core/vxstream.c's Apple tone filter (vxs_set_apple_tone).

Apple's playback (iOS and macOS) converts the voices' 22,050 Hz audio to the
hardware's rate with its sample-rate converter at medium quality, which rolls
off the top of the band: -0.8 dB at 9 kHz, -2.7 at 9.5, -6.8 at 10, -13.9
at 10.5, -20 at 11. Windows, Linux and Android keep that band, which sounds
brighter. This measures the curve on this Mac (afconvert, quality 64: the same
response as AVAudioEngine's mixer to 0.1 dB) with white noise, and turns it
into a minimum-phase FIR at 22,050 Hz: 24 taps match it to 0.01 dB below 11 kHz
with a group delay of 0.1 sample, so speech starts no later.

    tools/apple_tone_filter.py      (macOS; prints the C table)
"""
import os
import subprocess
import tempfile
import wave

import numpy as np

TAPS = 24
RATE = 22050


def measure(tmp):
    rng = np.random.default_rng(7)
    x = rng.standard_normal(RATE * 60) * 0.1
    src, dst = os.path.join(tmp, "noise.wav"), os.path.join(tmp, "out.wav")
    with wave.open(src, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(np.round(x * 32767).astype(np.int16).tobytes())
    subprocess.check_call(["afconvert", "-f", "WAVE", "-d", "LEF32@44100", "-r", "64", src, dst])
    with wave.open(src) as w:
        x = np.frombuffer(w.readframes(w.getnframes()), np.int16) / 32768.0
    y = read_f32(dst)[::2][:len(x)]  # back on 22,050 Hz terms: exact below 11 kHz for the response
    n = 1024
    win = np.hanning(n)
    sxy = sxx = 0
    for i in range(0, len(x) - n, n // 2):
        X, Y = np.fft.rfft(x[i:i + n] * win), np.fft.rfft(y[i:i + n] * win)
        sxy, sxx = sxy + Y * np.conj(X), sxx + np.abs(X) ** 2
    f = np.fft.rfftfreq(n, 1 / RATE)
    h = np.abs(sxy / sxx)
    return f, h / h[(f > 500) & (f < 3000)].mean()


def read_f32(path):
    import struct
    b = open(path, "rb").read()
    i = 12
    while i < len(b):
        cid, size = b[i:i + 4], struct.unpack_from("<I", b, i + 4)[0]
        if cid == b"data":
            return np.frombuffer(b[i + 8:i + 8 + size], np.float32).astype(np.float64)
        i += 8 + size + (size & 1)
    raise ValueError("no data in " + path)


def min_phase(f_meas, h_meas, taps):
    nf = 1 << 14
    f = np.linspace(0, RATE / 2, nf // 2 + 1)
    mag = np.maximum(np.interp(f, f_meas, h_meas), 1e-4)
    c = np.fft.ifft(np.log(np.concatenate([mag, mag[-2:0:-1]]))).real
    fold = np.zeros(len(c))
    fold[0] = fold[len(c) // 2] = 1
    fold[1:len(c) // 2] = 2
    h = np.fft.ifft(np.exp(np.fft.fft(c * fold))).real[:taps]
    h *= np.concatenate([np.ones(taps - taps // 4), np.hanning(2 * (taps // 4))[taps // 4:]])
    return h / h.sum()  # passes speech's low frequencies at exactly their level


def main():
    with tempfile.TemporaryDirectory() as tmp:
        f, h = measure(tmp)
    k = min_phase(f, h, TAPS)
    print("static const float kAppleTone[%d] = {" % TAPS)
    for i in range(0, TAPS, 4):
        print("    " + " ".join("%.9ef," % v for v in k[i:i + 4]))
    print("};")


if __name__ == "__main__":
    main()
