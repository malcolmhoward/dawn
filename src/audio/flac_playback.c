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
 * DAWN FLAC Playback Module
 *
 * Decodes and plays FLAC audio files with automatic sample rate conversion.
 *
 * ALSA/dmix Note:
 *   For FLAC playback to work with ALSA, a dmix-compatible output device
 *   should be used (e.g., "default" or a dmix plugin). The dmix plugin
 *   allows multiple applications to share the sound card by mixing audio
 *   in software. Hardware devices (hw:X,Y) do not support mixing.
 *
 *   The configured output rate (audio.output_rate in dawn.toml) is requested
 *   from ALSA, but the actual rate may differ if the hardware doesn't support
 *   it. For example, configuring 44100 Hz may result in 48000 Hz if dmix is
 *   configured for 48000 Hz. The converter automatically adapts to the actual
 *   hardware rate returned by ALSA.
 */

#include "audio/flac_playback.h"

#include <FLAC/stream_decoder.h>
#include <mosquitto.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio/audio_backend.h"
#include "audio/audio_converter.h"
#include "logging.h"
#include "mosquitto_comms.h"

/* Maximum channels supported for FLAC playback (stereo) */
#define FLAC_MAX_CHANNELS 2

/**
 * @brief Context passed to FLAC callbacks for audio playback
 *
 * Contains all state needed by write_callback to output audio at configured rate.
 * All buffers are pre-allocated to avoid malloc in the hot path.
 */
typedef struct {
   audio_stream_playback_handle_t *playback_handle;
   audio_converter_t *converter;
   int16_t *interleaved_buffer;      /* Pre-allocated interleave buffer (input) */
   size_t interleaved_buffer_frames; /* Size of interleave buffer in frames */
   int16_t *output_buffer;           /* Pre-allocated conversion output buffer */
   size_t output_buffer_frames;      /* Size of output buffer in frames */
} flac_playback_context_t;

/**
 * @var float global_volume
 * @brief Global volume control for audio playback.
 *
 * This variable controls the volume level of audio playback throughout the application.
 * The volume level is represented as a floating-point number between 0.0 and 1.0,
 * where 0.0 means silence and 1.0 corresponds to the maximum volume level without amplification.
 * Values above 1.0 may result in amplification and potentially introduce distortion or clipping.
 *
 * Usage:
 * Assign a value to this variable to adjust the playback volume before starting or during audio
 * playback. For example, setting `global_volume = 0.75;` adjusts the volume to 75% of the maximum
 * level.
 */
static float global_volume = 0.5f;

// Global variable to control music playback state.
// When set to 0, music playback is stopped.
// When set to 1, music playback is active.
static int music_play = 0;

void setMusicPlay(int play) {
   music_play = play;
}

int getMusicPlay(void) {
   return music_play;
}

/**
 * Callback function for processing FLAC metadata.
 * This function is called by the FLAC decoder when metadata is encountered in the FLAC stream.
 *
 * @param decoder The FLAC stream decoder instance.
 * @param metadata The metadata object encountered in the stream.
 * @param client_data Optional user data provided to the decoder.
 */
void metadata_callback(const FLAC__StreamDecoder *decoder,
                       const FLAC__StreamMetadata *metadata,
                       void *client_data) {
   if (metadata->type == FLAC__METADATA_TYPE_STREAMINFO) {
      FLAC__StreamMetadata_StreamInfo info = metadata->data.stream_info;
      LOG_INFO("Sample rate: %u Hz", info.sample_rate);
      LOG_INFO("Channels: %u", info.channels);
      LOG_INFO("Bits per sample: %u", info.bits_per_sample);
   } else if (metadata->type == FLAC__METADATA_TYPE_PICTURE) {
      LOG_INFO("*** Got FLAC__METADATA_TYPE_PICTURE.");
      FLAC__StreamMetadata_Picture picture = metadata->data.picture;

      if (picture.type == FLAC__STREAM_METADATA_PICTURE_TYPE_FRONT_COVER) {
         LOG_INFO("Found front cover art. MIME type: %s", picture.mime_type);

         // Now, picture.data contains the image data of length picture.data_length
         // You can write this data to a file or use it as needed.
      }
   }
}

