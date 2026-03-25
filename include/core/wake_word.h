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
 * Wake word matching utilities shared between local session and WebUI always-on mode.
 */

#ifndef WAKE_WORD_H
#define WAKE_WORD_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Result of a wake word check
 */
typedef struct {
   bool detected;       /**< True if wake word was found */
   const char *command; /**< Pointer into original text after wake word (NULL if none) */
   bool has_command;    /**< True if non-whitespace text follows the wake word */
   bool is_goodbye;     /**< True if text matches a goodbye phrase */
   bool is_ignore;      /**< True if text matches an ignore phrase */
   bool is_cancel;      /**< True if text matches a cancel phrase */
} wake_word_result_t;

/**
 * @brief Initialize wake word tables from the configured AI name
 *
 * Builds the wake word list by combining prefixes ("hey ", "hello ", etc.)
 * with the AI name. Must be called once at startup after config is loaded.
 *
 * @param ai_name The AI name to use (e.g., "friday")
 */
void wake_word_init(const char *ai_name);

/**
 * @brief Check if text contains a wake word
 *
 * Normalizes the text (lowercase, strip punctuation), then searches for
 * any configured wake word. Also checks for goodbye, cancel, and ignore phrases.
 *
 * @param text The ASR transcript to check
 * @return Result struct with detection status and command pointer
 *
 * @note The command pointer in the result points into the original text string.
 *       It is only valid as long as the original text is valid.
 */
wake_word_result_t wake_word_check(const char *text);

/**
 * @brief Normalize text for wake word matching
 *
 * Converts to lowercase, removes all punctuation, keeps letters/digits/spaces.
 *
 * @param input Original text
 * @return Normalized text (caller must free), or NULL on error
 */
char *wake_word_normalize(const char *input);

#ifdef __cplusplus
}
#endif

#endif /* WAKE_WORD_H */
