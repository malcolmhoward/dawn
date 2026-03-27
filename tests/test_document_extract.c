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
 * Unit tests for document_extract.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tools/document_extract.h"

static int passed = 0;
static int failed = 0;

#define ASSERT(cond, msg)                                  \
   do {                                                    \
      if (cond) {                                          \
         printf("  [PASS] %s\n", msg);                     \
         passed++;                                         \
      } else {                                             \
         printf("  [FAIL] %s (line %d)\n", msg, __LINE__); \
         failed++;                                         \
      }                                                    \
   } while (0)

/* =============================================================================
 * Extension Validation Tests
 * ============================================================================= */

static void test_extension_allowed(void) {
   printf("\n--- test_extension_allowed ---\n");

   ASSERT(document_extension_allowed(".txt"), ".txt allowed");
   ASSERT(document_extension_allowed(".md"), ".md allowed");
   ASSERT(document_extension_allowed(".csv"), ".csv allowed");
   ASSERT(document_extension_allowed(".json"), ".json allowed");
   ASSERT(document_extension_allowed(".c"), ".c allowed");
   ASSERT(document_extension_allowed(".h"), ".h allowed");
   ASSERT(document_extension_allowed(".py"), ".py allowed");
   ASSERT(document_extension_allowed(".js"), ".js allowed");
   ASSERT(document_extension_allowed(".html"), ".html allowed");
   ASSERT(document_extension_allowed(".htm"), ".htm allowed");
   ASSERT(document_extension_allowed(".xml"), ".xml allowed");
   ASSERT(document_extension_allowed(".yaml"), ".yaml allowed");
   ASSERT(document_extension_allowed(".yml"), ".yml allowed");
   ASSERT(document_extension_allowed(".toml"), ".toml allowed");
   ASSERT(document_extension_allowed(".css"), ".css allowed");
   ASSERT(document_extension_allowed(".sql"), ".sql allowed");

   /* Case insensitive */
   ASSERT(document_extension_allowed(".TXT"), ".TXT case insensitive");
   ASSERT(document_extension_allowed(".Md"), ".Md case insensitive");
   ASSERT(document_extension_allowed(".JSON"), ".JSON case insensitive");

   /* Disallowed */
   ASSERT(!document_extension_allowed(".exe"), ".exe not allowed");
   ASSERT(!document_extension_allowed(".bin"), ".bin not allowed");
   ASSERT(!document_extension_allowed(".zip"), ".zip not allowed");
   ASSERT(!document_extension_allowed(".tar"), ".tar not allowed");
   ASSERT(!document_extension_allowed(".dll"), ".dll not allowed");
   ASSERT(!document_extension_allowed(".so"), ".so not allowed");
   ASSERT(!document_extension_allowed(""), "empty not allowed");
   ASSERT(!document_extension_allowed(NULL), "NULL not allowed");
}

/* =============================================================================
 * Extension From Filename Tests
 * ============================================================================= */

static void test_get_extension(void) {
   printf("\n--- test_get_extension ---\n");

   ASSERT(strcmp(document_get_extension("file.txt"), ".txt") == 0, "file.txt -> .txt");
   ASSERT(strcmp(document_get_extension("my.doc.pdf"), ".pdf") == 0, "my.doc.pdf -> .pdf");
   ASSERT(strcmp(document_get_extension("report.DOCX"), ".DOCX") == 0, "report.DOCX -> .DOCX");
   ASSERT(document_get_extension("noext") == NULL, "noext -> NULL");
   ASSERT(document_get_extension(NULL) == NULL, "NULL -> NULL");
}

/* =============================================================================
 * Content-Type Mapping Tests
 * ============================================================================= */

