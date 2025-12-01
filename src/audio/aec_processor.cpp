/*
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * By contributing to this project, you agree to license your contributions
 * under the GPLv3 (or any later version) or any future licenses chosen by
 * the project author(s). Contributions include any modifications,
 * enhancements, or additions to the project. These contributions become
 * part of the project and are adopted by the project author(s).
 *
 * WebRTC AEC3 Processor Implementation
 *
 * Key Design Decisions:
 *
 * 1. Per-Frame Locking: Instead of locking for the entire aec_process() call,
 *    we lock only during WebRTC API calls (~10ms frames). This prevents
 *    blocking the real-time audio capture thread for extended periods.
 *
 * 2. Lock-Free Reference Path: aec_add_reference() uses ring_buffer_write()
 *    which is internally mutex-protected but non-blocking. The TTS thread
 *    can always write without waiting.
 *
 * 3. Graceful Degradation: On errors, AEC passes through unprocessed audio
 *    and tracks consecutive errors. After AEC_MAX_CONSECUTIVE_ERRORS,
 *    AEC disables itself to prevent log spam and wasted CPU.
 *
 * 4. Pre-allocated Buffers: All frame buffers are allocated at init time.
 *    No malloc/realloc in the processing path.
 *
 * 5. Reference Buffer Sizing: Default 500ms buffer accommodates typical
 *    acoustic delays (speaker to mic) plus system buffering delays.
 */

#include "audio/aec_processor.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <mutex>
#include <vector>

// Note: No resampler needed in AEC processor
// - Mic input: native 48kHz from capture thread
// - TTS reference: TTS module resamples 22050Hz→48kHz before calling aec_add_reference()

// WebRTC build configuration - must be before WebRTC headers
#ifndef WEBRTC_POSIX
#define WEBRTC_POSIX 1
#endif
#ifndef WEBRTC_APM_DEBUG_DUMP
#define WEBRTC_APM_DEBUG_DUMP 0
#endif

// WebRTC audio processing includes
// Note: Using local build path structure
#include <webrtc/api/audio/echo_canceller3_config.h>
#include <webrtc/api/audio/echo_control.h>
#include <webrtc/modules/audio_processing/aec3/echo_canceller3.h>
#include <webrtc/modules/audio_processing/include/audio_processing.h>
#include <webrtc/modules/audio_processing/include/audio_processing_statistics.h>

extern "C" {
#include "audio/ring_buffer.h"
#include "logging.h"
}

/**
 * AEC Native 48kHz Processing
 *
 * CRITICAL: WebRTC AEC3 doesn't work properly at 16kHz!
 * Reports show that echo cancellation only works at 32kHz or 48kHz.
 * At 16kHz, ERL (detection) works but ERLE (cancellation) doesn't.
 *
 * Architecture (native 48kHz capture):
 * - Mic input: Captured at 48kHz, processed directly by AEC
 * - AEC output: 48kHz, downsampled to 16kHz in capture thread for ASR
 * - TTS reference: 16kHz from TTS, upsampled to 48kHz here before feeding to AEC
 *
 * This eliminates the mic up/downsample path, reducing CPU and latency.
 */
namespace {

// TTS module resamples 22050Hz → AEC_SAMPLE_RATE (48kHz) before calling aec_add_reference()
// So reference audio arrives at 48kHz - no resampling needed here

/**
 * A single audio frame (10ms = 480 samples at 48kHz = AEC_FRAME_SAMPLES)
 */
struct AudioFrame {
   int16_t samples[AEC_FRAME_SAMPLES];
};

/**
 * Delay Line Reference Buffer for AEC (at 48kHz internal rate)
 *
 * DESIGN: Simple delay line that outputs samples delayed by a fixed amount.
 * This is much simpler than trying to pace based on wall-clock time.
 *
 * How it works:
 * 1. TTS writes samples (already resampled to 48kHz) to a circular buffer
 * 2. We track total written and total read counts
 * 3. Read always lags behind write by delay_samples_
 * 4. If we haven't written enough yet, return silence
 *
 * This naturally creates the delay needed for AEC: the reference signal
 * is delayed by the same amount as the acoustic path (ALSA buffer + air).
 *
 * Thread safety:
 * - write(): Called from TTS thread, protected by mutex
 * - read_frame(): Called from capture thread, protected by mutex
 */
class DelayLineBuffer {
 public:
   // 2 seconds of buffer at 48kHz
   static constexpr size_t BUFFER_SAMPLES = 96000;

