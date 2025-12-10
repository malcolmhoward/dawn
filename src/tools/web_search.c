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
 * Web Search Module - SearXNG Integration
 *
 * Implements web search capability via a local SearXNG instance using
 * libcurl for HTTP requests and json-c for response parsing.
 */

#include "tools/web_search.h"

#include <curl/curl.h>
#include <json-c/json.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "logging.h"

// =============================================================================
// Module State (thread-safe with mutex protection)
// =============================================================================

static char *searxng_base_url = NULL;
static int module_initialized = 0;
static pthread_mutex_t module_mutex = PTHREAD_MUTEX_INITIALIZER;

// =============================================================================
// CURL Response Buffer
// =============================================================================

// Initial buffer capacity for curl responses
#define CURL_BUFFER_INITIAL_CAPACITY 4096
// Maximum buffer capacity (64KB should be plenty for search results)
#define CURL_BUFFER_MAX_CAPACITY 65536

typedef struct {
   char *data;
   size_t size;
   size_t capacity;
} curl_buffer_t;

static size_t searxng_write_callback(void *contents, size_t size, size_t nmemb, void *userp) {
   size_t total_size = size * nmemb;
   curl_buffer_t *buf = (curl_buffer_t *)userp;

   // Check if we need to grow the buffer
   size_t required = buf->size + total_size + 1;
   if (required > buf->capacity) {
      // Exponential growth to reduce reallocations
      size_t new_capacity = buf->capacity ? buf->capacity : CURL_BUFFER_INITIAL_CAPACITY;
      while (new_capacity < required && new_capacity <= CURL_BUFFER_MAX_CAPACITY / 2) {
         new_capacity *= 2;
      }
      // Cap at reasonable maximum
      if (new_capacity < required && required <= CURL_BUFFER_MAX_CAPACITY) {
         new_capacity = CURL_BUFFER_MAX_CAPACITY;
      }

      char *new_data = realloc(buf->data, new_capacity);
      if (!new_data) {
         LOG_ERROR("web_search: Failed to allocate response buffer");
         return 0;
      }
      buf->data = new_data;
      buf->capacity = new_capacity;
   }

   memcpy(&(buf->data[buf->size]), contents, total_size);
   buf->size += total_size;
   buf->data[buf->size] = '\0';

   return total_size;
}

// =============================================================================
// Lifecycle
// =============================================================================

int web_search_init(const char *searxng_url) {
   pthread_mutex_lock(&module_mutex);

   if (module_initialized) {
      pthread_mutex_unlock(&module_mutex);
      LOG_WARNING("web_search: Already initialized");
      return 0;
   }

   // Store base URL (or use default)
   if (searxng_url) {
      searxng_base_url = strdup(searxng_url);
   } else {
      searxng_base_url = strdup(SEARXNG_DEFAULT_URL);
   }

   if (!searxng_base_url) {
      pthread_mutex_unlock(&module_mutex);
      LOG_ERROR("web_search: Failed to allocate URL string");
      return 1;
   }

   // Note: curl_global_init() is called in main() - not here
   // This avoids conflicts with other modules using libcurl

   module_initialized = 1;
   pthread_mutex_unlock(&module_mutex);
   LOG_INFO("web_search: Initialized with URL %s", searxng_base_url);
   return 0;
}

void web_search_cleanup(void) {
   pthread_mutex_lock(&module_mutex);

   if (!module_initialized) {
      pthread_mutex_unlock(&module_mutex);
      return;
   }

   if (searxng_base_url) {
      free(searxng_base_url);
      searxng_base_url = NULL;
   }

   // Note: curl_global_cleanup() is called in main() - not here
   module_initialized = 0;
   pthread_mutex_unlock(&module_mutex);
   LOG_INFO("web_search: Cleanup complete");
}

int web_search_is_initialized(void) {
   // Use atomic load for thread-safe access without full mutex lock
   return __atomic_load_n(&module_initialized, __ATOMIC_ACQUIRE);
}

