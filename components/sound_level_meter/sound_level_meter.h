#pragma once

#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <cmath>
#include <memory>
#include <mutex>

#include "esp_timer.h"

#include "dsp_math.h"

#include "esphome/core/application.h"
#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"
#include "esphome/components/microphone/microphone_source.h"
#include "esphome/components/ring_buffer/ring_buffer.h"

#ifdef USE_SENSOR
#include "esphome/components/sensor/sensor.h"
#endif

#ifdef USE_OTA_STATE_LISTENER
#include "esphome/components/ota/ota_backend.h"
#endif

namespace esphome::sound_level_meter {

class Filter;

/// A stack of equally sized audio buffers holding the intermediate results of the DSP filter
/// chains. Sensors sharing a filter prefix share the buffer computed for that prefix.
/// All storage is one allocation made by init(); the stack never grows afterwards.
class BufferStack {
 public:
  /// @param buffer_size frames per level
  /// @param depth number of levels (longest filter chain + 1)
  /// @return false if the single allocation failed
  bool init(uint32_t buffer_size, uint8_t depth);

  float *current() { return this->storage_.get() + static_cast<size_t>(this->index_) * this->buffer_size_; }
  size_t size() const { return this->size_; }
  uint32_t capacity() const { return this->buffer_size_; }

  /// Duplicate the current level into the next one and descend into it.
  void push();
  void pop();
  /// Return to level 0 and declare how many frames it holds.
  void reset(size_t size);

 protected:
  std::unique_ptr<float[]> storage_;
  uint32_t buffer_size_{0};
  size_t size_{0};
  uint8_t depth_{0};
  uint8_t index_{0};
};

class Filter {
 public:
  virtual ~Filter() = default;
  virtual void process(float *data, size_t len) = 0;
  virtual void reset() = 0;
};

/// Cascade of second-order sections. Coefficients live in a codegen-emitted `static const`
/// table in flash; only the delay lines are per-instance.
class SosFilter final : public Filter {
 public:
  SosFilter(const SosCoeffs *coeffs, uint8_t count);
  void process(float *data, size_t len) override;
  void reset() override;

 protected:
  const SosCoeffs *coeffs_{nullptr};
  FixedVector<SosState> state_;
  uint8_t count_{0};
};

#ifdef USE_SENSOR
class SoundLevelMeterSensor;
#endif

class SoundLevelMeter : public Component
#ifdef USE_OTA_STATE_LISTENER
    ,
                        public ota::OTAGlobalStateListener
#endif
{
#ifdef USE_SENSOR
  friend class SoundLevelMeterSensor;
#endif

 public:
  void set_update_interval(uint32_t update_interval_ms);
  uint32_t get_update_interval() const { return this->update_interval_ms_; }
  void set_ring_buffer_size(uint32_t ring_buffer_size_ms) { this->ring_buffer_size_ms_ = ring_buffer_size_ms; }
  void set_microphone_source(microphone::MicrophoneSource *microphone_source);
  void set_warmup_interval(uint32_t warmup_interval_ms) { this->warmup_interval_ms_ = warmup_interval_ms; }
  void set_task_stack_size(uint32_t task_stack_size) { this->task_stack_size_ = task_stack_size; }
  void set_task_priority(uint8_t task_priority) { this->task_priority_ = task_priority; }
  void set_task_core(uint8_t task_core) { this->task_core_ = task_core; }
  void set_mic_sensitivity(float mic_sensitivity);
  void set_mic_sensitivity_ref(float mic_sensitivity_ref);
  void set_offset(float offset);
  void set_is_high_freq(bool is_high_freq) { this->is_high_freq_ = is_high_freq; }
  void set_is_auto_start(bool is_auto_start) { this->is_auto_start_ = is_auto_start; }
  const DbAdjustment &get_db_adjustment() const { return this->db_adjustment_; }

  /// Sizing hooks emitted by codegen ahead of the matching add_*() calls.
  void init_dsp_filters(uint16_t count) { this->dsp_filters_.init(count); }
  void add_dsp_filter(Filter *dsp_filter);
#ifdef USE_SENSOR
  void init_sensors(uint16_t count) { this->sensors_.init(count); }
  void add_sensor(SoundLevelMeterSensor *sensor);
#endif

  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::LATE; }

  void start();
  void stop();
  bool is_running() const { return this->is_running_.load(); }

#ifdef USE_OTA_STATE_LISTENER
  void on_ota_global_state(ota::OTAState state, float progress, uint8_t error, ota::OTAComponent *comp) override;
#endif

 protected:
  microphone::MicrophoneSource *microphone_source_{nullptr};
  FixedVector<Filter *> dsp_filters_;
  DbAdjustment db_adjustment_;
  BufferStack buffers_;
  std::unique_ptr<ring_buffer::RingBuffer> ring_buffer_;
  std::mutex task_mutex_;
  TaskHandle_t task_handle_{nullptr};
  HighFrequencyLoopRequester high_freq_;

  uint32_t update_interval_ms_{60000};
  uint32_t ring_buffer_size_ms_{256};
  uint32_t warmup_interval_ms_{500};
  uint32_t task_stack_size_{1024};
  uint32_t last_overflow_log_ms_{0};
  size_t ring_buffer_stats_free_{SIZE_MAX};

