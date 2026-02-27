#define _GNU_SOURCE /* For memmem, strcasestr */

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
 * WebUI Document Upload Handlers
 *
 * Handles POST /api/documents for plain text document upload.
 * Extracts text content server-side and returns it in a JSON response.
 * Uses json-c for safe JSON construction (handles escaping of quotes,
 * backslashes, newlines in document content).
 */

#include "webui/webui_documents.h"

#include <ctype.h>
#include <json-c/json.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "logging.h"
#include "utils/string_utils.h"

/* =============================================================================
 * Session Data (forward-declared in header)
 * ============================================================================= */

struct document_upload_session {
   char boundary[128];
   char filename[256];
   char *content_buf;
   size_t content_len;
   size_t content_cap;
   bool headers_parsed;
};

/* =============================================================================
 * Allowed Extensions (Phase 1: plain text only)
 * ============================================================================= */

static const char *allowed_extensions[] = {
   ".txt", ".md",   ".csv",  ".json", ".c",   ".h",   ".cpp", ".py",    ".js",   ".ts",
   ".sh",  ".toml", ".yaml", ".yml",  ".xml", ".log", ".cfg", ".ini",   ".conf", ".rs",
   ".go",  ".java", ".rb",   ".html", ".css", ".sql", ".mk",  ".cmake",
};

static const size_t allowed_extensions_count = sizeof(allowed_extensions) /
                                               sizeof(allowed_extensions[0]);

/* =============================================================================
 * Internal Helpers
 * ============================================================================= */

/**
 * @brief Send JSON error response for document upload
 */
static int send_doc_error(struct lws *wsi, int status, const char *error) {
   char body[256];
   int body_len = snprintf(body, sizeof(body), "{\"error\":\"%s\"}", error);

   unsigned char buffer[LWS_PRE + 512];
   unsigned char *start = &buffer[LWS_PRE];
   unsigned char *p = start;
   unsigned char *end = &buffer[sizeof(buffer) - 1];

   if (lws_add_http_header_status(wsi, (unsigned int)status, &p, end))
      return -1;
   if (lws_add_http_header_by_token(wsi, WSI_TOKEN_HTTP_CONTENT_TYPE,
                                    (unsigned char *)"application/json", 16, &p, end))
      return -1;
   if (lws_add_http_header_content_length(wsi, (unsigned long)body_len, &p, end))
      return -1;
   if (lws_finalize_http_header(wsi, &p, end))
      return -1;

   int n = lws_write(wsi, start, (size_t)(p - start), LWS_WRITE_HTTP_HEADERS);
   if (n < 0)
      return -1;

   n = lws_write(wsi, (unsigned char *)body, (size_t)body_len, LWS_WRITE_HTTP);
   if (n < 0)
      return -1;

   return -1; /* Close connection */
}

/**
 * @brief Check if a file extension is in the allowed list (case-insensitive)
 */
static bool is_extension_allowed(const char *ext) {
   if (!ext || *ext == '\0')
      return false;

   for (size_t i = 0; i < allowed_extensions_count; i++) {
      if (strcasecmp(ext, allowed_extensions[i]) == 0)
         return true;
   }
   return false;
}

/**
 * @brief Get file extension from filename (including the dot)
 */
static const char *get_extension(const char *filename) {
   if (!filename)
      return NULL;

   const char *dot = strrchr(filename, '.');
   if (!dot || dot == filename)
      return NULL;

   return dot;
}

/**
 * @brief Extract boundary from Content-Type header
 */
static bool extract_doc_boundary(const char *content_type, char *boundary, size_t boundary_size) {
   const char *boundary_start = strstr(content_type, "boundary=");
   if (!boundary_start)
      return false;

   boundary_start += 9; /* Skip "boundary=" */

   if (*boundary_start == '"') {
      boundary_start++;
      const char *end = strchr(boundary_start, '"');
      if (!end)
         return false;
      size_t len = (size_t)(end - boundary_start);
      if (len >= boundary_size)
         return false;
      memcpy(boundary, boundary_start, len);
      boundary[len] = '\0';
   } else {
      size_t i = 0;
      while (boundary_start[i] && boundary_start[i] != ';' && boundary_start[i] != ' ' &&
             boundary_start[i] != '\r' && boundary_start[i] != '\n') {
         if (i >= boundary_size - 1)
            return false;
         boundary[i] = boundary_start[i];
         i++;
      }
      boundary[i] = '\0';
   }

   return strlen(boundary) > 0;
}