// =============================================================================
// Helper Functions
// =============================================================================

static char *truncate_string(const char *str, size_t max_len) {
   if (!str) {
      return NULL;
   }

   size_t len = strlen(str);
   if (len <= max_len) {
      // Avoid double strlen by using memcpy with known length
      char *copy = malloc(len + 1);
      if (copy) {
         memcpy(copy, str, len + 1);
      }
      return copy;
   }

   // Allocate for truncated string + "..." + null terminator
   char *truncated = malloc(max_len + 4);
   if (!truncated) {
      return NULL;
   }

   // Use memcpy instead of strncpy (we know the exact length)
   memcpy(truncated, str, max_len);
   // Append "..." directly (4 bytes including null terminator)
   memcpy(truncated + max_len, "...", 4);
   return truncated;
}

static char *get_json_string(struct json_object *obj, const char *key) {
   struct json_object *val = NULL;
   if (json_object_object_get_ex(obj, key, &val)) {
      const char *str = json_object_get_string(val);
      if (str) {
         return strdup(str);
      }
   }
   return NULL;
}

// =============================================================================
// Search Query
// =============================================================================

search_response_t *web_search_query(const char *query, int max_results) {
   if (!module_initialized) {
      LOG_ERROR("web_search: Module not initialized");
      return NULL;
   }

   if (!query || query[0] == '\0') {
      LOG_ERROR("web_search: Empty query");
      return NULL;
   }

   if (max_results <= 0) {
      max_results = SEARXNG_MAX_RESULTS;
   }

   // Allocate response structure
   search_response_t *response = calloc(1, sizeof(search_response_t));
   if (!response) {
      LOG_ERROR("web_search: Failed to allocate response");
      return NULL;
   }

   // Create CURL handle
   CURL *curl = curl_easy_init();
   if (!curl) {
      LOG_ERROR("web_search: Failed to create CURL handle");
      response->error = strdup("Failed to initialize HTTP client");
      return response;
   }

   // URL-encode the query
   char *encoded_query = curl_easy_escape(curl, query, 0);
   if (!encoded_query) {
      LOG_ERROR("web_search: Failed to encode query");
      curl_easy_cleanup(curl);
      response->error = strdup("Failed to encode search query");
      return response;
   }

// Build URL: {base}/search?q={query}&format=json
#define SEARCH_URL_MAX_LEN 1024
   char url[SEARCH_URL_MAX_LEN];
   snprintf(url, sizeof(url), "%s/search?q=%s&format=json", searxng_base_url, encoded_query);
   curl_free(encoded_query);

   LOG_INFO("web_search: Querying %s", url);

   // Set up response buffer
   curl_buffer_t buffer = { .data = NULL, .size = 0, .capacity = 0 };

   // Configure CURL
   curl_easy_setopt(curl, CURLOPT_URL, url);
   curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, searxng_write_callback);
   curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buffer);
   curl_easy_setopt(curl, CURLOPT_TIMEOUT, SEARXNG_TIMEOUT_SEC);
   curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5);
   curl_easy_setopt(curl, CURLOPT_USERAGENT, "DAWN/1.0");

   // Perform request
   CURLcode res = curl_easy_perform(curl);
   if (res != CURLE_OK) {
      LOG_ERROR("web_search: Request failed: %s", curl_easy_strerror(res));
      curl_easy_cleanup(curl);
      if (buffer.data) {
         free(buffer.data);
      }
      response->error = strdup(curl_easy_strerror(res));
      return response;
   }

   // Check HTTP response code
   long http_code = 0;
   curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
   curl_easy_cleanup(curl);

   if (http_code != 200) {
      LOG_ERROR("web_search: HTTP error %ld", http_code);
      if (buffer.data) {
         free(buffer.data);
      }
      char err_msg[64];
      snprintf(err_msg, sizeof(err_msg), "HTTP error %ld", http_code);
      response->error = strdup(err_msg);
      return response;
   }

   // Parse JSON response
   struct json_object *root = json_tokener_parse(buffer.data);
   free(buffer.data);
   buffer.data = NULL;

   if (!root) {
      LOG_ERROR("web_search: Failed to parse JSON response");
      response->error = strdup("Invalid JSON response from search service");
      return response;
   }

   // Extract query time
   struct json_object *query_time_obj = NULL;
   if (json_object_object_get_ex(root, "query", &query_time_obj)) {
      response->query_time_sec = (float)json_object_get_double(query_time_obj);
   }

   // Extract results array
   struct json_object *results_array = NULL;
   if (!json_object_object_get_ex(root, "results", &results_array)) {
      LOG_WARNING("web_search: No 'results' field in response");
      json_object_put(root);
      response->error = strdup("No results in response");
      return response;
   }

   int result_count = json_object_array_length(results_array);
   if (result_count == 0) {
      LOG_INFO("web_search: No results found for query");
      json_object_put(root);
      return response;  // No error, just empty results
   }

   // Limit results
   if (result_count > max_results) {
      result_count = max_results;
   }

   // Allocate results array
   response->results = calloc(result_count, sizeof(search_result_t));
   if (!response->results) {
      LOG_ERROR("web_search: Failed to allocate results array");
      json_object_put(root);
      response->error = strdup("Memory allocation failed");
      return response;
   }

   // Parse each result
   for (int i = 0; i < result_count; i++) {
      struct json_object *item = json_object_array_get_idx(results_array, i);
      if (!item) {
         continue;
      }

      response->results[response->count].title = get_json_string(item, "title");
      response->results[response->count].url = get_json_string(item, "url");
      response->results[response->count].engine = get_json_string(item, "engine");

      // Get snippet (called "content" in SearXNG)
      char *full_snippet = get_json_string(item, "content");
      if (full_snippet) {
         response->results[response->count].snippet = truncate_string(full_snippet,
                                                                      SEARXNG_SNIPPET_LEN);
         free(full_snippet);
      }

      response->count++;
   }

   json_object_put(root);
   LOG_INFO("web_search: Found %d results", response->count);
   return response;
}

