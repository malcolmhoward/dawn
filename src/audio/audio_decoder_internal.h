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
 * DAWN Audio Decoder - Internal Header
 *
 * Internal definitions shared between audio_decoder.c and format-specific
 * decoder implementations. NOT for use by external code.
 */

#ifndef AUDIO_DECODER_INTERNAL_H
#define AUDIO_DECODER_INTERNAL_H

#include "audio/audio_decoder.h"

#ifdef __cplusplus
extern "C" {
#endif

/* =============================================================================
 * Internal Vtable Definition
 * ============================================================================= */

/**
 * @brief Internal vtable for format-specific decoder operations
 *
 * Each decoder (FLAC, MP3, Ogg) provides an implementation of these functions.
 */
typedef struct audio_decoder_vtable {
   const char *name;           /**< Format name for logging */
   const char **extensions;    /**< NULL-terminated array of extensions */
   audio_format_type_t format; /**< Format type enum */

   /* Operations */
   audio_decoder_t *(*open)(const char *path);
   void (*close)(audio_decoder_t *dec);
   int (*get_info)(audio_decoder_t *dec, audio_decoder_info_t *info);
   ssize_t (*read)(audio_decoder_t *dec, int16_t *buffer, size_t max_frames);
   int (*seek)(audio_decoder_t *dec, uint64_t sample_pos);
} audio_decoder_vtable_t;

/* =============================================================================
 * Base Decoder Structure
 * ============================================================================= */

/**
 * @brief Base structure for all decoder handles
 *
 * Format-specific decoders embed this as the first member of their handle struct.
 * This allows safe casting from audio_decoder_t* to format-specific handles.
 *
 * Example usage in format-specific decoder:
 * @code
 * typedef struct {
 *    audio_decoder_base_t base;  // Must be first member
 *    // Format-specific fields...
 * } my_decoder_handle_t;
 * @endcode
 */
typedef struct audio_decoder {
   const audio_decoder_vtable_t *vtable;
   audio_format_type_t format;
} audio_decoder_base_t;

#ifdef __cplusplus
}
#endif

#endif /* AUDIO_DECODER_INTERNAL_H */
