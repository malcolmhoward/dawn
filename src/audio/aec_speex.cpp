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
 * Speex AEC Processor Implementation
 *
 * Uses speex_echo_cancellation() with our own ring buffer to decouple
 * TTS burst writes from steady capture reads.
 *
 * Thread Model:
 * - TTS thread calls aec_add_reference() -> writes to ring buffer
 * - Capture thread calls aec_process() -> reads from ring buffer,
 *   pairs with mic frame, calls speex_echo_cancellation()
 *
 * The ring buffer allows TTS to generate audio in bursts while capture
 * consumes it at a steady rate. When no reference audio is available
 * (underflow), we pass zeros to Speex which effectively passes through
 * the microphone audio unchanged.
 */

#include <speex/speex_echo.h>
#include <speex/speex_preprocess.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <vector>

#include "audio/aec_processor.h"

extern "C" {
#include "logging.h"
}

namespace {

/**
 * WAV file header structure (44 bytes) for recording
 */
struct WavHeader {
   char riff[4] = { 'R', 'I', 'F', 'F' };
   uint32_t file_size = 0;
   char wave[4] = { 'W', 'A', 'V', 'E' };
   char fmt[4] = { 'f', 'm', 't', ' ' };
   uint32_t fmt_size = 16;
   uint16_t audio_format = 1;  // PCM
   uint16_t num_channels = 1;
   uint32_t sample_rate = AEC_SAMPLE_RATE;
   uint32_t byte_rate = AEC_SAMPLE_RATE * 2;
   uint16_t block_align = 2;
   uint16_t bits_per_sample = 16;
   char data[4] = { 'd', 'a', 't', 'a' };
   uint32_t data_size = 0;
};

/**
 * Simple WAV recorder for debugging
 */
class WavRecorder {
 public:
   WavRecorder() = default;
   ~WavRecorder() {
      close();
   }

   bool open(const char *filename) {
      std::lock_guard<std::mutex> lock(mutex_);
      if (file_) {
         fclose(file_);
      }

      file_ = fopen(filename, "wb");
      if (!file_) {
         return false;
      }

      WavHeader header;
      fwrite(&header, sizeof(header), 1, file_);
      samples_written_ = 0;
      return true;
   }

   void write(const int16_t *samples, size_t num_samples) {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!file_ || !samples || num_samples == 0)
         return;

      fwrite(samples, sizeof(int16_t), num_samples, file_);
      samples_written_ += num_samples;
   }

   void close() {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!file_)
         return;

      uint32_t data_size = samples_written_ * sizeof(int16_t);
      uint32_t file_size = data_size + sizeof(WavHeader) - 8;

      fseek(file_, 4, SEEK_SET);
      fwrite(&file_size, sizeof(file_size), 1, file_);

      fseek(file_, 40, SEEK_SET);
      fwrite(&data_size, sizeof(data_size), 1, file_);

      fclose(file_);
      file_ = nullptr;
   }

   bool is_open() const {
      std::lock_guard<std::mutex> lock(mutex_);
      return file_ != nullptr;
   }

   size_t get_samples_written() const {
      return samples_written_;
   }

 private:
   mutable std::mutex mutex_;
   FILE *file_ = nullptr;
   size_t samples_written_ = 0;
};

// Speex AEC state
SpeexEchoState *g_echo_state = nullptr;
SpeexPreprocessState *g_preprocess_state = nullptr;
std::mutex g_aec_mutex;  // Protects Speex API calls
std::atomic<bool> g_initialized{ false };
std::atomic<bool> g_active{ true };

// Frame buffers
int16_t g_ref_frame[AEC_FRAME_SAMPLES];
int16_t g_mic_frame[AEC_FRAME_SAMPLES];
int16_t g_out_frame[AEC_FRAME_SAMPLES];

// Timestamped reference frame for synchronized AEC
// Each entry holds one frame of audio with its expected playback time
struct TimestampedFrame {
   int16_t samples[AEC_FRAME_SAMPLES];
   uint64_t pts_us;  // Presentation timestamp in microseconds
   bool valid;
};

