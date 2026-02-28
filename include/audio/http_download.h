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
 * HTTP file download utility — downloads a URL to a temporary file
 */

#ifndef HTTP_DOWNLOAD_H
#define HTTP_DOWNLOAD_H

#include <curl/curl.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Download a URL to a temporary file
 *
 * Creates a temp file with mkstemps() using the given suffix (e.g., ".flac"),
 * downloads the URL content, sets permissions to 0600.
 *
 * The caller is responsible for unlinking the file when done.
 *
 * @param curl      Reusable CURL handle (caller manages lifecycle)
 * @param url       Full URL to download
 * @param headers   Optional CURL header list (e.g., for auth), or NULL
 * @param suffix    File suffix including dot (e.g., ".flac"), or NULL for none
 * @param prefix    Temp file prefix (e.g., "/tmp/dawn_plex_")
 * @param max_size  Maximum download size in bytes (0 = no limit)
 * @param out_path  Output: path to created temp file
 * @param out_path_size Size of out_path buffer
 * @return 0 on success, non-zero on error (temp file cleaned up on error)
 */
int http_download_to_temp(CURL *curl,
                          const char *url,
                          struct curl_slist *headers,
                          const char *suffix,
                          const char *prefix,
                          int64_t max_size,
                          char *out_path,
                          size_t out_path_size);

#ifdef __cplusplus
}
#endif

#endif /* HTTP_DOWNLOAD_H */
