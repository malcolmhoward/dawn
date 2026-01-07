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
 * WebUI Server Implementation - libwebsockets HTTP + WebSocket handling
 */

#define _GNU_SOURCE /* For strcasestr */

#include "webui/webui_server.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <ifaddrs.h>
#include <json-c/json.h>
#include <libwebsockets.h>
#include <limits.h>
#include <mosquitto.h>
#include <net/if.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <string.h>
#include <sys/random.h>
#include <unistd.h>

#ifdef ENABLE_WEBUI_AUDIO
#include "webui/webui_audio.h"
#endif

#include "config/config_env.h"
#include "config/config_parser.h"
#include "config/dawn_config.h"
#include "core/command_router.h"
#include "core/ocp_helpers.h"
#include "core/rate_limiter.h"
#include "core/session_manager.h"
#include "core/text_filter.h"
#include "core/worker_pool.h"
#include "dawn.h"
#include "llm/llm_command_parser.h"
#include "llm/llm_context.h"
#include "llm/llm_interface.h"
#include "llm/llm_tools.h"
#include "logging.h"
#include "state_machine.h"
#include "tools/smartthings_service.h"
#include "tools/string_utils.h"
#include "tts/tts_preprocessing.h"
#include "ui/metrics.h"
#include "version.h"

#ifdef ENABLE_AUTH
#include "auth/auth_crypto.h"
#include "auth/auth_db.h"

/* HTTP 429 Too Many Requests - not defined in older libwebsockets */
#ifndef HTTP_STATUS_TOO_MANY_REQUESTS
#define HTTP_STATUS_TOO_MANY_REQUESTS 429
#endif

/* Rate limiting constants */
#define RATE_LIMIT_WINDOW_SEC (15 * 60) /* 15 minutes */
#define RATE_LIMIT_MAX_ATTEMPTS 20      /* Max attempts per IP in window */

/* CSRF endpoint rate limiting (more permissive - token generation is lighter) */
#define CSRF_RATE_LIMIT_WINDOW_SEC 60 /* 1 minute */
#define CSRF_RATE_LIMIT_MAX 30        /* Max 30 tokens per minute per IP */

/* CSRF single-use nonce tracking (circular buffer)
 * 1024 entries @ 16 bytes = 16KB, covers ~102 req/min with 10-min validity.
 * Must be power of 2 for bitwise AND optimization in circular buffer. */
#define CSRF_USED_NONCE_SIZE 16    /* Nonce size in bytes */
#define CSRF_USED_NONCE_COUNT 1024 /* Track last 1024 used nonces */
_Static_assert((CSRF_USED_NONCE_COUNT & (CSRF_USED_NONCE_COUNT - 1)) == 0,
               "CSRF_USED_NONCE_COUNT must be power of 2");

/* Multi-IP rate limiting for CSRF endpoint */
#define CSRF_RATE_LIMIT_SLOTS 32 /* Track up to 32 concurrent IPs */

/* Multi-IP rate limiting for login endpoint (in-memory fast-path)
 * This supplements the database-backed rate limiting with fast rejection */
#define LOGIN_RATE_LIMIT_SLOTS 32 /* Track up to 32 concurrent IPs */

/**
 * @brief Dummy password hash for timing equalization
 *
 * Used when user is not found to prevent username enumeration via timing.
 * This is a valid Argon2id hash structure that will always fail verification.
 */
static const char DUMMY_PASSWORD_HASH[] = "$argon2id$v=19$m=16384,t=3,p=1$"
                                          "AAAAAAAAAAAAAAAAAAAAAA$"
                                          "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";

#endif /* ENABLE_AUTH */

/* =============================================================================
 * Module State
 * ============================================================================= */

static struct lws_context *s_lws_context = NULL;
static pthread_t s_webui_thread;
static volatile int s_running = 0;
static volatile int s_client_count = 0;
static int s_port = 0;
static char s_www_path[256] = { 0 };
static pthread_mutex_t s_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Config modification mutex - protects against concurrent config reads during writes */
static pthread_rwlock_t s_config_rwlock = PTHREAD_RWLOCK_INITIALIZER;

#ifdef ENABLE_AUTH
/* CSRF single-use nonce tracking (circular buffer)
 * Tracks recently used nonces to prevent token replay within validity window */
static struct {
   unsigned char nonces[CSRF_USED_NONCE_COUNT][CSRF_USED_NONCE_SIZE];
   int head;
   pthread_mutex_t mutex;
} s_csrf_used = { .head = 0, .mutex = PTHREAD_MUTEX_INITIALIZER };

/* CSRF endpoint rate limiting (uses generic rate limiter) */
static rate_limit_entry_t s_csrf_rate_entries[CSRF_RATE_LIMIT_SLOTS];
static rate_limiter_t s_csrf_rate = RATE_LIMITER_STATIC_INIT(s_csrf_rate_entries,
                                                             CSRF_RATE_LIMIT_SLOTS,
                                                             CSRF_RATE_LIMIT_MAX,
                                                             CSRF_RATE_LIMIT_WINDOW_SEC);

/* Login rate limiting (uses generic rate limiter) */
static rate_limit_entry_t s_login_rate_entries[LOGIN_RATE_LIMIT_SLOTS];
static rate_limiter_t s_login_rate = RATE_LIMITER_STATIC_INIT(s_login_rate_entries,
                                                              LOGIN_RATE_LIMIT_SLOTS,
                                                              RATE_LIMIT_MAX_ATTEMPTS,
                                                              RATE_LIMIT_WINDOW_SEC);
#endif /* ENABLE_AUTH */

/* =============================================================================
 * Response Queue (worker -> WebUI thread)
 *
 * Workers cannot call lws_write() directly (not thread-safe).
 * They queue responses here, then call lws_cancel_service() to wake
 * the WebUI thread, which processes the queue in LWS_CALLBACK_EVENT_WAIT_CANCELLED.
 * ============================================================================= */

typedef struct {
   session_t *session;
   ws_response_type_t type;
   union {
      struct {
         char *state;
         char *detail; /* Optional detail message (e.g., "Fetching URL...") */
      } state;
      struct {
         char *role;
         char *text;
      } transcript;
      struct {
         char *code;
         char *message;
      } error;
      struct {
         char *token;
      } session_token;
      struct {
         uint8_t *data;
         size_t len;
      } audio;
      struct {
         int current_tokens;
         int max_tokens;
         float threshold;
      } context;
      struct {
         uint32_t stream_id;
         char text[128]; /* Fixed buffer for delta/end text (no malloc/free churn) */
      } stream;
      struct {
         char state[16];   /* idle, listening, thinking, speaking, error */
         int ttft_ms;      /* Time to first token (ms) */
         float token_rate; /* Tokens per second */
         int context_pct;  /* Context utilization 0-100 */
      } metrics;
      struct {
         int tokens_before;
         int tokens_after;
         int messages_summarized;
         char *summary;
      } compaction;
   };
} ws_response_t;

static ws_response_t s_response_queue[WEBUI_RESPONSE_QUEUE_SIZE];
static int s_queue_head = 0;
static int s_queue_tail = 0;
static pthread_mutex_t s_queue_mutex = PTHREAD_MUTEX_INITIALIZER;

/* =============================================================================
 * Token-to-Session Mapping (for reconnection)
 * ============================================================================= */

#define MAX_TOKEN_MAPPINGS 16

typedef struct {
   char token[WEBUI_SESSION_TOKEN_LEN];
   uint32_t session_id;
   time_t created;
   bool in_use;
} token_mapping_t;

static token_mapping_t s_token_map[MAX_TOKEN_MAPPINGS];
static pthread_mutex_t s_token_mutex = PTHREAD_MUTEX_INITIALIZER;

static void register_token(const char *token, uint32_t session_id) {
   pthread_mutex_lock(&s_token_mutex);

   /* Find existing or empty slot */
   int empty_slot = -1;
   for (int i = 0; i < MAX_TOKEN_MAPPINGS; i++) {
      if (s_token_map[i].in_use && strcmp(s_token_map[i].token, token) == 0) {
         /* Update existing */
         s_token_map[i].session_id = session_id;
         s_token_map[i].created = time(NULL);
         pthread_mutex_unlock(&s_token_mutex);
         return;
      }
      if (!s_token_map[i].in_use && empty_slot < 0) {
         empty_slot = i;
      }
   }

   if (empty_slot >= 0) {
      strncpy(s_token_map[empty_slot].token, token, WEBUI_SESSION_TOKEN_LEN - 1);
      s_token_map[empty_slot].token[WEBUI_SESSION_TOKEN_LEN - 1] = '\0';
      s_token_map[empty_slot].session_id = session_id;
      s_token_map[empty_slot].created = time(NULL);
      s_token_map[empty_slot].in_use = true;
   } else {
      /* Table full - evict oldest */
      int oldest = 0;
      for (int i = 1; i < MAX_TOKEN_MAPPINGS; i++) {
         if (s_token_map[i].created < s_token_map[oldest].created) {
            oldest = i;
         }
      }
      strncpy(s_token_map[oldest].token, token, WEBUI_SESSION_TOKEN_LEN - 1);
      s_token_map[oldest].token[WEBUI_SESSION_TOKEN_LEN - 1] = '\0';
      s_token_map[oldest].session_id = session_id;
      s_token_map[oldest].created = time(NULL);
   }

   pthread_mutex_unlock(&s_token_mutex);
}

static session_t *lookup_session_by_token(const char *token) {
   pthread_mutex_lock(&s_token_mutex);

   for (int i = 0; i < MAX_TOKEN_MAPPINGS; i++) {
      if (s_token_map[i].in_use && strcmp(s_token_map[i].token, token) == 0) {
         uint32_t session_id = s_token_map[i].session_id;
         pthread_mutex_unlock(&s_token_mutex);

         /* Look up actual session - use reconnect variant to allow disconnected sessions */
         session_t *session = session_get_for_reconnect(session_id);
         if (session) {
            /* Session exists - reconnect handler will clear disconnected flag */
            LOG_INFO("WebUI: Found existing session %u for token %.8s...", session_id, token);
            return session;
         }
         LOG_INFO("WebUI: Token %.8s... mapped to session %u but session destroyed", token,
                  session_id);
         return NULL;
      }
   }

   pthread_mutex_unlock(&s_token_mutex);
   return NULL;
}

/* =============================================================================
 * Model/Interface Cache (avoids repeated filesystem/network scans)
 * ============================================================================= */

#define MODEL_CACHE_TTL 60 /* Cache refresh interval in seconds */

typedef struct {
   json_object *models_response;     /* Cached list_models_response */
   json_object *interfaces_response; /* Cached list_interfaces_response */
   time_t models_cache_time;         /* When models were last scanned */
   time_t interfaces_cache_time;     /* When interfaces were last enumerated */
   pthread_mutex_t cache_mutex;      /* Protects cache access */
} discovery_cache_t;

static discovery_cache_t s_discovery_cache = { .models_response = NULL,
                                               .interfaces_response = NULL,
                                               .models_cache_time = 0,
                                               .interfaces_cache_time = 0,
                                               .cache_mutex = PTHREAD_MUTEX_INITIALIZER };

/* =============================================================================
 * Allowed Path Prefixes for Model Directory Scanning
 *
 * Security: Restricts which directories can be scanned for models.
 * The current working directory is always allowed in addition to these.
 * Modify this list to allow/disallow specific paths.
 * ============================================================================= */

static const char *s_allowed_path_prefixes[] = {
   "/home/",      "/var/lib/", "/opt/", "/usr/local/share/",
   "/usr/share/", NULL /* Sentinel - must be last */
};

/* =============================================================================
 * Per-WebSocket Connection Data
 * ============================================================================= */

typedef struct {
   struct lws *wsi;                             /* libwebsockets handle */
   session_t *session;                          /* Session manager reference */
   char session_token[WEBUI_SESSION_TOKEN_LEN]; /* Reconnection token */
   uint8_t *audio_buffer;                       /* Phase 4: Opus audio accumulation */
   size_t audio_buffer_len;
   size_t audio_buffer_capacity;
   bool in_binary_fragment; /* True if receiving fragmented binary frame */
   uint8_t binary_msg_type; /* Message type from first fragment */
   bool use_opus;           /* True if client supports Opus codec */

   /* Auth state (populated at WebSocket establishment from HTTP cookie) */
   bool authenticated;
   int auth_user_id;
   char auth_session_token[AUTH_TOKEN_LEN]; /* For DB re-validation */
   char username[AUTH_USERNAME_MAX];
   /* Note: is_admin NOT cached - re-validated from DB on each admin operation */

   /* Client IP address (captured at connection establishment for reliable logging) */
   char client_ip[64];
} ws_connection_t;

/* =============================================================================
 * MIME Type Mapping
 * ============================================================================= */

static const struct {
   const char *extension;
   const char *mime_type;
} s_mime_types[] = {
   { ".html", "text/html" },
   { ".htm", "text/html" },
   { ".css", "text/css" },
   { ".js", "application/javascript" },
   { ".json", "application/json" },
   { ".wasm", "application/wasm" },
   { ".png", "image/png" },
   { ".jpg", "image/jpeg" },
   { ".jpeg", "image/jpeg" },
   { ".gif", "image/gif" },
   { ".svg", "image/svg+xml" },
   { ".ico", "image/x-icon" },
   { ".woff", "font/woff" },
   { ".woff2", "font/woff2" },
   { ".ttf", "font/ttf" },
   { ".txt", "text/plain" },
   { NULL, NULL },
};

static const char *get_mime_type(const char *path) {
   const char *ext = strrchr(path, '.');
   if (!ext) {
      return "application/octet-stream";
   }

   for (int i = 0; s_mime_types[i].extension != NULL; i++) {
      if (strcasecmp(ext, s_mime_types[i].extension) == 0) {
         return s_mime_types[i].mime_type;
      }
   }

   return "application/octet-stream";
}

/**
 * @brief Check if a path contains directory traversal patterns
 *
 * Checks for literal ".." as well as URL-encoded variants (%2e, %252e)
 * to prevent path traversal attacks.
 *
 * @param path The URL path to check
 * @return true if traversal pattern detected, false if path is safe
 */
static bool contains_path_traversal(const char *path) {
   if (!path) {
      return false;
   }

   /* Check for literal ".." */
   if (strstr(path, "..") != NULL) {
      return true;
   }

   /* Check for URL-encoded variants (case-insensitive) */
   /* %2e = ".", so %2e%2e = ".." */
   if (strcasestr(path, "%2e%2e") != NULL) {
      return true;
   }

   /* Single encoded dot followed by literal dot or vice versa */
   if (strcasestr(path, "%2e.") != NULL || strcasestr(path, ".%2e") != NULL) {
      return true;
   }

   /* Double-encoded: %252e = "%2e" after first decode */
   if (strcasestr(path, "%252e") != NULL) {
      return true;
   }

   return false;
}

/**
 * @brief Validate that a resolved path is within the allowed directory
 *
 * Uses realpath() to resolve symlinks and relative paths, then verifies
 * the canonical path is within the www directory.
 *
 * @param filepath The filesystem path to validate
 * @param www_path The allowed base directory (www path)
 * @return true if path is safe (within www_path), false otherwise
 */
static bool is_path_within_www(const char *filepath, const char *www_path) {
   char resolved_path[PATH_MAX];
   char resolved_www[PATH_MAX];

   /* Resolve the www base path */
   if (realpath(www_path, resolved_www) == NULL) {
      LOG_ERROR("WebUI: Cannot resolve www path: %s", www_path);
      return false;
   }

   /* Resolve the requested file path */
   if (realpath(filepath, resolved_path) == NULL) {
      /* File doesn't exist - check parent directory instead */
      /* This allows serving files that don't exist yet (404 handled elsewhere) */
      char *filepath_copy = strdup(filepath);
      if (!filepath_copy) {
         return false;
      }

      /* Find last slash to get parent directory */
      char *last_slash = strrchr(filepath_copy, '/');
      if (last_slash && last_slash != filepath_copy) {
         *last_slash = '\0';
         if (realpath(filepath_copy, resolved_path) == NULL) {
            free(filepath_copy);
            return false;
         }
      } else {
         free(filepath_copy);
         return false;
      }
      free(filepath_copy);
   }

   /* Ensure resolved path starts with resolved www path */
   size_t www_len = strlen(resolved_www);
   if (strncmp(resolved_path, resolved_www, www_len) != 0) {
      return false;
   }

   /* Ensure it's either exact match or followed by '/' */
   if (resolved_path[www_len] != '\0' && resolved_path[www_len] != '/') {
      return false;
   }

   return true;
}

/* =============================================================================
 * HTTP Session Data
 * ============================================================================= */

#define HTTP_MAX_POST_BODY 4096
#define AUTH_COOKIE_NAME "dawn_session"
#define AUTH_COOKIE_MAX_AGE (24 * 60 * 60) /* 24 hours */

struct http_session_data {
   char path[256]; /* Request path */
   char post_body[HTTP_MAX_POST_BODY];
   size_t post_body_len;
   bool is_post;
};

/* =============================================================================
 * Session Token Generation
 * ============================================================================= */

/**
 * @brief Generate a cryptographically secure session token
 *
 * @param token_out Buffer to store the hex-encoded token (must be WEBUI_SESSION_TOKEN_LEN)
 * @return 0 on success, 1 on failure (token_out will be empty string)
 */
static int generate_session_token(char token_out[WEBUI_SESSION_TOKEN_LEN]) {
   uint8_t random_bytes[16];
   if (getrandom(random_bytes, 16, 0) != 16) {
      /* Security: fail instead of using weak random - getrandom should never fail on modern Linux
       */
      LOG_ERROR("getrandom() failed - cannot generate secure session token");
      token_out[0] = '\0';
      return 1;
   }
   for (int i = 0; i < 16; i++) {
      snprintf(&token_out[i * 2], 3, "%02x", random_bytes[i]);
   }
   token_out[32] = '\0';
   return 0;
}

/* =============================================================================
 * Response Queue Functions
 * ============================================================================= */

static void queue_response(ws_response_t *resp) {
   pthread_mutex_lock(&s_queue_mutex);

   int next_tail = (s_queue_tail + 1) % WEBUI_RESPONSE_QUEUE_SIZE;
   if (next_tail == s_queue_head) {
      /* Queue full - drop oldest */
      LOG_WARNING("WebUI: Response queue full, dropping oldest entry");
      s_queue_head = (s_queue_head + 1) % WEBUI_RESPONSE_QUEUE_SIZE;
   }

   s_response_queue[s_queue_tail] = *resp;
   s_queue_tail = next_tail;

   pthread_mutex_unlock(&s_queue_mutex);

   /* Wake up lws_service() to process queue */
   if (s_lws_context) {
      lws_cancel_service(s_lws_context);
   }
}

static void free_response(ws_response_t *resp) {
   switch (resp->type) {
      case WS_RESP_STATE:
         free(resp->state.state);
         free(resp->state.detail);
         break;
      case WS_RESP_TRANSCRIPT:
         free(resp->transcript.role);
         free(resp->transcript.text);
         break;
      case WS_RESP_ERROR:
         free(resp->error.code);
         free(resp->error.message);
         break;
      case WS_RESP_SESSION:
         free(resp->session_token.token);
         break;
      case WS_RESP_AUDIO:
         free(resp->audio.data);
         break;
      case WS_RESP_AUDIO_END:
         /* No data to free */
         break;
      case WS_RESP_CONTEXT:
         /* No data to free - all inline values */
         break;
      case WS_RESP_STREAM_START:
      case WS_RESP_STREAM_DELTA:
      case WS_RESP_STREAM_END:
         /* No data to free - text[] is inline fixed buffer */
         break;
      case WS_RESP_METRICS_UPDATE:
         /* No data to free - all inline values */
         break;
      case WS_RESP_COMPACTION_COMPLETE:
         free(resp->compaction.summary);
         break;
   }
}

/* =============================================================================
 * WebSocket Send Helpers (called from WebUI thread only)
 * ============================================================================= */

/* LWS requires LWS_PRE bytes before the buffer for protocol framing */
/* Buffer size for WebSocket messages. Must be large enough to hold tool results
 * (LLM_TOOLS_RESULT_LEN = 8192) plus JSON overhead for transcript messages. */
#define WS_SEND_BUFFER_SIZE 16384

static int send_json_message(struct lws *wsi, const char *json) {
   size_t len = strlen(json);

   /* Log messages that may be too large for HTTP/2 frames (default 16KB) */
   if (len > 12000) {
      /* Try to extract message type for better logging */
      const char *type_start = strstr(json, "\"type\":\"");
      char type_buf[32] = "unknown";
      if (type_start) {
         type_start += 8;
         const char *type_end = strchr(type_start, '"');
         if (type_end && (size_t)(type_end - type_start) < sizeof(type_buf) - 1) {
            strncpy(type_buf, type_start, (size_t)(type_end - type_start));
            type_buf[type_end - type_start] = '\0';
         }
      }
      LOG_WARNING("WebUI: Large message via send_json_message: type=%s, size=%zu bytes", type_buf,
                  len);
   }

   if (len >= WS_SEND_BUFFER_SIZE - LWS_PRE) {
      LOG_ERROR("WebUI: JSON message too large (%zu bytes, max %d)", len,
                (int)(WS_SEND_BUFFER_SIZE - LWS_PRE));
      return -1;
   }

   unsigned char buf[LWS_PRE + WS_SEND_BUFFER_SIZE];
   memcpy(&buf[LWS_PRE], json, len);

   int written = lws_write(wsi, &buf[LWS_PRE], len, LWS_WRITE_TEXT);
   if (written < (int)len) {
      LOG_ERROR("WebUI: lws_write failed (wrote %d of %zu)", written, len);
      return -1;
   }

   return 0;
}

static int send_binary_message(struct lws *wsi, uint8_t msg_type, const uint8_t *data, size_t len) {
   if (!wsi) {
      LOG_ERROR("WebUI: send_binary_message called with NULL wsi");
      return -1;
   }

   /* Allocate buffer with LWS_PRE padding + 1 byte for type + data */
   size_t total_len = 1 + len;
   unsigned char *buf = malloc(LWS_PRE + total_len);
   if (!buf) {
      LOG_ERROR("WebUI: Failed to allocate binary send buffer (%zu bytes)", LWS_PRE + total_len);
      return -1;
   }

   buf[LWS_PRE] = msg_type;
   if (data && len > 0) {
      memcpy(&buf[LWS_PRE + 1], data, len);
   }

   int written = lws_write(wsi, &buf[LWS_PRE], total_len, LWS_WRITE_BINARY);
   free(buf);

   if (written < 0) {
      LOG_ERROR("WebUI: lws_write binary failed with error %d", written);
      return -1;
   }

   if (written < (int)total_len) {
      LOG_ERROR("WebUI: lws_write binary partial write (%d of %zu)", written, total_len);
      return -1;
   }

   return 0;
}

static void send_audio_impl(struct lws *wsi, const uint8_t *data, size_t len) {
   int ret = send_binary_message(wsi, WS_BIN_AUDIO_OUT, data, len);
   if (ret != 0) {
      LOG_ERROR("WebUI: Failed to send audio chunk (%zu bytes)", len);
   }
}

static void send_audio_end_impl(struct lws *wsi) {
   send_binary_message(wsi, WS_BIN_AUDIO_SEGMENT_END, NULL, 0);
}

/**
 * @brief Check if client capabilities include Opus audio codec support
 *
 * Parses the payload for capabilities.audio_codecs array and checks
 * if "opus" is in the list. Falls back to PCM if not specified.
 *
 * @param payload JSON object from init/reconnect message
 * @return true if client supports Opus, false otherwise
 */
static bool check_opus_capability(struct json_object *payload) {
   if (!payload) {
      return false;
   }

   struct json_object *capabilities;
   if (!json_object_object_get_ex(payload, "capabilities", &capabilities)) {
      return false;
   }

   struct json_object *audio_codecs;
   if (!json_object_object_get_ex(capabilities, "audio_codecs", &audio_codecs)) {
      return false;
   }

   if (!json_object_is_type(audio_codecs, json_type_array)) {
      return false;
   }

   int len = json_object_array_length(audio_codecs);
   /* Defensive bound: no client should send more than 16 codecs */
   if (len > 16) {
      LOG_WARNING("WebUI: Too many audio codecs in capability list (%d), ignoring", len);
      return false;
   }

   for (int i = 0; i < len; i++) {
      struct json_object *codec = json_object_array_get_idx(audio_codecs, i);
      if (codec && json_object_is_type(codec, json_type_string)) {
         const char *codec_str = json_object_get_string(codec);
         if (codec_str && strcmp(codec_str, "opus") == 0) {
            return true;
         }
      }
   }

   return false;
}

static void send_state_impl(struct lws *wsi, const char *state, const char *detail) {
   char json[512];
   if (detail && detail[0] != '\0') {
      snprintf(json, sizeof(json),
               "{\"type\":\"state\",\"payload\":{\"state\":\"%s\",\"detail\":\"%s\"}}", state,
               detail);
   } else {
      snprintf(json, sizeof(json), "{\"type\":\"state\",\"payload\":{\"state\":\"%s\"}}", state);
   }
   send_json_message(wsi, json);
}

static void send_transcript_impl(struct lws *wsi, const char *role, const char *text) {
   /* Escape JSON special characters in text */
   struct json_object *obj = json_object_new_object();
   struct json_object *payload = json_object_new_object();

   json_object_object_add(payload, "role", json_object_new_string(role));
   json_object_object_add(payload, "text", json_object_new_string(text));
   json_object_object_add(obj, "type", json_object_new_string("transcript"));
   json_object_object_add(obj, "payload", payload);

   const char *json_str = json_object_to_json_string(obj);
   send_json_message(wsi, json_str);
   json_object_put(obj);
}

static void send_error_impl(struct lws *wsi, const char *code, const char *message) {
   struct json_object *obj = json_object_new_object();
   struct json_object *payload = json_object_new_object();

   json_object_object_add(payload, "code", json_object_new_string(code));
   json_object_object_add(payload, "message", json_object_new_string(message));
   json_object_object_add(payload, "recoverable", json_object_new_boolean(1));
   json_object_object_add(obj, "type", json_object_new_string("error"));
   json_object_object_add(obj, "payload", payload);

   const char *json_str = json_object_to_json_string(obj);
   send_json_message(wsi, json_str);
   json_object_put(obj);
}

static void send_session_token_impl(ws_connection_t *conn, const char *token) {
   char json[512];

   /* Include auth state in session response to avoid separate config fetch */
   if (conn->authenticated) {
      /* Fetch is_admin from DB (not cached to prevent stale state) */
      auth_session_t auth_session;
      bool is_admin = false;
      if (auth_db_get_session(conn->auth_session_token, &auth_session) == AUTH_DB_SUCCESS) {
         is_admin = auth_session.is_admin;
      }
      snprintf(json, sizeof(json),
               "{\"type\":\"session\",\"payload\":{\"token\":\"%s\","
               "\"authenticated\":true,\"username\":\"%s\",\"is_admin\":%s}}",
               token, conn->username, is_admin ? "true" : "false");
   } else {
      snprintf(json, sizeof(json),
               "{\"type\":\"session\",\"payload\":{\"token\":\"%s\","
               "\"authenticated\":false}}",
               token);
   }
   send_json_message(conn->wsi, json);
}

static void send_config_impl(struct lws *wsi) {
   char json[256];
   snprintf(json, sizeof(json), "{\"type\":\"config\",\"payload\":{\"audio_chunk_ms\":%d}}",
            g_config.webui.audio_chunk_ms);
   send_json_message(wsi, json);
}

static void send_context_impl(struct lws *wsi,
                              int current_tokens,
                              int max_tokens,
                              float threshold) {
   char json[256];
   float usage_pct = (max_tokens > 0) ? (float)current_tokens / (float)max_tokens * 100.0f : 0;
   snprintf(json, sizeof(json),
            "{\"type\":\"context\",\"payload\":{\"current\":%d,\"max\":%d,\"usage\":%.1f,"
            "\"threshold\":%.0f}}",
            current_tokens, max_tokens, usage_pct, threshold * 100.0f);
   send_json_message(wsi, json);
}

static void send_metrics_impl(struct lws *wsi,
                              const char *state,
                              int ttft_ms,
                              float token_rate,
                              int context_pct) {
   char json[256];
   snprintf(json, sizeof(json),
            "{\"type\":\"metrics_update\",\"payload\":{\"state\":\"%s\",\"ttft_ms\":%d,"
            "\"token_rate\":%.1f,\"context_percent\":%d}}",
            state, ttft_ms, token_rate, context_pct);
   send_json_message(wsi, json);
}

static void send_compaction_impl(struct lws *wsi,
                                 int tokens_before,
                                 int tokens_after,
                                 int messages_summarized,
                                 const char *summary) {
   struct json_object *obj = json_object_new_object();
   struct json_object *payload = json_object_new_object();

   json_object_object_add(payload, "tokens_before", json_object_new_int(tokens_before));
   json_object_object_add(payload, "tokens_after", json_object_new_int(tokens_after));
   json_object_object_add(payload, "messages_summarized", json_object_new_int(messages_summarized));
   if (summary) {
      json_object_object_add(payload, "summary", json_object_new_string(summary));
   }
   json_object_object_add(obj, "type", json_object_new_string("context_compacted"));
   json_object_object_add(obj, "payload", payload);

   const char *json = json_object_to_json_string(obj);
   send_json_message(wsi, json);
   json_object_put(obj);
}

/* =============================================================================
 * LLM Streaming Impl Functions (ChatGPT-style real-time text)
 *
 * Protocol:
 *   stream_start - Create new assistant entry, enter streaming state
 *   stream_delta - Append text to current entry
 *   stream_end   - Finalize entry, exit streaming state
 * ============================================================================= */

static void send_stream_start_impl(struct lws *wsi, uint32_t stream_id) {
   char json[128];
   snprintf(json, sizeof(json), "{\"type\":\"stream_start\",\"payload\":{\"stream_id\":%u}}",
            stream_id);
   send_json_message(wsi, json);
}

static void send_stream_delta_impl(struct lws *wsi, uint32_t stream_id, const char *text) {
   struct json_object *obj = json_object_new_object();
   struct json_object *payload = json_object_new_object();

   json_object_object_add(payload, "stream_id", json_object_new_int((int32_t)stream_id));
   json_object_object_add(payload, "delta", json_object_new_string(text));
   json_object_object_add(obj, "type", json_object_new_string("stream_delta"));
   json_object_object_add(obj, "payload", payload);

   const char *json_str = json_object_to_json_string(obj);
   send_json_message(wsi, json_str);
   json_object_put(obj);
}

static void send_stream_end_impl(struct lws *wsi, uint32_t stream_id, const char *reason) {
   char json[256];
   snprintf(json, sizeof(json),
            "{\"type\":\"stream_end\",\"payload\":{\"stream_id\":%u,\"reason\":\"%s\"}}", stream_id,
            reason ? reason : "complete");
   send_json_message(wsi, json);
}

/**
 * @brief Send conversation history to client on reconnect
 *
 * Iterates through the session's conversation history and sends each
 * user/assistant message as a transcript. Skips system messages.
 *
 * @param wsi WebSocket instance
 * @param session Session with conversation history
 */
static void send_history_impl(struct lws *wsi, session_t *session) {
   if (!wsi || !session) {
      return;
   }

   struct json_object *history = session_get_history(session);
   if (!history) {
      LOG_WARNING("WebUI: Failed to get history for session %u", session->session_id);
      return;
   }

   int len = json_object_array_length(history);
   int sent_count = 0;

   LOG_INFO("WebUI: Sending %d history entries to reconnected client", len);

   for (int i = 0; i < len; i++) {
      struct json_object *msg = json_object_array_get_idx(history, i);
      if (!msg)
         continue;

      struct json_object *role_obj = NULL;
      struct json_object *content_obj = NULL;

      if (!json_object_object_get_ex(msg, "role", &role_obj) ||
          !json_object_object_get_ex(msg, "content", &content_obj)) {
         continue;
      }

      const char *role = json_object_get_string(role_obj);
      const char *content = json_object_get_string(content_obj);

      /* Skip system messages (prompts) - only send user/assistant */
      if (!role || !content || strcmp(role, "system") == 0) {
         continue;
      }

      send_transcript_impl(wsi, role, content);
      sent_count++;
   }

   /* Release the reference we got from session_get_history */
   json_object_put(history);

   LOG_INFO("WebUI: Sent %d transcript entries to reconnected client", sent_count);
}

/* =============================================================================
 * Response Queue Processing (called from WebUI thread)
 *
 * IMPORTANT: libwebsockets only allows ONE lws_write() per writeable callback.
 * This function processes only ONE response per call. If more responses are
 * pending, it requests a writeable callback via lws_callback_on_writable().
 * ============================================================================= */