// Circular buffer of timestamped frames
// Size: ~2 seconds of audio (200 frames at 10ms each)
// Need larger buffer because TTS bursts audio faster than real-time
constexpr size_t PTS_BUFFER_FRAMES = 200;
TimestampedFrame g_pts_buffer[PTS_BUFFER_FRAMES];
std::atomic<size_t> g_pts_write_idx{ 0 };
std::atomic<size_t> g_pts_read_idx{ 0 };
std::mutex g_pts_mutex;  // Protects PTS buffer operations

// Simple ring buffer for accumulating incoming samples until we have a full frame
int16_t g_accumulator[AEC_FRAME_SAMPLES];
size_t g_accumulator_count = 0;
uint64_t g_accumulator_pts = 0;

// Playback synchronization: track when TTS playback started
std::atomic<uint64_t> g_playback_start_time{ 0 };   // When first frame was written
std::atomic<uint64_t> g_playback_start_delay{ 0 };  // Initial ALSA buffer latency
std::atomic<bool> g_playback_active{ false };

// Tracking for when playback is active
std::atomic<uint64_t> g_playback_frames{ 0 };
std::atomic<uint64_t> g_capture_frames{ 0 };
std::atomic<uint64_t> g_pts_buffer_overflows{ 0 };
std::atomic<uint64_t> g_pts_buffer_underflows{ 0 };
std::atomic<uint64_t> g_pts_matches{ 0 };

// Get current time in microseconds (monotonic clock)
static uint64_t get_time_us() {
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

// Error tracking
std::atomic<int> g_consecutive_errors{ 0 };

// Performance tracking
std::atomic<float> g_avg_processing_time_us{ 0.0f };
std::atomic<uint64_t> g_frames_processed{ 0 };
std::atomic<uint64_t> g_frames_passed_through{ 0 };

// Configuration
aec_config_t g_config;

// Recording state
std::atomic<bool> g_recording_enabled{ false };
std::atomic<bool> g_recording_active{ false };
WavRecorder g_mic_recorder;
WavRecorder g_ref_recorder;
WavRecorder g_out_recorder;
std::string g_recording_dir = "/tmp";
std::string g_current_session;

}  // anonymous namespace

