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

#ifndef LLM_OPENAI_H
#define LLM_OPENAI_H

#include <json-c/json.h>
#include <stddef.h>

/*
 * OpenAI Configuration
 *
 * Model and max_tokens are now configured via g_config.llm.cloud.model
 * and g_config.llm.max_tokens (see config/dawn_config.h).
 *
 * Legacy OPENAI_MODEL and OPENAI_MAX_TOKENS defines have been removed.
 */

/* API endpoint path */
#define OPENAI_CHAT_ENDPOINT "/v1/chat/completions"

/**
 * @brief Callback function type for streaming text chunks
 */
typedef void (*llm_openai_text_chunk_callback)(const char *chunk, void *userdata);

/**
 * @brief OpenAI chat completion (non-streaming)
 *
 * Handles OpenAI-compatible API calls (works for both cloud OpenAI and local LLMs).
 * Supports vision API when vision_image is provided.
 * Conversation history is always in OpenAI format (role/content pairs).
 *
 * @param conversation_history JSON array of messages (OpenAI format)
 * @param input_text User input text
 * @param vision_image Optional base64 image for vision models (NULL if not used)
 * @param vision_image_size Image size in bytes (0 if not used)
 * @param base_url Base URL (cloud: https://api.openai.com, local: http://127.0.0.1:8080)
 * @param api_key API key (NULL for local LLM, required for cloud)
 * @return Response text (caller must free), or NULL on error
 */
char *llm_openai_chat_completion(struct json_object *conversation_history,
                                 const char *input_text,
                                 char *vision_image,
                                 size_t vision_image_size,
                                 const char *base_url,
                                 const char *api_key);

/**
 * @brief OpenAI chat completion with streaming
 *
 * Handles OpenAI-compatible API calls with Server-Sent Events (SSE) streaming.
 * Calls chunk_callback for each incremental text chunk as it arrives.
 * Returns the complete accumulated response when streaming completes.
 *
 * @param conversation_history JSON array of messages (OpenAI format)
 * @param input_text User input text
 * @param vision_image Optional base64 image for vision models (NULL if not used)
 * @param vision_image_size Image size in bytes (0 if not used)
 * @param base_url Base URL (cloud: https://api.openai.com, local: http://127.0.0.1:8080)
 * @param api_key API key (NULL for local LLM, required for cloud)
 * @param chunk_callback Function to call for each text chunk
 * @param callback_userdata User context passed to chunk_callback
 * @return Complete response text (caller must free), or NULL on error
 */
char *llm_openai_chat_completion_streaming(struct json_object *conversation_history,
                                           const char *input_text,
                                           char *vision_image,
                                           size_t vision_image_size,
                                           const char *base_url,
                                           const char *api_key,
                                           llm_openai_text_chunk_callback chunk_callback,
                                           void *callback_userdata);

#endif  // LLM_OPENAI_H
