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

#include "llm/llm_interface.h"

#include <curl/curl.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/time.h>
#include <unistd.h>

#include "dawn.h"
#include "llm/sentence_buffer.h"
#include "logging.h"
#include "secrets.h"
#include "tts/text_to_speech.h"
#include "ui/metrics.h"

// Provider implementations
#ifdef OPENAI_API_KEY
#include "llm/llm_openai.h"
#endif

#ifdef CLAUDE_API_KEY
#include "llm/llm_claude.h"
#endif

// LLM URLs
#define CLOUDAI_URL "https://api.openai.com"
#define CLAUDE_URL "https://api.anthropic.com"
#define LOCALAI_URL "http://127.0.0.1:8080"

// Global state
static llm_type_t current_type = LLM_UNDEFINED;
static cloud_provider_t current_cloud_provider = CLOUD_PROVIDER_NONE;
static char llm_url[2048] = "";

// Global interrupt flag - set by main thread when wake word detected during LLM processing
static volatile sig_atomic_t llm_interrupt_requested = 0;

/**
 * @brief Structure to hold dynamically allocated response data from CURL
 */
struct MemoryStruct {
   char *memory; /**< Pointer to the dynamically allocated buffer */
   size_t size;  /**< Current size of the buffer */
};

/**
 * @brief Callback for writing data to a MemoryStruct buffer (used by CURL)
 *
 * This function is intended to be used with libcurl's CURLOPT_WRITEFUNCTION option.
 * Whenever libcurl receives data that is to be saved, this function is called. It
 * reallocates the MemoryStruct's memory block to fit the new piece of data, ensuring
 * that the data is concatenated properly within the buffer.
 *
 * @param contents Pointer to the data libcurl has ready for us.
 * @param size The size of the data in the block, always 1.
 * @param nmemb Number of blocks to write, each of size 'size'.
 * @param userp Pointer to a MemoryStruct structure where the data should be stored.
 *
 * @return The number of bytes actually taken care of. If that amount differs from the
 * amount passed to your function, it'll signal an error to libcurl. Returning 0
 * will signal an out-of-memory error.
 */
size_t WriteMemoryCallback(void *contents, size_t size, size_t nmemb, void *userp) {
   size_t realsize = size * nmemb;
   struct MemoryStruct *mem = (struct MemoryStruct *)userp;

   mem->memory = realloc(mem->memory, mem->size + realsize + 1);
   if (mem->memory == NULL) {
      LOG_ERROR("Not enough memory (realloc returned NULL)");
      return 0;
   }

   memcpy(&(mem->memory[mem->size]), contents, realsize);
   mem->size += realsize;
   mem->memory[mem->size] = 0;

   return realsize;
}

/**
 * @brief Extracts the host and port from a URL, removing protocol and paths.
 *
 * This function handles stripping protocols (http, https) and extracts the host and port from
 * URLs. If no port is provided, it defaults to port 80 for http and 443 for https.
 *
 * @param url The input URL string.
 * @param host Output buffer to store the extracted host (must be pre-allocated).
 * @param port Output buffer to store the extracted port (must be large enough for the port
 * number).
 * @return int Returns 0 on success, -1 on failure.
 */
static int extract_host_and_port(const char *url, char *host, char *port) {
   // Validate the input arguments
   if (url == NULL || host == NULL || port == NULL) {
      LOG_ERROR("Error: NULL argument passed to extract_host_and_port.");
      return -1;
   }

   if (strlen(url) == 0) {
      LOG_ERROR("Error: Empty URL provided.");
      return -1;
   }

   const char *start = url;

   // Determine protocol and set default port
   if (strncmp(url, "http://", 7) == 0) {
      start = url + 7;     // Skip "http://"
      strcpy(port, "80");  // Default port for http
   } else if (strncmp(url, "https://", 8) == 0) {
      start = url + 8;      // Skip "https://"
      strcpy(port, "443");  // Default port for https
   } else {
      // If no recognizable protocol, assume http and continue
      strcpy(port, "80");
   }

   // Find the end of the host part (either ':' for port or '/' for path)
   const char *end = strpbrk(start, ":/");
   if (end == NULL) {
      // No port or path, the host is the entire remaining string
      strcpy(host, start);
   } else if (*end == ':') {
      // Extract the host and port
      strncpy(host, start, end - start);
      host[end - start] = '\0';  // Null-terminate the host
      strcpy(port, end + 1);     // Port starts after ':'
   } else {
      // Extract the host only (no port, but has a path)
      strncpy(host, start, end - start);
      host[end - start] = '\0';  // Null-terminate the host
   }

   return 0;
}

