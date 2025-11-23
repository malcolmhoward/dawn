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

#include "audio/flac_playback.h"

#include <FLAC/stream_decoder.h>
#include <mosquitto.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef ALSA_DEVICE
#include <alsa/asoundlib.h>
#else
#include <pulse/error.h>
#include <pulse/simple.h>
#endif

#include "logging.h"
#include "mosquitto_comms.h"

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
 * It interleaves the decoded audio samples and writes them to the audio playback stream.
 *
 * @param decoder The FLAC stream decoder instance calling this callback.
 * @param frame The decoded audio frame containing audio samples to be processed.
 * @param buffer An array of pointers to the decoded audio samples for each channel.
 * @param client_data A pointer to user-defined data, used to pass the audio playback handle
 *                    (ALSA snd_pcm_t* or PulseAudio pa_simple*).
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

#ifdef ALSA_DEVICE
   // Cast client_data back to ALSA PCM handle for audio playback.
   snd_pcm_t *pcm_handle = (snd_pcm_t *)client_data;
#else
   // Cast client_data back to pa_simple pointer for audio playback.
   pa_simple *s = (pa_simple *)client_data;
#endif

   // Allocate memory for interleaved audio samples.
   int16_t *interleaved = malloc(frame->header.blocksize * frame->header.channels *
                                 sizeof(int16_t));
   if (!interleaved) {
      LOG_ERROR("Memory allocation failed for interleaved audio buffer.");
      return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
   }

   // Interleave audio samples from all channels into a single buffer.
   for (unsigned i = 0, j = 0; i < frame->header.blocksize; ++i) {
      for (unsigned ch = 0; ch < frame->header.channels; ++ch, ++j) {
         // Adjust the sample volume before interleaving.
         int32_t adjusted_sample = (int32_t)(buffer[ch][i] * global_volume);

         // Clipping protection (optional but recommended).
         if (adjusted_sample < INT16_MIN) {
            adjusted_sample = INT16_MIN;
         } else if (adjusted_sample > INT16_MAX) {
            adjusted_sample = INT16_MAX;
         }

         interleaved[j] = (int16_t)adjusted_sample;
      }
   }

   // Write the interleaved audio samples to the audio stream.
#ifdef ALSA_DEVICE
   snd_pcm_sframes_t frames_written = snd_pcm_writei(pcm_handle, interleaved,
                                                     frame->header.blocksize);
   if (frames_written < 0) {
      // Handle buffer underrun
      frames_written = snd_pcm_recover(pcm_handle, frames_written, 0);
      if (frames_written < 0) {
         LOG_ERROR("ALSA write error: %s", snd_strerror(frames_written));
         free(interleaved);
         return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
      }
   }
#else
   if (pa_simple_write(s, interleaved,
                       frame->header.blocksize * frame->header.channels * sizeof(int16_t),
                       NULL) < 0) {
      free(interleaved);
      return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
   }
#endif

   // Free the allocated memory for the interleaved buffer after writing to the audio stream.
   free(interleaved);

   // Signal the FLAC decoder to continue decoding the next frame.
   return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}

