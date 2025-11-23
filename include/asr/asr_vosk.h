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

#ifndef ASR_VOSK_H
#define ASR_VOSK_H

#include <stddef.h>
#include <stdint.h>

#include "asr/asr_interface.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize Vosk ASR engine
 *
 * @param model_path Path to Vosk model directory
 * @param sample_rate Audio sample rate (typically 16000)
 * @return Opaque context pointer, or NULL on error
 */
void *asr_vosk_init(const char *model_path, int sample_rate);

/**
 * @brief Process audio and get partial result
 *
 * @param ctx Vosk context from asr_vosk_init()
 * @param audio PCM audio data (16-bit signed, mono)
 * @param samples Number of samples in audio
 * @return Partial ASR result, or NULL on error
 */
asr_result_t *asr_vosk_process_partial(void *ctx, const int16_t *audio, size_t samples);

/**
 * @brief Finalize processing and get final result
 *
 * @param ctx Vosk context from asr_vosk_init()
 * @return Final ASR result, or NULL on error
 */
asr_result_t *asr_vosk_finalize(void *ctx);

/**
 * @brief Reset ASR state for new utterance
 *
 * @param ctx Vosk context from asr_vosk_init()
 * @return 0 on success, non-zero on error
 */
int asr_vosk_reset(void *ctx);

/**
 * @brief Clean up Vosk context
 *
 * @param ctx Vosk context from asr_vosk_init()
 */
void asr_vosk_cleanup(void *ctx);

#ifdef __cplusplus
}
#endif

#endif  // ASR_VOSK_H