/**
 * Callback function for handling errors during FLAC decoding.
 * This function is called by the FLAC decoder when an error occurs.
 *
 * @param decoder The FLAC stream decoder instance.
 * @param status The error status code indicating the type of error.
 * @param client_data Optional user data provided to the decoder.
 */
void error_callback(const FLAC__StreamDecoder *decoder,
                    FLAC__StreamDecoderErrorStatus status,
                    void *client_data) {
   const char *status_str = FLAC__StreamDecoderErrorStatusString[status];
   LOG_ERROR("FLAC Error callback: %s", status_str);
}

/**
 * Callback function for handling decoded audio frames from the FLAC stream.
 * This function is called by the FLAC decoder each time an audio frame is successfully decoded.
 * It interleaves the decoded audio samples, converts to 48kHz stereo, and writes to playback.
 *
 * @param decoder The FLAC stream decoder instance calling this callback.
 * @param frame The decoded audio frame containing audio samples to be processed.
 * @param buffer An array of pointers to the decoded audio samples for each channel.
 * @param client_data A pointer to flac_playback_context_t with playback handle and converter.
 *
 * @return A FLAC__StreamDecoderWriteStatus value indicating whether the write operation was
 * successful. Returns FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE to continue decoding, or
 * FLAC__STREAM_DECODER_WRITE_STATUS_ABORT to stop decoding due to an error.
 */
FLAC__StreamDecoderWriteStatus write_callback(const FLAC__StreamDecoder *decoder,
                                              const FLAC__Frame *frame,
                                              const FLAC__int32 *const buffer[],
                                              void *client_data) {
   // Check if music playback has been stopped externally.
   if (!music_play) {
      LOG_WARNING("Stop playback requested.");
      return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
   }

   // Cast client_data to playback context
   flac_playback_context_t *ctx = (flac_playback_context_t *)client_data;

   // Validate block size fits in pre-allocated buffer
   if (frame->header.blocksize > ctx->interleaved_buffer_frames) {
      LOG_ERROR("FLAC block size %u exceeds buffer %zu", frame->header.blocksize,
                ctx->interleaved_buffer_frames);
      return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
   }

   // Validate channel count (buffer only supports up to FLAC_MAX_CHANNELS)
   if (frame->header.channels > FLAC_MAX_CHANNELS) {
      LOG_ERROR("FLAC has %u channels, max supported is %d", frame->header.channels,
                FLAC_MAX_CHANNELS);
      return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
   }

   // Use pre-allocated interleaved buffer (no malloc in hot path)
   int16_t *interleaved = ctx->interleaved_buffer;

   // Interleave audio samples from all channels into a single buffer.
   for (unsigned i = 0, j = 0; i < frame->header.blocksize; ++i) {
      for (unsigned ch = 0; ch < frame->header.channels; ++ch, ++j) {
         // Adjust the sample volume before interleaving.
         int32_t adjusted_sample = (int32_t)(buffer[ch][i] * global_volume);

         // Clipping protection
         if (adjusted_sample < INT16_MIN) {
            adjusted_sample = INT16_MIN;
         } else if (adjusted_sample > INT16_MAX) {
            adjusted_sample = INT16_MAX;
         }

         interleaved[j] = (int16_t)adjusted_sample;
      }
   }

   // Convert to configured output rate/channels if converter is available
   ssize_t frames_written;
   if (ctx->converter) {
      ssize_t converted_frames = audio_converter_process(ctx->converter, interleaved,
                                                         frame->header.blocksize,
                                                         ctx->output_buffer,
                                                         ctx->output_buffer_frames);
      if (converted_frames < 0) {
         LOG_ERROR("Audio conversion failed");
         return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
      }

      // Write converted audio
      frames_written = audio_stream_playback_write(ctx->playback_handle, ctx->output_buffer,
                                                   (size_t)converted_frames);
   } else {
      // No conversion needed - write directly
      frames_written = audio_stream_playback_write(ctx->playback_handle, interleaved,
                                                   frame->header.blocksize);
   }

   if (frames_written < 0) {
      int err = (int)(-frames_written);
      // Attempt recovery from underrun
      if (err == AUDIO_ERR_UNDERRUN) {
         LOG_WARNING("Audio underrun during FLAC playback, recovering...");
         audio_stream_playback_recover(ctx->playback_handle, err);
      } else {
         LOG_ERROR("Audio write error: %s", audio_error_string((audio_error_t)err));
         return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
      }
   }

   return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}

