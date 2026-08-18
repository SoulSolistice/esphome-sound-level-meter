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

/// The component's dBFS reference: a full-scale sine reads 0 dBFS. Its RMS is 1/sqrt(2), so
/// every linear amplitude ratio - RMS *and* peak alike - needs this offset to land on that
/// reference. Spelled as a literal because std::log10 is not constexpr.
/// see: https://dsp.stackexchange.com/a/50947/65262
constexpr float DBFS_OFFSET = 3.010299956639812f;

/// Exponential time weightings defined by IEC 61672-1.
constexpr uint32_t TIME_WEIGHTING_FAST_MS = 125;
constexpr uint32_t TIME_WEIGHTING_SLOW_MS = 1000;

/// How many time constants the exponential detector is allowed to settle before its output
/// is trusted. After 5 tau the step response is within 0.7 % of its final value.
constexpr uint32_t TIME_WEIGHTING_SETTLE_TAUS = 5;

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

/// Mean square (a linear power ratio) to dBFS. Returns NAN for digital silence rather than
/// -inf, which would otherwise be published as a sensor state.
inline float mean_square_to_dbfs(double mean_square) {
  if (!(mean_square > 0.0))
    return NAN;
  return static_cast<float>(10.0 * std::log10(mean_square));
}

/// Absolute peak amplitude (a linear amplitude ratio) to dBFS. NAN for digital silence.
inline float peak_to_dbfs(float peak) {
  if (!(peak > 0.f))
    return NAN;
  return static_cast<float>(20.0 * std::log10(static_cast<double>(peak)));
}

/// Turn a raw dBFS reading into the published value. DBFS_OFFSET is part of the dBFS
/// reference itself, so it applies to peak readings exactly as it does to RMS ones.
inline float adjust_dbfs(float db, const DbAdjustment &adjustment) {
  double result = static_cast<double>(db) + DBFS_OFFSET;
  if (adjustment.has_sensitivity)
    result += static_cast<double>(adjustment.sensitivity_ref) - adjustment.sensitivity;
  if (adjustment.has_offset)
    result += adjustment.offset;
  return static_cast<float>(result);
}

/// Smoothing coefficient of the single-pole detector realising IEC 61672-1 exponential
/// time weighting: y[n] = y[n-1] + alpha * (x[n]^2 - y[n-1]).
inline float time_weighting_alpha(uint32_t sample_rate, uint32_t time_constant_ms) {
  if (sample_rate == 0 || time_constant_ms == 0)
    return 1.f;
  const double tau_samples = static_cast<double>(sample_rate) * time_constant_ms / 1000.0;
  return static_cast<float>(1.0 - std::exp(-1.0 / tau_samples));
}

/// Mean-square accumulator over a fixed number of frames.
///
/// The running total is a double so that a long interval does not lose the quiet samples,
/// but samples are summed in float and folded into it only every FOLD_FRAMES samples. The
/// target has a single-precision FPU, so a float multiply-add is one instruction while a
/// double one costs three soft-float calls; folding keeps each float run short enough that
/// the result is bit-comparable to accumulating entirely in double.
class WindowedMeanSquare {
 public:
  static constexpr uint32_t FOLD_FRAMES = 1024;

  void configure(uint32_t sample_rate, uint32_t window_ms) {
    this->window_samples_ = ms_to_frames(sample_rate, window_ms);
    this->restart();
  }

  bool enabled() const { return this->window_samples_ > 0; }
  uint32_t window_samples() const { return this->window_samples_; }

  /// Discard whatever has been accumulated so far and start a fresh window.
  void restart() {
    this->total_ = 0.;
    this->partial_ = 0.f;
    this->count_ = 0;
    this->fold_count_ = 0;
  }

  /// Feed one sample. Returns true and writes the window's mean square when it completes.
  bool process(float sample, float &mean_square) {
    if (this->window_samples_ == 0)
      return false;
    this->partial_ += sample * sample;
    this->count_++;
    if (++this->fold_count_ >= FOLD_FRAMES) {
      this->total_ += this->partial_;
      this->partial_ = 0.f;
      this->fold_count_ = 0;
    }
    if (this->count_ < this->window_samples_)
      return false;
    mean_square = static_cast<float>((this->total_ + this->partial_) / this->count_);
    this->restart();
    return true;
  }

 protected:
  double total_{0.};
  float partial_{0.f};
  uint32_t count_{0};
  uint32_t fold_count_{0};
  uint32_t window_samples_{0};
};

/// Single-pole detector implementing IEC 61672-1 exponential time weighting (F / S).
///
/// Primed from the first sample rather than from zero, and it withholds its output until it
/// has settled, so a cold start cannot report a spurious minimum of zero.
class TimeWeightingDetector {
 public:
  void configure(uint32_t sample_rate, uint32_t time_constant_ms) {
    this->time_constant_ms_ = time_constant_ms;
    this->alpha_ = time_weighting_alpha(sample_rate, time_constant_ms);
    this->settle_samples_ = TIME_WEIGHTING_SETTLE_TAUS * ms_to_frames(sample_rate, time_constant_ms);
    this->restart();
  }

  bool enabled() const { return this->time_constant_ms_ > 0; }
  uint32_t settle_samples() const { return this->settle_samples_; }

  void restart() {
    this->state_ = 0.f;
    this->primed_ = false;
    this->settle_remaining_ = this->settle_samples_;
  }

  /// Feed one sample. Returns true and writes the time-weighted mean square once settled.
  bool process(float sample, float &mean_square) {
    const float square = sample * sample;
    if (this->primed_) {
      this->state_ += this->alpha_ * (square - this->state_);
    } else {
      this->state_ = square;
      this->primed_ = true;
    }
    if (this->settle_remaining_ > 0) {
      this->settle_remaining_--;
      return false;
    }
    mean_square = this->state_;
    return true;
  }

 protected:
  float alpha_{1.f};
  float state_{0.f};
  uint32_t settle_samples_{0};
  uint32_t settle_remaining_{0};
  uint32_t time_constant_ms_{0};
  bool primed_{false};
};

}  // namespace esphome::sound_level_meter
