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
 * DAWN Audio Playback Module
 *
 * Decodes and plays audio files (FLAC, MP3, Ogg Vorbis) with automatic
 * sample rate conversion using the unified audio_decoder API.
 *
 * Note: Function is still named playFlacAudio() for backward compatibility,
 * but now supports all formats registered with the audio_decoder subsystem.
 *
 * ALSA/dmix Note:
 *   For audio playback to work with ALSA, a dmix-compatible output device
 *   should be used (e.g., "default" or a dmix plugin). The dmix plugin
 *   allows multiple applications to share the sound card by mixing audio
 *   in software. Hardware devices (hw:X,Y) do not support mixing.
 */

#include "audio/flac_playback.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio/audio_backend.h"
#include "audio/audio_converter.h"
#include "audio/audio_decoder.h"
#include "logging.h"
#include "mosquitto_comms.h"

/* Maximum channels supported for playback (stereo) */
#define PLAYBACK_MAX_CHANNELS 2

/* Read buffer size in frames (matches FLAC max block size) */
#define READ_BUFFER_FRAMES 8192

/**
 * @var float global_volume
 * @brief Global volume control for audio playback.
 *
 * The volume level is represented as a floating-point number between 0.0 and 1.0,
 * where 0.0 means silence and 1.0 corresponds to the maximum volume level without
 * amplification. Values above 1.0 may result in amplification and potentially
 * introduce distortion or clipping.
 */
static float global_volume = 0.5f;

/* Global variable to control music playback state.
 * When set to 0, music playback is stopped.
 * When set to 1, music playback is active.
 */
static int music_play = 0;

/* Current playback position in samples (per channel).
 * Used for pause/resume functionality - allows resuming from
 * the approximate position where playback was paused.
 * Volatile because it's written from playback thread and read from main thread.
 */
static volatile uint64_t s_current_position = 0;

/* Current playback sample rate (cached for pause/resume).
 * Avoids needing to re-open the decoder just to get sample rate on pause.
 */
static volatile uint32_t s_current_sample_rate = 0;

void setMusicPlay(int play) {
   music_play = play;
}

int getMusicPlay(void) {
   return music_play;
}

uint64_t audio_playback_get_position(void) {
   return s_current_position;
}

uint32_t audio_playback_get_sample_rate(void) {
   /* Return cached sample rate from current playback, or 0 if not playing */
   return s_current_sample_rate;
}

/**
 * @brief Apply volume scaling to audio samples
 *
 * Applies global_volume scaling with clipping protection.
 *
 * @param buffer Sample buffer (interleaved)
 * @param frames Number of frames
 * @param channels Number of channels
 */
static void apply_volume(int16_t *buffer, size_t frames, unsigned int channels) {
   size_t total_samples = frames * channels;

   for (size_t i = 0; i < total_samples; i++) {
      int32_t adjusted = (int32_t)(buffer[i] * global_volume);

      /* Clipping protection */
      if (adjusted < INT16_MIN) {
         adjusted = INT16_MIN;
      } else if (adjusted > INT16_MAX) {
         adjusted = INT16_MAX;
      }

      buffer[i] = (int16_t)adjusted;
   }
}

