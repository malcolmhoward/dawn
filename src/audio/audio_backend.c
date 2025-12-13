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
 * DAWN Audio Backend - Auto-detection and Dispatch
 *
 * This module provides runtime selection between ALSA and PulseAudio backends.
 * It uses function pointers to dispatch to the appropriate backend implementation.
 *
 * Thread Safety:
 *   - audio_backend_init() protected by static mutex
 *   - Handle operations dispatch to backend without additional locking
 *
 * Memory Management:
 *   - No dynamic allocation in dispatch layer
 *   - Backends use static handle pools (see audio_alsa.c, audio_pulse.c)
 */

#include "audio/audio_backend.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h> /* For strcasecmp */
#include <unistd.h>

#include "logging.h"

/* =============================================================================
 * Backend-specific declarations
 * ============================================================================= */

/* ALSA backend functions (implemented in audio_alsa.c) */
extern audio_stream_capture_handle_t *alsa_capture_open(const char *device,
                                                        const audio_stream_params_t *params,
                                                        audio_hw_params_t *hw_params);
extern ssize_t alsa_capture_read(audio_stream_capture_handle_t *handle,
                                 void *buffer,
                                 size_t frames);
extern ssize_t alsa_capture_avail(audio_stream_capture_handle_t *handle);
extern int alsa_capture_recover(audio_stream_capture_handle_t *handle, int err);
extern void alsa_capture_close(audio_stream_capture_handle_t *handle);

extern audio_stream_playback_handle_t *alsa_playback_open(const char *device,
                                                          const audio_stream_params_t *params,
                                                          audio_hw_params_t *hw_params);
extern ssize_t alsa_playback_write(audio_stream_playback_handle_t *handle,
                                   const void *buffer,
                                   size_t frames);
extern ssize_t alsa_playback_avail(audio_stream_playback_handle_t *handle);
extern int alsa_playback_drain(audio_stream_playback_handle_t *handle);
extern int alsa_playback_drop(audio_stream_playback_handle_t *handle);
extern int alsa_playback_recover(audio_stream_playback_handle_t *handle, int err);
extern void alsa_playback_close(audio_stream_playback_handle_t *handle);

/* PulseAudio backend functions (implemented in audio_pulse.c) */
extern audio_stream_capture_handle_t *pulse_capture_open(const char *device,
                                                         const audio_stream_params_t *params,
                                                         audio_hw_params_t *hw_params);
extern ssize_t pulse_capture_read(audio_stream_capture_handle_t *handle,
                                  void *buffer,
                                  size_t frames);
extern ssize_t pulse_capture_avail(audio_stream_capture_handle_t *handle);
extern int pulse_capture_recover(audio_stream_capture_handle_t *handle, int err);
extern void pulse_capture_close(audio_stream_capture_handle_t *handle);

extern audio_stream_playback_handle_t *pulse_playback_open(const char *device,
                                                           const audio_stream_params_t *params,
                                                           audio_hw_params_t *hw_params);
extern ssize_t pulse_playback_write(audio_stream_playback_handle_t *handle,
                                    const void *buffer,
                                    size_t frames);
extern ssize_t pulse_playback_avail(audio_stream_playback_handle_t *handle);
extern int pulse_playback_drain(audio_stream_playback_handle_t *handle);
extern int pulse_playback_drop(audio_stream_playback_handle_t *handle);
extern int pulse_playback_recover(audio_stream_playback_handle_t *handle, int err);
extern void pulse_playback_close(audio_stream_playback_handle_t *handle);

/* =============================================================================
 * Backend vtable definition
 * ============================================================================= */

