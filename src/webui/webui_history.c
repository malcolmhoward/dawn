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
 * WebUI History Handlers - Conversation history management
 *
 * This module handles WebSocket messages for conversation history:
 * - list_conversations, new_conversation, load_conversation
 * - delete_conversation, rename_conversation, search_conversations
 * - save_message, update_context, clear_session, continue_conversation
 */

#include <string.h>

#include "auth/auth_db.h"
#include "config/dawn_config.h"
#include "core/ocp_helpers.h"
#include "core/session_manager.h"
#include "llm/llm_command_parser.h"
#include "logging.h"
#include "memory/memory_extraction.h"
#include "webui/webui_internal.h"
#include "webui/webui_server.h" /* For WEBUI_MAX_THUMBNAIL_BASE64 */

/* =============================================================================
 * Image Marker Validation (Security)
 * ============================================================================ */

/* Safe data URI prefixes for thumbnails (SVG explicitly excluded for XSS prevention) */
static const char *SAFE_IMAGE_PREFIXES[] = { "data:image/jpeg;base64,", "data:image/png;base64,",
                                             "data:image/gif;base64,", "data:image/webp;base64,",
                                             NULL };

/* Valid base64 character lookup table (A-Z, a-z, 0-9, +, /, =) */
static const unsigned char BASE64_VALID[256] = {
   ['A'] = 1, ['B'] = 1, ['C'] = 1, ['D'] = 1, ['E'] = 1, ['F'] = 1, ['G'] = 1, ['H'] = 1,
   ['I'] = 1, ['J'] = 1, ['K'] = 1, ['L'] = 1, ['M'] = 1, ['N'] = 1, ['O'] = 1, ['P'] = 1,
   ['Q'] = 1, ['R'] = 1, ['S'] = 1, ['T'] = 1, ['U'] = 1, ['V'] = 1, ['W'] = 1, ['X'] = 1,
   ['Y'] = 1, ['Z'] = 1, ['a'] = 1, ['b'] = 1, ['c'] = 1, ['d'] = 1, ['e'] = 1, ['f'] = 1,
   ['g'] = 1, ['h'] = 1, ['i'] = 1, ['j'] = 1, ['k'] = 1, ['l'] = 1, ['m'] = 1, ['n'] = 1,
   ['o'] = 1, ['p'] = 1, ['q'] = 1, ['r'] = 1, ['s'] = 1, ['t'] = 1, ['u'] = 1, ['v'] = 1,
   ['w'] = 1, ['x'] = 1, ['y'] = 1, ['z'] = 1, ['0'] = 1, ['1'] = 1, ['2'] = 1, ['3'] = 1,
   ['4'] = 1, ['5'] = 1, ['6'] = 1, ['7'] = 1, ['8'] = 1, ['9'] = 1, ['+'] = 1, ['/'] = 1,
   ['='] = 1
};

/**
 * @brief Check if string is a valid image ID
 *
 * Image ID format: "img_" + 12 alphanumeric characters (16 total)
 *
 * @param str String to check
 * @param len Length of string
 * @return true if valid image ID format
 */
static bool is_valid_image_id(const char *str, size_t len) {
   /* Must be exactly 16 characters: "img_" + 12 alphanumeric */
   if (len != 16) {
      return false;
   }

   /* Must start with "img_" */
   if (strncmp(str, "img_", 4) != 0) {
      return false;
   }

   /* Characters 4-15 must be alphanumeric */
   for (int i = 4; i < 16; i++) {
      char c = str[i];
      if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) {
         return false;
      }
   }

   return true;
}

/**
 * @brief Validate a single image marker
 *
 * Accepts two formats:
 * 1. Image ID: [IMAGE:img_xxxxxxxxxxxx] - server-stored image reference
 * 2. Data URI: [IMAGE:data:image/jpeg;base64,...] - legacy inline data
 *
 * @param marker_start Pointer to start of "[IMAGE:" marker
 * @param marker_end Output: pointer to closing ']' if found
 * @return true if marker is valid, false if malicious/oversized
 */
