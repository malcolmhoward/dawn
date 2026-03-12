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
 * WebUI Document Library Handlers
 *
 * WebSocket handlers for the Document Library panel. Provides list, delete,
 * and index (chunk + embed + store) operations for RAG document search.
 */

#include "webui/webui_doc_library.h"

#include <json-c/json.h>
#include <openssl/sha.h>
#include <string.h>
#include <time.h>

#include "auth/auth_db.h"
#include "config/dawn_config.h"
#include "core/embedding_engine.h"
#include "logging.h"
#include "tools/document_chunker.h"
#include "tools/document_db.h"
#include "webui/webui_internal.h"

/* =============================================================================
 * Helper: quiet admin check (no error response sent)
 * ============================================================================= */

static bool conn_check_admin_quiet(ws_connection_t *conn) {
   if (!conn->authenticated)
      return false;
   auth_session_t session;
   if (auth_db_get_session(conn->auth_session_token, &session) != AUTH_DB_SUCCESS)
      return false;
   return session.is_admin;
}

/* =============================================================================
 * List Documents
 * ============================================================================= */

#define DOC_LIBRARY_DEFAULT_LIMIT 20
#define DOC_LIBRARY_MAX_LIMIT 100

void handle_doc_library_list(ws_connection_t *conn, json_object *payload) {
   if (!conn_require_auth(conn))
      return;

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("doc_library_list_response"));

   json_object *resp_payload = json_object_new_object();

   /* Parse pagination params */
   int limit = DOC_LIBRARY_DEFAULT_LIMIT;
   int offset = 0;
   bool show_all = false;
   if (payload) {
      json_object *limit_obj, *offset_obj, *show_all_obj;
      if (json_object_object_get_ex(payload, "limit", &limit_obj)) {
         limit = json_object_get_int(limit_obj);
         if (limit < 1 || limit > DOC_LIBRARY_MAX_LIMIT)
            limit = DOC_LIBRARY_DEFAULT_LIMIT;
      }
      if (json_object_object_get_ex(payload, "offset", &offset_obj)) {
         offset = json_object_get_int(offset_obj);
         if (offset < 0)
            offset = 0;
      }
      if (json_object_object_get_ex(payload, "show_all", &show_all_obj)) {
         show_all = json_object_get_boolean(show_all_obj);
      }
   }

   /* show_all requires admin — silently fall back to user-only if not admin */
   if (show_all && !conn_check_admin_quiet(conn))
      show_all = false;

   document_t docs[DOC_LIBRARY_DEFAULT_LIMIT]; /* Stack-friendly: sized to typical page */
   if (limit > DOC_LIBRARY_DEFAULT_LIMIT)
      limit = DOC_LIBRARY_DEFAULT_LIMIT;

   int count = show_all ? document_db_list_all(docs, limit, offset)
                        : document_db_list(conn->auth_user_id, docs, limit, offset);

   if (count < 0) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Failed to list documents"));
   } else {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(1));

      json_object *docs_array = json_object_new_array();
      for (int i = 0; i < count; i++) {
         json_object *doc = json_object_new_object();
         json_object_object_add(doc, "id", json_object_new_int64(docs[i].id));
         json_object_object_add(doc, "filename", json_object_new_string(docs[i].filename));
         json_object_object_add(doc, "filetype", json_object_new_string(docs[i].filetype));
         json_object_object_add(doc, "num_chunks", json_object_new_int(docs[i].num_chunks));
         json_object_object_add(doc, "is_global", json_object_new_boolean(docs[i].is_global));
         json_object_object_add(doc, "created_at", json_object_new_int64(docs[i].created_at));
         if (show_all) {
            json_object_object_add(doc, "user_id", json_object_new_int(docs[i].user_id));
            json_object_object_add(doc, "owner_name", json_object_new_string(docs[i].owner_name));
         }
         json_object_array_add(docs_array, doc);
      }
      json_object_object_add(resp_payload, "documents", docs_array);
      json_object_object_add(resp_payload, "count", json_object_new_int(count));
      json_object_object_add(resp_payload, "has_more", json_object_new_boolean(count == limit));
   }

   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn, response);
   json_object_put(response);
}

