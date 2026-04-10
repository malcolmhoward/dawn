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
 * DAWN Audio Utilities - Shared audio processing functions
 */

#ifndef AUDIO_UTILS_H
#define AUDIO_UTILS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Apply volume scaling to interleaved 16-bit audio samples
 *
 * Scales each sample by the given volume factor with clipping protection.
 * Volume of 0.0 = silence, 1.0 = unity gain. Values above 1.0 amplify
 * and may clip.
 *
 * @param buffer   Sample buffer (interleaved int16_t)
 * @param frames   Number of frames
 * @param channels Number of channels per frame
 * @param volume   Volume factor (0.0 to 1.0+)
 */
static inline void audio_apply_volume(int16_t *buffer,
                                      size_t frames,
                                      unsigned int channels,
                                      float volume) {
   size_t total_samples = frames * channels;

   for (size_t i = 0; i < total_samples; i++) {
      int32_t adjusted = (int32_t)(buffer[i] * volume);

      /* Clipping protection */
      if (adjusted < INT16_MIN) {
         adjusted = INT16_MIN;
      } else if (adjusted > INT16_MAX) {
         adjusted = INT16_MAX;
      }

      buffer[i] = (int16_t)adjusted;
   }
}

#ifdef __cplusplus
}
#endif

#endif /* AUDIO_UTILS_H */