static void process_one_response(void) {
   pthread_mutex_lock(&s_queue_mutex);

   if (s_queue_head == s_queue_tail) {
      pthread_mutex_unlock(&s_queue_mutex);
      return;
   }

   ws_response_t resp = s_response_queue[s_queue_head];
   s_queue_head = (s_queue_head + 1) % WEBUI_RESPONSE_QUEUE_SIZE;
   bool more_pending = (s_queue_head != s_queue_tail);
   pthread_mutex_unlock(&s_queue_mutex);

   /* Find connection for this session */
   if (!resp.session || resp.session->disconnected) {
      free_response(&resp);
      /* If more responses, schedule another callback */
      if (more_pending && s_lws_context) {
         lws_cancel_service(s_lws_context);
      }
      return;
   }

   ws_connection_t *conn = (ws_connection_t *)resp.session->client_data;
   if (!conn || !conn->wsi) {
      free_response(&resp);
      if (more_pending && s_lws_context) {
         lws_cancel_service(s_lws_context);
      }
      return;
   }

   /* Send via lws_write (one write per callback!) */
   switch (resp.type) {
      case WS_RESP_STATE:
         send_state_impl(conn->wsi, resp.state.state, resp.state.detail);
         free(resp.state.state);
         free(resp.state.detail);
         break;
      case WS_RESP_TRANSCRIPT:
         send_transcript_impl(conn->wsi, resp.transcript.role, resp.transcript.text);
         free(resp.transcript.role);
         free(resp.transcript.text);
         break;
      case WS_RESP_ERROR:
         send_error_impl(conn->wsi, resp.error.code, resp.error.message);
         free(resp.error.code);
         free(resp.error.message);
         break;
      case WS_RESP_SESSION:
         send_session_token_impl(conn, resp.session_token.token);
         free(resp.session_token.token);
         break;
      case WS_RESP_AUDIO:
         send_audio_impl(conn->wsi, resp.audio.data, resp.audio.len);
         free(resp.audio.data);
         break;
      case WS_RESP_AUDIO_END:
         send_audio_end_impl(conn->wsi);
         break;
      case WS_RESP_CONTEXT:
         send_context_impl(conn->wsi, resp.context.current_tokens, resp.context.max_tokens,
                           resp.context.threshold);
         break;
      case WS_RESP_STREAM_START:
         send_stream_start_impl(conn->wsi, resp.stream.stream_id);
         break;
      case WS_RESP_STREAM_DELTA:
         send_stream_delta_impl(conn->wsi, resp.stream.stream_id, resp.stream.text);
         /* text[] is inline buffer - no free needed */
         break;
      case WS_RESP_STREAM_END:
         send_stream_end_impl(conn->wsi, resp.stream.stream_id, resp.stream.text);
         /* text[] is inline buffer - no free needed */
         break;
      case WS_RESP_METRICS_UPDATE:
         send_metrics_impl(conn->wsi, resp.metrics.state, resp.metrics.ttft_ms,
                           resp.metrics.token_rate, resp.metrics.context_pct);
         break;
      case WS_RESP_COMPACTION_COMPLETE:
         send_compaction_impl(conn->wsi, resp.compaction.tokens_before,
                              resp.compaction.tokens_after, resp.compaction.messages_summarized,
                              resp.compaction.summary);
         free(resp.compaction.summary);
         break;
   }

   /* If more responses pending, request writeable callback for this connection */
   if (more_pending) {
      lws_callback_on_writable(conn->wsi);
   }
}

/* Legacy name for backward compatibility */
static void process_response_queue(void) {
   process_one_response();
}

/* =============================================================================
 * Authentication Helpers
 * ============================================================================= */

#ifdef ENABLE_AUTH

/**
 * @brief Extract session token from Cookie header
 * @param wsi WebSocket/HTTP connection
 * @param token_out Buffer to store token (must be AUTH_TOKEN_LEN bytes)
 * @return true if token found, false otherwise
 */
static bool extract_session_cookie(struct lws *wsi, char *token_out) {
   char cookie_buf[512];
   int len = lws_hdr_copy(wsi, cookie_buf, sizeof(cookie_buf), WSI_TOKEN_HTTP_COOKIE);
   if (len <= 0) {
      return false;
   }

   /* Parse cookie header for dawn_session=<token> */
   const char *prefix = AUTH_COOKIE_NAME "=";
   char *start = strstr(cookie_buf, prefix);
   if (!start) {
      return false;
   }

   start += strlen(prefix);
   char *end = strchr(start, ';');
   size_t token_len = end ? (size_t)(end - start) : strlen(start);

   if (token_len >= AUTH_TOKEN_LEN || token_len == 0) {
      return false;
   }

   memcpy(token_out, start, token_len);
   token_out[token_len] = '\0';
   return true;
}

/**
 * @brief Check if request is authenticated via session cookie
 * @param wsi WebSocket/HTTP connection
 * @param session_out If not NULL, filled with session info on success
 * @return true if authenticated, false otherwise
 */
static bool is_request_authenticated(struct lws *wsi, auth_session_t *session_out) {
   char token[AUTH_TOKEN_LEN];
   if (!extract_session_cookie(wsi, token)) {
      return false;
   }

   auth_session_t session;
   if (auth_db_get_session(token, &session) != AUTH_DB_SUCCESS) {
      return false;
   }

   /* Update session activity */
   auth_db_update_session_activity(token);

   if (session_out) {
      *session_out = session;
   }
   return true;
}

/**
 * @brief Check if WebSocket connection is authenticated
 *
 * CRITICAL: Re-validates session against database to prevent TOCTOU attacks
 * where session may have been revoked (password change, admin action, etc.)
 * but cached conn->authenticated flag remains true.
 *
 * Sends UNAUTHORIZED error if not authenticated or session invalid.
 *
 * @param conn WebSocket connection
 * @return true if authenticated with valid session, false otherwise (error sent)
 */
static bool conn_require_auth(ws_connection_t *conn) {
   if (!conn->authenticated) {
      send_error_impl(conn->wsi, "UNAUTHORIZED", "Authentication required");
      return false;
   }

   /* Re-validate session from DB (prevents stale session exploitation) */
   auth_session_t session;
   if (auth_db_get_session(conn->auth_session_token, &session) != AUTH_DB_SUCCESS) {
      conn->authenticated = false;
      send_error_impl(conn->wsi, "UNAUTHORIZED", "Session expired or revoked");
      return false;
   }

   return true;
}

/**
 * @brief Check if WebSocket connection has admin privileges
 *
 * CRITICAL: Re-validates is_admin against database to prevent stale cache
 * exploitation if user is demoted mid-session.
 *
 * Sends UNAUTHORIZED if not authenticated, FORBIDDEN if not admin.
 *
 * @param conn WebSocket connection
 * @return true if admin, false otherwise (error sent)
 */
static bool conn_require_admin(ws_connection_t *conn) {
   if (!conn->authenticated) {
      send_error_impl(conn->wsi, "UNAUTHORIZED", "Authentication required");
      return false;
   }

   /* Re-validate session from DB (prevents stale is_admin cache) */
   auth_session_t session;
   if (auth_db_get_session(conn->auth_session_token, &session) != AUTH_DB_SUCCESS) {
      conn->authenticated = false;
      send_error_impl(conn->wsi, "UNAUTHORIZED", "Session expired");
      return false;
   }

   if (!session.is_admin) {
      auth_db_log_event("PERMISSION_DENIED", conn->username, conn->client_ip,
                        "Admin access required");
      send_error_impl(conn->wsi, "FORBIDDEN", "Admin access required");
      return false;
   }

   return true;
}

/**
 * @brief Build a personalized system prompt with user settings
 *
 * Supports two modes based on user's persona_mode setting:
 * - "append" (default): Add user settings as additional context at the end
 * - "replace": Prepend user's custom persona with override instruction
 *
 * @param user_id User ID (0 for unauthenticated - returns base prompt copy)
 * @return Allocated prompt string (caller must free)
 */
static char *build_user_prompt(int user_id) {
   const char *base_prompt = get_remote_command_prompt();
   if (!base_prompt) {
      return NULL;
   }

   /* No user ID - return copy of base prompt */
   if (user_id <= 0) {
      return strdup(base_prompt);
   }

   /* Load user settings */
   auth_user_settings_t settings;
   if (auth_db_get_user_settings(user_id, &settings) != AUTH_DB_SUCCESS) {
      return strdup(base_prompt);
   }

   /* Check if any settings are customized */
   bool has_persona = settings.persona_description[0] != '\0';
   bool has_location = settings.location[0] != '\0';
   bool has_timezone = settings.timezone[0] != '\0';
   bool has_units = settings.units[0] != '\0';
   bool is_replace_mode = (strcmp(settings.persona_mode, "replace") == 0);

   if (!has_persona && !has_location && !has_timezone && !has_units) {
      return strdup(base_prompt);
   }

   size_t base_len = strlen(base_prompt);

   /* Replace mode: Prepend custom persona with override instruction */
   if (is_replace_mode && has_persona) {
      /* Build replacement prefix (persona 512 + boilerplate ~130 = ~650 max) */
      char prefix[768];
      int prefix_len = snprintf(prefix, sizeof(prefix),
                                "## Your Identity\n%s\n\n"
                                "IMPORTANT: Use the identity above. Ignore any conflicting persona "
                                "descriptions that follow.\n\n",
                                settings.persona_description);

      /* Build suffix with other user context (loc 128 + tz 64 + units 16 = ~250 max) */
      char suffix[320];
      int suffix_offset = 0;

      if (has_location || has_timezone || has_units) {
         suffix_offset += snprintf(suffix + suffix_offset, sizeof(suffix) - suffix_offset,
                                   "\n\n## User Info\n");
         if (has_location) {
            suffix_offset += snprintf(suffix + suffix_offset, sizeof(suffix) - suffix_offset,
                                      "Location: %s\n", settings.location);
         }
         if (has_timezone) {
            suffix_offset += snprintf(suffix + suffix_offset, sizeof(suffix) - suffix_offset,
                                      "Timezone: %s\n", settings.timezone);
         }
         if (has_units) {
            suffix_offset += snprintf(suffix + suffix_offset, sizeof(suffix) - suffix_offset,
                                      "Preferred units: %s\n", settings.units);
         }
      } else {
         suffix[0] = '\0';
      }

      /* Allocate: prefix + base + suffix */
      size_t suffix_len = strlen(suffix);
      char *combined = malloc(prefix_len + base_len + suffix_len + 1);
      if (!combined) {
         return strdup(base_prompt);
      }

      memcpy(combined, prefix, prefix_len);
      memcpy(combined + prefix_len, base_prompt, base_len);
      memcpy(combined + prefix_len + base_len, suffix, suffix_len + 1);

      LOG_INFO("Built REPLACE prompt for user_id=%d (%d + %zu + %zu bytes)", user_id, prefix_len,
               base_len, suffix_len);

      return combined;
   }

   /* Append mode: Add user context (persona 512 + loc 128 + tz 64 + units 16 = ~750 max) */
   char user_context[768];
   int offset = 0;

   offset += snprintf(user_context + offset, sizeof(user_context) - offset,
                      "\n\n## User Context\n");

   if (has_persona) {
      offset += snprintf(user_context + offset, sizeof(user_context) - offset,
                         "Additional persona traits: %s\n", settings.persona_description);
   }
   if (has_location) {
      offset += snprintf(user_context + offset, sizeof(user_context) - offset, "Location: %s\n",
                         settings.location);
   }
   if (has_timezone) {
      offset += snprintf(user_context + offset, sizeof(user_context) - offset, "Timezone: %s\n",
                         settings.timezone);
   }
   if (has_units) {
      offset += snprintf(user_context + offset, sizeof(user_context) - offset,
                         "Preferred units: %s\n", settings.units);
   }

   /* Allocate combined prompt */
   size_t context_len = strlen(user_context);
   char *combined = malloc(base_len + context_len + 1);
   if (!combined) {
      return strdup(base_prompt);
   }

   memcpy(combined, base_prompt, base_len);
   memcpy(combined + base_len, user_context, context_len + 1);

   LOG_INFO("Built APPEND prompt for user_id=%d (%zu + %zu bytes)", user_id, base_len, context_len);

   return combined;
}

/**
 * @brief Send JSON response with optional Set-Cookie header
 * @param wsi HTTP connection
 * @param status HTTP status code
 * @param json_body JSON string to send
 * @param cookie Cookie value to set (NULL for no cookie, empty string to clear)
 * @return 0 on success, -1 on failure
 */
static int send_auth_response(struct lws *wsi,
                              int status,
                              const char *json_body,
                              const char *cookie) {
   size_t body_len = strlen(json_body);
   unsigned char buffer[LWS_PRE + 4096];
   unsigned char *start = &buffer[LWS_PRE];
   unsigned char *p = start;
   unsigned char *end = &buffer[sizeof(buffer) - 1];

   if (lws_add_http_header_status(wsi, (unsigned int)status, &p, end))
      return -1;
   if (lws_add_http_header_by_token(wsi, WSI_TOKEN_HTTP_CONTENT_TYPE,
                                    (unsigned char *)"application/json", 16, &p, end))
      return -1;
   if (lws_add_http_header_content_length(wsi, body_len, &p, end))
      return -1;

   /* Add Set-Cookie header if provided */
   if (cookie) {
      char cookie_header[256];
      if (cookie[0] == '\0') {
         /* Clear cookie */
         snprintf(cookie_header, sizeof(cookie_header),
                  "%s=; Path=/; HttpOnly; Secure; SameSite=Strict; Max-Age=0", AUTH_COOKIE_NAME);
      } else {
         /* Set cookie */
         snprintf(cookie_header, sizeof(cookie_header),
                  "%s=%s; Path=/; HttpOnly; Secure; SameSite=Strict; Max-Age=%d", AUTH_COOKIE_NAME,
                  cookie, AUTH_COOKIE_MAX_AGE);
      }
      if (lws_add_http_header_by_name(wsi, (unsigned char *)"Set-Cookie:",
                                      (unsigned char *)cookie_header, (int)strlen(cookie_header),
                                      &p, end))
         return -1;
   }

   if (lws_finalize_http_header(wsi, &p, end))
      return -1;

   /* Write headers first */
   int n = lws_write(wsi, start, (size_t)(p - start), LWS_WRITE_HTTP_HEADERS);
   if (n < 0)
      return -1;

   /* Write body - use LWS_WRITE_HTTP_FINAL to indicate completion */
   n = lws_write(wsi, (unsigned char *)json_body, body_len, LWS_WRITE_HTTP_FINAL);
   if (n < 0)
      return -1;

   return 0;
}

/**
 * @brief Send JSON response with no-cache headers
 *
 * Used for sensitive endpoints like CSRF token generation where caching
 * would be a security risk.
 *
 * @param wsi HTTP connection
 * @param status HTTP status code
 * @param json_body JSON string to send
 * @return 0 on success, -1 on failure
 */
static int send_nocache_json_response(struct lws *wsi, int status, const char *json_body) {
   size_t body_len = strlen(json_body);
   unsigned char buffer[LWS_PRE + 4096];
   unsigned char *start = &buffer[LWS_PRE];
   unsigned char *p = start;
   unsigned char *end = &buffer[sizeof(buffer) - 1];

   if (lws_add_http_header_status(wsi, (unsigned int)status, &p, end))
      return -1;
   if (lws_add_http_header_by_token(wsi, WSI_TOKEN_HTTP_CONTENT_TYPE,
                                    (unsigned char *)"application/json", 16, &p, end))
      return -1;
   if (lws_add_http_header_content_length(wsi, body_len, &p, end))
      return -1;

   /* Add no-cache headers to prevent token caching */
   if (lws_add_http_header_by_name(wsi, (unsigned char *)"Cache-Control:",
                                   (unsigned char *)"no-store, no-cache, must-revalidate, private",
                                   44, &p, end))
      return -1;
   if (lws_add_http_header_by_name(wsi, (unsigned char *)"Pragma:", (unsigned char *)"no-cache", 8,
                                   &p, end))
      return -1;

   if (lws_finalize_http_header(wsi, &p, end))
      return -1;

   /* Write headers first */
   int n = lws_write(wsi, start, (size_t)(p - start), LWS_WRITE_HTTP_HEADERS);
   if (n < 0)
      return -1;

   /* Write body - use LWS_WRITE_HTTP_FINAL to indicate completion */
   n = lws_write(wsi, (unsigned char *)json_body, body_len, LWS_WRITE_HTTP_FINAL);
   if (n < 0)
      return -1;

   return 0;
}

/* Rate limiting uses generic rate_limiter API from core/rate_limiter.h */

/**
 * @brief Record a CSRF nonce as used (single-use enforcement)
 *
 * @param nonce 16-byte nonce from CSRF token
 */
static void csrf_record_used_nonce(const unsigned char *nonce) {
   pthread_mutex_lock(&s_csrf_used.mutex);
   memcpy(s_csrf_used.nonces[s_csrf_used.head], nonce, CSRF_USED_NONCE_SIZE);
   s_csrf_used.head = (s_csrf_used.head + 1) &
                      (CSRF_USED_NONCE_COUNT - 1); /* Bitwise AND for power-of-2 */
   pthread_mutex_unlock(&s_csrf_used.mutex);
}

/**
 * @brief Check if a CSRF nonce has already been used
 *
 * @param nonce 16-byte nonce from CSRF token
 * @return true if already used (replay attack), false if fresh
 */
static bool csrf_is_nonce_used(const unsigned char *nonce) {
   pthread_mutex_lock(&s_csrf_used.mutex);
   for (int i = 0; i < CSRF_USED_NONCE_COUNT; i++) {
      if (sodium_memcmp(s_csrf_used.nonces[i], nonce, CSRF_USED_NONCE_SIZE) == 0) {
         pthread_mutex_unlock(&s_csrf_used.mutex);
         return true;
      }
   }
   pthread_mutex_unlock(&s_csrf_used.mutex);
   return false;
}

/**
 * @brief Handle POST /api/auth/login
 * @param wsi HTTP connection
 * @param pss Session data containing POST body
 * @return -1 to close connection after response
 */
static int handle_auth_login(struct lws *wsi, struct http_session_data *pss) {
   char response[256];

   /* Get client IP early for rate limiting and logging */
   char client_ip[64] = "unknown";
   lws_get_peer_simple(wsi, client_ip, sizeof(client_ip));

   /* Normalize IP for rate limiting (IPv6 /64 prefix) */
   char normalized_ip[RATE_LIMIT_IP_SIZE];
   rate_limiter_normalize_ip(client_ip, normalized_ip, sizeof(normalized_ip));

   /* Check IP-based rate limiting - in-memory fast-path first, then database */
   if (rate_limiter_check(&s_login_rate, normalized_ip)) {
      LOG_WARNING("WebUI: Rate limited IP (in-memory): %s (normalized: %s)", client_ip,
                  normalized_ip);
      auth_db_log_event("RATE_LIMITED", NULL, client_ip, "Too many failed attempts");
      snprintf(response, sizeof(response),
               "{\"success\":false,\"error\":\"Too many attempts. Try again later.\"}");
      send_auth_response(wsi, HTTP_STATUS_TOO_MANY_REQUESTS, response, NULL);
      return -1;
   }

   /* Also check database for persistence across restarts */
   time_t window_start = time(NULL) - RATE_LIMIT_WINDOW_SEC;
   int recent_failures = auth_db_count_recent_failures(normalized_ip, window_start);
   if (recent_failures >= RATE_LIMIT_MAX_ATTEMPTS) {
      LOG_WARNING("WebUI: Rate limited IP (database): %s (normalized: %s)", client_ip,
                  normalized_ip);
      auth_db_log_event("RATE_LIMITED", NULL, client_ip, "Too many failed attempts");
      snprintf(response, sizeof(response),
               "{\"success\":false,\"error\":\"Too many attempts. Try again later.\"}");
      send_auth_response(wsi, HTTP_STATUS_TOO_MANY_REQUESTS, response, NULL);
      return -1;
   }

   /* Parse JSON body */
   struct json_object *req = json_tokener_parse(pss->post_body);
   if (!req) {
      snprintf(response, sizeof(response), "{\"success\":false,\"error\":\"Invalid JSON\"}");
      send_auth_response(wsi, HTTP_STATUS_BAD_REQUEST, response, NULL);
      return -1;
   }

   /* Extract and validate CSRF token */
   struct json_object *csrf_obj;
   if (!json_object_object_get_ex(req, "csrf_token", &csrf_obj)) {
      json_object_put(req);
      LOG_WARNING("WebUI: Login attempt without CSRF token from %s", client_ip);
      snprintf(response, sizeof(response), "{\"success\":false,\"error\":\"Missing CSRF token\"}");
      send_auth_response(wsi, HTTP_STATUS_BAD_REQUEST, response, NULL);
      return -1;
   }

   const char *csrf_token = json_object_get_string(csrf_obj);
   unsigned char csrf_nonce[AUTH_CSRF_NONCE_SIZE];
   if (!auth_verify_csrf_token_extract_nonce(csrf_token, csrf_nonce)) {
      json_object_put(req);
      LOG_WARNING("WebUI: Invalid CSRF token from %s", client_ip);
      auth_db_log_event("CSRF_FAILED", NULL, client_ip, "Invalid or expired CSRF token");
      snprintf(response, sizeof(response),
               "{\"success\":false,\"error\":\"Invalid or expired token. Please refresh.\"}");
      send_auth_response(wsi, HTTP_STATUS_FORBIDDEN, response, NULL);
      return -1;
   }

   /* Check for CSRF token replay (single-use enforcement) */
   if (csrf_is_nonce_used(csrf_nonce)) {
      json_object_put(req);
      LOG_WARNING("WebUI: CSRF token replay attempt from %s", client_ip);
      auth_db_log_event("CSRF_REPLAY", NULL, client_ip, "CSRF token reuse detected");
      snprintf(response, sizeof(response),
               "{\"success\":false,\"error\":\"Token already used. Please refresh.\"}");
      send_auth_response(wsi, HTTP_STATUS_FORBIDDEN, response, NULL);
      return -1;
   }

   /* Mark CSRF token as used (do this early, even before checking credentials) */
   csrf_record_used_nonce(csrf_nonce);

   struct json_object *username_obj, *password_obj;
   if (!json_object_object_get_ex(req, "username", &username_obj) ||
       !json_object_object_get_ex(req, "password", &password_obj)) {
      json_object_put(req);
      snprintf(response, sizeof(response),
               "{\"success\":false,\"error\":\"Missing username or password\"}");
      send_auth_response(wsi, HTTP_STATUS_BAD_REQUEST, response, NULL);
      return -1;
   }

   const char *username = json_object_get_string(username_obj);
   const char *password = json_object_get_string(password_obj);

   /* Get user from database */
   auth_user_t user;
   if (auth_db_get_user(username, &user) != AUTH_DB_SUCCESS) {
      /* Timing equalization: perform dummy password hash verification
       * to prevent timing attacks that could enumerate valid usernames */
      (void)auth_verify_password(DUMMY_PASSWORD_HASH, password);
      json_object_put(req);
      LOG_WARNING("WebUI: Login failed - user not found: %s", username);
      auth_db_log_attempt(normalized_ip, username, false);
      snprintf(response, sizeof(response), "{\"success\":false,\"error\":\"Invalid credentials\"}");
      send_auth_response(wsi, HTTP_STATUS_UNAUTHORIZED, response, NULL);
      return -1;
   }

   /* Check if account is locked */
   time_t now = time(NULL);
   if (user.lockout_until > now) {
      json_object_put(req);
      LOG_WARNING("WebUI: Login failed - account locked: %s", username);
      auth_db_log_attempt(normalized_ip, username, false);
      snprintf(response, sizeof(response),
               "{\"success\":false,\"error\":\"Account temporarily locked\"}");
      send_auth_response(wsi, HTTP_STATUS_FORBIDDEN, response, NULL);
      return -1;
   } else if (user.lockout_until > 0 && user.lockout_until <= now) {
      /* Lockout expired - reset failed attempts counter */
      auth_db_reset_failed_attempts(username);
      auth_db_set_lockout(username, 0);
      LOG_INFO("WebUI: Lockout expired, reset failed attempts: %s", username);
   }

   /* Verify password - auth_verify_password returns bool (true=success) */
   if (!auth_verify_password(user.password_hash, password)) {
      json_object_put(req);
      auth_db_increment_failed_attempts(username);
      auth_db_log_attempt(normalized_ip, username, false);

      /* Check if account should be locked after this failed attempt */
      auth_user_t updated_user;
      if (auth_db_get_user(username, &updated_user) == AUTH_DB_SUCCESS) {
         if (updated_user.failed_attempts >= AUTH_MAX_LOGIN_ATTEMPTS) {
            time_t lockout_until = time(NULL) + AUTH_LOCKOUT_DURATION_SEC;
            auth_db_set_lockout(username, lockout_until);
            auth_db_log_event("ACCOUNT_LOCKED", username, client_ip,
                              "Too many failed login attempts");
            LOG_WARNING("WebUI: Account locked due to %d failed attempts: %s",
                        updated_user.failed_attempts, username);
         }
      }

      LOG_WARNING("WebUI: Login failed - wrong password: %s", username);
      snprintf(response, sizeof(response), "{\"success\":false,\"error\":\"Invalid credentials\"}");
      send_auth_response(wsi, HTTP_STATUS_UNAUTHORIZED, response, NULL);
      return -1;
   }

   json_object_put(req);

   /* Generate session token */
   char session_token[AUTH_TOKEN_LEN];
   if (auth_generate_token(session_token) != AUTH_CRYPTO_SUCCESS) {
      LOG_ERROR("WebUI: Failed to generate session token");
      snprintf(response, sizeof(response), "{\"success\":false,\"error\":\"Server error\"}");
      send_auth_response(wsi, HTTP_STATUS_INTERNAL_SERVER_ERROR, response, NULL);
      return -1;
   }

   /* Get User-Agent header for session tracking */
   char user_agent[AUTH_USER_AGENT_MAX] = "Unknown";
   int ua_len = lws_hdr_copy(wsi, user_agent, sizeof(user_agent), WSI_TOKEN_HTTP_USER_AGENT);
   if (ua_len <= 0) {
      strncpy(user_agent, "Unknown", sizeof(user_agent));
   }

   /* Create session in database */
   if (auth_db_create_session(user.id, session_token, client_ip, user_agent) != AUTH_DB_SUCCESS) {
      LOG_ERROR("WebUI: Failed to create session for user: %s", username);
      snprintf(response, sizeof(response), "{\"success\":false,\"error\":\"Server error\"}");
      send_auth_response(wsi, HTTP_STATUS_INTERNAL_SERVER_ERROR, response, NULL);
      auth_secure_zero(session_token, sizeof(session_token));
      return -1;
   }

   /* Reset failed attempts and update last login */
   auth_db_reset_failed_attempts(username);
   rate_limiter_reset(&s_login_rate, normalized_ip); /* Clear in-memory rate limit on success */
   auth_db_update_last_login(username);
   auth_db_log_attempt(normalized_ip, username, true);
   auth_db_log_event("LOGIN_SUCCESS", username, client_ip, "WebUI login successful");

   LOG_INFO("WebUI: User logged in: %s from %s", username, client_ip);

   /* Send success response with session cookie */
   snprintf(response, sizeof(response), "{\"success\":true,\"username\":\"%s\",\"is_admin\":%s}",
            username, user.is_admin ? "true" : "false");
   send_auth_response(wsi, HTTP_STATUS_OK, response, session_token);

   /* Clear session token from stack after use */
   auth_secure_zero(session_token, sizeof(session_token));
   return -1;
}

/**
 * @brief Handle POST /api/auth/logout
 * @param wsi HTTP connection
 * @return -1 to close connection after response
 */
static int handle_auth_logout(struct lws *wsi) {
   char token[AUTH_TOKEN_LEN];
   if (extract_session_cookie(wsi, token)) {
      auth_session_t session;
      if (auth_db_get_session(token, &session) == AUTH_DB_SUCCESS) {
         char client_ip[64] = "unknown";
         lws_get_peer_simple(wsi, client_ip, sizeof(client_ip));
         auth_db_log_event("logout", session.username, client_ip, "WebUI logout");
         auth_db_delete_session(token);
         LOG_INFO("WebUI: User logged out: %s", session.username);
      }
   }

   /* Use simple HTTP status - no body needed, avoids lws_write issues.
    * JavaScript redirects regardless of response content. */
   lws_return_http_status(wsi, HTTP_STATUS_OK, NULL);
   return -1;
}

/**
 * @brief Handle GET /api/auth/status
 * @param wsi HTTP connection
 * @return -1 to close connection after response
 */
static int handle_auth_status(struct lws *wsi) {
   auth_session_t session;
   char response[256];

   if (is_request_authenticated(wsi, &session)) {
      snprintf(response, sizeof(response),
               "{\"authenticated\":true,\"username\":\"%s\",\"is_admin\":%s}", session.username,
               session.is_admin ? "true" : "false");
   } else {
      snprintf(response, sizeof(response), "{\"authenticated\":false}");
   }

   send_auth_response(wsi, HTTP_STATUS_OK, response, NULL);
   return -1;
}

/**
 * @brief Handle GET /api/auth/csrf
 * @param wsi HTTP connection
 * @return -1 to close connection after response
 *
 * Returns a CSRF token for use in login and other state-changing requests.
 * Token is HMAC-signed and valid for AUTH_CSRF_TIMEOUT_SEC seconds.
 */
static int handle_auth_csrf(struct lws *wsi) {
   /* Get client IP and normalize for rate limiting (IPv6 /64 prefix) */
   char client_ip[64] = "unknown";
   char normalized_ip[RATE_LIMIT_IP_SIZE];
   lws_get_peer_simple(wsi, client_ip, sizeof(client_ip));
   rate_limiter_normalize_ip(client_ip, normalized_ip, sizeof(normalized_ip));

   /* Check CSRF endpoint rate limiting (prevent DoS via token generation) */
   if (rate_limiter_check(&s_csrf_rate, normalized_ip)) {
      LOG_WARNING("WebUI: CSRF rate limited: %s", normalized_ip);
      send_nocache_json_response(wsi, HTTP_STATUS_TOO_MANY_REQUESTS,
                                 "{\"error\":\"Too many requests\"}");
      return -1;
   }

   char csrf_token[AUTH_CSRF_TOKEN_LEN];

   if (auth_generate_csrf_token(csrf_token) != AUTH_CRYPTO_SUCCESS) {
      LOG_ERROR("WebUI: Failed to generate CSRF token");
      send_nocache_json_response(wsi, HTTP_STATUS_INTERNAL_SERVER_ERROR,
                                 "{\"error\":\"Failed to generate token\"}");
      return -1;
   }

   char response[256];
   snprintf(response, sizeof(response), "{\"csrf_token\":\"%s\"}", csrf_token);

   /* Clear token from stack */
   auth_secure_zero(csrf_token, sizeof(csrf_token));

   /* Use no-cache response to prevent browser/proxy caching */
   send_nocache_json_response(wsi, HTTP_STATUS_OK, response);
   return -1;
}

#endif /* ENABLE_AUTH */

/* =============================================================================
 * HTTP Protocol Callbacks
 * ============================================================================= */

