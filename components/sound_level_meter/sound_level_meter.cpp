#include "sound_level_meter.h"

#include <cstring>
#include <new>

#ifdef USE_ESP_DSP
#include "dsps_biquad.h"
#endif

namespace esphome::sound_level_meter {

static const char *const TAG = "sound_level_meter";

static constexpr uint32_t AUDIO_BUFFER_DURATION_MS = 20;
/// Sensor states handed to the API per loop iteration, so a burst of simultaneous
/// updates cannot stall the main loop.
static constexpr uint8_t MAX_PUBLISHES_PER_LOOP = 5;
static constexpr uint32_t OVERFLOW_LOG_INTERVAL_MS = 1000;

/* BufferStack */

bool BufferStack::init(uint32_t buffer_size, uint8_t depth) {
  this->buffer_size_ = buffer_size;
  this->depth_ = depth;
  this->size_ = 0;
  this->index_ = 0;
  if (buffer_size == 0 || depth == 0)
    return false;
  // nothrow: on failure we want a null pointer to report, not an abort.
  this->storage_.reset(new (std::nothrow) float[static_cast<size_t>(buffer_size) * depth]());
  return this->storage_ != nullptr;
}

void BufferStack::push() {
  if (this->index_ + 1 >= this->depth_) {
    // Cannot happen: depth is derived from the longest configured filter chain in setup().
    ESP_LOGE(TAG, "Buffer stack depth exceeded, filter output discarded");
    return;
  }
  const float *src = this->current();
  this->index_++;
  memcpy(this->current(), src, this->size_ * sizeof(float));
}

void BufferStack::pop() {
  if (this->index_ > 0)
    this->index_--;
}

void BufferStack::reset(size_t size) {
  this->index_ = 0;
  this->size_ = std::min<size_t>(size, this->buffer_size_);
}

/* SosFilter */

SosFilter::SosFilter(const SosCoeffs *coeffs, uint8_t count) : coeffs_(coeffs), count_(count) {
  this->state_.init(count);
  for (uint8_t i = 0; i < count; i++)
    this->state_.push_back(SosState{0.f, 0.f});
}

void SosFilter::process(float *data, size_t len) {
  for (uint8_t j = 0; j < this->count_; j++) {
#ifdef USE_ESP_DSP
    // SosCoeffs/SosState are standard-layout float aggregates, so they alias the flat
    // {b0,b1,b2,a1,a2} / {s0,s1} arrays esp-dsp expects.
    static_assert(sizeof(SosCoeffs) == 5 * sizeof(float), "SosCoeffs must be tightly packed");
    static_assert(sizeof(SosState) == 2 * sizeof(float), "SosState must be tightly packed");
    auto *coeffs = const_cast<float *>(reinterpret_cast<const float *>(&this->coeffs_[j]));
    auto *state = reinterpret_cast<float *>(&this->state_[j]);
    const int block = static_cast<int>(len);
#if defined(USE_ESP32_VARIANT_ESP32)
    dsps_biquad_f32_ae32(data, data, block, coeffs, state);
#elif defined(USE_ESP32_VARIANT_ESP32S3)
    dsps_biquad_f32_aes3(data, data, block, coeffs, state);
#elif defined(USE_ESP32_VARIANT_ESP32P4)
    dsps_biquad_f32_arp4(data, data, block, coeffs, state);
#else
    dsps_biquad_f32_ansi(data, data, block, coeffs, state);
#endif
#else
    sos_process_section(this->coeffs_[j], this->state_[j], data, len);
#endif
  }
}

void SosFilter::reset() {
  for (auto &s : this->state_)
    s = SosState{0.f, 0.f};
}

/* SoundLevelMeter */

void SoundLevelMeter::set_update_interval(uint32_t update_interval_ms) {
  this->update_interval_ms_ = update_interval_ms;
}

void SoundLevelMeter::set_microphone_source(microphone::MicrophoneSource *microphone_source) {
  this->microphone_source_ = microphone_source;
}

// The sensitivity correction needs both the datasheet figure and the reference level, so it
// only becomes active once each has been configured.
void SoundLevelMeter::set_mic_sensitivity(float mic_sensitivity) {
  this->db_adjustment_.sensitivity = mic_sensitivity;
  this->has_mic_sensitivity_ = true;
  this->db_adjustment_.has_sensitivity = this->has_mic_sensitivity_ && this->has_mic_sensitivity_ref_;
}

void SoundLevelMeter::set_mic_sensitivity_ref(float mic_sensitivity_ref) {
  this->db_adjustment_.sensitivity_ref = mic_sensitivity_ref;
  this->has_mic_sensitivity_ref_ = true;
  this->db_adjustment_.has_sensitivity = this->has_mic_sensitivity_ && this->has_mic_sensitivity_ref_;
}

void SoundLevelMeter::set_offset(float offset) {
  this->db_adjustment_.offset = offset;
  this->db_adjustment_.has_offset = true;
}

void SoundLevelMeter::add_dsp_filter(Filter *dsp_filter) {
  if (this->is_sized_ || this->dsp_filters_.full()) {
    ESP_LOGE(TAG, "Filter registered after sizing, ignored");
    return;
  }
  this->dsp_filters_.push_back(dsp_filter);
}

#ifdef USE_SENSOR
void SoundLevelMeter::add_sensor(SoundLevelMeterSensor *sensor) {
  if (this->is_sized_ || this->sensors_.full()) {
    ESP_LOGE(TAG, "Sensor registered after sizing, ignored");
    return;
  }
  this->sensors_.push_back(sensor);
}
#endif

void SoundLevelMeter::setup() {
  if (this->microphone_source_ == nullptr) {
    this->mark_failed(LOG_STR("no microphone source"));
    return;
  }

#ifdef USE_SENSOR
  this->sort_sensors_();

  uint8_t max_depth = 0;
  this->pending_publish_.init(this->sensors_.size());
  for (uint16_t i = 0; i < this->sensors_.size(); i++) {
    this->sensors_[i]->publish_index_ = i;
    this->pending_publish_.push_back({});
    max_depth = std::max<uint8_t>(max_depth, static_cast<uint8_t>(this->sensors_[i]->dsp_filters_.size()));
  }
  if (this->pending_publish_.size() != this->sensors_.size()) {
    ESP_LOGE(TAG, "Publish queue alloc failed for %zu sensors", this->sensors_.size());
    this->mark_failed(LOG_STR("publish queue allocation failed"));
    return;
  }

  this->max_filter_depth_ = max_depth;
  this->prefix_scratch_.init(max_depth);
  for (uint8_t i = 0; i < max_depth; i++)
    this->prefix_scratch_.push_back(nullptr);
  if (this->prefix_scratch_.size() != max_depth) {
    ESP_LOGE(TAG, "Filter prefix alloc failed for depth %u", max_depth);
    this->mark_failed(LOG_STR("filter prefix allocation failed"));
    return;
  }
#endif

  // No further entities may be registered from here on; sizes above are now fixed.
  this->is_sized_ = true;

  this->microphone_source_->add_data_callback([this](const std::vector<uint8_t> &data) {
    if (!this->is_running_.load() || this->ring_buffer_ == nullptr)
      return;
    if (this->ring_buffer_->free() < data.size()) {
      const uint32_t now = App.get_loop_component_start_time();
      if (now - this->last_overflow_log_ms_ >= OVERFLOW_LOG_INTERVAL_MS) {
        this->last_overflow_log_ms_ = now;
        ESP_LOGW(TAG, "Ring buffer full, dropping audio chunk");
      }
      return;
    }
    this->ring_buffer_->write(data.data(), data.size());
    this->ring_buffer_stats_free_ = std::min(this->ring_buffer_->free(), this->ring_buffer_stats_free_);
  });

  if (this->is_auto_start_)
    this->start();

#ifdef USE_OTA_STATE_LISTENER
  ota::get_global_ota_callback()->add_global_state_listener(this);
#endif
}

void SoundLevelMeter::dump_config() {
  ESP_LOGCONFIG(TAG,
                "Sound Level Meter:\n"
                "  Update Interval: %" PRIu32 " ms\n"
                "  Ring Buffer Size: %" PRIu32 " ms\n"
                "  Warmup Interval: %" PRIu32 " ms\n"
                "  Task Stack Size: %" PRIu32 "\n"
                "  Task Priority: %u\n"
                "  Task Core: %u\n"
                "  Auto Start: %s",
                this->update_interval_ms_, this->ring_buffer_size_ms_, this->warmup_interval_ms_,
                this->task_stack_size_, this->task_priority_, this->task_core_, YESNO(this->is_auto_start_));
#ifdef USE_SENSOR
  for (auto *s : this->sensors_)
    LOG_SENSOR("  ", "Sound Pressure Level", s);
#endif
}

void SoundLevelMeter::loop() {
#ifdef USE_SENSOR
  for (uint8_t published = 0; published < MAX_PUBLISHES_PER_LOOP; published++) {
    SoundLevelMeterSensor *sensor = nullptr;
    float value = NAN;

    {
      std::lock_guard<std::mutex> lock(this->publish_mutex_);
      const size_t count = this->pending_publish_.size();
      for (size_t k = 0; k < count; k++) {
        const size_t i = (this->publish_cursor_ + k) % count;
        auto &slot = this->pending_publish_[i];
        if (!slot.has_value)
          continue;
        slot.has_value = false;
        sensor = this->sensors_[i];
        value = slot.value;
        this->publish_cursor_ = (i + 1) % count;
        break;
      }
      if (sensor == nullptr) {
        // Nothing left to hand over; the audio task wakes us again via
        // enable_loop_soon_any_context() when it queues the next state.
        this->disable_loop();
        return;
      }
    }

    sensor->publish_state(value);
  }
#else
  this->disable_loop();
#endif
}

bool SoundLevelMeter::allocate_buffers_() {
  // Allocated on the first start() and kept for the lifetime of the component: repeatedly
  // freeing and re-taking blocks this large across stop/start cycles fragments the heap.
  if (this->ring_buffer_ != nullptr)
    return true;

  const auto stream_info = this->get_audio_stream_info_();

  this->ring_buffer_ = ring_buffer::RingBuffer::create(stream_info.ms_to_bytes(this->ring_buffer_size_ms_));
  if (this->ring_buffer_ == nullptr) {
    ESP_LOGE(TAG, "Ring buffer alloc failed for %" PRIu32 " ms", this->ring_buffer_size_ms_);
    this->mark_failed(LOG_STR("ring buffer allocation failed"));
    return false;
  }

  const uint32_t frames = ms_to_frames(stream_info.get_sample_rate(), AUDIO_BUFFER_DURATION_MS);
  const uint8_t depth = this->max_filter_depth_ + 1;
  if (!this->buffers_.init(frames, depth)) {
    ESP_LOGE(TAG, "Audio buffer alloc failed for %" PRIu32 " frames x %u", frames, depth);
    this->mark_failed(LOG_STR("audio buffer allocation failed"));
    return false;
  }

  return true;
}

void SoundLevelMeter::start() {
  std::lock_guard<std::mutex> lock(this->task_mutex_);
  if (this->is_running_.load() || this->task_handle_ != nullptr)
    return;
  if (this->is_failed() || !this->allocate_buffers_())
    return;

  this->ring_buffer_->reset();
  this->ring_buffer_stats_free_ = SIZE_MAX;
  this->is_pending_stop_.store(false);
  this->is_running_.store(true);

  if (xTaskCreatePinnedToCore(SoundLevelMeter::task, "sound_level_meter", this->task_stack_size_, this,
                              this->task_priority_, &this->task_handle_, this->task_core_) != pdPASS) {
    this->is_running_.store(false);
    this->task_handle_ = nullptr;
    ESP_LOGE(TAG, "Task creation failed");
    this->mark_failed(LOG_STR("task creation failed"));
    return;
  }

  ESP_LOGD(TAG, "Started");
}

void SoundLevelMeter::stop() {
  if ((this->is_running_.load() || this->task_handle_ != nullptr) && !this->is_pending_stop_.load()) {
    this->is_pending_stop_.store(true);
    ESP_LOGD(TAG, "Stop requested");
  }
}

#ifdef USE_OTA_STATE_LISTENER
void SoundLevelMeter::on_ota_global_state(ota::OTAState state, float progress, uint8_t error, ota::OTAComponent *comp) {
  if (state == ota::OTA_STARTED) {
    this->was_running_before_ota_.store(this->is_running());
    this->stop();
  } else if (state == ota::OTA_ERROR || state == ota::OTA_ABORT) {
    if (this->was_running_before_ota_.load())
      this->start();
  }
}
#endif

void SoundLevelMeter::task(void *param) {
  auto *this_ = static_cast<SoundLevelMeter *>(param);
  const uint32_t sample_rate = this_->get_audio_stream_info_().get_sample_rate();
  const TickType_t read_timeout = 2 * pdMS_TO_TICKS(AUDIO_BUFFER_DURATION_MS);

  this_->reset_();
  this_->microphone_source_->start();

#ifdef USE_SENSOR
  for (auto *s : this_->sensors_)
    s->update_sample_counts(sample_rate);
#endif

  if (this_->is_high_freq_)
    this_->high_freq_.start();

  // Deliberately millis(): this runs on the component's own task, where the main loop's
  // cached start time would not advance in step with the audio being consumed here.
  const uint32_t warmup_start = millis();
  while (!this_->is_pending_stop_.load() && millis() - warmup_start < this_->warmup_interval_ms_)
    this_->read_samples_(read_timeout);

#if defined(ESPHOME_LOG_LEVEL) && ESPHOME_LOG_LEVEL >= ESPHOME_LOG_LEVEL_DEBUG
  const uint32_t stats_frames = ms_to_frames(sample_rate, this_->update_interval_ms_);
  uint64_t process_time = 0;
  uint32_t process_count = 0;
#endif

  while (!this_->is_pending_stop_.load()) {
    if (!this_->microphone_source_->is_running()) {
      if (!this_->status_has_warning()) {
        this_->status_set_warning(LOG_STR("microphone isn't running, can't compute statistics"));
        this_->reset_();
      }
      delay(AUDIO_BUFFER_DURATION_MS);
      continue;
    }

    if (this_->status_has_warning())
      this_->status_clear_warning();

    if (this_->read_samples_(read_timeout) == 0)
      continue;

#if defined(ESPHOME_LOG_LEVEL) && ESPHOME_LOG_LEVEL >= ESPHOME_LOG_LEVEL_DEBUG
    const int64_t process_start = esp_timer_get_time();
#endif

    this_->process_();

#if defined(ESPHOME_LOG_LEVEL) && ESPHOME_LOG_LEVEL >= ESPHOME_LOG_LEVEL_DEBUG
    process_time += static_cast<uint64_t>(esp_timer_get_time() - process_start);
    process_count += this_->buffers_.size();

    if (stats_frames > 0 && process_count >= stats_frames) {
      const float cpu_util = static_cast<float>(process_time) / 1000.f / this_->update_interval_ms_;
      const size_t rb_size = this_->ring_buffer_->available() + this_->ring_buffer_->free();
      const float rb_util = static_cast<float>(rb_size - this_->ring_buffer_stats_free_) / rb_size;
      ESP_LOGD(TAG, "CPU (core %d) utilization: %.1f%%, ring buffer utilization: %.1f%%",
               static_cast<int>(xPortGetCoreID()), cpu_util * 100.f, rb_util * 100.f);
      process_time = 0;
      process_count = 0;
      this_->ring_buffer_stats_free_ = SIZE_MAX;
    }
#endif
  }

  this_->microphone_source_->stop();

  if (this_->is_high_freq_)
    this_->high_freq_.stop();

  this_->reset_();

  this_->is_running_.store(false);
  this_->is_pending_stop_.store(false);
  {
    std::lock_guard<std::mutex> lock(this_->task_mutex_);
    this_->task_handle_ = nullptr;
  }
  vTaskDelete(nullptr);
}

size_t SoundLevelMeter::read_samples_(TickType_t ticks_to_wait) {
  const uint8_t bytes_per_sample = this->get_audio_stream_info_().samples_to_bytes(1);
  float *data = this->buffers_.current();

  const size_t bytes_read = this->ring_buffer_->read(data, this->buffers_.capacity() * bytes_per_sample, ticks_to_wait);
  const size_t samples_read = bytes_read / bytes_per_sample;
  this->buffers_.reset(samples_read);
  if (samples_read == 0)
    return 0;

  // Expand in place, back to front: each sample grows from bytes_per_sample to sizeof(float).
  const auto *data_as_uint8 = reinterpret_cast<const uint8_t *>(data);
  for (int i = static_cast<int>(bytes_read) - bytes_per_sample, j = static_cast<int>(samples_read) - 1; i >= 0;
       i -= bytes_per_sample, j--) {
    data[j] = audio::unpack_audio_sample_to_q31(&data_as_uint8[i], bytes_per_sample) / static_cast<float>(INT32_MAX);
  }

  return samples_read;
}

void SoundLevelMeter::process_() {
#ifdef USE_SENSOR
  // Sensors are sorted by filter chain, so consecutive sensors often share a prefix: walk
  // only the sections that differ from the previous sensor and reuse the rest.
  // prefix_len tracks how much of prefix_scratch_ is live (FixedVector has no pop_back).
  size_t prefix_len = 0;

  for (auto *s : this->sensors_) {
    const size_t chain_length = s->dsp_filters_.size();
    size_t i = 0;
    while (i < chain_length && i < prefix_len && s->dsp_filters_[i] == this->prefix_scratch_[i])
      i++;

    while (prefix_len > i) {
      prefix_len--;
      this->buffers_.pop();
    }

    for (; i < chain_length; i++) {
      Filter *f = s->dsp_filters_[i];
      this->buffers_.push();
      f->process(this->buffers_.current(), this->buffers_.size());
      this->prefix_scratch_[prefix_len++] = f;
    }

    s->process(this->buffers_.current(), this->buffers_.size());
  }
#endif
}

void SoundLevelMeter::reset_() {
  for (auto *f : this->dsp_filters_)
    f->reset();
#ifdef USE_SENSOR
  for (auto *s : this->sensors_)
    s->reset();
#endif
}

#ifdef USE_SENSOR

void SoundLevelMeter::sort_sensors_() {
  // Group sensors by their filter chain so that shared prefixes are computed once.
  std::sort(this->sensors_.begin(), this->sensors_.end(), [](SoundLevelMeterSensor *a, SoundLevelMeterSensor *b) {
    return std::lexicographical_compare(a->dsp_filters_.begin(), a->dsp_filters_.end(), b->dsp_filters_.begin(),
                                        b->dsp_filters_.end());
  });
}

void SoundLevelMeter::defer_publish_state_(uint16_t index, float state) {
  {
    std::lock_guard<std::mutex> lock(this->publish_mutex_);
    if (index >= this->pending_publish_.size())
      return;
    this->pending_publish_[index].value = state;
    this->pending_publish_[index].has_value = true;
  }
  // Called from the audio task: this is the thread-safe, allocation-free wake API.
  this->enable_loop_soon_any_context();
}

/* SoundLevelMeterSensor */

void SoundLevelMeterSensor::set_parent(SoundLevelMeter *parent) {
  this->parent_ = parent;
  this->update_interval_ms_ = parent->get_update_interval();
}

void SoundLevelMeterSensor::add_dsp_filter(Filter *dsp_filter) {
  if (this->dsp_filters_.full()) {
    ESP_LOGE(TAG, "Sensor filter registered beyond capacity, ignored");
    return;
  }
  this->dsp_filters_.push_back(dsp_filter);
}

void SoundLevelMeterSensor::update_sample_counts(uint32_t sample_rate) {
  this->update_samples_ = ms_to_frames(sample_rate, this->update_interval_ms_);
}

/* SoundLevelMeterSensorEq */

void SoundLevelMeterSensorEq::process(const float *data, size_t len) {
  double local_sum = 0.;
  for (size_t i = 0; i < len; i++) {
    local_sum += static_cast<double>(data[i]) * data[i];
    this->count_++;
    if (this->count_ == this->update_samples_) {
      const double mean_square = (this->sum_ + local_sum) / this->count_;
      this->defer_publish_state_(this->adjust_db_(mean_square_to_dbfs(mean_square)));
      this->sum_ = 0.;
      this->count_ = 0;
      local_sum = 0.;
    }
  }
  this->sum_ += local_sum;
}

void SoundLevelMeterSensorEq::reset() {
  this->sum_ = 0.;
  this->count_ = 0;
  this->defer_publish_state_(NAN);
}

/* SoundLevelMeterSensorMax */

void SoundLevelMeterSensorMax::update_sample_counts(uint32_t sample_rate) {
  SoundLevelMeterSensor::update_sample_counts(sample_rate);
  this->window_samples_ = ms_to_frames(sample_rate, this->window_size_ms_);
}

void SoundLevelMeterSensorMax::process(const float *data, size_t len) {
  for (size_t i = 0; i < len; i++) {
    this->sum_ += static_cast<double>(data[i]) * data[i];
    this->count_sum_++;
    if (this->count_sum_ == this->window_samples_) {
      const double window_value = this->sum_ / this->count_sum_;
      this->max_ = this->has_max_window_ ? std::max(this->max_, window_value) : window_value;
      this->has_max_window_ = true;
      this->sum_ = 0.;
      this->count_sum_ = 0;
    }
    this->count_max_++;
    if (this->count_max_ == this->update_samples_) {
      this->defer_publish_state_(this->has_max_window_ ? this->adjust_db_(mean_square_to_dbfs(this->max_)) : NAN);
      this->max_ = 0.;
      this->has_max_window_ = false;
      this->count_max_ = 0;
    }
  }
}

void SoundLevelMeterSensorMax::reset() {
  this->sum_ = 0.;
  this->max_ = 0.;
  this->has_max_window_ = false;
  this->count_max_ = 0;
  this->count_sum_ = 0;
  this->defer_publish_state_(NAN);
}

/* SoundLevelMeterSensorMin */

void SoundLevelMeterSensorMin::update_sample_counts(uint32_t sample_rate) {
  SoundLevelMeterSensor::update_sample_counts(sample_rate);
  this->window_samples_ = ms_to_frames(sample_rate, this->window_size_ms_);
}

void SoundLevelMeterSensorMin::process(const float *data, size_t len) {
  for (size_t i = 0; i < len; i++) {
    this->sum_ += static_cast<double>(data[i]) * data[i];
    this->count_sum_++;
    if (this->count_sum_ == this->window_samples_) {
      const double window_value = this->sum_ / this->count_sum_;
      this->min_ = this->has_min_window_ ? std::min(this->min_, window_value) : window_value;
      this->has_min_window_ = true;
      this->sum_ = 0.;
      this->count_sum_ = 0;
    }
    this->count_min_++;
    if (this->count_min_ == this->update_samples_) {
      this->defer_publish_state_(this->has_min_window_ ? this->adjust_db_(mean_square_to_dbfs(this->min_)) : NAN);
      this->min_ = 0.;
      this->has_min_window_ = false;
      this->count_min_ = 0;
    }
  }
}

void SoundLevelMeterSensorMin::reset() {
  this->sum_ = 0.;
  this->min_ = 0.;
  this->has_min_window_ = false;
  this->count_min_ = 0;
  this->count_sum_ = 0;
  this->defer_publish_state_(NAN);
}

/* SoundLevelMeterSensorPeak */

void SoundLevelMeterSensorPeak::process(const float *data, size_t len) {
  for (size_t i = 0; i < len; i++) {
    this->peak_ = std::max(this->peak_, std::fabs(data[i]));
    this->count_++;
    if (this->count_ == this->update_samples_) {
      this->defer_publish_state_(this->adjust_db_(peak_to_dbfs(this->peak_), false));
      this->peak_ = 0.f;
      this->count_ = 0;
    }
  }
}

void SoundLevelMeterSensorPeak::reset() {
  this->peak_ = 0.f;
  this->count_ = 0;
  this->defer_publish_state_(NAN);
}

#endif  // USE_SENSOR

}  // namespace esphome::sound_level_meter