typedef struct {
   /* Capture operations */
   audio_stream_capture_handle_t *(*capture_open)(const char *device,
                                                  const audio_stream_params_t *params,
                                                  audio_hw_params_t *hw_params);
   ssize_t (*capture_read)(audio_stream_capture_handle_t *handle, void *buffer, size_t frames);
   ssize_t (*capture_avail)(audio_stream_capture_handle_t *handle);
   int (*capture_recover)(audio_stream_capture_handle_t *handle, int err);
   void (*capture_close)(audio_stream_capture_handle_t *handle);

   /* Playback operations */
   audio_stream_playback_handle_t *(*playback_open)(const char *device,
                                                    const audio_stream_params_t *params,
                                                    audio_hw_params_t *hw_params);
   ssize_t (*playback_write)(audio_stream_playback_handle_t *handle,
                             const void *buffer,
                             size_t frames);
   ssize_t (*playback_avail)(audio_stream_playback_handle_t *handle);
   int (*playback_drain)(audio_stream_playback_handle_t *handle);
   int (*playback_drop)(audio_stream_playback_handle_t *handle);
   int (*playback_recover)(audio_stream_playback_handle_t *handle, int err);
   void (*playback_close)(audio_stream_playback_handle_t *handle);
} audio_backend_vtable_t;

/* Static vtables for each backend (const for potential ROM placement) */
static const audio_backend_vtable_t g_alsa_vtable = {
   .capture_open = alsa_capture_open,
   .capture_read = alsa_capture_read,
   .capture_avail = alsa_capture_avail,
   .capture_recover = alsa_capture_recover,
   .capture_close = alsa_capture_close,
   .playback_open = alsa_playback_open,
   .playback_write = alsa_playback_write,
   .playback_avail = alsa_playback_avail,
   .playback_drain = alsa_playback_drain,
   .playback_drop = alsa_playback_drop,
   .playback_recover = alsa_playback_recover,
   .playback_close = alsa_playback_close,
};

static const audio_backend_vtable_t g_pulse_vtable = {
   .capture_open = pulse_capture_open,
   .capture_read = pulse_capture_read,
   .capture_avail = pulse_capture_avail,
   .capture_recover = pulse_capture_recover,
   .capture_close = pulse_capture_close,
   .playback_open = pulse_playback_open,
   .playback_write = pulse_playback_write,
   .playback_avail = pulse_playback_avail,
   .playback_drain = pulse_playback_drain,
   .playback_drop = pulse_playback_drop,
   .playback_recover = pulse_playback_recover,
   .playback_close = pulse_playback_close,
};

/* =============================================================================
 * Module state
 * ============================================================================= */

static pthread_mutex_t g_init_mutex = PTHREAD_MUTEX_INITIALIZER;
static audio_backend_type_t g_backend_type = AUDIO_BACKEND_NONE;
static bool g_backend_initialized = false;
static const audio_backend_vtable_t *g_vtable = NULL;

/* Backend availability flags (set at compile time since we link statically) */
static const bool g_alsa_available = true;  /* Always linked */
static const bool g_pulse_available = true; /* Always linked */

/* =============================================================================
 * Error code handling
 * ============================================================================= */

const char *audio_error_string(audio_error_t err) {
   switch (err) {
      case AUDIO_SUCCESS:
         return "Success";
      case AUDIO_ERR_INVALID:
         return "Invalid parameter or NULL handle";
      case AUDIO_ERR_NOT_INIT:
         return "Backend not initialized";
      case AUDIO_ERR_NO_DEVICE:
         return "Device not found or cannot open";
      case AUDIO_ERR_OVERRUN:
         return "Capture buffer overrun";
      case AUDIO_ERR_UNDERRUN:
         return "Playback buffer underrun";
      case AUDIO_ERR_SUSPENDED:
         return "Device suspended";
      case AUDIO_ERR_IO:
         return "I/O error";
      case AUDIO_ERR_BUSY:
         return "Device busy or no handles available";
      case AUDIO_ERR_TIMEOUT:
         return "Operation timed out";
      case AUDIO_ERR_UNKNOWN:
      default:
         return "Unknown error";
   }
}