static int callback_http(struct lws *wsi,
                         enum lws_callback_reasons reason,
                         void *user,
                         void *in,
                         size_t len) {
   struct http_session_data *pss = (struct http_session_data *)user;

   switch (reason) {
      case LWS_CALLBACK_FILTER_NETWORK_CONNECTION:
      case LWS_CALLBACK_FILTER_HTTP_CONNECTION:
         /* Allow all connections */
         return 0;

      case LWS_CALLBACK_HTTP: {
         /* Serve static files - allocate buffers only when needed */
         char path[512];
         char filepath[768];
         const char *mime_type;
         int n;

         if (len < 1) {
            lws_return_http_status(wsi, HTTP_STATUS_BAD_REQUEST, NULL);
            return -1;
         }

         /* Get requested path */
         strncpy(path, (const char *)in, sizeof(path) - 1);
         path[sizeof(path) - 1] = '\0';

         /* Initialize session data */
         if (pss) {
            strncpy(pss->path, path, sizeof(pss->path) - 1);
            pss->path[sizeof(pss->path) - 1] = '\0';
            pss->post_body_len = 0;
            pss->post_body[0] = '\0';
            pss->is_post = (lws_hdr_total_length(wsi, WSI_TOKEN_POST_URI) > 0);
         }

#ifdef ENABLE_AUTH
         /* Auth API endpoints - no auth required for these */
         if (strcmp(path, "/api/auth/status") == 0) {
            return handle_auth_status(wsi);
         }

         if (strcmp(path, "/api/auth/csrf") == 0) {
            return handle_auth_csrf(wsi);
         }

         if (strcmp(path, "/api/auth/logout") == 0) {
            return handle_auth_logout(wsi);
         }

         /* POST /api/auth/login - defer to body completion */
         if (strcmp(path, "/api/auth/login") == 0 && pss && pss->is_post) {
            /* Return 0 to allow body callbacks */
            return 0;
         }

         /* Public paths that don't require auth */
         bool is_public_path = (strcmp(path, "/login.html") == 0 || strcmp(path, "/health") == 0 ||
                                strncmp(path, "/css/", 5) == 0 ||
                                strncmp(path, "/fonts/", 7) == 0 ||
                                strcmp(path, "/favicon.svg") == 0);

         /* Check authentication for protected paths */
         if (!is_public_path && !is_request_authenticated(wsi, NULL)) {
            /* Redirect to login page */
            unsigned char buffer[LWS_PRE + 256];
            unsigned char *start = &buffer[LWS_PRE];
            unsigned char *p = start;
            unsigned char *end = &buffer[sizeof(buffer) - 1];

            if (lws_add_http_header_status(wsi, HTTP_STATUS_FOUND, &p, end))
               return -1;
            if (lws_add_http_header_by_name(wsi, (unsigned char *)"Location:",
                                            (unsigned char *)"/login.html", 11, &p, end))
               return -1;
            if (lws_add_http_header_content_length(wsi, 0, &p, end))
               return -1;
            if (lws_finalize_http_header(wsi, &p, end))
               return -1;

            n = lws_write(wsi, start, (size_t)(p - start), LWS_WRITE_HTTP_HEADERS);
            if (n < 0)
               return -1;

            return -1; /* Close connection */
         }
#endif /* ENABLE_AUTH */

         /* SmartThings OAuth callback - extract code and state from URL */
         if (strncmp(path, "/smartthings/callback", 21) == 0) {
            /* Generate inline HTML that extracts params and posts to opener */
            static const char oauth_callback_html[] =
                "<!DOCTYPE html><html><head><title>SmartThings Auth</title></head>"
                "<body><script>"
                "const params = new URLSearchParams(window.location.search);"
                "const code = params.get('code');"
                "const state = params.get('state');"
                "const error = params.get('error');"
                "if (window.opener) {"
                "  window.opener.postMessage({"
                "    type: 'smartthings_oauth_callback',"
                "    code: code,"
                "    state: state,"
                "    error: error"
                "  }, window.location.origin);"
                "  setTimeout(function() { window.close(); }, 500);"
                "} else {"
                "  document.body.innerHTML = '<p>Authorization ' + "
                "    (code ? 'successful' : 'failed') + '. You can close this window.</p>';"
                "}"
                "</script><p>Processing authorization...</p></body></html>";

            unsigned char buffer[LWS_PRE + sizeof(oauth_callback_html)];
            unsigned char *start = &buffer[LWS_PRE];
            unsigned char *p = start;
            unsigned char *end = &buffer[sizeof(buffer) - 1];

            if (lws_add_http_header_status(wsi, HTTP_STATUS_OK, &p, end))
               return -1;
            if (lws_add_http_header_by_token(wsi, WSI_TOKEN_HTTP_CONTENT_TYPE,
                                             (unsigned char *)"text/html", 9, &p, end))
               return -1;
            if (lws_add_http_header_content_length(wsi, sizeof(oauth_callback_html) - 1, &p, end))
               return -1;
            if (lws_finalize_http_header(wsi, &p, end))
               return -1;

            n = lws_write(wsi, start, p - start, LWS_WRITE_HTTP_HEADERS);
            if (n < 0)
               return -1;

            n = lws_write(wsi, (unsigned char *)oauth_callback_html,
                          sizeof(oauth_callback_html) - 1, LWS_WRITE_HTTP);
            if (n < 0)
               return -1;

            return -1; /* Close connection after response */
         }

         /* Health check endpoint - returns JSON status */
         if (strcmp(path, "/health") == 0) {
            dawn_metrics_t snapshot;
            metrics_get_snapshot(&snapshot);

            /* Build JSON response */
            char json_body[512];
            snprintf(json_body, sizeof(json_body),
                     "{\"status\":\"ok\",\"version\":\"%s\",\"git_sha\":\"%s\","
                     "\"uptime_seconds\":%ld,\"state\":\"%s\",\"queries\":%u,"
                     "\"active_sessions\":%d}",
                     VERSION_NUMBER, GIT_SHA, (long)metrics_get_uptime(),
                     dawn_state_name(snapshot.current_state), snapshot.queries_total,
                     s_client_count);

            size_t body_len = strlen(json_body);
            unsigned char buffer[LWS_PRE + 512];
            unsigned char *start = &buffer[LWS_PRE];
            unsigned char *p = start;
            unsigned char *end = &buffer[sizeof(buffer) - 1];

            if (lws_add_http_header_status(wsi, HTTP_STATUS_OK, &p, end))
               return -1;
            if (lws_add_http_header_by_token(wsi, WSI_TOKEN_HTTP_CONTENT_TYPE,
                                             (unsigned char *)"application/json", 16, &p, end))
               return -1;
            if (lws_add_http_header_content_length(wsi, body_len, &p, end))
               return -1;
            if (lws_finalize_http_header(wsi, &p, end))
               return -1;

            n = lws_write(wsi, start, p - start, LWS_WRITE_HTTP_HEADERS);
            if (n < 0)
               return -1;

            n = lws_write(wsi, (unsigned char *)json_body, body_len, LWS_WRITE_HTTP);
            if (n < 0)
               return -1;

            return -1; /* Close connection after response */
         }

         /* Default to index.html for root */
         if (strcmp(path, "/") == 0) {
            strncpy(path, "/index.html", sizeof(path) - 1);
         }

         /* Prevent directory traversal - check for patterns including URL-encoded */
         if (contains_path_traversal(path)) {
            LOG_WARNING("WebUI: Directory traversal attempt blocked: %s", path);
            lws_return_http_status(wsi, HTTP_STATUS_FORBIDDEN, NULL);
            return -1;
         }

         /* Build full filesystem path */
         snprintf(filepath, sizeof(filepath), "%s%s", s_www_path, path);

         /* Second layer: verify resolved path is within www directory */
         if (!is_path_within_www(filepath, s_www_path)) {
            LOG_WARNING("WebUI: Path escape attempt blocked: %s", filepath);
            lws_return_http_status(wsi, HTTP_STATUS_FORBIDDEN, NULL);
            return -1;
         }

         /* Get MIME type */
         mime_type = get_mime_type(filepath);

         /* Serve the file (no extra headers - CSP set via meta tag) */
         n = lws_serve_http_file(wsi, filepath, mime_type, NULL, 0);
         if (n < 0) {
            /* File not found or error */
            LOG_WARNING("WebUI: File not found: %s", filepath);
            lws_return_http_status(wsi, HTTP_STATUS_NOT_FOUND, NULL);
            return -1;
         }
         if (n > 0) {
            /* File is being sent, connection will close after */
            return 0;
         }
         break;
      }

      case LWS_CALLBACK_HTTP_FILE_COMPLETION:
         /* File transfer complete */
         return -1; /* Close connection */

#ifdef ENABLE_AUTH
      case LWS_CALLBACK_HTTP_BODY: {
         /* Accumulate POST body */
         if (!pss)
            return -1;

         size_t remaining = HTTP_MAX_POST_BODY - pss->post_body_len - 1;
         size_t to_copy = (len < remaining) ? len : remaining;

         if (to_copy > 0) {
            memcpy(pss->post_body + pss->post_body_len, in, to_copy);
            pss->post_body_len += to_copy;
            pss->post_body[pss->post_body_len] = '\0';
         }
         return 0;
      }

      case LWS_CALLBACK_HTTP_BODY_COMPLETION: {
         /* POST body complete - process it */
         if (!pss)
            return -1;

         /* Handle login endpoint */
         if (strcmp(pss->path, "/api/auth/login") == 0) {
            return handle_auth_login(wsi, pss);
         }

         /* Unknown POST endpoint */
         lws_return_http_status(wsi, HTTP_STATUS_NOT_FOUND, NULL);
         return -1;
      }
#endif /* ENABLE_AUTH */

      default:
         break;
   }

   return 0;
}

/* =============================================================================
 * JSON Message Handling
 * ============================================================================= */

static void handle_text_message(ws_connection_t *conn, const char *json_str, size_t len);
static void handle_cancel_message(ws_connection_t *conn);
static void handle_get_config(ws_connection_t *conn);
static void handle_set_config(ws_connection_t *conn, struct json_object *payload);
static void handle_set_secrets(ws_connection_t *conn, struct json_object *payload);
static void handle_get_audio_devices(ws_connection_t *conn, struct json_object *payload);
static void handle_list_models(ws_connection_t *conn);
static void send_json_response(struct lws *wsi, json_object *response);
static void handle_list_interfaces(ws_connection_t *conn);
static void handle_get_tools_config(ws_connection_t *conn);
static void handle_set_tools_config(ws_connection_t *conn, struct json_object *payload);
static void handle_get_metrics(ws_connection_t *conn);
#ifdef ENABLE_WEBUI_AUDIO
static void handle_binary_message(ws_connection_t *conn, const uint8_t *data, size_t len);
#endif

/* Settings that require restart when changed */
static const char *s_restart_required_fields[] = { "audio.backend",
                                                   "audio.capture_device",
                                                   "audio.playback_device",
                                                   "asr.model",
                                                   "asr.models_path",
                                                   "tts.models_path",
                                                   "tts.voice_model",
                                                   "network.enabled",
                                                   "network.host",
                                                   "network.port",
                                                   "network.workers",
                                                   "webui.port",
                                                   "webui.max_clients",
                                                   "webui.workers",
                                                   "webui.https",
                                                   "webui.ssl_cert_path",
                                                   "webui.ssl_key_path",
                                                   "webui.bind_address",
                                                   NULL };

static void handle_get_config(ws_connection_t *conn) {
   /* Build response with config, secrets status, and metadata */
   json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("get_config_response"));

   json_object *payload = json_object_new_object();

   /* Check if user is admin (re-validate from DB to prevent stale cache) */
   bool is_admin = false;
   if (conn->authenticated) {
      auth_session_t session;
      if (auth_db_get_session(conn->auth_session_token, &session) == AUTH_DB_SUCCESS) {
         is_admin = session.is_admin;
      }
   }

   /* Add config path (redacted for non-admins) */
   const char *config_path = config_get_loaded_path();
   json_object_object_add(payload, "config_path",
                          json_object_new_string(is_admin ? config_path : "(configured)"));

   /* Add secrets path (redacted for non-admins) */
   const char *secrets_path = config_get_secrets_path();
   json_object_object_add(payload, "secrets_path",
                          json_object_new_string(is_admin ? secrets_path : "(configured)"));

   /* Add full config as JSON */
   json_object *config_json = config_to_json(config_get());
   if (config_json) {
      json_object_object_add(payload, "config", config_json);
   }

   /* Add secrets status (only is_set flags, never actual values) */
   json_object *secrets_status = secrets_to_json_status(config_get_secrets());
   if (secrets_status) {
      json_object_object_add(payload, "secrets", secrets_status);
   }

   /* Add list of fields that require restart */
   json_object *restart_fields = json_object_new_array();
   for (int i = 0; s_restart_required_fields[i]; i++) {
      json_object_array_add(restart_fields, json_object_new_string(s_restart_required_fields[i]));
   }
   json_object_object_add(payload, "requires_restart", restart_fields);

   /* Add session LLM status (resolved config for this session) */
   json_object *llm_runtime = json_object_new_object();

   /* Get session's resolved LLM config (or global default if no session yet) */
   session_llm_config_t session_config;
   llm_resolved_config_t resolved = { 0 };
   if (conn->session) {
      session_get_llm_config(conn->session, &session_config);
   } else {
      /* No session yet - use global defaults */
      llm_get_default_config(&session_config);
   }
   llm_resolve_config(&session_config, &resolved);

   json_object_object_add(llm_runtime, "type",
                          json_object_new_string(resolved.type == LLM_LOCAL ? "local" : "cloud"));

   const char *provider_name = resolved.cloud_provider == CLOUD_PROVIDER_OPENAI   ? "OpenAI"
                               : resolved.cloud_provider == CLOUD_PROVIDER_CLAUDE ? "Claude"
                                                                                  : "None";
   json_object_object_add(llm_runtime, "provider", json_object_new_string(provider_name));
   json_object_object_add(llm_runtime, "model",
                          json_object_new_string(resolved.model ? resolved.model : ""));
   json_object_object_add(llm_runtime, "openai_available",
                          json_object_new_boolean(llm_has_openai_key()));
   json_object_object_add(llm_runtime, "claude_available",
                          json_object_new_boolean(llm_has_claude_key()));
   json_object_object_add(payload, "llm_runtime", llm_runtime);

   /* Add auth state for frontend UI visibility control */
   json_object_object_add(payload, "authenticated", json_object_new_boolean(conn->authenticated));
   json_object_object_add(payload, "is_admin", json_object_new_boolean(is_admin));
   if (conn->authenticated) {
      json_object_object_add(payload, "username", json_object_new_string(conn->username));
   }

   json_object_object_add(response, "payload", payload);

   /* Send response */
   const char *json_str = json_object_to_json_string(response);
   size_t json_len = strlen(json_str);
   unsigned char *buf = malloc(LWS_PRE + json_len);
   if (buf) {
      memcpy(buf + LWS_PRE, json_str, json_len);
      lws_write(conn->wsi, buf + LWS_PRE, json_len, LWS_WRITE_TEXT);
      free(buf);
   }

   json_object_put(response);
   LOG_INFO("WebUI: Sent configuration to client");
}

/* Helper to safely copy string from JSON to config field */
#define JSON_TO_CONFIG_STR(obj, key, dest)                \
   do {                                                   \
      struct json_object *_val;                           \
      if (json_object_object_get_ex(obj, key, &_val)) {   \
         const char *_str = json_object_get_string(_val); \
         if (_str) {                                      \
            strncpy(dest, _str, sizeof(dest) - 1);        \
            dest[sizeof(dest) - 1] = '\0';                \
         }                                                \
      }                                                   \
   } while (0)

#define JSON_TO_CONFIG_INT(obj, key, dest)              \
   do {                                                 \
      struct json_object *_val;                         \
      if (json_object_object_get_ex(obj, key, &_val)) { \
         dest = json_object_get_int(_val);              \
      }                                                 \
   } while (0)

#define JSON_TO_CONFIG_BOOL(obj, key, dest)             \
   do {                                                 \
      struct json_object *_val;                         \
      if (json_object_object_get_ex(obj, key, &_val)) { \
         dest = json_object_get_boolean(_val);          \
      }                                                 \
   } while (0)

#define JSON_TO_CONFIG_DOUBLE(obj, key, dest)           \
   do {                                                 \
      struct json_object *_val;                         \
      if (json_object_object_get_ex(obj, key, &_val)) { \
         dest = (float)json_object_get_double(_val);    \
      }                                                 \
   } while (0)

#define JSON_TO_CONFIG_SIZE_T(obj, key, dest)           \
   do {                                                 \
      struct json_object *_val;                         \
      if (json_object_object_get_ex(obj, key, &_val)) { \
         dest = (size_t)json_object_get_int64(_val);    \
      }                                                 \
   } while (0)

static void apply_config_from_json(dawn_config_t *config, struct json_object *payload) {
   struct json_object *section;

   /* [general] */
   if (json_object_object_get_ex(payload, "general", &section)) {
      JSON_TO_CONFIG_STR(section, "ai_name", config->general.ai_name);
      JSON_TO_CONFIG_STR(section, "log_file", config->general.log_file);
   }

   /* [persona] */
   if (json_object_object_get_ex(payload, "persona", &section)) {
      JSON_TO_CONFIG_STR(section, "description", config->persona.description);
   }

   /* [localization] */
   if (json_object_object_get_ex(payload, "localization", &section)) {
      JSON_TO_CONFIG_STR(section, "location", config->localization.location);
      JSON_TO_CONFIG_STR(section, "timezone", config->localization.timezone);
      JSON_TO_CONFIG_STR(section, "units", config->localization.units);
   }

   /* [audio] */
   if (json_object_object_get_ex(payload, "audio", &section)) {
      JSON_TO_CONFIG_STR(section, "backend", config->audio.backend);
      JSON_TO_CONFIG_STR(section, "capture_device", config->audio.capture_device);
      JSON_TO_CONFIG_STR(section, "playback_device", config->audio.playback_device);
      JSON_TO_CONFIG_INT(section, "output_rate", config->audio.output_rate);
      JSON_TO_CONFIG_INT(section, "output_channels", config->audio.output_channels);

      struct json_object *bargein;
      if (json_object_object_get_ex(section, "bargein", &bargein)) {
         JSON_TO_CONFIG_BOOL(bargein, "enabled", config->audio.bargein.enabled);
         JSON_TO_CONFIG_INT(bargein, "cooldown_ms", config->audio.bargein.cooldown_ms);
         JSON_TO_CONFIG_INT(bargein, "startup_cooldown_ms",
                            config->audio.bargein.startup_cooldown_ms);
      }
   }

   /* [vad] */
   if (json_object_object_get_ex(payload, "vad", &section)) {
      JSON_TO_CONFIG_DOUBLE(section, "speech_threshold", config->vad.speech_threshold);
      JSON_TO_CONFIG_DOUBLE(section, "speech_threshold_tts", config->vad.speech_threshold_tts);
      JSON_TO_CONFIG_DOUBLE(section, "silence_threshold", config->vad.silence_threshold);
      JSON_TO_CONFIG_DOUBLE(section, "end_of_speech_duration", config->vad.end_of_speech_duration);
      JSON_TO_CONFIG_DOUBLE(section, "max_recording_duration", config->vad.max_recording_duration);
      JSON_TO_CONFIG_INT(section, "preroll_ms", config->vad.preroll_ms);

      struct json_object *chunking;
      if (json_object_object_get_ex(section, "chunking", &chunking)) {
         JSON_TO_CONFIG_BOOL(chunking, "enabled", config->vad.chunking.enabled);
         JSON_TO_CONFIG_DOUBLE(chunking, "pause_duration", config->vad.chunking.pause_duration);
         JSON_TO_CONFIG_DOUBLE(chunking, "min_duration", config->vad.chunking.min_duration);
         JSON_TO_CONFIG_DOUBLE(chunking, "max_duration", config->vad.chunking.max_duration);
      }
   }

   /* [asr] */
   if (json_object_object_get_ex(payload, "asr", &section)) {
      JSON_TO_CONFIG_STR(section, "model", config->asr.model);
      JSON_TO_CONFIG_STR(section, "models_path", config->asr.models_path);
   }

   /* [tts] */
   if (json_object_object_get_ex(payload, "tts", &section)) {
      JSON_TO_CONFIG_STR(section, "models_path", config->tts.models_path);
      JSON_TO_CONFIG_STR(section, "voice_model", config->tts.voice_model);
      JSON_TO_CONFIG_DOUBLE(section, "length_scale", config->tts.length_scale);
   }

   /* [commands] */
   if (json_object_object_get_ex(payload, "commands", &section)) {
      JSON_TO_CONFIG_STR(section, "processing_mode", config->commands.processing_mode);
   }

   /* [llm] */
   if (json_object_object_get_ex(payload, "llm", &section)) {
      JSON_TO_CONFIG_STR(section, "type", config->llm.type);
      JSON_TO_CONFIG_INT(section, "max_tokens", config->llm.max_tokens);

      struct json_object *cloud;
      if (json_object_object_get_ex(section, "cloud", &cloud)) {
         JSON_TO_CONFIG_STR(cloud, "provider", config->llm.cloud.provider);
         JSON_TO_CONFIG_STR(cloud, "openai_model", config->llm.cloud.openai_model);
         JSON_TO_CONFIG_STR(cloud, "claude_model", config->llm.cloud.claude_model);
         JSON_TO_CONFIG_STR(cloud, "endpoint", config->llm.cloud.endpoint);
         JSON_TO_CONFIG_BOOL(cloud, "vision_enabled", config->llm.cloud.vision_enabled);
      }

      struct json_object *local;
      if (json_object_object_get_ex(section, "local", &local)) {
         JSON_TO_CONFIG_STR(local, "endpoint", config->llm.local.endpoint);
         JSON_TO_CONFIG_STR(local, "model", config->llm.local.model);
         JSON_TO_CONFIG_BOOL(local, "vision_enabled", config->llm.local.vision_enabled);
      }

      struct json_object *tools;
      if (json_object_object_get_ex(section, "tools", &tools)) {
         JSON_TO_CONFIG_BOOL(tools, "native_enabled", config->llm.tools.native_enabled);
      }

      /* Context management settings */
      JSON_TO_CONFIG_DOUBLE(section, "summarize_threshold", config->llm.summarize_threshold);
      JSON_TO_CONFIG_BOOL(section, "conversation_logging", config->llm.conversation_logging);
   }

   /* [search] */
   if (json_object_object_get_ex(payload, "search", &section)) {
      JSON_TO_CONFIG_STR(section, "engine", config->search.engine);
      JSON_TO_CONFIG_STR(section, "endpoint", config->search.endpoint);

      struct json_object *summarizer;
      if (json_object_object_get_ex(section, "summarizer", &summarizer)) {
         JSON_TO_CONFIG_STR(summarizer, "backend", config->search.summarizer.backend);
         JSON_TO_CONFIG_SIZE_T(summarizer, "threshold_bytes",
                               config->search.summarizer.threshold_bytes);
         JSON_TO_CONFIG_SIZE_T(summarizer, "target_words", config->search.summarizer.target_words);
      }
   }

   /* [url_fetcher] */
   if (json_object_object_get_ex(payload, "url_fetcher", &section)) {
      struct json_object *flaresolverr;
      if (json_object_object_get_ex(section, "flaresolverr", &flaresolverr)) {
         JSON_TO_CONFIG_BOOL(flaresolverr, "enabled", config->url_fetcher.flaresolverr.enabled);
         JSON_TO_CONFIG_STR(flaresolverr, "endpoint", config->url_fetcher.flaresolverr.endpoint);
         JSON_TO_CONFIG_INT(flaresolverr, "timeout_sec",
                            config->url_fetcher.flaresolverr.timeout_sec);
         JSON_TO_CONFIG_SIZE_T(flaresolverr, "max_response_bytes",
                               config->url_fetcher.flaresolverr.max_response_bytes);
      }
   }

   /* [mqtt] */
   if (json_object_object_get_ex(payload, "mqtt", &section)) {
      JSON_TO_CONFIG_BOOL(section, "enabled", config->mqtt.enabled);
      JSON_TO_CONFIG_STR(section, "broker", config->mqtt.broker);
      JSON_TO_CONFIG_INT(section, "port", config->mqtt.port);
   }

   /* [network] */
   if (json_object_object_get_ex(payload, "network", &section)) {
      JSON_TO_CONFIG_BOOL(section, "enabled", config->network.enabled);
      JSON_TO_CONFIG_STR(section, "host", config->network.host);
      JSON_TO_CONFIG_INT(section, "port", config->network.port);
      JSON_TO_CONFIG_INT(section, "workers", config->network.workers);
      JSON_TO_CONFIG_INT(section, "socket_timeout_sec", config->network.socket_timeout_sec);
      JSON_TO_CONFIG_INT(section, "session_timeout_sec", config->network.session_timeout_sec);
      JSON_TO_CONFIG_INT(section, "llm_timeout_ms", config->network.llm_timeout_ms);
   }

   /* [tui] */
   if (json_object_object_get_ex(payload, "tui", &section)) {
      JSON_TO_CONFIG_BOOL(section, "enabled", config->tui.enabled);
   }

   /* [webui] */
   if (json_object_object_get_ex(payload, "webui", &section)) {
      JSON_TO_CONFIG_BOOL(section, "enabled", config->webui.enabled);
      JSON_TO_CONFIG_INT(section, "port", config->webui.port);
      JSON_TO_CONFIG_INT(section, "max_clients", config->webui.max_clients);
      JSON_TO_CONFIG_INT(section, "audio_chunk_ms", config->webui.audio_chunk_ms);
      JSON_TO_CONFIG_INT(section, "workers", config->webui.workers);
      JSON_TO_CONFIG_STR(section, "www_path", config->webui.www_path);
      JSON_TO_CONFIG_STR(section, "bind_address", config->webui.bind_address);
      JSON_TO_CONFIG_BOOL(section, "https", config->webui.https);
      JSON_TO_CONFIG_STR(section, "ssl_cert_path", config->webui.ssl_cert_path);
      JSON_TO_CONFIG_STR(section, "ssl_key_path", config->webui.ssl_key_path);
   }

   /* [shutdown] */
   if (json_object_object_get_ex(payload, "shutdown", &section)) {
      JSON_TO_CONFIG_BOOL(section, "enabled", config->shutdown.enabled);
      JSON_TO_CONFIG_STR(section, "passphrase", config->shutdown.passphrase);
   }

   /* [debug] */
   if (json_object_object_get_ex(payload, "debug", &section)) {
      JSON_TO_CONFIG_BOOL(section, "mic_record", config->debug.mic_record);
      JSON_TO_CONFIG_BOOL(section, "asr_record", config->debug.asr_record);
      JSON_TO_CONFIG_BOOL(section, "aec_record", config->debug.aec_record);
      JSON_TO_CONFIG_STR(section, "record_path", config->debug.record_path);
   }

   /* [paths] */
   if (json_object_object_get_ex(payload, "paths", &section)) {
      JSON_TO_CONFIG_STR(section, "music_dir", config->paths.music_dir);
      JSON_TO_CONFIG_STR(section, "commands_config", config->paths.commands_config);
   }
}

static void handle_set_config(ws_connection_t *conn, struct json_object *payload) {
   /* Admin-only operation */
   if (!conn_require_admin(conn)) {
      return;
   }

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("set_config_response"));
   json_object *resp_payload = json_object_new_object();

   /* Get the config file path */
   const char *config_path = config_get_loaded_path();
   if (!config_path || strcmp(config_path, "(none - using defaults)") == 0) {
      /* No config file loaded - use default path */
      config_path = "./dawn.toml";
   }

   /* Create backup before modifying */
   if (config_backup_file(config_path) != 0) {
      LOG_WARNING("WebUI: Failed to create config backup");
      /* Continue anyway - backup is optional */
   }

   /* Apply changes to global config with mutex protection.
    * The write lock ensures no other threads are reading config during modification. */
   pthread_rwlock_wrlock(&s_config_rwlock);
   dawn_config_t *mutable_config = (dawn_config_t *)config_get();
   apply_config_from_json(mutable_config, payload);
   pthread_rwlock_unlock(&s_config_rwlock);

   /* Write to file (outside lock - file I/O shouldn't block config reads) */
   int result = config_write_toml(mutable_config, config_path);

   if (result == 0) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
      json_object_object_add(resp_payload, "message",
                             json_object_new_string("Configuration saved successfully"));
      LOG_INFO("WebUI: Configuration saved to %s", config_path);

      /* Apply runtime changes for LLM type if it was updated */
      struct json_object *llm_section = NULL;
      struct json_object *llm_type_obj = NULL;
      if (json_object_object_get_ex(payload, "llm", &llm_section) &&
          json_object_object_get_ex(llm_section, "type", &llm_type_obj)) {
         const char *new_type = json_object_get_string(llm_type_obj);
         if (new_type) {
            if (strcmp(new_type, "cloud") == 0) {
               int rc = llm_set_type(LLM_CLOUD);
               if (rc != 0) {
                  /* Update response to indicate partial success */
                  json_object_object_add(resp_payload, "warning",
                                         json_object_new_string(
                                             "Config saved but failed to switch to cloud LLM - "
                                             "API key not configured"));
               }
            } else if (strcmp(new_type, "local") == 0) {
               llm_set_type(LLM_LOCAL);
            }
         }
      }

      /* Apply runtime changes for cloud provider if it was updated */
      struct json_object *cloud_section = NULL;
      struct json_object *provider_obj = NULL;
      if (json_object_object_get_ex(payload, "llm", &llm_section) &&
          json_object_object_get_ex(llm_section, "cloud", &cloud_section) &&
          json_object_object_get_ex(cloud_section, "provider", &provider_obj)) {
         const char *new_provider = json_object_get_string(provider_obj);
         if (new_provider) {
            int rc = 0;
            if (strcmp(new_provider, "openai") == 0) {
               rc = llm_set_cloud_provider(CLOUD_PROVIDER_OPENAI);
            } else if (strcmp(new_provider, "claude") == 0) {
               rc = llm_set_cloud_provider(CLOUD_PROVIDER_CLAUDE);
            }
            if (rc != 0) {
               json_object_object_add(resp_payload, "warning",
                                      json_object_new_string(
                                          "Config saved but failed to switch cloud provider - "
                                          "API key not configured"));
            }
         }
      }
   } else {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Failed to write configuration file"));
      LOG_ERROR("WebUI: Failed to save configuration");
   }

   json_object_object_add(response, "payload", resp_payload);

   send_json_response(conn->wsi, response);
   json_object_put(response);
}

static void handle_set_secrets(ws_connection_t *conn, struct json_object *payload) {
   /* Admin-only operation */
   if (!conn_require_admin(conn)) {
      return;
   }

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("set_secrets_response"));
   json_object *resp_payload = json_object_new_object();

   /* Get secrets file path */
   const char *secrets_path = config_get_secrets_path();
   if (!secrets_path || strcmp(secrets_path, "(none)") == 0) {
      /* No secrets file loaded - use default path */
      secrets_path = "./secrets.toml";
   }

   /* Create backup before modifying */
   config_backup_file(secrets_path);

   /* Get mutable secrets config */
   secrets_config_t *mutable_secrets = (secrets_config_t *)config_get_secrets();

   /* Apply changes from payload - only update fields that are provided */
   struct json_object *val;
   if (json_object_object_get_ex(payload, "openai_api_key", &val)) {
      const char *str = json_object_get_string(val);
      if (str) {
         strncpy(mutable_secrets->openai_api_key, str, sizeof(mutable_secrets->openai_api_key) - 1);
         mutable_secrets->openai_api_key[sizeof(mutable_secrets->openai_api_key) - 1] = '\0';
      }
   }
   if (json_object_object_get_ex(payload, "claude_api_key", &val)) {
      const char *str = json_object_get_string(val);
      if (str) {
         strncpy(mutable_secrets->claude_api_key, str, sizeof(mutable_secrets->claude_api_key) - 1);
         mutable_secrets->claude_api_key[sizeof(mutable_secrets->claude_api_key) - 1] = '\0';
      }
   }
   if (json_object_object_get_ex(payload, "mqtt_username", &val)) {
      const char *str = json_object_get_string(val);
      if (str) {
         strncpy(mutable_secrets->mqtt_username, str, sizeof(mutable_secrets->mqtt_username) - 1);
         mutable_secrets->mqtt_username[sizeof(mutable_secrets->mqtt_username) - 1] = '\0';
      }
   }
   if (json_object_object_get_ex(payload, "mqtt_password", &val)) {
      const char *str = json_object_get_string(val);
      if (str) {
         strncpy(mutable_secrets->mqtt_password, str, sizeof(mutable_secrets->mqtt_password) - 1);
         mutable_secrets->mqtt_password[sizeof(mutable_secrets->mqtt_password) - 1] = '\0';
      }
   }

   /* Write to file */
   int result = secrets_write_toml(mutable_secrets, secrets_path);

   if (result == 0) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
      json_object_object_add(resp_payload, "message",
                             json_object_new_string("Secrets saved successfully"));

      /* Also update the secrets status */
      json_object *secrets_status = secrets_to_json_status(mutable_secrets);
      if (secrets_status) {
         json_object_object_add(resp_payload, "secrets", secrets_status);
      }

      /* Refresh LLM providers to pick up new API keys immediately */
      llm_refresh_providers();

      LOG_INFO("WebUI: Secrets saved to %s", secrets_path);
   } else {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Failed to write secrets file"));
      LOG_ERROR("WebUI: Failed to save secrets");
   }

   json_object_object_add(response, "payload", resp_payload);

   send_json_response(conn->wsi, response);
   json_object_put(response);
}

/**
 * @brief Send a JSON response efficiently using stack buffer when possible
 *
 * Uses stack allocation for small responses (<2KB) to avoid heap fragmentation.
 * Falls back to heap allocation for larger responses.
 */
