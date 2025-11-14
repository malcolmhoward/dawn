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

#ifndef LLM_STREAMING_H
#define LLM_STREAMING_H

#include <stddef.h>

#include "llm_interface.h"

/**
 * @brief Callback function type for text chunks from LLM stream
 *
 * Called for each incremental text chunk received from the LLM.
 * The text should be processed immediately (e.g., sent to TTS).
 *
 * @param text Incremental text chunk
 * @param userdata User-provided context pointer
 */
typedef void (*text_chunk_callback)(const char *text, void *userdata);

/**
 * @brief LLM stream context structure
 *
 * Maintains state for processing streaming LLM responses.
 * Extracts text deltas based on provider-specific format.
 */
typedef struct {
   llm_type_t llm_type;             /**< LLM type (LOCAL or CLOUD) */
   cloud_provider_t cloud_provider; /**< Cloud provider (if CLOUD) */
   text_chunk_callback callback;    /**< User callback for text chunks */
   void *callback_userdata;         /**< User context passed to callback */

   // State tracking for Claude
   int message_started;      /**< Claude: message_start received */
   int content_block_active; /**< Claude: content block in progress */

   // Accumulated complete response for conversation history
   char *accumulated_response;
   size_t accumulated_size;
   size_t accumulated_capacity;

   // Stream completion flag
   int stream_complete;
} llm_stream_context_t;

/**
 * @brief Create a new LLM stream context
 *
 * @param llm_type LLM type (LLM_LOCAL or LLM_CLOUD)
 * @param cloud_provider Cloud provider (if LLM_CLOUD)
 * @param callback Function to call for each text chunk
 * @param userdata User context pointer passed to callback
 * @return Newly allocated stream context, or NULL on error
 */
llm_stream_context_t *llm_stream_create(llm_type_t llm_type,
                                        cloud_provider_t cloud_provider,
                                        text_chunk_callback callback,
                                        void *userdata);

/**
 * @brief Free an LLM stream context
 *
 * @param ctx Stream context to free
 */
void llm_stream_free(llm_stream_context_t *ctx);

/**
 * @brief Handle an SSE event from the stream
 *
 * Parses the event data (JSON) and extracts text chunks based on
 * provider-specific format. Calls the text callback for each chunk.
 *
 * @param ctx Stream context
 * @param event_data Event data (JSON string)
 */
void llm_stream_handle_event(llm_stream_context_t *ctx, const char *event_data);

/**
 * @brief Get the complete accumulated response
 *
 * Returns the full text response accumulated from all chunks.
 * This should be called after the stream is complete.
 *
 * @param ctx Stream context
 * @return Complete response string (caller must free), or NULL on error
 */
char *llm_stream_get_response(llm_stream_context_t *ctx);

/**
 * @brief Check if stream is complete
 *
 * @param ctx Stream context
 * @return 1 if stream has completed, 0 otherwise
 */
int llm_stream_is_complete(llm_stream_context_t *ctx);

#endif  // LLM_STREAMING_H
