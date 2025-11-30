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
#include <cstring>
#include <mutex>

// WebRTC audio processing includes
// Note: Using local build path structure
#include <webrtc/modules/audio_processing/include/audio_processing.h>

extern "C" {
#include "audio/ring_buffer.h"
#include "logging.h"
}

namespace {

// AEC state - using raw pointer as v1.3 API returns AudioProcessing*
webrtc::AudioProcessing *g_apm = nullptr;
std::mutex g_aec_mutex;  // Protects WebRTC API calls only
std::atomic<bool> g_initialized{ false };
std::atomic<bool> g_active{ true };  // Can be disabled on repeated errors

// Reference signal buffer (TTS output, resampled to 16kHz)
ring_buffer_t *g_ref_buffer = nullptr;

// Pre-allocated frame buffers (sized for single 10ms frame)
int16_t g_ref_frame[AEC_FRAME_SAMPLES];
int16_t g_mic_frame[AEC_FRAME_SAMPLES];

// Error tracking
std::atomic<int> g_consecutive_errors{ 0 };

// Performance tracking
std::atomic<float> g_avg_processing_time_us{ 0.0f };
std::atomic<int> g_frame_count{ 0 };
std::atomic<uint64_t> g_frames_processed{ 0 };
std::atomic<uint64_t> g_frames_passed_through{ 0 };

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
      .noise_gate_threshold = 0  // Disabled - VAD threshold approach is more effective
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

   std::lock_guard<std::mutex> lock(g_aec_mutex);

   if (g_initialized.load()) {
      LOG_WARNING("AEC already initialized");
      return 0;
   }

   // Create AudioProcessing instance using builder pattern
   webrtc::AudioProcessingBuilder builder;
   g_apm = builder.Create();

   if (!g_apm) {
      LOG_ERROR("Failed to create AudioProcessing instance");
      return 1;
   }

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

   // Calculate reference buffer size
   size_t ref_buffer_samples = (AEC_SAMPLE_RATE * g_config.ref_buffer_ms) / 1000;
   size_t ref_buffer_bytes = ref_buffer_samples * sizeof(int16_t);

   g_ref_buffer = ring_buffer_create(ref_buffer_bytes);
   if (!g_ref_buffer) {
      LOG_ERROR("Failed to create AEC reference buffer (%zu bytes)", ref_buffer_bytes);
      delete g_apm;
      g_apm = nullptr;
      return 1;
   }

   // Reset state
   g_consecutive_errors.store(0);
   g_avg_processing_time_us.store(0.0f);
   g_frame_count.store(0);
   g_frames_processed.store(0);
   g_frames_passed_through.store(0);
   g_active.store(true);
   g_initialized.store(true);

   LOG_INFO("AEC3 initialized: %d Hz, %d samples/frame, %zu ms ref buffer, mobile=%d, NS=%d, "
            "noise_gate=%d",
            AEC_SAMPLE_RATE, AEC_FRAME_SAMPLES, g_config.ref_buffer_ms, g_config.mobile_mode,
            g_config.enable_noise_suppression, g_config.noise_gate_threshold);

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

   if (g_ref_buffer) {
      ring_buffer_free(g_ref_buffer);
      g_ref_buffer = nullptr;
   }

   LOG_INFO("AEC cleaned up (processed: %llu frames, passed through: %llu frames)",
            (unsigned long long)g_frames_processed.load(),
            (unsigned long long)g_frames_passed_through.load());
}

bool aec_is_enabled(void) {
   return g_initialized.load() && g_active.load();
}