#define MAX_STACK_RESPONSE 2048

static void send_json_response(struct lws *wsi, json_object *response) {
   const char *json_str = json_object_to_json_string(response);
   size_t json_len = strlen(json_str);

   /* Log large responses that may cause HTTP/2 issues */
   if (json_len > 10000) {
      json_object *type_obj;
      const char *type = "unknown";
      if (json_object_object_get_ex(response, "type", &type_obj)) {
         type = json_object_get_string(type_obj);
      }
      LOG_WARNING("WebUI: Large response: type=%s, size=%zu bytes", type, json_len);
   }

   if (json_len < MAX_STACK_RESPONSE - LWS_PRE) {
      /* Use stack buffer for small responses */
      unsigned char buf[LWS_PRE + MAX_STACK_RESPONSE];
      memcpy(buf + LWS_PRE, json_str, json_len);
      lws_write(wsi, buf + LWS_PRE, json_len, LWS_WRITE_TEXT);
   } else {
      /* Fall back to heap for large responses */
      unsigned char *buf = malloc(LWS_PRE + json_len);
      if (buf) {
         memcpy(buf + LWS_PRE, json_str, json_len);
         lws_write(wsi, buf + LWS_PRE, json_len, LWS_WRITE_TEXT);
         free(buf);
      } else {
         LOG_ERROR("WebUI: Failed to allocate buffer for JSON response (%zu bytes)", json_len);
      }
   }
}

/**
 * @brief Whitelisted shell commands for audio device enumeration
 *
 * SECURITY: Only these exact commands can be executed via run_whitelisted_command().
 * This prevents command injection even if a caller mistakenly passes user input.
 */
static const char *const ALLOWED_COMMANDS[] = {
   "arecord -L 2>/dev/null", "aplay -L 2>/dev/null", "pactl list sources short 2>/dev/null",
   "pactl list sinks short 2>/dev/null", NULL /* Sentinel */
};

/**
 * @brief Check if a command is in the whitelist
 */
static bool is_command_whitelisted(const char *cmd) {
   for (int i = 0; ALLOWED_COMMANDS[i] != NULL; i++) {
      if (strcmp(cmd, ALLOWED_COMMANDS[i]) == 0) {
         return true;
      }
   }
   return false;
}

/**
 * @brief Run a whitelisted shell command and capture output
 * @param cmd Command to run (must be in ALLOWED_COMMANDS whitelist)
 * @param output Buffer to store output
 * @param output_size Size of output buffer
 * @return Number of bytes read on success, 0 on error (with empty output)
 *
 * SECURITY: This function only executes commands that match the exact strings
 * in ALLOWED_COMMANDS. Any other command is rejected. This prevents command
 * injection even if caller accidentally passes user-controlled input.
 */
static size_t run_whitelisted_command(const char *cmd, char *output, size_t output_size) {
   if (output_size > 0) {
      output[0] = '\0';
   }

   /* Security check: only run whitelisted commands */
   if (!is_command_whitelisted(cmd)) {
      LOG_ERROR("WebUI: Blocked non-whitelisted command: %.50s...", cmd ? cmd : "(null)");
      return 0;
   }

   FILE *fp = popen(cmd, "r");
   if (!fp) {
      LOG_WARNING("WebUI: popen failed for command");
      return 0;
   }

   size_t total = 0;
   char buf[256];
   while (fgets(buf, sizeof(buf), fp) && total < output_size - 1) {
      size_t len = strlen(buf);
      if (total + len >= output_size) {
         len = output_size - total - 1;
      }
      memcpy(output + total, buf, len);
      total += len;
   }
   output[total] = '\0';

   pclose(fp);
   return total;
}

/**
 * @brief Parse ALSA device list (arecord -L or aplay -L output)
 */
static void parse_alsa_devices(const char *output, json_object *arr) {
   if (!output || !arr)
      return;

   /* ALSA -L output format:
    * devicename
    *     Description line
    *     ...
    * nextdevice
    */
   const char *line = output;
   while (*line) {
      /* Skip whitespace-prefixed description lines */
      if (*line != ' ' && *line != '\t' && *line != '\n') {
         /* This is a device name line */
         const char *end = strchr(line, '\n');
         size_t len = end ? (size_t)(end - line) : strlen(line);

         if (len > 0 && len < 256) {
            char device[256];
            strncpy(device, line, len);
            device[len] = '\0';

            /* Skip null device and some internal devices */
            if (strcmp(device, "null") != 0 && strncmp(device, "hw:", 3) != 0 &&
                strncmp(device, "plughw:", 7) != 0) {
               json_object_array_add(arr, json_object_new_string(device));
            }
         }
      }

      /* Move to next line */
      const char *next = strchr(line, '\n');
      if (!next)
         break;
      line = next + 1;
   }
}

/**
 * @brief Parse PulseAudio source/sink list (pactl list short output)
 * @param output Command output to parse
 * @param arr JSON array to add devices to
 * @param filter_monitors If true, filter out .monitor devices (for sources only)
 */
static void parse_pulse_devices(const char *output, json_object *arr, bool filter_monitors) {
   if (!output || !arr)
      return;

   /* pactl list sources/sinks short format:
    * index\tname\tmodule\tsample_spec\tstate
    */
   const char *line = output;
   while (*line) {
      /* Find the name field (second column, tab-separated) */
      const char *tab1 = strchr(line, '\t');
      if (tab1) {
         tab1++; /* Skip the tab */
         const char *tab2 = strchr(tab1, '\t');
         if (tab2) {
            size_t len = (size_t)(tab2 - tab1);
            if (len > 0 && len < 256) {
               char device[256];
               strncpy(device, tab1, len);
               device[len] = '\0';

               /* Filter out monitor sources if requested (they capture sink output, not mic input)
                */
               if (filter_monitors && strstr(device, ".monitor") != NULL) {
                  /* Skip to next line */
                  const char *next = strchr(line, '\n');
                  if (!next)
                     break;
                  line = next + 1;
                  continue;
               }

               /* Add device (PulseAudio names are usually descriptive) */
               json_object_array_add(arr, json_object_new_string(device));
            }
         }
      }

      /* Move to next line */
      const char *next = strchr(line, '\n');
      if (!next)
         break;
      line = next + 1;
   }
}

/* Audio device cache to avoid repeated popen() calls */
#define AUDIO_DEVICE_CACHE_TTL_SEC 30
#define AUDIO_DEVICE_BUFFER_SIZE 2048

static struct {
   time_t alsa_capture_time;
   time_t alsa_playback_time;
   time_t pulse_capture_time;
   time_t pulse_playback_time;
   char alsa_capture[AUDIO_DEVICE_BUFFER_SIZE];
   char alsa_playback[AUDIO_DEVICE_BUFFER_SIZE];
   char pulse_capture[AUDIO_DEVICE_BUFFER_SIZE];
   char pulse_playback[AUDIO_DEVICE_BUFFER_SIZE];
} s_device_cache = { 0 };

static void handle_get_audio_devices(ws_connection_t *conn, struct json_object *payload) {
   json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("get_audio_devices_response"));
   json_object *resp_payload = json_object_new_object();

   /* Get backend from payload */
   const char *backend = "auto";
   if (payload) {
      struct json_object *backend_obj;
      if (json_object_object_get_ex(payload, "backend", &backend_obj)) {
         backend = json_object_get_string(backend_obj);
      }
   }

   json_object *capture_devices = json_object_new_array();
   json_object *playback_devices = json_object_new_array();

   /* Always add "default" as first option */
   json_object_array_add(capture_devices, json_object_new_string("default"));
   json_object_array_add(playback_devices, json_object_new_string("default"));

   time_t now = time(NULL);

   if (strcmp(backend, "alsa") == 0) {
      /* Get ALSA devices (with caching) */
      if (now - s_device_cache.alsa_capture_time > AUDIO_DEVICE_CACHE_TTL_SEC) {
         if (run_whitelisted_command("arecord -L 2>/dev/null", s_device_cache.alsa_capture,
                                     sizeof(s_device_cache.alsa_capture)) > 0) {
            s_device_cache.alsa_capture_time = now;
         }
      }
      if (s_device_cache.alsa_capture[0]) {
         parse_alsa_devices(s_device_cache.alsa_capture, capture_devices);
      }

      if (now - s_device_cache.alsa_playback_time > AUDIO_DEVICE_CACHE_TTL_SEC) {
         if (run_whitelisted_command("aplay -L 2>/dev/null", s_device_cache.alsa_playback,
                                     sizeof(s_device_cache.alsa_playback)) > 0) {
            s_device_cache.alsa_playback_time = now;
         }
      }
      if (s_device_cache.alsa_playback[0]) {
         parse_alsa_devices(s_device_cache.alsa_playback, playback_devices);
      }
   } else if (strcmp(backend, "pulse") == 0) {
      /* Get PulseAudio devices (with caching) */
      if (now - s_device_cache.pulse_capture_time > AUDIO_DEVICE_CACHE_TTL_SEC) {
         if (run_whitelisted_command("pactl list sources short 2>/dev/null",
                                     s_device_cache.pulse_capture,
                                     sizeof(s_device_cache.pulse_capture)) > 0) {
            s_device_cache.pulse_capture_time = now;
         }
      }
      if (s_device_cache.pulse_capture[0]) {
         parse_pulse_devices(s_device_cache.pulse_capture, capture_devices,
                             true); /* Filter out .monitor sources */
      }

      if (now - s_device_cache.pulse_playback_time > AUDIO_DEVICE_CACHE_TTL_SEC) {
         if (run_whitelisted_command("pactl list sinks short 2>/dev/null",
                                     s_device_cache.pulse_playback,
                                     sizeof(s_device_cache.pulse_playback)) > 0) {
            s_device_cache.pulse_playback_time = now;
         }
      }
      if (s_device_cache.pulse_playback[0]) {
         parse_pulse_devices(s_device_cache.pulse_playback, playback_devices,
                             false); /* Sinks don't need filtering */
      }
   }
   /* For "auto", just return default - actual device selection happens at runtime */

   json_object_object_add(resp_payload, "backend", json_object_new_string(backend));
   json_object_object_add(resp_payload, "capture_devices", capture_devices);
   json_object_object_add(resp_payload, "playback_devices", playback_devices);
   json_object_object_add(response, "payload", resp_payload);

   const char *json_str = json_object_to_json_string(response);
   size_t json_len = strlen(json_str);
   unsigned char *buf = malloc(LWS_PRE + json_len);
   if (buf) {
      memcpy(buf + LWS_PRE, json_str, json_len);
      lws_write(conn->wsi, buf + LWS_PRE, json_len, LWS_WRITE_TEXT);
      free(buf);
   }

   json_object_put(response);
   LOG_INFO("WebUI: Sent audio devices for backend '%s'", backend);
}

/**
 * @brief Validate that a resolved path is within allowed directories
 *
 * Prevents path traversal attacks by ensuring model paths are in expected locations.
 */
static bool is_path_allowed(const char *resolved_path) {
   if (!resolved_path) {
      return false;
   }

   /* Get current working directory as base - always allowed */
   char cwd[CONFIG_PATH_MAX];
   if (getcwd(cwd, sizeof(cwd)) != NULL) {
      if (strncmp(resolved_path, cwd, strlen(cwd)) == 0) {
         return true;
      }
   }

   /* Check against allowed prefixes (defined at top of file) */
   for (int i = 0; s_allowed_path_prefixes[i] != NULL; i++) {
      if (strncmp(resolved_path, s_allowed_path_prefixes[i], strlen(s_allowed_path_prefixes[i])) ==
          0) {
         return true;
      }
   }

   return false;
}

/**
 * @brief Scan a directory for model files and build the response
 *
 * Internal helper that does the actual directory scanning.
 */
static json_object *scan_models_directory(void) {
   const dawn_config_t *config = config_get();
   json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("list_models_response"));
   json_object *payload = json_object_new_object();

   json_object *asr_models = json_object_new_array();
   json_object *tts_voices = json_object_new_array();

   /* Resolve ASR models path - use dynamic realpath to avoid buffer overflow */
   char asr_path[CONFIG_PATH_MAX];
   char *resolved = realpath(config->asr.models_path, NULL); /* Dynamic allocation */
   bool asr_valid = false;

   if (resolved) {
      asr_valid = is_path_allowed(resolved);
      strncpy(asr_path, resolved, sizeof(asr_path) - 1);
      asr_path[sizeof(asr_path) - 1] = '\0';
      free(resolved);
   } else {
      /* realpath failed - use original path with validation */
      strncpy(asr_path, config->asr.models_path, sizeof(asr_path) - 1);
      asr_path[sizeof(asr_path) - 1] = '\0';
      asr_valid = (asr_path[0] == '.' || is_path_allowed(asr_path));
   }

   if (!asr_valid) {
      LOG_WARNING("WebUI: ASR models path outside allowed directories: %s", asr_path);
   }

   /* Scan ASR models directory for ggml-*.bin files */
   if (asr_valid) {
      DIR *asr_dir = opendir(asr_path);
      if (asr_dir) {
         struct dirent *entry;
         while ((entry = readdir(asr_dir)) != NULL) {
            /* Look for ggml-*.bin files */
            if (entry->d_type == DT_REG || entry->d_type == DT_LNK || entry->d_type == DT_UNKNOWN) {
               const char *name = entry->d_name;
               if (strncmp(name, "ggml-", 5) == 0) {
                  const char *ext = strrchr(name, '.');
                  if (ext && strcmp(ext, ".bin") == 0) {
                     /* Extract model name between "ggml-" and ".bin" */
                     size_t model_len = ext - (name + 5);
                     if (model_len > 0 && model_len < 64) {
                        char model_name[64];
                        strncpy(model_name, name + 5, model_len);
                        model_name[model_len] = '\0';
                        json_object_array_add(asr_models, json_object_new_string(model_name));
                     }
                  }
               }
            }
         }
         closedir(asr_dir);
      } else {
         LOG_WARNING("WebUI: Could not open ASR models path: %s", asr_path);
      }
   }

   /* Resolve TTS models path - use dynamic realpath to avoid buffer overflow */
   char tts_path[CONFIG_PATH_MAX];
   resolved = realpath(config->tts.models_path, NULL); /* Dynamic allocation */
   bool tts_valid = false;

   if (resolved) {
      tts_valid = is_path_allowed(resolved);
      strncpy(tts_path, resolved, sizeof(tts_path) - 1);
      tts_path[sizeof(tts_path) - 1] = '\0';
      free(resolved);
   } else {
      /* realpath failed - use original path with validation */
      strncpy(tts_path, config->tts.models_path, sizeof(tts_path) - 1);
      tts_path[sizeof(tts_path) - 1] = '\0';
      tts_valid = (tts_path[0] == '.' || is_path_allowed(tts_path));
   }

   if (!tts_valid) {
      LOG_WARNING("WebUI: TTS models path outside allowed directories: %s", tts_path);
   }

   /* Scan TTS models directory for *.onnx files (excluding VAD models) */
   if (tts_valid) {
      DIR *tts_dir = opendir(tts_path);
      if (tts_dir) {
         struct dirent *entry;
         while ((entry = readdir(tts_dir)) != NULL) {
            if (entry->d_type == DT_REG || entry->d_type == DT_LNK || entry->d_type == DT_UNKNOWN) {
               const char *name = entry->d_name;
               const char *ext = strrchr(name, '.');
               /* Check extension and skip VAD models in single pass */
               if (ext && strcmp(ext, ".onnx") == 0 && strstr(name, "vad") == NULL &&
                   strstr(name, "VAD") == NULL) {
                  /* Extract voice name (filename without .onnx extension) */
                  size_t voice_len = ext - name;
                  if (voice_len > 0 && voice_len < 128) {
                     char voice_name[128];
                     strncpy(voice_name, name, voice_len);
                     voice_name[voice_len] = '\0';
                     json_object_array_add(tts_voices, json_object_new_string(voice_name));
                  }
               }
            }
         }
         closedir(tts_dir);
      } else {
         LOG_WARNING("WebUI: Could not open TTS models path: %s", tts_path);
      }
   }

   json_object_object_add(payload, "asr_models", asr_models);
   json_object_object_add(payload, "tts_voices", tts_voices);
   json_object_object_add(payload, "asr_path", json_object_new_string(config->asr.models_path));
   json_object_object_add(payload, "tts_path", json_object_new_string(config->tts.models_path));
   json_object_object_add(response, "payload", payload);

   LOG_INFO("WebUI: Scanned models (%zu ASR, %zu TTS)", json_object_array_length(asr_models),
            json_object_array_length(tts_voices));

   return response;
}

/**
 * @brief List available ASR and TTS models from configured paths
 *
 * Scans the configured model directories for:
 * - ASR: ggml-*.bin files (Whisper models) - extracts model name (tiny, base, small, etc.)
 * - TTS: *.onnx files (Piper voices) - returns full filename without extension
 *
 * Results are cached for MODEL_CACHE_TTL seconds to avoid repeated filesystem scans.
 */
static void handle_list_models(ws_connection_t *conn) {
   time_t now = time(NULL);

   pthread_mutex_lock(&s_discovery_cache.cache_mutex);

   /* Check if cache is still valid */
   if (s_discovery_cache.models_response &&
       (now - s_discovery_cache.models_cache_time) < MODEL_CACHE_TTL) {
      /* Return cached response */
      send_json_response(conn->wsi, s_discovery_cache.models_response);
      LOG_INFO("WebUI: Sent cached model list");
      pthread_mutex_unlock(&s_discovery_cache.cache_mutex);
      return;
   }

   /* Invalidate old cache */
   if (s_discovery_cache.models_response) {
      json_object_put(s_discovery_cache.models_response);
      s_discovery_cache.models_response = NULL;
   }

   pthread_mutex_unlock(&s_discovery_cache.cache_mutex);

   /* Build new response (outside lock to avoid blocking) */
   json_object *response = scan_models_directory();

   /* Update cache */
   pthread_mutex_lock(&s_discovery_cache.cache_mutex);
   s_discovery_cache.models_response = json_object_get(response); /* Increment refcount */
   s_discovery_cache.models_cache_time = now;
   pthread_mutex_unlock(&s_discovery_cache.cache_mutex);

   /* Send response */
   send_json_response(conn->wsi, response);
   json_object_put(response);
}

/**
 * @brief Scan network interfaces and build the response
 *
 * Internal helper that does the actual interface enumeration.
 */
static json_object *scan_network_interfaces(void) {
   json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("list_interfaces_response"));
   json_object *payload = json_object_new_object();

   json_object *addresses = json_object_new_array();

   /* Track seen IPs efficiently without JSON library overhead */
   char seen_ips[16][INET_ADDRSTRLEN];
   int seen_count = 0;

   /* Always include common options first */
   json_object_array_add(addresses, json_object_new_string("0.0.0.0"));
   strncpy(seen_ips[seen_count++], "0.0.0.0", INET_ADDRSTRLEN);
   json_object_array_add(addresses, json_object_new_string("127.0.0.1"));
   strncpy(seen_ips[seen_count++], "127.0.0.1", INET_ADDRSTRLEN);

   /* Get actual interface addresses */
   struct ifaddrs *ifaddr, *ifa;
   if (getifaddrs(&ifaddr) == 0) {
      for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
         if (ifa->ifa_addr == NULL)
            continue;

         /* Only IPv4 addresses */
         if (ifa->ifa_addr->sa_family == AF_INET) {
            /* Skip loopback (already added 127.0.0.1) */
            if (ifa->ifa_flags & IFF_LOOPBACK)
               continue;

            struct sockaddr_in *addr = (struct sockaddr_in *)ifa->ifa_addr;
            char ip_str[INET_ADDRSTRLEN];
            if (inet_ntop(AF_INET, &addr->sin_addr, ip_str, sizeof(ip_str))) {
               /* Check for duplicates using simple array (faster than JSON iteration) */
               int duplicate = 0;
               for (int j = 0; j < seen_count; j++) {
                  if (strcmp(seen_ips[j], ip_str) == 0) {
                     duplicate = 1;
                     break;
                  }
               }
               if (!duplicate && seen_count < 16) {
                  strncpy(seen_ips[seen_count++], ip_str, INET_ADDRSTRLEN);
                  json_object_array_add(addresses, json_object_new_string(ip_str));
               }
            }
         }
      }
      freeifaddrs(ifaddr);
   } else {
      LOG_WARNING("WebUI: getifaddrs failed: %s", strerror(errno));
      /* Continue with just 0.0.0.0 and 127.0.0.1 */
   }

   json_object_object_add(payload, "addresses", addresses);
   json_object_object_add(response, "payload", payload);

   LOG_INFO("WebUI: Scanned interfaces (%d addresses)", seen_count);
   return response;
}

/**
 * @brief List available network interfaces and their IP addresses
 *
 * Returns bind address options including:
 * - 0.0.0.0 (all interfaces)
 * - 127.0.0.1 (localhost)
 * - Individual interface IPs (e.g., 192.168.1.100)
 *
 * Results are cached for MODEL_CACHE_TTL seconds to avoid repeated system calls.
 */
static void handle_list_interfaces(ws_connection_t *conn) {
   time_t now = time(NULL);

   pthread_mutex_lock(&s_discovery_cache.cache_mutex);

   /* Check if cache is still valid */
   if (s_discovery_cache.interfaces_response &&
       (now - s_discovery_cache.interfaces_cache_time) < MODEL_CACHE_TTL) {
      /* Return cached response */
      send_json_response(conn->wsi, s_discovery_cache.interfaces_response);
      LOG_INFO("WebUI: Sent cached interface list");
      pthread_mutex_unlock(&s_discovery_cache.cache_mutex);
      return;
   }

   /* Invalidate old cache */
   if (s_discovery_cache.interfaces_response) {
      json_object_put(s_discovery_cache.interfaces_response);
      s_discovery_cache.interfaces_response = NULL;
   }

   pthread_mutex_unlock(&s_discovery_cache.cache_mutex);

   /* Build new response (outside lock to avoid blocking) */
   json_object *response = scan_network_interfaces();

   /* Update cache */
   pthread_mutex_lock(&s_discovery_cache.cache_mutex);
   s_discovery_cache.interfaces_response = json_object_get(response); /* Increment refcount */
   s_discovery_cache.interfaces_cache_time = now;
   pthread_mutex_unlock(&s_discovery_cache.cache_mutex);

   /* Send response */
   send_json_response(conn->wsi, response);
   json_object_put(response);
}

/* =============================================================================
 * Tool Configuration Handlers
 * ============================================================================= */

static void handle_get_tools_config(ws_connection_t *conn) {
   json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("get_tools_config_response"));

   json_object *payload = json_object_new_object();
   json_object *tools_array = json_object_new_array();

   /* Get all tools with their enable states */
   tool_info_t tools[LLM_TOOLS_MAX_TOOLS];
   int count = llm_tools_get_all(tools, LLM_TOOLS_MAX_TOOLS);

   for (int i = 0; i < count; i++) {
      json_object *tool_obj = json_object_new_object();
      json_object_object_add(tool_obj, "name", json_object_new_string(tools[i].name));
      json_object_object_add(tool_obj, "description", json_object_new_string(tools[i].description));
      json_object_object_add(tool_obj, "available", json_object_new_boolean(tools[i].enabled));
      json_object_object_add(tool_obj, "local", json_object_new_boolean(tools[i].enabled_local));
      json_object_object_add(tool_obj, "remote", json_object_new_boolean(tools[i].enabled_remote));
      json_object_object_add(tool_obj, "armor_feature",
                             json_object_new_boolean(tools[i].armor_feature));
      json_object_array_add(tools_array, tool_obj);
   }

   json_object_object_add(payload, "tools", tools_array);

   /* Add token estimates */
   json_object *estimates = json_object_new_object();
   json_object_object_add(estimates, "local",
                          json_object_new_int(llm_tools_estimate_tokens(false)));
   json_object_object_add(estimates, "remote",
                          json_object_new_int(llm_tools_estimate_tokens(true)));
   json_object_object_add(payload, "token_estimate", estimates);

   json_object_object_add(response, "payload", payload);
   send_json_response(conn->wsi, response);
   json_object_put(response);

   LOG_INFO("WebUI: Sent tools config (%d tools)", count);
}

/* =============================================================================
 * Metrics Handler
 * ============================================================================= */

static void handle_get_metrics(ws_connection_t *conn) {
   dawn_metrics_t snapshot;
   metrics_get_snapshot(&snapshot);

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("get_metrics_response"));

   json_object *payload = json_object_new_object();

   /* Session statistics */
   json_object *session = json_object_new_object();
   json_object_object_add(session, "uptime_seconds", json_object_new_int64(metrics_get_uptime()));
   json_object_object_add(session, "queries_total", json_object_new_int(snapshot.queries_total));
   json_object_object_add(session, "queries_cloud", json_object_new_int(snapshot.queries_cloud));
   json_object_object_add(session, "queries_local", json_object_new_int(snapshot.queries_local));
   json_object_object_add(session, "errors", json_object_new_int(snapshot.errors_count));
   json_object_object_add(session, "fallbacks", json_object_new_int(snapshot.fallbacks_count));
   json_object_object_add(session, "bargeins", json_object_new_int(snapshot.bargein_count));
   json_object_object_add(payload, "session", session);

   /* Token usage */
   json_object *tokens = json_object_new_object();
   json_object_object_add(tokens, "cloud_input",
                          json_object_new_int64(snapshot.tokens_cloud_input));
   json_object_object_add(tokens, "cloud_output",
                          json_object_new_int64(snapshot.tokens_cloud_output));
   json_object_object_add(tokens, "local_input",
                          json_object_new_int64(snapshot.tokens_local_input));
   json_object_object_add(tokens, "local_output",
                          json_object_new_int64(snapshot.tokens_local_output));
   json_object_object_add(tokens, "cached", json_object_new_int64(snapshot.tokens_cached));
   json_object_object_add(payload, "tokens", tokens);

   /* Last query timing */
   json_object *last = json_object_new_object();
   json_object_object_add(last, "vad_ms", json_object_new_double(snapshot.last_vad_time_ms));
   json_object_object_add(last, "asr_ms", json_object_new_double(snapshot.last_asr_time_ms));
   json_object_object_add(last, "asr_rtf", json_object_new_double(snapshot.last_asr_rtf));
   json_object_object_add(last, "llm_ttft_ms", json_object_new_double(snapshot.last_llm_ttft_ms));
   json_object_object_add(last, "llm_total_ms", json_object_new_double(snapshot.last_llm_total_ms));
   json_object_object_add(last, "tts_ms", json_object_new_double(snapshot.last_tts_time_ms));
   json_object_object_add(last, "pipeline_ms",
                          json_object_new_double(snapshot.last_total_pipeline_ms));
   json_object_object_add(payload, "last", last);

   /* Average timing */
   json_object *avg = json_object_new_object();
   json_object_object_add(avg, "vad_ms", json_object_new_double(snapshot.avg_vad_ms));
   json_object_object_add(avg, "asr_ms", json_object_new_double(snapshot.avg_asr_ms));
   json_object_object_add(avg, "asr_rtf", json_object_new_double(snapshot.avg_asr_rtf));
   json_object_object_add(avg, "llm_ttft_ms", json_object_new_double(snapshot.avg_llm_ttft_ms));
   json_object_object_add(avg, "llm_total_ms", json_object_new_double(snapshot.avg_llm_total_ms));
   json_object_object_add(avg, "tts_ms", json_object_new_double(snapshot.avg_tts_ms));
   json_object_object_add(avg, "pipeline_ms",
                          json_object_new_double(snapshot.avg_total_pipeline_ms));
   json_object_object_add(payload, "averages", avg);

   /* Real-time state */
   json_object *state = json_object_new_object();
   json_object_object_add(state, "current",
                          json_object_new_string(dawn_state_name(snapshot.current_state)));
   json_object_object_add(state, "vad_probability",
                          json_object_new_double(snapshot.current_vad_probability));
   json_object_object_add(state, "audio_buffer_fill",
                          json_object_new_double(snapshot.audio_buffer_fill_pct));
   json_object_object_add(payload, "state", state);

   /* AEC status */
   json_object *aec = json_object_new_object();
   json_object_object_add(aec, "enabled", json_object_new_boolean(snapshot.aec_enabled));
   json_object_object_add(aec, "calibrated", json_object_new_boolean(snapshot.aec_calibrated));
   json_object_object_add(aec, "delay_ms", json_object_new_int(snapshot.aec_delay_ms));
   json_object_object_add(aec, "correlation", json_object_new_double(snapshot.aec_correlation));
   json_object_object_add(payload, "aec", aec);

   /* Summarizer stats */
   json_object *summarizer = json_object_new_object();
   json_object_object_add(summarizer, "backend",
                          json_object_new_string(snapshot.summarizer_backend));
   json_object_object_add(summarizer, "threshold",
                          json_object_new_int64(snapshot.summarizer_threshold));
   json_object_object_add(summarizer, "calls", json_object_new_int(snapshot.summarizer_call_count));
   json_object_object_add(payload, "summarizer", summarizer);

   json_object_object_add(response, "payload", payload);
   send_json_response(conn->wsi, response);
   json_object_put(response);
}

/**
 * @brief Validate a tool name for safe processing.
 *
 * Tool names must be non-empty, under LLM_TOOL_NAME_MAX length,
 * and contain only alphanumeric characters, underscores, and hyphens.
 *
 * @param name The tool name to validate
 * @return true if valid, false otherwise
 */
static bool is_valid_tool_name(const char *name) {
   if (!name || name[0] == '\0') {
      return false;
   }

   size_t len = strlen(name);
   if (len >= LLM_TOOL_NAME_MAX) {
      return false;
   }

   for (size_t i = 0; i < len; i++) {
      char c = name[i];
      if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '_' || c == '-')) {
         return false;
      }
   }

   return true;
}

static void handle_set_tools_config(ws_connection_t *conn, struct json_object *payload) {
   /* Admin-only operation */
   if (!conn_require_admin(conn)) {
      return;
   }

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("set_tools_config_response"));
   json_object *resp_payload = json_object_new_object();

   json_object *tools_array;
   if (!json_object_object_get_ex(payload, "tools", &tools_array)) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Missing 'tools' array"));
      json_object_object_add(response, "payload", resp_payload);
      send_json_response(conn->wsi, response);
      json_object_put(response);
      return;
   }

   int updated = 0;
   int skipped = 0;
   int len = json_object_array_length(tools_array);
   for (int i = 0; i < len; i++) {
      json_object *tool_obj = json_object_array_get_idx(tools_array, i);

      json_object *name_obj, *local_obj, *remote_obj;
      if (json_object_object_get_ex(tool_obj, "name", &name_obj) &&
          json_object_object_get_ex(tool_obj, "local", &local_obj) &&
          json_object_object_get_ex(tool_obj, "remote", &remote_obj)) {
         const char *name = json_object_get_string(name_obj);

         /* Validate tool name before processing */
         if (!is_valid_tool_name(name)) {
            LOG_WARNING("WebUI: Skipping invalid tool name: '%s'", name ? name : "(null)");
            skipped++;
            continue;
         }

         bool local = json_object_get_boolean(local_obj);
         bool remote = json_object_get_boolean(remote_obj);

         if (llm_tools_set_enabled(name, local, remote) == 0) {
            updated++;
         }
      }
   }

   /* Update config arrays for persistence - requires write lock on g_config */
   tool_info_t tools[LLM_TOOLS_MAX_TOOLS];
   int count = llm_tools_get_all(tools, LLM_TOOLS_MAX_TOOLS);

   pthread_rwlock_wrlock(&s_config_rwlock);

   g_config.llm.tools.local_enabled_count = 0;
   g_config.llm.tools.remote_enabled_count = 0;

   for (int i = 0; i < count; i++) {
      if (tools[i].enabled_local &&
          g_config.llm.tools.local_enabled_count < LLM_TOOLS_MAX_CONFIGURED) {
         safe_strncpy(g_config.llm.tools.local_enabled[g_config.llm.tools.local_enabled_count++],
                      tools[i].name, LLM_TOOL_NAME_MAX);
      }
      if (tools[i].enabled_remote &&
          g_config.llm.tools.remote_enabled_count < LLM_TOOLS_MAX_CONFIGURED) {
         safe_strncpy(g_config.llm.tools.remote_enabled[g_config.llm.tools.remote_enabled_count++],
                      tools[i].name, LLM_TOOL_NAME_MAX);
      }
   }

   /* Save to TOML */
   const char *config_path = config_get_loaded_path();
   if (!config_path || strcmp(config_path, "(none - using defaults)") == 0) {
      config_path = "./dawn.toml";
   }
   config_write_toml(&g_config, config_path);

   pthread_rwlock_unlock(&s_config_rwlock);

   json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
   json_object_object_add(resp_payload, "updated", json_object_new_int(updated));

   /* Include updated token estimates */
   json_object *estimates = json_object_new_object();
   json_object_object_add(estimates, "local",
                          json_object_new_int(llm_tools_estimate_tokens(false)));
   json_object_object_add(estimates, "remote",
                          json_object_new_int(llm_tools_estimate_tokens(true)));
   json_object_object_add(resp_payload, "token_estimate", estimates);

   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn->wsi, response);
   json_object_put(response);

   LOG_INFO("WebUI: Updated %d tool enable states", updated);
}

