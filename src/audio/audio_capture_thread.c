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
 */

#include "audio/audio_capture_thread.h"

#include <errno.h>
#include <sched.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "dawn.h"
#include "logging.h"

// Audio format constants (match dawn.c)
#define DEFAULT_RATE 16000
#define DEFAULT_CHANNELS 1

#ifdef ALSA_DEVICE
#define DEFAULT_ACCESS SND_PCM_ACCESS_RW_INTERLEAVED
#define DEFAULT_FORMAT SND_PCM_FORMAT_S16_LE
#define DEFAULT_FRAMES 64
#else
#include <pulse/error.h>

#define DEFAULT_PULSE_FORMAT PA_SAMPLE_S16LE
#endif

/**
 * @brief Open ALSA capture device with error recovery
 */
#ifdef ALSA_DEVICE
static int open_alsa_capture(snd_pcm_t **handle,
                             const char *pcm_device,
                             snd_pcm_uframes_t *frames) {
   snd_pcm_hw_params_t *params = NULL;
   unsigned int rate = DEFAULT_RATE;
   int dir = 0;
   // Use 512 frames (32ms at 16kHz) to match PulseAudio chunk size
   // This provides good balance between latency and efficiency
   *frames = 512;
   int rc = 0;

   LOG_INFO("Opening ALSA capture device: %s", pcm_device);

   rc = snd_pcm_open(handle, pcm_device, SND_PCM_STREAM_CAPTURE, 0);
   if (rc < 0) {
      LOG_ERROR("Unable to open PCM device for capture (%s): %s", pcm_device, snd_strerror(rc));
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

   rc = snd_pcm_hw_params_set_format(*handle, params, DEFAULT_FORMAT);
   if (rc < 0) {
      LOG_ERROR("Unable to set sample format: %s", snd_strerror(rc));
      snd_pcm_close(*handle);
      *handle = NULL;
      return 1;
   }

   rc = snd_pcm_hw_params_set_channels(*handle, params, DEFAULT_CHANNELS);
   if (rc < 0) {
      LOG_ERROR("Unable to set channel count: %s", snd_strerror(rc));
      snd_pcm_close(*handle);
      *handle = NULL;
      return 1;
   }

   rc = snd_pcm_hw_params_set_rate_near(*handle, params, &rate, &dir);
   if (rc < 0) {
      LOG_ERROR("Unable to set sample rate: %s", snd_strerror(rc));
      snd_pcm_close(*handle);
      *handle = NULL;
      return 1;
   }
   LOG_INFO("Capture rate set to %u Hz", rate);

   rc = snd_pcm_hw_params_set_period_size_near(*handle, params, frames, &dir);
   if (rc < 0) {
      LOG_ERROR("Unable to set period size: %s", snd_strerror(rc));
      snd_pcm_close(*handle);
      *handle = NULL;
      return 1;
   }
   LOG_INFO("Frames set to %lu", *frames);

   rc = snd_pcm_hw_params(*handle, params);
   if (rc < 0) {
      LOG_ERROR("Unable to set hw parameters: %s", snd_strerror(rc));
      snd_pcm_close(*handle);
      *handle = NULL;
      return 1;
   }

   return 0;
}
#endif

/**
 * @brief Open PulseAudio capture device
 */
#ifndef ALSA_DEVICE
static pa_simple *open_pulse_capture(const char *pcm_device) {
   static const pa_sample_spec ss = { .format = DEFAULT_PULSE_FORMAT,
                                      .rate = DEFAULT_RATE,
                                      .channels = DEFAULT_CHANNELS };

   int error;
   pa_simple *s = pa_simple_new(NULL,              // Server name (NULL = default)
                                "DAWN",            // Application name
                                PA_STREAM_RECORD,  // Stream direction
                                pcm_device,        // Device name
                                "Audio Capture",   // Stream description
                                &ss,               // Sample format
                                NULL,              // Channel map (NULL = default)
                                NULL,              // Buffering attributes (NULL = default)
                                &error);           // Error code

   if (!s) {
      LOG_ERROR("PulseAudio capture device open failed: %s", pa_strerror(error));
   } else {
      LOG_INFO("PulseAudio capture device opened: %s", pcm_device);
   }

   return s;
}
#endif

/**
 * @brief Set realtime priority for current thread
 */
static int set_realtime_priority(void) {
   struct sched_param param;
   int max_priority = sched_get_priority_max(SCHED_FIFO);

   if (max_priority == -1) {
      LOG_WARNING("Failed to get max realtime priority: %s", strerror(errno));
      return 1;
   }

   // Use priority 90 (high, but not maximum to leave room for critical system tasks)
   param.sched_priority = (max_priority > 90) ? 90 : max_priority;

   if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &param) != 0) {
      LOG_WARNING("Failed to set realtime priority (try: sudo setcap cap_sys_nice=ep ./dawn): %s",
                  strerror(errno));
      return 1;
   }

   LOG_INFO("Audio capture thread running with SCHED_FIFO priority %d", param.sched_priority);
   return 0;
}

