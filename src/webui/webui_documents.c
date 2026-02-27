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
 * Handles POST /api/documents for document upload with text extraction.
 * Supports plain text, PDF (MuPDF), DOCX (libzip + libxml2), and HTML.
 * Uses json-c for safe JSON construction (handles escaping of quotes,
 * backslashes, newlines in document content).
 */

#include "webui/webui_documents.h"

#include <ctype.h>
#include <json-c/json.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "config/dawn_config.h"
#include "logging.h"
#include "tools/html_parser.h"
#include "tools/tfidf_summarizer.h"
#include "utils/string_utils.h"

#ifdef HAVE_MUPDF
#include <mupdf/fitz.h>
#endif

#ifdef HAVE_LIBZIP
#include <zip.h>
#endif

#if defined(HAVE_LIBZIP) && defined(HAVE_LIBXML2)
#include <libxml/parser.h>
#include <libxml/tree.h>
#endif

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
   int page_count; /* PDF page count, -1 if not applicable */
   /* Config snapshots captured at upload_start to avoid TOCTOU */
   size_t max_file_size;      /* bytes */
   size_t max_extracted_size; /* bytes */
   int max_pages;
};

/* =============================================================================
 * Allowed Extensions
 * ============================================================================= */

static const char *allowed_extensions[] = {
   ".txt",  ".md",   ".csv",  ".json", ".c",   ".h",   ".cpp", ".py",  ".js",    ".ts",
   ".sh",   ".toml", ".yaml", ".yml",  ".xml", ".log", ".cfg", ".ini", ".conf",  ".rs",
   ".go",   ".java", ".rb",   ".html", ".htm", ".css", ".sql", ".mk",  ".cmake",
#ifdef HAVE_MUPDF
   ".pdf",
#endif
#if defined(HAVE_LIBZIP) && defined(HAVE_LIBXML2)
   ".docx",
#endif
};

static const size_t allowed_extensions_count = sizeof(allowed_extensions) /
                                               sizeof(allowed_extensions[0]);

/* =============================================================================
 * Internal Helpers
 * ============================================================================= */

/**
 * @brief Send JSON error response for document upload (uses json-c for safe escaping)
 */