static bool validate_single_image_marker(const char *marker_start, const char **marker_end) {
   /* Find the closing bracket */
   const char *end = strchr(marker_start + 7, ']');
   if (!end) {
      return false; /* Malformed marker */
   }
   *marker_end = end;

   /* Extract content (skip "[IMAGE:" prefix) */
   const char *content = marker_start + 7;
   size_t content_len = end - content;

   /* Check if it's an image ID (new format: img_xxxxxxxxxxxx) */
   if (is_valid_image_id(content, content_len)) {
      return true; /* Valid image ID reference */
   }

   /* Otherwise, validate as legacy data URI */

   /* Check against safe prefixes */
   bool has_safe_prefix = false;
   for (int i = 0; SAFE_IMAGE_PREFIXES[i] != NULL; i++) {
      size_t prefix_len = strlen(SAFE_IMAGE_PREFIXES[i]);
      if (content_len > prefix_len && strncmp(content, SAFE_IMAGE_PREFIXES[i], prefix_len) == 0) {
         has_safe_prefix = true;
         break;
      }
   }

   if (!has_safe_prefix) {
      LOG_WARNING("WebUI: Rejected message with unsafe image data URI prefix");
      return false;
   }

   /* Check size (base64 portion only) */
   const char *base64_start = strchr(content, ',');
   if (!base64_start || base64_start >= end) {
      return false; /* Malformed data URI */
   }
   base64_start++; /* Skip comma */

   size_t base64_len = end - base64_start;
   if (base64_len > WEBUI_MAX_THUMBNAIL_BASE64) {
      LOG_WARNING("WebUI: Rejected oversized thumbnail (%zu > %d bytes)", base64_len,
                  WEBUI_MAX_THUMBNAIL_BASE64);
      return false;
   }

   /* Validate base64 characters (prevents injection via malformed data) */
   for (size_t i = 0; i < base64_len; i++) {
      unsigned char c = (unsigned char)base64_start[i];
      if (!BASE64_VALID[c]) {
         LOG_WARNING("WebUI: Rejected thumbnail with invalid base64 character at position %zu", i);
         return false;
      }
   }

   return true;
}

/**
 * @brief Validate ALL embedded image markers in message content
 *
 * Accepts two marker formats:
 * - Image ID: [IMAGE:img_xxxxxxxxxxxx] (server-stored reference)
 * - Data URI: [IMAGE:data:image/jpeg;base64,...] (legacy inline)
 *
 * For image IDs: validates format (img_ + 12 alphanumeric)
 * For data URIs: validates prefix, size, and base64 characters
 *
 * SECURITY: Validates every marker, not just the first, to prevent bypass
 * attacks where a valid first image masks a malicious second image.
 *
 * @param content Message content to validate
 * @return true if all markers are safe, false if any malicious/invalid marker found
 */
