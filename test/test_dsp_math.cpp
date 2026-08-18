// Host unit tests for the framework-free DSP/units helpers.
// These deliberately do not touch ESPHome or FreeRTOS: see components/sound_level_meter/dsp_math.h

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

namespace {

/* DBFS_OFFSET */

TEST(DbfsOffset, MatchesTwentyLogSqrtTwo) { EXPECT_NEAR(DBFS_OFFSET, 20.0 * std::log10(std::sqrt(2.0)), 1e-6); }

TEST(DbfsOffset, FullScaleSineReadsZeroDbfs) {
  // rms of a full-scale sine is 1/sqrt(2), so its mean square is 0.5.
  const float db = adjust_dbfs(mean_square_to_dbfs(0.5), DbAdjustment{}, true);
  EXPECT_NEAR(db, 0.0f, 1e-5f);
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

TEST(DbConversion, SilenceIsNegativeInfinity) {
  EXPECT_TRUE(std::isinf(peak_to_dbfs(0.0f)));
  EXPECT_LT(peak_to_dbfs(0.0f), 0.0f);
}

/* adjust_dbfs */

TEST(AdjustDbfs, RmsAddsOffsetAndPeakDoesNot) {
  const DbAdjustment none{};
  EXPECT_NEAR(adjust_dbfs(-10.f, none, true), -10.f + DBFS_OFFSET, 1e-5f);
  EXPECT_NEAR(adjust_dbfs(-10.f, none, false), -10.f, 1e-5f);
}

TEST(AdjustDbfs, SensitivityAppliesOnlyWhenComplete) {
  DbAdjustment adjustment{};
  adjustment.sensitivity = -26.f;
  adjustment.sensitivity_ref = 94.f;

  // Both values present but not flagged: no correction.
  EXPECT_NEAR(adjust_dbfs(-40.f, adjustment, false), -40.f, 1e-5f);

  adjustment.has_sensitivity = true;
  EXPECT_NEAR(adjust_dbfs(-40.f, adjustment, false), -40.f + 94.f + 26.f, 1e-4f);
}

TEST(AdjustDbfs, OffsetIsAdditive) {
  DbAdjustment adjustment{};
  adjustment.offset = 2.5f;
  adjustment.has_offset = true;

  EXPECT_NEAR(adjust_dbfs(-40.f, adjustment, false), -37.5f, 1e-5f);
}

TEST(AdjustDbfs, CorrectionsCombine) {
  DbAdjustment adjustment{};
  adjustment.sensitivity = -26.f;
  adjustment.sensitivity_ref = 94.f;
  adjustment.has_sensitivity = true;
  adjustment.offset = -1.5f;
  adjustment.has_offset = true;

  const float expected = -40.f + DBFS_OFFSET + (94.f + 26.f) - 1.5f;
  EXPECT_NEAR(adjust_dbfs(-40.f, adjustment, true), expected, 1e-4f);
}

/* Accumulator width */

TEST(Accumulator, DoubleSurvivesAFullUpdateInterval) {
  // Regression guard for the Leq/min/max accumulators. Summing x^2 over one 60s interval
  // at 48kHz in float drifts by a few hundredths of a dB (and by several tenths over
  // longer intervals or at low levels), which is why sum_ is a double: the error is
  // signal-dependent and lands squarely in the resolution the sensors report.
  const uint32_t frames = ms_to_frames(48000, 60000);
  const float amplitude = 0.1f;
  const double square = static_cast<double>(amplitude) * amplitude;

  double accurate = 0.;
  float naive = 0.f;
  for (uint32_t i = 0; i < frames; i++) {
    accurate += square;
    naive += amplitude * amplitude;
  }

  const float accurate_db = mean_square_to_dbfs(accurate / frames);
  const float naive_db = mean_square_to_dbfs(static_cast<double>(naive) / frames);

  EXPECT_NEAR(accurate_db, -20.0f, 1e-4f);
  // Measured drift for this signal is ~0.088 dB; assert well clear of it in both
  // directions so the test states the problem without pinning an exact figure.
  EXPECT_GT(std::fabs(naive_db - accurate_db), 0.01f);
  EXPECT_LT(std::fabs(naive_db - accurate_db), 1.0f);
}

}  // namespace