/* =============================================================================
 * Availability detection
 * ============================================================================= */

/**
 * @brief Check if PulseAudio daemon is running
 */
static bool detect_pulse_running(void) {
   /* Check for PULSE_SERVER environment variable */
   const char *pulse_server = getenv("PULSE_SERVER");
   if (pulse_server && pulse_server[0] != '\0') {
      return true;
   }

   /* Check for XDG_RUNTIME_DIR/pulse */
   const char *xdg_runtime = getenv("XDG_RUNTIME_DIR");
   if (xdg_runtime) {
      char path[256];
      snprintf(path, sizeof(path), "%s/pulse/native", xdg_runtime);
      if (access(path, F_OK) == 0) {
         return true;
      }
   }

   /* Fallback: check /run/user/$UID/pulse */
   char path[256];
   snprintf(path, sizeof(path), "/run/user/%d/pulse/native", getuid());
   if (access(path, F_OK) == 0) {
      return true;
   }

   return false;
}

/* =============================================================================
 * Backend Initialization API
 * ============================================================================= */

int audio_backend_init(audio_backend_type_t type) {
   int result = AUDIO_SUCCESS;

   pthread_mutex_lock(&g_init_mutex);

   if (g_backend_initialized) {
      LOG_WARNING("Audio backend already initialized (type=%s)",
                  audio_backend_type_name(g_backend_type));
      pthread_mutex_unlock(&g_init_mutex);
      return AUDIO_SUCCESS;
   }

   LOG_INFO("Audio backend detection: ALSA=%s, PulseAudio=%s",
            g_alsa_available ? "available" : "not found",
            g_pulse_available ? "available" : "not found");

   audio_backend_type_t selected = type;

   /* Auto-detect best backend */
   if (type == AUDIO_BACKEND_AUTO) {
      /* Prefer ALSA for embedded/low-latency scenarios */
      /* But if PulseAudio is running, use it for desktop integration */
      if (g_pulse_available && detect_pulse_running()) {
         selected = AUDIO_BACKEND_PULSE;
         LOG_INFO("Auto-detected: PulseAudio (daemon running)");
      } else if (g_alsa_available) {
         selected = AUDIO_BACKEND_ALSA;
         LOG_INFO("Auto-detected: ALSA (direct hardware access)");
      } else if (g_pulse_available) {
         selected = AUDIO_BACKEND_PULSE;
         LOG_INFO("Auto-detected: PulseAudio (fallback)");
      } else {
         LOG_ERROR("No audio backend available");
         result = AUDIO_ERR_NO_DEVICE;
         goto done;
      }
   }

   /* Validate selected backend */
   if (selected == AUDIO_BACKEND_ALSA && !g_alsa_available) {
      LOG_ERROR("ALSA backend requested but not available");
      result = AUDIO_ERR_NO_DEVICE;
      goto done;
   }
   if (selected == AUDIO_BACKEND_PULSE && !g_pulse_available) {
      LOG_ERROR("PulseAudio backend requested but not available");
      result = AUDIO_ERR_NO_DEVICE;
      goto done;
   }

   /* Set up vtable pointer */
   switch (selected) {
      case AUDIO_BACKEND_ALSA:
         g_vtable = &g_alsa_vtable;
         break;
      case AUDIO_BACKEND_PULSE:
         g_vtable = &g_pulse_vtable;
         break;
      case AUDIO_BACKEND_NONE:
         g_vtable = NULL;
         break;
      default:
         LOG_ERROR("Unknown backend type: %d", selected);
         result = AUDIO_ERR_INVALID;
         goto done;
   }

   g_backend_type = selected;
   g_backend_initialized = true;

   LOG_INFO("Audio backend initialized: %s", audio_backend_type_name(g_backend_type));

done:
   pthread_mutex_unlock(&g_init_mutex);
   return result;
}

