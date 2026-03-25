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
 * Unit tests for instruction_loader.c — file loading, path traversal
 * sanitization, module concatenation, and edge cases.
 *
 * Creates temporary instruction files in /tmp for testing, cleans up after.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "tools/instruction_loader.h"

/* ============================================================================
 * Test Harness
 * ============================================================================ */

static int tests_passed = 0;
static int tests_failed = 0;

#define ASSERT(cond, msg)                                  \
   do {                                                    \
      if (cond) {                                          \
         printf("  [PASS] %s\n", msg);                     \
         tests_passed++;                                   \
      } else {                                             \
         printf("  [FAIL] %s (line %d)\n", msg, __LINE__); \
         tests_failed++;                                   \
      }                                                    \
   } while (0)

/* ============================================================================
 * Test Fixture — create temp instruction files
 * ============================================================================ */

#define TEST_DIR "/tmp/dawn_test_instructions"
#define TOOL_NAME "test_tool"

static void write_file(const char *path, const char *content) {
   FILE *f = fopen(path, "w");
   if (f) {
      fputs(content, f);
      fclose(f);
   }
}

static void setup_test_files(void) {
   mkdir(TEST_DIR, 0755);
   mkdir(TEST_DIR "/" TOOL_NAME, 0755);

   write_file(TEST_DIR "/" TOOL_NAME "/_core.md", "# Core rules\nAlways be polite.\n");
   write_file(TEST_DIR "/" TOOL_NAME "/formal.md", "# Formal style\nUse titles.\n");
   write_file(TEST_DIR "/" TOOL_NAME "/casual.md", "# Casual style\nUse first names.\n");
   write_file(TEST_DIR "/" TOOL_NAME "/empty.md", "");
}

static void cleanup_test_files(void) {
   unlink(TEST_DIR "/" TOOL_NAME "/_core.md");
   unlink(TEST_DIR "/" TOOL_NAME "/formal.md");
   unlink(TEST_DIR "/" TOOL_NAME "/casual.md");
   unlink(TEST_DIR "/" TOOL_NAME "/empty.md");
   rmdir(TEST_DIR "/" TOOL_NAME);
   rmdir(TEST_DIR);
}

/* ============================================================================
 * We need to override the base dir for testing. The loader uses
 * instruction_loader_get_base_dir() which returns the compile-time default.
 * We'll test with the real function by symlinking, or we can test the
 * loader's internal logic by creating files at the default path.
 *
 * Simpler approach: create a symlink from the default dir to our test dir,
 * or just create files in CWD-relative tool_instructions/.
 *
 * Simplest: create test files at the path the loader expects.
 * ============================================================================ */

#define CWD_TEST_DIR "tool_instructions"
#define CWD_TOOL_NAME "test_loader_tmp"

static void setup_cwd_test_files(void) {
   mkdir(CWD_TEST_DIR, 0755); /* May already exist */
   mkdir(CWD_TEST_DIR "/" CWD_TOOL_NAME, 0755);

   write_file(CWD_TEST_DIR "/" CWD_TOOL_NAME "/_core.md", "# Core rules\nAlways be polite.\n");
   write_file(CWD_TEST_DIR "/" CWD_TOOL_NAME "/formal.md", "# Formal style\nUse titles.\n");
   write_file(CWD_TEST_DIR "/" CWD_TOOL_NAME "/casual.md", "# Casual style\nUse first names.\n");
   write_file(CWD_TEST_DIR "/" CWD_TOOL_NAME "/empty.md", "");
}

static void cleanup_cwd_test_files(void) {
   unlink(CWD_TEST_DIR "/" CWD_TOOL_NAME "/_core.md");
   unlink(CWD_TEST_DIR "/" CWD_TOOL_NAME "/formal.md");
   unlink(CWD_TEST_DIR "/" CWD_TOOL_NAME "/casual.md");
   unlink(CWD_TEST_DIR "/" CWD_TOOL_NAME "/empty.md");
   rmdir(CWD_TEST_DIR "/" CWD_TOOL_NAME);
   /* Don't rmdir CWD_TEST_DIR — may contain real tool instructions */
}

