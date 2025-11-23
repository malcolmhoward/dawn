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

#ifndef ASR_WHISPER_H
#define ASR_WHISPER_H

#include <stddef.h>
#include <stdint.h>

#include "asr/asr_interface.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize Whisper ASR engine
 *
 * @param model_path Path to Whisper .bin model file
 * @param sample_rate Audio sample rate (must be 16000 for Whisper)
 * @return Opaque context pointer, or NULL on error
 */
void *asr_whisper_init(const char *model_path, int sample_rate);

/**
 * @brief Process audio and get partial result
 *
 * Note: Whisper is not designed for streaming, so this accumulates audio
 * and returns empty partial results. Use finalize() to get actual transcription.
 *
 * @param ctx Whisper context from asr_whisper_init()
 * @param audio PCM audio data (16-bit signed, mono)
 * @param samples Number of samples in audio
 * @return Partial ASR result (empty text), or NULL on error
 */
asr_result_t *asr_whisper_process_partial(void *ctx, const int16_t *audio, size_t samples);

/**
 * @brief Finalize processing and get final result
 *
 * Runs Whisper inference on all accumulated audio and returns transcription.
 *
 * @param ctx Whisper context from asr_whisper_init()
 * @return Final ASR result, or NULL on error
 */
asr_result_t *asr_whisper_finalize(void *ctx);

/**
 * @brief Reset ASR state for new utterance
 *
 * Clears accumulated audio buffer.
 *
 * @param ctx Whisper context from asr_whisper_init()
 * @return 0 on success, non-zero on error
 */
int asr_whisper_reset(void *ctx);

/**
 * @brief Clean up Whisper context
 *
 * @param ctx Whisper context from asr_whisper_init()
 */
void asr_whisper_cleanup(void *ctx);

#ifdef __cplusplus
}
#endif

#endif  // ASR_WHISPER_H
