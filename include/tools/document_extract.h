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
 * Shared document text extraction — PDF, DOCX, HTML, plain text
 *
 * Used by both the WebUI upload handler and the document_index LLM tool.
 * Thread-safe: all functions are stateless with no shared mutable state.
 */

#ifndef DOCUMENT_EXTRACT_H
#define DOCUMENT_EXTRACT_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Error codes */
#define DOC_EXTRACT_SUCCESS 0
#define DOC_EXTRACT_ERROR_UNSUPPORTED 1 /* Unknown/unsupported extension */
#define DOC_EXTRACT_ERROR_CORRUPT 2     /* Magic bytes mismatch or parse failure */
#define DOC_EXTRACT_ERROR_EMPTY 3       /* Extraction produced no text */
#define DOC_EXTRACT_ERROR_TOO_LARGE 4   /* Extracted text exceeds max size */
#define DOC_EXTRACT_ERROR_ALLOC 5       /* Memory allocation failure */

/* MuPDF allocation ceiling (security) */
#define DOC_MUPDF_MEM_LIMIT (32 * 1024 * 1024)

/**
 * @brief Result of document text extraction
 */
typedef struct {
   char *text;      /* Extracted text (caller frees with free()) */
   size_t text_len; /* Length of extracted text in bytes */
   int page_count;  /* PDF page count, -1 if not applicable */
} doc_extract_result_t;

/**
 * @brief Extract text from a document buffer based on file extension
 *
 * Dispatches to format-specific extractors: PDF (MuPDF), DOCX (libzip+libxml2),
 * HTML (tag stripping), or plain text passthrough. Validates magic bytes for
 * binary formats.
 *
 * @param data Raw document data
 * @param data_len Length of data in bytes
 * @param extension File extension including dot (e.g., ".pdf", ".docx")
 * @param max_extracted_size Maximum extracted text size in bytes
 * @param max_pages Maximum PDF pages to extract (ignored for non-PDF)
 * @param out Result struct (caller frees out->text)
 * @return DOC_EXTRACT_SUCCESS or error code
 */
int document_extract_from_buffer(const char *data,
                                 size_t data_len,
                                 const char *extension,
                                 size_t max_extracted_size,
                                 int max_pages,
                                 doc_extract_result_t *out);

/**
 * @brief Check if a file extension is in the allowed list (case-insensitive)
 *
 * The allowed list includes text, code, config, and (when compiled in)
 * PDF and DOCX formats.
 *
 * @param ext Extension including dot (e.g., ".pdf")
 * @return true if allowed, false otherwise
 */
bool document_extension_allowed(const char *ext);

/**
 * @brief Get file extension from filename (including the dot)
 *
 * @param filename Filename or path
 * @return Pointer to the dot in the filename, or NULL if no extension
 */
const char *document_get_extension(const char *filename);

/**
 * @brief Map HTTP Content-Type header to a file extension
 *
 * Used when downloading URLs where Content-Type is available but
 * the URL path may lack a file extension.
 *
 * @param content_type Content-Type value (e.g., "application/pdf")
 * @return Extension including dot (e.g., ".pdf"), or NULL if unknown
 */
const char *document_extension_from_content_type(const char *content_type);

/**
 * @brief Get human-readable error message for an extraction error code
 *
 * @param error_code Error code from document_extract_from_buffer
 * @return Static string describing the error
 */
const char *document_extract_error_string(int error_code);

#ifdef __cplusplus
}
#endif

#endif /* DOCUMENT_EXTRACT_H */
