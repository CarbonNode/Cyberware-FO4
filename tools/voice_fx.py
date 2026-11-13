"""Varga's world treatment (RIPPERDOC VOICES, docs/RIPPERDOC-VOICE.md): cap dead air, a basement-clinic room, a faint
chrome sheen, a low noise floor, then a linear gain to -18 LUFS (EBU R128, true peak <= -1.5 dBTP). Needs numpy and ffmpeg
(with afreqshift).

    python voice_fx.py in.(mp3|wav) out.wav [--cap] [--lufs -18] [--limit]

Loudness is measured with ffmpeg's ebur128 and applied as one gain, NOT with loudnorm: loudnorm needs 3 s of audio and
silently mangles anything shorter (Varga's 2.99 s "We never discussed this." came out at -38.8 LUFS, 21 dB under the
rest, 2026-11-09). Vanilla FO4 dialogue measures about -22 to -25 LUFS (FemaleEvenToned, Piper, FemaleBoston, 24 clips).

ebur128 itself is NOT duration-sensitive at these lengths: padding a clip with 1-5 s of silence moves the reading by
0.00 dB on clips of 2.99 s, 4.03 s and 9.72 s (measured 2026-11-10). So a short clip can be measured as it is. What the
paragraph above describes is a clip MASTERED in the loudnorm era and never re-treated after the switch; it sat in the
shipped set 21 dB down until a sweep of every final caught it (2026-11-10). A switch of method does not repair the files
the old method already wrote -- re-measure the whole set after one, which `sweep` now does. `relevel()` is the repair:
it corrects an already-treated clip's level without re-rendering or re-treating it, so an accepted performance survives.
"""
import re
import os
import subprocess
import sys
import tempfile
import wave

import numpy as np

SR = 44100
ROOM_WET = 0.16          # room level against the dry voice
SHEEN_DB = -20.0         # chrome sheen under the voice; if you can name it as an effect, it is too loud
FLOOR_DB = -62.0         # room noise floor
CAP_S = 0.75             # longest silence kept inside a line
LUFS = -18.0             # integrated loudness target (vanilla dialogue is ~-23; see the docstring)
TRUE_PEAK = -1.5         # dBTP ceiling after gain
LIMIT_MARGIN = 0.4       # headroom under TRUE_PEAK the limiter aims for (ffmpeg's true peak oversamples)
LIMIT_MAX_DB = 3.0       # most the limiter will pull a peak down; past this it is a take problem, not a mastering one


def loudness(path):
    """(integrated LUFS, true peak dBTP) of a file, by ffmpeg ebur128.

    Measured 2026-11-10: the reading does NOT move when the clip is padded with silence (0.00 dB
    across 1-5 s of padding, on clips of 2.99 s, 4.03 s and 9.72 s), so it is stable on short
    clips too -- the module docstring's warning was about a clip that really was 21 dB down.
    """
    err = subprocess.run(['ffmpeg', '-hide_banner', '-nostats', '-i', path, '-af', 'ebur128=peak=true', '-f', 'null', '-'],
                         capture_output=True, text=True, check=True).stderr
    summary = err[err.rindex('Summary:'):]
    i = float(re.search(r'I:\s+(-?[\d.]+|-inf) LUFS', summary).group(1))
    tp = float(re.search(r'Peak:\s+(-?[\d.]+|-inf) dBFS', summary).group(1))
    return i, tp


def load(path):
    raw = subprocess.run(['ffmpeg', '-loglevel', 'error', '-i', path, '-ac', '1', '-ar', str(SR), '-f', 's16le', '-'],
                         capture_output=True, check=True).stdout
    return np.frombuffer(raw, np.int16).astype(np.float32) / 32768


def save(path, x):
    x = np.clip(x, -1, 1)
    with wave.open(path, 'wb') as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(SR)
        w.writeframes((x * 32767).astype(np.int16).tobytes())


def fftconv(a, b):
    n = 1 << int(np.ceil(np.log2(len(a) + len(b))))
    return np.fft.irfft(np.fft.rfft(a, n) * np.fft.rfft(b, n), n)[:len(a) + len(b) - 1]


