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
 * Chime/alarm tone generation for scheduler notifications
 *
 * Pure math sine-wave synthesis with ADSR envelope. No platform dependencies.
 * Used by both the daemon scheduler and satellite alarm overlay.
 */

#ifndef DAWN_CHIME_H
#define DAWN_CHIME_H

#include <stddef.h>
#include <stdint.h>

#define DAWN_CHIME_SAMPLE_RATE 22050

typedef struct {
   int16_t *pcm;
   size_t samples;
   int sample_rate; /* always DAWN_CHIME_SAMPLE_RATE */
} dawn_chime_buf_t;

/**
 * @brief Generate a 3-note ascending chime (C5/E5/G5, ~750ms)
 * @param out Buffer to fill (pcm allocated internally via calloc)
 * @return 0 on success, 1 on allocation failure
 */
int dawn_chime_generate(dawn_chime_buf_t *out);

/**
 * @brief Generate a 2-note alarm tone (A5/E5, ~500ms)
 * @param out Buffer to fill (pcm allocated internally via calloc)
 * @return 0 on success, 1 on allocation failure
 */
int dawn_alarm_tone_generate(dawn_chime_buf_t *out);

/**
 * @brief Free PCM buffer allocated by generate functions
 * @param buf Buffer to free (safe to call on zeroed struct)
 */
void dawn_chime_free(dawn_chime_buf_t *buf);

/**
 * @brief Apply volume scaling to PCM samples
 * @param dst Output buffer (must hold at least 'samples' int16_t values)
 * @param src Source PCM samples
 * @param samples Number of samples
 * @param vol_scale Volume multiplier (0.0–1.0)
 */
void dawn_chime_apply_volume(int16_t *dst, const int16_t *src, size_t samples, float vol_scale);

#endif /* DAWN_CHIME_H */