/* =============================================================================
 * Delete Document
 * ============================================================================= */

void handle_doc_library_delete(ws_connection_t *conn, json_object *payload) {
   if (!conn_require_auth(conn))
      return;

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("doc_library_delete_response"));

   json_object *resp_payload = json_object_new_object();

   json_object *id_obj;
   if (!json_object_object_get_ex(payload, "id", &id_obj)) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error", json_object_new_string("Missing document id"));
   } else {
      int64_t doc_id = json_object_get_int64(id_obj);

      /* Verify ownership: get the doc first */
      document_t doc;
      if (document_db_get(doc_id, &doc) != 0) {
         json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
         json_object_object_add(resp_payload, "error",
                                json_object_new_string("Document not found"));
      } else if (doc.user_id != conn->auth_user_id && !conn_check_admin_quiet(conn)) {
         json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
         json_object_object_add(resp_payload, "error", json_object_new_string("Permission denied"));
      } else {
         int rc = document_db_delete(doc_id);
         json_object_object_add(resp_payload, "success", json_object_new_boolean(rc == 0));
         if (rc != 0) {
            json_object_object_add(resp_payload, "error", json_object_new_string("Delete failed"));
         } else {
            json_object_object_add(resp_payload, "id", json_object_new_int64(doc_id));
            if (doc.user_id != conn->auth_user_id) {
               LOG_INFO("doc_library: admin user %d deleted document %lld (%s) owned by user %d",
                        conn->auth_user_id, (long long)doc_id, doc.filename, doc.user_id);
            } else {
               LOG_INFO("doc_library: deleted document %lld (%s)", (long long)doc_id, doc.filename);
            }
         }
      }
   }

   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn, response);
   json_object_put(response);
}

/* =============================================================================
 * Index Document (chunk + embed + store)
 * ============================================================================= */

static void sha256_hex(const char *data, size_t len, char *out_hex) {
   unsigned char hash[SHA256_DIGEST_LENGTH];
   SHA256((const unsigned char *)data, len, hash);
   for (int i = 0; i < SHA256_DIGEST_LENGTH; i++) {
      sprintf(out_hex + (i * 2), "%02x", hash[i]);
   }
   out_hex[SHA256_DIGEST_LENGTH * 2] = '\0';
}