   DelayLineBuffer(size_t delay_samples = 3360)  // 70ms at 48kHz = 3360 samples
       : delay_samples_(delay_samples) {
      buffer_.resize(BUFFER_SAMPLES, 0);
   }

   /**
    * Write samples to the circular buffer (expects 48kHz audio)
    */
   void write(const int16_t *samples, size_t num_samples) {
      if (!samples || num_samples == 0)
         return;

      std::lock_guard<std::mutex> lock(mutex_);

      for (size_t i = 0; i < num_samples; i++) {
         buffer_[write_pos_] = samples[i];
         write_pos_ = (write_pos_ + 1) % BUFFER_SAMPLES;
      }
      total_written_ += num_samples;
      total_writes_++;
   }

   // Compatibility alias
   void write_with_delay(const int16_t *samples, size_t num_samples, uint64_t /*delay_us*/) {
      write(samples, num_samples);
   }

   /**
    * Read one frame from the delay line (480 samples at 48kHz = 10ms)
    *
    * Returns samples that were written delay_samples_ ago.
    * If we haven't written that much yet, returns silence.
    */
   bool read_frame(int16_t *out_frame) {
      if (!out_frame)
         return false;

      std::lock_guard<std::mutex> lock(mutex_);

      // Check if we have enough data to satisfy the delay
      // We need: delay_samples_ + frame_size samples in the buffer
      uint64_t samples_available = total_written_ - total_read_;
      uint64_t samples_needed = delay_samples_ + AEC_FRAME_SAMPLES;

      if (samples_available < samples_needed) {
         // Not enough data yet - return silence
         memset(out_frame, 0, AEC_FRAME_SAMPLES * sizeof(int16_t));
         frames_empty_++;
         return false;
      }

      // Read from the delayed position
      for (size_t i = 0; i < AEC_FRAME_SAMPLES; i++) {
         out_frame[i] = buffer_[read_pos_];
         read_pos_ = (read_pos_ + 1) % BUFFER_SAMPLES;
      }
      total_read_ += AEC_FRAME_SAMPLES;
      frames_read_++;
      return true;
   }

   /**
    * Get buffer statistics
    */
   size_t get_frame_count() const {
      std::lock_guard<std::mutex> lock(mutex_);
      if (total_written_ <= total_read_ + delay_samples_)
         return 0;
      return (total_written_ - total_read_ - delay_samples_) / AEC_FRAME_SAMPLES;
   }

   uint64_t get_total_writes() const {
      return total_writes_;
   }
   uint64_t get_frames_read() const {
      return frames_read_;
   }
   uint64_t get_frames_empty() const {
      return frames_empty_;
   }
   uint64_t get_frames_dropped() const {
      return 0;
   }  // Delay line doesn't drop
   uint64_t get_frames_repeated() const {
      return 0;
   }  // Delay line doesn't repeat

   void clear() {
      std::lock_guard<std::mutex> lock(mutex_);
      std::fill(buffer_.begin(), buffer_.end(), 0);
      write_pos_ = 0;
      read_pos_ = 0;
      total_written_ = 0;
      total_read_ = 0;
      // Note: Don't reset cumulative stats (frames_read_, frames_empty_)
   }

 private:
   mutable std::mutex mutex_;
   std::vector<int16_t> buffer_;
   size_t delay_samples_;

   size_t write_pos_ = 0;
   size_t read_pos_ = 0;
   uint64_t total_written_ = 0;
   uint64_t total_read_ = 0;

   // Statistics (cumulative)
   std::atomic<uint64_t> total_writes_{ 0 };
   std::atomic<uint64_t> frames_read_{ 0 };
   std::atomic<uint64_t> frames_empty_{ 0 };
};

}  // namespace