/**
 * @brief Extract filename from Content-Disposition header within multipart data
 */
static bool extract_filename(const char *headers,
                             size_t headers_len,
                             char *filename,
                             size_t filename_size) {
   /* Look for filename="..." in the headers */
   const char *fn_start = (const char *)memmem(headers, headers_len, "filename=\"", 10);
   if (!fn_start)
      return false;

   fn_start += 10; /* Skip 'filename="' */
   const char *fn_end = memchr(fn_start, '"', (size_t)(headers + headers_len - fn_start));
   if (!fn_end)
      return false;

   size_t fn_len = (size_t)(fn_end - fn_start);

   /* Check for embedded null bytes (security) */
   if (memchr(fn_start, '\0', fn_len))
      return false;

   /* Strip path components (both Unix and Windows paths) */
   const char *basename = fn_start;
   for (const char *c = fn_start; c < fn_end; c++) {
      if (*c == '/' || *c == '\\')
         basename = c + 1;
   }
   fn_len = (size_t)(fn_end - basename);

   /* Truncate to display size */
   if (fn_len >= filename_size)
      fn_len = filename_size - 1;

   memcpy(filename, basename, fn_len);
   filename[fn_len] = '\0';

   return fn_len > 0;
}

/**
 * @brief Parse multipart form data to extract document content and filename
 */
static bool parse_doc_multipart(document_upload_session_t *session) {
   char full_boundary[136];
   snprintf(full_boundary, sizeof(full_boundary), "--%s", session->boundary);
   size_t boundary_len = strlen(full_boundary);

   const char *data = session->content_buf;
   size_t data_len = session->content_len;

   /* Find first boundary */
   const char *part_start = (const char *)memmem(data, data_len, full_boundary, boundary_len);
   if (!part_start)
      return false;

   /* Move past boundary and CRLF */
   part_start += boundary_len;
   if (part_start + 2 > data + data_len)
      return false;
   if (part_start[0] == '\r' && part_start[1] == '\n')
      part_start += 2;

   /* Find headers end (blank line) */
   const char *headers_end = (const char *)memmem(part_start,
                                                  (size_t)(data + data_len - part_start),
                                                  "\r\n\r\n", 4);
   if (!headers_end)
      return false;

   size_t headers_len = (size_t)(headers_end - part_start);

   /* Extract filename from Content-Disposition */
   extract_filename(part_start, headers_len, session->filename, sizeof(session->filename));

   /* Content starts after headers */
   const char *content_start = headers_end + 4;

   /* Find end boundary */
   char end_boundary[140];
   snprintf(end_boundary, sizeof(end_boundary), "\r\n--%s", session->boundary);
   size_t end_boundary_len = strlen(end_boundary);

   const char *content_end = (const char *)memmem(content_start,
                                                  (size_t)(data + data_len - content_start),
                                                  end_boundary, end_boundary_len);
   if (!content_end)
      return false;

   size_t content_len = (size_t)(content_end - content_start);

   /* Move content to beginning of buffer (overwrite multipart framing) */
   memmove(session->content_buf, content_start, content_len);
   session->content_buf[content_len] = '\0';
   session->content_len = content_len;

   return true;
}

/**
 * @brief Send JSON success response with extracted document content
 *
 * Uses json-c for safe JSON construction (handles escaping).
 */
