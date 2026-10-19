"""Reference model of the channel filter's state-variable sections.

One section is the trapezoidal-integrator (topology-preserving) state-variable
filter. With g = tan(pi * f / fs) and damping k = 1 / Q:

    a1 = 1 / (1 + g * (g + k)),  a2 = g * a1,  a3 = g * a2
    v3 = x - ic2
    v1 = a1 * ic1 + a2 * v3          bandpass
    v2 = ic2 + a2 * ic1 + a3 * v3    lowpass
    ic1 = 2 * v1 - ic1,  ic2 = 2 * v2 - ic2
    hp = x - k * v1 - v2             highpass

It stays stable at any cutoff below Nyquist and while the cutoff moves every
sample. `fixed_section` follows the renderer's arithmetic: Q31 values
combined with the NEON saturating rounding doubling multiply-high
(vqrdmulh.s32) and saturating add/subtract, so a loud resonance clips instead
of wrapping. The input keeps INPUT_GUARD_BITS of headroom for the resonant
peak, as the EQ keeps three.

Run `python3 tools/svf_model.py` to print the checks that sized the format.
"""
from __future__ import annotations

import math

import numpy as np

RATE = 48000
Q31 = 1 << 31
INT_MIN, INT_MAX = -(1 << 31), (1 << 31) - 1
INPUT_GUARD_BITS = 5  # 30 dB of resonant peak before the states clip
CUTOFF_MIN, CUTOFF_MAX = 20.0, 20000.0
DAMPING_MAX = 2.0  # Q = 0.5, the least resonant setting


def coefficients(cutoff: float, damping: float) -> tuple[float, float, float, float]:
    """a1, a2, a3 and k for one section."""
    g = math.tan(math.pi * min(cutoff, CUTOFF_MAX) / RATE)
    a1 = 1.0 / (1.0 + g * (g + damping))
    a2 = g * a1
    return a1, a2, g * a2, damping


def float_section(x: np.ndarray, cutoff: float, damping: float):
    """Lowpass, bandpass and highpass outputs for the whole signal."""
    a1, a2, a3, k = coefficients(cutoff, damping)
    ic1 = ic2 = 0.0
    lp = np.empty_like(x)
    bp = np.empty_like(x)
    hp = np.empty_like(x)
    for n, sample in enumerate(x):
        v3 = sample - ic2
        v1 = a1 * ic1 + a2 * v3
        v2 = ic2 + a2 * ic1 + a3 * v3
        ic1 = 2 * v1 - ic1
        ic2 = 2 * v2 - ic2
        lp[n], bp[n], hp[n] = v2, v1, sample - k * v1 - v2
    return lp, bp, hp


def sat(value: int) -> int:
    return INT_MAX if value > INT_MAX else INT_MIN if value < INT_MIN else value


def qrdmulh(a: int, b: int) -> int:
    """vqrdmulh.s32: round(2 * a * b / 2^32), saturating."""
    return sat((2 * a * b + (1 << 31)) >> 32)


def q31(value: float) -> int:
    return sat(int(round(value * Q31)))


def fixed_coefficients(cutoff: float, damping: float):
    """Q31 a1, a2, a3 and k/4 (k reaches 4, which Q31 cannot hold)."""
    a1, a2, a3, k = coefficients(cutoff, damping)
    return q31(a1), q31(a2), q31(a3), q31(k / 4)


def fixed_section(x: np.ndarray, cutoff: float, damping: float):
    """The same section in the renderer's arithmetic; x is signed Q31."""
    a1, a2, a3, quarter_k = fixed_coefficients(cutoff, damping)
    ic1 = ic2 = 0
    lp = np.empty(len(x), dtype=np.int64)
    bp = np.empty(len(x), dtype=np.int64)
    hp = np.empty(len(x), dtype=np.int64)
    for n, sample in enumerate(x):
        sample = int(sample)
        v3 = sat(sample - ic2)
        v1 = sat(qrdmulh(a1, ic1) + qrdmulh(a2, v3))
        v2 = sat(sat(ic2 + qrdmulh(a2, ic1)) + qrdmulh(a3, v3))
        ic1 = sat(sat(2 * v1) - ic1)
        ic2 = sat(sat(2 * v2) - ic2)
        kv1 = sat(4 * qrdmulh(quarter_k, v1))
        lp[n], bp[n], hp[n] = v2, v1, sat(sat(sample - kv1) - v2)
    return lp, bp, hp


def response(cutoff: float, damping: float, frequencies, fixed: bool = False):
    """Gain of the lowpass, bandpass and highpass taps at each frequency,
    measured with a sine long enough to settle."""
    out = []
    for frequency in frequencies:
        n = np.arange(int(RATE * 0.4))
        tone = 0.1 * np.sin(2 * np.pi * frequency * n / RATE)
        if fixed:
            taps = fixed_section(np.round(tone * (Q31 >> INPUT_GUARD_BITS)).astype(np.int64), cutoff, damping)
            taps = [t / (Q31 >> INPUT_GUARD_BITS) for t in taps]
        else:
            taps = float_section(tone, cutoff, damping)
        tail = slice(len(n) // 2, None)
        out.append([np.sqrt(2 * np.mean(t[tail] ** 2)) / 0.1 for t in taps])
    return np.array(out)


def db(x):
    return 20 * np.log10(np.maximum(x, 1e-12))


def main():
    print("Fixed point against float: lowpass/bandpass/highpass gain error in dB")
    worst = 0.0
    for cutoff in (20, 100, 1000, 8000, 18000):
        for damping in (DAMPING_MAX, math.sqrt(2), 0.5, 0.1, 0.04):
            freqs = [cutoff / 4, cutoff, min(cutoff * 4, 20000)]
            a = response(cutoff, damping, freqs)
            b = response(cutoff, damping, freqs, fixed=True)
            err = np.max(np.abs(db(a) - db(b)))
            worst = max(worst, err)
            print(f"  fc {cutoff:6.0f} Hz  k {damping:5.2f}  max error {err:6.3f} dB")
    print(f"worst response error {worst:.3f} dB")

    print("Idle (zero input) after a full-scale burst: largest state left, in LSB")
    for cutoff, damping in ((20, 0.04), (200, 0.04), (5000, 0.04), (18000, 0.04)):
        burst = np.zeros(4800, dtype=np.int64)
        burst[:64] = Q31 >> INPUT_GUARD_BITS
        quiet = np.concatenate([burst, np.zeros(RATE * 2, dtype=np.int64)])
        lp, bp, hp = fixed_section(quiet, cutoff, damping)
        print(f"  fc {cutoff:6.0f} Hz  k {damping:4.2f}  last-second peak {max(np.abs(lp[-RATE:]).max(), np.abs(bp[-RATE:]).max())}")

    print("Full-scale sine at resonance: peak output as a fraction of full scale")
    for damping in (math.sqrt(2), 0.2, 0.05, 0.02):
        n = np.arange(RATE // 2)
        tone = np.round(np.sin(2 * np.pi * 1000 * n / RATE) * (Q31 >> INPUT_GUARD_BITS)).astype(np.int64)
        lp, bp, hp = fixed_section(tone, 1000, damping)
        print(f"  k {damping:5.2f} (Q {1 / damping:5.1f})  lowpass peak {np.abs(lp).max() / Q31:.3f}  bandpass peak {np.abs(bp).max() / Q31:.3f}")


if __name__ == "__main__":
    main()