void aec_add_reference(const int16_t *samples, size_t num_samples) {
   // Quick checks without locking
   if (!g_initialized.load() || !g_active.load()) {
      return;
   }
   if (!samples || num_samples == 0) {
      return;
   }

   // ring_buffer_write is internally thread-safe, no additional locking needed
   // Note: If buffer overflows, oldest data is dropped (expected behavior)
   ring_buffer_write(g_ref_buffer, (const char *)samples, num_samples * sizeof(int16_t));
}

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

   // Process in AEC_FRAME_SAMPLES chunks (10ms frames)
   // Lock is acquired per-frame to minimize blocking
   size_t processed = 0;
   webrtc::StreamConfig stream_config(AEC_SAMPLE_RATE, 1);  // 16kHz, 1 channel

   while (processed < num_samples) {
      size_t chunk = num_samples - processed;
      if (chunk > AEC_FRAME_SAMPLES) {
         chunk = AEC_FRAME_SAMPLES;
      }

      auto frame_start = std::chrono::high_resolution_clock::now();

      // Copy mic input to frame buffer
      memcpy(g_mic_frame, mic_in + processed, chunk * sizeof(int16_t));

      // Pad with zeros if partial frame (last chunk may be smaller)
      if (chunk < AEC_FRAME_SAMPLES) {
         memset(g_mic_frame + chunk, 0, (AEC_FRAME_SAMPLES - chunk) * sizeof(int16_t));
      }

      // Get reference audio (lock-free read from ring buffer)
      size_t ref_available = ring_buffer_bytes_available(g_ref_buffer);
      bool has_reference = (ref_available >= AEC_FRAME_BYTES);

      if (has_reference) {
         ring_buffer_read(g_ref_buffer, (char *)g_ref_frame, AEC_FRAME_BYTES);
      } else {
         // No reference available - assume silence (no TTS playing)
         memset(g_ref_frame, 0, AEC_FRAME_BYTES);
         g_frames_passed_through.fetch_add(1);
      }

      // Lock only for WebRTC API calls (brief, ~1ms typically)
      bool frame_success = false;
      {
         std::lock_guard<std::mutex> lock(g_aec_mutex);

         if (!g_apm) {
            // AEC was cleaned up while we were processing
            memcpy(clean_out + processed, mic_in + processed, chunk * sizeof(int16_t));
            processed += chunk;
            continue;
         }

         // Feed reference signal (render/playback/far-end)
         int16_t *ref_ptr = g_ref_frame;
         int reverse_result = g_apm->ProcessReverseStream(ref_ptr, stream_config, stream_config,
                                                          ref_ptr);

         // Process capture stream (microphone/near-end)
         int16_t *mic_ptr = g_mic_frame;
         int stream_result = g_apm->ProcessStream(mic_ptr, stream_config, stream_config, mic_ptr);

         frame_success = (reverse_result == 0 && stream_result == 0);
      }

      if (frame_success) {
         // Copy processed audio to output
         memcpy(clean_out + processed, g_mic_frame, chunk * sizeof(int16_t));

         // Apply noise gate to eliminate residual echo
         // This gates low-amplitude samples that AEC couldn't fully cancel
         if (g_config.noise_gate_threshold > 0) {
            int16_t *out_ptr = clean_out + processed;
            int16_t threshold = g_config.noise_gate_threshold;
            for (size_t i = 0; i < chunk; i++) {
               if (out_ptr[i] > -threshold && out_ptr[i] < threshold) {
                  out_ptr[i] = 0;
               }
            }
         }

         // Reset error counter on success
         g_consecutive_errors.store(0);
         g_frames_processed.fetch_add(1);
      } else {
         // On error, pass through unprocessed audio
         memcpy(clean_out + processed, mic_in + processed, chunk * sizeof(int16_t));

         int errors = g_consecutive_errors.fetch_add(1) + 1;
         if (errors == 1 || errors % 100 == 0) {
            LOG_WARNING("AEC ProcessStream failed (consecutive errors: %d)", errors);
         }

         // Disable AEC after too many errors
         if (errors >= AEC_MAX_CONSECUTIVE_ERRORS) {
            LOG_ERROR("AEC disabled after %d consecutive errors - call aec_reset() to re-enable",
                      errors);
            g_active.store(false);
         }
      }

      // Update performance tracking
      auto frame_end = std::chrono::high_resolution_clock::now();
      float frame_us = std::chrono::duration<float, std::micro>(frame_end - frame_start).count();

      g_frame_count.fetch_add(1);
      float avg = g_avg_processing_time_us.load();
      // Exponential moving average
      g_avg_processing_time_us.store(avg * 0.99f + frame_us * 0.01f);

      processed += chunk;
   }
}

int aec_get_stats(aec_stats_t *stats) {
   if (!stats) {
      return 1;
   }

   if (!g_initialized.load()) {
      memset(stats, 0, sizeof(aec_stats_t));
      return 1;
   }

   // Calculate delay estimate from reference buffer level
   size_t ref_bytes = g_ref_buffer ? ring_buffer_bytes_available(g_ref_buffer) : 0;
   size_t ref_samples = ref_bytes / sizeof(int16_t);

   stats->estimated_delay_ms = (int)((ref_samples * 1000) / AEC_SAMPLE_RATE);
   stats->ref_buffer_samples = ref_samples;
   stats->consecutive_errors = g_consecutive_errors.load();
   stats->is_active = g_active.load();
   stats->avg_processing_time_us = g_avg_processing_time_us.load();
   stats->frames_processed = g_frames_processed.load();
   stats->frames_passed_through = g_frames_passed_through.load();

   return 0;
}

void aec_reset(void) {
   if (!g_initialized.load()) {
      return;
   }

   std::lock_guard<std::mutex> lock(g_aec_mutex);

   // Clear reference buffer
   if (g_ref_buffer) {
      ring_buffer_clear(g_ref_buffer);
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