namespace {

// Global acoustic delay for factory (set before aec_init creates factory)
// Default 70ms = ALSA buffer (~50ms) + acoustic path (~20ms)
static size_t g_acoustic_delay_ms = 70;

/**
 * Custom EchoControlFactory for DAWN
 *
 * Creates EchoCanceller3 with configuration tuned for:
 * - Embedded systems (Jetson/RPi) with typical 50-150ms total latency
 * - Speaker-to-mic feedback path in voice assistant setup
 * - Wider delay search range for variable ALSA buffering
 */
class DawnEchoControlFactory : public webrtc::EchoControlFactory {
 public:
   std::unique_ptr<webrtc::EchoControl> Create(int sample_rate_hz,
                                               int num_render_channels,
                                               int num_capture_channels) override {
      webrtc::EchoCanceller3Config config;

      // Delay configuration - Use EXTERNAL delay estimator
      //
      // With external estimator, AEC3 trusts our set_stream_delay_ms() value.
      // This gives us more control and avoids the estimator finding wrong correlations.
      //
      // Default delay = 70ms = ALSA buffer (~50ms) + acoustic path (~20ms)
      // In blocks: 70ms / 4ms = 17 blocks
      config.delay.default_delay = 17;                   // Starting point
      config.delay.use_external_delay_estimator = true;  // Trust our delay hint

      // Narrower search range since we're providing the delay
      config.delay.num_filters = 20;  // ~80ms search range (20 * 4ms)
      config.delay.delay_headroom_samples = 128;

      // More aggressive delay tracking
      config.delay.hysteresis_limit_blocks = 1;
      config.delay.delay_selection_thresholds.initial = 5;
      config.delay.delay_selection_thresholds.converged = 20;

      // Enable delay logging for debugging
      config.delay.log_warning_on_delay_changes = true;

      // Filter configuration - match typical delay range
      // Filter length in blocks determines max echo tail length
      config.filter.refined.length_blocks = 25;  // 25 blocks = 100ms echo tail
      config.filter.coarse.length_blocks = 25;   // Match refined
      config.filter.refined_initial.length_blocks = 20;
      config.filter.coarse_initial.length_blocks = 20;
      config.filter.initial_state_seconds = 1.0f;  // Faster adaptation

      // Echo model - stable echo path (speaker position doesn't change)
      config.echo_removal_control.has_clock_drift = false;
      config.echo_removal_control.linear_and_stable_echo_path = true;

      // Render levels - lower threshold to detect quieter reference
      config.render_levels.active_render_limit = 50.f;  // Default 100

      LOG_INFO("AEC3 factory: sample_rate=%dHz, EXTERNAL estimator, default=%zu blocks (%zums), "
               "search_range=%zu blocks (%zums)",
               sample_rate_hz, config.delay.default_delay, config.delay.default_delay * 4,
               config.delay.num_filters, config.delay.num_filters * 4);

      return std::make_unique<webrtc::EchoCanceller3>(config, sample_rate_hz, num_render_channels,
                                                      num_capture_channels);
   }
};

// AEC state - using raw pointer as v1.3 API returns AudioProcessing*
webrtc::AudioProcessing *g_apm = nullptr;
std::mutex g_aec_mutex;  // Protects WebRTC API calls only
std::atomic<bool> g_initialized{ false };
std::atomic<bool> g_active{ true };  // Can be disabled on repeated errors

// Simple FIFO reference buffer (TTS output at 48kHz internal rate)
// AEC3's internal delay estimator finds correlation between reference and capture
DelayLineBuffer *g_ref_buffer = nullptr;  // Simple delay line buffer

// Pre-allocated frame buffers at 48kHz (10ms = 480 samples)
int16_t g_ref_frame[AEC_FRAME_SAMPLES];
int16_t g_mic_frame[AEC_FRAME_SAMPLES];  // Mic input frame (native 48kHz)
int16_t g_out_frame[AEC_FRAME_SAMPLES];  // AEC output at 48kHz

// Note: TTS module already resamples 22050Hz → 48kHz before calling aec_add_reference()
// No resampler needed here - reference audio arrives at native 48kHz

// Error tracking
std::atomic<int> g_consecutive_errors{ 0 };

// Performance tracking
std::atomic<float> g_avg_processing_time_us{ 0.0f };
std::atomic<int> g_frame_count{ 0 };
std::atomic<uint64_t> g_frames_processed{ 0 };
std::atomic<uint64_t> g_frames_passed_through{ 0 };
std::atomic<int> g_last_delay_ms{ 0 };  // Last delay passed to AEC

// Configuration (set at init)
aec_config_t g_config;

}  // anonymous namespace

