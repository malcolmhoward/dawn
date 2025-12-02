#include "tts/text_to_speech.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <atomic>
#include <queue>
#include <sstream>
#include <vector>

#include "audio/audio_capture_thread.h"
#include "dawn.h"
#include "logging.h"

#ifdef ENABLE_AEC
#include "audio/aec_processor.h"
#include "audio/resampler.h"

// Separate resamplers for thread safety:
// - g_tts_thread_resampler: Used by TTS playback thread to feed AEC reference
//
// NOTE: Network WAV generation (text_to_speech_to_wav) does NOT need to feed AEC reference
// because network clients (ESP32) play audio on THEIR speaker, not DAWN's local speaker.
// DAWN's microphone won't hear ESP32's output, so there's no echo to cancel.
//
// IMPORTANT: These must NOT be shared between threads - resampler_t is not thread-safe!
static resampler_t *g_tts_thread_resampler = nullptr;

// Pre-allocated resample buffer (one per resampler)
static int16_t g_tts_resample_buffer[RESAMPLER_MAX_SAMPLES];

// Rate-limited warning for resampler overflow (warn once per 60 seconds)
static time_t g_last_resample_warning = 0;

// Atomic sequence counter for detecting discard during unlocked audio write (TOCTOU protection).
// Incremented each time TTS is discarded. Used to detect if discard happened between
// releasing mutex and completing audio write.
static std::atomic<uint32_t> g_tts_discard_sequence{ 0 };
#endif
#include "network/dawn_wav_utils.h"
#include "text_to_command_nuevo.h"
#include "tts/piper.hpp"
#include "ui/metrics.h"

#define DEFAULT_RATE 22050
#define DEFAULT_CHANNELS 1
#define DEFAULT_FRAMES 2

#ifdef ALSA_DEVICE
#include <alsa/asoundlib.h>
#include <samplerate.h>

#define DEFAULT_ACCESS SND_PCM_ACCESS_RW_INTERLEAVED
#define DEFAULT_FORMAT SND_PCM_FORMAT_S16_LE

// Maximum conversion buffer size (10 seconds at 48kHz stereo)
#define TTS_CONV_BUFFER_SIZE (48000 * 2 * 10)

#else
#include <pulse/error.h>
#include <pulse/simple.h>

#define DEFAULT_PULSE_FORMAT PA_SAMPLE_S16LE
#endif

using namespace std;
using namespace piper;

typedef struct {
   PiperConfig config;
   Voice voice;
   int is_initialized;
   char pcm_capture_device[MAX_WORD_LENGTH + 1];
#ifdef ALSA_DEVICE
   snd_pcm_t *handle;
   snd_pcm_uframes_t frames;
   // Actual hardware parameters (may differ from defaults)
   snd_pcm_format_t hw_format;
   unsigned int hw_rate;
   unsigned int hw_channels;
   // Conversion state
   bool needs_conversion;
   SRC_STATE *resampler;
   double resample_ratio;
   // Conversion buffers
   float *resample_in;
   float *resample_out;
   uint8_t *convert_out;
   size_t convert_out_size;
#else
   pa_simple *pa_handle;
#endif
} TTS_Handle;

// Global TTS_Handle object
static TTS_Handle tts_handle;

#ifdef ALSA_DEVICE

/**
 * Convert S16_LE mono samples to hardware format
 * Handles: rate conversion, mono→stereo, S16→S24_3LE
 *
 * @param in_samples Input S16_LE mono samples at DEFAULT_RATE
 * @param num_in_samples Number of input samples
 * @param out_buffer Output buffer (must be pre-allocated)
 * @param out_buffer_size Size of output buffer in bytes
 * @return Number of frames written to output, or -1 on error
 */
static ssize_t tts_convert_audio(const int16_t *in_samples,
                                 size_t num_in_samples,
                                 uint8_t *out_buffer,
                                 size_t out_buffer_size) {
   if (!tts_handle.needs_conversion) {
      // No conversion needed - just copy
      size_t bytes = num_in_samples * sizeof(int16_t);
      if (bytes > out_buffer_size)
         bytes = out_buffer_size;
      memcpy(out_buffer, in_samples, bytes);
      return num_in_samples;
   }

   // Step 1: Convert S16 to float for resampler
   size_t max_in = num_in_samples;
   if (max_in > TTS_CONV_BUFFER_SIZE)
      max_in = TTS_CONV_BUFFER_SIZE;

   for (size_t i = 0; i < max_in; i++) {
      tts_handle.resample_in[i] = in_samples[i] / 32768.0f;
   }

   // Step 2: Resample if needed
   float *resampled_data = tts_handle.resample_in;
   size_t resampled_samples = max_in;

   if (tts_handle.resampler && tts_handle.resample_ratio != 1.0) {
      SRC_DATA src_data;
      src_data.data_in = tts_handle.resample_in;
      src_data.input_frames = (long)max_in;
      src_data.data_out = tts_handle.resample_out;
      src_data.output_frames = (long)(max_in * tts_handle.resample_ratio + 100);
      src_data.src_ratio = tts_handle.resample_ratio;
      src_data.end_of_input = 0;

      int err = src_process(tts_handle.resampler, &src_data);
      if (err) {
         LOG_ERROR("Resampler error: %s", src_strerror(err));
         return -1;
      }

      resampled_data = tts_handle.resample_out;
      resampled_samples = src_data.output_frames_gen;
   }

   // Step 3: Convert to hardware format
   size_t out_frames = resampled_samples;
   size_t bytes_per_frame;

   if (tts_handle.hw_format == SND_PCM_FORMAT_S24_3LE) {
      // 24-bit packed (3 bytes per sample)
      bytes_per_frame = 3 * tts_handle.hw_channels;
   } else if (tts_handle.hw_format == SND_PCM_FORMAT_S32_LE) {
      bytes_per_frame = 4 * tts_handle.hw_channels;
   } else {
      // S16_LE
      bytes_per_frame = 2 * tts_handle.hw_channels;
   }

   size_t needed_bytes = out_frames * bytes_per_frame;
   if (needed_bytes > out_buffer_size) {
      out_frames = out_buffer_size / bytes_per_frame;
   }

   uint8_t *out = out_buffer;

   for (size_t i = 0; i < out_frames; i++) {
      float sample = resampled_data[i];
      // Clamp
      if (sample > 1.0f)
         sample = 1.0f;
      if (sample < -1.0f)
         sample = -1.0f;

      if (tts_handle.hw_format == SND_PCM_FORMAT_S24_3LE) {
         // Convert float to 24-bit signed
         int32_t s24 = (int32_t)(sample * 8388607.0f);
         // Write for each channel (mono→stereo duplication)
         for (unsigned int ch = 0; ch < tts_handle.hw_channels; ch++) {
            out[0] = (uint8_t)(s24 & 0xFF);
            out[1] = (uint8_t)((s24 >> 8) & 0xFF);
            out[2] = (uint8_t)((s24 >> 16) & 0xFF);
            out += 3;
         }
      } else if (tts_handle.hw_format == SND_PCM_FORMAT_S32_LE) {
         int32_t s32 = (int32_t)(sample * 2147483647.0f);
         for (unsigned int ch = 0; ch < tts_handle.hw_channels; ch++) {
            memcpy(out, &s32, 4);
            out += 4;
         }
      } else {
         // S16_LE
         int16_t s16 = (int16_t)(sample * 32767.0f);
         for (unsigned int ch = 0; ch < tts_handle.hw_channels; ch++) {
            memcpy(out, &s16, 2);
            out += 2;
         }
      }
   }

   return (ssize_t)out_frames;
}

