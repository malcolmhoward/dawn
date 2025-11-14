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

#ifndef SSE_PARSER_H
#define SSE_PARSER_H

#include <stddef.h>

/**
 * @brief Callback function type for SSE events
 *
 * Called for each complete SSE event parsed from the stream.
 *
 * @param event_type The event type (e.g., "message", "ping"), or NULL for default
 * @param event_data The data payload of the event
 * @param userdata User-provided context pointer
 */
typedef void (*sse_event_callback)(const char *event_type, const char *event_data, void *userdata);

/**
 * @brief SSE parser context structure
 *
 * Maintains state for parsing Server-Sent Events from streaming HTTP responses.
 * Buffers partial events and calls the callback for each complete event.
 */
typedef struct {
   char *buffer;                /**< Accumulation buffer for partial events */
   size_t buffer_size;          /**< Current size of buffered data */
   size_t buffer_capacity;      /**< Total capacity of buffer */
   sse_event_callback callback; /**< User callback for complete events */
   void *callback_userdata;     /**< User context passed to callback */

   // Event state
   char *current_event_type;   /**< Type of event being accumulated */
   char *current_event_data;   /**< Data of event being accumulated */
   size_t event_type_capacity; /**< Capacity of event_type buffer */
   size_t event_data_capacity; /**< Capacity of event_data buffer */
} sse_parser_t;

/**
 * @brief Create a new SSE parser
 *
 * @param callback Function to call for each complete event
 * @param userdata User context pointer passed to callback
 * @return Newly allocated parser, or NULL on error
 */
sse_parser_t *sse_parser_create(sse_event_callback callback, void *userdata);

/**
 * @brief Free an SSE parser and all associated resources
 *
 * @param parser Parser to free
 */
void sse_parser_free(sse_parser_t *parser);

/**
 * @brief Feed data to the SSE parser
 *
 * Processes incoming chunks from the HTTP stream. May call the event callback
 * zero or more times depending on how many complete events are in the data.
 *
 * Handles partial events across multiple feed() calls.
 *
 * @param parser Parser instance
 * @param data Incoming data chunk
 * @param len Length of data in bytes
 */
void sse_parser_feed(sse_parser_t *parser, const char *data, size_t len);

/**
 * @brief Reset parser state
 *
 * Clears all buffered data and resets to initial state.
 * Useful for reusing a parser for a new stream.
 *
 * @param parser Parser to reset
 */
void sse_parser_reset(sse_parser_t *parser);

#endif  // SSE_PARSER_H