int llm_check_connection(const char *url, int timeout_seconds) {
   char host[2048];
   char port[6];

   // Extract host from the URL (ignores path and protocol)
   if (extract_host_and_port(url, host, port) == -1) {
      LOG_ERROR("Error: Invalid URL format");
      return 0;
   }

   // Set up address resolution hints
   struct addrinfo hints, *res;
   memset(&hints, 0, sizeof(hints));
   hints.ai_family = AF_INET;        // Use IPv4
   hints.ai_socktype = SOCK_STREAM;  // TCP stream sockets

   // Resolve host (works for both hostnames and IP addresses)
   int status = getaddrinfo(host, port, &hints, &res);
   if (status != 0) {
      LOG_ERROR("getaddrinfo: %s", gai_strerror(status));
      return 0;
   }

   // Create a socket
   int sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
   if (sock == -1) {
      LOG_ERROR("socket: %s", strerror(errno));
      freeaddrinfo(res);
      return 0;
   }

   // Set socket as non-blocking
   fcntl(sock, F_SETFL, O_NONBLOCK);

   // Attempt to connect
   int result = connect(sock, res->ai_addr, res->ai_addrlen);
   if (result == -1 && errno != EINPROGRESS) {
      LOG_ERROR("connect: %s", strerror(errno));
      close(sock);
      freeaddrinfo(res);
      return 0;
   }

   // Set up the file descriptor set for select()
   fd_set write_fds;
   FD_ZERO(&write_fds);
   FD_SET(sock, &write_fds);

   // Set the timeout value
   struct timeval timeout;
   timeout.tv_sec = timeout_seconds;
   timeout.tv_usec = 0;

   // Wait for the socket to become writable within the timeout
   result = select(sock + 1, NULL, &write_fds, NULL, &timeout);
   if (result == 1) {
      // Socket is writable, check for connection success
      int error;
      socklen_t error_len = sizeof(error);
      if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &error, &error_len) == 0 && error == 0) {
         result = 1;  // Connection successful
      } else {
         result = 0;  // Connection failed
      }
   } else {
      result = 0;  // Timeout or error
   }

   // Clean up
   close(sock);
   freeaddrinfo(res);

   return result;
}

void llm_init(const char *cloud_provider_override) {
   // Detect available providers
   bool openai_available = false;
   bool claude_available = false;

#ifdef OPENAI_API_KEY
   openai_available = true;
#endif

#ifdef CLAUDE_API_KEY
   claude_available = true;
#endif

   if (!openai_available && !claude_available) {
      LOG_WARNING("No cloud LLM providers configured in secrets.h");
      current_cloud_provider = CLOUD_PROVIDER_NONE;
      llm_set_type(LLM_LOCAL);
      return;
   }

   // Handle command-line override
   if (cloud_provider_override != NULL) {
      if (strcmp(cloud_provider_override, "openai") == 0) {
         if (!openai_available) {
            LOG_ERROR("OpenAI requested but OPENAI_API_KEY not defined in secrets.h");
            exit(1);
         }
         current_cloud_provider = CLOUD_PROVIDER_OPENAI;
         LOG_INFO("Cloud provider set to OpenAI (command-line override)");
      } else if (strcmp(cloud_provider_override, "claude") == 0) {
         if (!claude_available) {
            LOG_ERROR("Claude requested but CLAUDE_API_KEY not defined in secrets.h");
            exit(1);
         }
         current_cloud_provider = CLOUD_PROVIDER_CLAUDE;
         LOG_INFO("Cloud provider set to Claude (command-line override)");
      } else {
         LOG_ERROR("Unknown cloud provider: %s (valid: openai, claude)", cloud_provider_override);
         exit(1);
      }
   } else {
      // Auto-detect: prefer OpenAI for backward compatibility
      if (openai_available) {
         current_cloud_provider = CLOUD_PROVIDER_OPENAI;
         LOG_INFO("Cloud provider auto-detected: OpenAI");
      } else {
         current_cloud_provider = CLOUD_PROVIDER_CLAUDE;
         LOG_INFO("Cloud provider auto-detected: Claude");
      }
   }

   // Note: LLM type (local/cloud) is set by dawn.c after this function returns,
   // allowing proper TTS announcement after TTS is initialized.
}