void *playFlacAudio(void *arg) {
   int should_respond = 0;
   PlaybackArgs *args = (PlaybackArgs *)arg;

   /* Buffers and handles */
   audio_decoder_t *decoder = NULL;
   audio_stream_playback_handle_t *playback_handle = NULL;
   audio_converter_t *converter = NULL;
   int16_t *read_buffer = NULL;
   int16_t *output_buffer = NULL;
   size_t output_buffer_frames = 0;

   int song_finished_naturally = 0;

   /* Check that audio backend is initialized */
   if (audio_backend_get_type() == AUDIO_BACKEND_NONE) {
      LOG_ERROR("Audio backend not initialized. Call audio_backend_init() first.");
      goto cleanup;
   }

   /* Open audio file with decoder (auto-detects format by extension) */
   decoder = audio_decoder_open(args->file_name);
   if (!decoder) {
      LOG_ERROR("Failed to open audio file: %s", args->file_name);
      goto cleanup;
   }

   /* Get audio file info */
   audio_decoder_info_t info;
   if (audio_decoder_get_info(decoder, &info) != AUDIO_DECODER_SUCCESS) {
      LOG_ERROR("Failed to get audio file info");
      goto cleanup;
   }

   LOG_INFO("Audio file: %s (%s)", args->file_name, audio_decoder_format_name(info.format));
   LOG_INFO("  Format: %uHz %uch %ubps", info.sample_rate, info.channels, info.bits_per_sample);

   /* Cache sample rate for pause/resume (avoids re-opening decoder on pause) */
   s_current_sample_rate = info.sample_rate;

   /* Get configured output rate/channels for dmix compatibility */
   unsigned int output_rate = audio_conv_get_output_rate();
   unsigned int output_channels = audio_conv_get_output_channels();

   /* Open playback device at configured rate */
   audio_stream_params_t params = { .sample_rate = output_rate,
                                    .channels = output_channels,
                                    .format = AUDIO_FORMAT_S16_LE,
                                    .period_frames = 1024,
                                    .buffer_frames = 4096 };
   audio_hw_params_t hw_params;

   LOG_INFO("Opening playback device: %s (backend: %s)", args->sink_name,
            audio_backend_type_name(audio_backend_get_type()));

   playback_handle = audio_stream_playback_open(args->sink_name, &params, &hw_params);
   if (!playback_handle) {
      LOG_ERROR("Failed to open audio device %s for playback", args->sink_name);
      goto cleanup;
   }

   /* Use actual hardware rate/channels (may differ from requested) */
   unsigned int actual_rate = hw_params.sample_rate;
   unsigned int actual_channels = hw_params.channels;

   LOG_INFO("Playback: requested=%uHz/%uch actual=%uHz/%uch", output_rate, output_channels,
            actual_rate, actual_channels);

   /* Allocate read buffer for decoded samples */
   read_buffer = malloc(READ_BUFFER_FRAMES * PLAYBACK_MAX_CHANNELS * sizeof(int16_t));
   if (!read_buffer) {
      LOG_ERROR("Failed to allocate read buffer");
      goto cleanup;
   }

   /* Create converter if source and output formats differ */
   audio_converter_params_t conv_params = { .sample_rate = info.sample_rate,
                                            .channels = info.channels };
   if (audio_converter_needed_ex(&conv_params, actual_rate, actual_channels)) {
      converter = audio_converter_create_ex(&conv_params, actual_rate, actual_channels);
      if (!converter) {
         LOG_ERROR("Failed to create audio converter");
         goto cleanup;
      }

      /* Allocate output buffer for converted audio */
      output_buffer_frames = audio_converter_max_output_frames(converter, READ_BUFFER_FRAMES);
      output_buffer = malloc(output_buffer_frames * actual_channels * sizeof(int16_t));
      if (!output_buffer) {
         LOG_ERROR("Failed to allocate output buffer");
         goto cleanup;
      }
   }

   music_play = 1; /* Enable playback */

   /* Seek to start time if specified */
   if (args->start_time > 0) {
      uint64_t start_sample = (uint64_t)args->start_time * info.sample_rate;
      if (audio_decoder_seek(decoder, start_sample) != AUDIO_DECODER_SUCCESS) {
         LOG_WARNING("Failed to seek to start time %u seconds", args->start_time);
         s_current_position = 0; /* Reset position on seek failure */
      } else {
         s_current_position = start_sample;
      }
   } else {
      s_current_position = 0;
   }

   /* Main playback loop */
   while (getMusicPlay()) {
      /* Read decoded samples */
      ssize_t frames_read = audio_decoder_read(decoder, read_buffer, READ_BUFFER_FRAMES);

      if (frames_read < 0) {
         LOG_ERROR("Audio decode error: %s",
                   audio_decoder_error_string((audio_decoder_error_t)(-frames_read)));
         break;
      }

      if (frames_read == 0) {
         /* EOF - song finished naturally */
         song_finished_naturally = 1;
         LOG_INFO("Audio playback completed");
         break;
      }

      /* Update playback position for pause/resume tracking */
      s_current_position += (uint64_t)frames_read;

      /* Apply volume */
      apply_volume(read_buffer, (size_t)frames_read, info.channels);

      /* Write to playback device (with conversion if needed) */
      ssize_t frames_written;
      if (converter) {
         ssize_t converted_frames = audio_converter_process(converter, read_buffer,
                                                            (size_t)frames_read, output_buffer,
                                                            output_buffer_frames);
         if (converted_frames < 0) {
            LOG_ERROR("Audio conversion failed");
            break;
         }

         frames_written = audio_stream_playback_write(playback_handle, output_buffer,
                                                      (size_t)converted_frames);
      } else {
         frames_written = audio_stream_playback_write(playback_handle, read_buffer,
                                                      (size_t)frames_read);
      }

      if (frames_written < 0) {
         int err = (int)(-frames_written);
         if (err == AUDIO_ERR_UNDERRUN) {
            LOG_WARNING("Audio underrun during playback, recovering...");
            audio_stream_playback_recover(playback_handle, err);
         } else {
            LOG_ERROR("Audio write error: %s", audio_error_string((audio_error_t)err));
            break;
         }
      }
   }

   int stopped_by_user = (getMusicPlay() == 0);

   if (stopped_by_user) {
      LOG_INFO("Playback stopped by user");
   }

cleanup:
   /* Clean up resources */
   free(output_buffer);
   output_buffer = NULL;

   audio_converter_destroy(converter);
   converter = NULL;

   free(read_buffer);
   read_buffer = NULL;

   if (decoder) {
      audio_decoder_close(decoder);
      decoder = NULL;
   }

   /* Handle audio device cleanup based on stop reason */
   if (playback_handle) {
      if (stopped_by_user) {
         /* Drop buffered audio immediately for responsive stop */
         audio_stream_playback_drop(playback_handle);
      } else {
         /* Drain buffered audio for smooth playback finish */
         audio_stream_playback_drain(playback_handle);
      }
      audio_stream_playback_close(playback_handle);
   }

   /* Auto-play next track only if song finished naturally (not manually stopped or error) */
   if (song_finished_naturally) {
      musicCallback("next", NULL, &should_respond);
   }

   /* Free the heap-allocated args (caller allocated, thread frees) */
   free(args);
   return NULL;
}

void setMusicVolume(float val) {
   global_volume = val;
}

float getMusicVolume(void) {
   return global_volume;
}
