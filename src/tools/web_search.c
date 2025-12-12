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
#include "tools/curl_buffer.h"

// =============================================================================
// Module State (thread-safe with mutex protection)
// =============================================================================

static char *searxng_base_url = NULL;
static int module_initialized = 0;
static pthread_mutex_t module_mutex = PTHREAD_MUTEX_INITIALIZER;

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
// Search Query (internal implementation)
// =============================================================================

/**
 * @brief Parse results from the standard "results" array
 */
static void parse_results_array(struct json_object *results_array,
                                search_response_t *response,
                                int max_results) {
   int result_count = json_object_array_length(results_array);
   if (result_count == 0) {
      return;
   }

   if (result_count > max_results) {
      result_count = max_results;
   }

   response->results = calloc(result_count, sizeof(search_result_t));
   if (!response->results) {
      LOG_ERROR("web_search: Failed to allocate results array");
      response->error = strdup("Memory allocation failed");
      return;
   }

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
}

/**
 * @brief Parse results from the "infoboxes" array (Wikipedia facts)
 */
static void parse_infoboxes_array(struct json_object *infoboxes_array,
                                  search_response_t *response,
                                  int max_results) {
   int infobox_count = json_object_array_length(infoboxes_array);
   if (infobox_count == 0) {
      return;
   }

   if (infobox_count > max_results) {
      infobox_count = max_results;
   }

   response->results = calloc(infobox_count, sizeof(search_result_t));
   if (!response->results) {
      LOG_ERROR("web_search: Failed to allocate infobox results array");
      response->error = strdup("Memory allocation failed");
      return;
   }

   for (int i = 0; i < infobox_count; i++) {
      struct json_object *item = json_object_array_get_idx(infoboxes_array, i);
      if (!item) {
         continue;
      }

      // Infobox structure: {infobox: title, content: description, id: url, engine: "wikipedia"}
      response->results[response->count].title = get_json_string(item, "infobox");
      response->results[response->count].engine = get_json_string(item, "engine");

      // Get URL from id field or from urls array
      char *url = get_json_string(item, "id");
      if (!url) {
         struct json_object *urls_array = NULL;
         if (json_object_object_get_ex(item, "urls", &urls_array) &&
             json_object_array_length(urls_array) > 0) {
            struct json_object *first_url = json_object_array_get_idx(urls_array, 0);
            if (first_url) {
               url = get_json_string(first_url, "url");
            }
         }
      }
      response->results[response->count].url = url;

      // Get content as snippet (may be longer than regular snippets)
      char *full_content = get_json_string(item, "content");
      if (full_content) {
         // Use larger snippet length for facts (more useful context)
         response->results[response->count].snippet = truncate_string(full_content, 500);
         free(full_content);
      }

      response->count++;
   }
}

// =============================================================================
// Search Query (public API)
// =============================================================================