void llm_set_type(llm_type_t type) {
   current_type = type;

   if (type == LLM_CLOUD) {
      if (current_cloud_provider == CLOUD_PROVIDER_CLAUDE) {
         snprintf(llm_url, sizeof(llm_url), "%s", CLAUDE_URL);
         text_to_speech("Setting AI to cloud LLM using Claude.");
      } else {
         snprintf(llm_url, sizeof(llm_url), "%s", CLOUDAI_URL);
         text_to_speech("Setting AI to cloud LLM using OpenAI.");
      }
      LOG_INFO("LLM set to CLOUD (%s)", llm_get_cloud_provider_name());
   } else if (type == LLM_LOCAL) {
      snprintf(llm_url, sizeof(llm_url), "%s", LOCALAI_URL);
      text_to_speech("Setting AI to local LLM.");
      LOG_INFO("LLM set to LOCAL");
   }

   // Update metrics with current LLM configuration
   metrics_update_llm_config(type, current_cloud_provider);
}

llm_type_t llm_get_type(void) {
   return current_type;
}

const char *llm_get_cloud_provider_name(void) {
   switch (current_cloud_provider) {
      case CLOUD_PROVIDER_OPENAI:
         return "OpenAI";
      case CLOUD_PROVIDER_CLAUDE:
         return "Claude";
      case CLOUD_PROVIDER_NONE:
         return "None";
      default:
         return "Unknown";
   }
}

const char *llm_get_model_name(void) {
   switch (current_cloud_provider) {
      case CLOUD_PROVIDER_OPENAI:
#ifdef OPENAI_API_KEY
         return OPENAI_MODEL;
#else
         return "N/A";
#endif
      case CLOUD_PROVIDER_CLAUDE:
#ifdef CLAUDE_API_KEY
         return CLAUDE_MODEL;
#else
         return "N/A";
#endif
      case CLOUD_PROVIDER_NONE:
         return "None";
      default:
         return "Unknown";
   }
}

void llm_request_interrupt(void) {
   llm_interrupt_requested = 1;
}

void llm_clear_interrupt(void) {
   llm_interrupt_requested = 0;
}

int llm_is_interrupt_requested(void) {
   return llm_interrupt_requested;
}

/**
 * @brief CURL progress callback to check for interruption requests
 *
 * Called periodically by CURL during transfer. Returns non-zero to abort.
 *
 * @param clientp User data pointer (unused)
 * @param dltotal Total bytes to download
 * @param dlnow Bytes downloaded so far
 * @param ultotal Total bytes to upload
 * @param ulnow Bytes uploaded so far
 * @return 0 to continue transfer, non-zero to abort
 */
int llm_curl_progress_callback(void *clientp,
                               curl_off_t dltotal,
                               curl_off_t dlnow,
                               curl_off_t ultotal,
                               curl_off_t ulnow) {
   (void)clientp;  // Unused
   (void)dltotal;
   (void)dlnow;
   (void)ultotal;
   (void)ulnow;

   // Check if interrupt was requested
   if (llm_interrupt_requested) {
      LOG_INFO("LLM transfer interrupted by user");
      return 1;  // Non-zero aborts transfer
   }
   return 0;  // Zero continues transfer
}

