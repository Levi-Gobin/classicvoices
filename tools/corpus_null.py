#!/usr/bin/env python3
"""Null-test two decoded corpora (FLAC): align by cross-correlation and report
the residual. Usage:
  corpus_null.py ref.flac test.flac [--windows N] [--diff out.wav --seconds S]
--windows: N 20-second windows spread over the file (SNR of ref - test).
--diff: write the first S seconds of (ref - aligned test) as a WAV, plus a
        stereo A/B WAV (left = ref, right = test)."""
import argparse, subprocess, numpy as np, wave, struct

def total_samples(p):
    return int(subprocess.check_output(["metaflac", "--show-total-samples", p]).strip())

def decode(p, skip, n):
    raw = subprocess.check_output(["flac", "-d", "-c", "--silent", "--force-raw-format", "--endian=little", "--sign=signed",
                                   f"--skip={skip}", f"--until=+{n}", p])
    return np.frombuffer(raw, dtype="<i2").astype(np.float64)

def best_lag(a, b, maxlag=4000):
    # lag such that b[i + lag] ~ a[i]; use FFT cross-correlation on a 1 s slice
    n = min(len(a), len(b)) - maxlag
    x = a[maxlag:maxlag + 44100]; y = b[:44100 + 2 * maxlag]
    c = np.correlate(y, x, mode="valid")  # index k => y[k..] matches x
    k = int(np.argmax(c))
    return k - maxlag

def snr_db(ref, test):
    e = ref - test
    return 10 * np.log10(np.sum(ref ** 2) / max(np.sum(e ** 2), 1e-9))

ap = argparse.ArgumentParser()
ap.add_argument("ref"); ap.add_argument("test")
ap.add_argument("--windows", type=int, default=8)
ap.add_argument("--diff"); ap.add_argument("--seconds", type=float, default=180)
a = ap.parse_args()
SR = 22050
na, nb = total_samples(a.ref), total_samples(a.test)
print(f"ref {na / SR / 3600:.3f} h, test {nb / SR / 3600:.3f} h, length diff {nb - na:+d} samples")
W = 20 * SR
for i in range(a.windows):
    start = int((i + 0.5) * min(na, nb) / a.windows)
    ra, rb = decode(a.ref, start, W + 8000), decode(a.test, start, W + 8000)
    lag = best_lag(ra, rb)
    x = ra[4000:4000 + W]; y = rb[4000 + lag:4000 + lag + W]
    print(f"  window @{start / SR / 60:7.1f} min: lag {lag:+d}, SNR {snr_db(x, y):6.2f} dB, ref rms {np.sqrt(np.mean(x**2)):7.0f}, corr {np.corrcoef(x, y)[0,1]:.5f}")
if a.diff:
    n = int(a.seconds * SR)
    ra, rb = decode(a.ref, 0, n + 8000), decode(a.test, 0, n + 8000)
    lag = best_lag(ra, rb)
    x = ra[4000:4000 + n]; y = rb[4000 + lag:4000 + lag + n]
    d = np.clip(x - y, -32768, 32767).astype("<i2")
    print(f"diff: lag {lag:+d}, SNR {snr_db(x, y):.2f} dB over {a.seconds:.0f} s -> {a.diff}")
    with wave.open(a.diff, "wb") as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(SR); w.writeframes(d.tobytes())
    ab = np.stack([x, y], axis=1).astype("<i2")
    with wave.open(a.diff.replace(".wav", "_AB.wav"), "wb") as w:
        w.setnchannels(2); w.setsampwidth(2); w.setframerate(SR); w.writeframes(ab.tobytes())
