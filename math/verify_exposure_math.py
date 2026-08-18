"""Verify the on-device exposure arithmetic in the noise-exposure config.

Reproduces the lambdas from configs/noise-exposure-example-config-ics43434.yaml and
checks them against exposure profiles whose answers follow directly from the
definitions in ISO 9612 / IEC 61252 (3 dB energy rule) and OSHA 29 CFR 1910.95
(5 dB exchange rate).

    python math/verify_exposure_math.py
"""

import math

P_REF_SQUARED = 4.0e-10  # (20 uPa)^2, in Pa^2
EIGHT_HOURS_S = 28800.0
# Sound exposure equivalent to 85 dB(A) over 8 h - the EU upper action value.
DOSE_REFERENCE_PA2S = P_REF_SQUARED * 10.0 ** 8.5 * EIGHT_HOURS_S


def accumulate(profile):
    """profile: list of (LAeq in dB(A), duration in whole minutes)."""
    exposure_pa2s = 0.0
    exposure_seconds = 0.0
    osha_dose_pct = 0.0
    for laeq, minutes in profile:
        for _ in range(minutes):
            exposure_pa2s += P_REF_SQUARED * 10.0 ** (laeq / 10.0) * 60.0
            exposure_seconds += 60.0
            if laeq >= 80.0:
                allowed_hours = 8.0 / 2.0 ** ((laeq - 90.0) / 5.0)
                osha_dose_pct += (60.0 / 3600.0) / allowed_hours * 100.0
    return {
        "LEX": 10 * math.log10(exposure_pa2s / (EIGHT_HOURS_S * P_REF_SQUARED)) if exposure_pa2s else float("nan"),
        "LAeqTe": (10 * math.log10(exposure_pa2s / (exposure_seconds * P_REF_SQUARED))
                   if exposure_seconds else float("nan")),
        "EA_Pa2h": exposure_pa2s / 3600.0,
        "dose_eu": 100.0 * exposure_pa2s / DOSE_REFERENCE_PA2S,
        "dose_osha": osha_dose_pct,
    }


def osha_allowed_hours(level):
    return 8.0 / 2.0 ** ((level - 90.0) / 5.0)


CASES = [
    # A steady level for exactly 8 h must reproduce itself as L_EX,8h.
    ("85 dB(A) for 8 h", [(85, 480)],
     {"LEX": 85.0, "LAeqTe": 85.0, "dose_eu": 100.0, "EA_Pa2h": 1.0119,
      "dose_osha": 100 * 8 / osha_allowed_hours(85)}),
    ("80 dB(A) for 8 h", [(80, 480)],
     {"LEX": 80.0, "dose_eu": 31.623, "EA_Pa2h": 0.32,
      "dose_osha": 100 * 8 / osha_allowed_hours(80)}),
    ("87 dB(A) for 8 h (EU limit value)", [(87, 480)],
     {"LEX": 87.0, "dose_eu": 158.489, "dose_osha": 100 * 8 / osha_allowed_hours(87)}),
    ("90 dB(A) for 8 h (OSHA criterion)", [(90, 480)],
     {"LEX": 90.0, "dose_osha": 100.0}),
    # Halving the time costs exactly 10*log10(2) = 3.0103 dB, not the "3 dB" shorthand.
    ("88 dB(A) for 4 h", [(88, 240)],
     {"LEX": 88.0 - 10 * math.log10(2.0),
      "dose_eu": 100.0 * 10.0 ** ((88.0 - 10 * math.log10(2.0) - 85.0) / 10.0),
      "dose_osha": 100 * 4 / osha_allowed_hours(88)}),
    # ... and exactly 5 dB under the OSHA exchange rate.
    ("95 dB(A) for 4 h", [(95, 240)], {"dose_osha": 100.0}),
    # Levels below 80 dB(A) do not contribute to the OSHA dose.
    ("75 dB(A) for 8 h", [(75, 480)], {"LEX": 75.0, "dose_osha": 0.0}),
    # Energy addition across a mixed shift.
    ("4 h at 80 + 4 h at 90 dB(A)", [(80, 240), (90, 240)], {"LEX": 87.404}),
]


def main():
    print(f"{'case':36} {'quantity':10} {'computed':>10} {'expected':>10}  result")
    failures = 0
    for label, profile, expected in CASES:
        got = accumulate(profile)
        for key, want in expected.items():
            value = got[key]
            ok = abs(value - want) <= 0.01
            failures += not ok
            print(f"{label:36} {key:10} {value:10.3f} {want:10.3f}  {'ok' if ok else 'MISMATCH'}")
    print("\nall exposure quantities correct" if not failures else f"\n{failures} MISMATCHES")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