extern "C" {

aec_config_t aec_get_default_config(void) {
   aec_config_t config = {
      .enable_noise_suppression = true,  // Speex NS works well
      .noise_suppression_level = AEC_NS_LEVEL_MODERATE,
      .enable_high_pass_filter = true,
      .mobile_mode = false,       // Not used by Speex
      .ref_buffer_ms = 500,       // Not used - Speex has internal buffer
      .noise_gate_threshold = 0,  // Disabled
      .acoustic_delay_ms = 70     // Hint for filter length
   };
   return config;
}

int aec_init(const aec_config_t *config) {
   if (config) {
      g_config = *config;
   } else {
      g_config = aec_get_default_config();
   }

   std::lock_guard<std::mutex> lock(g_aec_mutex);

   if (g_initialized.load()) {
      LOG_WARNING("AEC already initialized");
      return 0;
   }

   // Calculate filter length from acoustic delay hint
   // Speex recommends filter_length = 100-500ms worth of samples
   // Use the maximum for best echo cancellation in varied acoustic environments
   size_t filter_length_ms = 500;  // 500ms = 24000 samples at 48kHz
   int filter_length = (AEC_SAMPLE_RATE * filter_length_ms) / 1000;

   // Create Speex echo canceller
   // frame_size = 480 samples (10ms at 48kHz)
   // filter_length = echo tail length in samples
   g_echo_state = speex_echo_state_init(AEC_FRAME_SAMPLES, filter_length);
   if (!g_echo_state) {
      LOG_ERROR("Failed to create Speex echo canceller");
      return 1;
   }

   // Set sample rate
   int sample_rate = AEC_SAMPLE_RATE;
   speex_echo_ctl(g_echo_state, SPEEX_ECHO_SET_SAMPLING_RATE, &sample_rate);

   LOG_INFO("Speex AEC: frame_size=%d, filter_length=%d (%zums), sample_rate=%d", AEC_FRAME_SAMPLES,
            filter_length, filter_length_ms, sample_rate);
   LOG_INFO("Speex AEC: Using playback/capture API (2-frame internal buffer)");

   // Create preprocessor for residual echo suppression only
   // NOTE: Noise suppression disabled - it causes "underwater" distortion
   // The core Speex AEC should handle echo cancellation on its own
   if (g_config.enable_noise_suppression) {
      g_preprocess_state = speex_preprocess_state_init(AEC_FRAME_SAMPLES, AEC_SAMPLE_RATE);
      if (g_preprocess_state) {
         // Link preprocessor to echo canceller for residual echo suppression
         speex_preprocess_ctl(g_preprocess_state, SPEEX_PREPROCESS_SET_ECHO_STATE, g_echo_state);

         // DISABLE noise suppression - it causes underwater distortion
         int denoise = 0;
         speex_preprocess_ctl(g_preprocess_state, SPEEX_PREPROCESS_SET_DENOISE, &denoise);

         // DISABLE AGC - let gain be natural
         int agc = 0;
         speex_preprocess_ctl(g_preprocess_state, SPEEX_PREPROCESS_SET_AGC, &agc);

         // DISABLE VAD in preprocessor
         int vad = 0;
         speex_preprocess_ctl(g_preprocess_state, SPEEX_PREPROCESS_SET_VAD, &vad);

         // Use MILD residual echo suppression only
         // -40dB is the Speex default, -60 was too aggressive
         int echo_suppress = -40;         // dB (Speex default)
         int echo_suppress_active = -15;  // dB during active echo (Speex default)
         speex_preprocess_ctl(g_preprocess_state, SPEEX_PREPROCESS_SET_ECHO_SUPPRESS,
                              &echo_suppress);
         speex_preprocess_ctl(g_preprocess_state, SPEEX_PREPROCESS_SET_ECHO_SUPPRESS_ACTIVE,
                              &echo_suppress_active);

         LOG_INFO("Speex preprocessor: denoise=OFF, AGC=OFF, echo_suppress=%ddB/%ddB",
                  echo_suppress, echo_suppress_active);
      }
   }

   // Reset state
   g_consecutive_errors.store(0);
   g_avg_processing_time_us.store(0.0f);
   g_frames_processed.store(0);
   g_frames_passed_through.store(0);
   g_playback_frames.store(0);
   g_capture_frames.store(0);
   g_active.store(true);
   g_initialized.store(true);

   LOG_INFO("Speex AEC initialized: %dHz, NS=%d", AEC_SAMPLE_RATE,
            g_config.enable_noise_suppression);

   return 0;
}

void aec_cleanup(void) {
   g_initialized.store(false);

   std::lock_guard<std::mutex> lock(g_aec_mutex);

   if (g_preprocess_state) {
      speex_preprocess_state_destroy(g_preprocess_state);
      g_preprocess_state = nullptr;
   }

   if (g_echo_state) {
      speex_echo_state_destroy(g_echo_state);
      g_echo_state = nullptr;
   }

   LOG_INFO("Speex AEC cleaned up (processed: %llu frames, passed through: %llu frames)",
            (unsigned long long)g_frames_processed.load(),
            (unsigned long long)g_frames_passed_through.load());
   LOG_INFO("  Playback frames: %llu, Capture frames: %llu",
            (unsigned long long)g_playback_frames.load(),
            (unsigned long long)g_capture_frames.load());
}

bool aec_is_enabled(void) {
   return g_initialized.load() && g_active.load();
}

/**
 * Helper to store a complete frame with its PTS in the buffer.
 */
static void store_pts_frame(const int16_t *frame, uint64_t pts_us) {
   std::lock_guard<std::mutex> lock(g_pts_mutex);

   size_t write_idx = g_pts_write_idx.load();
   size_t read_idx = g_pts_read_idx.load();

   // Check if buffer is full
   size_t next_write = (write_idx + 1) % PTS_BUFFER_FRAMES;
   if (next_write == read_idx) {
      // Buffer full - drop oldest frame
      g_pts_buffer_overflows.fetch_add(1);
      g_pts_read_idx.store((read_idx + 1) % PTS_BUFFER_FRAMES);
   }

   // Store frame with timestamp
   memcpy(g_pts_buffer[write_idx].samples, frame, AEC_FRAME_SAMPLES * sizeof(int16_t));
   g_pts_buffer[write_idx].pts_us = pts_us;
   g_pts_buffer[write_idx].valid = true;

   g_pts_write_idx.store((write_idx + 1) % PTS_BUFFER_FRAMES);
   g_playback_frames.fetch_add(1);
}

/**
 * Feed reference audio from TTS playback thread with delay information.
 *
 * Stores audio with presentation timestamp (PTS) = current_time + playback_delay.
 * The capture thread will retrieve frames when current time matches the PTS.
 */
void aec_add_reference_with_delay(const int16_t *samples,
                                  size_t num_samples,
                                  uint64_t playback_delay_us) {
   if (!g_initialized.load() || !g_active.load()) {
      return;
   }
   if (!samples || num_samples == 0) {
      return;
   }

   // Record reference audio if recording is active
   if (g_recording_active.load()) {
      g_ref_recorder.write(samples, num_samples);
   }

   // Track playback start for synchronization
   uint64_t now = get_time_us();

   // On first frame of a new playback session, record the start time
   if (!g_playback_active.load()) {
      g_playback_start_time.store(now);
      g_playback_start_delay.store(playback_delay_us);
      g_playback_active.store(true);
      LOG_INFO("AEC: Playback started (delay=%llums)",
               (unsigned long long)(playback_delay_us / 1000));
   }

   // Log timing info occasionally
   static uint64_t add_ref_count = 0;
   add_ref_count++;
   if (add_ref_count % 100 == 1) {
      LOG_INFO("AEC ref: delay=%llums samples=%zu", (unsigned long long)(playback_delay_us / 1000),
               num_samples);
   }

   // PTS not used in FIFO mode, but keep accumulator logic
   uint64_t base_pts = now;

   // Accumulate samples and emit full frames with PTS
   size_t offset = 0;
   while (offset < num_samples) {
      // If accumulator is empty, set its PTS
      if (g_accumulator_count == 0) {
         // Calculate PTS based on how far into the chunk we are
         // Each sample is 1/48000 seconds = ~20.83 microseconds
         uint64_t sample_offset_us = (offset * 1000000ULL) / AEC_SAMPLE_RATE;
         g_accumulator_pts = base_pts + sample_offset_us;
      }

      // Fill accumulator
      size_t to_copy = num_samples - offset;
      size_t space = AEC_FRAME_SAMPLES - g_accumulator_count;
      if (to_copy > space) {
         to_copy = space;
      }

      memcpy(g_accumulator + g_accumulator_count, samples + offset, to_copy * sizeof(int16_t));
      g_accumulator_count += to_copy;
      offset += to_copy;

      // If we have a full frame, store it with PTS
      if (g_accumulator_count >= AEC_FRAME_SAMPLES) {
         store_pts_frame(g_accumulator, g_accumulator_pts);
         g_accumulator_count = 0;
      }
   }
}

/**
 * Feed reference audio from TTS playback thread (no delay info).
 * Uses a default delay estimate.
 */
void aec_add_reference(const int16_t *samples, size_t num_samples) {
   // Default delay estimate: 50ms (typical ALSA buffer latency)
   aec_add_reference_with_delay(samples, num_samples, 50000);
}

/**
 * Helper to read a frame from the reference buffer.
 *
 * Delayed FIFO approach:
 * 1. TTS writes frames to buffer in burst mode (faster than real-time)
 * 2. We wait until playback_start_time + playback_delay before reading
 * 3. Then consume frames in FIFO order, matching capture rate
 *
 * Returns true if a frame was available, false if not ready or empty.
 */
static bool read_ref_frame(int16_t *frame_out) {
   std::lock_guard<std::mutex> lock(g_pts_mutex);

   size_t write_idx = g_pts_write_idx.load();
   size_t read_idx = g_pts_read_idx.load();

   // Buffer empty?
   if (read_idx == write_idx) {
      memset(frame_out, 0, AEC_FRAME_SAMPLES * sizeof(int16_t));
      // Only count as underflow if playback is active
      if (g_playback_active.load()) {
         g_pts_buffer_underflows.fetch_add(1);
      }
      return false;
   }

   // Check if playback has actually started yet
   if (!g_playback_active.load()) {
      memset(frame_out, 0, AEC_FRAME_SAMPLES * sizeof(int16_t));
      return false;
   }

   // Wait for initial playback delay before consuming frames
   uint64_t now = get_time_us();
   uint64_t start_time = g_playback_start_time.load();
   uint64_t start_delay = g_playback_start_delay.load();

   if (start_time > 0 && now < start_time + start_delay) {
      // Still waiting for playback to reach speaker
      memset(frame_out, 0, AEC_FRAME_SAMPLES * sizeof(int16_t));

      static uint64_t wait_count = 0;
      wait_count++;
      if (wait_count % 100 == 1) {
         int64_t wait_ms = (int64_t)(start_time + start_delay - now) / 1000;
         LOG_INFO("AEC: Waiting for playback start (%lldms remaining)", (long long)wait_ms);
      }
      return false;
   }

   // Get the oldest frame
   TimestampedFrame &frame = g_pts_buffer[read_idx];

   if (!frame.valid) {
      memset(frame_out, 0, AEC_FRAME_SAMPLES * sizeof(int16_t));
      g_pts_buffer_underflows.fetch_add(1);
      return false;
   }

   // FIFO: take the next frame in order
   memcpy(frame_out, frame.samples, AEC_FRAME_SAMPLES * sizeof(int16_t));
   frame.valid = false;
   g_pts_read_idx.store((read_idx + 1) % PTS_BUFFER_FRAMES);
   g_pts_matches.fetch_add(1);

   // Log queue depth occasionally
   static uint64_t read_count = 0;
   read_count++;
   if (read_count % 500 == 1) {
      size_t queue_depth = (write_idx >= read_idx) ? (write_idx - read_idx)
                                                   : (PTS_BUFFER_FRAMES - read_idx + write_idx);
      LOG_INFO("AEC: FIFO read (queue=%zu frames)", queue_depth);
   }

   return true;
}

/**
 * Process microphone audio from capture thread.
 *
 * Reads reference audio from ring buffer and calls speex_echo_cancellation()
 * with synchronized frame pairs.
 */
void aec_process(const int16_t *mic_in, int16_t *clean_out, size_t num_samples) {
   if (!clean_out) {
      return;
   }

   if (!mic_in || num_samples == 0) {
      if (num_samples > 0 && num_samples <= AEC_MAX_SAMPLES) {
         memset(clean_out, 0, num_samples * sizeof(int16_t));
      }
      return;
   }

   if (num_samples > AEC_MAX_SAMPLES) {
      LOG_ERROR("AEC input too large: %zu > %d", num_samples, AEC_MAX_SAMPLES);
      memset(clean_out, 0, AEC_MAX_SAMPLES * sizeof(int16_t));
      return;
   }

   if (!g_initialized.load() || !g_active.load()) {
      memcpy(clean_out, mic_in, num_samples * sizeof(int16_t));
      return;
   }

   auto frame_start = std::chrono::high_resolution_clock::now();

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

      // Get reference frame from ring buffer
      bool has_reference = read_ref_frame(g_ref_frame);

      // Process with Speex AEC
      bool frame_success = false;
      {
         std::lock_guard<std::mutex> lock(g_aec_mutex);

         if (!g_echo_state) {
            memcpy(clean_out + processed, mic_in + processed, chunk * sizeof(int16_t));
            processed += chunk;
            continue;
         }

         // Use speex_echo_cancellation() with synchronized frame pairs
         // This is the correct API when we control both ref and mic timing
         speex_echo_cancellation(g_echo_state, g_mic_frame, g_ref_frame, g_out_frame);
         g_capture_frames.fetch_add(1);

         // Apply preprocessor for noise suppression and residual echo suppression
         if (g_preprocess_state) {
            speex_preprocess_run(g_preprocess_state, g_out_frame);
         }

         frame_success = true;

         // Log stats periodically
         static uint64_t log_counter = 0;
         log_counter++;
         if (log_counter % 500 == 0) {
            // Calculate RMS for this frame
            int64_t in_sum = 0, out_sum = 0, ref_sum = 0;
            for (size_t i = 0; i < chunk; i++) {
               in_sum += (int64_t)g_mic_frame[i] * g_mic_frame[i];
               out_sum += (int64_t)g_out_frame[i] * g_out_frame[i];
               ref_sum += (int64_t)g_ref_frame[i] * g_ref_frame[i];
            }
            double in_rms = sqrt((double)in_sum / chunk);
            double out_rms = sqrt((double)out_sum / chunk);
            double ref_rms = sqrt((double)ref_sum / chunk);

            float attenuation_db = 0;
            if (in_rms > 10) {
               attenuation_db = 20.0f * log10f((float)out_rms / (float)in_rms);
            }

            uint64_t overflows = g_pts_buffer_overflows.load();
            uint64_t underflows = g_pts_buffer_underflows.load();
            uint64_t matches = g_pts_matches.load();

            LOG_INFO("SpeexAEC@48k: atten=%.1fdB ref=%.0f mic=%.0f out=%.0f "
                     "match=%llu over=%llu under=%llu",
                     attenuation_db, ref_rms, in_rms, out_rms, (unsigned long long)matches,
                     (unsigned long long)overflows, (unsigned long long)underflows);
         }
      }

      if (frame_success) {
         memcpy(clean_out + processed, g_out_frame, chunk * sizeof(int16_t));
         g_consecutive_errors.store(0);
         g_frames_processed.fetch_add(1);
      } else {
         memcpy(clean_out + processed, mic_in + processed, chunk * sizeof(int16_t));

         int errors = g_consecutive_errors.fetch_add(1) + 1;
         if (errors == 1 || errors % 100 == 0) {
            LOG_WARNING("Speex AEC failed (consecutive errors: %d)", errors);
         }

         if (errors >= AEC_MAX_CONSECUTIVE_ERRORS) {
            LOG_ERROR("AEC disabled after %d consecutive errors", errors);
            g_active.store(false);
         }
      }

      processed += chunk;
   }

   // Record mic input and AEC output if recording is active
   if (g_recording_active.load()) {
      g_mic_recorder.write(mic_in, num_samples);
      g_out_recorder.write(clean_out, num_samples);
   }

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

   stats->estimated_delay_ms = 0;  // Speex handles internally
   stats->ref_buffer_samples = 0;  // Not applicable - Speex internal buffer
   stats->consecutive_errors = g_consecutive_errors.load();
   stats->is_active = g_active.load();
   stats->avg_processing_time_us = g_avg_processing_time_us.load();
   stats->frames_processed = g_frames_processed.load();
   stats->frames_passed_through = g_frames_passed_through.load();

   // Speex doesn't expose ERLE metrics
   stats->erle_db = 0.0f;
   stats->residual_echo_likelihood = 0.0f;
   stats->metrics_valid = false;

   return 0;
}

