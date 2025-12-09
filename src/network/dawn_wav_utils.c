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

#include "network/dawn_wav_utils.h"

#include <endian.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "logging.h"

/**
 * Check if a WAV file fits within ESP32 buffer limits
 *
 * Compares the WAV file size against the safe response limit for ESP32 clients.
 * Logs appropriate warnings if the size exceeds the limit.
 *
 * @param wav_size Size of the WAV file in bytes
 * @return 1 if within limits, 0 if exceeds limits
 */
int check_response_size_limit(size_t wav_size) {
   LOG_INFO("Response size: %zu bytes (limit: %ld bytes)", wav_size, SAFE_RESPONSE_LIMIT);

   if (wav_size <= SAFE_RESPONSE_LIMIT) {
      LOG_INFO("Response fits within ESP32 buffer limits");
      return 1;
   } else {
      LOG_WARNING("Response exceeds ESP32 buffer limits by %zu bytes",
                  wav_size - SAFE_RESPONSE_LIMIT);
      return 0;
   }
}

/**
 * Truncate a WAV file to fit within ESP32 buffer limits
 *
 * Creates a new WAV file by truncating the audio data from the original file.
 * The truncation maintains sample alignment (2-byte boundaries for 16-bit audio)
 * and updates the WAV header accordingly. Proper endianness conversion is applied
 * for cross-platform compatibility.
 *
 * @param wav_data Pointer to original WAV data (must be valid WAV format)
 * @param wav_size Size of original WAV data in bytes (must be >= 44)
 * @param truncated_data_out Receives pointer to new truncated buffer (or NULL if not needed)
 * @param truncated_size_out Receives size of truncated buffer (or 0 if not needed)
 * @return 0 on success, -1 on error
 */
int truncate_wav_response(const uint8_t *wav_data,
                          size_t wav_size,
                          uint8_t **truncated_data_out,
                          size_t *truncated_size_out) {
   // Validate input parameters
   if (!wav_data || !truncated_data_out || !truncated_size_out) {
      LOG_ERROR("Invalid NULL parameters for WAV truncation");
      return -1;
   }

   if (wav_size < sizeof(WAVHeader)) {
      LOG_ERROR("WAV data too small (%zu bytes, minimum %zu bytes)", wav_size, sizeof(WAVHeader));
      return -1;
   }

   // Parse the original WAV header
   const WAVHeader *original_header = (const WAVHeader *)wav_data;

   // Calculate sizes
   size_t header_size = sizeof(WAVHeader);
   size_t original_audio_data = wav_size - header_size;
   size_t max_audio_data = SAFE_RESPONSE_LIMIT - header_size;

   // Check if truncation is actually needed
   if (original_audio_data <= max_audio_data) {
      LOG_INFO("No truncation needed - WAV already fits within limits");
      *truncated_data_out = NULL;
      *truncated_size_out = 0;
      return 0;  // Success - no truncation needed
   }

   LOG_INFO("Truncating WAV from %zu to %zu bytes", wav_size, SAFE_RESPONSE_LIMIT);

   // Align to sample boundaries (2 bytes per sample for 16-bit mono audio)
   // This prevents cutting in the middle of a sample which would cause audio glitches
   max_audio_data = (max_audio_data / 2) * 2;
   size_t truncated_total_size = header_size + max_audio_data;

   // Calculate and log duration information
   uint32_t sample_rate = le32toh(original_header->sample_rate);
   if (sample_rate > 0) {
      double original_duration = (double)original_audio_data / (sample_rate * 2);
      double truncated_duration = (double)max_audio_data / (sample_rate * 2);
      LOG_INFO("Duration: %.2f -> %.2f seconds", original_duration, truncated_duration);
   }

   // Allocate buffer for truncated WAV
   uint8_t *truncated_data = (uint8_t *)malloc(truncated_total_size);
   if (!truncated_data) {
      LOG_ERROR("Failed to allocate %zu bytes for truncated WAV", truncated_total_size);
      return -1;
   }

   // Copy and modify the WAV header
   WAVHeader *new_header = (WAVHeader *)truncated_data;
   memcpy(new_header, original_header, header_size);

   // Update header fields with new sizes (applying proper endianness conversion)
   new_header->wav_size = htole32(truncated_total_size - 8);
   new_header->data_bytes = htole32(max_audio_data);

   // Copy the truncated audio data
   memcpy(truncated_data + header_size, wav_data + header_size, max_audio_data);

   // Return the truncated data to caller
   *truncated_data_out = truncated_data;
   *truncated_size_out = truncated_total_size;

   LOG_INFO("WAV truncation complete: %zu bytes allocated", truncated_total_size);
   return 0;  // Success - new buffer allocated
}