static bool validate_image_marker(const char *content) {
   if (!content)
      return true;

   const char *search_pos = content;
   const char *marker_start;
   int marker_count = 0;

   /* Iterate through ALL [IMAGE: markers in content */
   while ((marker_start = strstr(search_pos, "[IMAGE:")) != NULL) {
      const char *marker_end = NULL;

      if (!validate_single_image_marker(marker_start, &marker_end)) {
         LOG_WARNING("WebUI: Rejected invalid image marker #%d in message", marker_count + 1);
         return false;
      }

      marker_count++;

      /* Limit total markers to prevent DoS (matches WEBUI_MAX_VISION_IMAGES) */
      if (marker_count > WEBUI_MAX_VISION_IMAGES) {
         LOG_WARNING("WebUI: Rejected message with too many image markers (%d > %d)", marker_count,
                     WEBUI_MAX_VISION_IMAGES);
         return false;
      }

      /* Move search position past this marker */
      search_pos = marker_end + 1;
   }

   return true;
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
   json_object_object_add(conv_obj, "is_private", json_object_new_boolean(conv->is_private));
   json_object_object_add(conv_obj, "origin",
                          json_object_new_string(conv->origin[0] ? conv->origin : "webui"));

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
void handle_list_conversations(ws_connection_t *conn, struct json_object *payload) {
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

/* =============================================================================
 * Privacy Check Helper
 * ============================================================================ */

/**
 * @brief Check if memory extraction should be skipped for the active conversation
 *
 * Centralizes the privacy check logic and handles race conditions by re-verifying
 * from the database when needed. Also updates the cached state if stale.
 *
 * @param conn WebSocket connection with conversation context
 * @return true if memory extraction should be skipped, false otherwise
 */
static bool should_skip_memory_extraction(ws_connection_t *conn) {
   /* No conversation to extract from */
   if (conn->active_conversation_id <= 0) {
      return true;
   }

   /* Memory system disabled */
   if (!g_config.memory.enabled) {
      return true;
   }

   /* Check cached privacy flag first */
   if (conn->active_conversation_private) {
      return true;
   }

   /* Re-verify from database to handle race conditions (e.g., set_private in flight) */
   int db_private = conv_db_is_private(conn->active_conversation_id, conn->auth_user_id);
   if (db_private > 0) {
      /* Update cached state to match database */
      conn->active_conversation_private = true;
      LOG_INFO("WebUI: privacy check found stale cache, conversation %lld is private",
               (long long)conn->active_conversation_id);
      return true;
   }

   /* db_private == 0 means not private, -1 means error/not found - proceed with extraction */
   return false;
}

/**
 * @brief Create a new conversation
 */
void handle_new_conversation(ws_connection_t *conn, struct json_object *payload) {
   if (!conn_require_auth(conn)) {
      return;
   }

   /* Trigger memory extraction for old conversation before creating new one (async, non-blocking).
    * This captures the conversation state before switching to a fresh context. */
   if (conn->session && !should_skip_memory_extraction(conn)) {
      struct json_object *old_history = session_get_history(conn->session);
      if (old_history) {
         int msg_count = json_object_array_length(old_history);
         if (msg_count >= 2) {
            LOG_INFO("WebUI: Triggering memory extraction for conversation %lld before new",
                     (long long)conn->active_conversation_id);
            memory_trigger_extraction(conn->auth_user_id, conn->active_conversation_id, NULL,
                                      old_history, msg_count, 0);
         }
         json_object_put(old_history);
      }
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

      /* NOTE: We intentionally do NOT clear session history here.
       *
       * The client sends "new_conversation" AFTER sending the first text message.
       * The server may have already added that message to session history and started
       * the LLM call. Clearing the history here would wipe out the user's message
       * mid-request, breaking conversation continuity.
       *
       * The session history is the active in-memory context for the LLM.
       * The database conversation is for persistence across sessions.
       * These serve different purposes and should not be coupled.
       *
       * Session history is cleared only when:
       * - User explicitly requests clear_history
       * - User loads a different conversation (load_conversation)
       * - User starts a new chat via UI (which sends clear_history first)
       */

      auth_db_log_event("CONVERSATION_CREATED", conn->username, conn->client_ip,
                        "New conversation");

      /* Update active conversation tracking */
      conn->active_conversation_id = conv_id;
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
 * @brief Clear session history for a fresh start
 *
 * Called when user starts a new conversation to clear the in-memory
 * session history. This prevents stale messages from being sent to
 * new LLM conversations. Sends acknowledgment to client.
 */
void handle_clear_session(ws_connection_t *conn) {
   json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("clear_session_response"));
   json_object *resp_payload = json_object_new_object();

   if (!conn || !conn->session) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error", json_object_new_string("No active session"));
      json_object_object_add(response, "payload", resp_payload);
      if (conn) {
         send_json_response(conn->wsi, response);
      }
      json_object_put(response);
      return;
   }

   session_clear_history(conn->session);

   /* Re-add system prompt for the new conversation */
   char *prompt = build_user_prompt(conn->auth_user_id);
   session_add_message(conn->session, "system", prompt ? prompt : get_remote_command_prompt());
   free(prompt);

   json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn->wsi, response);
   json_object_put(response);

   LOG_INFO("WebUI: Session history cleared for user '%s'",
            conn->username ? conn->username : "unknown");
}

/**
 * @brief Continue a conversation (after context compaction)
 *
 * Archives the current conversation and creates a new one linked to it.
 * Called by the client when server notifies that context was compacted.
 */
void handle_continue_conversation(ws_connection_t *conn, struct json_object *payload) {
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

/* Callback for message enumeration - includes message ID for pagination */
static int load_msg_callback(const conversation_message_t *msg, void *context) {
   json_object *msg_array = (json_object *)context;
   json_object *msg_obj = json_object_new_object();

   json_object_object_add(msg_obj, "id", json_object_new_int64(msg->id));
   json_object_object_add(msg_obj, "role", json_object_new_string(msg->role));
   json_object_object_add(msg_obj, "content",
                          json_object_new_string(msg->content ? msg->content : ""));
   json_object_object_add(msg_obj, "created_at", json_object_new_int64(msg->created_at));

   json_object_array_add(msg_array, msg_obj);
   return 0;
}

/* Default page size for message pagination */
#define MESSAGE_PAGE_SIZE 50

/**
 * @brief Reverse a JSON array in place
 *
 * Messages come from DB in reverse order (newest first for cursor pagination)
 * but need to be displayed oldest first.
 */
static void reverse_json_array(json_object *array) {
   int len = json_object_array_length(array);
   for (int i = 0; i < len / 2; i++) {
      json_object *a = json_object_array_get_idx(array, i);
      json_object *b = json_object_array_get_idx(array, len - 1 - i);
      /* Increment refs before replacing */
      json_object_get(a);
      json_object_get(b);
      json_object_array_put_idx(array, i, b);
      json_object_array_put_idx(array, len - 1 - i, a);
   }
}

/**
 * @brief Load a conversation and its messages with pagination
 *
 * Supports cursor-based pagination for efficient "scroll up to load more":
 * - Initial load: Returns latest MESSAGE_PAGE_SIZE messages
 * - Load more: Pass before_id to get older messages
 *
 * Response includes:
 * - messages: Array of messages (oldest first within the page)
 * - total: Total message count in conversation
 * - has_more: Whether there are older messages to load
 * - oldest_id: ID of oldest message in response (use as before_id for next request)
 */
void handle_load_conversation(ws_connection_t *conn, struct json_object *payload) {
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

   /* Get pagination parameters (optional) */
   int limit = MESSAGE_PAGE_SIZE;
   int64_t before_id = 0;

   json_object *limit_obj;
   if (json_object_object_get_ex(payload, "limit", &limit_obj)) {
      int requested_limit = json_object_get_int(limit_obj);
      if (requested_limit > 0 && requested_limit <= 200) {
         limit = requested_limit;
      }
   }

   json_object *before_obj;
   if (json_object_object_get_ex(payload, "before_id", &before_obj)) {
      before_id = json_object_get_int64(before_obj);
   }

   bool is_load_more = (before_id > 0);
   bool needs_session_context = !is_load_more && conn->session;

   /* Trigger memory extraction for old conversation before switching (async, non-blocking).
    * Only triggers on actual conversation switch, not pagination or reloading same conversation. */
   if (!is_load_more && conn->active_conversation_id != conv_id && conn->session &&
       !should_skip_memory_extraction(conn)) {
      struct json_object *old_history = session_get_history(conn->session);
      if (old_history) {
         int msg_count = json_object_array_length(old_history);
         if (msg_count >= 2) {
            LOG_INFO("WebUI: Triggering memory extraction for conversation %lld before switch",
                     (long long)conn->active_conversation_id);
            memory_trigger_extraction(conn->auth_user_id, conn->active_conversation_id, NULL,
                                      old_history, msg_count, 0);
         }
         json_object_put(old_history);
      }
   }

   /* Get conversation metadata */
   conversation_t conv;
   int result = conv_db_get(conv_id, conn->auth_user_id, &conv);

   if (result == AUTH_DB_SUCCESS) {
      json_object *msg_array = json_object_new_array();
      json_object *all_msgs = NULL; /* For session context restoration */
      int total_messages = 0;
      int returned_count = 0;
      int64_t oldest_id = 0;
      bool has_more = false;

      /* Optimization: For initial load of non-archived conversations that need session context,
       * fetch ALL messages once and use them for both session context and UI display.
       * This avoids the previous double-fetch pattern. */
      if (needs_session_context && !conv.is_archived) {
         /* Fetch all messages in one query */
         all_msgs = json_object_new_array();
         result = conv_db_get_messages(conv_id, conn->auth_user_id, load_msg_callback, all_msgs);

         if (result == AUTH_DB_SUCCESS) {
            total_messages = json_object_array_length(all_msgs);

            /* Extract last 'limit' messages for UI display */
            int start_idx = (total_messages > limit) ? (total_messages - limit) : 0;
            for (int i = start_idx; i < total_messages; i++) {
               json_object *msg = json_object_array_get_idx(all_msgs, i);
               json_object_get(msg); /* Increment ref count before adding to new array */
               json_object_array_add(msg_array, msg);
            }
            returned_count = json_object_array_length(msg_array);

            /* Get oldest message ID for cursor (first message in display array) */
            if (returned_count > 0) {
               json_object *first_msg = json_object_array_get_idx(msg_array, 0);
               json_object *id_field;
               if (json_object_object_get_ex(first_msg, "id", &id_field)) {
                  oldest_id = json_object_get_int64(id_field);
               }
            }

            /* has_more is true if we couldn't show all messages */
            has_more = (total_messages > limit);
         }
      } else {
         /* For load-more requests or archived/no-session cases, use paginated query.
          * Fetch limit+1 to determine has_more accurately (avoid false positive when
          * exactly 'limit' messages remain). */
         result = conv_db_get_messages_paginated(conv_id, conn->auth_user_id, limit + 1, before_id,
                                                 load_msg_callback, msg_array, &total_messages);

         if (result == AUTH_DB_SUCCESS) {
            returned_count = json_object_array_length(msg_array);

            /* Determine has_more based on whether we got the extra message */
            if (returned_count > limit) {
               /* Got extra message, so there are definitely more */
               has_more = true;
               /* Remove the extra message (it's at position 0 since DB returns newest-first) */
               json_object_array_del_idx(msg_array, 0, 1);
               returned_count = limit;
            } else {
               /* Didn't get extra, so this is the last page */
               has_more = false;
            }

            /* Messages come from DB newest-first, reverse for display (oldest first) */
            reverse_json_array(msg_array);

            /* Get oldest message ID for cursor (after reversal, it's the first message) */
            if (returned_count > 0) {
               json_object *first_msg = json_object_array_get_idx(msg_array, 0);
               json_object *id_field;
               if (json_object_object_get_ex(first_msg, "id", &id_field)) {
                  oldest_id = json_object_get_int64(id_field);
               }
            }
         }
      }

      if (result == AUTH_DB_SUCCESS) {
         /* Restore to session context on initial load of non-archived conversations */
         if (all_msgs && !conv.is_archived && conn->session) {
            int all_count = json_object_array_length(all_msgs);

            /* Check if first message is a system prompt */
            bool has_system_prompt = false;
            if (all_count > 0) {
               json_object *first_msg = json_object_array_get_idx(all_msgs, 0);
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

            /* If this is a continuation conversation, inject the compaction summary */
            if (conv.compaction_summary && strlen(conv.compaction_summary) > 0) {
               char summary_msg[4096];
               snprintf(summary_msg, sizeof(summary_msg),
                        "Previous conversation context (summarized): %s", conv.compaction_summary);
               session_add_message(conn->session, "system", summary_msg);
               LOG_INFO("WebUI: Injected compaction summary into session context");
            }

            /* Add all stored messages to session context */
            for (int i = 0; i < all_count; i++) {
               json_object *msg = json_object_array_get_idx(all_msgs, i);
               json_object *role_obj, *content_obj;
               if (json_object_object_get_ex(msg, "role", &role_obj) &&
                   json_object_object_get_ex(msg, "content", &content_obj)) {
                  session_add_message(conn->session, json_object_get_string(role_obj),
                                      json_object_get_string(content_obj));
               }
            }
            LOG_INFO(
                "WebUI: Restored %d messages to session %u context (single-fetch optimization)",
                all_count, conn->session->session_id);

            /* Apply stored LLM settings to session (if any were locked) */
            if (conv.llm_type[0] != '\0' || conv.tools_mode[0] != '\0') {
               session_llm_config_t cfg;
               session_get_llm_config(conn->session, &cfg);

               if (conv.llm_type[0] != '\0') {
                  if (strcmp(conv.llm_type, "local") == 0) {
                     cfg.type = LLM_LOCAL;
                  } else if (strcmp(conv.llm_type, "cloud") == 0) {
                     cfg.type = LLM_CLOUD;
                  }
               }
               if (conv.cloud_provider[0] != '\0') {
                  if (strcmp(conv.cloud_provider, "openai") == 0) {
                     cfg.cloud_provider = CLOUD_PROVIDER_OPENAI;
                  } else if (strcmp(conv.cloud_provider, "claude") == 0) {
                     cfg.cloud_provider = CLOUD_PROVIDER_CLAUDE;
                  } else if (strcmp(conv.cloud_provider, "gemini") == 0) {
                     cfg.cloud_provider = CLOUD_PROVIDER_GEMINI;
                  }
               }
               if (conv.model[0] != '\0') {
                  strncpy(cfg.model, conv.model, sizeof(cfg.model) - 1);
                  cfg.model[sizeof(cfg.model) - 1] = '\0';

                  /* Infer provider from model name if not explicitly stored
                   * (for conversations created before cloud_provider was saved) */
                  if (conv.cloud_provider[0] == '\0') {
                     if (strncmp(conv.model, "gpt-", 4) == 0 ||
                         strncmp(conv.model, "o1-", 3) == 0 || strncmp(conv.model, "o3-", 3) == 0) {
                        cfg.cloud_provider = CLOUD_PROVIDER_OPENAI;
                        LOG_INFO("WebUI: Inferred OpenAI provider from model '%s'", conv.model);
                     } else if (strncmp(conv.model, "claude-", 7) == 0) {
                        cfg.cloud_provider = CLOUD_PROVIDER_CLAUDE;
                        LOG_INFO("WebUI: Inferred Claude provider from model '%s'", conv.model);
                     } else if (strncmp(conv.model, "gemini-", 7) == 0) {
                        cfg.cloud_provider = CLOUD_PROVIDER_GEMINI;
                        LOG_INFO("WebUI: Inferred Gemini provider from model '%s'", conv.model);
                     }
                  }
               }
               if (conv.tools_mode[0] != '\0') {
                  strncpy(cfg.tool_mode, conv.tools_mode, sizeof(cfg.tool_mode) - 1);
                  cfg.tool_mode[sizeof(cfg.tool_mode) - 1] = '\0';
               }

               session_set_llm_config(conn->session, &cfg);
               LOG_INFO("WebUI: Applied stored LLM config (type=%s, model=%s, tools=%s)",
                        conv.llm_type, conv.model, conv.tools_mode);
            }
         }

         /* Free all_msgs if it was allocated */
         if (all_msgs) {
            json_object_put(all_msgs);
         }

         if (conv.is_archived && !is_load_more) {
            LOG_INFO("WebUI: Loaded archived conversation %lld (read-only)", (long long)conv.id);
         }

         /* Build response */
         json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
         json_object_object_add(resp_payload, "conversation_id", json_object_new_int64(conv.id));
         json_object_object_add(resp_payload, "messages", msg_array);

         /* Pagination info */
         json_object_object_add(resp_payload, "total", json_object_new_int(total_messages));
         json_object_object_add(resp_payload, "has_more", json_object_new_boolean(has_more));
         json_object_object_add(resp_payload, "oldest_id", json_object_new_int64(oldest_id));
         json_object_object_add(resp_payload, "is_load_more",
                                json_object_new_boolean(is_load_more));

         /* Only include metadata on initial load, not on load-more requests */
         if (!is_load_more) {
            json_object_object_add(resp_payload, "is_archived",
                                   json_object_new_boolean(conv.is_archived));
            json_object_object_add(resp_payload, "title", json_object_new_string(conv.title));
            json_object_object_add(resp_payload, "message_count",
                                   json_object_new_int(total_messages));
            json_object_object_add(resp_payload, "context_tokens",
                                   json_object_new_int(conv.context_tokens));
            json_object_object_add(resp_payload, "context_max",
                                   json_object_new_int(conv.context_max));

            /* Per-conversation LLM settings */
            json_object *llm_settings = json_object_new_object();
            json_object_object_add(llm_settings, "llm_type",
                                   json_object_new_string(conv.llm_type[0] ? conv.llm_type : ""));
            json_object_object_add(llm_settings, "cloud_provider",
                                   json_object_new_string(
                                       conv.cloud_provider[0] ? conv.cloud_provider : ""));
            json_object_object_add(llm_settings, "model",
                                   json_object_new_string(conv.model[0] ? conv.model : ""));
            json_object_object_add(llm_settings, "tools_mode",
                                   json_object_new_string(conv.tools_mode[0] ? conv.tools_mode
                                                                             : ""));
            json_object_object_add(llm_settings, "thinking_mode",
                                   json_object_new_string(conv.thinking_mode[0] ? conv.thinking_mode
                                                                                : ""));
            json_object_object_add(resp_payload, "llm_settings", llm_settings);

            json_object_object_add(resp_payload, "llm_locked",
                                   json_object_new_boolean(total_messages > 0));

            /* Privacy flag */
            json_object_object_add(resp_payload, "is_private",
                                   json_object_new_boolean(conv.is_private));

            /* Continuation data */
            if (conv.continued_from > 0) {
               json_object_object_add(resp_payload, "continued_from",
                                      json_object_new_int64(conv.continued_from));
               if (conv.compaction_summary) {
                  json_object_object_add(resp_payload, "compaction_summary",
                                         json_object_new_string(conv.compaction_summary));
               }
            }

            /* For archived conversations, find continuation ID */
            if (conv.is_archived) {
               int64_t continuation_id = 0;
               if (conv_db_find_continuation(conv.id, conn->auth_user_id, &continuation_id) ==
                       AUTH_DB_SUCCESS &&
                   continuation_id > 0) {
                  json_object_object_add(resp_payload, "continued_by",
                                         json_object_new_int64(continuation_id));
               }
            }
         }

         /* Update active conversation tracking (only on initial load, not load-more) */
         if (!is_load_more) {
            conn->active_conversation_id = conv_id;
            conn->active_conversation_private = conv.is_private;
         }

         json_object_object_add(response, "payload", resp_payload);
         send_json_response(conn->wsi, response);
         json_object_put(response);
         conv_free(&conv);
         return;
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
void handle_delete_conversation(ws_connection_t *conn, struct json_object *payload) {
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
void handle_rename_conversation(ws_connection_t *conn, struct json_object *payload) {
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
 * @brief Set private mode for a conversation
 *
 * Private conversations are excluded from memory extraction.
 */
void handle_set_private(ws_connection_t *conn, struct json_object *payload) {
   if (!conn_require_auth(conn)) {
      return;
   }

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("set_private_response"));
   json_object *resp_payload = json_object_new_object();

   /* Get conversation ID and private flag */
   json_object *id_obj, *private_obj;
   if (!json_object_object_get_ex(payload, "conversation_id", &id_obj) ||
       !json_object_object_get_ex(payload, "is_private", &private_obj)) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Missing conversation_id or is_private"));
      json_object_object_add(response, "payload", resp_payload);
      send_json_response(conn->wsi, response);
      json_object_put(response);
      return;
   }

   int64_t conv_id = json_object_get_int64(id_obj);
   bool is_private = json_object_get_boolean(private_obj);

   int result = conv_db_set_private(conv_id, conn->auth_user_id, is_private);

   if (result == AUTH_DB_SUCCESS) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
      json_object_object_add(resp_payload, "conversation_id", json_object_new_int64(conv_id));
      json_object_object_add(resp_payload, "is_private", json_object_new_boolean(is_private));
      json_object_object_add(resp_payload, "message",
                             json_object_new_string(is_private ? "Conversation marked private"
                                                               : "Conversation marked public"));

      /* Update active conversation tracking if this is the current conversation */
      if (conn->active_conversation_id == conv_id) {
         conn->active_conversation_private = is_private;
      }
   } else if (result == AUTH_DB_NOT_FOUND) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Conversation not found"));
   } else {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Failed to update privacy"));
   }

   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn->wsi, response);
   json_object_put(response);
}

/**
 * @brief Search conversations by title or content
 */
void handle_search_conversations(ws_connection_t *conn, struct json_object *payload) {
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

/**
 * @brief Save a message to a conversation
 */
void handle_save_message(ws_connection_t *conn, struct json_object *payload) {
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

   /* SECURITY: Validate any embedded image thumbnails (size limit, safe prefix) */
   if (!validate_image_marker(content)) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Invalid or oversized image data"));
      json_object_object_add(response, "payload", resp_payload);
      send_json_response(conn->wsi, response);
      json_object_put(response);
      return;
   }

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
void handle_update_context(ws_connection_t *conn, struct json_object *payload) {
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

/**
 * @brief Lock LLM settings for a conversation
 *
 * Called when the first message is sent in a conversation.
 * Stores the current LLM settings and locks them for the conversation's lifetime.
 */
void handle_lock_conversation_llm(ws_connection_t *conn, struct json_object *payload) {
   if (!conn_require_auth(conn)) {
      return;
   }

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type",
                          json_object_new_string("lock_conversation_llm_response"));
   json_object *resp_payload = json_object_new_object();

   /* Get conversation ID */
   json_object *conv_id_obj;
   if (!json_object_object_get_ex(payload, "conversation_id", &conv_id_obj)) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Missing conversation_id"));
      json_object_object_add(response, "payload", resp_payload);
      send_json_response(conn->wsi, response);
      json_object_put(response);
      return;
   }
   int64_t conv_id = json_object_get_int64(conv_id_obj);

   /* Get LLM settings object */
   json_object *settings_obj;
   if (!json_object_object_get_ex(payload, "llm_settings", &settings_obj)) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error", json_object_new_string("Missing llm_settings"));
      json_object_object_add(response, "payload", resp_payload);
      send_json_response(conn->wsi, response);
      json_object_put(response);
      return;
   }

   /* Extract settings fields */
   const char *llm_type = NULL;
   const char *cloud_provider = NULL;
   const char *model = NULL;
   const char *tools_mode = NULL;
   const char *thinking_mode = NULL;

   json_object *val;
   if (json_object_object_get_ex(settings_obj, "llm_type", &val)) {
      llm_type = json_object_get_string(val);
   }
   if (json_object_object_get_ex(settings_obj, "cloud_provider", &val)) {
      cloud_provider = json_object_get_string(val);
   }
   if (json_object_object_get_ex(settings_obj, "model", &val)) {
      model = json_object_get_string(val);
   }
   if (json_object_object_get_ex(settings_obj, "tools_mode", &val)) {
      tools_mode = json_object_get_string(val);
   }
   if (json_object_object_get_ex(settings_obj, "thinking_mode", &val)) {
      thinking_mode = json_object_get_string(val);
   }

   /* Validate input lengths against database field sizes */
   if ((llm_type && strlen(llm_type) > 15) || (cloud_provider && strlen(cloud_provider) > 15) ||
       (model && strlen(model) > 63) || (tools_mode && strlen(tools_mode) > 15) ||
       (thinking_mode && strlen(thinking_mode) > 15)) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error", json_object_new_string("Field value too long"));
      json_object_object_add(response, "payload", resp_payload);
      send_json_response(conn->wsi, response);
      json_object_put(response);
      return;
   }

   /* Lock settings in database (only works if message_count == 0) */
   int result = conv_db_lock_llm_settings(conv_id, conn->auth_user_id, llm_type, cloud_provider,
                                          model, tools_mode, thinking_mode);

   if (result == AUTH_DB_SUCCESS) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
      json_object_object_add(resp_payload, "locked", json_object_new_boolean(1));
      LOG_INFO("WebUI: Locked LLM settings for conversation %lld (user %d)", (long long)conv_id,
               conn->auth_user_id);
   } else if (result == AUTH_DB_NOT_FOUND) {
      /* Conversation already has messages - settings already locked */
      json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
      json_object_object_add(resp_payload, "locked", json_object_new_boolean(1));
      json_object_object_add(resp_payload, "already_locked", json_object_new_boolean(1));
   } else {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Failed to lock settings"));
   }

   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn->wsi, response);
   json_object_put(response);
}