int openAlsaPcmPlaybackDevice(snd_pcm_t **handle, char *pcm_device, snd_pcm_uframes_t *frames) {
   snd_pcm_hw_params_t *params = NULL;
   int dir = 0;
   *frames = DEFAULT_FRAMES;
   int rc = 0;

   LOG_INFO("ALSA PLAYBACK DRIVER: %s", pcm_device);

   /* Open PCM device for playback. */
   rc = snd_pcm_open(handle, pcm_device, SND_PCM_STREAM_PLAYBACK, 0);
   if (rc < 0) {
      LOG_ERROR("unable to open pcm device for playback (%s): %s", pcm_device, snd_strerror(rc));
      return 1;
   }

   snd_pcm_hw_params_alloca(&params);

   rc = snd_pcm_hw_params_any(*handle, params);
   if (rc < 0) {
      LOG_ERROR("Unable to get hardware parameter structure: %s", snd_strerror(rc));
      snd_pcm_close(*handle);
      *handle = NULL;
      return 1;
   }

   rc = snd_pcm_hw_params_set_access(*handle, params, DEFAULT_ACCESS);
   if (rc < 0) {
      LOG_ERROR("Unable to set access type: %s", snd_strerror(rc));
      snd_pcm_close(*handle);
      *handle = NULL;
      return 1;
   }

   // Try formats in order of preference
   snd_pcm_format_t formats_to_try[] = {
      SND_PCM_FORMAT_S16_LE,   // Preferred (matches TTS output)
      SND_PCM_FORMAT_S24_3LE,  // ReSpeaker uses this
      SND_PCM_FORMAT_S32_LE,   // Some devices use this
   };
   const char *format_names[] = { "S16_LE", "S24_3LE", "S32_LE" };

   snd_pcm_format_t chosen_format = SND_PCM_FORMAT_UNKNOWN;
   for (int i = 0; i < 3; i++) {
      if (snd_pcm_hw_params_test_format(*handle, params, formats_to_try[i]) == 0) {
         rc = snd_pcm_hw_params_set_format(*handle, params, formats_to_try[i]);
         if (rc == 0) {
            chosen_format = formats_to_try[i];
            LOG_INFO("ALSA format: %s", format_names[i]);
            break;
         }
      }
   }

   if (chosen_format == SND_PCM_FORMAT_UNKNOWN) {
      LOG_ERROR("No compatible audio format found");
      snd_pcm_close(*handle);
      *handle = NULL;
      return 1;
   }
   tts_handle.hw_format = chosen_format;

   // Try channel counts
   unsigned int channels_to_try[] = { 1, 2 };
   unsigned int chosen_channels = 0;
   for (int i = 0; i < 2; i++) {
      if (snd_pcm_hw_params_test_channels(*handle, params, channels_to_try[i]) == 0) {
         rc = snd_pcm_hw_params_set_channels(*handle, params, channels_to_try[i]);
         if (rc == 0) {
            chosen_channels = channels_to_try[i];
            LOG_INFO("ALSA channels: %u", chosen_channels);
            break;
         }
      }
   }

   if (chosen_channels == 0) {
      LOG_ERROR("No compatible channel count found");
      snd_pcm_close(*handle);
      *handle = NULL;
      return 1;
   }
   tts_handle.hw_channels = chosen_channels;

   // Set sample rate - try exact match first, then nearest
   unsigned int rate = DEFAULT_RATE;
   rc = snd_pcm_hw_params_set_rate_near(*handle, params, &rate, &dir);
   if (rc < 0) {
      LOG_ERROR("Unable to set sample rate: %s", snd_strerror(rc));
      snd_pcm_close(*handle);
      *handle = NULL;
      return 1;
   }
   tts_handle.hw_rate = rate;
   LOG_INFO("ALSA rate: %u Hz (requested %d Hz)", rate, DEFAULT_RATE);

   // Set larger buffer to prevent underruns during conversion
   unsigned int buffer_time = 100000;  // 100ms
   rc = snd_pcm_hw_params_set_buffer_time_near(*handle, params, &buffer_time, &dir);
   if (rc < 0) {
      LOG_WARNING("Unable to set buffer time: %s", snd_strerror(rc));
   } else {
      LOG_INFO("ALSA buffer: %u us", buffer_time);
   }

   // Set period size
   snd_pcm_uframes_t period_size = rate / 50;  // 20ms periods
   rc = snd_pcm_hw_params_set_period_size_near(*handle, params, &period_size, &dir);
   if (rc < 0) {
      LOG_WARNING("Unable to set period size: %s", snd_strerror(rc));
   }
   *frames = period_size;
   LOG_INFO("ALSA period: %lu frames", period_size);

   rc = snd_pcm_hw_params(*handle, params);
   if (rc < 0) {
      LOG_ERROR("unable to set hw parameters: %s", snd_strerror(rc));
      snd_pcm_close(*handle);
      *handle = NULL;
      return 1;
   }

   // Determine if conversion is needed
   tts_handle.needs_conversion = (chosen_format != SND_PCM_FORMAT_S16_LE) ||
                                 (chosen_channels != DEFAULT_CHANNELS) || (rate != DEFAULT_RATE);

   if (tts_handle.needs_conversion) {
      LOG_INFO("Audio conversion enabled: %dHz/%dch/S16 → %uHz/%uch/%s", DEFAULT_RATE,
               DEFAULT_CHANNELS, rate, chosen_channels,
               chosen_format == SND_PCM_FORMAT_S24_3LE  ? "S24_3LE"
               : chosen_format == SND_PCM_FORMAT_S32_LE ? "S32_LE"
                                                        : "S16_LE");

      // Create resampler if rate differs
      tts_handle.resample_ratio = (double)rate / (double)DEFAULT_RATE;
      if (rate != DEFAULT_RATE) {
         int error;
         tts_handle.resampler = src_new(SRC_SINC_FASTEST, 1, &error);
         if (!tts_handle.resampler) {
            LOG_ERROR("Failed to create resampler: %s", src_strerror(error));
            snd_pcm_close(*handle);
            *handle = NULL;
            return 1;
         }
         LOG_INFO("Resampler created: ratio=%.4f", tts_handle.resample_ratio);
      }

      // Allocate conversion buffers
      tts_handle.resample_in = (float *)malloc(TTS_CONV_BUFFER_SIZE * sizeof(float));
      tts_handle.resample_out = (float *)malloc(TTS_CONV_BUFFER_SIZE * sizeof(float));
      // Output buffer: worst case is S32 stereo at 2x rate
      tts_handle.convert_out_size = TTS_CONV_BUFFER_SIZE * 8;
      tts_handle.convert_out = (uint8_t *)malloc(tts_handle.convert_out_size);

      if (!tts_handle.resample_in || !tts_handle.resample_out || !tts_handle.convert_out) {
         LOG_ERROR("Failed to allocate conversion buffers");
         snd_pcm_close(*handle);
         *handle = NULL;
         return 1;
      }
   } else {
      tts_handle.resampler = NULL;
      tts_handle.resample_in = NULL;
      tts_handle.resample_out = NULL;
      tts_handle.convert_out = NULL;
      LOG_INFO("No audio conversion needed");
   }

   return 0;
}
#else
pa_simple *openPulseaudioPlaybackDevice(char *pcm_playback_device) {
   static const pa_sample_spec ss = { .format = DEFAULT_PULSE_FORMAT,
                                      .rate = DEFAULT_RATE,
                                      .channels = DEFAULT_CHANNELS };

   int rc = 0;
   pa_simple *pa_handle = NULL;

   LOG_INFO("PULSEAUDIO PLAYBACK DRIVER: %s", pcm_playback_device);

   /* Create a new PulseAudio simple connection for playback. */
   pa_handle = pa_simple_new(NULL, APPLICATION_NAME, PA_STREAM_PLAYBACK, pcm_playback_device,
                             "playback", &ss, NULL, NULL, &rc);
   if (!pa_handle) {
      LOG_ERROR("PA simple error: %s", pa_strerror(rc));
      return NULL;
   }

   return pa_handle;
}
#endif

