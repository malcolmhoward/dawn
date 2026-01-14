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
#include "core/ocp_helpers.h"
#include "core/session_manager.h"
#include "llm/llm_command_parser.h"
#include "logging.h"
#include "webui/webui_internal.h"

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

/**
 * @brief Create a new conversation
 */
void handle_new_conversation(ws_connection_t *conn, struct json_object *payload) {
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

      /* Clear session history for fresh start and re-add system prompt */
      if (conn->session) {
         session_clear_history(conn->session);
         char *prompt = build_user_prompt(conn->auth_user_id);
         session_add_message(conn->session, "system",
                             prompt ? prompt : get_remote_command_prompt());
         free(prompt);
      }

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

            /* Apply stored LLM settings to session (if any were locked) */
            if (conv.llm_type[0] != '\0' || conv.tools_mode[0] != '\0') {
               session_llm_config_t cfg;
               session_get_llm_config(conn->session, &cfg);

               /* Map stored strings to enums/values */
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
                  }
               }
               if (conv.model[0] != '\0') {
                  strncpy(cfg.model, conv.model, sizeof(cfg.model) - 1);
                  cfg.model[sizeof(cfg.model) - 1] = '\0';
               }
               if (conv.tools_mode[0] != '\0') {
                  strncpy(cfg.tool_mode, conv.tools_mode, sizeof(cfg.tool_mode) - 1);
                  cfg.tool_mode[sizeof(cfg.tool_mode) - 1] = '\0';
               }
               /* Note: thinking_mode is handled per-request, not stored in session */

               session_set_llm_config(conn->session, &cfg);
               LOG_INFO("WebUI: Applied stored LLM config to session (type=%s, model=%s, tools=%s)",
                        conv.llm_type, conv.model, conv.tools_mode);
            }
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

         /* Per-conversation LLM settings (schema v11+) */
         json_object *llm_settings = json_object_new_object();
         json_object_object_add(llm_settings, "llm_type",
                                json_object_new_string(conv.llm_type[0] ? conv.llm_type : ""));
         json_object_object_add(llm_settings, "cloud_provider",
                                json_object_new_string(conv.cloud_provider[0] ? conv.cloud_provider
                                                                              : ""));
         json_object_object_add(llm_settings, "model",
                                json_object_new_string(conv.model[0] ? conv.model : ""));
         json_object_object_add(llm_settings, "tools_mode",
                                json_object_new_string(conv.tools_mode[0] ? conv.tools_mode : ""));
         json_object_object_add(llm_settings, "thinking_mode",
                                json_object_new_string(conv.thinking_mode[0] ? conv.thinking_mode
                                                                             : ""));
         json_object_object_add(resp_payload, "llm_settings", llm_settings);

         /* Settings are locked if conversation has messages (message_count > 0) */
         json_object_object_add(resp_payload, "llm_locked",
                                json_object_new_boolean(total_messages > 0));

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
