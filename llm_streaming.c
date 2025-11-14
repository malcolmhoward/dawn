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

#include "llm_streaming.h"

#include <json-c/json.h>
#include <stdlib.h>
#include <string.h>

#include "logging.h"

#define DEFAULT_ACCUMULATED_CAPACITY 8192
#define MAX_ACCUMULATED_SIZE (10 * 1024 * 1024)  // 10MB hard limit for LLM responses

/**
 * @brief Append text to accumulated response buffer
 */
static int append_to_accumulated(llm_stream_context_t *ctx, const char *text) {
   if (!text || strlen(text) == 0) {
      return 1;
   }

   size_t text_len = strlen(text);
   size_t needed = ctx->accumulated_size + text_len + 1;

   // Prevent runaway memory allocation from excessively long LLM responses
   if (needed > MAX_ACCUMULATED_SIZE) {
      LOG_ERROR("Accumulated response size limit exceeded: %zu bytes, maximum %zu bytes (%.1f MB)",
                needed, MAX_ACCUMULATED_SIZE, MAX_ACCUMULATED_SIZE / (1024.0 * 1024.0));
      return 0;
   }

   // Reallocate if needed
   if (needed > ctx->accumulated_capacity) {
      size_t new_capacity = ctx->accumulated_capacity * 2;
      while (new_capacity < needed) {
         new_capacity *= 2;
      }

      // Cap at maximum size
      if (new_capacity > MAX_ACCUMULATED_SIZE) {
         new_capacity = MAX_ACCUMULATED_SIZE;
      }

      char *new_buffer = realloc(ctx->accumulated_response, new_capacity);
      if (!new_buffer) {
         LOG_ERROR("Failed to reallocate accumulated response buffer");
         return 0;
      }

      ctx->accumulated_response = new_buffer;
      ctx->accumulated_capacity = new_capacity;
   }

   // Append text
   memcpy(ctx->accumulated_response + ctx->accumulated_size, text, text_len);
   ctx->accumulated_size += text_len;
   ctx->accumulated_response[ctx->accumulated_size] = '\0';

   return 1;
}

/**
 * @brief Parse OpenAI/llama.cpp streaming chunk
 *
 * Format: {"choices":[{"delta":{"content":"text"}}]}
 * Or: [DONE]
 */
static void parse_openai_chunk(llm_stream_context_t *ctx, const char *event_data) {
   // Check for [DONE] signal
   if (strcmp(event_data, "[DONE]") == 0) {
      ctx->stream_complete = 1;
      return;
   }

   // Parse JSON
   json_object *chunk = json_tokener_parse(event_data);
   if (!chunk) {
      LOG_WARNING("Failed to parse OpenAI chunk JSON");
      return;
   }

   // Extract choices[0].delta.content
   json_object *choices, *first_choice, *delta, *content;

   if (json_object_object_get_ex(chunk, "choices", &choices) &&
       json_object_get_type(choices) == json_type_array && json_object_array_length(choices) > 0) {
      first_choice = json_object_array_get_idx(choices, 0);

      if (json_object_object_get_ex(first_choice, "delta", &delta)) {
         if (json_object_object_get_ex(delta, "content", &content)) {
            const char *text = json_object_get_string(content);
            if (text && strlen(text) > 0) {
               // Call user callback with chunk
               ctx->callback(text, ctx->callback_userdata);

               // Append to accumulated response
               append_to_accumulated(ctx, text);
            }
         }

         // Check for finish_reason (stream may end without [DONE])
         json_object *finish_reason;
         if (json_object_object_get_ex(first_choice, "finish_reason", &finish_reason)) {
            if (!json_object_is_type(finish_reason, json_type_null)) {
               ctx->stream_complete = 1;
            }
         }
      }
   }

   json_object_put(chunk);
}

/**
 * @brief Parse Claude streaming event
 *
 * Format depends on event type:
 * - message_start: {"type":"message_start",...}
 * - content_block_delta: {"type":"content_block_delta","delta":{"text":"..."}}
 * - message_stop: {"type":"message_stop"}
 */