bool aec_get_erle(float *erle_db) {
   if (erle_db) {
      *erle_db = 0.0f;
   }
   // Speex doesn't expose ERLE
   return false;
}

bool aec_get_residual_echo_likelihood(float *likelihood) {
   if (likelihood) {
      *likelihood = 0.0f;
   }
   // Speex doesn't expose this metric
   return false;
}

void aec_reset(void) {
   if (!g_initialized.load()) {
      return;
   }

   // Reset AEC state
   {
      std::lock_guard<std::mutex> lock(g_aec_mutex);
      if (g_echo_state) {
         speex_echo_state_reset(g_echo_state);
      }
   }

   // Reset PTS buffer and playback tracking
   {
      std::lock_guard<std::mutex> lock(g_pts_mutex);
      g_pts_write_idx.store(0);
      g_pts_read_idx.store(0);
      for (size_t i = 0; i < PTS_BUFFER_FRAMES; i++) {
         g_pts_buffer[i].valid = false;
      }
      g_accumulator_count = 0;
   }
   g_playback_active.store(false);
   g_playback_start_time.store(0);
   g_playback_start_delay.store(0);

   g_consecutive_errors.store(0);
   g_active.store(true);
   g_frames_processed.store(0);
   g_frames_passed_through.store(0);
   g_playback_frames.store(0);
   g_capture_frames.store(0);
   g_pts_buffer_overflows.store(0);
   g_pts_buffer_underflows.store(0);
   g_pts_matches.store(0);
   g_avg_processing_time_us.store(0.0f);

   LOG_INFO("Speex AEC state reset");
}