static int send_doc_error(struct lws *wsi, int status, const char *error) {
   json_object *err_obj = json_object_new_object();
   if (!err_obj) {
      /* Fallback: bare minimum response */
      lws_return_http_status(wsi, (unsigned int)status, error);
      return -1;
   }

   json_object_object_add(err_obj, "error", json_object_new_string(error));
   const char *json_str = json_object_to_json_string_ext(err_obj, JSON_C_TO_STRING_PLAIN);
   size_t json_len = strlen(json_str);

   unsigned char buffer[LWS_PRE + 512];
   unsigned char *start = &buffer[LWS_PRE];
   unsigned char *p = start;
   unsigned char *end = &buffer[sizeof(buffer) - 1];

   if (lws_add_http_header_status(wsi, (unsigned int)status, &p, end) ||
       lws_add_http_header_by_token(wsi, WSI_TOKEN_HTTP_CONTENT_TYPE,
                                    (unsigned char *)"application/json", 16, &p, end) ||
       lws_add_http_header_content_length(wsi, (unsigned long)json_len, &p, end) ||
       lws_finalize_http_header(wsi, &p, end)) {
      json_object_put(err_obj);
      return -1;
   }

   int n = lws_write(wsi, start, (size_t)(p - start), LWS_WRITE_HTTP_HEADERS);
   if (n >= 0)
      lws_write(wsi, (unsigned char *)json_str, json_len, LWS_WRITE_HTTP);

   json_object_put(err_obj);
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

/* =============================================================================
 * Format Extraction: PDF (MuPDF)
 * ============================================================================= */

#ifdef HAVE_MUPDF
/**
 * @brief Extract text from PDF using MuPDF's in-memory buffer API
 *
 * Security: fz_try/fz_catch for longjmp safety, 32MB allocation ceiling,
 * page count cap, output size cap. No temp files.
 */
static char *extract_pdf_text(const char *data,
                              size_t data_len,
                              size_t *out_len,
                              int *out_page_count,
                              size_t max_extracted,
                              int max_pages) {
   *out_len = 0;
   *out_page_count = 0;

   /* Validate magic bytes: PDF starts with %PDF- */
   if (data_len < 5 || memcmp(data, "%PDF-", 5) != 0) {
      LOG_WARNING("webui_documents: PDF magic bytes not found");
      return NULL;
   }

   fz_context *ctx = fz_new_context(NULL, NULL, DOC_MUPDF_MEM_LIMIT);
   if (!ctx) {
      LOG_ERROR("webui_documents: fz_new_context failed");
      return NULL;
   }

   char *result = NULL;
   fz_stream *stm = NULL;
   fz_document *doc = NULL;
   fz_buffer *out = NULL;
   fz_output *output = NULL;

   fz_try(ctx) {
      fz_register_document_handlers(ctx);

      stm = fz_open_memory(ctx, (const unsigned char *)data, data_len);
      doc = fz_open_document_with_stream(ctx, "application/pdf", stm);

      int pages = fz_count_pages(ctx, doc);
      *out_page_count = pages;

      if (pages > max_pages)
         pages = max_pages;

      out = fz_new_buffer(ctx, 4096);
      output = fz_new_output_with_buffer(ctx, out);

      for (int i = 0; i < pages; i++) {
         fz_stext_page *stext = fz_new_stext_page_from_page_number(ctx, doc, i, NULL);
         fz_print_stext_page_as_text(ctx, output, stext);
         fz_drop_stext_page(ctx, stext);

         /* Check output size cap */
         size_t current = fz_buffer_storage(ctx, out, NULL);
         if (current > max_extracted)
            break;
      }

      fz_close_output(ctx, output);
      fz_drop_output(ctx, output);
      output = NULL;

      /* Extract result */
      unsigned char *text_data;
      size_t len = fz_buffer_storage(ctx, out, &text_data);
      if (len > max_extracted)
         len = max_extracted;

      result = malloc(len + 1);
      if (result) {
         memcpy(result, text_data, len);
         result[len] = '\0';
         *out_len = len;
      }
   }
   fz_always(ctx) {
      if (output)
         fz_drop_output(ctx, output);
      if (out)
         fz_drop_buffer(ctx, out);
      if (doc)
         fz_drop_document(ctx, doc);
      if (stm)
         fz_drop_stream(ctx, stm);
   }
   fz_catch(ctx) {
      LOG_ERROR("webui_documents: PDF extraction failed: %s", fz_caught_message(ctx));
      free(result);
      result = NULL;
      *out_len = 0;
   }

   fz_drop_context(ctx);
   return result;
}
#endif /* HAVE_MUPDF */

/* =============================================================================
 * Format Extraction: DOCX (libzip + libxml2)
 * ============================================================================= */

#if defined(HAVE_LIBZIP) && defined(HAVE_LIBXML2)

/* Dynamic text buffer for XML walker output */
typedef struct {
   char *buf;
   size_t len;
   size_t cap;
   size_t max_cap; /* Extraction limit snapshot from config */
} text_buf_t;

static void text_buf_append(text_buf_t *tb, const char *str, size_t slen) {
   if (tb->len + slen >= tb->max_cap)
      return; /* Output cap reached */

   if (tb->len + slen >= tb->cap) {
      size_t new_cap = tb->cap * 2;
      if (new_cap < tb->len + slen + 1)
         new_cap = tb->len + slen + 1;
      if (new_cap > tb->max_cap + 1)
         new_cap = tb->max_cap + 1;
      char *new_buf = realloc(tb->buf, new_cap);
      if (!new_buf)
         return;
      tb->buf = new_buf;
      tb->cap = new_cap;
   }
   memcpy(tb->buf + tb->len, str, slen);
   tb->len += slen;
   tb->buf[tb->len] = '\0';
}

/**
 * @brief Recursively walk OOXML nodes extracting text from <w:t> elements
 *
 * Handles paragraphs (double newline), line breaks, tabs, and table cells.
 */
static void docx_walk_xml(xmlNode *node, text_buf_t *tb) {
   for (xmlNode *cur = node; cur; cur = cur->next) {
      if (tb->len >= tb->max_cap)
         return;

      if (cur->type == XML_ELEMENT_NODE) {
         const char *name = (const char *)cur->name;

         /* Recurse into children first */
         docx_walk_xml(cur->children, tb);

         /* Structural whitespace after processing children */
         if (strcmp(name, "p") == 0) {
            text_buf_append(tb, "\n\n", 2);
         } else if (strcmp(name, "br") == 0) {
            text_buf_append(tb, "\n", 1);
         } else if (strcmp(name, "tab") == 0) {
            text_buf_append(tb, "\t", 1);
         } else if (strcmp(name, "tc") == 0) {
            text_buf_append(tb, "\t", 1);
         } else if (strcmp(name, "tr") == 0) {
            text_buf_append(tb, "\n", 1);
         }
      } else if (cur->type == XML_TEXT_NODE && cur->content) {
         /* Only collect text from <w:t> nodes */
         if (cur->parent && cur->parent->name &&
             strcmp((const char *)cur->parent->name, "t") == 0) {
            size_t tlen = strlen((const char *)cur->content);
            text_buf_append(tb, (const char *)cur->content, tlen);
         }
      }
   }
}

/**
 * @brief Extract text from DOCX (ZIP + OOXML) using libzip + libxml2
 *
 * Security: ZIP bomb protection (read loop size cap), XXE prevention
 * (no entity expansion, no DTD loading, no network access), freep=0
 * on zip_source_buffer_create.
 */
static char *extract_docx_text(const char *data,
                               size_t data_len,
                               size_t *out_len,
                               size_t max_extracted) {
   *out_len = 0;

   /* Validate magic bytes: ZIP starts with PK\x03\x04 */
   if (data_len < 4 || memcmp(data, "PK\x03\x04", 4) != 0) {
      LOG_WARNING("webui_documents: DOCX magic bytes not found (not a ZIP)");
      return NULL;
   }

   zip_error_t zerr;
   zip_error_init(&zerr);

   /* freep=0: we manage the buffer, don't let libzip free it */
   zip_source_t *src = zip_source_buffer_create(data, data_len, 0, &zerr);
   if (!src) {
      LOG_ERROR("webui_documents: zip_source_buffer_create failed: %s", zip_error_strerror(&zerr));
      zip_error_fini(&zerr);
      return NULL;
   }

   zip_t *za = zip_open_from_source(src, ZIP_RDONLY, &zerr);
   if (!za) {
      LOG_ERROR("webui_documents: zip_open_from_source failed: %s", zip_error_strerror(&zerr));
      zip_source_free(src);
      zip_error_fini(&zerr);
      return NULL;
   }
   zip_error_fini(&zerr);

   /* Open word/document.xml */
   zip_file_t *zf = zip_fopen(za, "word/document.xml", 0);
   if (!zf) {
      LOG_WARNING("webui_documents: word/document.xml not found in DOCX");
      zip_close(za);
      return NULL;
   }

   /* Read with decompression size limit (ZIP bomb protection) */
   char *xml_buf = malloc(max_extracted + 1);
   if (!xml_buf) {
      zip_fclose(zf);
      zip_close(za);
      return NULL;
   }

   size_t total = 0;
   zip_int64_t n;
   while ((n = zip_fread(zf, xml_buf + total, max_extracted - total)) > 0) {
      total += (size_t)n;
      if (total >= max_extracted)
         break;
   }
   xml_buf[total] = '\0';
   zip_fclose(zf);
   zip_close(za);

   if (total == 0) {
      free(xml_buf);
      return NULL;
   }

   /* Parse XML with full XXE prevention */
   xmlParserCtxtPtr parser = xmlNewParserCtxt();
   if (!parser) {
      free(xml_buf);
      return NULL;
   }

   /* Defensive flags: no network, no warnings/errors to stdout */
   int flags = XML_PARSE_NONET | XML_PARSE_NOWARNING | XML_PARSE_NOERROR;

   xmlDocPtr xmldoc = xmlCtxtReadMemory(parser, xml_buf, (int)total, NULL, NULL, flags);
   free(xml_buf);

   if (!xmldoc) {
      LOG_ERROR("webui_documents: DOCX XML parse failed");
      xmlFreeParserCtxt(parser);
      return NULL;
   }

   /* Reject DTDs entirely (legitimate DOCX never has one — XXE prevention) */
   if (xmldoc->intSubset) {
      LOG_WARNING("webui_documents: DOCX XML contains DTD, rejecting (XXE prevention)");
      xmlFreeDoc(xmldoc);
      xmlFreeParserCtxt(parser);
      return NULL;
   }

   /* Walk XML tree to extract text */
   xmlNode *root = xmlDocGetRootElement(xmldoc);
   text_buf_t tb = { .buf = malloc(4096), .len = 0, .cap = 4096, .max_cap = max_extracted };
   if (!tb.buf) {
      xmlFreeDoc(xmldoc);
      xmlFreeParserCtxt(parser);
      return NULL;
   }
   tb.buf[0] = '\0';

   docx_walk_xml(root, &tb);

   xmlFreeDoc(xmldoc);
   xmlFreeParserCtxt(parser);

   *out_len = tb.len;
   return tb.buf;
}
#endif /* HAVE_LIBZIP && HAVE_LIBXML2 */

/* =============================================================================
 * Send JSON Success Response
 * ============================================================================= */

/**
 * @brief Send JSON success response with extracted document content
 *
 * Uses json-c for safe JSON construction (handles escaping).
 * Includes estimated_tokens for client-side token budget checks.
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

   /* Token estimate: chars / 4 heuristic (matches llm_context_estimate_tokens) */
   int estimated_tokens = (int)(content_size / 4);

   /* Create content string object, then free raw buffer to reduce peak memory */
   json_object *content_obj = json_object_new_string(session->content_buf);
   free(session->content_buf);
   session->content_buf = NULL;
   session->content_len = 0;
   json_object_object_add(resp, "content", content_obj);

   json_object_object_add(resp, "size", json_object_new_int64((int64_t)content_size));
   json_object_object_add(resp, "type", json_object_new_string(ext ? ext : ""));
   json_object_object_add(resp, "estimated_tokens", json_object_new_int(estimated_tokens));

   /* Include page count for PDF files */
   if (session->page_count > 0) {
      json_object_object_add(resp, "page_count", json_object_new_int(session->page_count));
   }

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

/**
 * @brief Send a JSON response for the summarize endpoint
 */
static int send_json_response(struct lws *wsi, int status, json_object *resp) {
   const char *json_str = json_object_to_json_string_ext(resp, JSON_C_TO_STRING_PLAIN);
   size_t json_len = strlen(json_str);

   size_t buf_size = LWS_PRE + 512;
   unsigned char *buffer = malloc(buf_size);
   if (!buffer) {
      json_object_put(resp);
      return -1;
   }

   unsigned char *start = &buffer[LWS_PRE];
   unsigned char *p = start;
   unsigned char *end = &buffer[buf_size - 1];

   if (lws_add_http_header_status(wsi, (unsigned int)status, &p, end) ||
       lws_add_http_header_by_token(wsi, WSI_TOKEN_HTTP_CONTENT_TYPE,
                                    (unsigned char *)"application/json", 16, &p, end) ||
       lws_add_http_header_content_length(wsi, (unsigned long)json_len, &p, end) ||
       lws_finalize_http_header(wsi, &p, end)) {
      free(buffer);
      json_object_put(resp);
      return -1;
   }

   int n = lws_write(wsi, start, (size_t)(p - start), LWS_WRITE_HTTP_HEADERS);
   if (n >= 0)
      lws_write(wsi, (unsigned char *)json_str, json_len, LWS_WRITE_HTTP);

   free(buffer);
   json_object_put(resp);
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
   /* Snapshot config limits for this upload (avoids TOCTOU if config changes mid-upload) */
   const size_t max_file_size = (size_t)g_config.documents.max_file_size_kb * 1024;

   size_t content_length = 0;
   if (cl_len > 0) {
      char *endptr;
      long long parsed = strtoll(content_length_str, &endptr, 10);
      if (*endptr != '\0' || parsed < 0 || (size_t)parsed > max_file_size + 1024) {
         char err_msg[64];
         snprintf(err_msg, sizeof(err_msg), "File too large (max %d KB)",
                  g_config.documents.max_file_size_kb);
         return send_doc_error(wsi, HTTP_STATUS_REQ_ENTITY_TOO_LARGE, err_msg);
      }
      content_length = (size_t)parsed;
   }

   /* Allocate session */
   document_upload_session_t *session = calloc(1, sizeof(document_upload_session_t));
   if (!session) {
      return send_doc_error(wsi, HTTP_STATUS_INTERNAL_SERVER_ERROR, "Memory allocation failed");
   }

   session->page_count = -1; /* Not applicable by default */
   session->max_file_size = max_file_size;
   session->max_extracted_size = (size_t)g_config.documents.max_extracted_size_kb * 1024;
   session->max_pages = g_config.documents.max_pages;

   /* Extract boundary */
   if (!extract_doc_boundary(content_type, session->boundary, sizeof(session->boundary))) {
      free(session);
      return send_doc_error(wsi, HTTP_STATUS_BAD_REQUEST, "Missing multipart boundary");
   }

   /* Allocate initial data buffer */
   session->content_cap = (content_length > 0 && content_length <= max_file_size + 1024)
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
   if (session->content_len + len > session->max_file_size + 1024) {
      LOG_WARNING("webui_documents: upload exceeds size limit (%zu + %zu)", session->content_len,
                  len);
      return -1;
   }

   /* Grow buffer if needed */
   if (session->content_len + len > session->content_cap) {
      size_t new_cap = session->content_cap * 2;
      if (new_cap < session->content_len + len)
         new_cap = session->content_len + len;
      if (new_cap > session->max_file_size + 1024)
         new_cap = session->max_file_size + 1024;

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
   if (session->content_len > session->max_file_size) {
      char err_msg[64];
      snprintf(err_msg, sizeof(err_msg), "File too large (max %zu KB)",
               session->max_file_size / 1024);
      webui_documents_session_free(session);
      return send_doc_error(wsi, HTTP_STATUS_REQ_ENTITY_TOO_LARGE, err_msg);
   }

   /* =========================================================================
    * Format-specific text extraction (PDF, DOCX, HTML)
    * Runs BEFORE null-termination and UTF-8 sanitization since extractors
    * produce their own clean text output.
    * ========================================================================= */

#ifdef HAVE_MUPDF
   if (strcasecmp(ext, ".pdf") == 0) {
      size_t extracted_len = 0;
      int page_count = 0;
      char *extracted = extract_pdf_text(session->content_buf, session->content_len, &extracted_len,
                                         &page_count, session->max_extracted_size,
                                         session->max_pages);
      if (!extracted || extracted_len == 0) {
         free(extracted);
         webui_documents_session_free(session);
         return send_doc_error(wsi, 422, "Could not extract text from PDF");
      }
      free(session->content_buf);
      session->content_buf = extracted;
      session->content_len = extracted_len;
      session->content_cap = extracted_len + 1;
      session->page_count = page_count;
      goto extracted;
   }
#endif

#if defined(HAVE_LIBZIP) && defined(HAVE_LIBXML2)
   if (strcasecmp(ext, ".docx") == 0) {
      size_t extracted_len = 0;
      char *extracted = extract_docx_text(session->content_buf, session->content_len,
                                          &extracted_len, session->max_extracted_size);
      if (!extracted || extracted_len == 0) {
         free(extracted);
         webui_documents_session_free(session);
         return send_doc_error(wsi, 422, "Could not extract text from DOCX");
      }
      free(session->content_buf);
      session->content_buf = extracted;
      session->content_len = extracted_len;
      session->content_cap = extracted_len + 1;
      goto extracted;
   }
#endif

   if (strcasecmp(ext, ".html") == 0 || strcasecmp(ext, ".htm") == 0) {
      /* Null-terminate for html_extract_text */
      if (session->content_len < session->content_cap) {
         session->content_buf[session->content_len] = '\0';
      } else {
         char *new_buf = realloc(session->content_buf, session->content_len + 1);
         if (!new_buf) {
            webui_documents_session_free(session);
            return send_doc_error(wsi, HTTP_STATUS_INTERNAL_SERVER_ERROR,
                                  "Memory allocation failed");
         }
         session->content_buf = new_buf;
         session->content_cap = session->content_len + 1;
         session->content_buf[session->content_len] = '\0';
      }

      char *extracted = NULL;
      int rc = html_extract_text(session->content_buf, session->content_len, &extracted);
      if (rc == HTML_PARSE_SUCCESS && extracted) {
         free(session->content_buf);
         session->content_buf = extracted;
         session->content_len = strlen(extracted);
         session->content_cap = session->content_len + 1;
      }
      /* If HTML extraction fails, fall through with raw content */
      goto extracted;
   }

extracted:
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

/* =============================================================================
 * Summarize Endpoint
 * ============================================================================= */

int webui_documents_handle_summarize(struct lws *wsi, const char *body, size_t body_len) {
   if (!body || body_len == 0) {
      return send_doc_error(wsi, HTTP_STATUS_BAD_REQUEST, "Empty request body");
   }

   /* Parse JSON body */
   json_object *req = json_tokener_parse(body);
   if (!req) {
      return send_doc_error(wsi, HTTP_STATUS_BAD_REQUEST, "Invalid JSON");
   }

   json_object *content_obj = NULL;
   json_object *target_obj = NULL;

   if (!json_object_object_get_ex(req, "content", &content_obj) ||
       !json_object_object_get_ex(req, "target_tokens", &target_obj)) {
      json_object_put(req);
      return send_doc_error(wsi, HTTP_STATUS_BAD_REQUEST, "Missing content or target_tokens field");
   }

   const char *content = json_object_get_string(content_obj);
   int target_tokens = json_object_get_int(target_obj);

   if (!content || target_tokens <= 0) {
      json_object_put(req);
      return send_doc_error(wsi, HTTP_STATUS_BAD_REQUEST, "Invalid content or target_tokens");
   }

   /* Calculate keep ratio */
   int current_tokens = (int)(strlen(content) / 4);
   float keep_ratio = (current_tokens > 0) ? (float)target_tokens / (float)current_tokens : 1.0f;
   if (keep_ratio >= 1.0f)
      keep_ratio = 1.0f;
   if (keep_ratio < 0.05f)
      keep_ratio = 0.05f; /* Floor at 5% */

   /* Run TF-IDF summarization */
   char *summary = NULL;
   int rc = tfidf_summarize(content, &summary, keep_ratio, TFIDF_MIN_SENTENCES);

   json_object_put(req); /* Done with input */

   if (rc != TFIDF_SUCCESS || !summary) {
      free(summary);
      return send_doc_error(wsi, 422, "Summarization failed");
   }

   /* Build response */
   json_object *resp = json_object_new_object();
   if (!resp) {
      free(summary);
      return send_doc_error(wsi, HTTP_STATUS_INTERNAL_SERVER_ERROR, "JSON construction failed");
   }

   size_t summary_len = strlen(summary);
   int new_estimated_tokens = (int)(summary_len / 4);

   json_object_object_add(resp, "success", json_object_new_boolean(1));
   json_object_object_add(resp, "content", json_object_new_string(summary));
   json_object_object_add(resp, "size", json_object_new_int64((int64_t)summary_len));
   json_object_object_add(resp, "estimated_tokens", json_object_new_int(new_estimated_tokens));

   free(summary);
   return send_json_response(wsi, HTTP_STATUS_OK, resp);
}