/* ============================================================================
 * 1. Null/Empty Input Handling
 * ============================================================================ */

static void test_null_inputs(void) {
   printf("\n=== test_null_inputs ===\n");
   char *output = NULL;

   ASSERT(instruction_loader_load(NULL, "formal", &output) != 0, "NULL tool_name rejected");
   ASSERT(output == NULL, "output is NULL on error");

   ASSERT(instruction_loader_load("", "formal", &output) != 0, "empty tool_name rejected");
   ASSERT(output == NULL, "output is NULL on empty tool_name");

   /* NULL modules with valid tool — should still load _core.md */
   int rc = instruction_loader_load(CWD_TOOL_NAME, NULL, &output);
   ASSERT(rc == 0, "NULL modules loads _core.md only");
   ASSERT(output != NULL, "output not NULL when _core.md exists");
   if (output) {
      ASSERT(strstr(output, "Core rules") != NULL, "_core.md content present");
      free(output);
      output = NULL;
   }

   /* Empty modules string */
   rc = instruction_loader_load(CWD_TOOL_NAME, "", &output);
   ASSERT(rc == 0, "empty modules loads _core.md only");
   if (output) {
      ASSERT(strstr(output, "Core rules") != NULL, "_core.md content in empty modules");
      free(output);
      output = NULL;
   }
}

/* ============================================================================
 * 2. Single Module Loading
 * ============================================================================ */

static void test_single_module(void) {
   printf("\n=== test_single_module ===\n");
   char *output = NULL;

   int rc = instruction_loader_load(CWD_TOOL_NAME, "formal", &output);
   ASSERT(rc == 0, "single module loads successfully");
   ASSERT(output != NULL, "output not NULL");
   if (output) {
      ASSERT(strstr(output, "Core rules") != NULL, "_core.md content present");
      ASSERT(strstr(output, "Formal style") != NULL, "formal.md content present");
      ASSERT(strstr(output, "Casual style") == NULL, "casual.md NOT present");
      ASSERT(strstr(output, "---") != NULL, "separator between sections");
      free(output);
   }
}

/* ============================================================================
 * 3. Multiple Modules Loading
 * ============================================================================ */

static void test_multiple_modules(void) {
   printf("\n=== test_multiple_modules ===\n");
   char *output = NULL;

   int rc = instruction_loader_load(CWD_TOOL_NAME, "formal,casual", &output);
   ASSERT(rc == 0, "multiple modules load successfully");
   ASSERT(output != NULL, "output not NULL");
   if (output) {
      ASSERT(strstr(output, "Core rules") != NULL, "_core.md content present");
      ASSERT(strstr(output, "Formal style") != NULL, "formal.md content present");
      ASSERT(strstr(output, "Casual style") != NULL, "casual.md content present");
      free(output);
   }
}

/* ============================================================================
 * 4. Whitespace in Module List
 * ============================================================================ */

static void test_whitespace_handling(void) {
   printf("\n=== test_whitespace_handling ===\n");
   char *output = NULL;

   int rc = instruction_loader_load(CWD_TOOL_NAME, " formal , casual ", &output);
   ASSERT(rc == 0, "whitespace-padded modules load");
   ASSERT(output != NULL, "output not NULL");
   if (output) {
      ASSERT(strstr(output, "Formal style") != NULL, "formal.md found despite whitespace");
      ASSERT(strstr(output, "Casual style") != NULL, "casual.md found despite whitespace");
      free(output);
   }
}

/* ============================================================================
 * 5. Nonexistent Module (graceful handling)
 * ============================================================================ */

static void test_missing_module(void) {
   printf("\n=== test_missing_module ===\n");
   char *output = NULL;

   /* One valid, one invalid */
   int rc = instruction_loader_load(CWD_TOOL_NAME, "formal,nonexistent", &output);
   ASSERT(rc == 0, "partial load succeeds (valid + missing)");
   ASSERT(output != NULL, "output not NULL");
   if (output) {
      ASSERT(strstr(output, "Formal style") != NULL, "valid module content present");
      free(output);
   }
}

