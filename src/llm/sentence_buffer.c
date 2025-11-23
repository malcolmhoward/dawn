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

#include "llm/sentence_buffer.h"

#include <stdlib.h>
#include <string.h>

#include "logging.h"

#define DEFAULT_CAPACITY 4096
#define MAX_BUFFER_SIZE (10 * 1024 * 1024)  // 10MB hard limit for sentence buffering

/**
 * @brief Check if character is a sentence terminator
 */
static int is_sentence_terminator(char c) {
   return (c == '.' || c == '!' || c == '?' || c == ':');
}

/**
 * @brief Ensure buffer has enough capacity
 */
static int ensure_capacity(sentence_buffer_t *buf, size_t required) {
   if (required <= buf->capacity) {
      return 1;
   }

   // Prevent runaway memory allocation from excessively long unbounded text
   if (required > MAX_BUFFER_SIZE) {
      LOG_ERROR(
          "Sentence buffer size limit exceeded: requested %zu bytes, maximum %zu bytes (%.1f MB)",
          required, MAX_BUFFER_SIZE, MAX_BUFFER_SIZE / (1024.0 * 1024.0));
      return 0;
   }

   size_t new_capacity = buf->capacity * 2;
   while (new_capacity < required) {
      new_capacity *= 2;
   }

   // Cap at maximum size
   if (new_capacity > MAX_BUFFER_SIZE) {
      new_capacity = MAX_BUFFER_SIZE;
   }

   char *new_buffer = realloc(buf->buffer, new_capacity);
   if (!new_buffer) {
      LOG_ERROR("Failed to reallocate sentence buffer from %zu to %zu bytes", buf->capacity,
                new_capacity);
      return 0;
   }

   buf->buffer = new_buffer;
   buf->capacity = new_capacity;
   return 1;
}

sentence_buffer_t *sentence_buffer_create(sentence_callback callback, void *userdata) {
   if (!callback) {
      LOG_ERROR("Sentence buffer callback cannot be NULL");
      return NULL;
   }

   sentence_buffer_t *buf = calloc(1, sizeof(sentence_buffer_t));
   if (!buf) {
      LOG_ERROR("Failed to allocate sentence buffer");
      return NULL;
   }

   buf->buffer = malloc(DEFAULT_CAPACITY);
   if (!buf->buffer) {
      LOG_ERROR("Failed to allocate sentence buffer storage");
      free(buf);
      return NULL;
   }

   buf->buffer[0] = '\0';
   buf->capacity = DEFAULT_CAPACITY;
   buf->size = 0;
   buf->callback = callback;
   buf->callback_userdata = userdata;
   buf->inside_command_tag = 0;

   return buf;
}

void sentence_buffer_free(sentence_buffer_t *buf) {
   if (!buf) {
      return;
   }

   // Flush any remaining text
   sentence_buffer_flush(buf);

   free(buf->buffer);
   free(buf);
}

void sentence_buffer_feed(sentence_buffer_t *buf, const char *chunk) {
   if (!buf || !chunk || strlen(chunk) == 0) {
      return;
   }

   size_t chunk_len = strlen(chunk);
   size_t needed = buf->size + chunk_len + 1;

   // Ensure capacity
   if (!ensure_capacity(buf, needed)) {
      return;
   }

   // Append chunk to buffer
   memcpy(buf->buffer + buf->size, chunk, chunk_len);
   buf->size += chunk_len;
   buf->buffer[buf->size] = '\0';

   // Search for complete sentences
   size_t search_start = 0;

   while (search_start < buf->size) {
      // Find next sentence terminator
      size_t i;
      int found_terminator = 0;
      size_t terminator_pos = 0;

      for (i = search_start; i < buf->size; i++) {
         // Check for command tag boundaries
         if (i + 9 <= buf->size && strncmp(buf->buffer + i, "<command>", 9) == 0) {
            buf->inside_command_tag = 1;
            i += 8;  // Skip ahead (loop will increment by 1)
            continue;
         }
         if (i + 10 <= buf->size && strncmp(buf->buffer + i, "</command>", 10) == 0) {
            buf->inside_command_tag = 0;
            i += 9;  // Skip ahead (loop will increment by 1)
            continue;
         }

         // Only look for sentence terminators when NOT inside command tags
         if (!buf->inside_command_tag && is_sentence_terminator(buf->buffer[i])) {
            // Found a terminator - check if followed by space/newline/end
            if (i + 1 >= buf->size) {
               // At end of buffer - not complete yet (might get more text)
               break;
            } else if (buf->buffer[i + 1] == ' ' || buf->buffer[i + 1] == '\n' ||
                       buf->buffer[i + 1] == '\r') {
               // Complete sentence found
               found_terminator = 1;
               terminator_pos = i + 1;  // Include the space/newline
               break;
            }
         }
      }

      if (!found_terminator) {
         // No complete sentence found
         break;
      }

      // Extract sentence (from search_start to terminator_pos, inclusive)
      size_t sentence_len = (terminator_pos + 1) - search_start;
      char *sentence = malloc(sentence_len + 1);
      if (!sentence) {
         LOG_ERROR("Failed to allocate sentence");
         break;
      }

      memcpy(sentence, buf->buffer + search_start, sentence_len);
      sentence[sentence_len] = '\0';

      // Trim leading/trailing whitespace from sentence
      char *trimmed = sentence;
      while (*trimmed == ' ' || *trimmed == '\n' || *trimmed == '\r') {
         trimmed++;
      }

      size_t trimmed_len = strlen(trimmed);
      while (trimmed_len > 0 &&
             (trimmed[trimmed_len - 1] == ' ' || trimmed[trimmed_len - 1] == '\n' ||
              trimmed[trimmed_len - 1] == '\r')) {
         trimmed[trimmed_len - 1] = '\0';
         trimmed_len--;
      }

      // Send to callback if non-empty
      if (trimmed_len > 0) {
         buf->callback(trimmed, buf->callback_userdata);
      }

      free(sentence);

      // Move search position forward
      search_start = terminator_pos + 1;
   }

   // Remove processed sentences from buffer
   if (search_start > 0) {
      size_t remaining = buf->size - search_start;
      if (remaining > 0) {
         memmove(buf->buffer, buf->buffer + search_start, remaining);
      }
      buf->size = remaining;
      buf->buffer[buf->size] = '\0';
   }
}

void sentence_buffer_flush(sentence_buffer_t *buf) {
   if (!buf || buf->size == 0) {
      return;
   }

   // Trim whitespace
   char *trimmed = buf->buffer;
   while (*trimmed == ' ' || *trimmed == '\n' || *trimmed == '\r') {
      trimmed++;
   }

   size_t trimmed_len = strlen(trimmed);
   while (trimmed_len > 0 && (trimmed[trimmed_len - 1] == ' ' || trimmed[trimmed_len - 1] == '\n' ||
                              trimmed[trimmed_len - 1] == '\r')) {
      trimmed[trimmed_len - 1] = '\0';
      trimmed_len--;
   }

   // Send remaining text if non-empty
   if (trimmed_len > 0) {
      buf->callback(trimmed, buf->callback_userdata);
   }

   // Clear buffer
   buf->size = 0;
   buf->buffer[0] = '\0';
}
