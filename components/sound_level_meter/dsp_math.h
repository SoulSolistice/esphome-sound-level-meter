#pragma once

// Pure DSP/units logic for the sound level meter.
//
// This header deliberately has no ESPHome, ESP-IDF or FreeRTOS includes so that it can be
// compiled and unit-tested on the host:
//   g++ -std=c++17 -Wall -Wextra -Werror
// See test/test_dsp_math.cpp.

#include <cmath>
#include <cstddef>
#include <cstdint>

namespace esphome::sound_level_meter {

/// Coefficients of one second-order section (biquad), normalised so that a0 == 1.
/// Layout matches what esp-dsp's dsps_biquad_f32_* expects: {b0, b1, b2, a1, a2}.
struct SosCoeffs {
  float b0;
  float b1;
  float b2;
  float a1;
  float a2;
};

/// Delay line of one second-order section (transposed direct form II).
struct SosState {
  float s0;
  float s1;
};

/// Gain corrections applied to a raw dBFS reading to turn it into a dB SPL estimate.
struct DbAdjustment {
  float sensitivity{0.f};
  float sensitivity_ref{0.f};
  float offset{0.f};
  bool has_sensitivity{false};
  bool has_offset{false};
};

/// By definition the dBFS value of a full-scale sine wave equals 0. The RMS of a full-scale
/// sine is 1/sqrt(2), so adding this offset makes the formula evaluate to 0 for such a signal.
/// Spelled as a literal rather than 20*log10(sqrt(2)) because std::log10 is not constexpr.
/// see: https://dsp.stackexchange.com/a/50947/65262
constexpr float DBFS_OFFSET = 3.010299956639812f;

/// Number of audio frames covered by a duration, computed in 64-bit so that long intervals
/// at high sample rates stay exact (float loses whole frames above 2^24).
inline uint32_t ms_to_frames(uint32_t sample_rate, uint32_t ms) {
  return static_cast<uint32_t>((static_cast<uint64_t>(sample_rate) * ms) / 1000ULL);
}

/// Apply one second-order section to a block of samples, in place.
/// Portable reference implementation; esp-dsp provides accelerated equivalents.
inline void sos_process_section(const SosCoeffs &coeffs, SosState &state, float *data, size_t len) {
  for (size_t i = 0; i < len; i++) {
    const float xi = data[i];
    const float yi = coeffs.b0 * xi + state.s0;
    state.s0 = coeffs.b1 * xi - coeffs.a1 * yi + state.s1;
    state.s1 = coeffs.b2 * xi - coeffs.a2 * yi;
    data[i] = yi;
  }
}

/// Mean square (a linear power ratio) to dBFS. Accumulate in double; narrow only here.
inline float mean_square_to_dbfs(double mean_square) { return static_cast<float>(10.0 * std::log10(mean_square)); }

/// Absolute peak amplitude (a linear amplitude ratio) to dBFS.
inline float peak_to_dbfs(float peak) { return static_cast<float>(20.0 * std::log10(static_cast<double>(peak))); }

/// Turn a raw dBFS reading into the published value.
/// @param is_rms true for RMS-derived readings (Leq/min/max), false for peak amplitude.
inline float adjust_dbfs(float db, const DbAdjustment &adjustment, bool is_rms) {
  double result = db;
  if (is_rms)
    result += DBFS_OFFSET;
  if (adjustment.has_sensitivity)
    result += static_cast<double>(adjustment.sensitivity_ref) - adjustment.sensitivity;
  if (adjustment.has_offset)
    result += adjustment.offset;
  return static_cast<float>(result);
}

}  // namespace esphome::sound_level_meter