static void test_content_type_mapping(void) {
   printf("\n--- test_content_type_mapping ---\n");

   ASSERT(strcmp(document_extension_from_content_type("application/pdf"), ".pdf") == 0,
          "application/pdf -> .pdf");
   ASSERT(strcmp(document_extension_from_content_type("text/html"), ".html") == 0,
          "text/html -> .html");
   ASSERT(strcmp(document_extension_from_content_type("text/html; charset=utf-8"), ".html") == 0,
          "text/html; charset=utf-8 -> .html");
   ASSERT(strcmp(document_extension_from_content_type("text/plain"), ".txt") == 0,
          "text/plain -> .txt");
   ASSERT(strcmp(document_extension_from_content_type("application/json"), ".json") == 0,
          "application/json -> .json");
   ASSERT(strcmp(document_extension_from_content_type("text/markdown"), ".md") == 0,
          "text/markdown -> .md");
   ASSERT(strcmp(document_extension_from_content_type("text/csv"), ".csv") == 0,
          "text/csv -> .csv");
   ASSERT(strcmp(document_extension_from_content_type("text/xml"), ".xml") == 0,
          "text/xml -> .xml");
   ASSERT(strcmp(document_extension_from_content_type("application/xml"), ".xml") == 0,
          "application/xml -> .xml");
   ASSERT(strcmp(document_extension_from_content_type("application/xhtml+xml"), ".html") == 0,
          "application/xhtml+xml -> .html");
   ASSERT(strcmp(document_extension_from_content_type("text/yaml"), ".yaml") == 0,
          "text/yaml -> .yaml");

   const char *docx_ct = "application/vnd.openxmlformats-officedocument.wordprocessingml.document";
   ASSERT(strcmp(document_extension_from_content_type(docx_ct), ".docx") == 0,
          "DOCX content-type -> .docx");

   /* Unknown types */
   ASSERT(document_extension_from_content_type("application/octet-stream") == NULL,
          "octet-stream -> NULL");
   ASSERT(document_extension_from_content_type("image/png") == NULL, "image/png -> NULL");
   ASSERT(document_extension_from_content_type(NULL) == NULL, "NULL -> NULL");
   ASSERT(document_extension_from_content_type("") == NULL, "empty -> NULL");

   /* Partial match should not match */
   ASSERT(document_extension_from_content_type("text/htmlx") == NULL,
          "text/htmlx should not match text/html");
}

/* =============================================================================
 * Plain Text Extraction Tests
 * ============================================================================= */

static void test_plain_text_extraction(void) {
   printf("\n--- test_plain_text_extraction ---\n");

   const char *text = "Hello, this is a test document.\nLine two.";
   doc_extract_result_t result = { 0 };

   int rc = document_extract_from_buffer(text, strlen(text), ".txt", 1024 * 1024, 100, &result);
   ASSERT(rc == DOC_EXTRACT_SUCCESS, "plain text extraction succeeds");
   ASSERT(result.text != NULL, "result text not NULL");
   ASSERT(result.text_len == strlen(text), "text length matches");
   ASSERT(strcmp(result.text, text) == 0, "text content matches");
   ASSERT(result.page_count == -1, "page_count is -1 for non-PDF");
   free(result.text);
}

static void test_plain_text_truncation(void) {
   printf("\n--- test_plain_text_truncation ---\n");

   const char *text = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
   doc_extract_result_t result = { 0 };

   int rc = document_extract_from_buffer(text, strlen(text), ".txt", 10, 100, &result);
   ASSERT(rc == DOC_EXTRACT_SUCCESS, "truncated extraction succeeds");
   ASSERT(result.text_len == 10, "truncated to max_extracted_size");
   ASSERT(memcmp(result.text, "ABCDEFGHIJ", 10) == 0, "truncated content correct");
   free(result.text);
}

static void test_code_file_extraction(void) {
   printf("\n--- test_code_file_extraction ---\n");

   const char *code = "#include <stdio.h>\nint main() { return 0; }\n";
   doc_extract_result_t result = { 0 };

   int rc = document_extract_from_buffer(code, strlen(code), ".c", 1024 * 1024, 100, &result);
   ASSERT(rc == DOC_EXTRACT_SUCCESS, ".c extraction succeeds");
   ASSERT(result.text_len == strlen(code), ".c text length matches");
   free(result.text);

   memset(&result, 0, sizeof(result));
   rc = document_extract_from_buffer(code, strlen(code), ".py", 1024 * 1024, 100, &result);
   ASSERT(rc == DOC_EXTRACT_SUCCESS, ".py extraction succeeds");
   free(result.text);
}

/* =============================================================================
 * Error Case Tests
 * ============================================================================= */

static void test_empty_input(void) {
   printf("\n--- test_empty_input ---\n");

   doc_extract_result_t result = { 0 };

   int rc = document_extract_from_buffer(NULL, 0, ".txt", 1024, 100, &result);
   ASSERT(rc == DOC_EXTRACT_ERROR_EMPTY, "NULL data returns EMPTY");

   rc = document_extract_from_buffer("test", 0, ".txt", 1024, 100, &result);
   ASSERT(rc == DOC_EXTRACT_ERROR_EMPTY, "zero-length returns EMPTY");
}

static void test_unsupported_extension(void) {
   printf("\n--- test_unsupported_extension ---\n");

   const char *data = "some data";
   doc_extract_result_t result = { 0 };

   int rc = document_extract_from_buffer(data, strlen(data), ".exe", 1024, 100, &result);
   ASSERT(rc == DOC_EXTRACT_ERROR_UNSUPPORTED, ".exe returns UNSUPPORTED");

   rc = document_extract_from_buffer(data, strlen(data), ".bin", 1024, 100, &result);
   ASSERT(rc == DOC_EXTRACT_ERROR_UNSUPPORTED, ".bin returns UNSUPPORTED");
}

