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
 * Safe snprintf accumulator for building strings into fixed-size buffers.
 *
 * Prevents the classic snprintf accumulation bug where offset += snprintf(...)
 * accumulates the would-be length on truncation, causing size_t underflow
 * when computing remaining space (buf_size - offset).
 */

#ifndef BUF_PRINTF_H
#define BUF_PRINTF_H

#include <stddef.h>
#include <stdio.h>

/**
 * @brief Safe snprintf accumulator — appends formatted text to a buffer.
 *
 * Tracks @p off (bytes written) and @p rem (bytes remaining) correctly
 * even when snprintf truncates. On truncation, off is clamped to the
 * buffer end and rem is set to 0, preventing subsequent writes.
 *
 * @param buf   char* buffer being written to
 * @param off   size_t offset variable (updated in place)
 * @param rem   size_t remaining variable (updated in place)
 * @param ...   printf-style format string and arguments
 *
 * Usage:
 * @code
 *   char buf[256];
 *   size_t off = 0, rem = sizeof(buf);
 *   BUF_PRINTF(buf, off, rem, "hello %s", name);
 *   BUF_PRINTF(buf, off, rem, ", age %d", age);
 *   // off = total bytes written, rem = space left, buf is always null-terminated
 * @endcode
 */
#define BUF_PRINTF(buf, off, rem, ...)                              \
   do {                                                             \
      if ((rem) > 0) {                                              \
         int _bp_n = snprintf((buf) + (off), (rem), __VA_ARGS__);   \
         if (_bp_n > 0 && (size_t)_bp_n < (rem)) {                  \
            (off) += (size_t)_bp_n;                                 \
            (rem) -= (size_t)_bp_n;                                 \
         } else if (_bp_n > 0) {                                    \
            (off) += (rem)-1; /* snprintf null-terminated at end */ \
            (rem) = 0;                                              \
         }                                                          \
      }                                                             \
   } while (0)

#endif /* BUF_PRINTF_H */
