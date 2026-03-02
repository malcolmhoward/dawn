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
 * Plex Database Sync Provider
 *
 * Fetches track metadata from a Plex Media Server and writes it into the
 * shared music_metadata table for unified search across all sources.
 */

#ifndef PLEX_DB_H
#define PLEX_DB_H

#include "audio/music_source.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Get the Plex source provider (for scanner registration) */
const music_source_provider_t *plex_db_get_provider(void);

#ifdef __cplusplus
}
#endif

#endif /* PLEX_DB_H */