/* =============================================================================
 * User Management Handlers (Admin-only)
 * ============================================================================= */

/* Callback for user list enumeration */
static int list_users_callback(const auth_user_summary_t *user, void *context) {
   json_object *users_array = (json_object *)context;
   json_object *user_obj = json_object_new_object();
   json_object_object_add(user_obj, "id", json_object_new_int(user->id));
   json_object_object_add(user_obj, "username", json_object_new_string(user->username));
   json_object_object_add(user_obj, "is_admin", json_object_new_boolean(user->is_admin));
   json_object_object_add(user_obj, "created_at", json_object_new_int64(user->created_at));
   json_object_object_add(user_obj, "last_login", json_object_new_int64(user->last_login));
   json_object_object_add(user_obj, "failed_attempts", json_object_new_int(user->failed_attempts));
   json_object_object_add(user_obj, "is_locked",
                          json_object_new_boolean(user->lockout_until > time(NULL)));
   json_object_array_add(users_array, user_obj);
   return 0;
}

/**
 * @brief List all users (admin only)
 */
static void handle_list_users(ws_connection_t *conn) {
   if (!conn_require_admin(conn)) {
      return;
   }

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("list_users_response"));
   json_object *resp_payload = json_object_new_object();
   json_object *users_array = json_object_new_array();

   int result = auth_db_list_users(list_users_callback, users_array);
   if (result == AUTH_DB_SUCCESS) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
      json_object_object_add(resp_payload, "users", users_array);
   } else {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error", json_object_new_string("Failed to list users"));
      json_object_put(users_array);
   }

   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn->wsi, response);
   json_object_put(response);
}

/**
 * @brief Create a new user (admin only)
 */
static void handle_create_user(ws_connection_t *conn, struct json_object *payload) {
   if (!conn_require_admin(conn)) {
      return;
   }

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("create_user_response"));
   json_object *resp_payload = json_object_new_object();

   /* Get required fields */
   json_object *username_obj, *password_obj, *is_admin_obj;
   if (!json_object_object_get_ex(payload, "username", &username_obj) ||
       !json_object_object_get_ex(payload, "password", &password_obj)) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Missing username or password"));
      json_object_object_add(response, "payload", resp_payload);
      send_json_response(conn->wsi, response);
      json_object_put(response);
      return;
   }

   const char *username = json_object_get_string(username_obj);
   const char *password = json_object_get_string(password_obj);
   bool is_admin = json_object_object_get_ex(payload, "is_admin", &is_admin_obj)
                       ? json_object_get_boolean(is_admin_obj)
                       : false;

   /* Validate username format */
   if (auth_db_validate_username(username) != AUTH_DB_SUCCESS) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Invalid username format"));
      json_object_object_add(response, "payload", resp_payload);
      send_json_response(conn->wsi, response);
      json_object_put(response);
      return;
   }

   /* Validate password length */
   if (!password || strlen(password) < 8) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Password must be at least 8 characters"));
      json_object_object_add(response, "payload", resp_payload);
      send_json_response(conn->wsi, response);
      json_object_put(response);
      return;
   }

   /* Hash password */
   char hash[AUTH_HASH_LEN];
   if (auth_hash_password(password, hash) != 0) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Failed to hash password"));
      json_object_object_add(response, "payload", resp_payload);
      send_json_response(conn->wsi, response);
      json_object_put(response);
      return;
   }

   /* Create user */
   int result = auth_db_create_user(username, hash, is_admin);

   if (result == AUTH_DB_SUCCESS) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
      json_object_object_add(resp_payload, "message", json_object_new_string("User created"));

      /* Log event */
      char details[256];
      snprintf(details, sizeof(details), "Created user '%s' (admin=%s) by '%s'", username,
               is_admin ? "yes" : "no", conn->username);
      auth_db_log_event("USER_CREATED", username, conn->client_ip, details);
      LOG_INFO("WebUI: %s", details);
   } else if (result == AUTH_DB_DUPLICATE) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Username already exists"));
   } else {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Failed to create user"));
   }

   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn->wsi, response);
   json_object_put(response);
}

/**
 * @brief Delete a user (admin only)
 */
static void handle_delete_user(ws_connection_t *conn, struct json_object *payload) {
   if (!conn_require_admin(conn)) {
      return;
   }

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("delete_user_response"));
   json_object *resp_payload = json_object_new_object();

   json_object *username_obj;
   if (!json_object_object_get_ex(payload, "username", &username_obj)) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error", json_object_new_string("Missing username"));
      json_object_object_add(response, "payload", resp_payload);
      send_json_response(conn->wsi, response);
      json_object_put(response);
      return;
   }

   const char *username = json_object_get_string(username_obj);

   /* Prevent self-deletion */
   if (strcmp(username, conn->username) == 0) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Cannot delete your own account"));
      json_object_object_add(response, "payload", resp_payload);
      send_json_response(conn->wsi, response);
      json_object_put(response);
      return;
   }

   int result = auth_db_delete_user(username);

   if (result == AUTH_DB_SUCCESS) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
      json_object_object_add(resp_payload, "message", json_object_new_string("User deleted"));

      /* Log event */
      char details[256];
      snprintf(details, sizeof(details), "Deleted by '%s'", conn->username);
      auth_db_log_event("USER_DELETED", username, conn->client_ip, details);
      LOG_INFO("WebUI: User '%s' deleted by '%s'", username, conn->username);
   } else if (result == AUTH_DB_NOT_FOUND) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error", json_object_new_string("User not found"));
   } else if (result == AUTH_DB_LAST_ADMIN) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Cannot delete last admin user"));
   } else {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Failed to delete user"));
   }

   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn->wsi, response);
   json_object_put(response);
}

/**
 * @brief Change user password (admin for any user, or user for self with current password)
 */
static void handle_change_password(ws_connection_t *conn, struct json_object *payload) {
   if (!conn_require_auth(conn)) {
      return;
   }

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("change_password_response"));
   json_object *resp_payload = json_object_new_object();

   json_object *username_obj, *new_password_obj, *current_password_obj;
   if (!json_object_object_get_ex(payload, "username", &username_obj) ||
       !json_object_object_get_ex(payload, "new_password", &new_password_obj)) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Missing username or new_password"));
      json_object_object_add(response, "payload", resp_payload);
      send_json_response(conn->wsi, response);
      json_object_put(response);
      return;
   }

   const char *username = json_object_get_string(username_obj);
   const char *new_password = json_object_get_string(new_password_obj);
   bool is_self_change = (strcmp(username, conn->username) == 0);

   /* Check permissions: admin can change any password, user can only change own */
   auth_session_t session;
   bool is_admin = false;
   if (auth_db_get_session(conn->auth_session_token, &session) == AUTH_DB_SUCCESS) {
      is_admin = session.is_admin;
   }

   if (!is_admin && !is_self_change) {
      /* Non-admin trying to change someone else's password */
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Cannot change another user's password"));
      json_object_object_add(response, "payload", resp_payload);
      send_json_response(conn->wsi, response);
      json_object_put(response);
      return;
   }

   /* Non-admin self-change requires current password verification */
   if (!is_admin && is_self_change) {
      if (!json_object_object_get_ex(payload, "current_password", &current_password_obj)) {
         json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
         json_object_object_add(resp_payload, "error",
                                json_object_new_string("Current password required"));
         json_object_object_add(response, "payload", resp_payload);
         send_json_response(conn->wsi, response);
         json_object_put(response);
         return;
      }

      const char *current_password = json_object_get_string(current_password_obj);
      auth_user_t user;
      if (auth_db_get_user(username, &user) != AUTH_DB_SUCCESS ||
          auth_verify_password(current_password, user.password_hash) != 0) {
         json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
         json_object_object_add(resp_payload, "error",
                                json_object_new_string("Current password incorrect"));
         json_object_object_add(response, "payload", resp_payload);
         send_json_response(conn->wsi, response);
         json_object_put(response);
         return;
      }
   }

   /* Validate new password length */
   if (!new_password || strlen(new_password) < 8) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("New password must be at least 8 characters"));
      json_object_object_add(response, "payload", resp_payload);
      send_json_response(conn->wsi, response);
      json_object_put(response);
      return;
   }

   /* Hash new password */
   char hash[AUTH_HASH_LEN];
   if (auth_hash_password(new_password, hash) != 0) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Failed to hash password"));
      json_object_object_add(response, "payload", resp_payload);
      send_json_response(conn->wsi, response);
      json_object_put(response);
      return;
   }

   /* Update password (this also invalidates all sessions) */
   int result = auth_db_update_password(username, hash);

   if (result == AUTH_DB_SUCCESS) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
      json_object_object_add(resp_payload, "message", json_object_new_string("Password changed"));

      /* Log event */
      char details[256];
      snprintf(details, sizeof(details), "Password changed by '%s'", conn->username);
      auth_db_log_event("PASSWORD_CHANGED", username, conn->client_ip, details);
      LOG_INFO("WebUI: Password changed for '%s' by '%s'", username, conn->username);
   } else if (result == AUTH_DB_NOT_FOUND) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error", json_object_new_string("User not found"));
   } else {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Failed to change password"));
   }

   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn->wsi, response);
   json_object_put(response);
}

/**
 * @brief Unlock a locked user account (admin only)
 */
static void handle_unlock_user(ws_connection_t *conn, struct json_object *payload) {
   if (!conn_require_admin(conn)) {
      return;
   }

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("unlock_user_response"));
   json_object *resp_payload = json_object_new_object();

   json_object *username_obj;
   if (!json_object_object_get_ex(payload, "username", &username_obj)) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error", json_object_new_string("Missing username"));
      json_object_object_add(response, "payload", resp_payload);
      send_json_response(conn->wsi, response);
      json_object_put(response);
      return;
   }

   const char *username = json_object_get_string(username_obj);

   /* Unlock user and reset failed attempts */
   int result = auth_db_unlock_user(username);
   if (result == AUTH_DB_SUCCESS) {
      auth_db_reset_failed_attempts(username);
   }

   if (result == AUTH_DB_SUCCESS) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
      json_object_object_add(resp_payload, "message", json_object_new_string("User unlocked"));

      /* Log event */
      char details[256];
      snprintf(details, sizeof(details), "Unlocked by '%s'", conn->username);
      auth_db_log_event("USER_UNLOCKED", username, conn->client_ip, details);
      LOG_INFO("WebUI: User '%s' unlocked by '%s'", username, conn->username);
   } else if (result == AUTH_DB_NOT_FOUND) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error", json_object_new_string("User not found"));
   } else {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Failed to unlock user"));
   }

   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn->wsi, response);
   json_object_put(response);
}

/* ============================================================================
 * Personal Settings Handlers (authenticated users)
 * ============================================================================ */

/**
 * @brief Get current user's personal settings
 */
static void handle_get_my_settings(ws_connection_t *conn) {
   if (!conn_require_auth(conn)) {
      return;
   }

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("get_my_settings_response"));
   json_object *resp_payload = json_object_new_object();

   auth_user_settings_t settings;
   int result = auth_db_get_user_settings(conn->auth_user_id, &settings);

   if (result == AUTH_DB_SUCCESS) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(1));

      /* Include base persona (from config or dynamic default) for UI display */
      char base_persona_buf[2048];
      const char *base_persona;
      if (g_config.persona.description[0] != '\0') {
         base_persona = g_config.persona.description;
      } else {
         /* Build dynamic persona with configured AI name */
         const char *ai_name = g_config.general.ai_name[0] != '\0' ? g_config.general.ai_name
                                                                   : AI_NAME;

         /* Capitalize first letter for proper noun */
         char capitalized_name[64];
         snprintf(capitalized_name, sizeof(capitalized_name), "%s", ai_name);
         if (capitalized_name[0] >= 'a' && capitalized_name[0] <= 'z') {
            capitalized_name[0] -= 32;
         }

         snprintf(base_persona_buf, sizeof(base_persona_buf),
                  AI_PERSONA_NAME_TEMPLATE " " AI_PERSONA_TRAITS, capitalized_name);
         base_persona = base_persona_buf;
      }
      json_object_object_add(resp_payload, "base_persona", json_object_new_string(base_persona));

      /* User's custom settings */
      json_object_object_add(resp_payload, "persona_description",
                             json_object_new_string(settings.persona_description));
      json_object_object_add(resp_payload, "persona_mode",
                             json_object_new_string(settings.persona_mode));
      json_object_object_add(resp_payload, "location", json_object_new_string(settings.location));
      json_object_object_add(resp_payload, "timezone", json_object_new_string(settings.timezone));
      json_object_object_add(resp_payload, "units", json_object_new_string(settings.units));
      json_object_object_add(resp_payload, "tts_voice_model",
                             json_object_new_string(settings.tts_voice_model));
      json_object_object_add(resp_payload, "tts_length_scale",
                             json_object_new_double((double)settings.tts_length_scale));
   } else {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Failed to load settings"));
   }

   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn->wsi, response);
   json_object_put(response);
}

/**
 * @brief Update current user's personal settings
 */
static void handle_set_my_settings(ws_connection_t *conn, struct json_object *payload) {
   if (!conn_require_auth(conn)) {
      return;
   }

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("set_my_settings_response"));
   json_object *resp_payload = json_object_new_object();

   /* Get current settings as defaults */
   auth_user_settings_t settings;
   auth_db_get_user_settings(conn->auth_user_id, &settings);

   /* Update with any provided fields */
   json_object *field_obj;

   if (json_object_object_get_ex(payload, "persona_description", &field_obj)) {
      strncpy(settings.persona_description, json_object_get_string(field_obj),
              AUTH_PERSONA_DESC_MAX - 1);
      settings.persona_description[AUTH_PERSONA_DESC_MAX - 1] = '\0';
   }

   if (json_object_object_get_ex(payload, "persona_mode", &field_obj)) {
      const char *mode = json_object_get_string(field_obj);
      /* Validate mode value */
      if (strcmp(mode, "append") == 0 || strcmp(mode, "replace") == 0) {
         strncpy(settings.persona_mode, mode, AUTH_PERSONA_MODE_MAX - 1);
         settings.persona_mode[AUTH_PERSONA_MODE_MAX - 1] = '\0';
      }
   }

   if (json_object_object_get_ex(payload, "location", &field_obj)) {
      strncpy(settings.location, json_object_get_string(field_obj), AUTH_LOCATION_MAX - 1);
      settings.location[AUTH_LOCATION_MAX - 1] = '\0';
   }

   if (json_object_object_get_ex(payload, "timezone", &field_obj)) {
      strncpy(settings.timezone, json_object_get_string(field_obj), AUTH_TIMEZONE_MAX - 1);
      settings.timezone[AUTH_TIMEZONE_MAX - 1] = '\0';
   }

   if (json_object_object_get_ex(payload, "units", &field_obj)) {
      const char *units = json_object_get_string(field_obj);
      /* Validate units value */
      if (strcmp(units, "metric") == 0 || strcmp(units, "imperial") == 0) {
         strncpy(settings.units, units, AUTH_UNITS_MAX - 1);
         settings.units[AUTH_UNITS_MAX - 1] = '\0';
      }
   }

   if (json_object_object_get_ex(payload, "tts_voice_model", &field_obj)) {
      strncpy(settings.tts_voice_model, json_object_get_string(field_obj), AUTH_TTS_VOICE_MAX - 1);
      settings.tts_voice_model[AUTH_TTS_VOICE_MAX - 1] = '\0';
   }

   if (json_object_object_get_ex(payload, "tts_length_scale", &field_obj)) {
      double scale = json_object_get_double(field_obj);
      /* Validate range (0.5 to 2.0 is reasonable for speech rate) */
      if (scale >= 0.5 && scale <= 2.0) {
         settings.tts_length_scale = (float)scale;
      }
   }

   /* Save settings */
   int result = auth_db_set_user_settings(conn->auth_user_id, &settings);

   if (result == AUTH_DB_SUCCESS) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
      json_object_object_add(resp_payload, "message", json_object_new_string("Settings saved"));

      /* Refresh active session's system prompt immediately (preserves conversation) */
      if (conn->session) {
         char *new_prompt = build_user_prompt(conn->auth_user_id);
         if (new_prompt) {
            session_update_system_prompt(conn->session, new_prompt);
            LOG_INFO("WebUI: Refreshed system prompt for user %s", conn->username);

            /* Send updated prompt to client so debug view refreshes */
            json_object *prompt_msg = json_object_new_object();
            json_object_object_add(prompt_msg, "type",
                                   json_object_new_string("system_prompt_response"));
            json_object *prompt_payload = json_object_new_object();
            json_object_object_add(prompt_payload, "success", json_object_new_boolean(1));
            json_object_object_add(prompt_payload, "prompt", json_object_new_string(new_prompt));
            json_object_object_add(prompt_payload, "length",
                                   json_object_new_int((int)strlen(new_prompt)));
            json_object_object_add(prompt_msg, "payload", prompt_payload);
            send_json_response(conn->wsi, prompt_msg);
            json_object_put(prompt_msg);

            free(new_prompt);
         }
      }

      /* Log event */
      auth_db_log_event("SETTINGS_UPDATED", conn->username, conn->client_ip, "Personal settings");
   } else {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Failed to save settings"));
   }

   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn->wsi, response);
   json_object_put(response);
}

/* =============================================================================
 * Session Management Handlers (Authenticated Users)
 * ============================================================================ */

/* Callback for session enumeration */
static int list_sessions_callback(const auth_session_summary_t *session, void *context) {
   json_object *sessions_array = (json_object *)context;
   json_object *session_obj = json_object_new_object();

   json_object_object_add(session_obj, "token_prefix",
                          json_object_new_string(session->token_prefix));
   json_object_object_add(session_obj, "created_at", json_object_new_int64(session->created_at));
   json_object_object_add(session_obj, "last_activity",
                          json_object_new_int64(session->last_activity));
   json_object_object_add(session_obj, "ip_address", json_object_new_string(session->ip_address));
   json_object_object_add(session_obj, "user_agent", json_object_new_string(session->user_agent));

   json_object_array_add(sessions_array, session_obj);
   return 0;
}

/**
 * @brief List current user's active sessions
 *
 * Returns all sessions for the authenticated user, allowing them to see
 * where they're logged in and identify sessions to revoke.
 */
static void handle_list_my_sessions(ws_connection_t *conn) {
   if (!conn_require_auth(conn)) {
      return;
   }

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("list_my_sessions_response"));
   json_object *resp_payload = json_object_new_object();
   json_object *sessions_array = json_object_new_array();

   int result = auth_db_list_user_sessions(conn->auth_user_id, list_sessions_callback,
                                           sessions_array);
   if (result == AUTH_DB_SUCCESS) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
      json_object_object_add(resp_payload, "sessions", sessions_array);

      /* Include current session's token prefix so UI can highlight it */
      char current_prefix[17] = { 0 };
      strncpy(current_prefix, conn->auth_session_token, 16);
      json_object_object_add(resp_payload, "current_session",
                             json_object_new_string(current_prefix));
   } else {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Failed to list sessions"));
      json_object_put(sessions_array);
   }

   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn->wsi, response);
   json_object_put(response);
}

/**
 * @brief Revoke a session by token prefix
 *
 * Users can revoke their own sessions. Admins can revoke any session.
 * Cannot revoke your own current session (use logout instead).
 */
static void handle_revoke_session(ws_connection_t *conn, struct json_object *payload) {
   if (!conn_require_auth(conn)) {
      return;
   }

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("revoke_session_response"));
   json_object *resp_payload = json_object_new_object();

   /* Get token prefix */
   json_object *prefix_obj;
   if (!json_object_object_get_ex(payload, "token_prefix", &prefix_obj)) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error", json_object_new_string("Missing token_prefix"));
      json_object_object_add(response, "payload", resp_payload);
      send_json_response(conn->wsi, response);
      json_object_put(response);
      return;
   }

   const char *prefix = json_object_get_string(prefix_obj);

   /* Validate prefix length (16 chars for reduced collision risk) */
   if (!prefix || strlen(prefix) < 16) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error", json_object_new_string("Invalid token prefix"));
      json_object_object_add(response, "payload", resp_payload);
      send_json_response(conn->wsi, response);
      json_object_put(response);
      return;
   }

   /* Check if trying to revoke current session */
   if (strncmp(conn->auth_session_token, prefix, 16) == 0) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Cannot revoke current session - use logout"));
      json_object_object_add(response, "payload", resp_payload);
      send_json_response(conn->wsi, response);
      json_object_put(response);
      return;
   }

   /* For non-admins, verify the session belongs to them by checking if it appears
    * in their session list. Admins can revoke any session.
    */
   bool is_admin = false;
   auth_session_t auth_session;
   if (auth_db_get_session(conn->auth_session_token, &auth_session) == AUTH_DB_SUCCESS) {
      is_admin = auth_session.is_admin;
   }

   if (!is_admin) {
      /* Verify ownership with efficient single-query check */
      if (!auth_db_session_belongs_to_user(prefix, conn->auth_user_id)) {
         json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
         json_object_object_add(resp_payload, "error",
                                json_object_new_string("Session not found or access denied"));
         json_object_object_add(response, "payload", resp_payload);
         send_json_response(conn->wsi, response);
         json_object_put(response);
         return;
      }
   }

   /* Delete the session */
   int result = auth_db_delete_session_by_prefix(prefix);

   if (result == AUTH_DB_SUCCESS) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
      json_object_object_add(resp_payload, "message", json_object_new_string("Session revoked"));

      /* Log event */
      char details[128];
      snprintf(details, sizeof(details), "Revoked session: %.8s...", prefix);
      auth_db_log_event("SESSION_REVOKED", conn->username, conn->client_ip, details);
   } else if (result == AUTH_DB_NOT_FOUND) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error", json_object_new_string("Session not found"));
   } else {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Failed to revoke session"));
   }

   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn->wsi, response);
   json_object_put(response);
}

/* =============================================================================
 * Conversation History Handlers (Authenticated Users)
 * ============================================================================ */

/* Callback for conversation enumeration */
static int list_conv_callback(const conversation_t *conv, void *context) {
   json_object *conv_array = (json_object *)context;
   json_object *conv_obj = json_object_new_object();

   json_object_object_add(conv_obj, "id", json_object_new_int64(conv->id));
   json_object_object_add(conv_obj, "title", json_object_new_string(conv->title));
   json_object_object_add(conv_obj, "created_at", json_object_new_int64(conv->created_at));
   json_object_object_add(conv_obj, "updated_at", json_object_new_int64(conv->updated_at));
   json_object_object_add(conv_obj, "message_count", json_object_new_int(conv->message_count));
   json_object_object_add(conv_obj, "is_archived", json_object_new_boolean(conv->is_archived));

   /* Continuation indicator for history panel chain icon */
   if (conv->continued_from > 0) {
      json_object_object_add(conv_obj, "continued_from",
                             json_object_new_int64(conv->continued_from));
   }

   json_object_array_add(conv_array, conv_obj);
   return 0;
}

/**
 * @brief List conversations for the current user
 */
static void handle_list_conversations(ws_connection_t *conn, struct json_object *payload) {
   if (!conn_require_auth(conn)) {
      return;
   }

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("list_conversations_response"));
   json_object *resp_payload = json_object_new_object();
   json_object *conv_array = json_object_new_array();

   /* Parse pagination params if present */
   conv_pagination_t pagination = { 0, 0 };
   if (payload) {
      json_object *limit_obj, *offset_obj;
      if (json_object_object_get_ex(payload, "limit", &limit_obj)) {
         pagination.limit = json_object_get_int(limit_obj);
      }
      if (json_object_object_get_ex(payload, "offset", &offset_obj)) {
         pagination.offset = json_object_get_int(offset_obj);
      }
   }

   /* Include archived conversations so users can see the full chain */
   int result = conv_db_list(conn->auth_user_id, true, &pagination, list_conv_callback, conv_array);

   if (result == AUTH_DB_SUCCESS) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
      json_object_object_add(resp_payload, "conversations", conv_array);

      /* Include total count for pagination */
      int total = conv_db_count(conn->auth_user_id);
      json_object_object_add(resp_payload, "total", json_object_new_int(total >= 0 ? total : 0));
   } else {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Failed to list conversations"));
      json_object_put(conv_array);
   }

   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn->wsi, response);
   json_object_put(response);
}

/**
 * @brief Create a new conversation
 */
static void handle_new_conversation(ws_connection_t *conn, struct json_object *payload) {
   if (!conn_require_auth(conn)) {
      return;
   }

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("new_conversation_response"));
   json_object *resp_payload = json_object_new_object();

   /* Optional title from payload */
   const char *title = NULL;
   if (payload) {
      json_object *title_obj;
      if (json_object_object_get_ex(payload, "title", &title_obj)) {
         title = json_object_get_string(title_obj);
      }
   }

   int64_t conv_id;
   int result = conv_db_create(conn->auth_user_id, title, &conv_id);

   if (result == AUTH_DB_SUCCESS) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
      json_object_object_add(resp_payload, "conversation_id", json_object_new_int64(conv_id));

      auth_db_log_event("CONVERSATION_CREATED", conn->username, conn->client_ip,
                        "New conversation");
   } else if (result == AUTH_DB_LIMIT_EXCEEDED) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Maximum conversation limit reached"));
   } else {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Failed to create conversation"));
   }

   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn->wsi, response);
   json_object_put(response);
}

/**
 * @brief Continue a conversation (after context compaction)
 *
 * Archives the current conversation and creates a new one linked to it.
 * Called by the client when server notifies that context was compacted.
 */
static void handle_continue_conversation(ws_connection_t *conn, struct json_object *payload) {
   if (!conn_require_auth(conn)) {
      return;
   }

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type",
                          json_object_new_string("continue_conversation_response"));
   json_object *resp_payload = json_object_new_object();

   /* Get conversation ID */
   json_object *id_obj;
   if (!json_object_object_get_ex(payload, "conversation_id", &id_obj)) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Missing conversation_id"));
      json_object_object_add(response, "payload", resp_payload);
      send_json_response(conn->wsi, response);
      json_object_put(response);
      return;
   }

   int64_t old_conv_id = json_object_get_int64(id_obj);

   /* Get summary */
   const char *summary = "";
   json_object *summary_obj;
   if (json_object_object_get_ex(payload, "summary", &summary_obj)) {
      summary = json_object_get_string(summary_obj);
   }

   /* Create continuation */
   int64_t new_conv_id;
   int result = conv_db_create_continuation(conn->auth_user_id, old_conv_id, summary, &new_conv_id);

   if (result == AUTH_DB_SUCCESS) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
      json_object_object_add(resp_payload, "old_conversation_id",
                             json_object_new_int64(old_conv_id));
      json_object_object_add(resp_payload, "new_conversation_id",
                             json_object_new_int64(new_conv_id));
      json_object_object_add(resp_payload, "summary", json_object_new_string(summary));

      LOG_INFO("WebUI: Conversation %lld continued as %lld for user %s", (long long)old_conv_id,
               (long long)new_conv_id, conn->username);

      auth_db_log_event("CONVERSATION_CONTINUED", conn->username, conn->client_ip,
                        "Context compacted");
   } else if (result == AUTH_DB_NOT_FOUND) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Conversation not found"));
   } else if (result == AUTH_DB_FORBIDDEN) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error", json_object_new_string("Access denied"));
   } else {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Failed to continue conversation"));
   }

   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn->wsi, response);
   json_object_put(response);
}

/* Callback for message enumeration */
static int load_msg_callback(const conversation_message_t *msg, void *context) {
   json_object *msg_array = (json_object *)context;
   json_object *msg_obj = json_object_new_object();

   json_object_object_add(msg_obj, "role", json_object_new_string(msg->role));
   json_object_object_add(msg_obj, "content",
                          json_object_new_string(msg->content ? msg->content : ""));
   json_object_object_add(msg_obj, "created_at", json_object_new_int64(msg->created_at));

   json_object_array_add(msg_array, msg_obj);
   return 0;
}

/* Size-based chunking to stay under HTTP/2 frame size (~16KB).
 * Target 12KB to leave room for JSON envelope overhead. */
#define CHUNK_TARGET_SIZE 12288 /* 12KB target chunk size */
#define CHUNK_MSG_OVERHEAD \
   80 /* JSON overhead per message: {"role":"...","content":"...","created_at":N} */
#define CHUNK_ENVELOPE 256 /* Envelope overhead: {"type":"...","payload":{...}} */

/**
 * @brief Estimate serialized size of a message
 */
static size_t estimate_message_size(json_object *msg) {
   json_object *content_obj;
   if (json_object_object_get_ex(msg, "content", &content_obj)) {
      const char *content = json_object_get_string(content_obj);
      return (content ? strlen(content) : 0) + CHUNK_MSG_OVERHEAD;
   }
   return CHUNK_MSG_OVERHEAD;
}

/**
 * @brief Send a chunk of conversation messages
 */
static void send_messages_chunk(struct lws *wsi,
                                int64_t conv_id,
                                json_object *chunk,
                                int offset,
                                bool is_last) {
   json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("conversation_messages_chunk"));
   json_object *resp_payload = json_object_new_object();

   json_object_object_add(resp_payload, "conversation_id", json_object_new_int64(conv_id));
   json_object_object_add(resp_payload, "offset", json_object_new_int(offset));
   json_object_object_add(resp_payload, "is_last", json_object_new_boolean(is_last));
   json_object_object_add(resp_payload, "messages", chunk);

   json_object_object_add(response, "payload", resp_payload);
   send_json_response(wsi, response);
   json_object_put(response);
}

/**
 * @brief Load a conversation and its messages
 *
 * For large conversations, messages are sent in chunks to avoid HTTP/2 frame size limits.
 * Client receives: load_conversation_response (metadata) + conversation_messages_chunk(s)
 */