/* ============================================================================
 * 6. Nonexistent Tool Directory
 * ============================================================================ */

static void test_missing_tool(void) {
   printf("\n=== test_missing_tool ===\n");
   char *output = NULL;

   int rc = instruction_loader_load("no_such_tool_xyz", "formal", &output);
   ASSERT(rc != 0, "nonexistent tool returns error");
   ASSERT(output == NULL, "output is NULL for missing tool");
}

/* ============================================================================
 * 7. Path Traversal Prevention
 * ============================================================================ */

static void test_path_traversal(void) {
   printf("\n=== test_path_traversal ===\n");
   char *output = NULL;

   /* Tool name with path traversal */
   ASSERT(instruction_loader_load("../etc", "passwd", &output) != 0, "tool_name with ../ rejected");
   ASSERT(output == NULL, "no output on traversal attempt (tool)");

   ASSERT(instruction_loader_load("foo/bar", "baz", &output) != 0, "tool_name with / rejected");

   /* Module name with path traversal */
   int rc = instruction_loader_load(CWD_TOOL_NAME, "../../etc/passwd", &output);
   /* Should still succeed (loads _core.md), but the malicious module is skipped */
   if (rc == 0 && output) {
      ASSERT(strstr(output, "root:") == NULL, "no /etc/passwd content leaked");
      free(output);
      output = NULL;
   } else {
      /* If _core.md loaded, rc is 0. If not (no _core), could be error. Either is safe. */
      ASSERT(output == NULL || strstr(output, "root:") == NULL, "path traversal in module blocked");
      free(output);
      output = NULL;
   }

   /* Module starting with dot */
   rc = instruction_loader_load(CWD_TOOL_NAME, ".hidden", &output);
   if (rc == 0 && output) {
      /* Should only have _core.md, not the .hidden module */
      ASSERT(strstr(output, "Core rules") != NULL, "only _core.md loaded, dot module skipped");
      free(output);
      output = NULL;
   }
}

/* ============================================================================
 * 8. Core-Only Loading (no modules)
 * ============================================================================ */

static void test_core_only(void) {
   printf("\n=== test_core_only ===\n");
   char *output = NULL;

   int rc = instruction_loader_load(CWD_TOOL_NAME, "", &output);
   ASSERT(rc == 0, "core-only load succeeds");
   ASSERT(output != NULL, "output not NULL");
   if (output) {
      ASSERT(strstr(output, "Core rules") != NULL, "_core.md content present");
      ASSERT(strstr(output, "Formal") == NULL, "no module content");
      ASSERT(strstr(output, "Casual") == NULL, "no module content");
      free(output);
   }
}

/* ============================================================================
 * 9. Base Directory API
 * ============================================================================ */

static void test_base_dir_api(void) {
   printf("\n=== test_base_dir_api ===\n");

   const char *dir = instruction_loader_get_base_dir();
   ASSERT(dir != NULL, "base dir not NULL");
   ASSERT(strlen(dir) > 0, "base dir not empty");
   ASSERT(strcmp(dir, INSTRUCTION_LOADER_DEFAULT_DIR) == 0, "base dir matches default");
}

/* ============================================================================
 * Main
 * ============================================================================ */

int main(void) {
   printf("=== Instruction Loader Unit Tests ===\n");

   /* Set up test files in CWD-relative path */
   setup_cwd_test_files();

   test_null_inputs();
   test_single_module();
   test_multiple_modules();
   test_whitespace_handling();
   test_missing_module();
   test_missing_tool();
   test_path_traversal();
   test_core_only();
   test_base_dir_api();

   /* Clean up */
   cleanup_cwd_test_files();

   printf("\n=== Results: %d passed, %d failed ===\n", tests_passed, tests_failed);
   return tests_failed > 0 ? 1 : 0;
}