void *playFlacAudio(void *arg) {
   int should_respond = 0;
   PlaybackArgs *args = (PlaybackArgs *)arg;
   FLAC__StreamDecoderInitStatus init_status;
   int error = 0;

#ifdef ALSA_DEVICE
   // Initialize ALSA for playback.
   snd_pcm_t *alsa_handle = NULL;
   snd_pcm_hw_params_t *hw_params = NULL;
   unsigned int sample_rate = 44100;
   int dir = 0;

   // Open ALSA playback device
   if ((error = snd_pcm_open(&alsa_handle, args->sink_name, SND_PCM_STREAM_PLAYBACK, 0)) < 0) {
      LOG_ERROR("Error opening ALSA device %s for playback: %s", args->sink_name,
                snd_strerror(error));
      return NULL;
   }

   // Allocate hardware parameters object
   snd_pcm_hw_params_alloca(&hw_params);
   snd_pcm_hw_params_any(alsa_handle, hw_params);

   // Set hardware parameters
   snd_pcm_hw_params_set_access(alsa_handle, hw_params, SND_PCM_ACCESS_RW_INTERLEAVED);
   snd_pcm_hw_params_set_format(alsa_handle, hw_params, SND_PCM_FORMAT_S16_LE);
   snd_pcm_hw_params_set_channels(alsa_handle, hw_params, 2);
   snd_pcm_hw_params_set_rate_near(alsa_handle, hw_params, &sample_rate, &dir);

   if ((error = snd_pcm_hw_params(alsa_handle, hw_params)) < 0) {
      LOG_ERROR("Error setting ALSA hardware parameters: %s", snd_strerror(error));
      snd_pcm_close(alsa_handle);
      return NULL;
   }

   // Prepare the PCM device
   if ((error = snd_pcm_prepare(alsa_handle)) < 0) {
      LOG_ERROR("Error preparing ALSA device: %s", snd_strerror(error));
      snd_pcm_close(alsa_handle);
      return NULL;
   }
#else
   // Initialize PulseAudio for playback.
   pa_simple *pa_handle = NULL;
   pa_sample_spec ss = { .format = PA_SAMPLE_S16LE, .channels = 2, .rate = 44100 };

   // Open PulseAudio for playback.
   if (!(pa_handle = pa_simple_new(NULL, "FLAC Player", PA_STREAM_PLAYBACK, args->sink_name,
                                   "playback", &ss, NULL, NULL, &error))) {
      LOG_ERROR("Error opening PulseAudio for playback: %s", pa_strerror(error));
      return NULL;
   }
#endif

   // Initialize FLAC decoder.
   FLAC__StreamDecoder *decoder = FLAC__stream_decoder_new();
   if (decoder == NULL) {
      LOG_ERROR("Error creating FLAC decoder.");
#ifdef ALSA_DEVICE
      snd_pcm_close(alsa_handle);
#else
      pa_simple_free(pa_handle);
#endif
      return NULL;
   }

   // Enable MD5 checking for the decoder.
   if (!FLAC__stream_decoder_set_md5_checking(decoder, true)) {
      LOG_ERROR("Error setting FLAC md5 checking.");
      FLAC__stream_decoder_delete(decoder);
#ifdef ALSA_DEVICE
      snd_pcm_close(alsa_handle);
#else
      pa_simple_free(pa_handle);
#endif
      return NULL;
   }

   music_play = 1;  // Ensure playback is enabled.

   // Pass the appropriate audio handle to the FLAC decoder
#ifdef ALSA_DEVICE
   void *audio_handle = alsa_handle;
#else
   void *audio_handle = pa_handle;
#endif

   if ((init_status = FLAC__stream_decoder_init_file(decoder, args->file_name, write_callback,
                                                     metadata_callback, error_callback,
                                                     audio_handle)) !=
       FLAC__STREAM_DECODER_INIT_STATUS_OK) {
      LOG_ERROR("ERROR: initializing decoder: %s",
                FLAC__StreamDecoderInitStatusString[init_status]);
      FLAC__stream_decoder_delete(decoder);
#ifdef ALSA_DEVICE
      snd_pcm_close(alsa_handle);
#else
      pa_simple_free(pa_handle);
#endif
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

#ifdef ALSA_DEVICE
   if (stopped_by_user) {
      // Drop buffered audio immediately for responsive stop
      snd_pcm_drop(alsa_handle);
   } else {
      // Drain buffered audio for smooth playback finish
      snd_pcm_drain(alsa_handle);
   }
   snd_pcm_close(alsa_handle);
#else
   pa_simple_free(pa_handle);
#endif

   // Auto-play next track only if song finished naturally (not manually stopped or error)
   if (error && song_finished_naturally) {
      musicCallback("next", NULL, &should_respond);
   }

   return NULL;
}

void setMusicVolume(float val) {
   global_volume = val;
}