static int send_doc_success(struct lws *wsi, document_upload_session_t *session, const char *ext) {
   /* Build JSON response using json-c */
   json_object *resp = json_object_new_object();
   if (!resp) {
      LOG_ERROR("webui_documents: json_object_new_object failed");
      return send_doc_error(wsi, HTTP_STATUS_INTERNAL_SERVER_ERROR, "JSON construction failed");
   }

   json_object_object_add(resp, "success", json_object_new_boolean(1));
   json_object_object_add(resp, "filename", json_object_new_string(session->filename));

   /* Capture size before freeing buffer */
   size_t content_size = session->content_len;

   /* Create content string object, then free raw buffer to reduce peak memory */
   json_object *content_obj = json_object_new_string(session->content_buf);
   free(session->content_buf);
   session->content_buf = NULL;
   session->content_len = 0;
   json_object_object_add(resp, "content", content_obj);

   json_object_object_add(resp, "size", json_object_new_int64((int64_t)content_size));
   json_object_object_add(resp, "type", json_object_new_string(ext ? ext : ""));

   const char *json_str = json_object_to_json_string_ext(resp, JSON_C_TO_STRING_PLAIN);
   size_t json_len = strlen(json_str);

   /* Allocate response buffer for headers only (body written from json_str directly) */
   size_t buf_size = LWS_PRE + 512;
   unsigned char *buffer = malloc(buf_size);
   if (!buffer) {
      json_object_put(resp);
      return send_doc_error(wsi, HTTP_STATUS_INTERNAL_SERVER_ERROR, "Memory allocation failed");
   }

   unsigned char *start = &buffer[LWS_PRE];
   unsigned char *p = start;
   unsigned char *end = &buffer[buf_size - 1];

   if (lws_add_http_header_status(wsi, HTTP_STATUS_OK, &p, end)) {
      free(buffer);
      json_object_put(resp);
      return -1;
   }
   if (lws_add_http_header_by_token(wsi, WSI_TOKEN_HTTP_CONTENT_TYPE,
                                    (unsigned char *)"application/json", 16, &p, end)) {
      free(buffer);
      json_object_put(resp);
      return -1;
   }
   if (lws_add_http_header_content_length(wsi, (unsigned long)json_len, &p, end)) {
      free(buffer);
      json_object_put(resp);
      return -1;
   }
   if (lws_finalize_http_header(wsi, &p, end)) {
      free(buffer);
      json_object_put(resp);
      return -1;
   }

   /* Write headers */
   int n = lws_write(wsi, start, (size_t)(p - start), LWS_WRITE_HTTP_HEADERS);
   if (n < 0) {
      free(buffer);
      json_object_put(resp);
      return -1;
   }

   /* Write body */
   n = lws_write(wsi, (unsigned char *)json_str, json_len, LWS_WRITE_HTTP);

   free(buffer);
   json_object_put(resp); /* Frees json-c internal buffers including json_str */

   if (n < 0)
      return -1;

   return -1; /* Close connection */
}

/* =============================================================================
 * Public API
 * ============================================================================= */

void webui_documents_session_free(document_upload_session_t *session) {
   if (!session)
      return;

   if (session->content_buf) {
      free(session->content_buf);
      session->content_buf = NULL;
   }
   free(session);
}

int webui_documents_handle_upload_start(struct lws *wsi, document_upload_session_t **session_out) {
   if (!session_out)
      return -1;

   *session_out = NULL;

   /* Get Content-Type header */
   char content_type[256];
   int ct_len = lws_hdr_copy(wsi, content_type, sizeof(content_type), WSI_TOKEN_HTTP_CONTENT_TYPE);
   if (ct_len <= 0) {
      return send_doc_error(wsi, HTTP_STATUS_BAD_REQUEST, "Missing Content-Type");
   }

   /* Must be multipart/form-data */
   if (!strstr(content_type, "multipart/form-data")) {
      return send_doc_error(wsi, HTTP_STATUS_BAD_REQUEST, "Expected multipart/form-data");
   }

   /* Get Content-Length with overflow protection */
   char content_length_str[32];
   int cl_len = lws_hdr_copy(wsi, content_length_str, sizeof(content_length_str),
                             WSI_TOKEN_HTTP_CONTENT_LENGTH);
   size_t content_length = 0;
   if (cl_len > 0) {
      char *endptr;
      long long parsed = strtoll(content_length_str, &endptr, 10);
      if (*endptr != '\0' || parsed < 0 || (size_t)parsed > DOC_MAX_FILE_SIZE + 1024) {
         return send_doc_error(wsi, HTTP_STATUS_REQ_ENTITY_TOO_LARGE,
                               "File too large (max 512 KB)");
      }
      content_length = (size_t)parsed;
   }

   /* Allocate session */
   document_upload_session_t *session = calloc(1, sizeof(document_upload_session_t));
   if (!session) {
      return send_doc_error(wsi, HTTP_STATUS_INTERNAL_SERVER_ERROR, "Memory allocation failed");
   }

   /* Extract boundary */
   if (!extract_doc_boundary(content_type, session->boundary, sizeof(session->boundary))) {
      free(session);
      return send_doc_error(wsi, HTTP_STATUS_BAD_REQUEST, "Missing multipart boundary");
   }

   /* Allocate initial data buffer */
   session->content_cap = (content_length > 0 && content_length <= DOC_MAX_FILE_SIZE + 1024)
                              ? content_length
                              : 32 * 1024; /* Start with 32KB */
   session->content_buf = malloc(session->content_cap);
   if (!session->content_buf) {
      free(session);
      return send_doc_error(wsi, HTTP_STATUS_INTERNAL_SERVER_ERROR, "Memory allocation failed");
   }

   *session_out = session;
   return 0; /* Continue to body callbacks */
}