static void handle_load_conversation(ws_connection_t *conn, struct json_object *payload) {
   if (!conn_require_auth(conn)) {
      return;
   }

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("load_conversation_response"));
   json_object *resp_payload = json_object_new_object();

   /* Get conversation ID */
   json_object *id_obj;
   if (!json_object_object_get_ex(payload, "conversation_id", &id_obj)) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Missing conversation_id"));
      json_object_object_add(response, "payload", resp_payload);
      send_json_response(conn->wsi, response);
      json_object_put(response);
      return;
   }

   int64_t conv_id = json_object_get_int64(id_obj);

   /* Get conversation metadata */
   conversation_t conv;
   int result = conv_db_get(conv_id, conn->auth_user_id, &conv);

   if (result == AUTH_DB_SUCCESS) {
      json_object *msg_array = json_object_new_array();
      result = conv_db_get_messages(conv_id, conn->auth_user_id, load_msg_callback, msg_array);

      if (result == AUTH_DB_SUCCESS) {
         int total_messages = json_object_array_length(msg_array);

         /* Only restore to session context for non-archived conversations.
          * Archived conversations are read-only (view history only). */
         if (!conv.is_archived && conn->session && total_messages > 0) {
            /* Check if first message is a system prompt */
            bool has_system_prompt = false;
            json_object *first_msg = json_object_array_get_idx(msg_array, 0);
            if (first_msg) {
               json_object *role_obj;
               if (json_object_object_get_ex(first_msg, "role", &role_obj)) {
                  const char *role = json_object_get_string(role_obj);
                  if (role && strcmp(role, "system") == 0) {
                     has_system_prompt = true;
                  }
               }
            }

            session_clear_history(conn->session);

            /* If no system prompt in stored messages, add user's personalized prompt */
            if (!has_system_prompt) {
               char *prompt = build_user_prompt(conn->auth_user_id);
               session_add_message(conn->session, "system",
                                   prompt ? prompt : get_remote_command_prompt());
               free(prompt);
               LOG_INFO("WebUI: Added system prompt to restored conversation");
            }

            /* If this is a continuation conversation, inject the compaction summary
             * as a system message so the LLM has context from the previous conversation */
            if (conv.compaction_summary && strlen(conv.compaction_summary) > 0) {
               char summary_msg[4096];
               snprintf(summary_msg, sizeof(summary_msg),
                        "Previous conversation context (summarized): %s", conv.compaction_summary);
               session_add_message(conn->session, "system", summary_msg);
               LOG_INFO("WebUI: Injected compaction summary into session context");
            }

            /* Add all stored messages */
            for (int i = 0; i < total_messages; i++) {
               json_object *msg = json_object_array_get_idx(msg_array, i);
               json_object *role_obj, *content_obj;
               if (json_object_object_get_ex(msg, "role", &role_obj) &&
                   json_object_object_get_ex(msg, "content", &content_obj)) {
                  session_add_message(conn->session, json_object_get_string(role_obj),
                                      json_object_get_string(content_obj));
               }
            }
            LOG_INFO("WebUI: Restored %d messages to session %u context", total_messages,
                     conn->session->session_id);
         } else if (conv.is_archived) {
            LOG_INFO(
                "WebUI: Loaded archived conversation %lld (read-only, not restored to session)",
                (long long)conv.id);
         }

         json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
         json_object_object_add(resp_payload, "is_archived",
                                json_object_new_boolean(conv.is_archived));
         json_object_object_add(resp_payload, "conversation_id", json_object_new_int64(conv.id));
         json_object_object_add(resp_payload, "title", json_object_new_string(conv.title));
         json_object_object_add(resp_payload, "message_count", json_object_new_int(total_messages));
         json_object_object_add(resp_payload, "context_tokens",
                                json_object_new_int(conv.context_tokens));
         json_object_object_add(resp_payload, "context_max", json_object_new_int(conv.context_max));

         /* Continuation data for context banner */
         if (conv.continued_from > 0) {
            json_object_object_add(resp_payload, "continued_from",
                                   json_object_new_int64(conv.continued_from));
            if (conv.compaction_summary) {
               json_object_object_add(resp_payload, "compaction_summary",
                                      json_object_new_string(conv.compaction_summary));
            }
         }

         /* For archived conversations, find and include the continuation ID */
         if (conv.is_archived) {
            int64_t continuation_id = 0;
            if (conv_db_find_continuation(conv.id, conn->auth_user_id, &continuation_id) ==
                    AUTH_DB_SUCCESS &&
                continuation_id > 0) {
               json_object_object_add(resp_payload, "continued_by",
                                      json_object_new_int64(continuation_id));
            }
         }

         /* Calculate total size to decide if chunking is needed */
         size_t total_size = CHUNK_ENVELOPE;
         for (int i = 0; i < total_messages; i++) {
            total_size += estimate_message_size(json_object_array_get_idx(msg_array, i));
         }

         /* Small conversations: include all messages in single response */
         if (total_size <= CHUNK_TARGET_SIZE) {
            json_object_object_add(resp_payload, "messages", msg_array);
            json_object_object_add(resp_payload, "chunked", json_object_new_boolean(0));
            json_object_object_add(response, "payload", resp_payload);
            send_json_response(conn->wsi, response);
            json_object_put(response);
            conv_free(&conv);
            return;
         }

         /* Large conversation - send metadata first, then size-based chunks */
         json_object_object_add(resp_payload, "messages", json_object_new_array());
         json_object_object_add(resp_payload, "chunked", json_object_new_boolean(1));
         json_object_object_add(response, "payload", resp_payload);
         send_json_response(conn->wsi, response);
         json_object_put(response);

         /* Size-based chunking */
         json_object *current_chunk = json_object_new_array();
         size_t current_size = CHUNK_ENVELOPE;
         int chunk_start = 0;

         for (int i = 0; i < total_messages; i++) {
            json_object *msg = json_object_array_get_idx(msg_array, i);
            size_t msg_size = estimate_message_size(msg);

            /* If adding this message exceeds target AND chunk isn't empty, flush first */
            if (current_size + msg_size > CHUNK_TARGET_SIZE &&
                json_object_array_length(current_chunk) > 0) {
               /* send_messages_chunk takes ownership of chunk via json_object_object_add */
               send_messages_chunk(conn->wsi, conv_id, current_chunk, chunk_start, false);
               current_chunk = json_object_new_array();
               current_size = CHUNK_ENVELOPE;
               chunk_start = i;
            }

            /* Add message to current chunk (even if oversized - single msg gets its own chunk) */
            json_object_array_add(current_chunk, json_object_get(msg));
            current_size += msg_size;
         }

         /* Send final chunk (send_messages_chunk takes ownership) */
         if (json_object_array_length(current_chunk) > 0) {
            send_messages_chunk(conn->wsi, conv_id, current_chunk, chunk_start, true);
         } else {
            /* Empty final chunk - need to free since send_messages_chunk won't */
            json_object_put(current_chunk);
         }
         json_object_put(msg_array);

         conv_free(&conv);
         return; /* Already sent response */
      } else {
         json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
         json_object_object_add(resp_payload, "error",
                                json_object_new_string("Failed to load messages"));
         json_object_put(msg_array);
      }

      conv_free(&conv);
   } else if (result == AUTH_DB_NOT_FOUND) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Conversation not found"));
   } else if (result == AUTH_DB_FORBIDDEN) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error", json_object_new_string("Access denied"));
   } else {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Failed to load conversation"));
   }

   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn->wsi, response);
   json_object_put(response);
}

/**
 * @brief Delete a conversation
 */
static void handle_delete_conversation(ws_connection_t *conn, struct json_object *payload) {
   if (!conn_require_auth(conn)) {
      return;
   }

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("delete_conversation_response"));
   json_object *resp_payload = json_object_new_object();

   /* Get conversation ID */
   json_object *id_obj;
   if (!json_object_object_get_ex(payload, "conversation_id", &id_obj)) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Missing conversation_id"));
      json_object_object_add(response, "payload", resp_payload);
      send_json_response(conn->wsi, response);
      json_object_put(response);
      return;
   }

   int64_t conv_id = json_object_get_int64(id_obj);
   int result = conv_db_delete(conv_id, conn->auth_user_id);

   if (result == AUTH_DB_SUCCESS) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
      json_object_object_add(resp_payload, "message",
                             json_object_new_string("Conversation deleted"));

      char details[64];
      snprintf(details, sizeof(details), "Deleted conversation %lld", (long long)conv_id);
      auth_db_log_event("CONVERSATION_DELETED", conn->username, conn->client_ip, details);
   } else if (result == AUTH_DB_NOT_FOUND) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Conversation not found"));
   } else {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Failed to delete conversation"));
   }

   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn->wsi, response);
   json_object_put(response);
}

/**
 * @brief Rename a conversation
 */
static void handle_rename_conversation(ws_connection_t *conn, struct json_object *payload) {
   if (!conn_require_auth(conn)) {
      return;
   }

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("rename_conversation_response"));
   json_object *resp_payload = json_object_new_object();

   /* Get conversation ID and new title */
   json_object *id_obj, *title_obj;
   if (!json_object_object_get_ex(payload, "conversation_id", &id_obj) ||
       !json_object_object_get_ex(payload, "title", &title_obj)) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Missing conversation_id or title"));
      json_object_object_add(response, "payload", resp_payload);
      send_json_response(conn->wsi, response);
      json_object_put(response);
      return;
   }

   int64_t conv_id = json_object_get_int64(id_obj);
   const char *title = json_object_get_string(title_obj);

   if (!title || strlen(title) == 0) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Title cannot be empty"));
      json_object_object_add(response, "payload", resp_payload);
      send_json_response(conn->wsi, response);
      json_object_put(response);
      return;
   }

   int result = conv_db_rename(conv_id, conn->auth_user_id, title);

   if (result == AUTH_DB_SUCCESS) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
      json_object_object_add(resp_payload, "message",
                             json_object_new_string("Conversation renamed"));
   } else if (result == AUTH_DB_NOT_FOUND) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Conversation not found"));
   } else {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Failed to rename conversation"));
   }

   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn->wsi, response);
   json_object_put(response);
}

/**
 * @brief Search conversations by title or content
 */
static void handle_search_conversations(ws_connection_t *conn, struct json_object *payload) {
   if (!conn_require_auth(conn)) {
      return;
   }

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type",
                          json_object_new_string("search_conversations_response"));
   json_object *resp_payload = json_object_new_object();

   /* Get search query */
   json_object *query_obj;
   if (!json_object_object_get_ex(payload, "query", &query_obj)) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error", json_object_new_string("Missing query"));
      json_object_object_add(response, "payload", resp_payload);
      send_json_response(conn->wsi, response);
      json_object_put(response);
      return;
   }

   const char *query = json_object_get_string(query_obj);
   json_object *conv_array = json_object_new_array();

   /* Check if we should search message content */
   bool search_content = false;
   json_object *search_content_obj;
   if (json_object_object_get_ex(payload, "search_content", &search_content_obj)) {
      search_content = json_object_get_boolean(search_content_obj);
   }

   /* Parse pagination params */
   conv_pagination_t pagination = { 0, 0 };
   json_object *limit_obj, *offset_obj;
   if (json_object_object_get_ex(payload, "limit", &limit_obj)) {
      pagination.limit = json_object_get_int(limit_obj);
   }
   if (json_object_object_get_ex(payload, "offset", &offset_obj)) {
      pagination.offset = json_object_get_int(offset_obj);
   }

   int result;
   if (search_content) {
      result = conv_db_search_content(conn->auth_user_id, query, &pagination, list_conv_callback,
                                      conv_array);
   } else {
      result = conv_db_search(conn->auth_user_id, query, &pagination, list_conv_callback,
                              conv_array);
   }

   if (result == AUTH_DB_SUCCESS) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
      json_object_object_add(resp_payload, "conversations", conv_array);
   } else {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Failed to search conversations"));
      json_object_put(conv_array);
   }

   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn->wsi, response);
   json_object_put(response);
}

static void handle_save_message(ws_connection_t *conn, struct json_object *payload) {
   if (!conn_require_auth(conn)) {
      return;
   }

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("save_message_response"));
   json_object *resp_payload = json_object_new_object();

   /* Get required fields */
   json_object *conv_id_obj, *role_obj, *content_obj;
   if (!json_object_object_get_ex(payload, "conversation_id", &conv_id_obj) ||
       !json_object_object_get_ex(payload, "role", &role_obj) ||
       !json_object_object_get_ex(payload, "content", &content_obj)) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Missing conversation_id, role, or content"));
      json_object_object_add(response, "payload", resp_payload);
      send_json_response(conn->wsi, response);
      json_object_put(response);
      return;
   }

   int64_t conv_id = json_object_get_int64(conv_id_obj);
   const char *role = json_object_get_string(role_obj);
   const char *content = json_object_get_string(content_obj);

   int result = conv_db_add_message(conv_id, conn->auth_user_id, role, content);

   if (result == AUTH_DB_SUCCESS) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
   } else if (result == AUTH_DB_FORBIDDEN) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Access denied to conversation"));
   } else {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Failed to save message"));
   }

   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn->wsi, response);
   json_object_put(response);
}

/**
 * @brief Update context usage for a conversation
 */
static void handle_update_context(ws_connection_t *conn, struct json_object *payload) {
   if (!conn_require_auth(conn)) {
      return;
   }

   /* Get required fields */
   json_object *conv_id_obj, *tokens_obj, *max_obj;
   if (!json_object_object_get_ex(payload, "conversation_id", &conv_id_obj) ||
       !json_object_object_get_ex(payload, "context_tokens", &tokens_obj) ||
       !json_object_object_get_ex(payload, "context_max", &max_obj)) {
      /* Silently ignore incomplete updates - context is optional */
      return;
   }

   int64_t conv_id = json_object_get_int64(conv_id_obj);
   int context_tokens = json_object_get_int(tokens_obj);
   int context_max = json_object_get_int(max_obj);

   /* Update in database - no response needed */
   conv_db_update_context(conv_id, conn->auth_user_id, context_tokens, context_max);
}

static void handle_json_message(ws_connection_t *conn, const char *data, size_t len) {
   /* Null-terminate for JSON parsing */
   char *json_str = strndup(data, len);
   if (!json_str) {
      LOG_ERROR("WebUI: Failed to allocate JSON string");
      return;
   }

   struct json_object *root = json_tokener_parse(json_str);
   if (!root) {
      LOG_WARNING("WebUI: Invalid JSON received: %.*s", (int)len, data);
      free(json_str);
      return;
   }

   /* Get message type */
   struct json_object *type_obj;
   if (!json_object_object_get_ex(root, "type", &type_obj)) {
      LOG_WARNING("WebUI: JSON missing 'type' field");
      json_object_put(root);
      free(json_str);
      return;
   }

   const char *type = json_object_get_string(type_obj);
   struct json_object *payload;
   json_object_object_get_ex(root, "payload", &payload);

   if (strcmp(type, "text") == 0) {
      /* Text input from user */
      if (payload) {
         struct json_object *text_obj;
         if (json_object_object_get_ex(payload, "text", &text_obj)) {
            const char *text = json_object_get_string(text_obj);
            if (text && strlen(text) > 0) {
               handle_text_message(conn, text, strlen(text));
            }
         }
      }
   } else if (strcmp(type, "cancel") == 0) {
      /* Cancel current operation */
      handle_cancel_message(conn);
   } else if (strcmp(type, "get_config") == 0) {
      /* Request current configuration */
      handle_get_config(conn);
   } else if (strcmp(type, "get_system_prompt") == 0) {
      /* Request current system prompt for debugging */
      struct json_object *response = json_object_new_object();
      json_object_object_add(response, "type", json_object_new_string("system_prompt_response"));
      struct json_object *resp_payload = json_object_new_object();

      if (conn->session) {
         char *prompt = session_get_system_prompt(conn->session);
         if (prompt) {
            json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
            json_object_object_add(resp_payload, "prompt", json_object_new_string(prompt));
            json_object_object_add(resp_payload, "length",
                                   json_object_new_int((int)strlen(prompt)));
            free(prompt);
         } else {
            json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
            json_object_object_add(resp_payload, "error",
                                   json_object_new_string("No system prompt found"));
         }
      } else {
         json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
         json_object_object_add(resp_payload, "error", json_object_new_string("No active session"));
      }

      json_object_object_add(response, "payload", resp_payload);
      send_json_response(conn->wsi, response);
      json_object_put(response);
   } else if (strcmp(type, "set_config") == 0) {
      /* Update configuration settings */
      if (payload) {
         handle_set_config(conn, payload);
      }
   } else if (strcmp(type, "set_secrets") == 0) {
      /* Update secrets (API keys, credentials) */
      if (payload) {
         handle_set_secrets(conn, payload);
      }
   } else if (strcmp(type, "get_audio_devices") == 0) {
      /* Request available audio devices for given backend */
      handle_get_audio_devices(conn, payload);
   } else if (strcmp(type, "list_models") == 0) {
      /* Request available ASR and TTS models */
      handle_list_models(conn);
   } else if (strcmp(type, "list_interfaces") == 0) {
      /* Request available network interfaces */
      handle_list_interfaces(conn);
   } else if (strcmp(type, "restart") == 0) {
      /* Admin-only operation */
      if (!conn_require_admin(conn)) {
         return;
      }

      /* Request application restart */
      LOG_INFO("WebUI: Restart requested by client '%s'", conn->username);

      /* Send confirmation response before initiating restart */
      struct json_object *response = json_object_new_object();
      json_object_object_add(response, "type", json_object_new_string("restart_response"));
      struct json_object *resp_payload = json_object_new_object();
      json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
      json_object_object_add(resp_payload, "message",
                             json_object_new_string("DAWN is restarting..."));
      json_object_object_add(response, "payload", resp_payload);

      send_json_message(conn->wsi, json_object_to_json_string(response));
      json_object_put(response);

      /* Request restart - this will trigger clean shutdown and re-exec */
      dawn_request_restart();
   } else if (strcmp(type, "set_llm_runtime") == 0) {
      /* Admin-only: affects all clients */
      if (!conn_require_admin(conn)) {
         return;
      }
      /* Switch LLM type or provider at runtime (immediate effect, no restart) */
      struct json_object *response = json_object_new_object();
      json_object_object_add(response, "type", json_object_new_string("set_llm_runtime_response"));
      struct json_object *resp_payload = json_object_new_object();

      int success = 1;
      const char *error_msg = NULL;

      if (payload) {
         struct json_object *type_obj, *provider_obj;

         /* Handle LLM type change (local/cloud) */
         if (json_object_object_get_ex(payload, "type", &type_obj)) {
            const char *new_type = json_object_get_string(type_obj);
            if (new_type) {
               if (strcmp(new_type, "local") == 0) {
                  llm_set_type(LLM_LOCAL);
                  LOG_INFO("WebUI: Switched to local LLM");
               } else if (strcmp(new_type, "cloud") == 0) {
                  /* When switching to cloud, ensure we have a valid provider selected.
                   * Prefer OpenAI if available, otherwise Claude. */
                  if (llm_has_openai_key()) {
                     llm_set_cloud_provider(CLOUD_PROVIDER_OPENAI);
                  } else if (llm_has_claude_key()) {
                     llm_set_cloud_provider(CLOUD_PROVIDER_CLAUDE);
                  }
                  int rc = llm_set_type(LLM_CLOUD);
                  if (rc != 0) {
                     success = 0;
                     error_msg = "No cloud API key configured in secrets.toml";
                  } else {
                     LOG_INFO("WebUI: Switched to cloud LLM");
                  }
               }
            }
         }

         /* Handle cloud provider change (openai/claude) */
         if (success && json_object_object_get_ex(payload, "provider", &provider_obj)) {
            const char *new_provider = json_object_get_string(provider_obj);
            if (new_provider) {
               int rc = 0;
               if (strcmp(new_provider, "openai") == 0) {
                  rc = llm_set_cloud_provider(CLOUD_PROVIDER_OPENAI);
               } else if (strcmp(new_provider, "claude") == 0) {
                  rc = llm_set_cloud_provider(CLOUD_PROVIDER_CLAUDE);
               }
               if (rc != 0) {
                  success = 0;
                  error_msg = "API key not configured for this provider";
               } else {
                  LOG_INFO("WebUI: Switched cloud provider to %s", new_provider);
               }
            }
         }
      }

      json_object_object_add(resp_payload, "success", json_object_new_boolean(success));
      if (error_msg) {
         json_object_object_add(resp_payload, "error", json_object_new_string(error_msg));
      }

      /* Return updated runtime state */
      llm_type_t current_type = llm_get_type();
      json_object_object_add(resp_payload, "type",
                             json_object_new_string(current_type == LLM_LOCAL ? "local" : "cloud"));
      json_object_object_add(resp_payload, "provider",
                             json_object_new_string(llm_get_cloud_provider_name()));
      json_object_object_add(resp_payload, "model", json_object_new_string(llm_get_model_name()));

      /* Include API key availability so client can populate provider dropdown */
      json_object_object_add(resp_payload, "openai_available",
                             json_object_new_boolean(llm_has_openai_key()));
      json_object_object_add(resp_payload, "claude_available",
                             json_object_new_boolean(llm_has_claude_key()));

      json_object_object_add(response, "payload", resp_payload);
      send_json_response(conn->wsi, response);
      json_object_put(response);
   } else if (strcmp(type, "set_session_llm") == 0) {
      /* Per-session LLM configuration (does NOT affect other clients) */
      struct json_object *response = json_object_new_object();
      json_object_object_add(response, "type", json_object_new_string("set_session_llm_response"));
      struct json_object *resp_payload = json_object_new_object();

      int success = 1;
      const char *error_msg = NULL;

      if (!conn->session) {
         success = 0;
         error_msg = "No active session";
      } else if (payload) {
         struct json_object *type_obj, *provider_obj;

         /* Get current session config as starting point */
         session_llm_config_t config;
         session_get_llm_config(conn->session, &config);
         bool has_changes = false;

         /* Parse type (local/cloud) */
         if (json_object_object_get_ex(payload, "type", &type_obj)) {
            const char *new_type = json_object_get_string(type_obj);
            if (new_type) {
               has_changes = true;
               if (strcmp(new_type, "local") == 0) {
                  config.type = LLM_LOCAL;
               } else if (strcmp(new_type, "cloud") == 0) {
                  config.type = LLM_CLOUD;
                  /* If no provider is set, default to OpenAI (same as cloudLLMCallback) */
                  if (config.cloud_provider == CLOUD_PROVIDER_NONE) {
                     config.cloud_provider = CLOUD_PROVIDER_OPENAI;
                     LOG_INFO("WebUI: No cloud provider set, defaulting to OpenAI");
                  }
               } else if (strcmp(new_type, "reset") == 0) {
                  /* Reset to defaults from dawn.toml */
                  session_clear_llm_config(conn->session);
                  LOG_INFO("WebUI: Session %u LLM config reset to defaults",
                           conn->session->session_id);
                  has_changes = false; /* Already handled */
               }
            }
         }

         /* Parse provider (openai/claude) */
         if (json_object_object_get_ex(payload, "provider", &provider_obj)) {
            const char *new_provider = json_object_get_string(provider_obj);
            if (new_provider) {
               has_changes = true;
               if (strcmp(new_provider, "openai") == 0) {
                  config.cloud_provider = CLOUD_PROVIDER_OPENAI;
               } else if (strcmp(new_provider, "claude") == 0) {
                  config.cloud_provider = CLOUD_PROVIDER_CLAUDE;
               }
            }
         }

         /* Apply config if changes were made */
         if (has_changes) {
            int rc = session_set_llm_config(conn->session, &config);
            if (rc != 0) {
               success = 0;
               error_msg = "API key not configured for requested provider";
            } else {
               LOG_INFO("WebUI: Session %u LLM config updated (type=%d, provider=%d)",
                        conn->session->session_id, config.type, config.cloud_provider);
            }
         }
      }

      json_object_object_add(resp_payload, "success", json_object_new_boolean(success));
      if (error_msg) {
         json_object_object_add(resp_payload, "error", json_object_new_string(error_msg));
      }

      /* Return current session config for confirmation */
      if (conn->session) {
         session_llm_config_t current;
         session_get_llm_config(conn->session, &current);

         const char *type_str = current.type == LLM_LOCAL ? "local" : "cloud";
         const char *provider_str = current.cloud_provider == CLOUD_PROVIDER_OPENAI   ? "openai"
                                    : current.cloud_provider == CLOUD_PROVIDER_CLAUDE ? "claude"
                                                                                      : "none";
         json_object_object_add(resp_payload, "type", json_object_new_string(type_str));
         json_object_object_add(resp_payload, "provider", json_object_new_string(provider_str));
      }

      /* Include API key availability */
      json_object_object_add(resp_payload, "openai_available",
                             json_object_new_boolean(llm_has_openai_key()));
      json_object_object_add(resp_payload, "claude_available",
                             json_object_new_boolean(llm_has_claude_key()));

      json_object_object_add(response, "payload", resp_payload);
      send_json_response(conn->wsi, response);
      json_object_put(response);
   } else if (strcmp(type, "reconnect") == 0) {
      /* Session reconnection with stored token */
      if (payload) {
         struct json_object *token_obj;
         if (json_object_object_get_ex(payload, "token", &token_obj)) {
            const char *token = json_object_get_string(token_obj);
            if (token && strlen(token) > 0) {
               session_t *existing = lookup_session_by_token(token);
               if (existing) {
                  /* Found existing session - switch to it */
                  if (conn->session && conn->session != existing) {
                     /* Destroy the abandoned session (just created on connect) */
                     uint32_t abandoned_id = conn->session->session_id;
                     conn->session->client_data = NULL;
                     /* Release ref_count (was 1 from creation) before destroy */
                     session_release(conn->session);
                     session_destroy(abandoned_id);
                     LOG_INFO("WebUI: Destroyed abandoned session %u", abandoned_id);
                  }
                  conn->session = existing;
                  existing->client_data = conn;
                  existing->disconnected = false;
                  strncpy(conn->session_token, token, WEBUI_SESSION_TOKEN_LEN - 1);
                  conn->session_token[WEBUI_SESSION_TOKEN_LEN - 1] = '\0';

                  LOG_INFO("WebUI: Reconnected to session %u with token %.8s...",
                           existing->session_id, token);

                  /* Send confirmation, config, history replay, and current state */
                  send_session_token_impl(conn, token);
                  send_config_impl(conn->wsi);
                  send_history_impl(conn->wsi, existing);
                  send_state_impl(conn->wsi, "idle", NULL);
               } else {
                  /* Token not found or session expired - create new session */
                  LOG_INFO("WebUI: Token %.8s... not found, creating new session", token);
                  if (!conn->session) {
                     conn->session = session_create(SESSION_TYPE_WEBSOCKET, -1);
                     if (conn->session) {
                        /* Build personalized prompt with user settings */
                        char *prompt = build_user_prompt(conn->auth_user_id);
                        session_init_system_prompt(conn->session,
                                                   prompt ? prompt : get_remote_command_prompt());
                        free(prompt);
                        conn->session->client_data = conn;
                        if (generate_session_token(conn->session_token) != 0) {
                           LOG_ERROR("WebUI: Failed to generate session token");
                           session_destroy(conn->session->session_id);
                           conn->session = NULL;
                           return;
                        }
                        register_token(conn->session_token, conn->session->session_id);
                        send_session_token_impl(conn, conn->session_token);
                        send_config_impl(conn->wsi);
                        send_state_impl(conn->wsi, "idle", NULL);
                     }
                  }
               }
            }
         }
      }
   } else if (strcmp(type, "capabilities_update") == 0) {
      /* Client capability update (e.g., Opus codec became available after connect) */
      if (payload && conn->session) {
         conn->use_opus = check_opus_capability(payload);
         LOG_INFO("WebUI: Session %u capabilities updated (opus: %s)", conn->session->session_id,
                  conn->use_opus ? "yes" : "no");
      } else if (payload) {
         /* Session not yet created - just store capability, session will read it later */
         conn->use_opus = check_opus_capability(payload);
         LOG_INFO("WebUI: Connection capabilities updated before session (opus: %s)",
                  conn->use_opus ? "yes" : "no");
      }
   } else if (strcmp(type, "smartthings_status") == 0) {
      /* Get SmartThings connection status */
      struct json_object *response = json_object_new_object();
      json_object_object_add(response, "type",
                             json_object_new_string("smartthings_status_response"));
      struct json_object *resp_payload = json_object_new_object();

      json_object_object_add(resp_payload, "configured",
                             json_object_new_boolean(smartthings_is_configured()));
      json_object_object_add(resp_payload, "authenticated",
                             json_object_new_boolean(smartthings_is_authenticated()));

      if (smartthings_is_configured()) {
         st_status_t status;
         smartthings_get_status(&status);
         json_object_object_add(resp_payload, "has_tokens",
                                json_object_new_boolean(status.has_tokens));
         json_object_object_add(resp_payload, "tokens_valid",
                                json_object_new_boolean(status.tokens_valid));
         json_object_object_add(resp_payload, "token_expiry",
                                json_object_new_int64(status.token_expiry));
         json_object_object_add(resp_payload, "devices_count",
                                json_object_new_int(status.devices_count));
         json_object_object_add(resp_payload, "auth_mode",
                                json_object_new_string(
                                    smartthings_auth_mode_str(status.auth_mode)));
      }

      json_object_object_add(response, "payload", resp_payload);
      send_json_response(conn->wsi, response);
      json_object_put(response);
   } else if (strcmp(type, "smartthings_get_auth_url") == 0) {
      /* Admin-only: SmartThings configuration */
      if (!conn_require_admin(conn)) {
         return;
      }
      /* Get OAuth authorization URL */
      struct json_object *response = json_object_new_object();
      json_object_object_add(response, "type",
                             json_object_new_string("smartthings_auth_url_response"));
      struct json_object *resp_payload = json_object_new_object();

      if (!smartthings_is_configured()) {
         json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
         json_object_object_add(resp_payload, "error",
                                json_object_new_string(
                                    "SmartThings client credentials not configured"));
      } else {
         /* Build redirect URI from current WebUI URL */
         char redirect_uri[256];
         const dawn_config_t *cfg = config_get();
         snprintf(redirect_uri, sizeof(redirect_uri), "%s://localhost:%d/smartthings/callback",
                  cfg->webui.https ? "https" : "http", cfg->webui.port);

         char auth_url[1024];
         st_error_t err = smartthings_get_auth_url(redirect_uri, auth_url, sizeof(auth_url));
         if (err == ST_OK) {
            json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
            json_object_object_add(resp_payload, "auth_url", json_object_new_string(auth_url));
            json_object_object_add(resp_payload, "redirect_uri",
                                   json_object_new_string(redirect_uri));
         } else {
            json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
            json_object_object_add(resp_payload, "error",
                                   json_object_new_string(smartthings_error_str(err)));
         }
      }

      json_object_object_add(response, "payload", resp_payload);
      send_json_response(conn->wsi, response);
      json_object_put(response);
   } else if (strcmp(type, "smartthings_exchange_code") == 0) {
      /* Admin-only: SmartThings configuration */
      if (!conn_require_admin(conn)) {
         return;
      }
      /* Exchange OAuth authorization code for tokens */
      struct json_object *response = json_object_new_object();
      json_object_object_add(response, "type",
                             json_object_new_string("smartthings_exchange_response"));
      struct json_object *resp_payload = json_object_new_object();

      struct json_object *code_obj, *redirect_obj, *state_obj;
      if (payload && json_object_object_get_ex(payload, "code", &code_obj) &&
          json_object_object_get_ex(payload, "redirect_uri", &redirect_obj)) {
         const char *code = json_object_get_string(code_obj);
         const char *redirect_uri = json_object_get_string(redirect_obj);
         const char *state = NULL;
         if (json_object_object_get_ex(payload, "state", &state_obj)) {
            state = json_object_get_string(state_obj);
         }

         st_error_t err = smartthings_exchange_code(code, redirect_uri, state);
         if (err == ST_OK) {
            json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
            LOG_INFO("WebUI: SmartThings OAuth authorization successful");
         } else {
            json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
            json_object_object_add(resp_payload, "error",
                                   json_object_new_string(smartthings_error_str(err)));
            LOG_WARNING("WebUI: SmartThings OAuth failed: %s", smartthings_error_str(err));
         }
      } else {
         json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
         json_object_object_add(resp_payload, "error",
                                json_object_new_string("Missing code or redirect_uri"));
      }

      json_object_object_add(response, "payload", resp_payload);
      send_json_response(conn->wsi, response);
      json_object_put(response);
   } else if (strcmp(type, "smartthings_disconnect") == 0) {
      /* Admin-only: SmartThings configuration */
      if (!conn_require_admin(conn)) {
         return;
      }
      /* Disconnect (clear tokens) */
      struct json_object *response = json_object_new_object();
      json_object_object_add(response, "type",
                             json_object_new_string("smartthings_disconnect_response"));
      struct json_object *resp_payload = json_object_new_object();

      smartthings_disconnect();
      json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
      LOG_INFO("WebUI: SmartThings disconnected");

      json_object_object_add(response, "payload", resp_payload);
      send_json_response(conn->wsi, response);
      json_object_put(response);
   } else if (strcmp(type, "smartthings_list_devices") == 0) {
      /* Admin-only: SmartThings device access */
      if (!conn_require_admin(conn)) {
         return;
      }
      /* List all SmartThings devices */
      struct json_object *response = json_object_new_object();
      json_object_object_add(response, "type",
                             json_object_new_string("smartthings_devices_response"));
      struct json_object *resp_payload = json_object_new_object();

      if (!smartthings_is_authenticated()) {
         json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
         json_object_object_add(resp_payload, "error", json_object_new_string("Not authenticated"));
      } else {
         const st_device_list_t *devices;
         st_error_t err = smartthings_list_devices(&devices);
         if (err == ST_OK) {
            json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
            json_object_object_add(resp_payload, "count", json_object_new_int(devices->count));

            struct json_object *devices_arr = json_object_new_array();
            for (int i = 0; i < devices->count; i++) {
               const st_device_t *dev = &devices->devices[i];
               struct json_object *dev_obj = json_object_new_object();
               json_object_object_add(dev_obj, "id", json_object_new_string(dev->id));
               json_object_object_add(dev_obj, "name", json_object_new_string(dev->name));
               json_object_object_add(dev_obj, "label", json_object_new_string(dev->label));
               json_object_object_add(dev_obj, "room", json_object_new_string(dev->room));

               /* Build capabilities array */
               struct json_object *caps = json_object_new_array();
               for (int j = 0; j < 15; j++) {
                  st_capability_t cap = (st_capability_t)(1 << j);
                  if (dev->capabilities & cap) {
                     json_object_array_add(caps,
                                           json_object_new_string(smartthings_capability_str(cap)));
                  }
               }
               json_object_object_add(dev_obj, "capabilities", caps);

               json_object_array_add(devices_arr, dev_obj);
            }
            json_object_object_add(resp_payload, "devices", devices_arr);
         } else {
            json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
            json_object_object_add(resp_payload, "error",
                                   json_object_new_string(smartthings_error_str(err)));
         }
      }

      json_object_object_add(response, "payload", resp_payload);
      send_json_response(conn->wsi, response);
      json_object_put(response);
   } else if (strcmp(type, "smartthings_refresh_devices") == 0) {
      /* Admin-only: SmartThings device access */
      if (!conn_require_admin(conn)) {
         return;
      }
      /* Force refresh device list */
      struct json_object *response = json_object_new_object();
      json_object_object_add(response, "type",
                             json_object_new_string("smartthings_devices_response"));
      struct json_object *resp_payload = json_object_new_object();

      if (!smartthings_is_authenticated()) {
         json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
         json_object_object_add(resp_payload, "error", json_object_new_string("Not authenticated"));
      } else {
         const st_device_list_t *devices;
         st_error_t err = smartthings_refresh_devices(&devices);
         if (err == ST_OK) {
            json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
            json_object_object_add(resp_payload, "count", json_object_new_int(devices->count));

            struct json_object *devices_arr = json_object_new_array();
            for (int i = 0; i < devices->count; i++) {
               const st_device_t *dev = &devices->devices[i];
               struct json_object *dev_obj = json_object_new_object();
               json_object_object_add(dev_obj, "id", json_object_new_string(dev->id));
               json_object_object_add(dev_obj, "name", json_object_new_string(dev->name));
               json_object_object_add(dev_obj, "label", json_object_new_string(dev->label));
               json_object_object_add(dev_obj, "room", json_object_new_string(dev->room));

               struct json_object *caps = json_object_new_array();
               for (int j = 0; j < 15; j++) {
                  st_capability_t cap = (st_capability_t)(1 << j);
                  if (dev->capabilities & cap) {
                     json_object_array_add(caps,
                                           json_object_new_string(smartthings_capability_str(cap)));
                  }
               }
               json_object_object_add(dev_obj, "capabilities", caps);

               json_object_array_add(devices_arr, dev_obj);
            }
            json_object_object_add(resp_payload, "devices", devices_arr);
         } else {
            json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
            json_object_object_add(resp_payload, "error",
                                   json_object_new_string(smartthings_error_str(err)));
         }
      }

      json_object_object_add(response, "payload", resp_payload);
      send_json_response(conn->wsi, response);
      json_object_put(response);
   } else if (strcmp(type, "get_tools_config") == 0) {
      handle_get_tools_config(conn);
   } else if (strcmp(type, "set_tools_config") == 0) {
      if (payload) {
         handle_set_tools_config(conn, payload);
      }
   } else if (strcmp(type, "get_metrics") == 0) {
      handle_get_metrics(conn);
   }
   /* User management (admin only) */
   else if (strcmp(type, "list_users") == 0) {
      handle_list_users(conn);
   } else if (strcmp(type, "create_user") == 0) {
      if (payload) {
         handle_create_user(conn, payload);
      }
   } else if (strcmp(type, "delete_user") == 0) {
      if (payload) {
         handle_delete_user(conn, payload);
      }
   } else if (strcmp(type, "change_password") == 0) {
      if (payload) {
         handle_change_password(conn, payload);
      }
   } else if (strcmp(type, "unlock_user") == 0) {
      if (payload) {
         handle_unlock_user(conn, payload);
      }
   }
   /* Personal settings (authenticated users) */
   else if (strcmp(type, "get_my_settings") == 0) {
      handle_get_my_settings(conn);
   } else if (strcmp(type, "set_my_settings") == 0) {
      if (payload) {
         handle_set_my_settings(conn, payload);
      }
   }
   /* Session management (authenticated users) */
   else if (strcmp(type, "list_my_sessions") == 0) {
      handle_list_my_sessions(conn);
   } else if (strcmp(type, "revoke_session") == 0) {
      if (payload) {
         handle_revoke_session(conn, payload);
      }
   }
   /* Conversation history (authenticated users) */
   else if (strcmp(type, "list_conversations") == 0) {
      handle_list_conversations(conn, payload);
   } else if (strcmp(type, "new_conversation") == 0) {
      handle_new_conversation(conn, payload);
   } else if (strcmp(type, "load_conversation") == 0) {
      if (payload) {
         handle_load_conversation(conn, payload);
      }
   } else if (strcmp(type, "delete_conversation") == 0) {
      if (payload) {
         handle_delete_conversation(conn, payload);
      }
   } else if (strcmp(type, "rename_conversation") == 0) {
      if (payload) {
         handle_rename_conversation(conn, payload);
      }
   } else if (strcmp(type, "search_conversations") == 0) {
      if (payload) {
         handle_search_conversations(conn, payload);
      }
   } else if (strcmp(type, "save_message") == 0) {
      if (payload) {
         handle_save_message(conn, payload);
      }
   } else if (strcmp(type, "update_context") == 0) {
      if (payload) {
         handle_update_context(conn, payload);
      }
   } else if (strcmp(type, "continue_conversation") == 0) {
      if (payload) {
         handle_continue_conversation(conn, payload);
      }
   } else {
      LOG_WARNING("WebUI: Unknown message type: %s", type);
   }

   json_object_put(root);
   free(json_str);
}