  std::atomic<bool> is_running_{false};
  std::atomic<bool> was_running_before_ota_{false};
  std::atomic<bool> is_pending_stop_{false};
  uint8_t task_priority_{1};
  uint8_t task_core_{1};
  uint8_t max_filter_depth_{0};
  bool is_high_freq_{false};
  bool is_auto_start_{true};
  bool is_sized_{false};
  bool has_mic_sensitivity_{false};
  bool has_mic_sensitivity_ref_{false};

#ifdef USE_SENSOR
  struct PendingPublishState {
    float value{NAN};
    bool has_value{false};
  };

  FixedVector<SoundLevelMeterSensor *> sensors_;
  FixedVector<PendingPublishState> pending_publish_;
  FixedVector<Filter *> prefix_scratch_;
  std::mutex publish_mutex_;
  size_t publish_cursor_{0};

  void sort_sensors_();
  void defer_publish_state_(uint16_t index, float state);
#endif

  audio::AudioStreamInfo get_audio_stream_info_() const { return this->microphone_source_->get_audio_stream_info(); }
  uint32_t ms_to_frames_(uint32_t ms) const {
    return ms_to_frames(this->get_audio_stream_info_().get_sample_rate(), ms);
  }
  bool allocate_buffers_();
  size_t read_samples_(TickType_t ticks_to_wait);
  void process_();
  void reset_();

  static void task(void *param);
};

#ifdef USE_SENSOR
class SoundLevelMeterSensor : public sensor::Sensor {
  friend class SoundLevelMeter;

 public:
  void set_parent(SoundLevelMeter *parent);
  void set_update_interval(uint32_t update_interval_ms) { this->update_interval_ms_ = update_interval_ms; }
  void init_dsp_filters(uint16_t count) { this->dsp_filters_.init(count); }
  void add_dsp_filter(Filter *dsp_filter);

  virtual void process(const float *data, size_t len) = 0;
  virtual void reset() = 0;
  /// Convert the configured intervals into frame counts. Called by the audio task on start,
  /// once the microphone's stream info is known.
  virtual void update_sample_counts(uint32_t sample_rate);

 protected:
  SoundLevelMeter *parent_{nullptr};
  FixedVector<Filter *> dsp_filters_;
  uint32_t update_samples_{0};
  uint32_t update_interval_ms_{60000};
  uint16_t publish_index_{0};

  void defer_publish_state_(float state) { this->parent_->defer_publish_state_(this->publish_index_, state); }
  float adjust_db_(float db, bool is_rms = true) const {
    return adjust_dbfs(db, this->parent_->get_db_adjustment(), is_rms);
  }
};

/// Equivalent continuous sound level (Leq) over the update interval.
class SoundLevelMeterSensorEq final : public SoundLevelMeterSensor {
 public:
  void process(const float *data, size_t len) override;
  void reset() override;

 protected:
  double sum_{0.};
  uint32_t count_{0};
};

/// Loudest windowed Leq within the update interval.
class SoundLevelMeterSensorMax final : public SoundLevelMeterSensor {
 public:
  void set_window_size(uint32_t window_size_ms) { this->window_size_ms_ = window_size_ms; }
  void process(const float *data, size_t len) override;
  void reset() override;
  void update_sample_counts(uint32_t sample_rate) override;

 protected:
  double sum_{0.};
  double max_{0.};
  uint32_t window_size_ms_{0};
  uint32_t window_samples_{0};
  uint32_t count_sum_{0};
  uint32_t count_max_{0};
  bool has_max_window_{false};
};

/// Quietest windowed Leq within the update interval.
class SoundLevelMeterSensorMin final : public SoundLevelMeterSensor {
 public:
  void set_window_size(uint32_t window_size_ms) { this->window_size_ms_ = window_size_ms; }
  void process(const float *data, size_t len) override;
  void reset() override;
  void update_sample_counts(uint32_t sample_rate) override;

 protected:
  double sum_{0.};
  double min_{0.};
  uint32_t window_size_ms_{0};
  uint32_t window_samples_{0};
  uint32_t count_sum_{0};
  uint32_t count_min_{0};
  bool has_min_window_{false};
};

/// Highest instantaneous absolute amplitude within the update interval.
class SoundLevelMeterSensorPeak final : public SoundLevelMeterSensor {
 public:
  void process(const float *data, size_t len) override;
  void reset() override;

 protected:
  float peak_{0.f};
  uint32_t count_{0};
};
#endif  // USE_SENSOR

template<typename... Ts> class StartAction final : public Action<Ts...> {
 public:
  explicit StartAction(SoundLevelMeter *sound_level_meter) : sound_level_meter_(sound_level_meter) {}

  void play(Ts... x) override { this->sound_level_meter_->start(); }

 protected:
  SoundLevelMeter *sound_level_meter_;
};

template<typename... Ts> class StopAction final : public Action<Ts...> {
 public:
  explicit StopAction(SoundLevelMeter *sound_level_meter) : sound_level_meter_(sound_level_meter) {}

  void play(Ts... x) override { this->sound_level_meter_->stop(); }

 protected:
  SoundLevelMeter *sound_level_meter_;
};

}  // namespace esphome::sound_level_meter