int webui_documents_handle_upload_body(struct lws *wsi,
                                       document_upload_session_t *session,
                                       const char *data,
                                       size_t len) {
   (void)wsi;

   if (!session || !data || len == 0)
      return 0;

   /* Check size limit (content + multipart overhead) */
   if (session->content_len + len > DOC_MAX_FILE_SIZE + 1024) {
      LOG_WARNING("webui_documents: upload exceeds size limit (%zu + %zu)", session->content_len,
                  len);
      return -1;
   }

   /* Grow buffer if needed */
   if (session->content_len + len > session->content_cap) {
      size_t new_cap = session->content_cap * 2;
      if (new_cap < session->content_len + len)
         new_cap = session->content_len + len;
      if (new_cap > DOC_MAX_FILE_SIZE + 1024)
         new_cap = DOC_MAX_FILE_SIZE + 1024;

      char *new_buf = realloc(session->content_buf, new_cap);
      if (!new_buf) {
         LOG_ERROR("webui_documents: failed to grow buffer");
         return -1;
      }
      session->content_buf = new_buf;
      session->content_cap = new_cap;
   }

   /* Append data */
   memcpy(session->content_buf + session->content_len, data, len);
   session->content_len += len;

   return 0;
}

int webui_documents_handle_upload_complete(struct lws *wsi, document_upload_session_t *session) {
   if (!session) {
      return send_doc_error(wsi, HTTP_STATUS_INTERNAL_SERVER_ERROR, "No session data");
   }

   /* Parse multipart to extract content and filename */
   if (!parse_doc_multipart(session)) {
      webui_documents_session_free(session);
      return send_doc_error(wsi, HTTP_STATUS_BAD_REQUEST, "Invalid multipart data");
   }

   /* Validate filename has an allowed extension */
   const char *ext = get_extension(session->filename);
   if (!is_extension_allowed(ext)) {
      LOG_WARNING("webui_documents: rejected upload with extension: %s (file: %s)",
                  ext ? ext : "(none)", session->filename);
      webui_documents_session_free(session);
      return send_doc_error(wsi, HTTP_STATUS_UNSUPPORTED_MEDIA_TYPE, "Unsupported file type");
   }

   /* Check extracted content size */
   if (session->content_len > DOC_MAX_FILE_SIZE) {
      webui_documents_session_free(session);
      return send_doc_error(wsi, HTTP_STATUS_REQ_ENTITY_TOO_LARGE, "File too large (max 512 KB)");
   }

   /* Null-terminate for string operations */
   if (session->content_len < session->content_cap) {
      session->content_buf[session->content_len] = '\0';
   } else {
      /* Need to grow by 1 for null terminator */
      char *new_buf = realloc(session->content_buf, session->content_len + 1);
      if (!new_buf) {
         webui_documents_session_free(session);
         return send_doc_error(wsi, HTTP_STATUS_INTERNAL_SERVER_ERROR, "Memory allocation failed");
      }
      session->content_buf = new_buf;
      session->content_cap = session->content_len + 1;
      session->content_buf[session->content_len] = '\0';
   }

   /* Sanitize UTF-8 for JSON safety */
   sanitize_utf8_for_json(session->content_buf);
   session->content_len = strlen(session->content_buf);

   LOG_INFO("webui_documents: uploaded %s (%zu bytes, %s)", session->filename, session->content_len,
            ext);

   /* Send JSON response with extracted content */
   int result = send_doc_success(wsi, session, ext);

   webui_documents_session_free(session);
   return result;
}
