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
 * Generic instruction loader for the two-step tool pattern.
 * Reads markdown files from disk, concatenates _core.md + requested modules,
 * and returns the content for LLM context injection.
 */

#include "tools/instruction_loader.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "logging.h"

#define SEPARATOR "\n\n---\n\n"
#define SEPARATOR_LEN 7

/**
 * @brief Validate a module name for path traversal safety
 *
 * Rejects names containing '/', '\', '..', or starting with '.'.
 * Module names should be simple identifiers like "diagram" or "chart".
 */
static bool is_safe_module_name(const char *name) {
   if (!name || name[0] == '\0' || name[0] == '.') {
      return false;
   }

   for (const char *p = name; *p != '\0'; p++) {
      if (*p == '/' || *p == '\\') {
         return false;
      }
      /* Check for ".." anywhere in the name */
      if (*p == '.' && *(p + 1) == '.') {
         return false;
      }
   }

   return true;
}

/**
 * @brief Read a file into a buffer, appending at offset
 *
 * Uses stat() to determine file size, checks against remaining capacity,
 * reads the full file content.
 *
 * @param path       File path to read
 * @param buf        Output buffer
 * @param buf_size   Total buffer capacity
 * @param offset     Current write position (updated on success)
 * @return 0 on success, -1 on error
 */
static int read_file_into(const char *path, char *buf, size_t buf_size, size_t *offset) {
   struct stat st;
   if (stat(path, &st) != 0) {
      return -1; /* File not found — not an error for optional files */
   }

   size_t file_size = (size_t)st.st_size;
   if (file_size == 0) {
      return 0;
   }

   /* Check if file fits in remaining buffer space */
   size_t remaining = buf_size - *offset - 1; /* -1 for null terminator */
   if (file_size > remaining) {
      LOG_WARNING("Instruction file too large: %s (%zu bytes, %zu remaining)", path, file_size,
                  remaining);
      return -1;
   }

   FILE *f = fopen(path, "r");
   if (!f) {
      return -1;
   }

   size_t bytes_read = fread(buf + *offset, 1, file_size, f);
   fclose(f);

   *offset += bytes_read;
   return 0;
}

/**
 * @brief Append a separator between sections
 */
static void append_separator(char *buf, size_t buf_size, size_t *offset) {
   if (*offset + SEPARATOR_LEN < buf_size - 1) {
      memcpy(buf + *offset, SEPARATOR, SEPARATOR_LEN);
      *offset += SEPARATOR_LEN;
   }
}

const char *instruction_loader_get_base_dir(void) {
   return INSTRUCTION_LOADER_DEFAULT_DIR;
}

int instruction_loader_load(const char *tool_name, const char *modules, char **output) {
   *output = NULL;

   if (!tool_name || tool_name[0] == '\0') {
      LOG_ERROR("instruction_loader: tool_name is empty");
      return 1;
   }

   if (!is_safe_module_name(tool_name)) {
      LOG_ERROR("instruction_loader: invalid tool_name '%s'", tool_name);
      return 1;
   }

   /* Allocate output buffer */
   char *buf = (char *)malloc(INSTRUCTION_LOADER_MAX_SIZE);
   if (!buf) {
      LOG_ERROR("instruction_loader: failed to allocate %d bytes", INSTRUCTION_LOADER_MAX_SIZE);
      return 1;
   }

   const char *base_dir = instruction_loader_get_base_dir();
   char path[1024];
   size_t offset = 0;

   /* Always load _core.md first if it exists */
   snprintf(path, sizeof(path), "%s/%s/_core.md", base_dir, tool_name);
   if (read_file_into(path, buf, INSTRUCTION_LOADER_MAX_SIZE, &offset) == 0 && offset > 0) {
      LOG_INFO("instruction_loader: loaded _core.md (%zu bytes)", offset);
   }

   /* Parse and load each requested module */
   if (modules && modules[0] != '\0') {
      /* Make a mutable copy for strtok */
      char *modules_copy = strdup(modules);
      if (!modules_copy) {
         free(buf);
         return 1;
      }

      char *saveptr = NULL;
      char *module = strtok_r(modules_copy, ",", &saveptr);

      while (module != NULL) {
         /* Trim leading whitespace */
         while (*module == ' ') {
            module++;
         }

         /* Trim trailing whitespace */
         size_t len = strlen(module);
         while (len > 0 && module[len - 1] == ' ') {
            module[--len] = '\0';
         }

         if (len == 0) {
            module = strtok_r(NULL, ",", &saveptr);
            continue;
         }

         if (!is_safe_module_name(module)) {
            LOG_WARNING("instruction_loader: skipping invalid module name '%s'", module);
            module = strtok_r(NULL, ",", &saveptr);
            continue;
         }

         /* Add separator before this module if we already have content */
         if (offset > 0) {
            append_separator(buf, INSTRUCTION_LOADER_MAX_SIZE, &offset);
         }

         snprintf(path, sizeof(path), "%s/%s/%s.md", base_dir, tool_name, module);
         size_t before = offset;
         if (read_file_into(path, buf, INSTRUCTION_LOADER_MAX_SIZE, &offset) == 0) {
            LOG_INFO("instruction_loader: loaded %s.md (%zu bytes)", module, offset - before);
         } else {
            LOG_WARNING("instruction_loader: module not found: %s/%s", tool_name, module);
            /* Remove the separator we added */
            offset = before > SEPARATOR_LEN ? before - SEPARATOR_LEN : before;
         }

         module = strtok_r(NULL, ",", &saveptr);
      }

      free(modules_copy);
   }

   /* Null-terminate */
   buf[offset] = '\0';

   if (offset == 0) {
      LOG_WARNING("instruction_loader: no content loaded for tool '%s' modules '%s'", tool_name,
                  modules ? modules : "(none)");
      free(buf);
      return 1;
   }

   /* Shrink buffer to actual size */
   char *result = (char *)realloc(buf, offset + 1);
   *output = result ? result : buf;

   LOG_INFO("instruction_loader: loaded %zu bytes total for %s", offset, tool_name);
   return 0;
}
