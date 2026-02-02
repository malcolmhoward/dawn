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
 * Path Utilities - Common path manipulation functions
 */

#ifndef DAWN_PATH_UTILS_H
#define DAWN_PATH_UTILS_H

#include <stdbool.h>
#include <stddef.h>

/**
 * @brief Expand tilde in path to home directory
 *
 * Handles paths like "~/Music" -> "/home/user/Music"
 * Falls back to getpwuid() if HOME environment variable is not set.
 *
 * @param path Original path (may contain leading ~/)
 * @param expanded Output buffer for expanded path
 * @param expanded_size Size of output buffer
 * @return true if expansion occurred or path copied unchanged, false on error
 *
 * @note Thread-safe. Uses getenv() and getpwuid() which are MT-safe on Linux.
 */
bool path_expand_tilde(const char *path, char *expanded, size_t expanded_size);

/**
 * @brief Canonicalize path by resolving symlinks and relative components
 *
 * Wrapper around realpath() with additional validation.
 * Resolves "..", ".", and symbolic links to produce an absolute path.
 *
 * @param path Path to canonicalize (may contain ~/, .., symlinks)
 * @param canonical Output buffer for canonical path
 * @param canonical_size Size of output buffer
 * @return true on success, false if path doesn't exist or buffer too small
 *
 * @note The path must exist for canonicalization to succeed.
 */
bool path_canonicalize(const char *path, char *canonical, size_t canonical_size);

/**
 * @brief Check if a path is within a specified root directory
 *
 * Uses canonicalization to prevent symlink and ".." escape attacks.
 * Both paths are canonicalized before comparison.
 *
 * @param path Path to check
 * @param root_dir Root directory that path must be within
 * @return true if path is within root_dir, false otherwise
 *
 * @note Both paths must exist for this check to succeed.
 */
bool path_is_within_root(const char *path, const char *root_dir);

/**
 * @brief Copy string safely with guaranteed null termination
 *
 * Unlike strncpy(), this always null-terminates the destination.
 * Silently truncates if source is longer than destination.
 *
 * @param dst Destination buffer
 * @param src Source string (may be NULL)
 * @param dst_size Size of destination buffer
 *
 * @note If src is NULL, dst is set to empty string.
 */
void safe_strncpy(char *dst, const char *src, size_t dst_size);

/**
 * @brief Ensure parent directory exists for a file path
 *
 * Creates the parent directory if it doesn't exist (single level only).
 * Uses mode 0755 for the directory.
 *
 * @param file_path Path to a file (parent directory will be created)
 * @return true on success or if directory already exists, false on error
 *
 * @note Only creates one level of directory. If multiple levels are missing,
 *       this will fail. The file_path should be an expanded absolute path.
 */
bool path_ensure_parent_dir(const char *file_path);

#endif /* DAWN_PATH_UTILS_H */