// =============================================================================
// Format for LLM
// =============================================================================

int web_search_format_for_llm(const search_response_t *response, char *buffer, size_t buffer_size) {
   if (!response || !buffer || buffer_size == 0) {
      return -1;
   }

   if (response->error) {
      return snprintf(buffer, buffer_size, "Search failed: %s", response->error);
   }

   if (response->count == 0) {
      return snprintf(buffer, buffer_size, "No search results found.");
   }

   int written = 0;
   written += snprintf(buffer + written, buffer_size - written, "Web search results:\n\n");

   for (int i = 0; i < response->count && written < (int)buffer_size - 1; i++) {
      search_result_t *r = &response->results[i];

      written += snprintf(buffer + written, buffer_size - written, "%d. %s", i + 1,
                          r->title ? r->title : "(no title)");

      if (r->engine) {
         written += snprintf(buffer + written, buffer_size - written, " [%s]", r->engine);
      }

      written += snprintf(buffer + written, buffer_size - written, "\n");

      if (r->snippet) {
         written += snprintf(buffer + written, buffer_size - written, "   %s\n", r->snippet);
      }

      if (r->url) {
         written += snprintf(buffer + written, buffer_size - written, "   URL: %s\n", r->url);
      }

      written += snprintf(buffer + written, buffer_size - written, "\n");
   }

   return written;
}

// =============================================================================
// Free Response
// =============================================================================

void web_search_free_response(search_response_t *response) {
   if (!response) {
      return;
   }

   if (response->results) {
      for (int i = 0; i < response->count; i++) {
         if (response->results[i].title) {
            free(response->results[i].title);
         }
         if (response->results[i].url) {
            free(response->results[i].url);
         }
         if (response->results[i].snippet) {
            free(response->results[i].snippet);
         }
         if (response->results[i].engine) {
            free(response->results[i].engine);
         }
      }
      free(response->results);
   }

   if (response->error) {
      free(response->error);
   }

   free(response);
}