char *llm_chat_completion(struct json_object *conversation_history,
                          const char *input_text,
                          char *vision_image,
                          size_t vision_image_size) {
   char *response = NULL;

   if (current_type == LLM_LOCAL) {
      // Local LLM uses OpenAI-compatible API
#ifdef OPENAI_API_KEY
      response = llm_openai_chat_completion(conversation_history, input_text, vision_image,
                                            vision_image_size, llm_url,
                                            NULL  // No API key for local
      );
#else
      LOG_ERROR("Local LLM requires OpenAI-compatible implementation");
      return NULL;
#endif
   } else {
      // Route to cloud provider
      switch (current_cloud_provider) {
#ifdef OPENAI_API_KEY
         case CLOUD_PROVIDER_OPENAI:
            response = llm_openai_chat_completion(conversation_history, input_text, vision_image,
                                                  vision_image_size, llm_url, OPENAI_API_KEY);
            break;
#endif

#ifdef CLAUDE_API_KEY
         case CLOUD_PROVIDER_CLAUDE:
            response = llm_claude_chat_completion(conversation_history, input_text, vision_image,
                                                  vision_image_size, llm_url, CLAUDE_API_KEY);
            break;
#endif

         default:
            LOG_ERROR("No cloud provider configured");
            return NULL;
      }
   }

   // If cloud LLM failed (but not interrupted by user), try falling back to local
   if (response == NULL && current_type == LLM_CLOUD && !llm_is_interrupt_requested()) {
      if (strcmp(CLOUDAI_URL, llm_url) == 0 || strcmp(CLAUDE_URL, llm_url) == 0) {
         LOG_WARNING("Falling back to local LLM due to connection failure.");
         text_to_speech("Unable to contact cloud LLM.");
         llm_set_type(LLM_LOCAL);

         // Retry with local LLM
#ifdef OPENAI_API_KEY
         response = llm_openai_chat_completion(conversation_history, input_text, vision_image,
                                               vision_image_size, llm_url, NULL);
#endif
      }
   }

   return response;
}

char *llm_chat_completion_streaming(struct json_object *conversation_history,
                                    const char *input_text,
                                    char *vision_image,
                                    size_t vision_image_size,
                                    llm_text_chunk_callback chunk_callback,
                                    void *callback_userdata) {
   char *response = NULL;

   // Track LLM total time
   struct timeval start_time, end_time;
   gettimeofday(&start_time, NULL);

   // Record query metrics
   metrics_record_llm_query(current_type);

   if (current_type == LLM_LOCAL) {
      // Local LLM uses OpenAI-compatible API
#ifdef OPENAI_API_KEY
      response = llm_openai_chat_completion_streaming(conversation_history, input_text,
                                                      vision_image, vision_image_size, llm_url,
                                                      NULL,  // No API key for local
                                                      chunk_callback, callback_userdata);
#else
      LOG_ERROR("Local LLM requires OpenAI-compatible implementation");
      return NULL;
#endif
   } else {
      // Route to cloud provider
      switch (current_cloud_provider) {
#ifdef OPENAI_API_KEY
         case CLOUD_PROVIDER_OPENAI:
            response = llm_openai_chat_completion_streaming(conversation_history, input_text,
                                                            vision_image, vision_image_size,
                                                            llm_url, OPENAI_API_KEY, chunk_callback,
                                                            callback_userdata);
            break;
#endif

#ifdef CLAUDE_API_KEY
         case CLOUD_PROVIDER_CLAUDE:
            response = llm_claude_chat_completion_streaming(conversation_history, input_text,
                                                            vision_image, vision_image_size,
                                                            llm_url, CLAUDE_API_KEY, chunk_callback,
                                                            callback_userdata);
            break;
#endif

         default:
            LOG_ERROR("No cloud provider configured");
            return NULL;
      }
   }

   // If cloud LLM failed (but not interrupted by user), try falling back to local
   if (response == NULL && current_type == LLM_CLOUD && !llm_is_interrupt_requested()) {
      if (strcmp(CLOUDAI_URL, llm_url) == 0 || strcmp(CLAUDE_URL, llm_url) == 0) {
         LOG_WARNING("Falling back to local LLM due to connection failure.");
         text_to_speech("Unable to contact cloud LLM.");
         llm_set_type(LLM_LOCAL);

         // Retry with local LLM
#ifdef OPENAI_API_KEY
         response = llm_openai_chat_completion_streaming(conversation_history, input_text,
                                                         vision_image, vision_image_size, llm_url,
                                                         NULL, chunk_callback, callback_userdata);
#endif
         // Record fallback event
         metrics_record_fallback();
      }
   }

   // Record LLM total time
   if (response != NULL) {
      gettimeofday(&end_time, NULL);
      double total_ms = (end_time.tv_sec - start_time.tv_sec) * 1000.0 +
                        (end_time.tv_usec - start_time.tv_usec) / 1000.0;
      metrics_record_llm_total_time(total_ms);
   }

   return response;
}