def cap_pauses(x, max_s=CAP_S, thr_db=-45):
    hop = 441
    m = len(x) // hop * hop
    e = np.sqrt((x[:m].reshape(-1, hop) ** 2).mean(1))
    sil = 20 * np.log10(e + 1e-9) < thr_db
    keep = np.ones(len(x), bool)
    i = 0
    while i < len(sil):
        if not sil[i]:
            i += 1
            continue
        j = i
        while j < len(sil) and sil[j]:
            j += 1
        if (j - i) * hop / SR > max_s and i > 0 and j < len(sil):   # inner pauses only; edges stay
            keep[i * hop + int(max_s / 2 * SR):j * hop - int(max_s / 2 * SR)] = False
        i = j
    return x[keep]


def room_ir(rt60=0.32, seed=7):
    rng = np.random.default_rng(seed)
    n = int(SR * rt60 * 1.2)
    t = np.arange(n) / SR
    ir = rng.standard_normal(n) * np.exp(-6.9 * t / rt60)
    for d, g in ((0.0045, .5), (0.0071, .35), (0.0113, .28), (0.0162, .2)):   # close walls, low ceiling
        ir[int(d * SR)] += g * 3
    ir[:int(0.003 * SR)] = 0
    k = np.exp(-np.arange(64) / 6.0)
    ir = fftconv(ir, k / k.sum())[:n]                                         # damped highs: concrete and cloth
    return ir / np.abs(ir).max()


def highpass_fft(x, fc):
    X = np.fft.rfft(x)
    X[np.fft.rfftfreq(len(x), 1 / SR) < fc] = 0
    return np.fft.irfft(X, len(x)).astype(np.float32)


def _running_max(a, w):
    """Sliding maximum, |shift| <= w, without scipy."""
    out = a.copy()
    for s in range(1, w + 1):
        out[:-s] = np.maximum(out[:-s], a[s:])
        out[s:] = np.maximum(out[s:], a[:-s])
    return out


def _box_norm(a, w):
    """Moving average over 2w+1, normalised by the REAL overlap so the ends are not faded.

    A plain np.convolve(a, k, 'same') zero-pads past the ends, which on a gain curve reads as up to
    6 dB of reduction in the first and last half-window that is pure artifact (seen 2026-11-10).
    """
    k = np.ones(2 * w + 1)
    return np.convolve(a, k, 'same') / np.convolve(np.ones_like(a), k, 'same')


def _limit(y, ceiling_lin, look_ms=2.0, smooth_ms=4.0):
    """Lookahead true-peak limiter: duck only around peaks over the ceiling, smoothly.

    Returns (limited, max reduction in dB (<= 0), samples pulled down by more than 0.5 dB).
    """
    w1 = max(1, int(look_ms * 1e-3 * SR))
    w2 = max(1, int(smooth_ms * 1e-3 * SR))
    env = _running_max(np.abs(y).astype(np.float64), w1 + w2)   # max-filter wider than the smoother,
    g = np.minimum(1.0, ceiling_lin / np.maximum(env, 1e-12))   # so smoothing cannot let a peak back through
    g = np.minimum(_box_norm(g, w2), 1.0)
    return (y * g).astype(np.float32), 20 * np.log10(max(g.min(), 1e-12)), int((g < 10 ** (-0.5 / 20)).sum())


def _limit_to(y, dst, lufs, i_pre, passes=4):
    """Reach `lufs` by gain, ducking peaks over the ceiling, without exceeding LIMIT_MAX_DB.

    Limiting costs a little loudness of its own, so one pass undershoots; this repays the
    residual and re-limits, bounded. Returns (out_i, out_tp, total reduction dB, touched samples)
    or None when it cannot get there inside LIMIT_MAX_DB.
    """
    ceiling = 10 ** ((TRUE_PEAK - LIMIT_MARGIN) / 20)
    z = y * 10 ** ((lufs - i_pre) / 20)
    worst = 0.0
    touched = 0
    for _ in range(passes):
        z, red, t = _limit(z, ceiling)
        worst = min(worst, red)
        touched = max(touched, t)
        if -worst > LIMIT_MAX_DB:
            return None
        save(dst, z)
        out_i, out_tp = loudness(dst)
        if out_i >= lufs - 0.15 and out_tp <= TRUE_PEAK:
            return out_i, out_tp, worst, touched
        z = z * 10 ** ((lufs - out_i) / 20)
    save(dst, z)
    out_i, out_tp = loudness(dst)
    if out_tp <= TRUE_PEAK and abs(out_i - lufs) <= 1.5:
        return out_i, out_tp, worst, touched
    return None