static void test_invalid_pdf_magic(void) {
   printf("\n--- test_invalid_pdf_magic ---\n");

#ifdef HAVE_MUPDF
   const char *not_pdf = "This is not a PDF file at all";
   doc_extract_result_t result = { 0 };

   int rc = document_extract_from_buffer(not_pdf, strlen(not_pdf), ".pdf", 1024 * 1024, 100,
                                         &result);
   ASSERT(rc == DOC_EXTRACT_ERROR_CORRUPT, "non-PDF data with .pdf ext returns CORRUPT");
   ASSERT(result.text == NULL, "no text on corrupt PDF");
   printf("  (MuPDF available — PDF magic byte check tested)\n");
#else
   printf("  (MuPDF not available — skipping PDF tests)\n");
   passed++; /* Count as pass so total is consistent */
#endif
}

static void test_invalid_docx_magic(void) {
   printf("\n--- test_invalid_docx_magic ---\n");

#if defined(HAVE_LIBZIP) && defined(HAVE_LIBXML2)
   const char *not_docx = "This is definitely not a DOCX/ZIP file";
   doc_extract_result_t result = { 0 };

   int rc = document_extract_from_buffer(not_docx, strlen(not_docx), ".docx", 1024 * 1024, 100,
                                         &result);
   ASSERT(rc == DOC_EXTRACT_ERROR_CORRUPT, "non-ZIP data with .docx ext returns CORRUPT");
   ASSERT(result.text == NULL, "no text on corrupt DOCX");
   printf("  (libzip + libxml2 available — DOCX magic byte check tested)\n");
#else
   printf("  (libzip/libxml2 not available — skipping DOCX tests)\n");
   passed++; /* Count as pass */
#endif
}

/* =============================================================================
 * Error String Tests
 * ============================================================================= */

static void test_error_strings(void) {
   printf("\n--- test_error_strings ---\n");

   ASSERT(strcmp(document_extract_error_string(DOC_EXTRACT_SUCCESS), "Success") == 0,
          "SUCCESS string");
   ASSERT(strlen(document_extract_error_string(DOC_EXTRACT_ERROR_UNSUPPORTED)) > 0,
          "UNSUPPORTED has message");
   ASSERT(strlen(document_extract_error_string(DOC_EXTRACT_ERROR_CORRUPT)) > 0,
          "CORRUPT has message");
   ASSERT(strlen(document_extract_error_string(DOC_EXTRACT_ERROR_EMPTY)) > 0, "EMPTY has message");
   ASSERT(strlen(document_extract_error_string(DOC_EXTRACT_ERROR_TOO_LARGE)) > 0,
          "TOO_LARGE has message");
   ASSERT(strlen(document_extract_error_string(DOC_EXTRACT_ERROR_ALLOC)) > 0, "ALLOC has message");
   ASSERT(strlen(document_extract_error_string(999)) > 0, "unknown code has message");
}

/* =============================================================================
 * HTML Extraction Tests
 * ============================================================================= */

static void test_html_extraction(void) {
   printf("\n--- test_html_extraction ---\n");

   const char *html = "<html><body><h1>Title</h1><p>Hello world</p></body></html>";
   doc_extract_result_t result = { 0 };

   int rc = document_extract_from_buffer(html, strlen(html), ".html", 1024 * 1024, 100, &result);
   ASSERT(rc == DOC_EXTRACT_SUCCESS, "HTML extraction succeeds");
   ASSERT(result.text != NULL, "HTML extracted text not NULL");
   ASSERT(result.text_len > 0, "HTML extracted text has content");
   /* Should contain "Title" and "Hello world" without tags */
   ASSERT(strstr(result.text, "Title") != NULL, "HTML text contains 'Title'");
   ASSERT(strstr(result.text, "Hello world") != NULL, "HTML text contains 'Hello world'");
   ASSERT(strstr(result.text, "<h1>") == NULL, "HTML tags stripped");
   free(result.text);
}

/* =============================================================================
 * Main
 * ============================================================================= */

int main(void) {
   printf("=== test_document_extract ===\n");

   test_extension_allowed();
   test_get_extension();
   test_content_type_mapping();
   test_plain_text_extraction();
   test_plain_text_truncation();
   test_code_file_extraction();
   test_empty_input();
   test_unsupported_extension();
   test_invalid_pdf_magic();
   test_invalid_docx_magic();
   test_error_strings();
   test_html_extraction();

   printf("\n=== Results: %d passed, %d failed ===\n", passed, failed);
   return failed > 0 ? 1 : 0;
}