extern "C" {

aec_config_t aec_get_default_config(void) {
   aec_config_t config = {
      .enable_noise_suppression = true,
      .noise_suppression_level = AEC_NS_LEVEL_MODERATE,
      .enable_high_pass_filter = true,
      .mobile_mode = false,
      .ref_buffer_ms = 500,
      .noise_gate_threshold = 0,  // Disabled - VAD threshold approach is more effective
      .acoustic_delay_ms = 70     // ALSA buffer (~50ms) + acoustic path (~20ms)
   };
   return config;
}

int aec_init(const aec_config_t *config) {
   // Use defaults if no config provided
   if (config) {
      g_config = *config;
   } else {
      g_config = aec_get_default_config();
   }

   // Validate configuration
   if (g_config.ref_buffer_ms < AEC_MIN_REF_BUFFER_MS) {
      LOG_WARNING("AEC ref_buffer_ms (%zu) below minimum (%d), using minimum",
                  g_config.ref_buffer_ms, AEC_MIN_REF_BUFFER_MS);
      g_config.ref_buffer_ms = AEC_MIN_REF_BUFFER_MS;
   }

   // Validate and set acoustic delay (used by both AEC3 and PTS buffer)
   size_t acoustic_delay_ms = g_config.acoustic_delay_ms;
   if (acoustic_delay_ms < 10)
      acoustic_delay_ms = 10;  // Minimum 10ms
   if (acoustic_delay_ms > 200)
      acoustic_delay_ms = 200;               // Maximum 200ms
   g_acoustic_delay_ms = acoustic_delay_ms;  // Set for factory to use

   std::lock_guard<std::mutex> lock(g_aec_mutex);

   if (g_initialized.load()) {
      LOG_WARNING("AEC already initialized");
      return 0;
   }

   // Create AudioProcessing instance using builder pattern with custom AEC factory
   // NOTE: g_acoustic_delay_ms must be set before this call
   webrtc::AudioProcessingBuilder builder;
   builder.SetEchoControlFactory(std::make_unique<DawnEchoControlFactory>());
   g_apm = builder.Create();

   if (!g_apm) {
      LOG_ERROR("Failed to create AudioProcessing instance");
      return 1;
   }

   // Initialize AudioProcessing with the internal sample rate (48kHz)
   // This is CRITICAL - the EchoControlFactory's Create() is called during this!
   webrtc::ProcessingConfig processing_config;
   processing_config.input_stream().set_sample_rate_hz(AEC_SAMPLE_RATE);
   processing_config.input_stream().set_num_channels(1);
   processing_config.output_stream().set_sample_rate_hz(AEC_SAMPLE_RATE);
   processing_config.output_stream().set_num_channels(1);
   processing_config.reverse_input_stream().set_sample_rate_hz(AEC_SAMPLE_RATE);
   processing_config.reverse_input_stream().set_num_channels(1);
   processing_config.reverse_output_stream().set_sample_rate_hz(AEC_SAMPLE_RATE);
   processing_config.reverse_output_stream().set_num_channels(1);

   int init_result = g_apm->Initialize(processing_config);
   if (init_result != 0) {
      LOG_ERROR("Failed to initialize AudioProcessing at %dHz: error %d", AEC_SAMPLE_RATE,
                init_result);
      delete g_apm;
      g_apm = nullptr;
      return 1;
   }
   LOG_INFO("AEC: AudioProcessing initialized at %dHz", AEC_SAMPLE_RATE);

   // Configure AEC3
   webrtc::AudioProcessing::Config apm_config;
   apm_config.echo_canceller.enabled = true;
   apm_config.echo_canceller.mobile_mode = g_config.mobile_mode;

   // Noise suppression (optional, adds CPU load)
   apm_config.noise_suppression.enabled = g_config.enable_noise_suppression;
   if (g_config.enable_noise_suppression) {
      switch (g_config.noise_suppression_level) {
         case AEC_NS_LEVEL_LOW:
            apm_config.noise_suppression.level =
                webrtc::AudioProcessing::Config::NoiseSuppression::kLow;
            break;
         case AEC_NS_LEVEL_HIGH:
            apm_config.noise_suppression.level =
                webrtc::AudioProcessing::Config::NoiseSuppression::kHigh;
            break;
         case AEC_NS_LEVEL_MODERATE:
         default:
            apm_config.noise_suppression.level =
                webrtc::AudioProcessing::Config::NoiseSuppression::kModerate;
            break;
      }
   }

   // Disable AGC (DAWN handles gain elsewhere)
   apm_config.gain_controller1.enabled = false;
   apm_config.gain_controller2.enabled = false;

   // High-pass filter removes DC offset
   apm_config.high_pass_filter.enabled = g_config.enable_high_pass_filter;

   g_apm->ApplyConfig(apm_config);

   // Note: TTS module already resamples 22050Hz → 48kHz before calling aec_add_reference()
   // No resampler needed here - reference audio arrives at native 48kHz
   // Mic input is also native 48kHz from capture thread

   // Create delay line buffer at 48kHz
   // The delay line introduces a fixed delay matching the acoustic path
   // This aligns reference audio with when echo arrives at the microphone
   //
   // Delay = ALSA buffer (~50ms) + acoustic path (~20ms) = ~70ms
   // At 48kHz: 70ms = 3360 samples
   size_t delay_samples = (g_acoustic_delay_ms * AEC_SAMPLE_RATE) / 1000;

   g_ref_buffer = new DelayLineBuffer(delay_samples);
   LOG_INFO("AEC: Delay line buffer created with %zums (%zu samples at 48kHz) delay",
            g_acoustic_delay_ms, delay_samples);
   if (!g_ref_buffer) {
      LOG_ERROR("Failed to create AEC delay line buffer");
      delete g_apm;
      g_apm = nullptr;
      return 1;
   }

   // Note: Delay line outputs samples that were written delay_samples ago
   // This naturally aligns reference with echo arrival time

   // Reset state
   g_consecutive_errors.store(0);
   g_avg_processing_time_us.store(0.0f);
   g_frame_count.store(0);
   g_frames_processed.store(0);
   g_frames_passed_through.store(0);
   g_active.store(true);
   g_initialized.store(true);

   LOG_INFO("AEC3 initialized: %dHz (native capture), %zu samples/frame, "
            "delay_hint=%zums, mobile=%d, NS=%d",
            AEC_SAMPLE_RATE, (size_t)AEC_FRAME_SAMPLES, g_acoustic_delay_ms, g_config.mobile_mode,
            g_config.enable_noise_suppression);

   return 0;
}

void aec_cleanup(void) {
   // Mark as not initialized first to stop processing
   g_initialized.store(false);

   // Wait for any in-progress processing to complete
   std::lock_guard<std::mutex> lock(g_aec_mutex);

   if (g_apm) {
      delete g_apm;
      g_apm = nullptr;
   }

   // Note: No reference resampler to clean up - TTS sends 48kHz directly

   if (g_ref_buffer) {
      LOG_INFO("AEC buffer stats: read=%llu, empty=%llu",
               (unsigned long long)g_ref_buffer->get_frames_read(),
               (unsigned long long)g_ref_buffer->get_frames_empty());
      delete g_ref_buffer;
      g_ref_buffer = nullptr;
   }

   LOG_INFO("AEC cleaned up (processed: %llu frames, passed through: %llu frames)",
            (unsigned long long)g_frames_processed.load(),
            (unsigned long long)g_frames_passed_through.load());
}

bool aec_is_enabled(void) {
   return g_initialized.load() && g_active.load();
}

// Note: TTS module already resamples 22050Hz → 48kHz before calling these functions
// Reference audio arrives at native 48kHz - no resampling needed here

void aec_add_reference(const int16_t *samples, size_t num_samples) {
   // Quick checks without locking
   if (!g_initialized.load() || !g_active.load()) {
      return;
   }
   if (!samples || num_samples == 0 || !g_ref_buffer) {
      return;
   }

   // Input is already 48kHz from TTS (TTS resamples 22050→48kHz)
   // Write directly to reference buffer
   g_ref_buffer->write(samples, num_samples);
}

void aec_add_reference_with_delay(const int16_t *samples,
                                  size_t num_samples,
                                  uint64_t playback_delay_us) {
   // Quick checks without locking
   if (!g_initialized.load() || !g_active.load()) {
      return;
   }
   if (!samples || num_samples == 0 || !g_ref_buffer) {
      return;
   }

   // Input is already 48kHz from TTS (TTS resamples 22050→48kHz)
   // Write directly to reference buffer
   // Note: playback_delay_us is ignored in current delay line implementation
   g_ref_buffer->write_with_delay(samples, num_samples, playback_delay_us);
}

/**
 * Static buffer for processing mic input at 48kHz
 * Since we now receive native 48kHz, we just need a working output buffer
 */
static int16_t g_mic_out[AEC_MAX_SAMPLES];

void aec_process(const int16_t *mic_in, int16_t *clean_out, size_t num_samples) {
   // Handle NULL output buffer - zero it to prevent undefined behavior
   if (!clean_out) {
      return;
   }

   // Handle NULL input or invalid size - output silence
   if (!mic_in || num_samples == 0) {
      if (num_samples > 0 && num_samples <= AEC_MAX_SAMPLES) {
         memset(clean_out, 0, num_samples * sizeof(int16_t));
      }
      return;
   }

   // Validate sample count
   if (num_samples > AEC_MAX_SAMPLES) {
      LOG_ERROR("AEC input too large: %zu > %d", num_samples, AEC_MAX_SAMPLES);
      // Output silence for safety
      memset(clean_out, 0, AEC_MAX_SAMPLES * sizeof(int16_t));
      return;
   }

   // Check if AEC is available
   if (!g_initialized.load() || !g_active.load()) {
      // Pass through if AEC not available
      memcpy(clean_out, mic_in, num_samples * sizeof(int16_t));
      return;
   }

   auto frame_start = std::chrono::high_resolution_clock::now();

   // =========================================================================
   // Process in 480-sample frames (10ms at 48kHz) - NO resampling needed
   // Input is already 48kHz from native capture
   // =========================================================================
   webrtc::StreamConfig stream_config(AEC_SAMPLE_RATE, 1);  // 48kHz, 1 channel
   size_t processed = 0;

   while (processed < num_samples) {
      size_t chunk = num_samples - processed;
      if (chunk > AEC_FRAME_SAMPLES) {
         chunk = AEC_FRAME_SAMPLES;
      }

      // Copy mic chunk to frame buffer
      memcpy(g_mic_frame, mic_in + processed, chunk * sizeof(int16_t));

      // Pad with zeros if partial frame
      if (chunk < AEC_FRAME_SAMPLES) {
         memset(g_mic_frame + chunk, 0, (AEC_FRAME_SAMPLES - chunk) * sizeof(int16_t));
      }

      // Get reference audio from delay line buffer (at 48kHz)
      bool has_reference = false;
      if (g_ref_buffer) {
         has_reference = g_ref_buffer->read_frame(g_ref_frame);
      }

      if (!has_reference) {
         // Buffer empty - feed silence (AEC3 will adapt)
         memset(g_ref_frame, 0, AEC_FRAME_SAMPLES * sizeof(int16_t));
         g_frames_passed_through.fetch_add(1);
      }

      // Lock only for WebRTC API calls
      bool frame_success = false;
      {
         std::lock_guard<std::mutex> lock(g_aec_mutex);

         if (!g_apm) {
            // AEC was cleaned up while we were processing
            memcpy(g_mic_out + processed, mic_in + processed, chunk * sizeof(int16_t));
            processed += chunk;
            continue;
         }

         // Feed reference signal (render/playback/far-end) at 48kHz
         int16_t *ref_ptr = g_ref_frame;
         int reverse_result = g_apm->ProcessReverseStream(ref_ptr, stream_config, stream_config,
                                                          ref_ptr);

         // Set stream delay hint
         g_apm->set_stream_delay_ms(g_acoustic_delay_ms);

         // Process capture stream (microphone/near-end) at 48kHz
         int16_t *mic_ptr = g_mic_frame;
         int stream_result = g_apm->ProcessStream(mic_ptr, stream_config, stream_config, mic_ptr);

         frame_success = (reverse_result == 0 && stream_result == 0);

         // Log AEC3 internal stats periodically
         static uint64_t log_frame_counter = 0;
         log_frame_counter++;
         if (log_frame_counter % 500 == 0) {
            webrtc::AudioProcessingStats apm_stats = g_apm->GetStatistics();
            float erl = apm_stats.echo_return_loss.has_value()
                            ? (float)apm_stats.echo_return_loss.value()
                            : -999.0f;
            float erle = apm_stats.echo_return_loss_enhancement.has_value()
                             ? (float)apm_stats.echo_return_loss_enhancement.value()
                             : -999.0f;
            int delay = apm_stats.delay_ms.has_value() ? apm_stats.delay_ms.value() : -1;

            // Calculate RMS for this 48kHz frame
            int64_t in_sum = 0, out_sum = 0, ref_sum = 0;
            for (size_t i = 0; i < chunk; i++) {
               in_sum += (int64_t)mic_in[processed + i] * mic_in[processed + i];
               out_sum += (int64_t)g_mic_frame[i] * g_mic_frame[i];
               ref_sum += (int64_t)g_ref_frame[i] * g_ref_frame[i];
            }
            double in_rms = sqrt((double)in_sum / chunk);
            double out_rms = sqrt((double)out_sum / chunk);
            double ref_rms = sqrt((double)ref_sum / chunk);

            // Calculate actual attenuation when both ref and mic have signal
            float attenuation_db = 0;
            if (in_rms > 10 && ref_rms > 10) {
               attenuation_db = 20.0f * log10f((float)out_rms / (float)in_rms);
            }

            // Get buffer stats for logging
            size_t buf_frames = g_ref_buffer ? g_ref_buffer->get_frame_count() : 0;
            uint64_t read_count = g_ref_buffer ? g_ref_buffer->get_frames_read() : 0;
            uint64_t empty_count = g_ref_buffer ? g_ref_buffer->get_frames_empty() : 0;

            // Also check for divergent filter
            float divergent = apm_stats.divergent_filter_fraction.has_value()
                                  ? (float)apm_stats.divergent_filter_fraction.value()
                                  : 0.0f;

            LOG_INFO("AEC3@48k: ERL=%.1fdB ERLE=%.1fdB delay=%dms atten=%.1fdB div=%.2f "
                     "queued=%zu read=%llu empty=%llu "
                     "mic=%.0f ref=%.0f out=%.0f",
                     erl, erle, delay, attenuation_db, divergent, buf_frames,
                     (unsigned long long)read_count, (unsigned long long)empty_count, in_rms,
                     ref_rms, out_rms);
         }
      }

      if (frame_success) {
         // Copy processed frame to output buffer
         memcpy(g_mic_out + processed, g_mic_frame, chunk * sizeof(int16_t));
         g_consecutive_errors.store(0);
         g_frames_processed.fetch_add(1);
      } else {
         // On error, pass through unprocessed
         memcpy(g_mic_out + processed, mic_in + processed, chunk * sizeof(int16_t));

         int errors = g_consecutive_errors.fetch_add(1) + 1;
         if (errors == 1 || errors % 100 == 0) {
            LOG_WARNING("AEC ProcessStream failed (consecutive errors: %d)", errors);
         }

         if (errors >= AEC_MAX_CONSECUTIVE_ERRORS) {
            LOG_ERROR("AEC disabled after %d consecutive errors - call aec_reset() to re-enable",
                      errors);
            g_active.store(false);
         }
      }

      processed += chunk;
   }

   // Copy processed audio to output (no downsampling needed - capture thread does that)
   memcpy(clean_out, g_mic_out, num_samples * sizeof(int16_t));

   // Apply noise gate if configured
   if (g_config.noise_gate_threshold > 0) {
      int16_t threshold = g_config.noise_gate_threshold;
      for (size_t i = 0; i < num_samples; i++) {
         if (clean_out[i] > -threshold && clean_out[i] < threshold) {
            clean_out[i] = 0;
         }
      }
   }

   // Update performance tracking
   auto frame_end = std::chrono::high_resolution_clock::now();
   float total_us = std::chrono::duration<float, std::micro>(frame_end - frame_start).count();

   g_frame_count.fetch_add(1);
   float avg = g_avg_processing_time_us.load();
   g_avg_processing_time_us.store(avg * 0.99f + total_us * 0.01f);
}

int aec_get_stats(aec_stats_t *stats) {
   if (!stats) {
      return 1;
   }

   if (!g_initialized.load()) {
      memset(stats, 0, sizeof(aec_stats_t));
      return 1;
   }

   // Get buffer stats from FIFO buffer (at 48kHz)
   size_t ref_frames = g_ref_buffer ? g_ref_buffer->get_frame_count() : 0;
   // Report 48kHz sample count (native rate)
   size_t ref_samples = ref_frames * AEC_FRAME_SAMPLES;

   // Delay is estimated by AEC3 internally - we report 0 here
   stats->estimated_delay_ms = 0;
   stats->ref_buffer_samples = ref_samples;
   stats->consecutive_errors = g_consecutive_errors.load();
   stats->is_active = g_active.load();
   stats->avg_processing_time_us = g_avg_processing_time_us.load();
   stats->frames_processed = g_frames_processed.load();
   stats->frames_passed_through = g_frames_passed_through.load();

   // Get WebRTC statistics for ERLE and residual echo
   stats->erle_db = 0.0f;
   stats->residual_echo_likelihood = 0.0f;
   stats->metrics_valid = false;

   if (g_apm && g_active.load()) {
      std::lock_guard<std::mutex> lock(g_aec_mutex);
      if (g_apm) {
         webrtc::AudioProcessingStats apm_stats = g_apm->GetStatistics();

         if (apm_stats.echo_return_loss_enhancement.has_value()) {
            stats->erle_db = (float)apm_stats.echo_return_loss_enhancement.value();
            stats->metrics_valid = true;
         }

         if (apm_stats.residual_echo_likelihood.has_value()) {
            stats->residual_echo_likelihood = (float)apm_stats.residual_echo_likelihood.value();
            stats->metrics_valid = true;
         }

         // Log additional AEC3 diagnostics periodically
         static int log_counter = 0;
         if (++log_counter >= 500) {  // Every ~5 seconds at 100 calls/sec
            log_counter = 0;
            float erl = apm_stats.echo_return_loss.has_value()
                            ? (float)apm_stats.echo_return_loss.value()
                            : 0.0f;
            int delay = apm_stats.delay_ms.has_value() ? apm_stats.delay_ms.value() : -1;
            float divergent = apm_stats.divergent_filter_fraction.has_value()
                                  ? (float)apm_stats.divergent_filter_fraction.value()
                                  : 0.0f;
            LOG_INFO("AEC3 stats: ERL=%.1fdB ERLE=%.1fdB delay=%dms divergent=%.2f residual=%.2f",
                     erl, stats->erle_db, delay, divergent, stats->residual_echo_likelihood);
         }
      }
   }

   return 0;
}

bool aec_get_erle(float *erle_db) {
   if (!erle_db) {
      return false;
   }

   *erle_db = 0.0f;

   if (!g_initialized.load() || !g_active.load() || !g_apm) {
      return false;
   }

   std::lock_guard<std::mutex> lock(g_aec_mutex);
   if (!g_apm) {
      return false;
   }

   webrtc::AudioProcessingStats stats = g_apm->GetStatistics();
   if (stats.echo_return_loss_enhancement.has_value()) {
      *erle_db = (float)stats.echo_return_loss_enhancement.value();
      return true;
   }

   return false;
}

bool aec_get_residual_echo_likelihood(float *likelihood) {
   if (!likelihood) {
      return false;
   }

   *likelihood = 0.0f;

   if (!g_initialized.load() || !g_active.load() || !g_apm) {
      return false;
   }

   std::lock_guard<std::mutex> lock(g_aec_mutex);
   if (!g_apm) {
      return false;
   }

   webrtc::AudioProcessingStats stats = g_apm->GetStatistics();
   if (stats.residual_echo_likelihood.has_value()) {
      *likelihood = (float)stats.residual_echo_likelihood.value();
      return true;
   }

   return false;
}

void aec_reset(void) {
   if (!g_initialized.load()) {
      return;
   }

   std::lock_guard<std::mutex> lock(g_aec_mutex);

   // Clear reference buffer
   if (g_ref_buffer) {
      g_ref_buffer->clear();
   }

   // Reset error tracking and re-enable
   g_consecutive_errors.store(0);
   g_active.store(true);

   // Reset statistics
   g_frames_processed.store(0);
   g_frames_passed_through.store(0);
   g_avg_processing_time_us.store(0.0f);
   g_frame_count.store(0);

   // Note: WebRTC AEC3 state reset support varies by version
   // Some versions have Initialize() method, others don't expose reset

   LOG_INFO("AEC state reset - echo cancellation re-enabled");
}

}  // extern "C"