static void parse_claude_event(llm_stream_context_t *ctx, const char *event_data) {
   json_object *event = json_tokener_parse(event_data);
   if (!event) {
      LOG_WARNING("Failed to parse Claude event JSON");
      return;
   }

   // Get event type
   json_object *type_obj;
   if (!json_object_object_get_ex(event, "type", &type_obj)) {
      json_object_put(event);
      return;
   }

   const char *type = json_object_get_string(type_obj);

   if (strcmp(type, "message_start") == 0) {
      ctx->message_started = 1;
   } else if (strcmp(type, "content_block_start") == 0) {
      ctx->content_block_active = 1;
   } else if (strcmp(type, "content_block_delta") == 0) {
      // Extract delta.text
      json_object *delta, *delta_type_obj, *text_obj;

      if (json_object_object_get_ex(event, "delta", &delta)) {
         // Check delta type
         if (json_object_object_get_ex(delta, "type", &delta_type_obj)) {
            const char *delta_type = json_object_get_string(delta_type_obj);

            if (strcmp(delta_type, "text_delta") == 0) {
               // Extract text
               if (json_object_object_get_ex(delta, "text", &text_obj)) {
                  const char *text = json_object_get_string(text_obj);
                  if (text && strlen(text) > 0) {
                     // Call user callback with chunk
                     ctx->callback(text, ctx->callback_userdata);

                     // Append to accumulated response
                     append_to_accumulated(ctx, text);
                  }
               }
            }
            // Note: other delta types (input_json_delta, thinking_delta) are ignored
         }
      }
   } else if (strcmp(type, "content_block_stop") == 0) {
      ctx->content_block_active = 0;
   } else if (strcmp(type, "message_stop") == 0) {
      ctx->stream_complete = 1;
   }
   // Note: message_delta, ping, and error events are ignored

   json_object_put(event);
}

llm_stream_context_t *llm_stream_create(llm_type_t llm_type,
                                        cloud_provider_t cloud_provider,
                                        text_chunk_callback callback,
                                        void *userdata) {
   if (!callback) {
      LOG_ERROR("LLM stream callback cannot be NULL");
      return NULL;
   }

   llm_stream_context_t *ctx = calloc(1, sizeof(llm_stream_context_t));
   if (!ctx) {
      LOG_ERROR("Failed to allocate LLM stream context");
      return NULL;
   }

   ctx->accumulated_response = malloc(DEFAULT_ACCUMULATED_CAPACITY);
   if (!ctx->accumulated_response) {
      LOG_ERROR("Failed to allocate accumulated response buffer");
      free(ctx);
      return NULL;
   }

   ctx->accumulated_response[0] = '\0';
   ctx->accumulated_capacity = DEFAULT_ACCUMULATED_CAPACITY;
   ctx->accumulated_size = 0;

   ctx->llm_type = llm_type;
   ctx->cloud_provider = cloud_provider;
   ctx->callback = callback;
   ctx->callback_userdata = userdata;
   ctx->message_started = 0;
   ctx->content_block_active = 0;
   ctx->stream_complete = 0;

   return ctx;
}

void llm_stream_free(llm_stream_context_t *ctx) {
   if (!ctx) {
      return;
   }

   free(ctx->accumulated_response);
   free(ctx);
}

void llm_stream_handle_event(llm_stream_context_t *ctx, const char *event_data) {
   if (!ctx || !event_data) {
      return;
   }

   // Route to provider-specific parser
   // Local LLM (llama.cpp) uses OpenAI-compatible format
   // Cloud can be OpenAI or Claude
   if (ctx->llm_type == LLM_LOCAL || ctx->cloud_provider == CLOUD_PROVIDER_OPENAI) {
      parse_openai_chunk(ctx, event_data);
   } else if (ctx->cloud_provider == CLOUD_PROVIDER_CLAUDE) {
      parse_claude_event(ctx, event_data);
   }
}

char *llm_stream_get_response(llm_stream_context_t *ctx) {
   if (!ctx || !ctx->accumulated_response) {
      return NULL;
   }

   return strdup(ctx->accumulated_response);
}

int llm_stream_is_complete(llm_stream_context_t *ctx) {
   if (!ctx) {
      return 0;
   }

   return ctx->stream_complete;
}
