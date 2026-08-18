// Host unit tests for the framework-free DSP/units helpers.
// These deliberately do not touch ESPHome or FreeRTOS: see components/sound_level_meter/dsp_math.h

#include <algorithm>
#include <cmath>
#include <vector>

#include <gtest/gtest.h>

#include "dsp_math.h"

using esphome::sound_level_meter::adjust_dbfs;
using esphome::sound_level_meter::DBFS_OFFSET;
using esphome::sound_level_meter::DbAdjustment;
using esphome::sound_level_meter::mean_square_to_dbfs;
using esphome::sound_level_meter::ms_to_frames;
using esphome::sound_level_meter::peak_to_dbfs;
using esphome::sound_level_meter::sos_process_section;
using esphome::sound_level_meter::SosCoeffs;
using esphome::sound_level_meter::SosState;
using esphome::sound_level_meter::TIME_WEIGHTING_FAST_MS;
using esphome::sound_level_meter::TIME_WEIGHTING_SETTLE_TAUS;
using esphome::sound_level_meter::TIME_WEIGHTING_SLOW_MS;
using esphome::sound_level_meter::time_weighting_alpha;
using esphome::sound_level_meter::TimeWeightingDetector;
using esphome::sound_level_meter::WindowedMeanSquare;

namespace {

/* DBFS_OFFSET */

TEST(DbfsOffset, MatchesTwentyLogSqrtTwo) { EXPECT_NEAR(DBFS_OFFSET, 20.0 * std::log10(std::sqrt(2.0)), 1e-6); }

TEST(DbfsOffset, FullScaleSineReadsZeroDbfs) {
  // rms of a full-scale sine is 1/sqrt(2), so its mean square is 0.5.
  EXPECT_NEAR(adjust_dbfs(mean_square_to_dbfs(0.5), DbAdjustment{}), 0.0f, 1e-5f);
}

TEST(DbfsOffset, PeakOfAFullScaleSineAlsoReadsZeroDbfs) {
  // The offset defines the dBFS reference, so it applies to peak readings too. A sine's
  // peak must land 3.01 dB above its own Leq, per IEC 61672-1.
  const float leq = adjust_dbfs(mean_square_to_dbfs(0.5), DbAdjustment{});
  const float peak = adjust_dbfs(peak_to_dbfs(1.0f), DbAdjustment{});
  EXPECT_NEAR(peak, 0.0f + DBFS_OFFSET, 1e-5f);
  EXPECT_NEAR(peak - leq, DBFS_OFFSET, 1e-5f);
}

TEST(DbfsOffset, CrestFactorIsReportedFaithfully) {
  // A square wave has a crest factor of 0 dB: its peak equals its RMS.
  const float leq = adjust_dbfs(mean_square_to_dbfs(1.0), DbAdjustment{});
  const float peak = adjust_dbfs(peak_to_dbfs(1.0f), DbAdjustment{});
  EXPECT_NEAR(peak - leq, 0.0f, 1e-5f);
}

TEST(DbfsOffset, CalibratorReadsBothLevelsCorrectly) {
  // 94 dB SPL 1 kHz sine into a mic rated -26 dBFS at 94 dB SPL.
  DbAdjustment cal{};
  cal.sensitivity = -26.f;
  cal.sensitivity_ref = 94.f;
  cal.has_sensitivity = true;
  const double rms = std::pow(10.0, (-26.0 - DBFS_OFFSET) / 20.0);
  EXPECT_NEAR(adjust_dbfs(mean_square_to_dbfs(rms * rms), cal), 94.0f, 1e-3f);
  EXPECT_NEAR(adjust_dbfs(peak_to_dbfs(static_cast<float>(rms * std::sqrt(2.0))), cal), 97.01f, 1e-2f);
}

/* ms_to_frames */

TEST(MsToFrames, ExactForCommonRates) {
  EXPECT_EQ(ms_to_frames(48000, 1000), 48000u);
  EXPECT_EQ(ms_to_frames(48000, 20), 960u);
  EXPECT_EQ(ms_to_frames(16000, 125), 2000u);
}

TEST(MsToFrames, TruncatesPartialFrames) {
  // 44100 * 125 / 1000 == 5512.5
  EXPECT_EQ(ms_to_frames(44100, 125), 5512u);
}

TEST(MsToFrames, ExactBeyondFloatPrecision) {
  // 60s at 48kHz is 2'880'000 frames; a float intermediate would start dropping frames
  // for longer intervals. Check a value well past 2^24.
  EXPECT_EQ(ms_to_frames(48000, 60000), 2880000u);
  EXPECT_EQ(ms_to_frames(48000, 3600000), 172800000u);
  EXPECT_EQ(ms_to_frames(44100, 3600001), 158760044u);
}

TEST(MsToFrames, ZeroDuration) { EXPECT_EQ(ms_to_frames(48000, 0), 0u); }

/* sos_process_section */

TEST(SosSection, PassthroughLeavesDataUnchanged) {
  const SosCoeffs coeffs{1.f, 0.f, 0.f, 0.f, 0.f};
  SosState state{0.f, 0.f};
  std::vector<float> data{0.25f, -0.5f, 0.75f, -1.f};
  const std::vector<float> expected = data;

  sos_process_section(coeffs, state, data.data(), data.size());

  EXPECT_EQ(data, expected);
}

TEST(SosSection, GainScalesEverySample) {
  const SosCoeffs coeffs{0.5f, 0.f, 0.f, 0.f, 0.f};
  SosState state{0.f, 0.f};
  std::vector<float> data{1.f, 2.f, 3.f};

  sos_process_section(coeffs, state, data.data(), data.size());

  EXPECT_FLOAT_EQ(data[0], 0.5f);
  EXPECT_FLOAT_EQ(data[1], 1.0f);
  EXPECT_FLOAT_EQ(data[2], 1.5f);
}

TEST(SosSection, ImpulseResponseMatchesDifferenceEquation) {
  // y[n] = b0*x[n] + b1*x[n-1] + b2*x[n-2] - a1*y[n-1] - a2*y[n-2]
  const SosCoeffs coeffs{0.2f, 0.3f, 0.1f, -0.5f, 0.25f};
  SosState state{0.f, 0.f};
  std::vector<float> data(6, 0.f);
  data[0] = 1.f;

  sos_process_section(coeffs, state, data.data(), data.size());

  std::vector<float> expected(data.size(), 0.f);
  for (size_t n = 0; n < expected.size(); n++) {
    const float x0 = (n == 0) ? 1.f : 0.f;
    const float x1 = (n == 1) ? 1.f : 0.f;
    const float x2 = (n == 2) ? 1.f : 0.f;
    const float y1 = (n >= 1) ? expected[n - 1] : 0.f;
    const float y2 = (n >= 2) ? expected[n - 2] : 0.f;
    expected[n] = coeffs.b0 * x0 + coeffs.b1 * x1 + coeffs.b2 * x2 - coeffs.a1 * y1 - coeffs.a2 * y2;
  }

  for (size_t n = 0; n < data.size(); n++)
    EXPECT_NEAR(data[n], expected[n], 1e-6f) << "at sample " << n;
}

TEST(SosSection, StateCarriesAcrossBlocks) {
  const SosCoeffs coeffs{0.2f, 0.3f, 0.1f, -0.5f, 0.25f};
  std::vector<float> whole(64);
  for (size_t i = 0; i < whole.size(); i++)
    whole[i] = std::sin(0.1f * i);
  std::vector<float> split = whole;

  SosState state_whole{0.f, 0.f};
  sos_process_section(coeffs, state_whole, whole.data(), whole.size());

  SosState state_split{0.f, 0.f};
  sos_process_section(coeffs, state_split, split.data(), 20);
  sos_process_section(coeffs, state_split, split.data() + 20, split.size() - 20);

  for (size_t i = 0; i < whole.size(); i++)
    EXPECT_NEAR(whole[i], split[i], 1e-6f) << "at sample " << i;
}

TEST(SosSection, DcGainMatchesTransferFunction) {
  const SosCoeffs coeffs{0.2f, 0.3f, 0.1f, -0.5f, 0.25f};
  SosState state{0.f, 0.f};
  std::vector<float> data(4096, 1.f);

  sos_process_section(coeffs, state, data.data(), data.size());

  const float expected = (coeffs.b0 + coeffs.b1 + coeffs.b2) / (1.f + coeffs.a1 + coeffs.a2);
  EXPECT_NEAR(data.back(), expected, 1e-5f);
}

TEST(SosSection, EmptyBlockIsANoOp) {
  const SosCoeffs coeffs{0.2f, 0.3f, 0.1f, -0.5f, 0.25f};
  SosState state{1.f, 2.f};

  sos_process_section(coeffs, state, nullptr, 0);

  EXPECT_FLOAT_EQ(state.s0, 1.f);
  EXPECT_FLOAT_EQ(state.s1, 2.f);
}

/* dB conversions */

TEST(DbConversion, MeanSquareReferencePoints) {
  EXPECT_NEAR(mean_square_to_dbfs(1.0), 0.0f, 1e-6f);
  EXPECT_NEAR(mean_square_to_dbfs(0.5), -3.0102999f, 1e-5f);
  EXPECT_NEAR(mean_square_to_dbfs(0.01), -20.0f, 1e-5f);
}

TEST(DbConversion, PeakReferencePoints) {
  EXPECT_NEAR(peak_to_dbfs(1.0f), 0.0f, 1e-6f);
  EXPECT_NEAR(peak_to_dbfs(0.5f), -6.0205999f, 1e-5f);
  EXPECT_NEAR(peak_to_dbfs(0.1f), -20.0f, 1e-5f);
}

TEST(DbConversion, DigitalSilenceIsNanNotNegativeInfinity) {
  // -inf would be published verbatim as a sensor state.
  EXPECT_TRUE(std::isnan(peak_to_dbfs(0.0f)));
  EXPECT_TRUE(std::isnan(mean_square_to_dbfs(0.0)));
  EXPECT_TRUE(std::isnan(peak_to_dbfs(-1.0f)));
  EXPECT_TRUE(std::isnan(mean_square_to_dbfs(-1.0)));
  EXPECT_TRUE(std::isnan(adjust_dbfs(mean_square_to_dbfs(0.0), DbAdjustment{})));
}

/* adjust_dbfs */

TEST(AdjustDbfs, OffsetAlwaysApplies) { EXPECT_NEAR(adjust_dbfs(-10.f, DbAdjustment{}), -10.f + DBFS_OFFSET, 1e-5f); }

TEST(AdjustDbfs, SensitivityAppliesOnlyWhenComplete) {
  DbAdjustment adjustment{};
  adjustment.sensitivity = -26.f;
  adjustment.sensitivity_ref = 94.f;

  // Both values present but not flagged: no correction.
  EXPECT_NEAR(adjust_dbfs(-40.f, adjustment), -40.f + DBFS_OFFSET, 1e-5f);

  adjustment.has_sensitivity = true;
  EXPECT_NEAR(adjust_dbfs(-40.f, adjustment), -40.f + DBFS_OFFSET + 94.f + 26.f, 1e-4f);
}

TEST(AdjustDbfs, OffsetIsAdditive) {
  DbAdjustment adjustment{};
  adjustment.offset = 2.5f;
  adjustment.has_offset = true;

  EXPECT_NEAR(adjust_dbfs(-40.f, adjustment), -37.5f + DBFS_OFFSET, 1e-5f);
}

TEST(AdjustDbfs, CorrectionsCombine) {
  DbAdjustment adjustment{};
  adjustment.sensitivity = -26.f;
  adjustment.sensitivity_ref = 94.f;
  adjustment.has_sensitivity = true;
  adjustment.offset = -1.5f;
  adjustment.has_offset = true;

  const float expected = -40.f + DBFS_OFFSET + (94.f + 26.f) - 1.5f;
  EXPECT_NEAR(adjust_dbfs(-40.f, adjustment), expected, 1e-4f);
}

/* Accumulator width */

TEST(Accumulator, MatchesFullDoubleAccumulationOverALongInterval) {
  // The accumulator sums in float and folds into a double every FOLD_FRAMES samples, so
  // that the hot loop stays on the single-precision FPU. It must still track a full double
  // accumulation: plain float drifts by ~0.01 dB over a 60 s interval, and much further on
  // a constant signal, where every addend is eventually swallowed by the running total.
  const uint32_t sample_rate = 48000;
  WindowedMeanSquare acc;
  acc.configure(sample_rate, 60000);
  ASSERT_TRUE(acc.enabled());
  ASSERT_EQ(acc.window_samples(), 2880000u);

  const float amplitude = 0.1f;
  double reference = 0.0;
  float naive = 0.f;
  float got = NAN;
  bool fired = false;
  for (uint32_t i = 0; i < acc.window_samples(); i++) {
    reference += static_cast<double>(amplitude) * amplitude;
    naive += amplitude * amplitude;
    fired = acc.process(amplitude, got);
  }
  ASSERT_TRUE(fired);

  const float reference_db = mean_square_to_dbfs(reference / acc.window_samples());
  const float naive_db = mean_square_to_dbfs(static_cast<double>(naive) / acc.window_samples());
  EXPECT_NEAR(mean_square_to_dbfs(got), reference_db, 1e-4f);
  EXPECT_GT(std::fabs(naive_db - reference_db), 0.01f);
}

TEST(Accumulator, FiresExactlyOncePerWindow) {
  WindowedMeanSquare acc;
  acc.configure(48000, 10);  // 480 frames
  ASSERT_EQ(acc.window_samples(), 480u);
  int fires = 0;
  float ms = NAN;
  for (int i = 0; i < 480 * 3; i++) {
    if (acc.process(0.5f, ms)) {
      fires++;
      EXPECT_NEAR(ms, 0.25f, 1e-6f);
    }
  }
  EXPECT_EQ(fires, 3);
}

TEST(Accumulator, RestartDiscardsThePartialWindow) {
  // Guards the interval-boundary leak: a part-finished loud window must not survive into
  // the next update interval and dominate its extremum.
  WindowedMeanSquare acc;
  acc.configure(48000, 1000);  // 48000 frames
  float ms = NAN;
  for (int i = 0; i < 24000; i++)
    acc.process(0.5f, ms);  // half a window of loud audio
  acc.restart();
  bool fired = false;
  for (int i = 0; i < 48000; i++)
    fired = acc.process(0.001f, ms);
  ASSERT_TRUE(fired);
  EXPECT_NEAR(mean_square_to_dbfs(ms), mean_square_to_dbfs(1e-6), 1e-3f);  // no loud residue
}

TEST(Accumulator, DisabledWhenWindowIsZero) {
  WindowedMeanSquare acc;
  acc.configure(48000, 0);
  EXPECT_FALSE(acc.enabled());
  float ms = NAN;
  for (int i = 0; i < 1000; i++)
    EXPECT_FALSE(acc.process(0.5f, ms));
}

/* IEC 61672-1 exponential time weighting */

TEST(TimeWeighting, AlphaMatchesTheContinuousTimeConstant) {
  const uint32_t fs = 48000;
  for (uint32_t tau_ms : {TIME_WEIGHTING_FAST_MS, TIME_WEIGHTING_SLOW_MS}) {
    const double tau_samples = static_cast<double>(fs) * tau_ms / 1000.0;
    // alpha is a float in the 1e-4 range, so compare at float resolution.
    EXPECT_NEAR(time_weighting_alpha(fs, tau_ms), 1.0 - std::exp(-1.0 / tau_samples), 1e-10);
  }
  EXPECT_FLOAT_EQ(time_weighting_alpha(48000, 0), 1.f);
  EXPECT_FLOAT_EQ(time_weighting_alpha(0, 125), 1.f);
}

TEST(TimeWeighting, StepResponseReachesOneTimeConstantAt63Percent) {
  const uint32_t fs = 48000;
  TimeWeightingDetector det;
  det.configure(fs, TIME_WEIGHTING_FAST_MS);
  det.restart();
  // Prime on silence so the step starts from zero, then drive a unit-mean-square signal.
  float ms = NAN;
  for (uint32_t i = 0; i < det.settle_samples(); i++)
    det.process(0.f, ms);
  const uint32_t tau = fs * TIME_WEIGHTING_FAST_MS / 1000;
  for (uint32_t i = 0; i < tau; i++)
    det.process(1.f, ms);
  EXPECT_NEAR(ms, 1.0 - std::exp(-1.0), 1e-3);
}

TEST(TimeWeighting, DetectorWithholdsOutputUntilSettled) {
  TimeWeightingDetector det;
  det.configure(48000, TIME_WEIGHTING_FAST_MS);
  det.restart();
  EXPECT_EQ(det.settle_samples(), TIME_WEIGHTING_SETTLE_TAUS * 6000u);
  float ms = NAN;
  for (uint32_t i = 0; i < det.settle_samples(); i++)
    EXPECT_FALSE(det.process(0.5f, ms)) << "at sample " << i;
  EXPECT_TRUE(det.process(0.5f, ms));
}

TEST(TimeWeighting, PrimingPreventsASpuriousZeroMinimum) {
  // Starting the detector at zero would make an Lmin sensor latch 0 (i.e. -inf dB) on the
  // first sample. Priming from the first sample plus the settle guard must prevent it.
  TimeWeightingDetector det;
  det.configure(48000, TIME_WEIGHTING_FAST_MS);
  det.restart();
  float ms = NAN;
  float lowest = 1e30f;
  for (uint32_t i = 0; i < 48000 * 3; i++) {
    if (det.process(0.2f, ms))
      lowest = std::min(lowest, ms);
  }
  EXPECT_NEAR(lowest, 0.04f, 1e-3f);
  EXPECT_GT(lowest, 0.f);
}

TEST(TimeWeighting, SteadySignalConvergesToItsMeanSquare) {
  const uint32_t fs = 48000;
  TimeWeightingDetector det;
  det.configure(fs, TIME_WEIGHTING_SLOW_MS);
  det.restart();
  float ms = NAN;
  // 1 kHz full-scale sine: mean square 0.5
  for (uint32_t i = 0; i < fs * 20; i++)
    det.process(std::sin(2.f * static_cast<float>(M_PI) * 1000.f * i / fs), ms);
  EXPECT_NEAR(mean_square_to_dbfs(ms), mean_square_to_dbfs(0.5), 0.05f);
}

TEST(TimeWeighting, FastRespondsToAShortBurstAndSlowDoesNot) {
  // The whole point of the option: a 20 ms burst is fully captured by Fast but heavily
  // averaged away by Slow.
  const uint32_t fs = 48000;
  auto peak_of = [&](uint32_t tau_ms) {
    TimeWeightingDetector det;
    det.configure(fs, tau_ms);
    det.restart();
    float ms = NAN, top = 0.f;
    for (uint32_t i = 0; i < fs * 12; i++) {
      const bool in_burst = i >= fs * 10 && i < fs * 10 + fs / 50;  // 20 ms
      if (det.process(in_burst ? 0.5f : 0.005f, ms))
        top = std::max(top, ms);
    }
    return mean_square_to_dbfs(top);
  };
  const float fast = peak_of(TIME_WEIGHTING_FAST_MS);
  const float slow = peak_of(TIME_WEIGHTING_SLOW_MS);
  EXPECT_GT(fast, slow + 5.f);
  EXPECT_LT(fast, mean_square_to_dbfs(0.25));  // never exceeds the burst's own level
}

/* End-to-end guards for the extremum sensor's interval loop.
   These mirror SoundLevelMeterSensorExtremum::process() using the real production
   classes, which is where the two measurement bugs lived. */

namespace {

struct ExtremumModel {
  WindowedMeanSquare window;
  TimeWeightingDetector detector;
  float extreme{0.f};
  uint32_t count_update{0};
  uint32_t update_samples{0};
  bool has_value{false};
  bool find_max{true};
  bool drop_partial_window{true};  // set false to reproduce the pre-fix behaviour

