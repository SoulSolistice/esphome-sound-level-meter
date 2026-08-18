"""Generate and validate the octave-band filters used by the noise-exposure config.

Emits the `dsp_filters:` fragment for configs/exposure-example-config-ics43434.yaml.
The band set is 63 Hz .. 8 kHz, which is the octave-band set hearing-protector
attenuation data is published for (ISO 4869-2).

    python math/design_octave_bands.py            # print the YAML fragment
    python math/design_octave_bands.py --verify   # print the validation table
"""

import argparse

import numpy as np
from scipy import signal

SAMPLE_RATE = 48000.0
ORDER = 6  # 3 biquad sections per band
# Base-2 octave bands: nominal midband frequency -> exact midband frequency.
NOMINAL_BANDS = [63, 125, 250, 500, 1000, 2000, 4000, 8000]


def exact_midband(nominal: int) -> float:
    """Exact base-2 midband frequency for a nominal octave-band designation."""
    return 1000.0 * 2.0 ** round(np.log2(nominal / 1000.0))


def design(nominal: int) -> np.ndarray:
    """Butterworth band-pass spanning the band's -3 dB edges, as second-order sections."""
    fm = exact_midband(nominal)
    nyquist = SAMPLE_RATE / 2.0
    return signal.butter(
        ORDER // 2,
        [fm / np.sqrt(2.0) / nyquist, fm * np.sqrt(2.0) / nyquist],
        "bandpass",
        output="sos",
    )


def emit_yaml() -> str:
    lines = []
    for nominal in NOMINAL_BANDS:
        fm = exact_midband(nominal)
        lines.append(f"    - id: f_band_{nominal}          # {nominal} Hz octave band "
                     f"({fm / np.sqrt(2.0):.1f}-{fm * np.sqrt(2.0):.1f} Hz) @ 48kHz")
        lines.append("      type: sos")
        lines.append("      coeffs:")
        lines.append("        #          b0            b1            b2            a1            a2")
        for b0, b1, b2, _a0, a1, a2 in design(nominal):
            row = ", ".join(f"{v:13.9g}" for v in (b0, b1, b2, a1, a2))
            lines.append(f"        - [{row} ]")
    return "\n".join(lines)


def verify() -> None:
    print(f"Butterworth band-pass, order {ORDER} ({ORDER // 2} sections), fs = {SAMPLE_RATE:.0f} Hz\n")
    print(f"{'band':>6} {'exact fm':>10} {'-3dB edges':>19} {'max|pole|':>11} "
          f"{'stable':>7} {'@0.5fm':>8} {'@2fm':>8} {'@0.25fm':>9} {'gain@fm':>9}")
    ok = True
    for nominal in NOMINAL_BANDS:
        sos = design(nominal)
        fm = exact_midband(nominal)
        _z, poles, _k = signal.sos2zpk(sos)
        max_pole = float(np.max(np.abs(poles)))
        stable = max_pole < 1.0
        ok &= stable

        def att(multiplier: float, sos=sos, fm=fm) -> float:
            """Response at multiplier * fm, in dB."""
            freq = fm * multiplier
            if freq >= SAMPLE_RATE / 2:
                return float("nan")
            _w, h = signal.sosfreqz(sos, worN=[2 * np.pi * freq / SAMPLE_RATE])
            return float(20 * np.log10(abs(h[0])))

        print(f"{nominal:6d} {fm:10.2f} {fm / np.sqrt(2):8.1f}-{fm * np.sqrt(2):<10.1f} "
              f"{max_pole:11.7f} {str(stable):>7} {att(0.5):8.1f} {att(2.0):8.1f} "
              f"{att(0.25):9.1f} {att(1.0):9.3f}")
    print(f"\nall sections stable: {ok}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--verify", action="store_true", help="print the validation table")
    args = parser.parse_args()
    if args.verify:
        verify()
    else:
        print(emit_yaml())
