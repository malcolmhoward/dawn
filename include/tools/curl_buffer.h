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
 * Shared CURL buffer utilities for HTTP response handling
 */

#ifndef CURL_BUFFER_H
#define CURL_BUFFER_H

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

// Buffer capacity constants
#define CURL_BUFFER_INITIAL_CAPACITY 4096
#define CURL_BUFFER_MAX_CAPACITY 65536

/**
 * Buffer structure for accumulating CURL response data
 * Initialize with: curl_buffer_t buf = {NULL, 0, 0};
 */
typedef struct {
   char *data;       // Response data (null-terminated)
   size_t size;      // Current size of data (excluding null terminator)
   size_t capacity;  // Allocated capacity
} curl_buffer_t;

/**
 * CURL write callback with exponential buffer growth
 *
 * Usage:
 *   curl_buffer_t buffer = {NULL, 0, 0};
 *   curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_buffer_write_callback);
 *   curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buffer);
 *   // ... perform request ...
 *   // buffer.data now contains null-terminated response
 *   free(buffer.data);
 *
 * @param contents Data received from CURL
 * @param size Size of each element
 * @param nmemb Number of elements
 * @param userp Pointer to curl_buffer_t
 * @return Number of bytes handled, or 0 on error
 */
static inline size_t curl_buffer_write_callback(void *contents,
                                                size_t size,
                                                size_t nmemb,
                                                void *userp) {
   size_t total_size = size * nmemb;
   curl_buffer_t *buf = (curl_buffer_t *)userp;

   // Check if we need to grow the buffer
   size_t required = buf->size + total_size + 1;
   if (required > buf->capacity) {
      // Exponential growth to reduce reallocations
      size_t new_capacity = buf->capacity ? buf->capacity : CURL_BUFFER_INITIAL_CAPACITY;
      // Prevent overflow by checking before doubling
      while (new_capacity < required && new_capacity <= CURL_BUFFER_MAX_CAPACITY / 2) {
         new_capacity *= 2;
      }
      // Cap at reasonable maximum
      if (new_capacity < required && required <= CURL_BUFFER_MAX_CAPACITY) {
         new_capacity = CURL_BUFFER_MAX_CAPACITY;
      }

      char *new_data = (char *)realloc(buf->data, new_capacity);
      if (!new_data) {
         return 0;  // Signal error to CURL
      }
      buf->data = new_data;
      buf->capacity = new_capacity;
   }

   memcpy(&(buf->data[buf->size]), contents, total_size);
   buf->size += total_size;
   buf->data[buf->size] = '\0';

   return total_size;
}

/**
 * Initialize a curl buffer
 * @param buf Buffer to initialize
 */
static inline void curl_buffer_init(curl_buffer_t *buf) {
   buf->data = NULL;
   buf->size = 0;
   buf->capacity = 0;
}

/**
 * Free a curl buffer's data
 * @param buf Buffer to free
 */
static inline void curl_buffer_free(curl_buffer_t *buf) {
   if (buf->data) {
      free(buf->data);
      buf->data = NULL;
   }
   buf->size = 0;
   buf->capacity = 0;
}

#endif  // CURL_BUFFER_H