  void track(float ms) {
    if (!has_value || (find_max ? ms > extreme : ms < extreme)) {
      extreme = ms;
      has_value = true;
    }
  }

  /// Returns one published mean square per completed update interval (NAN if none seen).
  std::vector<float> run(const std::vector<float> &samples) {
    std::vector<float> published;
    float ms = NAN;
    for (float x : samples) {
      if (detector.enabled()) {
        if (detector.process(x, ms))
          track(ms);
      } else if (window.process(x, ms)) {
        track(ms);
      }
      if (++count_update < update_samples)
        continue;
      published.push_back(has_value ? extreme : NAN);
      count_update = 0;
      extreme = 0.f;
      has_value = false;
      if (drop_partial_window)
        window.restart();
    }
    return published;
  }
};

std::vector<float> alternating_loud_quiet(uint32_t sample_rate, int intervals, float loud, float quiet,
                                          double interval_s) {
  std::vector<float> out;
  const auto n = static_cast<size_t>(sample_rate * interval_s);
  for (int i = 0; i < intervals; i++)
    out.insert(out.end(), n, (i % 2 == 0) ? loud : quiet);
  return out;
}

}  // namespace

TEST(ExtremumSensor, PartialWindowDoesNotLeakIntoTheNextInterval) {
  // update_interval 10 s with window_size 700 ms does not divide evenly. Before the fix the
  // loud tail of one interval carried into the next interval's first window and dominated
  // its maximum, reporting the quiet interval tens of dB too high.
  const uint32_t fs = 48000;
  const auto samples = alternating_loud_quiet(fs, 4, 0.5f, 0.001f, 10.0);

  ExtremumModel fixed;
  fixed.window.configure(fs, 700);
  fixed.update_samples = fs * 10;
  const auto got = fixed.run(samples);

  ExtremumModel leaky;
  leaky.window.configure(fs, 700);
  leaky.update_samples = fs * 10;
  leaky.drop_partial_window = false;
  const auto before = leaky.run(samples);

  ASSERT_EQ(got.size(), 4u);
  ASSERT_EQ(before.size(), 4u);
  const float loud_db = mean_square_to_dbfs(0.25);
  const float quiet_db = mean_square_to_dbfs(1e-6);

  for (size_t i = 0; i < got.size(); i++) {
    const float expected = (i % 2 == 0) ? loud_db : quiet_db;
    EXPECT_NEAR(mean_square_to_dbfs(got[i]), expected, 0.01f) << "interval " << i;
  }
  // And confirm the guard is actually guarding something: the old behaviour was far off.
  EXPECT_GT(mean_square_to_dbfs(before[1]) - quiet_db, 10.f);
}

TEST(ExtremumSensor, ExponentialDetectorMatchesAReferenceLafmax) {
  // Compare the production detector against a straightforward float64 implementation of
  // the IEC 61672-1 single-pole detector over the same signal.
  const uint32_t fs = 48000;
  std::vector<float> sig(fs * 8, 0.01f);
  for (uint32_t i = fs * 4; i < fs * 4 + fs / 50; i++)
    sig[i] = 0.5f;  // 20 ms burst

  ExtremumModel model;
  model.detector.configure(fs, TIME_WEIGHTING_FAST_MS);
  model.detector.restart();
  model.update_samples = static_cast<uint32_t>(sig.size());
  const auto got = model.run(sig);
  ASSERT_EQ(got.size(), 1u);

  const double alpha = 1.0 - std::exp(-1.0 / (fs * TIME_WEIGHTING_FAST_MS / 1000.0));
  double state = static_cast<double>(sig[0]) * sig[0];
  double reference = 0.0;
  for (size_t i = 1; i < sig.size(); i++) {
    state += alpha * (static_cast<double>(sig[i]) * sig[i] - state);
    if (i >= model.detector.settle_samples())
      reference = std::max(reference, state);
  }
  EXPECT_NEAR(mean_square_to_dbfs(got[0]), mean_square_to_dbfs(reference), 0.02f);
}

TEST(ExtremumSensor, SlowAveragesAwayWhatFastCaptures) {
  const uint32_t fs = 48000;
  std::vector<float> sig(fs * 12, 0.005f);
  for (uint32_t i = fs * 6; i < fs * 6 + fs / 50; i++)
    sig[i] = 0.5f;

  auto peak_for = [&](uint32_t tau_ms) {
    ExtremumModel m;
    m.detector.configure(fs, tau_ms);
    m.detector.restart();
    m.update_samples = static_cast<uint32_t>(sig.size());
    return mean_square_to_dbfs(m.run(sig)[0]);
  };
  EXPECT_GT(peak_for(TIME_WEIGHTING_FAST_MS), peak_for(TIME_WEIGHTING_SLOW_MS) + 5.f);
}

TEST(ExtremumSensor, MinNeverLatchesSilenceFromAColdDetector) {
  // An un-primed detector starts at zero, which an Lmin sensor would latch as -inf dB.
  const uint32_t fs = 48000;
  ExtremumModel m;
  m.find_max = false;
  m.detector.configure(fs, TIME_WEIGHTING_FAST_MS);
  m.detector.restart();
  m.update_samples = fs * 2;
  const auto got = m.run(std::vector<float>(fs * 4, 0.2f));
  ASSERT_EQ(got.size(), 2u);
  for (float ms : got) {
    EXPECT_FALSE(std::isnan(mean_square_to_dbfs(ms)));
    EXPECT_NEAR(mean_square_to_dbfs(ms), mean_square_to_dbfs(0.04), 0.05f);
  }
}

}  // namespace