void aec_signal_playback_stop(void) {
   if (!g_initialized.load()) {
      return;
   }

   bool was_active = g_playback_active.exchange(false);
   if (was_active) {
      // Log stats from this playback session
      uint64_t matches = g_pts_matches.load();
      uint64_t underflows = g_pts_buffer_underflows.load();
      uint64_t overflows = g_pts_buffer_overflows.load();

      LOG_INFO("AEC: Playback stopped (match=%llu over=%llu under=%llu)",
               (unsigned long long)matches, (unsigned long long)overflows,
               (unsigned long long)underflows);

      // Clear playback timing for next session
      g_playback_start_time.store(0);
      g_playback_start_delay.store(0);
   }
}

// ============================================================================
// Audio Recording API
// ============================================================================

void aec_set_recording_dir(const char *dir) {
   if (dir) {
      g_recording_dir = dir;
      LOG_INFO("AEC recording directory set to: %s", dir);
   }
}

void aec_enable_recording(bool enable) {
   g_recording_enabled.store(enable);
   LOG_INFO("AEC recording %s", enable ? "enabled" : "disabled");

   if (!enable && g_recording_active.load()) {
      aec_stop_recording();
   }
}

bool aec_is_recording(void) {
   return g_recording_active.load();
}

int aec_start_recording(void) {
   if (!g_recording_enabled.load()) {
      LOG_WARNING("AEC recording not enabled");
      return 1;
   }

   if (g_recording_active.load()) {
      LOG_WARNING("AEC recording already active");
      return 0;
   }

   time_t now = time(nullptr);
   struct tm *tm_info = localtime(&now);
   char timestamp[32];
   strftime(timestamp, sizeof(timestamp), "%Y%m%d_%H%M%S", tm_info);
   g_current_session = timestamp;

   std::string mic_file = g_recording_dir + "/aec_mic_" + g_current_session + ".wav";
   std::string ref_file = g_recording_dir + "/aec_ref_" + g_current_session + ".wav";
   std::string out_file = g_recording_dir + "/aec_out_" + g_current_session + ".wav";

   bool success = true;
   if (!g_mic_recorder.open(mic_file.c_str())) {
      LOG_ERROR("Failed to open mic recording: %s", mic_file.c_str());
      success = false;
   }
   if (!g_ref_recorder.open(ref_file.c_str())) {
      LOG_ERROR("Failed to open ref recording: %s", ref_file.c_str());
      success = false;
   }
   if (!g_out_recorder.open(out_file.c_str())) {
      LOG_ERROR("Failed to open out recording: %s", out_file.c_str());
      success = false;
   }

   if (!success) {
      g_mic_recorder.close();
      g_ref_recorder.close();
      g_out_recorder.close();
      return 1;
   }

   g_recording_active.store(true);
   LOG_INFO("AEC recording started: %s/aec_*_%s.wav", g_recording_dir.c_str(),
            g_current_session.c_str());

   return 0;
}

void aec_stop_recording(void) {
   if (!g_recording_active.load()) {
      return;
   }

   g_recording_active.store(false);

   size_t mic_samples = g_mic_recorder.get_samples_written();
   size_t ref_samples = g_ref_recorder.get_samples_written();
   size_t out_samples = g_out_recorder.get_samples_written();

   g_mic_recorder.close();
   g_ref_recorder.close();
   g_out_recorder.close();

   float mic_secs = (float)mic_samples / AEC_SAMPLE_RATE;
   float ref_secs = (float)ref_samples / AEC_SAMPLE_RATE;
   float out_secs = (float)out_samples / AEC_SAMPLE_RATE;

   LOG_INFO("AEC recording stopped: mic=%.2fs, ref=%.2fs, out=%.2fs", mic_secs, ref_secs, out_secs);
   LOG_INFO("  Files: %s/aec_{mic,ref,out}_%s.wav", g_recording_dir.c_str(),
            g_current_session.c_str());
}

}  // extern "C"