void audio_backend_cleanup(void) {
   pthread_mutex_lock(&g_init_mutex);

   if (!g_backend_initialized) {
      pthread_mutex_unlock(&g_init_mutex);
      return;
   }

   g_vtable = NULL;
   g_backend_type = AUDIO_BACKEND_NONE;
   g_backend_initialized = false;

   LOG_INFO("Audio backend cleaned up");

   pthread_mutex_unlock(&g_init_mutex);
}

audio_backend_type_t audio_backend_get_type(void) {
   return g_backend_type;
}

const char *audio_backend_type_name(audio_backend_type_t type) {
   switch (type) {
      case AUDIO_BACKEND_AUTO:
         return "auto";
      case AUDIO_BACKEND_ALSA:
         return "alsa";
      case AUDIO_BACKEND_PULSE:
         return "pulse";
      case AUDIO_BACKEND_NONE:
         return "none";
      default:
         return "unknown";
   }
}

audio_backend_type_t audio_backend_parse_type(const char *name) {
   if (!name || name[0] == '\0') {
      return AUDIO_BACKEND_AUTO;
   }

   if (strcasecmp(name, "auto") == 0) {
      return AUDIO_BACKEND_AUTO;
   } else if (strcasecmp(name, "alsa") == 0) {
      return AUDIO_BACKEND_ALSA;
   } else if (strcasecmp(name, "pulse") == 0 || strcasecmp(name, "pulseaudio") == 0) {
      return AUDIO_BACKEND_PULSE;
   } else if (strcasecmp(name, "none") == 0) {
      return AUDIO_BACKEND_NONE;
   }

   LOG_WARNING("Unknown audio backend '%s', defaulting to auto", name);
   return AUDIO_BACKEND_AUTO;
}

bool audio_backend_is_available(audio_backend_type_t type) {
   switch (type) {
      case AUDIO_BACKEND_AUTO:
         return g_alsa_available || g_pulse_available;
      case AUDIO_BACKEND_ALSA:
         return g_alsa_available;
      case AUDIO_BACKEND_PULSE:
         return g_pulse_available;
      case AUDIO_BACKEND_NONE:
         return true;
      default:
         return false;
   }
}

/* =============================================================================
 * Audio Stream Capture API - Dispatch to backend
 * ============================================================================= */

audio_stream_capture_handle_t *audio_stream_capture_open(const char *device,
                                                         const audio_stream_params_t *params,
                                                         audio_hw_params_t *hw_params) {
   if (!g_backend_initialized) {
      LOG_ERROR("Audio backend not initialized");
      return NULL;
   }
   if (!g_vtable || !g_vtable->capture_open) {
      LOG_ERROR("Capture not supported by backend");
      return NULL;
   }

   return g_vtable->capture_open(device, params, hw_params);
}

ssize_t audio_stream_capture_read(audio_stream_capture_handle_t *handle,
                                  void *buffer,
                                  size_t frames) {
   if (!handle) {
      return -AUDIO_ERR_INVALID;
   }
   /* Skip vtable check in hot path - guaranteed valid after init */
   return g_vtable->capture_read(handle, buffer, frames);
}

ssize_t audio_stream_capture_avail(audio_stream_capture_handle_t *handle) {
   if (!handle) {
      return -AUDIO_ERR_INVALID;
   }
   return g_vtable->capture_avail(handle);
}

int audio_stream_capture_recover(audio_stream_capture_handle_t *handle, int err) {
   if (!handle) {
      return AUDIO_ERR_INVALID;
   }
   if (!g_vtable || !g_vtable->capture_recover) {
      return AUDIO_ERR_NOT_INIT;
   }
   return g_vtable->capture_recover(handle, err);
}

void audio_stream_capture_close(audio_stream_capture_handle_t *handle) {
   if (!handle) {
      return;
   }
   if (g_vtable && g_vtable->capture_close) {
      g_vtable->capture_close(handle);
   }
}

