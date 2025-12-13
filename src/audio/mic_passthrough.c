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
 * Microphone passthrough (voice amplification) using unified audio backend
 */

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>

#include "audio/audio_backend.h"
#include "dawn.h"
#include "logging.h"

#define BUFSIZE 256
#define VA_SAMPLE_RATE 44100
#define VA_CHANNELS 2

static int running = 1;  // Control variable

void setStopVA(void) {
   running = 0;
}

/**
 * @brief Voice amplification thread using unified audio backend
 *
 * Captures audio from microphone and plays it back through speakers
 * for real-time voice amplification.
 */
void *voiceAmplificationThread(void *arg) {
   (void)arg;  // Unused

   // Check that audio backend is initialized
   if (audio_backend_get_type() == AUDIO_BACKEND_NONE) {
      LOG_ERROR("Audio backend not initialized. Call audio_backend_init() first.");
      return NULL;
   }

   // Retrieve the device names for capture and playback
   const char *pcmCaptureDevice = getPcmCaptureDevice();
   const char *pcmPlaybackDevice = findAudioPlaybackDevice("speakers");

   // Validate device availability
   if (!pcmCaptureDevice || !pcmPlaybackDevice) {
      LOG_ERROR("Unable to find audio devices for voice amplification.");
      return NULL;
   }

   LOG_INFO("Voice amplification: capture=%s, playback=%s (backend: %s)", pcmCaptureDevice,
            pcmPlaybackDevice, audio_backend_type_name(audio_backend_get_type()));

   // Stream parameters for voice amplification
   audio_stream_params_t capture_params = { .sample_rate = VA_SAMPLE_RATE,
                                            .channels = VA_CHANNELS,
                                            .format = AUDIO_FORMAT_S16_LE,
                                            .period_frames = BUFSIZE,
                                            .buffer_frames = BUFSIZE * 4 };

   audio_stream_params_t playback_params = { .sample_rate = VA_SAMPLE_RATE,
                                             .channels = VA_CHANNELS,
                                             .format = AUDIO_FORMAT_S16_LE,
                                             .period_frames = BUFSIZE,
                                             .buffer_frames = BUFSIZE * 4 };

   audio_hw_params_t capture_hw_params;
   audio_hw_params_t playback_hw_params;

   // Open capture stream
   audio_stream_capture_handle_t *capture_handle = audio_stream_capture_open(pcmCaptureDevice,
                                                                             &capture_params,
                                                                             &capture_hw_params);
   if (!capture_handle) {
      LOG_ERROR("Error opening capture device for voice amplification: %s", pcmCaptureDevice);
      return NULL;
   }

   // Open playback stream
   audio_stream_playback_handle_t *playback_handle = audio_stream_playback_open(
       pcmPlaybackDevice, &playback_params, &playback_hw_params);
   if (!playback_handle) {
      LOG_ERROR("Error opening playback device for voice amplification: %s", pcmPlaybackDevice);
      audio_stream_capture_close(capture_handle);
      return NULL;
   }

   LOG_INFO("Voice amplification started: rate=%u ch=%u", capture_hw_params.sample_rate,
            capture_hw_params.channels);

   running = 1;

   // Audio buffer sized for frames (S16_LE stereo = 4 bytes per frame)
   size_t bytes_per_frame = audio_bytes_per_frame(AUDIO_FORMAT_S16_LE, VA_CHANNELS);
   uint8_t buffer[BUFSIZE * bytes_per_frame];

   // Main loop for capturing and playing back audio in real-time
   while (running) {
      // Read audio data from input
      ssize_t frames_read = audio_stream_capture_read(capture_handle, buffer, BUFSIZE);
      if (frames_read < 0) {
         int err = (int)(-frames_read);
         if (err == AUDIO_ERR_OVERRUN) {
            LOG_WARNING("Voice amp capture overrun, recovering...");
            audio_stream_capture_recover(capture_handle, err);
            continue;
         }
         LOG_ERROR("Voice amp read error: %s", audio_error_string((audio_error_t)err));
         break;
      }

      if (frames_read == 0) {
         continue;  // No data yet
      }

      // Write audio data to output
      ssize_t frames_written = audio_stream_playback_write(playback_handle, buffer,
                                                           (size_t)frames_read);
      if (frames_written < 0) {
         int err = (int)(-frames_written);
         if (err == AUDIO_ERR_UNDERRUN) {
            LOG_WARNING("Voice amp playback underrun, recovering...");
            audio_stream_playback_recover(playback_handle, err);
            continue;
         }
         LOG_ERROR("Voice amp write error: %s", audio_error_string((audio_error_t)err));
         break;
      }
   }

   LOG_INFO("Voice amplification stopped.");

   // Cleanup
   audio_stream_playback_close(playback_handle);
   audio_stream_capture_close(capture_handle);

   return NULL;
}