/**
 * @brief Thread-safe queue for text-to-speech requests.
 */
std::queue<std::string> tts_queue;

/**
 * @brief Mutex for synchronizing access to the text-to-speech queue.
 */
pthread_mutex_t tts_queue_mutex = PTHREAD_MUTEX_INITIALIZER;

/**
 * @brief Condition variable to signal the worker thread when new data is available.
 */
pthread_cond_t tts_queue_cond = PTHREAD_COND_INITIALIZER;

/**
 * @brief Thread handle for the text-to-speech worker thread.
 */
pthread_t tts_thread;

/**
 * @brief Flag indicating whether the worker thread should continue running.
 */
bool tts_thread_running = false;

/**
 * @brief TTS playback state (mutex-protected)
 *
 * All accesses must be protected by tts_mutex to ensure thread safety.
 * Changed from std::atomic to plain int to avoid C/C++ boundary issues.
 */
int tts_playback_state = TTS_PLAYBACK_IDLE;

extern "C" {
/**
 * @brief Worker thread function that processes text-to-speech requests.
 *
 * This function runs in a separate thread and continuously processes text strings
 * from the queue, converting them to speech and playing them back using the audio
 * system.
 *
 * @param arg Unused parameter.
 * @return Always returns NULL.
 */
void *tts_thread_function(void *arg) {
   LOG_INFO("tts_thread_function() started.");

   // Declare the interruption flag
   std::atomic<bool> tts_stop_processing(false);

   while (!get_quit()) {
      pthread_mutex_lock(&tts_queue_mutex);

      LOG_INFO("Waiting on text...");
      // Wait until there's text to process or the thread is signaled to exit
      while (tts_queue.empty() && tts_thread_running) {
         pthread_cond_wait(&tts_queue_cond, &tts_queue_mutex);
      }

      // Exit if the thread is no longer running and the queue is empty
      if (!tts_thread_running && tts_queue.empty()) {
         pthread_mutex_unlock(&tts_queue_mutex);
         break;
      }

      // Retrieve text from the queue
      std::string inputText = tts_queue.front();
      tts_queue.pop();
      pthread_mutex_unlock(&tts_queue_mutex);

      // Process text-to-speech
      SynthesisResult result;
      std::vector<int16_t> audioBuffer;
      int rc = 0;
      int error = 0;

      // Reset the interruption flag
      tts_stop_processing.store(false);

      // Convert text to audio data
      textToAudio(
          tts_handle.config, tts_handle.voice, inputText, audioBuffer, result, tts_stop_processing,
          [&]() {
             pthread_mutex_lock(&tts_mutex);
             tts_playback_state = TTS_PLAYBACK_PLAY;
#ifdef ENABLE_AEC
             // Start AEC recording for this TTS session (if enabled)
             if (aec_is_recording_enabled()) {
                aec_start_recording();
             }
#endif
             // Start mic recording for this TTS session (if enabled)
             if (mic_is_recording_enabled()) {
                mic_start_recording();
             }
             pthread_mutex_unlock(&tts_mutex);

#ifdef ALSA_DEVICE
             // Play audio data using ALSA
             for (size_t i = 0; i < audioBuffer.size(); i += tts_handle.frames) {
                // Check playback state
                pthread_mutex_lock(&tts_mutex);
                bool was_paused = false;
                while (tts_playback_state == TTS_PLAYBACK_PAUSE) {
                   if (!was_paused) {
                      LOG_WARNING("TTS playback is PAUSED.");
#ifdef ENABLE_AEC
                      // Signal AEC that playback is paused - stop underflow counting
                      aec_signal_playback_stop();
#endif
                      was_paused = true;
                   }
                   pthread_cond_wait(&tts_cond, &tts_mutex);
                }

                // Only log state transitions after being paused
                if (was_paused) {
                   if (tts_playback_state == TTS_PLAYBACK_DISCARD) {
                      LOG_WARNING("TTS unpaused to DISCARD.");
                      tts_playback_state = TTS_PLAYBACK_IDLE;
                      audioBuffer.clear();
                      LOG_WARNING("Emptying TTS queue.");
                      while (!tts_queue.empty()) {
                         tts_queue.pop();
                      }

#ifdef ENABLE_AEC
                      // Increment sequence counter to signal any in-flight playback
                      g_tts_discard_sequence.fetch_add(1, std::memory_order_release);

                      // Clear AEC reference buffer to prevent stale frames from
                      // contaminating future captures after discard
                      aec_reset();

                      // Stop AEC recording on discard (barge-in)
                      aec_stop_recording();
#endif
                      // Stop mic recording on discard (barge-in)
                      mic_stop_recording();

                      // Drop (flush) audio device buffer immediately to stop playback
                      snd_pcm_drop(tts_handle.handle);
                      snd_pcm_prepare(tts_handle.handle);

                      pthread_mutex_unlock(&tts_mutex);

                      tts_stop_processing.store(true);
                      return;
                   } else if (tts_playback_state == TTS_PLAYBACK_PLAY) {
                      LOG_WARNING("TTS unpaused to PLAY.");
                   } else if (tts_playback_state == TTS_PLAYBACK_IDLE) {
                      LOG_WARNING("TTS unpaused to IDLE.");
                   } else {
                      LOG_ERROR("TTS unpaused to UNKNOWN.");
                   }
                }

#ifdef ENABLE_AEC
                // Capture sequence counter before releasing mutex (TOCTOU protection)
                uint32_t seq_before_write = g_tts_discard_sequence.load(std::memory_order_acquire);

                // Store info needed for AEC reference (will feed AFTER snd_pcm_writei)
                size_t frames_to_write = std::min(tts_handle.frames, audioBuffer.size() - i);
                bool should_feed_aec = (tts_playback_state == TTS_PLAYBACK_PLAY &&
                                        g_tts_thread_resampler && aec_is_enabled());

                // Query ALSA delay BEFORE write - this tells us how much audio is
                // already queued up ahead of what we're about to write
                snd_pcm_sframes_t alsa_delay_before = 0;
                snd_pcm_delay(tts_handle.handle, &alsa_delay_before);
#endif
                pthread_mutex_unlock(&tts_mutex);

                // Write audio frames (blocking I/O - cannot hold mutex here)
                size_t samples_this_chunk = std::min(tts_handle.frames, audioBuffer.size() - i);

                if (tts_handle.needs_conversion) {
                   // Convert audio to hardware format (rate/channels/bit-depth)
                   ssize_t converted_frames = tts_convert_audio(&audioBuffer[i], samples_this_chunk,
                                                                tts_handle.convert_out,
                                                                tts_handle.convert_out_size);

                   if (converted_frames > 0) {
                      rc = snd_pcm_writei(tts_handle.handle, tts_handle.convert_out,
                                          converted_frames);
                   } else {
                      rc = -1;
                      LOG_ERROR("Audio conversion failed");
                   }
                } else {
                   // No conversion needed - write directly
                   rc = snd_pcm_writei(tts_handle.handle, &audioBuffer[i], samples_this_chunk);
                }

                if (rc == -EPIPE) {
                   LOG_ERROR("ALSA underrun occurred");
                   snd_pcm_prepare(tts_handle.handle);
                } else if (rc < 0) {
                   LOG_ERROR("ALSA error from writei: %s", snd_strerror(rc));
                }

#ifdef ENABLE_AEC
                // Feed AEC reference AFTER snd_pcm_writei() completes
                // Use the delay measured BEFORE write - this represents when the audio
                // we just wrote will start playing (after all currently queued audio)
                if (should_feed_aec && rc > 0) {
                   // The audio we just wrote will play AFTER the already-queued audio
                   // So: play_time = now + alsa_delay_before + time_for_this_chunk
                   // But for the reference buffer, we just need the delay to first sample
                   //
                   // alsa_delay_before = frames already in buffer waiting to play
                   // Convert to microseconds: frames * 1000000 / sample_rate
                   // DEFAULT_RATE is 22050 Hz (Piper TTS output rate)
                   //
                   // Use actual delay value - PTS buffer will hold frames until their
                   // scheduled echo arrival time
                   snd_pcm_sframes_t delay_frames = alsa_delay_before;
                   if (delay_frames < 0)
                      delay_frames = 0;

                   // For AEC timing, we only care about the acoustic delay from
                   // speaker to microphone, NOT the ALSA buffer depth.
                   //
                   // Why? Because we're calling aec_add_reference() RIGHT AFTER
                   // snd_pcm_writei() completes. At that moment, the audio is
                   // at the END of the ALSA buffer, about to play.
                   //
                   // The capture is running in real-time, so by the time this
                   // audio plays and reaches the mic, the capture will be at
                   // approximately: now + acoustic_delay
                   //
                   // ALSA buffer depth doesn't matter because we're adding
                   // reference audio immediately after writing - it will play
                   // in sequence regardless of buffer depth.
                   //
                   // Acoustic delay: speaker output -> microphone input
                   const uint64_t acoustic_delay_us = 50000;  // 50ms (tunable)

                   uint64_t playback_delay_us = acoustic_delay_us;
                   (void)delay_frames;  // Not used - see comment above

                   size_t in_samples = (size_t)rc;  // Actual samples written

                   // Enforce maximum chunk size
                   if (in_samples <= RESAMPLER_MAX_SAMPLES) {
                      size_t out_max = resampler_get_output_size(g_tts_thread_resampler,
                                                                 in_samples);

                      if (out_max <= RESAMPLER_MAX_SAMPLES) {
                         size_t resampled = resampler_process(g_tts_thread_resampler,
                                                              &audioBuffer[i], in_samples,
                                                              g_tts_resample_buffer,
                                                              RESAMPLER_MAX_SAMPLES);

                         if (resampled > 0) {
                            // Use delay-aware version for proper PTS calculation
                            aec_add_reference_with_delay(g_tts_resample_buffer, resampled,
                                                         playback_delay_us);
                         }
                      } else {
                         // Rate-limited warning (once per 60 seconds)
                         time_t now = time(NULL);
                         if (now - g_last_resample_warning >= 60) {
                            LOG_WARNING(
                                "AEC resampler output too large (%zu > %d), skipping reference",
                                out_max, RESAMPLER_MAX_SAMPLES);
                            g_last_resample_warning = now;
                         }
                      }
                   }
                }

                // Check if discard happened during audio write (TOCTOU detection)
                if (g_tts_discard_sequence.load(std::memory_order_acquire) != seq_before_write) {
                   // Discard occurred while we were writing - audio device already flushed
                   LOG_INFO("TTS discarded during ALSA write - exiting playback");
                   tts_stop_processing.store(true);
                   return;
                }
#endif
             }
#else
             const size_t chunk_frames = 1024;  // Adjust as needed
             const size_t chunk_bytes = chunk_frames * sizeof(int16_t);
             size_t total_bytes = audioBuffer.size() * sizeof(int16_t);

             for (size_t i = 0; i < total_bytes; i += chunk_bytes) {
                // Check playback state
                pthread_mutex_lock(&tts_mutex);
                bool was_paused = false;
                while (tts_playback_state == TTS_PLAYBACK_PAUSE) {
                   if (!was_paused) {
                      LOG_WARNING("TTS playback is PAUSED.");
#ifdef ENABLE_AEC
                      // Signal AEC that playback is paused - stop underflow counting
                      aec_signal_playback_stop();
#endif
                      was_paused = true;
                   }
                   pthread_cond_wait(&tts_cond, &tts_mutex);
                }

                // Only log state transitions after being paused
                if (was_paused) {
                   if (tts_playback_state == TTS_PLAYBACK_DISCARD) {
                      LOG_WARNING("TTS unpaused to DISCARD.");
                      tts_playback_state = TTS_PLAYBACK_IDLE;
                      audioBuffer.clear();
                      LOG_WARNING("Emptying TTS queue.");
                      while (!tts_queue.empty()) {
                         tts_queue.pop();
                      }

#ifdef ENABLE_AEC
                      // Increment sequence counter to signal any in-flight playback
                      g_tts_discard_sequence.fetch_add(1, std::memory_order_release);

                      // Clear AEC reference buffer to prevent stale frames from
                      // contaminating future captures after discard
                      aec_reset();

                      // Stop AEC recording on discard (barge-in)
                      aec_stop_recording();
#endif
                      // Stop mic recording on discard (barge-in)
                      mic_stop_recording();

                      // Flush PulseAudio buffer immediately to stop playback
                      int pa_error;
                      if (pa_simple_flush(tts_handle.pa_handle, &pa_error) < 0) {
                         LOG_ERROR("PulseAudio flush error: %s", pa_strerror(pa_error));
                      }

                      pthread_mutex_unlock(&tts_mutex);

                      tts_stop_processing.store(true);
                      return;
                   } else if (tts_playback_state == TTS_PLAYBACK_PLAY) {
                      LOG_WARNING("TTS unpaused to PLAY.");
                   } else if (tts_playback_state == TTS_PLAYBACK_IDLE) {
                      LOG_WARNING("TTS unpaused to IDLE.");
                   } else {
                      LOG_ERROR("TTS unpaused to UNKNOWN.");
                   }
                }

#ifdef ENABLE_AEC
                // Capture sequence counter before releasing mutex (TOCTOU protection)
                uint32_t seq_before_write = g_tts_discard_sequence.load(std::memory_order_acquire);

                // Store info needed for AEC reference (will feed AFTER pa_simple_write)
                size_t bytes_to_write_aec = std::min(chunk_bytes, total_bytes - i);
                bool should_feed_aec = (tts_playback_state == TTS_PLAYBACK_PLAY &&
                                        g_tts_thread_resampler && aec_is_enabled());
#endif
                pthread_mutex_unlock(&tts_mutex);

                // Calculate bytes to write
                size_t bytes_to_write = std::min(chunk_bytes, total_bytes - i);

                // Write audio data (blocking I/O - cannot hold mutex here)
                rc = pa_simple_write(tts_handle.pa_handle, ((uint8_t *)audioBuffer.data()) + i,
                                     bytes_to_write, &error);
                if (rc < 0) {
                   LOG_ERROR("PulseAudio error from pa_simple_write: %s", pa_strerror(error));
                   //audioBuffer.clear();

                   // Close the current PulseAudio connection
                   if (tts_handle.pa_handle) {
                      pa_simple_free(tts_handle.pa_handle);
                      tts_handle.pa_handle = NULL;
                   }

                   // Reopen the PulseAudio playback device
                   tts_handle.pa_handle = openPulseaudioPlaybackDevice(
                       tts_handle.pcm_capture_device);
                   if (tts_handle.pa_handle == NULL) {
                      LOG_ERROR("Error re-opening PulseAudio playback device.");
                   }
                }

#ifdef ENABLE_AEC
                // Feed AEC reference AFTER pa_simple_write() completes
                // Query PulseAudio latency to calculate when audio will actually play
                // Critical for correct AEC timing - reference must match actual playback time
                if (should_feed_aec && rc >= 0) {
                   // Query PulseAudio playback latency (microseconds until audio reaches speaker)
                   int pa_latency_error = 0;
                   pa_usec_t pa_latency = pa_simple_get_latency(tts_handle.pa_handle,
                                                                &pa_latency_error);
                   uint64_t playback_delay_us = (pa_latency_error >= 0 && pa_latency > 0)
                                                    ? (uint64_t)pa_latency
                                                    : 50000;  // Default 50ms if query fails

                   size_t in_samples = bytes_to_write_aec / sizeof(int16_t);

                   // Enforce maximum chunk size
                   if (in_samples <= RESAMPLER_MAX_SAMPLES) {
                      size_t out_max = resampler_get_output_size(g_tts_thread_resampler,
                                                                 in_samples);

                      if (out_max <= RESAMPLER_MAX_SAMPLES) {
                         size_t resampled = resampler_process(
                             g_tts_thread_resampler,
                             (int16_t *)(((uint8_t *)audioBuffer.data()) + i), in_samples,
                             g_tts_resample_buffer, RESAMPLER_MAX_SAMPLES);

                         if (resampled > 0) {
                            // Use delay-aware version for proper PTS calculation
                            aec_add_reference_with_delay(g_tts_resample_buffer, resampled,
                                                         playback_delay_us);
                         }
                      } else {
                         // Rate-limited warning (once per 60 seconds)
                         time_t now = time(NULL);
                         if (now - g_last_resample_warning >= 60) {
                            LOG_WARNING(
                                "AEC resampler output too large (%zu > %d), skipping reference",
                                out_max, RESAMPLER_MAX_SAMPLES);
                            g_last_resample_warning = now;
                         }
                      }
                   }
                }

                // Check if discard happened during audio write (TOCTOU detection)
                if (g_tts_discard_sequence.load(std::memory_order_acquire) != seq_before_write) {
                   // Discard occurred while we were writing - audio device already flushed
                   LOG_INFO("TTS discarded during PulseAudio write - exiting playback");
                   tts_stop_processing.store(true);
                   return;
                }
#endif
             }

         // Drain audio buffer to ensure all audio is played before returning
#ifdef ALSA_DEVICE
             snd_pcm_drain(tts_handle.handle);
#else
             if (pa_simple_drain(tts_handle.pa_handle, &error) < 0) {
                LOG_ERROR("PulseAudio drain error: %s", pa_strerror(error));
             }
#endif
#endif
             // Clear the audio buffer for the next request
             audioBuffer.clear();

             pthread_mutex_lock(&tts_mutex);
             tts_playback_state = TTS_PLAYBACK_IDLE;
#ifdef ENABLE_AEC
             // Signal AEC that playback has stopped - this prevents
             // underflow counting when no audio is playing
             aec_signal_playback_stop();

             // Stop AEC recording after TTS playback completes
             aec_stop_recording();
#endif
             // Stop mic recording after TTS playback completes
             mic_stop_recording();
             pthread_mutex_unlock(&tts_mutex);
          });

      // Record TTS timing metrics after synthesis completes
      if (result.inferSeconds > 0 && !tts_stop_processing.load()) {
         double tts_time_ms = result.inferSeconds * 1000.0;
         metrics_record_tts_timing(tts_time_ms);
      }

      // Check if queue is empty and LLM is not streaming - signal done speaking
      pthread_mutex_lock(&tts_queue_mutex);
      bool queue_empty = tts_queue.empty();
      pthread_mutex_unlock(&tts_queue_mutex);
      if (queue_empty && !tts_stop_processing.load() && !is_llm_processing()) {
         LOG_INFO("TTS complete - done speaking");
      }

      tts_stop_processing.store(false);
   }

   return NULL;
}

/**
 * @brief Initializes the text-to-speech system.
 *
 * This function loads the voice model, initializes the TTS engine, opens the
 * audio playback device, and starts the worker thread that processes TTS requests.
 *
 * @param pcm_device The name of the PCM device to use for audio playback.
 */
void initialize_text_to_speech(char *pcm_device) {
   // Check if already initialized
   if (tts_handle.is_initialized) {
      LOG_WARNING("Text-to-Speech system already initialized");
      return;
   }

   // Validate input
   if (!pcm_device) {
      LOG_ERROR("Invalid PCM device parameter");
      return;
   }

   LOG_INFO("Initializing Text-to-Speech system...");

   // Store device name (with bounds checking)
   strncpy(tts_handle.pcm_capture_device, pcm_device, MAX_WORD_LENGTH);
   tts_handle.pcm_capture_device[MAX_WORD_LENGTH] = '\0';

   // Load the voice model from models/ directory
   std::optional<SpeakerId> speakerIdOpt = 0;
   try {
      loadVoice(tts_handle.config, "../models/en_GB-alba-medium.onnx",
                "../models/en_GB-alba-medium.onnx.json", tts_handle.voice, speakerIdOpt, false);
   } catch (const std::exception &e) {
      LOG_ERROR("Failed to load voice model: %s", e.what());
      return;
   }

   // Initialize the TTS engine
   try {
      initialize(tts_handle.config);
   } catch (const std::exception &e) {
      LOG_ERROR("Failed to initialize TTS engine: %s", e.what());
      return;
   }

   // Configure synthesis parameters
   tts_handle.voice.synthesisConfig.lengthScale = 0.85f;

   // Initialize synchronization primitives
   pthread_mutex_init(&tts_queue_mutex, NULL);
   pthread_cond_init(&tts_queue_cond, NULL);

   // Open the audio playback device
#ifdef ALSA_DEVICE
   int rc = openAlsaPcmPlaybackDevice(&(tts_handle.handle), tts_handle.pcm_capture_device,
                                      &(tts_handle.frames));
   if (rc) {
      LOG_ERROR("Error creating ALSA playback device");
      // Cleanup Piper
      terminate(tts_handle.config);
      // Cleanup synchronization primitives
      pthread_mutex_destroy(&tts_queue_mutex);
      pthread_cond_destroy(&tts_queue_cond);
      return;
   }
#else
   tts_handle.pa_handle = openPulseaudioPlaybackDevice(tts_handle.pcm_capture_device);
   if (tts_handle.pa_handle == NULL) {
      LOG_ERROR("Error creating PulseAudio playback device");
      // Cleanup Piper
      terminate(tts_handle.config);
      // Cleanup synchronization primitives
      pthread_mutex_destroy(&tts_queue_mutex);
      pthread_cond_destroy(&tts_queue_cond);
      return;
   }
#endif

   // Start the worker thread
   tts_thread_running = true;
   int thread_result = pthread_create(&tts_thread, NULL, tts_thread_function, NULL);
   if (thread_result != 0) {
      LOG_ERROR("Failed to create TTS worker thread: %s", strerror(thread_result));
      tts_thread_running = false;

      // Cleanup audio device
#ifdef ALSA_DEVICE
      snd_pcm_close(tts_handle.handle);
      tts_handle.handle = NULL;
#else
      pa_simple_free(tts_handle.pa_handle);
      tts_handle.pa_handle = NULL;
#endif

      // Cleanup Piper
      terminate(tts_handle.config);

      // Cleanup synchronization primitives
      pthread_mutex_destroy(&tts_queue_mutex);
      pthread_cond_destroy(&tts_queue_cond);
      return;
   }

   // Only mark as initialized if everything succeeded
   tts_handle.is_initialized = 1;

#ifdef ENABLE_AEC
   // Create resampler for AEC reference (22050Hz TTS -> 48000Hz AEC native rate)
   g_tts_thread_resampler = resampler_create(DEFAULT_RATE, AEC_SAMPLE_RATE, 1);
   if (!g_tts_thread_resampler) {
      LOG_WARNING("Failed to create TTS resampler for AEC - echo cancellation may be limited");
   } else {
      LOG_INFO("TTS resampler initialized for AEC (%d -> %d Hz)", DEFAULT_RATE, AEC_SAMPLE_RATE);
   }
#endif

   LOG_INFO("Text-to-Speech system initialized successfully");
}

/**
 * @brief Enqueues a text string for conversion to speech.
 *
 * This function can be safely called from multiple threads. It adds the provided
 * text to a queue that is processed by a dedicated worker thread.
 *
 * @param text The text to be converted to speech.
 */
void text_to_speech(char *text) {
   if (!tts_handle.is_initialized) {
      LOG_ERROR("Text-to-Speech system not initialized. Call initialize_text_to_speech() first.");
      return;
   }

   assert(text != nullptr && "Received a null pointer");
   std::string inputText(text);

   // Preprocess: Replace em-dashes with commas for better TTS phrasing
   // Em-dash (—) doesn't create proper pauses in Piper, but commas do
   size_t pos = 0;
   while ((pos = inputText.find("—", pos)) != std::string::npos) {
      inputText.replace(pos, 3, ",");  // UTF-8 em-dash is 3 bytes
      pos += 1;
   }

   // Add text to the processing queue
   pthread_mutex_lock(&tts_queue_mutex);
   tts_queue.push(inputText);
   pthread_cond_signal(&tts_queue_cond);
   pthread_mutex_unlock(&tts_queue_mutex);
}

// Network WAV generation using existing TTS system
int text_to_speech_to_wav(const char *text, uint8_t **wav_data_out, size_t *wav_size_out) {
   if (!text || !wav_data_out || !wav_size_out) {
      LOG_ERROR("Invalid parameters for WAV generation");
      return -1;
   }

   if (!tts_handle.is_initialized) {
      LOG_ERROR("TTS not initialized for WAV generation");
      return -1;
   }

   // THREAD SAFETY: Lock the TTS mutex to prevent conflicts with local TTS
   pthread_mutex_lock(&tts_mutex);

   // Store original state (mutex already held)
   int original_state = tts_playback_state;

   try {
      LOG_INFO("Generating network WAV: \"%s\"", text);

      // Pause local TTS to prevent conflicts (mutex already held)
      if (tts_playback_state == TTS_PLAYBACK_PLAY) {
         tts_playback_state = TTS_PLAYBACK_PAUSE;
         LOG_INFO("Paused local TTS for network generation");
      }

      // Preprocess: Replace em-dashes with commas for better TTS phrasing
      std::string processedText(text);
      size_t pos = 0;
      while ((pos = processedText.find("—", pos)) != std::string::npos) {
         processedText.replace(pos, 3, ",");  // UTF-8 em-dash is 3 bytes
         pos += 1;
      }

      std::ostringstream audioStream;
      piper::SynthesisResult result;

      // Generate WAV using shared TTS handle (now thread-safe)
      piper::textToWavFile(tts_handle.config, tts_handle.voice, processedText, audioStream, result);

      // Record TTS timing metrics (inferSeconds is in seconds, convert to ms)
      double tts_time_ms = result.inferSeconds * 1000.0;
      metrics_record_tts_timing(tts_time_ms);
      LOG_INFO("TTS synthesis completed: %.1f ms (RTF: %.3f)", tts_time_ms, result.realTimeFactor);

      // Restore local TTS state (mutex already held)
      tts_playback_state = original_state;
      if (original_state == TTS_PLAYBACK_PLAY) {
         pthread_cond_signal(&tts_cond);
         LOG_INFO("Resumed local TTS after network generation");
      }

      std::string wavData = audioStream.str();
      if (wavData.empty()) {
         LOG_ERROR("Generated WAV data is empty");
         pthread_mutex_unlock(&tts_mutex);
         return -1;
      }

      *wav_size_out = wavData.size();
      *wav_data_out = (uint8_t *)malloc(*wav_size_out);

      if (!*wav_data_out) {
         LOG_ERROR("Failed to allocate WAV buffer (%zu bytes)", *wav_size_out);
         pthread_mutex_unlock(&tts_mutex);
         return -1;
      }

      memcpy(*wav_data_out, wavData.data(), *wav_size_out);
      LOG_INFO("Network WAV generated safely: %zu bytes", *wav_size_out);

      pthread_mutex_unlock(&tts_mutex);
      return 0;

   } catch (const std::exception &e) {
      LOG_ERROR("TTS WAV generation failed: %s", e.what());

      // Restore TTS state on error (mutex already held)
      tts_playback_state = original_state;
      if (original_state == TTS_PLAYBACK_PLAY) {
         pthread_cond_signal(&tts_cond);
      }

      pthread_mutex_unlock(&tts_mutex);
      return -1;
   }
}

uint8_t *error_to_wav(const char *error_message, size_t *tts_size_out) {
   uint8_t *tts_wav_data = NULL;
   size_t tts_wav_size = 0;

   LOG_INFO("Generating error TTS: \"%s\"", error_message);

   int tts_result = text_to_speech_to_wav(error_message, &tts_wav_data, &tts_wav_size);
   if (tts_result == 0 && tts_wav_data && tts_wav_size > 0) {
      // Apply same size/truncation logic as normal TTS
      if (!check_response_size_limit(tts_wav_size)) {
         uint8_t *truncated_data = NULL;
         size_t truncated_size = 0;

         if (truncate_wav_response(tts_wav_data, tts_wav_size, &truncated_data, &truncated_size) ==
             0) {
            if (truncated_data != NULL) {
               // Actual truncation occurred
               free(tts_wav_data);
               *tts_size_out = truncated_size;
               return truncated_data;
            }
            // Edge case: truncation succeeded but returned NULL
            // This shouldn't happen since we verified it needs truncation
            LOG_ERROR("Truncation logic error: succeeded but no data returned");
         } else {
            // Truncation failed - free and return NULL
            free(tts_wav_data);
            LOG_ERROR("Failed to truncate error TTS");
            *tts_size_out = 0;
            return NULL;
         }
      } else {
         // Fits in buffer, return original
         *tts_size_out = tts_wav_size;
         return tts_wav_data;
      }
   }
   if (tts_wav_data != NULL) {
      free(tts_wav_data);
   }

   LOG_ERROR("Failed to generate error TTS WAV");
   *tts_size_out = 0;
   return NULL;
}

/**
 * @brief Cleans up the text-to-speech system.
 *
 * This function signals the worker thread to terminate, waits for it to finish,
 * closes the audio playback device, and then releases all resources used by the TTS engine.
 */
void cleanup_text_to_speech() {
   if (!tts_handle.is_initialized) {
      LOG_ERROR("Text-to-Speech system not initialized. Call initialize_text_to_speech() first.");
      return;
   }

   tts_handle.is_initialized = 0;

   // Signal the worker thread to exit
   pthread_mutex_lock(&tts_queue_mutex);
   tts_thread_running = false;
   pthread_cond_signal(&tts_queue_cond);
   pthread_mutex_unlock(&tts_queue_mutex);

   // Wait for the worker thread to finish
   pthread_join(tts_thread, NULL);

   // Close the audio playback device
#ifdef ALSA_DEVICE
   if (tts_handle.handle) {
      snd_pcm_close(tts_handle.handle);
      tts_handle.handle = NULL;
   }
   // Clean up conversion resources
   if (tts_handle.resampler) {
      src_delete(tts_handle.resampler);
      tts_handle.resampler = NULL;
   }
   if (tts_handle.resample_in) {
      free(tts_handle.resample_in);
      tts_handle.resample_in = NULL;
   }
   if (tts_handle.resample_out) {
      free(tts_handle.resample_out);
      tts_handle.resample_out = NULL;
   }
   if (tts_handle.convert_out) {
      free(tts_handle.convert_out);
      tts_handle.convert_out = NULL;
   }
   tts_handle.needs_conversion = false;
#else
   if (tts_handle.pa_handle) {
      pa_simple_free(tts_handle.pa_handle);
      tts_handle.pa_handle = NULL;
   }
#endif

   // Destroy synchronization primitives
   pthread_mutex_destroy(&tts_queue_mutex);
   pthread_cond_destroy(&tts_queue_cond);

#ifdef ENABLE_AEC
   // Clean up AEC resampler
   if (g_tts_thread_resampler) {
      resampler_destroy(g_tts_thread_resampler);
      g_tts_thread_resampler = nullptr;
   }
   // Note: g_tts_resample_buffer is static, no free needed
#endif

   // Clean up the TTS engine
   terminate(tts_handle.config);
}

void remove_chars(char *str, const char *remove_chars) {
   char *src, *dst;
   bool should_remove;
   for (src = dst = str; *src != '\0'; src++) {
      should_remove = false;
      for (const char *rc = remove_chars; *rc != '\0'; rc++) {
         if (*src == *rc) {
            should_remove = true;
            break;
         }
      }
      if (!should_remove) {
         *dst++ = *src;
      }
   }
   *dst = '\0';
}

bool is_emoji(unsigned int codepoint) {
   // Basic emoji ranges (not exhaustive)
   return (codepoint >= 0x1F600 && codepoint <= 0x1F64F) ||  // Emoticons
          (codepoint >= 0x1F300 &&
           codepoint <= 0x1F5FF) ||  // Miscellaneous Symbols and Pictographs
          (codepoint >= 0x1F680 && codepoint <= 0x1F6FF) ||  // Transport and Map Symbols
          (codepoint >= 0x2600 && codepoint <= 0x26FF) ||    // Miscellaneous Symbols
          (codepoint >= 0x2700 && codepoint <= 0x27BF) ||    // Dingbats
          (codepoint >= 0x1F900 && codepoint <= 0x1F9FF);    // Supplemental Symbols and Pictographs
}

void remove_emojis(char *str) {
   char *src, *dst;
   src = dst = str;

   while (*src) {
      unsigned char byte = *src;
      unsigned int codepoint = 0;
      int bytes_in_char = 1;

      if (byte < 0x80) {
         codepoint = byte;  // 1-byte ASCII character
      } else if (byte < 0xE0) {
         codepoint = (byte & 0x1F) << 6;
         codepoint |= (*(src + 1) & 0x3F);
         bytes_in_char = 2;
      } else if (byte < 0xF0) {
         codepoint = (byte & 0x0F) << 12;
         codepoint |= (*(src + 1) & 0x3F) << 6;
         codepoint |= (*(src + 2) & 0x3F);
         bytes_in_char = 3;
      } else {
         codepoint = (byte & 0x07) << 18;
         codepoint |= (*(src + 1) & 0x3F) << 12;
         codepoint |= (*(src + 2) & 0x3F) << 6;
         codepoint |= (*(src + 3) & 0x3F);
         bytes_in_char = 4;
      }

      if (!is_emoji(codepoint)) {
         for (int i = 0; i < bytes_in_char; i++) {
            *dst++ = *src++;
         }
      } else {
         src += bytes_in_char;  // Skip emoji
      }
   }
   *dst = '\0';  // Null-terminate the filtered string
}
}