search_response_t *web_search_query_typed(const char *query, int max_results, search_type_t type) {
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

// Build URL based on search type
#define SEARCH_URL_MAX_LEN 1024
   char url[SEARCH_URL_MAX_LEN];
   const char *type_str = "web";

   // Map search type to SearXNG category parameter
   const char *category = NULL;
   switch (type) {
      case SEARCH_TYPE_NEWS:
         category = "news";
         type_str = "news";
         break;
      case SEARCH_TYPE_FACTS:
         // Wikipedia uses engines parameter, not categories
         snprintf(url, sizeof(url), "%s/search?q=%s&format=json&engines=wikipedia",
                  searxng_base_url, encoded_query);
         type_str = "facts";
         curl_free(encoded_query);
         goto do_request;
      case SEARCH_TYPE_SCIENCE:
         category = "science";
         type_str = "science";
         break;
      case SEARCH_TYPE_IT:
         category = "it";
         type_str = "it";
         break;
      case SEARCH_TYPE_SOCIAL:
         category = "social media";
         type_str = "social";
         break;
      case SEARCH_TYPE_QA:
         category = "q&a";
         type_str = "q&a";
         break;
      case SEARCH_TYPE_DICTIONARY:
         category = "dictionaries";
         type_str = "dictionary";
         break;
      case SEARCH_TYPE_PAPERS:
         category = "scientific publications";
         type_str = "papers";
         break;
      case SEARCH_TYPE_WEB:
      default:
         type_str = "web";
         break;
   }

   // Build URL with optional category
   if (category) {
      char *encoded_category = curl_easy_escape(curl, category, 0);
      snprintf(url, sizeof(url), "%s/search?q=%s&format=json&categories=%s", searxng_base_url,
               encoded_query, encoded_category);
      curl_free(encoded_category);
   } else {
      snprintf(url, sizeof(url), "%s/search?q=%s&format=json", searxng_base_url, encoded_query);
   }
   curl_free(encoded_query);

do_request:
   LOG_INFO("web_search: Querying [%s] %s", type_str, url);

   // Set up response buffer with web search max capacity (64KB)
   curl_buffer_t buffer;
   curl_buffer_init_with_max(&buffer, CURL_BUFFER_MAX_WEB_SEARCH);

   // Configure CURL
   curl_easy_setopt(curl, CURLOPT_URL, url);
   curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_buffer_write_callback);
   curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buffer);
   curl_easy_setopt(curl, CURLOPT_TIMEOUT, SEARXNG_TIMEOUT_SEC);
   curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5);
   curl_easy_setopt(curl, CURLOPT_USERAGENT, "DAWN/1.0");

   // Perform request
   CURLcode res = curl_easy_perform(curl);
   if (res != CURLE_OK) {
      LOG_ERROR("web_search: Request failed: %s", curl_easy_strerror(res));
      curl_easy_cleanup(curl);
      curl_buffer_free(&buffer);
      response->error = strdup(curl_easy_strerror(res));
      return response;
   }

   // Check HTTP response code
   long http_code = 0;
   curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
   curl_easy_cleanup(curl);

   if (http_code != 200) {
      LOG_ERROR("web_search: HTTP error %ld", http_code);
      curl_buffer_free(&buffer);
      char err_msg[64];
      snprintf(err_msg, sizeof(err_msg), "HTTP error %ld", http_code);
      response->error = strdup(err_msg);
      return response;
   }

   // Parse JSON response
   struct json_object *root = json_tokener_parse(buffer.data);
   curl_buffer_free(&buffer);

   if (!root) {
      LOG_ERROR("web_search: Failed to parse JSON response");
      response->error = strdup("Invalid JSON response from search service");
      return response;
   }

   // For FACTS type, check infoboxes first (Wikipedia returns data there)
   if (type == SEARCH_TYPE_FACTS) {
      struct json_object *infoboxes_array = NULL;
      if (json_object_object_get_ex(root, "infoboxes", &infoboxes_array) &&
          json_object_array_length(infoboxes_array) > 0) {
         parse_infoboxes_array(infoboxes_array, response, max_results);
         if (response->count > 0) {
            json_object_put(root);
            LOG_INFO("web_search: Found %d infobox results", response->count);
            return response;
         }
      }
      // Fall through to check results array if no infoboxes
   }

   // Extract results array (standard path for WEB and NEWS, fallback for FACTS)
   struct json_object *results_array = NULL;
   if (!json_object_object_get_ex(root, "results", &results_array)) {
      LOG_WARNING("web_search: No 'results' field in response");
      json_object_put(root);
      if (type == SEARCH_TYPE_FACTS) {
         response->error = strdup("No Wikipedia data found for this query");
      } else {
         response->error = strdup("No results in response");
      }
      return response;
   }

   int result_count = json_object_array_length(results_array);
   if (result_count == 0) {
      LOG_INFO("web_search: No results found for query");
      json_object_put(root);
      return response;  // No error, just empty results
   }

   parse_results_array(results_array, response, max_results);

   json_object_put(root);
   LOG_INFO("web_search: Found %d results", response->count);
   return response;
}

search_response_t *web_search_query(const char *query, int max_results) {
   return web_search_query_typed(query, max_results, SEARCH_TYPE_WEB);
}

// =============================================================================
// Format for LLM
// =============================================================================

int web_search_format_for_llm(const search_response_t *response, char *buffer, size_t buffer_size) {
   if (!response || !buffer || buffer_size == 0) {
      return 0;
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