static void handle_cancel_message(ws_connection_t *conn) {
   if (conn->session) {
      LOG_INFO("WebUI: Cancel requested for session %u", conn->session->session_id);
      conn->session->disconnected = true; /* Signal worker to abort */
      send_state_impl(conn->wsi, "idle", NULL);
   }
}

/* =============================================================================
 * WebSocket Protocol Callbacks
 * ============================================================================= */

static int callback_websocket(struct lws *wsi,
                              enum lws_callback_reasons reason,
                              void *user,
                              void *in,
                              size_t len) {
   ws_connection_t *conn = (ws_connection_t *)user;

   switch (reason) {
      case LWS_CALLBACK_ESTABLISHED: {
         /*
          * New WebSocket connection - defer session creation until init message.
          * This allows reconnecting clients (with valid token) to reuse their
          * existing session without counting against max_clients during the
          * brief overlap when refreshing a browser page.
          */
         memset(conn, 0, sizeof(*conn));
         conn->wsi = wsi;
         conn->session = NULL; /* Will be created/assigned on init message */

         /* Capture client IP at connection time for reliable logging later */
         lws_get_peer_simple(wsi, conn->client_ip, sizeof(conn->client_ip));
         if (conn->client_ip[0] == '\0') {
            strncpy(conn->client_ip, "(unknown)", sizeof(conn->client_ip) - 1);
         }

         /* Populate auth state from HTTP cookie (if present) */
         auth_session_t auth_session;
         if (is_request_authenticated(wsi, &auth_session)) {
            conn->authenticated = true;
            conn->auth_user_id = auth_session.user_id;
            strncpy(conn->auth_session_token, auth_session.token,
                    sizeof(conn->auth_session_token) - 1);
            conn->auth_session_token[sizeof(conn->auth_session_token) - 1] = '\0';
            strncpy(conn->username, auth_session.username, sizeof(conn->username) - 1);
            conn->username[sizeof(conn->username) - 1] = '\0';
            LOG_INFO("WebUI: WebSocket authenticated as user '%s' (id=%d)", conn->username,
                     conn->auth_user_id);
         } else {
            LOG_INFO("WebUI: WebSocket connection established (unauthenticated)");
         }

         LOG_INFO("WebUI: WebSocket connection established, awaiting init message");
         break;
      }

      case LWS_CALLBACK_CLOSED: {
         /* WebSocket disconnected */
         LOG_INFO("WebUI: WebSocket client disconnecting (session %u)",
                  conn->session ? conn->session->session_id : 0);

         bool had_session = (conn->session != NULL);

         if (conn->session) {
            /* Mark session as disconnected (aborts any pending LLM calls) */
            conn->session->disconnected = true;
            conn->session->client_data = NULL;

            /* Release our reference to the session.
             * Session manager will clean it up when ref_count reaches 0. */
            LOG_INFO("WebUI: Releasing session reference...");
            session_release(conn->session);
            LOG_INFO("WebUI: Session reference released");
            conn->session = NULL;
         }

         /* Clear wsi reference */
         conn->wsi = NULL;

         /* Free any pending audio buffer */
         if (conn->audio_buffer) {
            free(conn->audio_buffer);
            conn->audio_buffer = NULL;
         }

         /* Only decrement client count if this connection had a session
          * (i.e., completed the init handshake) */
         if (had_session) {
            LOG_INFO("WebUI: Acquiring s_mutex for client count...");
            pthread_mutex_lock(&s_mutex);
            if (s_client_count > 0) {
               s_client_count--;
            }
            pthread_mutex_unlock(&s_mutex);
            LOG_INFO("WebUI: s_mutex released");
         }

         LOG_INFO("WebUI: WebSocket client disconnected (total: %d)", s_client_count);
         break;
      }

      case LWS_CALLBACK_RECEIVE: {
         /* Message received from client */
         if (!conn->session) {
            /*
             * No session yet - this must be the init/reconnect message.
             * Parse it to determine if we're reconnecting with a token or
             * need a new session. Check client limits here.
             */
            char *json_str = strndup((const char *)in, len);
            if (!json_str) {
               LOG_ERROR("WebUI: Failed to allocate init message buffer");
               return -1;
            }

            struct json_object *root = json_tokener_parse(json_str);
            free(json_str);

            if (!root) {
               LOG_WARNING("WebUI: Invalid JSON in init message");
               return -1;
            }

            struct json_object *type_obj;
            const char *type = NULL;
            if (json_object_object_get_ex(root, "type", &type_obj)) {
               type = json_object_get_string(type_obj);
            }

            struct json_object *payload = NULL;
            json_object_object_get_ex(root, "payload", &payload);

            bool is_reconnect = false;
            session_t *existing_session = NULL;

            /* Check for reconnection token */
            if (type && strcmp(type, "reconnect") == 0 && payload) {
               struct json_object *token_obj;
               if (json_object_object_get_ex(payload, "token", &token_obj)) {
                  const char *token = json_object_get_string(token_obj);
                  if (token && strlen(token) > 0) {
                     existing_session = lookup_session_by_token(token);
                     if (existing_session) {
                        is_reconnect = true;
                        conn->session = existing_session;
                        existing_session->client_data = conn;
                        existing_session->disconnected = false;
                        strncpy(conn->session_token, token, WEBUI_SESSION_TOKEN_LEN - 1);
                        conn->session_token[WEBUI_SESSION_TOKEN_LEN - 1] = '\0';

                        /* Check for Opus codec support */
                        conn->use_opus = check_opus_capability(payload);

                        /* Reconnections still count against client limit */
                        pthread_mutex_lock(&s_mutex);
                        s_client_count++;
                        pthread_mutex_unlock(&s_mutex);

                        LOG_INFO("WebUI: Reconnected to session %u with token %.8s... (total: %d, "
                                 "opus: %s)",
                                 existing_session->session_id, token, s_client_count,
                                 conn->use_opus ? "yes" : "no");

                        /* Send confirmation */
                        send_session_token_impl(conn, token);
                        send_config_impl(conn->wsi);
                        send_history_impl(conn->wsi, existing_session);
                        send_state_impl(conn->wsi, "idle", NULL);
                     }
                  }
               }
            }

            /* If not reconnecting, create new session (with client limit check) */
            if (!is_reconnect) {
               pthread_mutex_lock(&s_mutex);
               if (s_client_count >= g_config.webui.max_clients) {
                  pthread_mutex_unlock(&s_mutex);
                  LOG_WARNING("WebUI: Connection rejected - max clients reached (%d)",
                              g_config.webui.max_clients);
                  send_error_impl(wsi, "MAX_CLIENTS",
                                  "Maximum WebUI clients reached. Please try again later.");
                  json_object_put(root);
                  return -1;
               }
               s_client_count++;
               pthread_mutex_unlock(&s_mutex);

               conn->session = session_create(SESSION_TYPE_WEBSOCKET, -1);
               if (!conn->session) {
                  LOG_ERROR("WebUI: Failed to create session");
                  send_error_impl(wsi, "SESSION_LIMIT", "Maximum sessions reached");
                  pthread_mutex_lock(&s_mutex);
                  s_client_count--;
                  pthread_mutex_unlock(&s_mutex);
                  json_object_put(root);
                  return -1;
               }

               /* Build personalized prompt with user settings */
               char *prompt = build_user_prompt(conn->auth_user_id);
               session_init_system_prompt(conn->session,
                                          prompt ? prompt : get_remote_command_prompt());
               free(prompt);
               conn->session->client_data = conn;

               /* Check for Opus codec support */
               conn->use_opus = check_opus_capability(payload);

               if (generate_session_token(conn->session_token) != 0) {
                  LOG_ERROR("WebUI: Failed to generate session token");
                  session_destroy(conn->session->session_id);
                  conn->session = NULL;
                  pthread_mutex_lock(&s_mutex);
                  s_client_count--;
                  pthread_mutex_unlock(&s_mutex);
                  json_object_put(root);
                  return -1;
               }
               register_token(conn->session_token, conn->session->session_id);

               LOG_INFO("WebUI: New session %u created (token %.8s..., total: %d, opus: %s)",
                        conn->session->session_id, conn->session_token, s_client_count,
                        conn->use_opus ? "yes" : "no");

               send_session_token_impl(conn, conn->session_token);
               send_config_impl(conn->wsi);
               send_state_impl(conn->wsi, "idle", NULL);
            }

            json_object_put(root);
            break; /* Don't process this message further - it was just the init */
         }

         session_touch(conn->session);

         int is_final = lws_is_final_fragment(wsi);
         int is_binary = lws_frame_is_binary(wsi);

         if (is_binary) {
#ifdef ENABLE_WEBUI_AUDIO
            /* Handle WebSocket frame fragmentation for binary messages */
            if (conn->in_binary_fragment) {
               /* Continuation of a fragmented message - append ALL bytes as payload */
               const uint8_t *data = (const uint8_t *)in;
               size_t data_len = len;

               if (conn->binary_msg_type == WS_BIN_AUDIO_IN && data_len > 0) {
                  /* Append continuation data to audio buffer */
                  if (conn->audio_buffer &&
                      conn->audio_buffer_len + data_len <= conn->audio_buffer_capacity) {
                     memcpy(conn->audio_buffer + conn->audio_buffer_len, data, data_len);
                     conn->audio_buffer_len += data_len;
                     LOG_INFO("WebUI: Fragment continuation, added %zu bytes (total: %zu)",
                              data_len, conn->audio_buffer_len);
                  }
               }

               /* Check if this is the final fragment */
               if (is_final) {
                  conn->in_binary_fragment = false;
               }
            } else {
               /* New message - parse type byte and handle */
               handle_binary_message(conn, (const uint8_t *)in, len);

               /* If not final, track that we're in a fragmented message */
               if (!is_final && len > 0) {
                  conn->in_binary_fragment = true;
                  conn->binary_msg_type = ((const uint8_t *)in)[0];
               }
            }
#else
            LOG_WARNING("WebUI: Audio not enabled, ignoring binary message (%zu bytes)", len);
#endif
         } else {
            /* Text message (JSON control) */
            handle_json_message(conn, (const char *)in, len);
         }
         break;
      }

      case LWS_CALLBACK_SERVER_WRITEABLE:
         /* Ready to send more data - process next queued response */
         process_one_response();
         break;

      case LWS_CALLBACK_EVENT_WAIT_CANCELLED:
         /* lws_cancel_service() was called - process response queue */
         process_response_queue();
         break;

      default:
         break;
   }

   return 0;
}

/* =============================================================================
 * Protocol Definitions
 * ============================================================================= */

static struct lws_protocols s_protocols[] = {
   /* HTTP protocol (must be first) */
   {
       .name = "http",
       .callback = callback_http,
       .per_session_data_size = sizeof(struct http_session_data),
       .rx_buffer_size = 0,
   },
   /* WebSocket protocol */
   {
       .name = WEBUI_SUBPROTOCOL,
       .callback = callback_websocket,
       .per_session_data_size = sizeof(ws_connection_t),
       .rx_buffer_size = 8192, /* Match DAP packet size */
   },
   /* Terminator */
   { NULL, NULL, 0, 0 },
};

/* =============================================================================
 * Server Thread
 * ============================================================================= */

static void *webui_thread_func(void *arg) {
   (void)arg;

   LOG_INFO("WebUI: Server thread started");

   while (s_running) {
      /* Process events with 50ms timeout */
      lws_service(s_lws_context, 50);

      /* Process any pending responses from worker threads */
      process_response_queue();
   }

   LOG_INFO("WebUI: Server thread exiting");
   return NULL;
}

/* =============================================================================
 * Tool Execution Callback (for debug display)
 * ============================================================================= */

/**
 * @brief Callback for native tool execution notifications
 *
 * Sends tool call/result information to the WebUI for debug display.
 * Called by llm_tools module when tools are executed.
 */
/**
 * @brief Send LLM state update to WebSocket client
 *
 * Pushes the current LLM configuration to the client so it can update
 * the UI controls dynamically (e.g., after switch_llm tool call).
 */
static void webui_send_llm_state_update(session_t *session) {
   if (!session || session->type != SESSION_TYPE_WEBSOCKET) {
      return;
   }

   /* Get session's current LLM config */
   session_llm_config_t config;
   session_get_llm_config(session, &config);

   /* Build JSON response */
   json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("llm_state_update"));

   json_object *payload = json_object_new_object();
   json_object_object_add(payload, "success", json_object_new_boolean(1));

   const char *type_str = config.type == LLM_LOCAL ? "local" : "cloud";
   const char *provider_str = config.cloud_provider == CLOUD_PROVIDER_OPENAI   ? "openai"
                              : config.cloud_provider == CLOUD_PROVIDER_CLAUDE ? "claude"
                                                                               : "none";

   json_object_object_add(payload, "type", json_object_new_string(type_str));
   json_object_object_add(payload, "provider", json_object_new_string(provider_str));
   json_object_object_add(payload, "model", json_object_new_string(config.model));
   json_object_object_add(payload, "openai_available",
                          json_object_new_boolean(llm_has_openai_key()));
   json_object_object_add(payload, "claude_available",
                          json_object_new_boolean(llm_has_claude_key()));

   json_object_object_add(response, "payload", payload);

   /* Queue response for WebSocket send */
   ws_response_t resp = { .session = session,
                          .type = WS_RESP_TRANSCRIPT,
                          .transcript = { .role = strdup("__llm_state__"),
                                          .text = strdup(json_object_to_json_string(response)) } };

   json_object_put(response);

   if (resp.transcript.role && resp.transcript.text) {
      queue_response(&resp);
   } else {
      free(resp.transcript.role);
      free(resp.transcript.text);
   }
}

static void webui_tool_execution_callback(void *session_ptr,
                                          const char *tool_name,
                                          const char *tool_args,
                                          const char *result,
                                          bool success) {
   session_t *session = (session_t *)session_ptr;
   if (!session || session->type != SESSION_TYPE_WEBSOCKET) {
      return;
   }

   /* result==NULL indicates tool execution is starting, not ending */
   if (result == NULL) {
      /* Tool is starting - switch to "thinking" state so UI doesn't show "speaking"
       * while no audio is playing. This is especially important for slow tools. */
      char detail[64];
      snprintf(detail, sizeof(detail), "Calling %s...", tool_name);
      webui_send_state_with_detail(session, "thinking", detail);
      return;
   }

   /* Tool execution completed - format as debug entry */
   char debug_msg[LLM_TOOLS_RESULT_LEN + 256];
   snprintf(debug_msg, sizeof(debug_msg), "[Tool Call: %s(%s) -> %s%s]", tool_name,
            tool_args ? tool_args : "", success ? "" : "FAILED: ", result);

   webui_send_transcript(session, "assistant", debug_msg);

   /* If this was a switch_llm call, send LLM state update to client */
   if (success && strcmp(tool_name, "switch_llm") == 0) {
      webui_send_llm_state_update(session);
   }
}

/* =============================================================================
 * Public API
 * ============================================================================= */

int webui_server_init(int port, const char *www_path) {
   struct lws_context_creation_info info;

   pthread_mutex_lock(&s_mutex);
   if (s_running) {
      pthread_mutex_unlock(&s_mutex);
      LOG_WARNING("WebUI: Server already running");
      return WEBUI_ERROR_ALREADY_RUNNING;
   }
   pthread_mutex_unlock(&s_mutex);

   /* Determine port */
   if (port <= 0) {
      port = g_config.webui.port;
      if (port <= 0) {
         port = WEBUI_DEFAULT_PORT;
      }
   }

   /* Determine www path */
   if (www_path && www_path[0] != '\0') {
      strncpy(s_www_path, www_path, sizeof(s_www_path) - 1);
   } else if (g_config.webui.www_path[0] != '\0') {
      strncpy(s_www_path, g_config.webui.www_path, sizeof(s_www_path) - 1);
   } else {
      strncpy(s_www_path, WEBUI_DEFAULT_WWW_PATH, sizeof(s_www_path) - 1);
   }
   s_www_path[sizeof(s_www_path) - 1] = '\0';

   /* Configure libwebsockets context */
   memset(&info, 0, sizeof(info));
   info.port = port;
   info.protocols = s_protocols;
   info.gid = -1;
   info.uid = -1;
   /* Increase service buffer for large WebSocket messages (conversation history).
    * Default is ~4KB which causes OVERSIZED_PAYLOAD errors on HTTP/2 connections. */
   info.pt_serv_buf_size = 128 * 1024; /* 128KB - enough for large conversation loads */
   /* Note: Not using LWS_SERVER_OPTION_HTTP_HEADERS_SECURITY_BEST_PRACTICES_ENFORCE
    * because it sets CSP: default-src 'none' which blocks WebAssembly for Opus codec.
    * Security headers are added via index.html meta tags instead. */
   info.options = 0;

   /* Configure HTTPS if enabled */
   bool use_https = g_config.webui.https;
   if (use_https) {
      if (g_config.webui.ssl_cert_path[0] == '\0' || g_config.webui.ssl_key_path[0] == '\0') {
         LOG_ERROR("WebUI: HTTPS enabled but ssl_cert_path or ssl_key_path not set");
         return WEBUI_ERROR_SOCKET;
      }

      /* Verify certificate files exist */
      if (access(g_config.webui.ssl_cert_path, R_OK) != 0) {
         LOG_ERROR("WebUI: Cannot read SSL certificate: %s", g_config.webui.ssl_cert_path);
         return WEBUI_ERROR_SOCKET;
      }
      if (access(g_config.webui.ssl_key_path, R_OK) != 0) {
         LOG_ERROR("WebUI: Cannot read SSL private key: %s", g_config.webui.ssl_key_path);
         return WEBUI_ERROR_SOCKET;
      }

      info.options |= LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
      info.ssl_cert_filepath = g_config.webui.ssl_cert_path;
      info.ssl_private_key_filepath = g_config.webui.ssl_key_path;

      /* Force HTTP/1.1 only via ALPN to avoid HTTP/2 frame size limits (16KB).
       * WebSocket over HTTP/2 has stricter frame size requirements that cause
       * OVERSIZED_PAYLOAD errors with large conversation messages. */
      info.alpn = "http/1.1";

      LOG_INFO("WebUI: HTTPS enabled with cert: %s (HTTP/1.1 only)", g_config.webui.ssl_cert_path);
   }

   LOG_INFO("WebUI: Initializing %s server on port %d, serving from: %s",
            use_https ? "HTTPS" : "HTTP", port, s_www_path);

   /* Create context */
   s_lws_context = lws_create_context(&info);
   if (!s_lws_context) {
      LOG_ERROR("WebUI: Failed to create libwebsockets context");
      return WEBUI_ERROR_SOCKET;
   }

   s_port = port;
   s_running = 1;
   s_client_count = 0;

   /* Initialize audio subsystem (optional - continues if not available) */
#ifdef ENABLE_WEBUI_AUDIO
   if (webui_audio_init() != WEBUI_AUDIO_SUCCESS) {
      LOG_WARNING("WebUI: Audio subsystem not available, voice input disabled");
   }
#endif

   /* Register tool execution callback for debug display */
   llm_tools_set_execution_callback(webui_tool_execution_callback);

   /* Start server thread */
   if (pthread_create(&s_webui_thread, NULL, webui_thread_func, NULL) != 0) {
      LOG_ERROR("WebUI: Failed to create server thread");
#ifdef ENABLE_WEBUI_AUDIO
      webui_audio_cleanup();
#endif
      lws_context_destroy(s_lws_context);
      s_lws_context = NULL;
      s_running = 0;
      return WEBUI_ERROR_THREAD;
   }

   LOG_INFO("WebUI: Server started successfully on port %d", port);
   return WEBUI_SUCCESS;
}

void webui_server_shutdown(void) {
   pthread_mutex_lock(&s_mutex);
   if (!s_running) {
      pthread_mutex_unlock(&s_mutex);
      return;
   }

   LOG_INFO("WebUI: Shutting down server...");
   s_running = 0;
   pthread_mutex_unlock(&s_mutex);

   /* Wake up lws_service() to process shutdown */
   if (s_lws_context) {
      lws_cancel_service(s_lws_context);
   }

   /* Wait for thread to exit with timeout */
   LOG_INFO("WebUI: Waiting for server thread to exit (max 2 seconds)...");
   struct timespec ts;
   clock_gettime(CLOCK_REALTIME, &ts);
   ts.tv_sec += 2;

   int join_result = pthread_timedjoin_np(s_webui_thread, NULL, &ts);
   if (join_result == ETIMEDOUT) {
      LOG_WARNING("WebUI: Server thread did not exit in time, cancelling...");
      pthread_cancel(s_webui_thread);
      pthread_join(s_webui_thread, NULL);
      LOG_INFO("WebUI: Server thread cancelled and joined");
   } else if (join_result != 0) {
      LOG_ERROR("WebUI: pthread_timedjoin_np failed: %d", join_result);
   } else {
      LOG_INFO("WebUI: Server thread exited cleanly");
   }

   /* Destroy context */
   if (s_lws_context) {
      lws_context_destroy(s_lws_context);
      s_lws_context = NULL;
   }

   /* Cleanup audio subsystem */
#ifdef ENABLE_WEBUI_AUDIO
   webui_audio_cleanup();
#endif

   s_port = 0;
   s_client_count = 0;

   LOG_INFO("WebUI: Server shutdown complete");
}

bool webui_server_is_running(void) {
   bool running;
   pthread_mutex_lock(&s_mutex);
   running = s_running != 0;
   pthread_mutex_unlock(&s_mutex);
   return running;
}

int webui_server_client_count(void) {
   int count;
   pthread_mutex_lock(&s_mutex);
   count = s_client_count;
   pthread_mutex_unlock(&s_mutex);
   return count;
}

int webui_server_get_port(void) {
   return s_port;
}

void webui_clear_login_rate_limit(const char *ip_address) {
   if (ip_address) {
      /* Normalize IP before resetting (same normalization used during check) */
      char normalized_ip[RATE_LIMIT_IP_SIZE];
      rate_limiter_normalize_ip(ip_address, normalized_ip, sizeof(normalized_ip));
      rate_limiter_reset(&s_login_rate, normalized_ip);
      LOG_INFO("WebUI: Cleared in-memory rate limit for IP: %s (normalized: %s)", ip_address,
               normalized_ip);
   } else {
      rate_limiter_clear_all(&s_login_rate);
      LOG_INFO("WebUI: Cleared all in-memory rate limits");
   }
}

/* =============================================================================
 * Worker-Callable Response Functions (Thread-Safe)
 * ============================================================================= */

void webui_send_transcript(session_t *session, const char *role, const char *text) {
   if (!session || session->type != SESSION_TYPE_WEBSOCKET) {
      return;
   }

   ws_response_t resp = { .session = session,
                          .type = WS_RESP_TRANSCRIPT,
                          .transcript = {
                              .role = strdup(role),
                              .text = strdup(text),
                          } };

   if (!resp.transcript.role || !resp.transcript.text) {
      free(resp.transcript.role);
      free(resp.transcript.text);
      LOG_ERROR("WebUI: Failed to allocate transcript response");
      return;
   }

   queue_response(&resp);
}

void webui_send_state_with_detail(session_t *session, const char *state, const char *detail) {
   if (!session || session->type != SESSION_TYPE_WEBSOCKET) {
      return;
   }

   ws_response_t resp = { .session = session,
                          .type = WS_RESP_STATE,
                          .state = {
                              .state = strdup(state),
                              .detail = detail ? strdup(detail) : NULL,
                          } };

   if (!resp.state.state) {
      LOG_ERROR("WebUI: Failed to allocate state response");
      return;
   }

   queue_response(&resp);
}

void webui_send_state(session_t *session, const char *state) {
   webui_send_state_with_detail(session, state, NULL);

   /* Also send metrics update with state change */
   int context_pct = -1; /* -1 = no data, JS will keep previous value */
   llm_context_usage_t usage;
   session_llm_config_t llm_cfg;
   session_get_llm_config(session, &llm_cfg);
   if (llm_context_get_usage(session->session_id, llm_cfg.type, llm_cfg.cloud_provider, NULL,
                             &usage) == 0 &&
       usage.max_tokens > 0) {
      context_pct = (int)((float)usage.current_tokens / (float)usage.max_tokens * 100.0f);
   }
   webui_send_metrics_update(session, state, 0, 0.0f, context_pct);
}

void webui_send_context(session_t *session, int current_tokens, int max_tokens, float threshold) {
   /* If session is NULL, broadcast to all WebSocket sessions */
   if (!session) {
      /* Get local session as default */
      session = session_get_local();
   }

   if (!session || session->type != SESSION_TYPE_WEBSOCKET) {
      return;
   }

   ws_response_t resp = { .session = session,
                          .type = WS_RESP_CONTEXT,
                          .context = {
                              .current_tokens = current_tokens,
                              .max_tokens = max_tokens,
                              .threshold = threshold,
                          } };

   queue_response(&resp);
}

void webui_send_error(session_t *session, const char *code, const char *message) {
   if (!session || session->type != SESSION_TYPE_WEBSOCKET) {
      return;
   }

   ws_response_t resp = { .session = session,
                          .type = WS_RESP_ERROR,
                          .error = {
                              .code = strdup(code),
                              .message = strdup(message),
                          } };

   if (!resp.error.code || !resp.error.message) {
      free(resp.error.code);
      free(resp.error.message);
      LOG_ERROR("WebUI: Failed to allocate error response");
      return;
   }

   queue_response(&resp);
}

void webui_send_compaction_complete(session_t *session,
                                    int tokens_before,
                                    int tokens_after,
                                    int messages_summarized,
                                    const char *summary) {
   if (!session || session->type != SESSION_TYPE_WEBSOCKET) {
      return;
   }

   ws_response_t resp = { .session = session,
                          .type = WS_RESP_COMPACTION_COMPLETE,
                          .compaction = {
                              .tokens_before = tokens_before,
                              .tokens_after = tokens_after,
                              .messages_summarized = messages_summarized,
                              .summary = summary ? strdup(summary) : NULL,
                          } };

   queue_response(&resp);
}

/**
 * @brief Queue audio data for sending to WebSocket client
 *
 * Copies the audio data to the response queue. The WebUI thread will
 * send it as binary WebSocket frames. Large audio is chunked to avoid
 * overwhelming libwebsockets with huge single writes.
 *
 * @param session WebSocket session
 * @param data PCM audio data
 * @param len Length of audio data
 */
#define AUDIO_CHUNK_SIZE \
   8192 /* 8KB chunks for WebSocket frames - keep small for lws compatibility */

static void webui_send_audio(session_t *session, const uint8_t *data, size_t len) {
   if (!session || session->type != SESSION_TYPE_WEBSOCKET || !data || len == 0) {
      return;
   }

   /* Send audio in chunks to avoid overwhelming lws_write with huge buffers */
   size_t offset = 0;
   int chunk_num = 0;

   while (offset < len) {
      size_t chunk_len = len - offset;
      if (chunk_len > AUDIO_CHUNK_SIZE) {
         chunk_len = AUDIO_CHUNK_SIZE;
      }

      uint8_t *chunk_copy = malloc(chunk_len);
      if (!chunk_copy) {
         LOG_ERROR("WebUI: Failed to allocate audio chunk buffer");
         return;
      }
      memcpy(chunk_copy, data + offset, chunk_len);

      ws_response_t resp = { .session = session,
                             .type = WS_RESP_AUDIO,
                             .audio = {
                                 .data = chunk_copy,
                                 .len = chunk_len,
                             } };

      queue_response(&resp);
      offset += chunk_len;
      chunk_num++;
   }
}