void *playFlacAudio(void *arg) {
   int should_respond = 0;
   PlaybackArgs *args = (PlaybackArgs *)arg;
   FLAC__StreamDecoderInitStatus init_status;
   int error = 0;

   // Check that audio backend is initialized
   if (audio_backend_get_type() == AUDIO_BACKEND_NONE) {
      LOG_ERROR("Audio backend not initialized. Call audio_backend_init() first.");
      free(args);
      return NULL;
   }

   // Get configured output rate/channels for dmix compatibility
   unsigned int output_rate = audio_conv_get_output_rate();
   unsigned int output_channels = audio_conv_get_output_channels();

   // Open playback device at configured rate for dmix compatibility
   // FLAC music files (typically 44100Hz) will be resampled if needed
   audio_stream_params_t params = { .sample_rate = output_rate,
                                    .channels = output_channels,
                                    .format = AUDIO_FORMAT_S16_LE,
                                    .period_frames = 1024,
                                    .buffer_frames = 4096 };
   audio_hw_params_t hw_params;

   LOG_INFO("Opening FLAC playback device: %s (backend: %s)", args->sink_name,
            audio_backend_type_name(audio_backend_get_type()));

   audio_stream_playback_handle_t *playback_handle = audio_stream_playback_open(args->sink_name,
                                                                                &params,
                                                                                &hw_params);
   if (!playback_handle) {
      LOG_ERROR("Error opening audio device %s for FLAC playback", args->sink_name);
      free(args);
      return NULL;
   }

   // Use actual hardware rate/channels (may differ from requested if ALSA fell back)
   unsigned int actual_rate = hw_params.sample_rate;
   unsigned int actual_channels = hw_params.channels;

   LOG_INFO("FLAC playback: requested=%uHz/%uch actual=%uHz/%uch", output_rate, output_channels,
            actual_rate, actual_channels);

   // Create playback context with pre-allocated buffers (no malloc in hot path)
   flac_playback_context_t ctx = { .playback_handle = playback_handle,
                                   .converter = NULL,
                                   .interleaved_buffer = NULL,
                                   .interleaved_buffer_frames = 0,
                                   .output_buffer = NULL,
                                   .output_buffer_frames = 0 };

   // Pre-allocate interleaved buffer for write_callback (avoids malloc in hot path)
   // Uses AUDIO_CONV_MAX_INPUT_FRAMES to stay consistent with audio_converter limits
   ctx.interleaved_buffer_frames = AUDIO_CONV_MAX_INPUT_FRAMES;
   ctx.interleaved_buffer = malloc(AUDIO_CONV_MAX_INPUT_FRAMES * FLAC_MAX_CHANNELS *
                                   sizeof(int16_t));
   if (!ctx.interleaved_buffer) {
      LOG_ERROR("Failed to allocate interleaved buffer for FLAC playback");
      audio_stream_playback_close(playback_handle);
      free(args);
      return NULL;
   }

   // Create converter for 44100Hz stereo -> actual ALSA output (common case for FLAC files)
   // Use actual_rate/actual_channels from hw_params, not config, since ALSA may have fallen back
   // If FLAC file has different parameters, write_callback handles it
   audio_converter_params_t conv_params = { .sample_rate = 44100, .channels = 2 };
   if (audio_converter_needed_ex(&conv_params, actual_rate, actual_channels)) {
      ctx.converter = audio_converter_create_ex(&conv_params, actual_rate, actual_channels);
      if (!ctx.converter) {
         LOG_ERROR("Failed to create audio converter for FLAC playback");
         free(ctx.interleaved_buffer);
         audio_stream_playback_close(playback_handle);
         free(args);
         return NULL;
      }

      // Allocate output buffer for converted audio (use actual channels from ALSA)
      ctx.output_buffer_frames = audio_converter_max_output_frames(ctx.converter,
                                                                   AUDIO_CONV_MAX_INPUT_FRAMES);
      ctx.output_buffer = malloc(ctx.output_buffer_frames * actual_channels * sizeof(int16_t));
      if (!ctx.output_buffer) {
         LOG_ERROR("Failed to allocate output buffer for FLAC playback");
         audio_converter_destroy(ctx.converter);
         free(ctx.interleaved_buffer);
         audio_stream_playback_close(playback_handle);
         free(args);
         return NULL;
      }
   }

   // Initialize FLAC decoder.
   FLAC__StreamDecoder *decoder = FLAC__stream_decoder_new();
   if (decoder == NULL) {
      LOG_ERROR("Error creating FLAC decoder.");
      free(ctx.output_buffer);
      audio_converter_destroy(ctx.converter);
      free(ctx.interleaved_buffer);
      audio_stream_playback_close(playback_handle);
      free(args);
      return NULL;
   }

   // Enable MD5 checking for the decoder.
   if (!FLAC__stream_decoder_set_md5_checking(decoder, true)) {
      LOG_ERROR("Error setting FLAC md5 checking.");
      FLAC__stream_decoder_delete(decoder);
      free(ctx.output_buffer);
      audio_converter_destroy(ctx.converter);
      free(ctx.interleaved_buffer);
      audio_stream_playback_close(playback_handle);
      free(args);
      return NULL;
   }

   music_play = 1;  // Ensure playback is enabled.

   // Pass the playback context to the FLAC decoder (contains playback handle + converter)
   if ((init_status = FLAC__stream_decoder_init_file(decoder, args->file_name, write_callback,
                                                     metadata_callback, error_callback, &ctx)) !=
       FLAC__STREAM_DECODER_INIT_STATUS_OK) {
      LOG_ERROR("ERROR: initializing decoder: %s",
                FLAC__StreamDecoderInitStatusString[init_status]);
      FLAC__stream_decoder_delete(decoder);
      free(ctx.output_buffer);
      audio_converter_destroy(ctx.converter);
      free(ctx.interleaved_buffer);
      audio_stream_playback_close(playback_handle);
      free(args);
      return NULL;
   }

   // Process the FLAC stream frame by frame, checking for stop signal
   error = 1;  // Assume success unless we get an error
   while (getMusicPlay() &&
          FLAC__stream_decoder_get_state(decoder) < FLAC__STREAM_DECODER_END_OF_STREAM) {
      if (!FLAC__stream_decoder_process_single(decoder)) {
         LOG_ERROR("Error during FLAC frame decoding.");
         error = 0;
         break;
      }
   }

   int stopped_by_user = (getMusicPlay() == 0);

   int song_finished_naturally = 0;

   if (stopped_by_user) {
      LOG_INFO("Playback stopped by user.");
   } else if (FLAC__stream_decoder_get_state(decoder) == FLAC__STREAM_DECODER_END_OF_STREAM) {
      LOG_INFO("Decoding completed successfully.");
      song_finished_naturally = 1;
   }

   // Cleanup and resource management.
   FLAC__stream_decoder_finish(decoder);
   FLAC__stream_decoder_delete(decoder);

   // Clean up audio converter and pre-allocated buffers
   free(ctx.output_buffer);
   ctx.output_buffer = NULL;
   audio_converter_destroy(ctx.converter);
   ctx.converter = NULL;
   free(ctx.interleaved_buffer);
   ctx.interleaved_buffer = NULL;

   // Handle audio device cleanup based on stop reason
   if (stopped_by_user) {
      // Drop buffered audio immediately for responsive stop
      audio_stream_playback_drop(ctx.playback_handle);
   } else {
      // Drain buffered audio for smooth playback finish
      audio_stream_playback_drain(ctx.playback_handle);
   }
   audio_stream_playback_close(ctx.playback_handle);

   // Auto-play next track only if song finished naturally (not manually stopped or error)
   if (error && song_finished_naturally) {
      musicCallback("next", NULL, &should_respond);
   }

   // Free the heap-allocated args (caller allocated, thread frees)
   free(args);
   return NULL;
}

void setMusicVolume(float val) {
   global_volume = val;
}

float getMusicVolume(void) {
   return global_volume;
}