def relevel(src, dst, lufs=LUFS):
    """Correct an ALREADY-TREATED clip's level in place, without re-treating or re-rendering it.

    For a clip that shipped at the wrong level: the performance, room, sheen and floor are all
    already baked in at the right ratios, so a uniform gain (with the limiter for the peaks)
    restores the intended mix and changes nothing else. Returns (out_i, out_tp, reduction, touched).
    """
    y = load(src).astype(np.float32)
    i, tp = loudness(src)
    r = _limit_to(y, dst, lufs, i)
    if r is None:
        raise RuntimeError(f'{src}: cannot reach {lufs} LUFS inside {LIMIT_MAX_DB} dB of limiting')
    return r


def treat(src, dst, cap=False, lufs=LUFS, limit=False):
    x = load(src)
    if cap:
        x = cap_pauses(x)
    x = np.concatenate([np.zeros(int(0.08 * SR)), x, np.zeros(int(0.35 * SR))])
    wet = fftconv(x, room_ir())[:len(x)]
    wet *= np.sqrt((x ** 2).mean() / ((wet ** 2).mean() + 1e-12))
    fd, tmp = tempfile.mkstemp(suffix='.wav'); os.close(fd)
    save(tmp, x)
    raw = subprocess.run(['ffmpeg', '-loglevel', 'error', '-i', tmp, '-af', 'highpass=f=1800,lowpass=f=7000,afreqshift=shift=11',
                          '-ac', '1', '-ar', str(SR), '-f', 's16le', '-'], capture_output=True, check=True).stdout
    os.unlink(tmp)
    sheen = np.frombuffer(raw, np.int16).astype(np.float32)[:len(x)] / 32768
    sheen = np.pad(sheen, (0, len(x) - len(sheen)))
    floor = highpass_fft(np.random.default_rng(3).standard_normal(len(x)).astype(np.float32), 120) * 10 ** (FLOOR_DB / 20)
    y = x + ROOM_WET * wet + 10 ** (SHEEN_DB / 20) * sheen + floor
    mid = dst + '.pre.wav'
    save(mid, y)
    i, tp = loudness(mid)
    os.unlink(mid)
    if i == float('-inf'):
        raise RuntimeError(f'{src}: silent after treatment')
    gain = min(lufs - i, TRUE_PEAK - tp)   # never push the true peak over the ceiling
    short = (lufs - i) - gain              # how much level a transient is costing the whole line
    if limit and short > 0.05:
        # One sub-millisecond plosive can hold a whole line 1-2 dB under target (the gain above is
        # linear). Duck just that transient so the line can reach `lufs`. Opt-in: without limit=True
        # the behaviour is exactly as before, so no already-treated line changes.
        if short > LIMIT_MAX_DB:
            print(f'voice_fx: WARNING {os.path.basename(dst)} needs {short:.1f} dB of limiting to reach '
                  f'{lufs} LUFS, over the {LIMIT_MAX_DB} dB cap; left peak-limited. Use another take.')
        else:
            r = _limit_to(y, dst, lufs, i)
            if r is not None:
                out_i, out_tp, red, touched = r
                print(f'voice_fx: limiter engaged on {os.path.basename(dst)}: recovered {short:.1f} dB, '
                      f'peak pulled {-red:.2f} dB over {1000.0 * touched / SR:.1f} ms '
                      f'-> {out_i:.1f} LUFS / {out_tp:.1f} dBTP')
                return out_i, out_tp
            print(f'voice_fx: WARNING limiter on {os.path.basename(dst)} could not reach {lufs} LUFS '
                  f'inside {LIMIT_MAX_DB} dB; falling back to the linear gain. Use another take.')
    save(dst, y * 10 ** (gain / 20))
    out_i, out_tp = loudness(dst)
    if abs(out_i - lufs) > 1.5:
        print(f'voice_fx: WARNING {os.path.basename(dst)} at {out_i:.1f} LUFS (target {lufs}), peak-limited by {lufs - i - gain:.1f} dB')
    return out_i, out_tp

if __name__ == '__main__':
    a = sys.argv
    print(treat(a[1], a[2], '--cap' in a, float(a[a.index('--lufs') + 1]) if '--lufs' in a else LUFS, '--limit' in a))
