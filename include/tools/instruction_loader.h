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
 * Reads markdown instruction files from disk and returns their content
 * for injection into the LLM conversation context.
 *
 * See docs/TWO_STEP_TOOL_PATTERN.md for the design rationale.
 */

#ifndef INSTRUCTION_LOADER_H
#define INSTRUCTION_LOADER_H

#ifdef __cplusplus
extern "C" {
#endif

/** Maximum size for loaded instruction content (128 KB) */
#define INSTRUCTION_LOADER_MAX_SIZE (128 * 1024)

/** Default base directory for tool instruction files */
#define INSTRUCTION_LOADER_DEFAULT_DIR "tool_instructions"

/**
 * @brief Load instruction files for a tool
 *
 * Reads _core.md (if present) plus each requested module file from the
 * tool's instruction directory. Module names are sanitized to prevent
 * path traversal. Output is dynamically allocated and must be freed by
 * the caller.
 *
 * @param tool_name  Tool directory name (e.g., "render_visual")
 * @param modules    Comma-separated module names (e.g., "diagram,chart")
 * @param output     Output: heap-allocated string with concatenated content
 * @return 0 on success, non-zero on error. *output is NULL on error.
 */
int instruction_loader_load(const char *tool_name, const char *modules, char **output);

/**
 * @brief Get the configured instructions base directory
 *
 * Returns the base directory path. Currently returns the compile-time
 * default; will support dawn.toml override in the future.
 *
 * @return Path string (do not free)
 */
const char *instruction_loader_get_base_dir(void);

#ifdef __cplusplus
}
#endif

#endif /* INSTRUCTION_LOADER_H */