/**
 * @brief Audio capture thread function
 *
 * Continuously reads audio from device and writes to ring buffer.
 * Handles errors with recovery logic.
 */
static void *capture_thread_func(void *arg) {
   audio_capture_context_t *ctx = (audio_capture_context_t *)arg;

   // Set realtime priority if requested
   if (ctx->use_realtime_priority) {
      set_realtime_priority();
   }

   // Allocate capture buffer
   char *buffer = (char *)malloc(ctx->buffer_size);
   if (!buffer) {
      LOG_ERROR("Failed to allocate capture buffer of %zu bytes", ctx->buffer_size);
      atomic_store(&ctx->running, false);
      return NULL;
   }

   LOG_INFO("Audio capture thread started (buffer=%zu bytes, device=%s)", ctx->buffer_size,
            ctx->pcm_device);

#ifdef ALSA_DEVICE
   // ALSA capture loop
   while (atomic_load(&ctx->running)) {
      int rc = snd_pcm_readi(ctx->handle, buffer, ctx->frames);

      if (rc > 0) {
         // Successful read - write to ring buffer
         size_t bytes_read = rc * DEFAULT_CHANNELS * 2;  // 2 bytes per sample (S16_LE)
         ring_buffer_write(ctx->ring_buffer, buffer, bytes_read);
      } else if (rc == -EPIPE) {
         LOG_WARNING("ALSA overrun in capture thread, recovering");
         snd_pcm_prepare(ctx->handle);
         continue;
      } else if (rc == -ESTRPIPE) {
         LOG_WARNING("ALSA stream suspended in capture thread, attempting resume");
         while ((rc = snd_pcm_resume(ctx->handle)) == -EAGAIN) {
            sleep(1);
         }
         if (rc < 0) {
            LOG_ERROR("Resume failed, preparing PCM");
            snd_pcm_prepare(ctx->handle);
         }
         continue;
      } else if (rc == -EINTR) {
         LOG_WARNING("ALSA read interrupted by signal in capture thread, retrying");
         continue;
      } else {
         LOG_ERROR("ALSA read error in capture thread: %s", snd_strerror(rc));
         // Continue trying despite error
         usleep(10000);  // 10ms delay before retry
      }
   }
#else
   // PulseAudio capture loop
   int error = 0;

   while (atomic_load(&ctx->running)) {
      if (pa_simple_read(ctx->pa_handle, buffer, ctx->buffer_size, &error) < 0) {
         LOG_ERROR("PulseAudio read error in capture thread: %s", pa_strerror(error));
         usleep(10000);  // 10ms delay before retry
         continue;
      }

      ring_buffer_write(ctx->ring_buffer, buffer, ctx->buffer_size);
   }
#endif

   free(buffer);
   LOG_INFO("Audio capture thread stopped");

   return NULL;
}