/* =============================================================================
 * Audio Stream Playback API - Dispatch to backend
 * ============================================================================= */

audio_stream_playback_handle_t *audio_stream_playback_open(const char *device,
                                                           const audio_stream_params_t *params,
                                                           audio_hw_params_t *hw_params) {
   if (!g_backend_initialized) {
      LOG_ERROR("Audio backend not initialized");
      return NULL;
   }
   if (!g_vtable || !g_vtable->playback_open) {
      LOG_ERROR("Playback not supported by backend");
      return NULL;
   }

   return g_vtable->playback_open(device, params, hw_params);
}

ssize_t audio_stream_playback_write(audio_stream_playback_handle_t *handle,
                                    const void *buffer,
                                    size_t frames) {
   if (!handle) {
      return -AUDIO_ERR_INVALID;
   }
   /* Skip vtable check in hot path - guaranteed valid after init */
   return g_vtable->playback_write(handle, buffer, frames);
}

ssize_t audio_stream_playback_avail(audio_stream_playback_handle_t *handle) {
   if (!handle) {
      return -AUDIO_ERR_INVALID;
   }
   return g_vtable->playback_avail(handle);
}

int audio_stream_playback_drain(audio_stream_playback_handle_t *handle) {
   if (!handle) {
      return AUDIO_ERR_INVALID;
   }
   if (!g_vtable || !g_vtable->playback_drain) {
      return AUDIO_ERR_NOT_INIT;
   }
   return g_vtable->playback_drain(handle);
}

int audio_stream_playback_drop(audio_stream_playback_handle_t *handle) {
   if (!handle) {
      return AUDIO_ERR_INVALID;
   }
   if (!g_vtable || !g_vtable->playback_drop) {
      return AUDIO_ERR_NOT_INIT;
   }
   return g_vtable->playback_drop(handle);
}

int audio_stream_playback_recover(audio_stream_playback_handle_t *handle, int err) {
   if (!handle) {
      return AUDIO_ERR_INVALID;
   }
   if (!g_vtable || !g_vtable->playback_recover) {
      return AUDIO_ERR_NOT_INIT;
   }
   return g_vtable->playback_recover(handle, err);
}

void audio_stream_playback_close(audio_stream_playback_handle_t *handle) {
   if (!handle) {
      return;
   }
   if (g_vtable && g_vtable->playback_close) {
      g_vtable->playback_close(handle);
   }
}

/* =============================================================================
 * Utility Functions
 * ============================================================================= */

size_t audio_bytes_per_frame(audio_sample_format_t format, unsigned int channels) {
   size_t bytes_per_sample;

   switch (format) {
      case AUDIO_FORMAT_S16_LE:
         bytes_per_sample = 2;
         break;
      case AUDIO_FORMAT_S24_3LE:
         bytes_per_sample = 3;
         break;
      case AUDIO_FORMAT_S32_LE:
      case AUDIO_FORMAT_FLOAT32:
         bytes_per_sample = 4;
         break;
      default:
         bytes_per_sample = 2;
         break;
   }

   return bytes_per_sample * channels;
}

void audio_stream_capture_default_params(audio_stream_params_t *params) {
   if (!params) {
      return;
   }

   params->sample_rate = 48000; /* Native capture rate for AEC */
   params->channels = 1;        /* Mono for voice */
   params->format = AUDIO_FORMAT_S16_LE;
   params->period_frames = 1536;     /* 32ms at 48kHz */
   params->buffer_frames = 1536 * 4; /* 4 periods */
}

void audio_stream_playback_default_params(audio_stream_params_t *params) {
   if (!params) {
      return;
   }

   params->sample_rate = 22050; /* Piper TTS output rate */
   params->channels = 1;        /* Mono for voice */
   params->format = AUDIO_FORMAT_S16_LE;
   params->period_frames = 512;     /* ~23ms at 22050Hz */
   params->buffer_frames = 512 * 4; /* 4 periods */
}
