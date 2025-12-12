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
 * String Utilities - Common string functions shared across tools
 *
 * This module provides portable string utility functions that may not be
 * available on all platforms or that need consistent behavior.
 */

#ifndef STRING_UTILS_H
#define STRING_UTILS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Sanitize string for safe use in JSON and LLM APIs
 *
 * Removes or replaces characters that cause problems with JSON parsing or
 * LLM API calls:
 * - Invalid UTF-8 sequences are replaced with '?'
 * - Control characters (except \n, \r, \t) are removed
 * - High surrogate/private use area codepoints are replaced
 *
 * Modifies the string in-place for efficiency. Safe to call on any string
 * that will be embedded in JSON or sent to an API.
 *
 * Thread Safety: This function is thread-safe (modifies only the input buffer).
 *
 * @param str String to sanitize (modified in-place)
 */
void sanitize_utf8_for_json(char *str);

/**
 * @brief Case-insensitive substring search (portable implementation)
 *
 * Finds the first occurrence of needle in haystack, ignoring case.
 * This is a portable alternative to the non-standard strcasestr().
 *
 * Thread Safety: This function is thread-safe (uses only input parameters).
 *
 * @param haystack String to search in
 * @param needle Substring to find
 * @return Pointer to first occurrence, or NULL if not found
 */
const char *strcasestr_portable(const char *haystack, const char *needle);

#ifdef __cplusplus
}
#endif

#endif /* STRING_UTILS_H */
