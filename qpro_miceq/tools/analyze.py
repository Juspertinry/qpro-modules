#!/usr/bin/env python3
# Per-channel level and 1/3-octave spectrum of a WAV capture.
# usage: analyze.py a.wav [b.wav]   (second file prints a difference column)
import sys
import wave

import numpy as np

BANDS = [50, 63, 80, 100, 125, 160, 200, 250, 315, 400, 500, 630, 800, 1000,
         1250, 1600, 2000, 2500, 3150, 4000, 5000, 6300, 8000, 10000, 12500,
         16000, 20000]


def load(path):
    with wave.open(path) as w:
        ch, rate = w.getnchannels(), w.getframerate()
        x = np.frombuffer(w.readframes(w.getnframes()), dtype='<i2')
    return x.reshape(-1, ch).T.astype(np.float64) / 32768.0, rate


def spectrum(x, rate, n=8192):
    # Welch average, Hann window
    win = np.hanning(n)
    segs = [x[i:i + n] * win for i in range(0, len(x) - n, n // 2)]
    p = np.mean([np.abs(np.fft.rfft(s)) ** 2 for s in segs], axis=0)
    f = np.fft.rfftfreq(n, 1 / rate)
    out = []
    for c in BANDS:
        lo, hi = c / 2 ** (1 / 6), c * 2 ** (1 / 6)
        m = (f >= lo) & (f < hi)
        out.append(10 * np.log10(p[m].sum() + 1e-20) if m.any() else np.nan)
    return np.array(out)


def summary(path):
    x, rate = load(path)
    print(f'{path}: {x.shape[0]}ch {rate}Hz {x.shape[1] / rate:.1f}s')
    specs = []
    for i, c in enumerate(x):
        rms = 20 * np.log10(np.sqrt(np.mean(c ** 2)) + 1e-12)
        peak = 20 * np.log10(np.max(np.abs(c)) + 1e-12)
        # noise floor: quietest 10% of 20 ms blocks
        blk = c[: len(c) // 960 * 960].reshape(-1, 960)
        br = np.sort(20 * np.log10(np.sqrt(np.mean(blk ** 2, axis=1)) + 1e-12))
        floor = br[: max(1, len(br) // 10)].mean()
        print(f'  ch{i}: rms {rms:6.1f} dBFS  peak {peak:6.1f}  floor {floor:6.1f}')
        specs.append(spectrum(c, rate))
    return specs


def main():
    a = summary(sys.argv[1])
    b = summary(sys.argv[2]) if len(sys.argv) > 2 else None
    hdr = '  band ' + ''.join(f'   A.ch{i}' for i in range(len(a)))
    if b:
        hdr += ''.join(f'   B.ch{i}' for i in range(len(b)))
        hdr += ''.join(f'  B-A.{i}' for i in range(min(len(a), len(b))))
    print(hdr)
    for k, c in enumerate(BANDS):
        row = f'{c:6d} ' + ''.join(f'{s[k]:8.1f}' for s in a)
        if b:
            row += ''.join(f'{s[k]:8.1f}' for s in b)
            row += ''.join(f'{b[i][k] - a[i][k]:8.1f}'
                           for i in range(min(len(a), len(b))))
        print(row)


if __name__ == '__main__':
    main()