/**
 * @brief Context for TTS streaming
 *
 * THREADING SAFETY CONTRACT:
 * This structure is stack-allocated in llm_chat_completion_streaming_tts() and relies
 * on synchronous CURL execution. The context remains valid throughout the entire CURL
 * request because curl_easy_perform() blocks until completion, invoking all callbacks
 * from the calling thread before returning.
 *
 * IMPORTANT: If migrating to asynchronous CURL (curl_easy_multi) or background threads:
 * 1. This structure MUST be heap-allocated with explicit lifetime management
 * 2. Add reference counting or retain until all callbacks complete
 * 3. Callbacks may execute after the original function returns
 * 4. Consider using atomic reference counts for multi-threaded access
 *
 * CURRENT IMPLEMENTATION STATUS:
 * - Single-threaded network client processing (dawn.c state machine)
 * - Synchronous CURL execution (safe with stack allocation)
 * - Per-request contexts (no sharing between requests)
 *
 * FUTURE MULTI-CLIENT ARCHITECTURE:
 * - Worker threads will each have their own stack and contexts
 * - No shared state between workers (inherently thread-safe)
 * - Each worker calls this function independently
 * - Stack allocation remains safe as long as CURL stays synchronous
 */
typedef struct {
   sentence_buffer_t *sentence_buffer;
   llm_sentence_callback user_callback;
   void *user_userdata;
} tts_streaming_context_t;

/**
 * @brief Chunk callback that feeds to sentence buffer
 */
static void tts_chunk_callback(const char *chunk, void *userdata) {
   tts_streaming_context_t *ctx = (tts_streaming_context_t *)userdata;
   sentence_buffer_feed(ctx->sentence_buffer, chunk);
}

/**
 * @brief Sentence callback wrapper
 */
static void tts_sentence_callback(const char *sentence, void *userdata) {
   tts_streaming_context_t *ctx = (tts_streaming_context_t *)userdata;
   ctx->user_callback(sentence, ctx->user_userdata);
}

char *llm_chat_completion_streaming_tts(struct json_object *conversation_history,
                                        const char *input_text,
                                        char *vision_image,
                                        size_t vision_image_size,
                                        llm_sentence_callback sentence_callback,
                                        void *callback_userdata) {
   char *response = NULL;
   tts_streaming_context_t ctx;

   // Create sentence buffer
   ctx.sentence_buffer = sentence_buffer_create(tts_sentence_callback, &ctx);
   if (!ctx.sentence_buffer) {
      LOG_ERROR("Failed to create sentence buffer for TTS streaming");
      return NULL;
   }

   ctx.user_callback = sentence_callback;
   ctx.user_userdata = callback_userdata;

   // Call streaming with chunk callback that feeds sentence buffer
   response = llm_chat_completion_streaming(conversation_history, input_text, vision_image,
                                            vision_image_size, tts_chunk_callback, &ctx);

   // Flush any remaining sentence
   sentence_buffer_flush(ctx.sentence_buffer);

   // Cleanup
   sentence_buffer_free(ctx.sentence_buffer);

   return response;
}