/**
 * @brief Queue end-of-audio marker for WebSocket client
 *
 * @param session WebSocket session
 */
static void webui_send_audio_end(session_t *session) {
   if (!session || session->type != SESSION_TYPE_WEBSOCKET) {
      return;
   }

   ws_response_t resp = { .session = session, .type = WS_RESP_AUDIO_END };

   queue_response(&resp);
}

/* =============================================================================
 * LLM Streaming Functions (ChatGPT-style real-time text)
 *
 * These functions provide real-time token streaming to WebUI clients.
 * Protocol:
 *   1. stream_start - Create new assistant entry, enter streaming state
 *   2. stream_delta - Append text to current entry (multiple calls)
 *   3. stream_end   - Finalize entry, exit streaming state
 *
 * Stream IDs prevent stale deltas from cancelled streams from being displayed.
 * ============================================================================= */

void webui_send_stream_start(session_t *session) {
   if (!session || session->type != SESSION_TYPE_WEBSOCKET) {
      return;
   }

   /* Increment stream ID and mark streaming active */
   atomic_fetch_add(&session->current_stream_id, 1);
   session->llm_streaming_active = true;

   /* Reset command tag filter state for new stream */
   session->cmd_tag_filter.nesting_depth = 0;
   session->cmd_tag_filter.len = 0;

   /* Cache whether to bypass filtering (native tools enabled) */
   session->cmd_tag_filter_bypass = llm_tools_enabled(NULL);

   ws_response_t resp = { .session = session,
                          .type = WS_RESP_STREAM_START,
                          .stream = {
                              .stream_id = session->current_stream_id,
                              .text = "",
                          } };

   queue_response(&resp);
   LOG_INFO("WebUI: Stream start id=%u for session %u", session->current_stream_id,
            session->session_id);
}

/* Command tag filter uses shared constants from core/text_filter.h */

/**
 * @brief Output callback for WebUI streaming (adapter for text_filter API)
 *
 * This callback adapts the shared text_filter's signature to WebUI needs.
 * Session is passed via ctx parameter.
 */
static void webui_filter_output(const char *text, size_t len, void *ctx) {
   session_t *session = (session_t *)ctx;
   if (!session || !text || len == 0) {
      return;
   }

   /* Start stream on first content (lazy initialization) */
   if (!session->llm_streaming_active) {
      webui_send_stream_start(session);
   }

   ws_response_t resp = { .session = session,
                          .type = WS_RESP_STREAM_DELTA,
                          .stream = {
                              .stream_id = session->current_stream_id,
                          } };

   size_t copy_len = len < sizeof(resp.stream.text) - 1 ? len : sizeof(resp.stream.text) - 1;
   memcpy(resp.stream.text, text, copy_len);
   resp.stream.text[copy_len] = '\0';

   session->stream_had_content = true;
   queue_response(&resp);
}

/**
 * @brief Filter command tags and return filtered text
 *
 * Public function for callers that need the filtered text (e.g., TTS).
 * Uses the same state machine as WebUI streaming.
 *
 * @param session Session with filter state
 * @param text Input text to filter
 * @param out_buf Output buffer for filtered text
 * @param out_size Size of output buffer
 * @return Length of filtered text written to out_buf
 */
int webui_filter_command_tags(session_t *session,
                              const char *text,
                              char *out_buf,
                              size_t out_size) {
   if (!session || !text || !out_buf || out_size == 0) {
      return 0;
   }

   /* Native tools mode: no command tags to filter */
   if (session->cmd_tag_filter_bypass) {
      size_t len = strlen(text);
      size_t copy = len < out_size - 1 ? len : out_size - 1;
      memcpy(out_buf, text, copy);
      out_buf[copy] = '\0';
      return (int)copy;
   }

   /* Use shared text filter implementation */
   return text_filter_command_tags_to_buffer(&session->cmd_tag_filter, text, out_buf, out_size);
}

/**
 * @brief Send streaming text to WebUI with command tag filtering
 *
 * Filters <command>...</command> tags when in legacy mode (native tools disabled).
 * Automatically starts the stream on first content. Thread-safe per session.
 */
void webui_send_stream_delta(session_t *session, const char *text) {
   if (!session || session->type != SESSION_TYPE_WEBSOCKET) {
      return;
   }

   if (!text || text[0] == '\0') {
      return;
   }

   /* If native tools are enabled, pass through without filtering */
   if (session->cmd_tag_filter_bypass) {
      if (!session->llm_streaming_active) {
         webui_send_stream_start(session);
      }

      ws_response_t resp = { .session = session,
                             .type = WS_RESP_STREAM_DELTA,
                             .stream = {
                                 .stream_id = session->current_stream_id,
                             } };
      strncpy(resp.stream.text, text, sizeof(resp.stream.text) - 1);
      resp.stream.text[sizeof(resp.stream.text) - 1] = '\0';
      session->stream_had_content = true;
      queue_response(&resp);
      return;
   }

   /* Legacy command tag mode: filter using shared state machine */
   text_filter_command_tags(&session->cmd_tag_filter, text, webui_filter_output, session);
}

void webui_send_stream_end(session_t *session, const char *reason) {
   if (!session || session->type != SESSION_TYPE_WEBSOCKET) {
      return;
   }

   /* Mark streaming inactive */
   session->llm_streaming_active = false;

   ws_response_t resp = { .session = session,
                          .type = WS_RESP_STREAM_END,
                          .stream = {
                              .stream_id = session->current_stream_id,
                          } };

   /* Copy reason into fixed buffer (no malloc/free churn) */
   const char *r = reason ? reason : "complete";
   strncpy(resp.stream.text, r, sizeof(resp.stream.text) - 1);
   resp.stream.text[sizeof(resp.stream.text) - 1] = '\0';

   queue_response(&resp);
   LOG_INFO("WebUI: Stream end id=%u reason=%s for session %u", session->current_stream_id, r,
            session->session_id);
}

/* =============================================================================
 * Real-Time Metrics for UI Visualization
 *
 * Provides metrics for multi-ring visualization. Sent on:
 * - State changes (immediate)
 * - Token chunk events (during streaming)
 * - Periodic heartbeat (1Hz when idle)
 * ============================================================================= */

void webui_send_metrics_update(session_t *session,
                               const char *state,
                               int ttft_ms,
                               float token_rate,
                               int context_percent) {
   if (!session || session->type != SESSION_TYPE_WEBSOCKET) {
      return;
   }

   ws_response_t resp = { .session = session, .type = WS_RESP_METRICS_UPDATE };

   /* Copy state into fixed buffer */
   const char *s = state ? state : "idle";
   strncpy(resp.metrics.state, s, sizeof(resp.metrics.state) - 1);
   resp.metrics.state[sizeof(resp.metrics.state) - 1] = '\0';

   resp.metrics.ttft_ms = ttft_ms;
   resp.metrics.token_rate = token_rate;
   resp.metrics.context_pct = context_percent;

   queue_response(&resp);
}

/* =============================================================================
 * Text Processing (Async Worker Thread)
 *
 * For Phase 2, we use a simple detached thread for text processing.
 * Phase 4 may integrate with the worker pool for audio + text.
 * ============================================================================= */

/* Command processing constants */
#define MAX_TOOL_RESULTS 8
#define TOOL_RESULT_MSG_SIZE 1024
#define WEBUI_WORKER_ID 100 /* Virtual worker ID for command router */

typedef struct {
   session_t *session;
   char *text;
} text_work_t;

/**
 * @brief Process commands in LLM response and make follow-up calls
 *
 * Searches for <command> tags in the response, executes them via MQTT,
 * and makes follow-up LLM calls with the results.
 *
 * @param llm_response The LLM response to process
 * @param session Session for follow-up LLM calls
 * @return Final response text (caller must free), or NULL if no commands
 */
static char *webui_process_commands(const char *llm_response, session_t *session) {
   if (!llm_response || !session) {
      return NULL;
   }

   struct mosquitto *mosq = worker_pool_get_mosq();
   if (!mosq) {
      LOG_WARNING("WebUI: No MQTT connection, cannot process commands");
      return NULL;
   }

   /* Collect all tool results */
   char *tool_results[MAX_TOOL_RESULTS];
   int num_results = 0;

   for (int i = 0; i < MAX_TOOL_RESULTS; i++) {
      tool_results[i] = NULL;
   }

   /* Search for <command> tags and process each one */
   const char *search_ptr = llm_response;
   const char *cmd_start;

   while ((cmd_start = strstr(search_ptr, "<command>")) != NULL && num_results < MAX_TOOL_RESULTS) {
      const char *cmd_end = strstr(cmd_start, "</command>");
      if (!cmd_end) {
         LOG_WARNING("WebUI: Unclosed <command> tag");
         break;
      }

      /* Extract command JSON */
      const char *json_start = cmd_start + strlen("<command>");
      size_t json_len = cmd_end - json_start;

      char *cmd_json = malloc(json_len + 1);
      if (!cmd_json) {
         LOG_ERROR("WebUI: Failed to allocate command JSON");
         break;
      }
      memcpy(cmd_json, json_start, json_len);
      cmd_json[json_len] = '\0';

      LOG_INFO("WebUI: Processing command: %s", cmd_json);

      /* Parse JSON to extract device/action */
      struct json_object *parsed_json = json_tokener_parse(cmd_json);
      if (!parsed_json) {
         LOG_WARNING("WebUI: Invalid command JSON: %s", cmd_json);
         free(cmd_json);
         search_ptr = cmd_end + strlen("</command>");
         continue;
      }

      /* Get device and action for result formatting */
      struct json_object *device_obj = NULL;
      struct json_object *action_obj = NULL;
      const char *device_name = "unknown";
      const char *action_name = "unknown";

      if (json_object_object_get_ex(parsed_json, "device", &device_obj)) {
         device_name = json_object_get_string(device_obj);
      }
      if (json_object_object_get_ex(parsed_json, "action", &action_obj)) {
         action_name = json_object_get_string(action_obj);
      }

      /* Register pending request */
      pending_request_t *req = command_router_register(WEBUI_WORKER_ID);
      if (!req) {
         LOG_ERROR("WebUI: Failed to register pending request");
         json_object_put(parsed_json);
         free(cmd_json);
         search_ptr = cmd_end + strlen("</command>");
         continue;
      }

      const char *request_id = command_router_get_id(req);
      LOG_INFO("WebUI: Registered request %s", request_id);

      /* Add request_id, session_id, and timestamp to command JSON (OCP v1.1) */
      json_object_object_add(parsed_json, "request_id", json_object_new_string(request_id));
      json_object_object_add(parsed_json, "session_id",
                             json_object_new_int((int32_t)session->session_id));
      json_object_object_add(parsed_json, "timestamp",
                             json_object_new_int64(ocp_get_timestamp_ms()));
      const char *cmd_with_id = json_object_to_json_string(parsed_json);

      /* Publish command via MQTT */
      int rc = mosquitto_publish(mosq, NULL, APPLICATION_NAME, strlen(cmd_with_id), cmd_with_id, 0,
                                 false);
      if (rc != MOSQ_ERR_SUCCESS) {
         LOG_ERROR("WebUI: MQTT publish failed: %d", rc);
         command_router_cancel(req);
         json_object_put(parsed_json);
         free(cmd_json);
         search_ptr = cmd_end + strlen("</command>");
         continue;
      }
      LOG_INFO("WebUI: Published command to %s", APPLICATION_NAME);

      /* Wait for result */
      char *callback_result = command_router_wait(req, COMMAND_RESULT_TIMEOUT_MS);

      /* Format result for LLM */
      tool_results[num_results] = malloc(TOOL_RESULT_MSG_SIZE);
      if (tool_results[num_results]) {
         if (callback_result && strlen(callback_result) > 0) {
            LOG_INFO("WebUI: Received callback result: %.50s%s", callback_result,
                     strlen(callback_result) > 50 ? "..." : "");
            snprintf(tool_results[num_results], TOOL_RESULT_MSG_SIZE,
                     "[Tool Result: %s.%s returned: %s]", device_name, action_name,
                     callback_result);
         } else {
            LOG_WARNING("WebUI: No callback result (timeout or empty)");
            snprintf(tool_results[num_results], TOOL_RESULT_MSG_SIZE,
                     "[Tool Result: %s.%s completed successfully]", device_name, action_name);
         }

         /* Send tool result to WebUI for debug display */
         webui_send_transcript(session, "assistant", tool_results[num_results]);

         num_results++;
      }

      if (callback_result) {
         free(callback_result);
      }

      json_object_put(parsed_json);
      free(cmd_json);
      search_ptr = cmd_end + strlen("</command>");
   }

   /* If no results collected, return NULL (no commands processed) */
   if (num_results == 0) {
      return NULL;
   }

   /* Build combined tool results message for LLM */
   size_t total_len = 1; /* For null terminator */
   for (int i = 0; i < num_results; i++) {
      if (tool_results[i]) {
         total_len += strlen(tool_results[i]) + 1; /* +1 for newline */
      }
   }

   char *combined_results = malloc(total_len);
   if (!combined_results) {
      LOG_ERROR("WebUI: Failed to allocate combined results");
      for (int i = 0; i < num_results; i++) {
         free(tool_results[i]);
      }
      return NULL;
   }

   char *ptr = combined_results;
   for (int i = 0; i < num_results; i++) {
      if (tool_results[i]) {
         size_t len = strlen(tool_results[i]);
         memcpy(ptr, tool_results[i], len);
         ptr += len;
         if (i < num_results - 1) {
            *ptr++ = '\n';
         }
         free(tool_results[i]);
      }
   }
   *ptr = '\0';

   LOG_INFO("WebUI: Sending tool results to LLM: %s", combined_results);

   /* Make follow-up LLM call with tool results */
   char *final_response = session_llm_call(session, combined_results);

   free(combined_results);

   if (!final_response) {
      LOG_ERROR("WebUI: Follow-up LLM call failed");
      return NULL;
   }

   LOG_INFO("WebUI: LLM final response: %.50s%s", final_response,
            strlen(final_response) > 50 ? "..." : "");

   return final_response;
}

/**
 * @brief Strip command tags from response text
 *
 * Removes all <command>...</command> blocks from the text.
 *
 * @param text Text to modify in place
 */
static void strip_command_tags(char *text) {
   if (!text)
      return;

   char *cmd_start, *cmd_end;
   while ((cmd_start = strstr(text, "<command>")) != NULL) {
      cmd_end = strstr(cmd_start, "</command>");
      if (cmd_end) {
         cmd_end += strlen("</command>");
         memmove(cmd_start, cmd_end, strlen(cmd_end) + 1);
      } else {
         break;
      }
   }

   /* Also remove <end_of_turn> tags (local AI models) */
   char *match = strstr(text, "<end_of_turn>");
   if (match) {
      *match = '\0';
   }
}

static void *text_worker_thread(void *arg) {
   text_work_t *work = (text_work_t *)arg;
   session_t *session = work->session;
   char *text = work->text;

   /* Check if session is still valid */
   if (!session || session->disconnected) {
      LOG_INFO("WebUI: Session already disconnected, aborting text processing");
      if (session) {
         session_release(session);
      }
      free(text);
      free(work);
      return NULL;
   }

   LOG_INFO("WebUI: Processing text input for session %u: %s", session->session_id, text);

   /* Send "thinking" state */
   webui_send_state_with_detail(session, "thinking", "Processing request...");

   /* Echo user input as transcript */
   webui_send_transcript(session, "user", text);

   /* Call LLM with session's conversation history */
   char *response = session_llm_call(session, text);

   /* Check disconnection again - session may have been invalidated during LLM call */
   if (session->disconnected) {
      /* Client disconnected during processing - don't try to send response */
      LOG_INFO("WebUI: Session %u disconnected during LLM call", session->session_id);
      session_release(session);
      free(response);
      free(text);
      free(work);
      return NULL;
   }

   if (!response) {
      /* LLM call failed */
      webui_send_error(session, "LLM_ERROR", "Failed to get response from AI");
      webui_send_state(session, "idle");
      session_release(session);
      free(text);
      free(work);
      return NULL;
   }

   /* Check for command tags and process them */
   char *final_response = response;
   if (strstr(response, "<command>")) {
      LOG_INFO("WebUI: Response contains commands, processing...");

      /* Note: Don't send intermediate response here - streaming already delivered it */

      /* Process commands and get follow-up response */
      char *processed = webui_process_commands(response, session);
      if (processed) {
         /* Check for disconnection after command processing */
         if (session->disconnected) {
            LOG_INFO("WebUI: Session %u disconnected during command processing",
                     session->session_id);
            session_release(session);
            free(response);
            free(processed);
            free(text);
            free(work);
            return NULL;
         }

         /* Recursively process if the follow-up also contains commands.
          * Limit iterations to prevent infinite loops from confused LLMs. */
         int follow_up_iterations = 0;
         const int MAX_FOLLOW_UP_ITERATIONS = 5;

         while (strstr(processed, "<command>") && !session->disconnected) {
            follow_up_iterations++;
            if (follow_up_iterations > MAX_FOLLOW_UP_ITERATIONS) {
               LOG_WARNING("WebUI: Command loop limit reached (%d iterations), breaking",
                           MAX_FOLLOW_UP_ITERATIONS);
               break;
            }

            LOG_INFO("WebUI: Follow-up response contains more commands, processing... (iter %d/%d)",
                     follow_up_iterations, MAX_FOLLOW_UP_ITERATIONS);
            /* Note: Don't send transcript - streaming already delivered it */

            char *next_processed = webui_process_commands(processed, session);
            free(processed);
            if (!next_processed) {
               processed = NULL;
               break;
            }
            processed = next_processed;
         }

         if (processed) {
            free(response);
            final_response = processed;
         } else {
            /* Command processing failed, use original response */
            final_response = response;
         }
      }
   }

   /* Strip any remaining command tags from final response */
   strip_command_tags(final_response);

   /* Note: Don't send transcript here - streaming already delivered the content.
    * The session_llm_call() uses webui_send_stream_start/delta/end for real-time delivery. */

   /* Free the final response (either original response or processed copy) */
   free(final_response);

   /* Send context usage update to WebUI */
   {
      int current_tokens, max_tokens;
      float threshold;
      llm_context_get_last_usage(&current_tokens, &max_tokens, &threshold);
      if (max_tokens > 0) {
         webui_send_context(session, current_tokens, max_tokens, threshold);
      }
   }

   /* Return to idle state */
   webui_send_state(session, "idle");

   /* Release session reference (acquired in webui_process_text_input) */
   session_release(session);

   free(text);
   free(work);
   return NULL;
}

int webui_process_text_input(session_t *session, const char *text) {
   if (!session || !text || strlen(text) == 0) {
      return 1;
   }

   /* Create work item */
   text_work_t *work = malloc(sizeof(text_work_t));
   if (!work) {
      LOG_ERROR("WebUI: Failed to allocate text work item");
      return 1;
   }

   work->session = session;
   work->text = strdup(text);
   if (!work->text) {
      LOG_ERROR("WebUI: Failed to allocate text copy");
      free(work);
      return 1;
   }

   /* Retain session for worker thread (worker will release when done) */
   session_retain(session);

   /* Spawn detached worker thread */
   pthread_t thread;
   pthread_attr_t attr;
   pthread_attr_init(&attr);
   pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

   int ret = pthread_create(&thread, &attr, text_worker_thread, work);
   pthread_attr_destroy(&attr);

   if (ret != 0) {
      LOG_ERROR("WebUI: Failed to create text worker thread");
      session_release(session);
      free(work->text);
      free(work);
      return 1;
   }

   return 0;
}

/* =============================================================================
 * JSON Message Handler Implementation
 * ============================================================================= */

static void handle_text_message(ws_connection_t *conn, const char *text, size_t len) {
   (void)len; /* Length already validated by caller */

   if (!conn->session) {
      LOG_WARNING("WebUI: Text message received but no session");
      return;
   }

   LOG_INFO("WebUI: Text input from session %u: %s", conn->session->session_id, text);

   int ret = webui_process_text_input(conn->session, text);
   if (ret != 0) {
      send_error_impl(conn->wsi, "PROCESSING_ERROR", "Failed to process text input");
   }
}

#ifdef ENABLE_WEBUI_AUDIO
/* =============================================================================
 * Audio Processing (Binary WebSocket Messages)
 *
 * Protocol:
 * - Client sends binary frames with 1-byte type prefix:
 *   - WS_BIN_AUDIO_IN (0x01): Opus audio chunk (length-prefixed frames)
 *   - WS_BIN_AUDIO_IN_END (0x02): End of utterance (triggers ASR + LLM + TTS)
 *
 * - Server responds with binary frames:
 *   - WS_BIN_AUDIO_OUT (0x11): PCM audio chunk for playback
 *   - WS_BIN_AUDIO_SEGMENT_END (0x12): Play accumulated audio segment now
 * ============================================================================= */

typedef struct {
   session_t *session;
   uint8_t *audio_data;
   size_t audio_len;
   bool use_opus; /* True if audio_data is Opus-encoded, false if raw PCM */
} audio_work_t;

/**
 * @brief Sentence callback for real-time TTS audio streaming
 *
 * Called for each complete sentence during LLM response streaming.
 * Generates TTS and sends audio immediately, enabling sentence-by-sentence
 * playback instead of waiting for the full response.
 */
static void webui_sentence_audio_callback(const char *sentence, void *userdata) {
   session_t *session = (session_t *)userdata;

   if (!sentence || strlen(sentence) == 0 || !session || session->disconnected) {
      return;
   }

   /* Make a copy for cleaning (same pattern as dawn_tts_sentence_callback) */
   char *cleaned = strdup(sentence);
   if (!cleaned) {
      return;
   }

   /* Remove command tags (they'll be processed from the full response later) */
   char *cmd_start, *cmd_end;
   while ((cmd_start = strstr(cleaned, "<command>")) != NULL) {
      cmd_end = strstr(cmd_start, "</command>");
      if (cmd_end) {
         cmd_end += strlen("</command>");
         memmove(cmd_start, cmd_end, strlen(cmd_end) + 1);
      } else {
         /* Incomplete command tag - strip from <command> to end */
         *cmd_start = '\0';
         break;
      }
   }

   /* Remove special characters that cause TTS issues */
   remove_chars(cleaned, "*");
   remove_emojis(cleaned);

   /* Trim whitespace */
   size_t len = strlen(cleaned);
   while (len > 0 && (cleaned[len - 1] == ' ' || cleaned[len - 1] == '\t' ||
                      cleaned[len - 1] == '\n' || cleaned[len - 1] == '\r')) {
      cleaned[--len] = '\0';
   }

   /* Only generate TTS if there's actual content */
   if (len > 0) {
      /* Switch to "speaking" state when first audio is ready */
      webui_send_state(session, "speaking");

      /* Check if client supports Opus codec */
      ws_connection_t *conn = (ws_connection_t *)session->client_data;
      bool use_opus = conn && conn->use_opus;

      LOG_INFO("WebUI: TTS streaming sentence (%s): %.60s%s", use_opus ? "opus" : "pcm", cleaned,
               len > 60 ? "..." : "");

      if (use_opus) {
         /* Encode TTS output as Opus for bandwidth savings */
         uint8_t *opus = NULL;
         size_t opus_len = 0;
         int ret = webui_audio_text_to_opus(cleaned, &opus, &opus_len);

         if (ret == WEBUI_AUDIO_SUCCESS && opus && opus_len > 0) {
            webui_send_audio(session, opus, opus_len);
            webui_send_audio_end(session);
            free(opus);
         }
      } else {
         /* Send raw PCM for legacy clients */
         int16_t *pcm = NULL;
         size_t samples = 0;
         int ret = webui_audio_text_to_pcm(cleaned, &pcm, &samples);

         if (ret == WEBUI_AUDIO_SUCCESS && pcm && samples > 0) {
            size_t bytes = samples * sizeof(int16_t);
            webui_send_audio(session, (const uint8_t *)pcm, bytes);
            webui_send_audio_end(session);
            free(pcm);
         }
      }
   }

   free(cleaned);
}

/**
 * @brief Audio worker thread - process voice input and generate response
 *
 * Uses sentence-by-sentence TTS streaming: audio is generated and sent as each
 * sentence completes during the LLM response, rather than waiting for the full
 * response. This significantly reduces perceived latency.
 */
static void *audio_worker_thread(void *arg) {
   audio_work_t *work = (audio_work_t *)arg;
   session_t *session = work->session;
   uint8_t *audio_data = work->audio_data;
   size_t audio_len = work->audio_len;

   if (!session || session->disconnected) {
      LOG_INFO("WebUI: Audio session disconnected, aborting");
      if (session) {
         session_release(session);
      }
      free(audio_data);
      free(work);
      return NULL;
   }

   LOG_INFO("WebUI: Processing audio for session %u (%zu bytes, %s)", session->session_id,
            audio_len, work->use_opus ? "opus" : "pcm");

   /* Send "listening" state (ASR in progress) */
   webui_send_state(session, "listening");

   /* Transcribe audio */
   char *transcript = NULL;
   int ret;

   if (work->use_opus) {
      /* Decode Opus (48kHz) and resample to 16kHz for ASR */
      ret = webui_audio_opus_to_text(audio_data, audio_len, &transcript);
      free(audio_data);
      work->audio_data = NULL;
   } else {
      /* Raw PCM: 16-bit signed, 48kHz, mono - resample to 16kHz for ASR */
      size_t pcm_samples = audio_len / sizeof(int16_t);
      ret = webui_audio_pcm48k_to_text((const int16_t *)audio_data, pcm_samples, &transcript);
      free(audio_data);
      work->audio_data = NULL;
   }

   if (ret != WEBUI_AUDIO_SUCCESS || !transcript || strlen(transcript) == 0) {
      LOG_WARNING("WebUI: Audio transcription failed or empty");
      webui_send_error(session, "ASR_FAILED", "Could not understand audio");
      webui_send_state(session, "idle");
      session_release(session);
      free(transcript);
      free(work);
      return NULL;
   }

   LOG_INFO("WebUI: Transcribed: \"%s\"", transcript);

   /* Check disconnection */
   if (session->disconnected) {
      session_release(session);
      free(transcript);
      free(work);
      return NULL;
   }

   /* Echo transcription as user message */
   webui_send_transcript(session, "user", transcript);

   /* Send "thinking" state while LLM processes - streaming callback will switch to "speaking" */
   webui_send_state_with_detail(session, "thinking", "Processing request...");

   /* Call LLM with TTS streaming - audio is generated and sent per-sentence */
   char *response = session_llm_call_with_tts(session, transcript, webui_sentence_audio_callback,
                                              session);
   free(transcript);

   if (!response || session->disconnected) {
      LOG_WARNING("WebUI: LLM call failed or session disconnected");
      if (!session->disconnected) {
         webui_send_error(session, "LLM_ERROR", "Failed to get response");
      }
      webui_send_state(session, "idle");
      session_release(session);
      free(response);
      free(work);
      return NULL;
   }

   /* Process commands if present (audio already sent via streaming callback) */
   if (strstr(response, "<command>")) {
      LOG_INFO("WebUI: Audio response contains commands, processing...");
      webui_send_state(session, "processing");

      char *processed = webui_process_commands(response, session);
      if (processed && !session->disconnected) {
         free(response);
         response = processed;

         /* Generate TTS for the follow-up response (command results) */
         LOG_INFO("WebUI: Generating TTS for command result: %.60s%s", processed,
                  strlen(processed) > 60 ? "..." : "");
         webui_sentence_audio_callback(processed, session);
      }
   }

   /* Send audio end marker (all audio chunks have been sent) */
   webui_send_audio_end(session);

   /* Free response */
   free(response);

   /* Send context usage update to WebUI */
   {
      int current_tokens, max_tokens;
      float threshold;
      llm_context_get_last_usage(&current_tokens, &max_tokens, &threshold);
      if (max_tokens > 0) {
         webui_send_context(session, current_tokens, max_tokens, threshold);
      }
   }

   webui_send_state(session, "idle");

   /* Release session reference (acquired before thread creation) */
   session_release(session);

   free(work);
   return NULL;
}

/**
 * @brief Handle binary WebSocket message (audio data)
 *
 * Binary message format:
 * - Byte 0: Message type (WS_BIN_AUDIO_IN or WS_BIN_AUDIO_IN_END)
 * - Bytes 1+: Opus audio data (for AUDIO_IN) or empty (for AUDIO_IN_END)
 */
static void handle_binary_message(ws_connection_t *conn, const uint8_t *data, size_t len) {
   if (len < 1) {
      LOG_WARNING("WebUI: Empty binary message");
      return;
   }

   if (!conn->session) {
      LOG_WARNING("WebUI: Binary message but no session");
      return;
   }

   uint8_t msg_type = data[0];
   const uint8_t *payload = data + 1;
   size_t payload_len = len - 1;

   switch (msg_type) {
      case WS_BIN_AUDIO_IN: {
         /* Accumulate audio data in connection buffer */
         if (payload_len == 0) {
            break;
         }

         /* Initialize buffer if needed */
         if (!conn->audio_buffer) {
            conn->audio_buffer_capacity = WEBUI_AUDIO_BUFFER_SIZE;
            conn->audio_buffer = malloc(conn->audio_buffer_capacity);
            if (!conn->audio_buffer) {
               LOG_ERROR("WebUI: Failed to allocate audio buffer");
               send_error_impl(conn->wsi, "BUFFER_ERROR", "Audio buffer allocation failed");
               return;
            }
            conn->audio_buffer_len = 0;
         }

         /* Check if we have room */
         if (conn->audio_buffer_len + payload_len > conn->audio_buffer_capacity) {
            /* Expand buffer */
            size_t new_capacity = conn->audio_buffer_capacity * 2;

            /* Enforce maximum capacity to prevent OOM from malicious/long recordings */
            if (new_capacity > WEBUI_AUDIO_MAX_CAPACITY) {
               LOG_WARNING("WebUI: Audio buffer would exceed max capacity (%d bytes)",
                           WEBUI_AUDIO_MAX_CAPACITY);
               send_error_impl(conn->wsi, "BUFFER_FULL", "Recording too long");
               return;
            }

            uint8_t *new_buffer = realloc(conn->audio_buffer, new_capacity);
            if (!new_buffer) {
               LOG_ERROR("WebUI: Failed to expand audio buffer");
               send_error_impl(conn->wsi, "BUFFER_ERROR", "Audio buffer allocation failed");
               return;
            }
            conn->audio_buffer = new_buffer;
            conn->audio_buffer_capacity = new_capacity;
         }

         /* Append audio data */
         memcpy(conn->audio_buffer + conn->audio_buffer_len, payload, payload_len);
         conn->audio_buffer_len += payload_len;

         LOG_INFO("WebUI: Accumulated %zu bytes audio (total: %zu)", payload_len,
                  conn->audio_buffer_len);
         break;
      }

      case WS_BIN_AUDIO_IN_END: {
         /* End of utterance - process accumulated audio */
         if (!conn->audio_buffer || conn->audio_buffer_len == 0) {
            LOG_WARNING("WebUI: AUDIO_IN_END but no audio accumulated");
            break;
         }

         LOG_INFO("WebUI: Audio end, processing %zu bytes", conn->audio_buffer_len);

         /* Create work item for worker thread */
         audio_work_t *work = malloc(sizeof(audio_work_t));
         if (!work) {
            LOG_ERROR("WebUI: Failed to allocate audio work");
            free(conn->audio_buffer);
            conn->audio_buffer = NULL;
            conn->audio_buffer_len = 0;
            send_error_impl(conn->wsi, "PROCESSING_ERROR", "Audio processing failed");
            return;
         }

         work->session = conn->session;
         work->audio_data = conn->audio_buffer;
         work->audio_len = conn->audio_buffer_len;
         work->use_opus = conn->use_opus;

         /* Transfer ownership to worker */
         conn->audio_buffer = NULL;
         conn->audio_buffer_len = 0;

         /* Retain session for worker thread (worker will release when done) */
         session_retain(conn->session);

         /* Spawn worker thread */
         pthread_t thread;
         pthread_attr_t attr;
         pthread_attr_init(&attr);
         pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

         int ret = pthread_create(&thread, &attr, audio_worker_thread, work);
         pthread_attr_destroy(&attr);

         if (ret != 0) {
            LOG_ERROR("WebUI: Failed to create audio worker thread");
            session_release(conn->session);
            free(work->audio_data);
            free(work);
            send_error_impl(conn->wsi, "PROCESSING_ERROR", "Audio processing failed");
         }
         break;
      }

      default:
         LOG_WARNING("WebUI: Unknown binary message type: 0x%02x", msg_type);
         break;
   }
}
#endif /* ENABLE_WEBUI_AUDIO */