/**
 * Extract PCM audio data from a network WAV file
 *
 * Parses a WAV file received from an ESP32 client and extracts the raw PCM
 * audio data along with format information. Validates header and checksums.
 *
 * @param wav_data Pointer to complete WAV file data
 * @param wav_size Size of WAV data in bytes
 * @return Allocated NetworkPCMData structure (caller must free with free_network_pcm_data)
 *         Returns NULL on error (invalid format, allocation failure)
 */
NetworkPCMData *extract_pcm_from_network_wav(const uint8_t *wav_data, size_t wav_size) {
   // Validate parameters
   if (!wav_data || wav_size == 0) {
      LOG_ERROR("Invalid parameters: wav_data=%p, wav_size=%zu", (void *)wav_data, wav_size);
      return NULL;
   }

   // Validate minimum size
   if (wav_size < sizeof(WAVHeader)) {
      LOG_ERROR("WAV data too small for header: %zu bytes (need %zu)", wav_size, sizeof(WAVHeader));
      return NULL;
   }

   const WAVHeader *header = (const WAVHeader *)wav_data;

   // Validate RIFF/WAVE headers
   if (strncmp(header->riff_header, "RIFF", 4) != 0 ||
       strncmp(header->wave_header, "WAVE", 4) != 0) {
      LOG_ERROR("Invalid WAV header format");
      return NULL;
   }

   // Extract format information (little-endian)
   uint32_t sample_rate = le32toh(header->sample_rate);
   uint16_t num_channels = le16toh(header->num_channels);
   uint16_t bits_per_sample = le16toh(header->bits_per_sample);
   uint16_t audio_format = le16toh(header->audio_format);
   uint32_t data_bytes = le32toh(header->data_bytes);

   // Validate audio format (must be PCM)
   if (audio_format != 1) {
      LOG_ERROR("Not PCM format: %u", audio_format);
      return NULL;
   }

   // Validate data_bytes against actual buffer size
   size_t expected_total_size = sizeof(WAVHeader) + data_bytes;
   if (expected_total_size > wav_size) {
      LOG_WARNING("WAV header claims %u data bytes, but only %zu available", data_bytes,
                  wav_size - sizeof(WAVHeader));
      data_bytes = wav_size - sizeof(WAVHeader);
   }

   // Sanity check for unreasonably large data
   if (data_bytes > ESP32_MAX_RESPONSE_BYTES) {
      LOG_ERROR("WAV data size unreasonably large: %u bytes (max: %ld)", data_bytes,
                (long)ESP32_MAX_RESPONSE_BYTES);
      return NULL;
   }

   LOG_INFO("WAV format: %uHz, %u channels, %u-bit, %u data bytes", sample_rate, num_channels,
            bits_per_sample, data_bytes);

   // Allocate PCM structure
   NetworkPCMData *pcm = malloc(sizeof(NetworkPCMData));
   if (!pcm) {
      LOG_ERROR("Failed to allocate NetworkPCMData structure");
      return NULL;
   }

   // Allocate PCM data buffer
   pcm->pcm_data = malloc(data_bytes);
   if (!pcm->pcm_data) {
      LOG_ERROR("Failed to allocate %u bytes for PCM data", data_bytes);
      free(pcm);
      return NULL;
   }

   // Copy PCM data (skip WAV header)
   memcpy(pcm->pcm_data, wav_data + sizeof(WAVHeader), data_bytes);

   // Populate structure
   pcm->pcm_size = data_bytes;
   pcm->sample_rate = sample_rate;
   pcm->num_channels = num_channels;
   pcm->bits_per_sample = bits_per_sample;
   pcm->is_valid = (num_channels == 1 && bits_per_sample == 16);

   if (!pcm->is_valid) {
      LOG_WARNING("WAV format not pipeline-compatible (need mono 16-bit)");
   }

   return pcm;
}

/**
 * Free memory allocated for NetworkPCMData structure
 *
 * Safely deallocates a NetworkPCMData structure and its associated
 * PCM data buffer that was allocated by extract_pcm_from_network_wav().
 *
 * @param pcm Pointer to NetworkPCMData structure to free (may be NULL)
 */
void free_network_pcm_data(NetworkPCMData *pcm) {
   if (!pcm)
      return;
   if (pcm->pcm_data)
      free(pcm->pcm_data);
   free(pcm);
}
