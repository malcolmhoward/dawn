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
 * Text filtering utilities for command tag stripping.
 */

#ifndef TEXT_FILTER_H
#define TEXT_FILTER_H

#include <stdbool.h>
#include <stddef.h>

/* =============================================================================
 * Command Tag Constants
 *
 * Shared between text_filter and webui_server to ensure consistency.
 * ============================================================================= */

#define CMD_TAG_OPEN "<command>"
#define CMD_TAG_CLOSE "</command>"
#define CMD_TAG_OPEN_LEN 9
#define CMD_TAG_CLOSE_LEN 10
#define CMD_TAG_BUF_SIZE 16     /* Enough for "</command>" (10) + margin */
#define CMD_TAG_MAX_NESTING 100 /* Max nesting depth to prevent overflow */

/**
 * @brief Command tag filter state
 *
 * Tracks state for filtering <command>...</command> tags from streaming text.
 * Must be initialized to zero before first use. Supports nested tags.
 */
typedef struct {
   char buffer[CMD_TAG_BUF_SIZE]; /**< Buffer for partial tag detection */
   unsigned char len;             /**< Current length of partial tag buffer */
   int nesting_depth;             /**< Nesting depth (0 = outside tags, >0 = inside) */
} cmd_tag_filter_state_t;

/**
 * @brief Output callback type for command tag filter
 *
 * @param text Filtered text chunk
 * @param len Length of text chunk
 * @param ctx User context
 */
typedef void (*text_filter_output_fn)(const char *text, size_t len, void *ctx);

/**
 * @brief Filter <command>...</command> tags from streaming text
 *
 * Uses a character-by-character state machine that handles partial tags
 * spanning chunk boundaries. Filtered text is emitted via callback.
 *
 * @param state Filter state (must be zeroed before first chunk)
 * @param text Input text to filter
 * @param output_fn Callback to emit filtered text
 * @param ctx User context passed to output_fn
 *
 * @note If stream ends with a partial tag buffer, content is silently dropped.
 */
void text_filter_command_tags(cmd_tag_filter_state_t *state,
                              const char *text,
                              text_filter_output_fn output_fn,
                              void *ctx);

/**
 * @brief Filter command tags into a buffer
 *
 * Convenience wrapper that filters text into a fixed-size buffer.
 *
 * @param state Filter state (must be zeroed before first chunk)
 * @param text Input text to filter
 * @param out_buf Output buffer
 * @param out_size Size of output buffer
 * @return Length of filtered text written (excluding null terminator)
 */
int text_filter_command_tags_to_buffer(cmd_tag_filter_state_t *state,
                                       const char *text,
                                       char *out_buf,
                                       size_t out_size);

/**
 * @brief Reset filter state
 *
 * Call this when starting a new stream or to clear partial tag state.
 *
 * @param state Filter state to reset
 */
void text_filter_reset(cmd_tag_filter_state_t *state);

#endif /* TEXT_FILTER_H */