audio_capture_context_t *audio_capture_start(const char *pcm_device,
                                             size_t ring_buffer_size,
                                             int use_realtime_priority) {
   if (!pcm_device) {
      LOG_ERROR("PCM device name cannot be NULL");
      return NULL;
   }

   audio_capture_context_t *ctx = (audio_capture_context_t *)calloc(
       1, sizeof(audio_capture_context_t));
   if (!ctx) {
      LOG_ERROR("Failed to allocate capture context");
      return NULL;
   }

   // Initialize context
   ctx->pcm_device = strdup(pcm_device);
   ctx->use_realtime_priority = use_realtime_priority;
   atomic_init(&ctx->running, false);

   // Create ring buffer
   ctx->ring_buffer = ring_buffer_create(ring_buffer_size);
   if (!ctx->ring_buffer) {
      LOG_ERROR("Failed to create ring buffer");
      free(ctx->pcm_device);
      free(ctx);
      return NULL;
   }

#ifdef ALSA_DEVICE
   // Open ALSA device
   if (open_alsa_capture(&ctx->handle, pcm_device, &ctx->frames) != 0) {
      LOG_ERROR("Failed to open ALSA capture device");
      ring_buffer_free(ctx->ring_buffer);
      free(ctx->pcm_device);
      free(ctx);
      return NULL;
   }
   ctx->buffer_size = ctx->frames * DEFAULT_CHANNELS * 2;  // 2 bytes per sample (S16_LE)
#else
   // Open PulseAudio device
   ctx->pa_handle = open_pulse_capture(pcm_device);
   if (!ctx->pa_handle) {
      LOG_ERROR("Failed to open PulseAudio capture device");
      ring_buffer_free(ctx->ring_buffer);
      free(ctx->pcm_device);
      free(ctx);
      return NULL;
   }
   ctx->pa_framesize = pa_frame_size(&(pa_sample_spec){ .format = DEFAULT_PULSE_FORMAT,
                                                        .rate = DEFAULT_RATE,
                                                        .channels = DEFAULT_CHANNELS });
   // Read chunks of 512 frames at a time (1024 bytes for 16-bit mono)
   ctx->buffer_size = ctx->pa_framesize * 512;
#endif

   // Start capture thread
   atomic_store(&ctx->running, true);
   if (pthread_create(&ctx->thread, NULL, capture_thread_func, ctx) != 0) {
      LOG_ERROR("Failed to create capture thread: %s", strerror(errno));
#ifdef ALSA_DEVICE
      snd_pcm_close(ctx->handle);
#else
      pa_simple_free(ctx->pa_handle);
#endif
      ring_buffer_free(ctx->ring_buffer);
      free(ctx->pcm_device);
      free(ctx);
      return NULL;
   }

   LOG_INFO("Audio capture started successfully");
   return ctx;
}

void audio_capture_stop(audio_capture_context_t *ctx) {
   if (!ctx) {
      return;
   }

   LOG_INFO("Stopping audio capture thread...");

   // Signal thread to stop
   atomic_store(&ctx->running, false);

   // Wait for thread to exit
   pthread_join(ctx->thread, NULL);

   // Close audio device
#ifdef ALSA_DEVICE
   if (ctx->handle) {
      snd_pcm_drop(ctx->handle);
      snd_pcm_close(ctx->handle);
   }
#else
   if (ctx->pa_handle) {
      pa_simple_free(ctx->pa_handle);
   }
#endif

   // Free ring buffer
   ring_buffer_free(ctx->ring_buffer);

   // Free device name
   free(ctx->pcm_device);

   // Free context
   free(ctx);

   LOG_INFO("Audio capture stopped and resources freed");
}

size_t audio_capture_read(audio_capture_context_t *ctx, char *data, size_t len) {
   if (!ctx || !ctx->ring_buffer) {
      return 0;
   }
   return ring_buffer_read(ctx->ring_buffer, data, len);
}

size_t audio_capture_wait_for_data(audio_capture_context_t *ctx, size_t min_bytes, int timeout_ms) {
   if (!ctx || !ctx->ring_buffer) {
      return 0;
   }
   return ring_buffer_wait_for_data(ctx->ring_buffer, min_bytes, timeout_ms);
}

size_t audio_capture_bytes_available(audio_capture_context_t *ctx) {
   if (!ctx || !ctx->ring_buffer) {
      return 0;
   }
   return ring_buffer_bytes_available(ctx->ring_buffer);
}

int audio_capture_is_running(audio_capture_context_t *ctx) {
   if (!ctx) {
      return 0;
   }
   return atomic_load(&ctx->running);
}

void audio_capture_clear(audio_capture_context_t *ctx) {
   if (!ctx || !ctx->ring_buffer) {
      return;
   }
   ring_buffer_clear(ctx->ring_buffer);
}
