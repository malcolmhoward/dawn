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

#include "sse_parser.h"

#include <stdlib.h>
#include <string.h>

#include "logging.h"

#define DEFAULT_BUFFER_CAPACITY 65536       // 64KB
#define DEFAULT_EVENT_CAPACITY 4096         // 4KB
#define MAX_BUFFER_SIZE (10 * 1024 * 1024)  // 10MB hard limit to prevent runaway streams

/**
 * @brief Ensure buffer has enough capacity, reallocating if necessary
 */
static int ensure_capacity(char **buffer, size_t *capacity, size_t required) {
   if (required <= *capacity) {
      return 1;  // Already have enough
   }

   // Prevent runaway memory allocation from malicious or buggy streams
   if (required > MAX_BUFFER_SIZE) {
      LOG_ERROR("Buffer size limit exceeded: requested %zu bytes, maximum %zu bytes (%.1f MB)",
                required, MAX_BUFFER_SIZE, MAX_BUFFER_SIZE / (1024.0 * 1024.0));
      return 0;
   }

   size_t new_capacity = *capacity * 2;
   while (new_capacity < required) {
      new_capacity *= 2;
   }

   // Cap at maximum size
   if (new_capacity > MAX_BUFFER_SIZE) {
      new_capacity = MAX_BUFFER_SIZE;
   }

   char *new_buffer = realloc(*buffer, new_capacity);
   if (!new_buffer) {
      LOG_ERROR("Failed to reallocate buffer from %zu to %zu bytes", *capacity, new_capacity);
      return 0;
   }

   *buffer = new_buffer;
   *capacity = new_capacity;
   return 1;
}

/**
 * @brief Append string to a dynamically growing buffer
 */
static int append_to_buffer(char **buffer,
                            size_t *size,
                            size_t *capacity,
                            const char *data,
                            size_t len) {
   if (!ensure_capacity(buffer, capacity, *size + len + 1)) {
      return 0;
   }

   memcpy(*buffer + *size, data, len);
   *size += len;
   (*buffer)[*size] = '\0';
   return 1;
}

sse_parser_t *sse_parser_create(sse_event_callback callback, void *userdata) {
   if (!callback) {
      LOG_ERROR("SSE parser callback cannot be NULL");
      return NULL;
   }

   sse_parser_t *parser = calloc(1, sizeof(sse_parser_t));
   if (!parser) {
      LOG_ERROR("Failed to allocate SSE parser");
      return NULL;
   }

   parser->buffer = malloc(DEFAULT_BUFFER_CAPACITY);
   parser->current_event_type = malloc(DEFAULT_EVENT_CAPACITY);
   parser->current_event_data = malloc(DEFAULT_EVENT_CAPACITY);

   if (!parser->buffer || !parser->current_event_type || !parser->current_event_data) {
      LOG_ERROR("Failed to allocate SSE parser buffers");
      sse_parser_free(parser);
      return NULL;
   }

   parser->buffer[0] = '\0';
   parser->current_event_type[0] = '\0';
   parser->current_event_data[0] = '\0';

   parser->buffer_capacity = DEFAULT_BUFFER_CAPACITY;
   parser->event_type_capacity = DEFAULT_EVENT_CAPACITY;
   parser->event_data_capacity = DEFAULT_EVENT_CAPACITY;
   parser->buffer_size = 0;

   parser->callback = callback;
   parser->callback_userdata = userdata;

   return parser;
}

void sse_parser_free(sse_parser_t *parser) {
   if (!parser) {
      return;
   }

   free(parser->buffer);
   free(parser->current_event_type);
   free(parser->current_event_data);
   free(parser);
}

void sse_parser_reset(sse_parser_t *parser) {
   if (!parser) {
      return;
   }

   parser->buffer_size = 0;
   parser->buffer[0] = '\0';
   parser->current_event_type[0] = '\0';
   parser->current_event_data[0] = '\0';
}

/**
 * @brief Dispatch accumulated event to callback and reset event state
 */
static void dispatch_event(sse_parser_t *parser) {
   // Only dispatch if we have data
   if (strlen(parser->current_event_data) > 0) {
      const char *event_type = strlen(parser->current_event_type) > 0 ? parser->current_event_type
                                                                      : NULL;
      parser->callback(event_type, parser->current_event_data, parser->callback_userdata);
   }

   // Reset event state
   parser->current_event_type[0] = '\0';
   parser->current_event_data[0] = '\0';
}

/**
 * @brief Process a single line from the SSE stream
 */
static void process_line(sse_parser_t *parser, const char *line, size_t len) {
   // Empty line = end of event
   if (len == 0) {
      dispatch_event(parser);
      return;
   }

   // Comment line (ignore)
   if (line[0] == ':') {
      return;
   }

   // Find colon separator
   const char *colon = memchr(line, ':', len);
   if (!colon) {
      // Line without colon - ignore per SSE spec
      return;
   }

   size_t field_len = colon - line;
   const char *value = colon + 1;
   size_t value_len = len - field_len - 1;

   // Skip leading space in value (per SSE spec)
   if (value_len > 0 && value[0] == ' ') {
      value++;
      value_len--;
   }

   // Process field
   if (field_len == 5 && strncmp(line, "event", 5) == 0) {
      // Event type
      if (!ensure_capacity(&parser->current_event_type, &parser->event_type_capacity,
                           value_len + 1)) {
         return;
      }
      memcpy(parser->current_event_type, value, value_len);
      parser->current_event_type[value_len] = '\0';
   } else if (field_len == 4 && strncmp(line, "data", 4) == 0) {
      // Data payload - append (multiple data lines are concatenated with \n)
      size_t current_len = strlen(parser->current_event_data);
      size_t needed = current_len + value_len + 2;  // +1 for \n, +1 for \0

      if (!ensure_capacity(&parser->current_event_data, &parser->event_data_capacity, needed)) {
         return;
      }

      // Append newline if we already have data
      if (current_len > 0) {
         parser->current_event_data[current_len] = '\n';
         current_len++;
      }

      memcpy(parser->current_event_data + current_len, value, value_len);
      parser->current_event_data[current_len + value_len] = '\0';
   }
   // Other fields (id, retry) are ignored for now
}

void sse_parser_feed(sse_parser_t *parser, const char *data, size_t len) {
   if (!parser || !data || len == 0) {
      return;
   }

   // Append to buffer
   if (!append_to_buffer(&parser->buffer, &parser->buffer_size, &parser->buffer_capacity, data,
                         len)) {
      return;
   }

   // Process complete lines
   char *buffer = parser->buffer;
   size_t remaining = parser->buffer_size;
   char *line_start = buffer;

   while (remaining > 0) {
      // Look for \n
      char *newline = memchr(line_start, '\n', remaining);
      if (!newline) {
         // No complete line yet
         break;
      }

      // Calculate line length (excluding \n)
      size_t line_len = newline - line_start;

      // Handle \r\n (trim \r if present)
      if (line_len > 0 && line_start[line_len - 1] == '\r') {
         line_len--;
      }

      // Process this line
      process_line(parser, line_start, line_len);

      // Move to next line
      size_t consumed = (newline - line_start) + 1;
      line_start = newline + 1;
      remaining -= consumed;
   }

   // Move any remaining partial line to start of buffer
   if (line_start != buffer) {
      if (remaining > 0) {
         memmove(buffer, line_start, remaining);
      }
      parser->buffer_size = remaining;
      parser->buffer[remaining] = '\0';
   }
}
