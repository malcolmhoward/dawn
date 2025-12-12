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
 */

#include "tools/string_utils.h"

#include <string.h>
#include <strings.h>

void sanitize_utf8_for_json(char *str) {
   if (!str)
      return;

   unsigned char *src = (unsigned char *)str;
   unsigned char *dst = (unsigned char *)str;

   while (*src) {
      unsigned char c = *src;

      // ASCII printable or allowed whitespace
      if (c >= 32 && c < 127) {
         *dst++ = c;
         src++;
      } else if (c == '\n' || c == '\r' || c == '\t') {
         *dst++ = c;
         src++;
      } else if (c < 32) {
         // Control character - skip
         src++;
      } else if ((c & 0xE0) == 0xC0) {
         // 2-byte UTF-8 sequence - check for truncation before accessing src[1]
         if (src[1] != '\0' && (src[1] & 0xC0) == 0x80) {
            // Valid 2-byte sequence - keep it
            *dst++ = *src++;
            *dst++ = *src++;
         } else {
            // Invalid/truncated sequence - replace with ?
            *dst++ = '?';
            src++;
         }
      } else if ((c & 0xF0) == 0xE0) {
         // 3-byte UTF-8 sequence - check for truncation before accessing src[1], src[2]
         if (src[1] != '\0' && src[2] != '\0' && (src[1] & 0xC0) == 0x80 &&
             (src[2] & 0xC0) == 0x80) {
            // Check for problematic codepoints (surrogates U+D800-U+DFFF)
            unsigned int codepoint = ((c & 0x0F) << 12) | ((src[1] & 0x3F) << 6) | (src[2] & 0x3F);
            if (codepoint >= 0xD800 && codepoint <= 0xDFFF) {
               // Surrogate - invalid in UTF-8
               *dst++ = '?';
               src += 3;
            } else {
               // Valid 3-byte sequence - keep it
               *dst++ = *src++;
               *dst++ = *src++;
               *dst++ = *src++;
            }
         } else {
            // Invalid/truncated sequence - replace with ?
            *dst++ = '?';
            src++;
         }
      } else if ((c & 0xF8) == 0xF0) {
         // 4-byte UTF-8 sequence - check for truncation before accessing src[1-3]
         if (src[1] != '\0' && src[2] != '\0' && src[3] != '\0' && (src[1] & 0xC0) == 0x80 &&
             (src[2] & 0xC0) == 0x80 && (src[3] & 0xC0) == 0x80) {
            // Valid 4-byte sequence - keep it
            *dst++ = *src++;
            *dst++ = *src++;
            *dst++ = *src++;
            *dst++ = *src++;
         } else {
            // Invalid/truncated sequence - replace with ?
            *dst++ = '?';
            src++;
         }
      } else {
         // Invalid UTF-8 start byte (0x80-0xBF or 0xF8-0xFF)
         *dst++ = '?';
         src++;
      }
   }
   *dst = '\0';
}

const char *strcasestr_portable(const char *haystack, const char *needle) {
   if (!haystack || !needle)
      return NULL;
   size_t needle_len = strlen(needle);
   if (needle_len == 0)
      return haystack;

   for (; *haystack; haystack++) {
      if (strncasecmp(haystack, needle, needle_len) == 0) {
         return haystack;
      }
   }
   return NULL;
}