/**
 * @brief Reassign a conversation to a different user (admin only)
 */
void handle_reassign_conversation(ws_connection_t *conn, struct json_object *payload) {
   if (!conn_require_admin(conn)) {
      return;
   }

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type",
                          json_object_new_string("reassign_conversation_response"));
   json_object *resp_payload = json_object_new_object();

   /* Get required fields */
   json_object *conv_id_obj, *new_user_id_obj;
   if (!json_object_object_get_ex(payload, "conversation_id", &conv_id_obj) ||
       !json_object_object_get_ex(payload, "new_user_id", &new_user_id_obj)) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Missing conversation_id or new_user_id"));
      json_object_object_add(response, "payload", resp_payload);
      send_json_response(conn->wsi, response);
      json_object_put(response);
      return;
   }

   int64_t conv_id = json_object_get_int64(conv_id_obj);
   int new_user_id = json_object_get_int(new_user_id_obj);

   if (conv_id <= 0 || new_user_id <= 0) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Invalid conversation_id or user_id"));
      json_object_object_add(response, "payload", resp_payload);
      send_json_response(conn->wsi, response);
      json_object_put(response);
      return;
   }

   /* Perform the reassignment */
   int result = conv_db_reassign(conv_id, new_user_id);

   if (result == AUTH_DB_SUCCESS) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
      json_object_object_add(resp_payload, "conversation_id", json_object_new_int64(conv_id));
      json_object_object_add(resp_payload, "new_user_id", json_object_new_int(new_user_id));
      json_object_object_add(resp_payload, "message",
                             json_object_new_string("Conversation reassigned successfully"));
      LOG_INFO("WebUI: Admin %s reassigned conversation %lld to user %d", conn->username,
               (long long)conv_id, new_user_id);
   } else if (result == AUTH_DB_NOT_FOUND) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Conversation not found"));
   } else {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Failed to reassign conversation"));
   }

   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn->wsi, response);
   json_object_put(response);
}