void handle_doc_library_index(ws_connection_t *conn, json_object *payload) {
   if (!conn_require_auth(conn))
      return;

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("doc_library_index_response"));
   json_object *resp_payload = json_object_new_object();

   /* Extract fields */
   json_object *filename_obj, *filetype_obj, *text_obj, *global_obj;
   if (!json_object_object_get_ex(payload, "filename", &filename_obj) ||
       !json_object_object_get_ex(payload, "filetype", &filetype_obj) ||
       !json_object_object_get_ex(payload, "text", &text_obj)) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Missing required fields"));
      goto done;
   }

   const char *filename = json_object_get_string(filename_obj);
   const char *filetype = json_object_get_string(filetype_obj);
   const char *text = json_object_get_string(text_obj);
   bool is_global = json_object_object_get_ex(payload, "is_global", &global_obj) &&
                    json_object_get_boolean(global_obj);

   /* Only admins can mark documents as global */
   if (is_global && !conn_check_admin_quiet(conn))
      is_global = false;

   size_t text_len = text ? strlen(text) : 0;
   if (text_len == 0) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Document text is empty"));
      goto done;
   }

   if (text_len > (size_t)g_config.documents.max_index_size_kb * 1024) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Document text exceeds maximum size"));
      goto done;
   }

   int user_doc_count = document_db_count_user(conn->auth_user_id);
   if (user_doc_count >= 0 && user_doc_count >= g_config.documents.max_indexed_documents) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Document limit reached"));
      goto done;
   }

   /* Check embedding engine */
   if (!embedding_engine_available()) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Embedding engine not available"));
      goto done;
   }

   /* Compute file hash for dedup */
   char file_hash[65];
   sha256_hex(text, text_len, file_hash);

   /* Check for duplicate */
   int64_t existing = document_db_find_by_hash(file_hash, conn->auth_user_id);
   if (existing > 0) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Document already indexed"));
      json_object_object_add(resp_payload, "existing_id", json_object_new_int64(existing));
      goto done;
   }

   /* Chunk the text */
   chunk_config_t chunk_cfg = CHUNK_CONFIG_DEFAULT;
   chunk_result_t chunks;
   if (document_chunk_text(text, &chunk_cfg, &chunks) != 0 || chunks.count == 0) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Failed to chunk document text"));
      goto done;
   }

   int dims = embedding_engine_dims();

   /* Create document record */
   int64_t doc_id = document_db_create(conn->auth_user_id, filename, filename, filetype, file_hash,
                                       chunks.count, is_global);
   if (doc_id < 0) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Failed to create document record"));
      chunk_result_free(&chunks);
      goto done;
   }

   /* TODO: offload to background thread for large documents */
   /* Embed and store each chunk */
   float *emb_buf = malloc((size_t)dims * sizeof(float));
   int embedded_count = 0;
   int failed_count = 0;

   if (emb_buf) {
      for (int i = 0; i < chunks.count; i++) {
         int out_dims = 0;
         int rc = embedding_engine_embed(chunks.chunks[i], emb_buf, dims, &out_dims);
         if (rc != 0 || out_dims != dims) {
            failed_count++;
            continue;
         }

         float norm = embedding_engine_l2_norm(emb_buf, dims);
         int64_t chunk_id = document_db_chunk_create(doc_id, i, chunks.chunks[i], emb_buf, dims,
                                                     norm);
         if (chunk_id >= 0) {
            embedded_count++;
         } else {
            failed_count++;
         }
      }
      free(emb_buf);
   } else {
      /* Memory allocation failed — delete the document record */
      document_db_delete(doc_id);
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Memory allocation failed"));
      chunk_result_free(&chunks);
      goto done;
   }

   chunk_result_free(&chunks);

   LOG_INFO("doc_library: indexed '%s' — %d chunks embedded, %d failed%s", filename, embedded_count,
            failed_count, is_global ? " [GLOBAL]" : "");

   json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
   json_object_object_add(resp_payload, "id", json_object_new_int64(doc_id));
   json_object_object_add(resp_payload, "filename", json_object_new_string(filename));
   json_object_object_add(resp_payload, "num_chunks", json_object_new_int(embedded_count));
   if (failed_count > 0) {
      json_object_object_add(resp_payload, "failed_chunks", json_object_new_int(failed_count));
   }

done:
   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn, response);
   json_object_put(response);
}

/* =============================================================================
 * Toggle Global Visibility
 * ============================================================================= */

void handle_doc_library_toggle_global(ws_connection_t *conn, json_object *payload) {
   if (!conn_require_auth(conn))
      return;

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type",
                          json_object_new_string("doc_library_toggle_global_response"));
   json_object *resp_payload = json_object_new_object();

   json_object *id_obj, *global_obj;
   if (!json_object_object_get_ex(payload, "id", &id_obj) ||
       !json_object_object_get_ex(payload, "is_global", &global_obj)) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Missing id or is_global"));
   } else {
      int64_t doc_id = json_object_get_int64(id_obj);
      bool new_global = json_object_get_boolean(global_obj);

      document_t doc;
      if (document_db_get(doc_id, &doc) != 0) {
         json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
         json_object_object_add(resp_payload, "error",
                                json_object_new_string("Document not found"));
      } else if (doc.user_id != conn->auth_user_id && !conn_check_admin_quiet(conn)) {
         json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
         json_object_object_add(resp_payload, "error", json_object_new_string("Permission denied"));
      } else {
         int rc = document_db_update_global(doc_id, new_global);
         json_object_object_add(resp_payload, "success", json_object_new_boolean(rc == 0));
         if (rc != 0) {
            json_object_object_add(resp_payload, "error", json_object_new_string("Update failed"));
         } else {
            json_object_object_add(resp_payload, "id", json_object_new_int64(doc_id));
            json_object_object_add(resp_payload, "is_global", json_object_new_boolean(new_global));
            LOG_INFO("doc_library: user %d toggled document %lld (%s) global=%s",
                     conn->auth_user_id, (long long)doc_id, doc.filename,
                     new_global ? "true" : "false");
         }
      }
   }

   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn, response);
   json_object_put(response);
}
