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
 * WebUI Music Streaming - Message Handlers
 *
 * WebSocket message handlers and LLM tool integration for music streaming.
 * Split from webui_music.c for maintainability.
 */

#include <json-c/json.h>
#include <stdlib.h>
#include <string.h>

#include "audio/music_db.h"
#include "audio/plex_client.h"
#include "core/path_utils.h"
#include "logging.h"
#include "tools/volume_tool.h"
#include "webui/webui_music_internal.h"
#include "webui/webui_server.h"

/* =============================================================================
 * Helpers
 * ============================================================================= */

/**
 * @brief Validate a Plex API key (artist_key or album_key) from client input.
 *
 * Plex keys should be paths like "/library/metadata/12345". Reject anything
 * containing path traversal, query strings, fragments, or authority injection.
 *
 * @return true if the key is safe to pass to plex_client_* functions
 */
static bool validate_plex_key(const char *key) {
   if (!key || key[0] == '\0')
      return false;
   /* Plex ratingKeys are purely numeric (e.g. "12345") */
   for (const char *p = key; *p; p++) {
      if (*p < '0' || *p > '9')
         return false;
   }
   return true;
}

/* =============================================================================
 * Message Handlers
 * ============================================================================= */

void handle_music_subscribe(ws_connection_t *conn, struct json_object *payload) {
   if (!conn_is_satellite_session(conn) && !conn_require_auth(conn)) {
      return;
   }

   if (!webui_music_is_available()) {
      webui_music_send_error(conn, "UNAVAILABLE", "Music streaming is not available");
      return;
   }

   /* Initialize session state if needed */
   if (!conn->music_state) {
      if (webui_music_session_init(conn) != 0) {
         webui_music_send_error(conn, "INIT_ERROR", "Failed to initialize music session");
         return;
      }
   }

   session_music_state_t *state = (session_music_state_t *)conn->music_state;

   /* Parse quality and bitrate mode settings */
   if (payload) {
      bool reconfigure = false;
      music_quality_t new_quality = state->quality;
      music_bitrate_mode_t new_bitrate_mode = state->bitrate_mode;

      struct json_object *quality_obj;
      if (json_object_object_get_ex(payload, "quality", &quality_obj)) {
         new_quality = webui_music_parse_quality(json_object_get_string(quality_obj));
         if (new_quality != state->quality) {
            reconfigure = true;
         }
      }

      struct json_object *bitrate_mode_obj;
      if (json_object_object_get_ex(payload, "bitrate_mode", &bitrate_mode_obj)) {
         const char *mode_str = json_object_get_string(bitrate_mode_obj);
         if (mode_str && strcmp(mode_str, "cbr") == 0) {
            new_bitrate_mode = MUSIC_BITRATE_CBR;
         } else {
            new_bitrate_mode = MUSIC_BITRATE_VBR;
         }
         if (new_bitrate_mode != state->bitrate_mode) {
            reconfigure = true;
         }
      }

      if (reconfigure) {
         /* Update state values immediately for display purposes */
         pthread_mutex_lock(&state->state_mutex);
         state->quality = new_quality;
         state->bitrate_mode = new_bitrate_mode;
         pthread_mutex_unlock(&state->state_mutex);

         /* Set pending values for streaming thread to reconfigure encoder safely */
         state->pending_quality = new_quality;
         state->pending_bitrate_mode = new_bitrate_mode;
         atomic_store(&state->reconfigure_requested, true);
      }
   }

   /* Send current state */
   pthread_mutex_lock(&state->state_mutex);
   webui_music_send_state(conn, state);
   pthread_mutex_unlock(&state->state_mutex);

   LOG_INFO("WebUI music: Client subscribed (quality: %s, bitrate: %s)",
            QUALITY_NAMES[state->quality],
            state->bitrate_mode == MUSIC_BITRATE_VBR ? "VBR" : "CBR");
}

void handle_music_unsubscribe(ws_connection_t *conn) {
   if (!conn_is_satellite_session(conn) && !conn_require_auth(conn)) {
      return;
   }

   session_music_state_t *state = (session_music_state_t *)conn->music_state;
   if (!state) {
      return;
   }

   /* Stop playback and streaming */
   pthread_mutex_lock(&state->state_mutex);
   state->playing = false;
   state->paused = false;
   pthread_mutex_unlock(&state->state_mutex);

   webui_music_stop_streaming(state);

   LOG_INFO("WebUI music: Client unsubscribed");
}

void handle_music_control(ws_connection_t *conn, struct json_object *payload) {
   if (!conn_is_satellite_session(conn) && !conn_require_auth(conn)) {
      return;
   }

   session_music_state_t *state = (session_music_state_t *)conn->music_state;
   if (!state) {
      /* Auto-initialize on first control message */
      if (webui_music_session_init(conn) != 0) {
         webui_music_send_error(conn, "INIT_ERROR", "Failed to initialize music session");
         return;
      }
      state = (session_music_state_t *)conn->music_state;
   }

   struct json_object *action_obj;
   if (!json_object_object_get_ex(payload, "action", &action_obj)) {
      webui_music_send_error(conn, "INVALID_REQUEST", "Missing action");
      return;
   }

   const char *action = json_object_get_string(action_obj);
   LOG_INFO("WebUI music: Control action '%s'", action);

   if (strcmp(action, "play") == 0) {
      /* Play specific track by path, or search query */
      struct json_object *path_obj;
      struct json_object *query_obj;

      if (json_object_object_get_ex(payload, "path", &path_obj)) {
         /* Play specific track by path - add to top of queue and play */
         const char *path = json_object_get_string(path_obj);

         /* Security: validate path is within music library */
         if (!webui_music_is_path_valid(path)) {
            webui_music_send_error(conn, "INVALID_PATH", "Path not in music library");
            return;
         }

         /* Get track metadata — skip DB lookup for Plex paths */
         music_search_result_t track_info;
         if (strncmp(path, "plex:", 5) == 0) {
            extract_plex_track_meta(payload, path, &track_info);
         } else if (music_db_get_by_path(path, &track_info) != 0) {
            /* Fallback: use path as title */
            safe_strncpy(track_info.path, path, sizeof(track_info.path));
            safe_strncpy(track_info.title, path, sizeof(track_info.title));
            track_info.artist[0] = '\0';
            track_info.album[0] = '\0';
            track_info.duration_sec = 0;
         }

         /* Stop any current playback */
         webui_music_stop_streaming(state);
         pthread_mutex_lock(&state->state_mutex);
         if (state->decoder) {
            audio_decoder_close(state->decoder);
            state->decoder = NULL;
         }

         /* Shift queue down and insert at position 0 */
         if (state->queue_length < WEBUI_MUSIC_MAX_QUEUE) {
            memmove(&state->queue[1], &state->queue[0],
                    state->queue_length * sizeof(music_queue_entry_t));
            state->queue_length++;
         } else {
            /* Queue full - shift and lose last item */
            memmove(&state->queue[1], &state->queue[0],
                    (WEBUI_MUSIC_MAX_QUEUE - 1) * sizeof(music_queue_entry_t));
         }

         /* Insert track at top */
         music_queue_entry_t *entry = &state->queue[0];
         safe_strncpy(entry->path, track_info.path, sizeof(entry->path));
         safe_strncpy(entry->title, track_info.title, sizeof(entry->title));
         safe_strncpy(entry->artist, track_info.artist, sizeof(entry->artist));
         safe_strncpy(entry->album, track_info.album, sizeof(entry->album));
         entry->duration_sec = track_info.duration_sec;
         state->queue_index = 0;
         pthread_mutex_unlock(&state->state_mutex);

         /* Start playback */
         if (webui_music_start_playback(state, path) != 0) {
            webui_music_send_error(conn, "PLAYBACK_ERROR", "Failed to start playback");
            return;
         }

         /* Send state update so client shows track info immediately */
         pthread_mutex_lock(&state->state_mutex);
         webui_music_send_state(conn, state);
         pthread_mutex_unlock(&state->state_mutex);

      } else if (json_object_object_get_ex(payload, "query", &query_obj)) {
         const char *query = json_object_get_string(query_obj);
         bool use_plex = g_music_use_plex;

         /* Stop any current playback */
         webui_music_stop_streaming(state);
         pthread_mutex_lock(&state->state_mutex);
         if (state->decoder) {
            audio_decoder_close(state->decoder);
            state->decoder = NULL;
         }
         state->queue_length = 0;
         state->queue_index = 0;
         pthread_mutex_unlock(&state->state_mutex);

         if (use_plex) {
            /* Search Plex and build queue from JSON results */
            if (!plex_client_is_configured()) {
               webui_music_send_error(conn, "UNAVAILABLE", "Plex not configured");
               return;
            }
            json_object *plex_result = plex_client_search(query, WEBUI_MUSIC_MAX_QUEUE);
            if (!plex_result) {
               webui_music_send_error(conn, "PLEX_ERROR", "Plex search failed");
               return;
            }
            json_object *results_arr;
            int count = 0;
            if (json_object_object_get_ex(plex_result, "results", &results_arr)) {
               count = (int)json_object_array_length(results_arr);
            }
            if (count <= 0) {
               json_object_put(plex_result);
               webui_music_send_error(conn, "NOT_FOUND", "No music found matching query");
               return;
            }

            pthread_mutex_lock(&state->state_mutex);
            for (int i = 0; i < count && state->queue_length < WEBUI_MUSIC_MAX_QUEUE; i++) {
               json_object *item = json_object_array_get_idx(results_arr, i);
               queue_entry_from_plex_json(&state->queue[state->queue_length], item);
               state->queue_length++;
            }
            pthread_mutex_unlock(&state->state_mutex);
            json_object_put(plex_result);

         } else {
            /* Search local database */
            music_search_result_t *results = malloc(WEBUI_MUSIC_MAX_QUEUE *
                                                    sizeof(music_search_result_t));
            if (!results) {
               webui_music_send_error(conn, "MEMORY_ERROR", "Failed to allocate search buffer");
               return;
            }

            int count = music_db_search(query, results, WEBUI_MUSIC_MAX_QUEUE);
            if (count <= 0) {
               free(results);
               webui_music_send_error(conn, "NOT_FOUND", "No music found matching query");
               return;
            }

            pthread_mutex_lock(&state->state_mutex);
            for (int i = 0; i < count && state->queue_length < WEBUI_MUSIC_MAX_QUEUE; i++) {
               music_queue_entry_t *entry = &state->queue[state->queue_length];
               safe_strncpy(entry->path, results[i].path, sizeof(entry->path));
               safe_strncpy(entry->title, results[i].title, sizeof(entry->title));
               safe_strncpy(entry->artist, results[i].artist, sizeof(entry->artist));
               safe_strncpy(entry->album, results[i].album, sizeof(entry->album));
               entry->duration_sec = results[i].duration_sec;
               state->queue_length++;
            }
            pthread_mutex_unlock(&state->state_mutex);
            free(results);
         }

         /* Start playback */
         if (webui_music_start_playback(state, state->queue[0].path) != 0) {
            webui_music_send_error(conn, "PLAYBACK_ERROR", "Failed to start playback");
            return;
         }

         /* Send state update so client shows track info immediately */
         pthread_mutex_lock(&state->state_mutex);
         webui_music_send_state(conn, state);
         pthread_mutex_unlock(&state->state_mutex);

      } else if (state->paused) {
         /* Resume from pause */
         pthread_mutex_lock(&state->state_mutex);
         state->paused = false;
         webui_music_send_state(conn, state);
         pthread_mutex_unlock(&state->state_mutex);
      } else {
         /* Already playing - just send state */
         pthread_mutex_lock(&state->state_mutex);
         webui_music_send_state(conn, state);
         pthread_mutex_unlock(&state->state_mutex);
      }

   } else if (strcmp(action, "pause") == 0) {
      pthread_mutex_lock(&state->state_mutex);
      state->paused = true;
      webui_music_send_state(conn, state);
      pthread_mutex_unlock(&state->state_mutex);

   } else if (strcmp(action, "stop") == 0) {
      /* Stop streaming thread completely before closing decoder.
       * This ensures no use-after-free - thread is fully stopped via pthread_join. */
      webui_music_stop_streaming(state);

      pthread_mutex_lock(&state->state_mutex);
      state->playing = false;
      state->paused = false;
      if (state->decoder) {
         audio_decoder_close(state->decoder);
         state->decoder = NULL;
      }
      state->position_frames = 0;
      webui_music_send_state(conn, state);
      pthread_mutex_unlock(&state->state_mutex);

   } else if (strcmp(action, "next") == 0) {
      pthread_mutex_lock(&state->state_mutex);
      bool can_advance = false;
      if (state->queue_length > 0) {
         if (state->shuffle && state->queue_length > 1) {
            state->queue_index = webui_music_pick_random_index(state);
            can_advance = true;
         } else if (state->queue_index < state->queue_length - 1) {
            state->queue_index++;
            can_advance = true;
         } else if (state->repeat_mode == MUSIC_REPEAT_ALL) {
            state->queue_index = 0;
            can_advance = true;
         }
      }
      if (can_advance) {
         const char *path = state->queue[state->queue_index].path;
         pthread_mutex_unlock(&state->state_mutex);
         webui_music_start_playback(state, path);
      } else {
         pthread_mutex_unlock(&state->state_mutex);
      }

      pthread_mutex_lock(&state->state_mutex);
      webui_music_send_state(conn, state);
      pthread_mutex_unlock(&state->state_mutex);

   } else if (strcmp(action, "previous") == 0) {
      pthread_mutex_lock(&state->state_mutex);
      bool can_go_back = false;
      if (state->queue_length > 0) {
         if (state->shuffle && state->queue_length > 1) {
            state->queue_index = webui_music_pick_random_index(state);
            can_go_back = true;
         } else if (state->queue_index > 0) {
            state->queue_index--;
            can_go_back = true;
         } else if (state->repeat_mode == MUSIC_REPEAT_ALL) {
            state->queue_index = state->queue_length - 1;
            can_go_back = true;
         }
      }
      if (can_go_back) {
         const char *path = state->queue[state->queue_index].path;
         pthread_mutex_unlock(&state->state_mutex);
         webui_music_start_playback(state, path);
      } else {
         pthread_mutex_unlock(&state->state_mutex);
      }

      pthread_mutex_lock(&state->state_mutex);
      webui_music_send_state(conn, state);
      pthread_mutex_unlock(&state->state_mutex);

   } else if (strcmp(action, "toggle_shuffle") == 0) {
      pthread_mutex_lock(&state->state_mutex);
      state->shuffle = !state->shuffle;
      webui_music_send_state(conn, state);
      pthread_mutex_unlock(&state->state_mutex);

   } else if (strcmp(action, "cycle_repeat") == 0) {
      pthread_mutex_lock(&state->state_mutex);
      state->repeat_mode = (music_repeat_mode_t)((state->repeat_mode + 1) % 3);
      webui_music_send_state(conn, state);
      pthread_mutex_unlock(&state->state_mutex);

   } else if (strcmp(action, "seek") == 0) {
      struct json_object *position_obj;
      if (json_object_object_get_ex(payload, "position_sec", &position_obj)) {
         double position_sec = json_object_get_double(position_obj);

         /* Stop streaming thread to safely access decoder
          * This prevents race conditions where thread reads while we seek */
         bool was_streaming = atomic_load(&state->streaming);
         if (was_streaming) {
            webui_music_stop_streaming(state);
         }

         pthread_mutex_lock(&state->state_mutex);
         if (state->decoder && state->source_rate > 0) {
            uint64_t sample_pos = (uint64_t)(position_sec * state->source_rate);
            if (audio_decoder_seek(state->decoder, sample_pos) == 0) {
               state->position_frames = sample_pos;
               /* Clear accumulation buffer after seek */
               state->resample_accum_count = 0;
            }
         }
         webui_music_send_state(conn, state);
         pthread_mutex_unlock(&state->state_mutex);

         /* Restart streaming if it was active */
         if (was_streaming && state->playing) {
            webui_music_start_streaming(state);
         }
      }

   } else if (strcmp(action, "play_index") == 0) {
      /* Play specific track from queue by index */
      struct json_object *index_obj;
      if (!json_object_object_get_ex(payload, "index", &index_obj)) {
         webui_music_send_error(conn, "INVALID_REQUEST", "Missing index");
         return;
      }
      int play_index = json_object_get_int(index_obj);

      pthread_mutex_lock(&state->state_mutex);
      if (play_index < 0 || play_index >= state->queue_length) {
         pthread_mutex_unlock(&state->state_mutex);
         webui_music_send_error(conn, "INVALID_INDEX", "Index out of range");
         return;
      }

      state->queue_index = play_index;
      const char *path = state->queue[play_index].path;
      pthread_mutex_unlock(&state->state_mutex);

      /* Start playback (handles closing existing decoder with proper wait) */
      if (webui_music_start_playback(state, path) != 0) {
         webui_music_send_error(conn, "PLAYBACK_ERROR", "Failed to start playback");
         return;
      }

      pthread_mutex_lock(&state->state_mutex);
      webui_music_send_state(conn, state);
      pthread_mutex_unlock(&state->state_mutex);

   } else if (strcmp(action, "add_to_queue") == 0) {
      /* Add track to end of queue without starting playback */
      struct json_object *path_obj;
      if (!json_object_object_get_ex(payload, "path", &path_obj)) {
         webui_music_send_error(conn, "INVALID_REQUEST", "Missing path");
         return;
      }
      const char *path = json_object_get_string(path_obj);

      /* Security: validate path is within music library */
      if (!webui_music_is_path_valid(path)) {
         webui_music_send_error(conn, "INVALID_PATH", "Path not in music library");
         return;
      }

      /* Get track metadata */
      music_search_result_t track_info;
      if (strncmp(path, "plex:", 5) == 0) {
         extract_plex_track_meta(payload, path, &track_info);
      } else if (music_db_get_by_path(path, &track_info) != 0) {
         safe_strncpy(track_info.path, path, sizeof(track_info.path));
         safe_strncpy(track_info.title, path, sizeof(track_info.title));
         track_info.artist[0] = '\0';
         track_info.album[0] = '\0';
         track_info.duration_sec = 0;
      }

      pthread_mutex_lock(&state->state_mutex);
      if (state->queue_length < WEBUI_MUSIC_MAX_QUEUE) {
         music_queue_entry_t *entry = &state->queue[state->queue_length];
         safe_strncpy(entry->path, track_info.path, sizeof(entry->path));
         safe_strncpy(entry->title, track_info.title, sizeof(entry->title));
         safe_strncpy(entry->artist, track_info.artist, sizeof(entry->artist));
         safe_strncpy(entry->album, track_info.album, sizeof(entry->album));
         entry->duration_sec = track_info.duration_sec;
         state->queue_length++;
      }
      webui_music_send_state(conn, state);
      pthread_mutex_unlock(&state->state_mutex);

   } else if (strcmp(action, "remove_from_queue") == 0) {
      /* Remove track from queue by index */
      struct json_object *index_obj;
      if (!json_object_object_get_ex(payload, "index", &index_obj)) {
         webui_music_send_error(conn, "INVALID_REQUEST", "Missing index");
         return;
      }
      int remove_index = json_object_get_int(index_obj);

      pthread_mutex_lock(&state->state_mutex);
      if (remove_index < 0 || remove_index >= state->queue_length) {
         pthread_mutex_unlock(&state->state_mutex);
         webui_music_send_error(conn, "INVALID_INDEX", "Index out of range");
         return;
      }

      bool removing_current = (remove_index == state->queue_index);
      bool was_playing = state->playing && !state->paused;

      /* Stop streaming before closing decoder to prevent use-after-free */
      if (removing_current) {
         pthread_mutex_unlock(&state->state_mutex);
         webui_music_stop_streaming(state);
         pthread_mutex_lock(&state->state_mutex);
      }

      /* Shift queue entries after the removed index */
      if (remove_index < state->queue_length - 1) {
         memmove(&state->queue[remove_index], &state->queue[remove_index + 1],
                 (state->queue_length - remove_index - 1) * sizeof(music_queue_entry_t));
      }
      state->queue_length--;

      /* Adjust current index if needed */
      if (remove_index < state->queue_index) {
         state->queue_index--;
      } else if (removing_current) {
         /* Removed current track - safe to close decoder (thread already stopped) */
         if (state->decoder) {
            audio_decoder_close(state->decoder);
            state->decoder = NULL;
         }

         if (state->queue_length > 0) {
            /* Adjust index if we were at end */
            if (state->queue_index >= state->queue_length) {
               state->queue_index = state->queue_length - 1;
            }

            if (was_playing) {
               /* Start playing the next track */
               const char *next_path = state->queue[state->queue_index].path;
               pthread_mutex_unlock(&state->state_mutex);
               webui_music_start_playback(state, next_path);
               pthread_mutex_lock(&state->state_mutex);
            }
         } else {
            /* Queue is now empty */
            state->playing = false;
            state->paused = false;
            state->queue_index = 0;
         }
      }

      webui_music_send_state(conn, state);
      pthread_mutex_unlock(&state->state_mutex);

   } else if (strcmp(action, "clear_queue") == 0) {
      /* Clear entire queue and stop playback */
      webui_music_stop_streaming(state);

      pthread_mutex_lock(&state->state_mutex);
      if (state->decoder) {
         audio_decoder_close(state->decoder);
         state->decoder = NULL;
      }
      state->playing = false;
      state->paused = false;
      state->queue_length = 0;
      state->queue_index = 0;
      state->position_frames = 0;
      webui_music_send_state(conn, state);
      pthread_mutex_unlock(&state->state_mutex);

   } else if (strcmp(action, "add_artist") == 0) {
      /* Add all tracks by an artist to queue */
      struct json_object *artist_obj;
      if (!json_object_object_get_ex(payload, "artist", &artist_obj)) {
         webui_music_send_error(conn, "INVALID_REQUEST", "Missing artist");
         return;
      }
      const char *artist_name = json_object_get_string(artist_obj);
      bool use_plex = g_music_use_plex;

      if (use_plex) {
         /* Plex: use artist_key to fetch all tracks via allLeaves */
         struct json_object *key_obj;
         if (!json_object_object_get_ex(payload, "artist_key", &key_obj)) {
            webui_music_send_error(conn, "INVALID_REQUEST",
                                   "Plex requires artist_key for add_artist");
            return;
         }
         const char *key_str = json_object_get_string(key_obj);
         if (!validate_plex_key(key_str)) {
            webui_music_send_error(conn, "INVALID_REQUEST", "Invalid Plex artist key");
            return;
         }
         json_object *plex_tracks = plex_client_list_artist_tracks(key_str);
         if (!plex_tracks) {
            webui_music_send_error(conn, "PLEX_ERROR", "Failed to fetch artist tracks");
            return;
         }
         json_object *tracks_arr;
         json_object_object_get_ex(plex_tracks, "tracks", &tracks_arr);
         int count = tracks_arr ? (int)json_object_array_length(tracks_arr) : 0;

         pthread_mutex_lock(&state->state_mutex);
         int added = 0;
         for (int i = 0; i < count && state->queue_length < WEBUI_MUSIC_MAX_QUEUE; i++) {
            json_object *item = json_object_array_get_idx(tracks_arr, i);
            queue_entry_from_plex_json(&state->queue[state->queue_length], item);
            state->queue_length++;
            added++;
         }
         webui_music_send_state(conn, state);
         pthread_mutex_unlock(&state->state_mutex);
         json_object_put(plex_tracks);
         LOG_INFO("WebUI music: Added %d Plex tracks by '%s' to queue", added, artist_name);
      } else {
         music_search_result_t *tracks = malloc(100 * sizeof(music_search_result_t));
         if (!tracks) {
            webui_music_send_error(conn, "MEMORY_ERROR", "Failed to allocate track list");
            return;
         }
         int count = music_db_get_by_artist(artist_name, tracks, 100);

         pthread_mutex_lock(&state->state_mutex);
         int added = 0;
         for (int i = 0; i < count && state->queue_length < WEBUI_MUSIC_MAX_QUEUE; i++) {
            music_queue_entry_t *entry = &state->queue[state->queue_length];
            safe_strncpy(entry->path, tracks[i].path, sizeof(entry->path));
            safe_strncpy(entry->title, tracks[i].title, sizeof(entry->title));
            safe_strncpy(entry->artist, tracks[i].artist, sizeof(entry->artist));
            safe_strncpy(entry->album, tracks[i].album, sizeof(entry->album));
            entry->duration_sec = tracks[i].duration_sec;
            state->queue_length++;
            added++;
         }
         webui_music_send_state(conn, state);
         pthread_mutex_unlock(&state->state_mutex);

         free(tracks);
         LOG_INFO("WebUI music: Added %d tracks by '%s' to queue", added, artist_name);
      }

   } else if (strcmp(action, "add_album") == 0) {
      /* Add all tracks from an album to queue */
      struct json_object *album_obj;
      if (!json_object_object_get_ex(payload, "album", &album_obj)) {
         webui_music_send_error(conn, "INVALID_REQUEST", "Missing album");
         return;
      }
      const char *album_name = json_object_get_string(album_obj);
      bool use_plex = g_music_use_plex;

      if (use_plex) {
         /* Plex: use album_key to fetch tracks */
         struct json_object *key_obj;
         if (!json_object_object_get_ex(payload, "album_key", &key_obj)) {
            webui_music_send_error(conn, "INVALID_REQUEST",
                                   "Plex requires album_key for add_album");
            return;
         }
         const char *key_str = json_object_get_string(key_obj);
         if (!validate_plex_key(key_str)) {
            webui_music_send_error(conn, "INVALID_REQUEST", "Invalid Plex album key");
            return;
         }
         json_object *plex_tracks = plex_client_list_tracks(key_str);
         if (!plex_tracks) {
            webui_music_send_error(conn, "PLEX_ERROR", "Failed to fetch album tracks");
            return;
         }
         json_object *tracks_arr;
         json_object_object_get_ex(plex_tracks, "tracks", &tracks_arr);
         int count = tracks_arr ? (int)json_object_array_length(tracks_arr) : 0;

         pthread_mutex_lock(&state->state_mutex);
         int added = 0;
         for (int i = 0; i < count && state->queue_length < WEBUI_MUSIC_MAX_QUEUE; i++) {
            json_object *item = json_object_array_get_idx(tracks_arr, i);
            queue_entry_from_plex_json(&state->queue[state->queue_length], item);
            state->queue_length++;
            added++;
         }
         webui_music_send_state(conn, state);
         pthread_mutex_unlock(&state->state_mutex);
         json_object_put(plex_tracks);
         LOG_INFO("WebUI music: Added %d Plex tracks from album '%s' to queue", added, album_name);
      } else {
         music_search_result_t *tracks = malloc(50 * sizeof(music_search_result_t));
         if (!tracks) {
            webui_music_send_error(conn, "MEMORY_ERROR", "Failed to allocate track list");
            return;
         }
         int count = music_db_get_by_album(album_name, tracks, 50);

         pthread_mutex_lock(&state->state_mutex);
         int added = 0;
         for (int i = 0; i < count && state->queue_length < WEBUI_MUSIC_MAX_QUEUE; i++) {
            music_queue_entry_t *entry = &state->queue[state->queue_length];
            safe_strncpy(entry->path, tracks[i].path, sizeof(entry->path));
            safe_strncpy(entry->title, tracks[i].title, sizeof(entry->title));
            safe_strncpy(entry->artist, tracks[i].artist, sizeof(entry->artist));
            safe_strncpy(entry->album, tracks[i].album, sizeof(entry->album));
            entry->duration_sec = tracks[i].duration_sec;
            state->queue_length++;
            added++;
         }
         webui_music_send_state(conn, state);
         pthread_mutex_unlock(&state->state_mutex);

         free(tracks);
         LOG_INFO("WebUI music: Added %d tracks from album '%s' to queue", added, album_name);
      }

   } else if (strcmp(action, "volume") == 0) {
      /* Set music volume for this session */
      struct json_object *level_obj;
      if (!json_object_object_get_ex(payload, "level", &level_obj)) {
         webui_music_send_error(conn, "INVALID_REQUEST", "Missing level");
         return;
      }
      double level = json_object_get_double(level_obj);
      if (level < 0.0)
         level = 0.0;
      if (level > 1.0)
         level = 1.0;
      conn->volume = (float)level;

      /* Send updated state so client confirms the value */
      pthread_mutex_lock(&state->state_mutex);
      webui_music_send_state(conn, state);
      pthread_mutex_unlock(&state->state_mutex);
      LOG_INFO("WebUI music: Volume set to %.0f%%", level * 100.0);

   } else {
      webui_music_send_error(conn, "UNKNOWN_ACTION", "Unknown control action");
   }
}

void handle_music_search(ws_connection_t *conn, struct json_object *payload) {
   if (!conn_is_satellite_session(conn) && !conn_require_auth(conn)) {
      return;
   }

   struct json_object *query_obj;
   if (!json_object_object_get_ex(payload, "query", &query_obj)) {
      webui_music_send_error(conn, "INVALID_REQUEST", "Missing query");
      return;
   }

   const char *query = json_object_get_string(query_obj);
   int limit = 50; /* Default limit */

   struct json_object *limit_obj;
   if (json_object_object_get_ex(payload, "limit", &limit_obj)) {
      limit = json_object_get_int(limit_obj);
      if (limit <= 0 || limit > WEBUI_MUSIC_MAX_QUEUE) {
         limit = 50;
      }
   }

   /* Route to Plex or local based on source config */
   bool use_plex = g_music_use_plex;

   if (use_plex) {
      if (!plex_client_is_configured()) {
         webui_music_send_error(conn, "UNAVAILABLE", "Plex not configured");
         return;
      }

      json_object *plex_result = plex_client_search(query, limit);
      if (!plex_result) {
         webui_music_send_error(conn, "PLEX_ERROR", "Plex search failed");
         return;
      }

      /* Wrap Plex response in standard music_search_response format */
      struct json_object *response = json_object_new_object();
      json_object_object_add(response, "type", json_object_new_string("music_search_response"));

      struct json_object *resp_payload = json_object_new_object();
      json_object_object_add(resp_payload, "query", json_object_new_string(query));
      json_object_object_add(resp_payload, "source", json_object_new_string("plex"));

      /* Transfer results array from Plex response */
      json_object *results_arr;
      if (json_object_object_get_ex(plex_result, "results", &results_arr)) {
         json_object_object_add(resp_payload, "results", json_object_get(results_arr));
         json_object_object_add(resp_payload, "count",
                                json_object_new_int((int)json_object_array_length(results_arr)));
      } else {
         json_object_object_add(resp_payload, "results", json_object_new_array());
         json_object_object_add(resp_payload, "count", json_object_new_int(0));
      }

      json_object_object_add(response, "payload", resp_payload);
      send_json_response(conn->wsi, response);
      json_object_put(response);
      json_object_put(plex_result);
      return;
   }

   /* Local source */
   if (!music_db_is_initialized()) {
      webui_music_send_error(conn, "UNAVAILABLE", "Music database not available");
      return;
   }

   /* Search database */
   music_search_result_t *results = malloc(limit * sizeof(music_search_result_t));
   if (!results) {
      webui_music_send_error(conn, "MEMORY_ERROR", "Failed to allocate search buffer");
      return;
   }

   int count = music_db_search(query, results, limit);

   /* Build response */
   struct json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("music_search_response"));

   struct json_object *resp_payload = json_object_new_object();
   json_object_object_add(resp_payload, "query", json_object_new_string(query));
   json_object_object_add(resp_payload, "count", json_object_new_int(count > 0 ? count : 0));
   json_object_object_add(resp_payload, "source", json_object_new_string("local"));

   struct json_object *results_arr = json_object_new_array();
   for (int i = 0; i < count; i++) {
      struct json_object *track = json_object_new_object();
      json_object_object_add(track, "path", json_object_new_string(results[i].path));
      json_object_object_add(track, "title", json_object_new_string(results[i].title));
      json_object_object_add(track, "artist", json_object_new_string(results[i].artist));
      json_object_object_add(track, "album", json_object_new_string(results[i].album));
      json_object_object_add(track, "display_name",
                             json_object_new_string(results[i].display_name));
      json_object_object_add(track, "duration_sec", json_object_new_int(results[i].duration_sec));
      json_object_array_add(results_arr, track);
   }
   json_object_object_add(resp_payload, "results", results_arr);

   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn->wsi, response);
   json_object_put(response);

   free(results);
}

/**
 * @brief Extract and clamp pagination parameters from JSON payload
 */
static void parse_pagination(struct json_object *payload, int *limit, int *offset) {
   *limit = 50;
   *offset = 0;

   struct json_object *limit_obj, *offset_obj;
   if (payload && json_object_object_get_ex(payload, "limit", &limit_obj))
      *limit = json_object_get_int(limit_obj);
   if (payload && json_object_object_get_ex(payload, "offset", &offset_obj))
      *offset = json_object_get_int(offset_obj);

   if (*limit < 1)
      *limit = 1;
   if (*limit > 200)
      *limit = 200;
   if (*offset < 0)
      *offset = 0;
}

void handle_music_library(ws_connection_t *conn, struct json_object *payload) {
   if (!conn_is_satellite_session(conn) && !conn_require_auth(conn)) {
      return;
   }

   const char *browse_type = "stats"; /* Default */

   struct json_object *type_obj;
   if (payload && json_object_object_get_ex(payload, "type", &type_obj)) {
      browse_type = json_object_get_string(type_obj);
   }

   /* Route to Plex for supported browse types */
   bool use_plex = g_music_use_plex;

   if (use_plex) {
      if (!plex_client_is_configured()) {
         webui_music_send_error(conn, "UNAVAILABLE", "Plex not configured");
         return;
      }

      int limit, offset;
      parse_pagination(payload, &limit, &offset);

      json_object *plex_result = NULL;

      if (strcmp(browse_type, "stats") == 0) {
         /* Query Plex for actual library counts */
         int artists = 0, albums = 0, tracks = 0;
         plex_client_get_stats(&artists, &albums, &tracks);

         struct json_object *response = json_object_new_object();
         json_object_object_add(response, "type", json_object_new_string("music_library_response"));
         struct json_object *resp_payload = json_object_new_object();
         json_object_object_add(resp_payload, "browse_type", json_object_new_string("stats"));
         json_object_object_add(resp_payload, "source", json_object_new_string("plex"));
         json_object_object_add(resp_payload, "track_count", json_object_new_int(tracks));
         json_object_object_add(resp_payload, "artist_count", json_object_new_int(artists));
         json_object_object_add(resp_payload, "album_count", json_object_new_int(albums));
         json_object_object_add(response, "payload", resp_payload);
         send_json_response(conn->wsi, response);
         json_object_put(response);
         return;
      } else if (strcmp(browse_type, "artists") == 0) {
         plex_result = plex_client_list_artists(offset, limit);
      } else if (strcmp(browse_type, "albums") == 0) {
         /* Check for artist_key to drill into a specific artist's albums */
         struct json_object *key_obj;
         const char *artist_key = NULL;
         if (payload && json_object_object_get_ex(payload, "artist_key", &key_obj)) {
            artist_key = json_object_get_string(key_obj);
            if (!validate_plex_key(artist_key)) {
               webui_music_send_error(conn, "INVALID_REQUEST", "Invalid Plex artist key");
               return;
            }
         }
         plex_result = plex_client_list_albums(artist_key, offset, limit);
      } else if (strcmp(browse_type, "tracks") == 0) {
         /* Check for album_key — if present, list album tracks; otherwise all tracks */
         struct json_object *key_obj;
         if (payload && json_object_object_get_ex(payload, "album_key", &key_obj)) {
            const char *key_str = json_object_get_string(key_obj);
            if (!validate_plex_key(key_str)) {
               webui_music_send_error(conn, "INVALID_REQUEST", "Invalid Plex album key");
               return;
            }
            plex_result = plex_client_list_tracks(key_str);
         } else {
            plex_result = plex_client_list_all_tracks(offset, limit);
         }
      } else if (strcmp(browse_type, "tracks_by_album") == 0) {
         /* Drill-down from album list — requires album_key */
         struct json_object *key_obj;
         if (payload && json_object_object_get_ex(payload, "album_key", &key_obj)) {
            const char *key_str = json_object_get_string(key_obj);
            if (!validate_plex_key(key_str)) {
               webui_music_send_error(conn, "INVALID_REQUEST", "Invalid Plex album key");
               return;
            }
            plex_result = plex_client_list_tracks(key_str);
            /* Override browse_type so JS renders as tracks_by_album */
            if (plex_result) {
               json_object_object_del(plex_result, "browse_type");
               json_object_object_add(plex_result, "browse_type",
                                      json_object_new_string("tracks_by_album"));
               /* Include album name for header display */
               struct json_object *album_obj;
               if (payload && json_object_object_get_ex(payload, "album", &album_obj)) {
                  json_object_object_add(plex_result, "album",
                                         json_object_new_string(json_object_get_string(album_obj)));
               }
            }
         } else {
            webui_music_send_error(conn, "INVALID_REQUEST",
                                   "Plex requires album_key for track listing");
            return;
         }
      } else if (strcmp(browse_type, "tracks_by_artist") == 0) {
         /* Drill-down from artist list — requires artist_key */
         struct json_object *key_obj;
         if (payload && json_object_object_get_ex(payload, "artist_key", &key_obj)) {
            const char *key_str = json_object_get_string(key_obj);
            if (!validate_plex_key(key_str)) {
               webui_music_send_error(conn, "INVALID_REQUEST", "Invalid Plex artist key");
               return;
            }
            plex_result = plex_client_list_artist_tracks(key_str);
         } else {
            webui_music_send_error(conn, "INVALID_REQUEST",
                                   "Plex requires artist_key for artist track listing");
            return;
         }
      }

      if (!plex_result) {
         webui_music_send_error(conn, "PLEX_ERROR", "Plex library request failed");
         return;
      }

      /* Wrap Plex response as music_library_response */
      struct json_object *response = json_object_new_object();
      json_object_object_add(response, "type", json_object_new_string("music_library_response"));

      /* The Plex client already returns properly formatted payload objects.
       * Add source indicator and wrap as response payload. */
      json_object_object_add(plex_result, "source", json_object_new_string("plex"));
      json_object_object_add(response, "payload", plex_result);
      send_json_response(conn->wsi, response);
      json_object_put(response);
      return;
   }

   /* Local source */
   if (!music_db_is_initialized()) {
      webui_music_send_error(conn, "UNAVAILABLE", "Music database not available");
      return;
   }

   struct json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("music_library_response"));

   struct json_object *resp_payload = json_object_new_object();
   json_object_object_add(resp_payload, "browse_type", json_object_new_string(browse_type));
   json_object_object_add(resp_payload, "source", json_object_new_string("local"));

   if (strcmp(browse_type, "stats") == 0) {
      music_db_stats_t stats;
      music_db_get_stats(&stats);

      json_object_object_add(resp_payload, "track_count", json_object_new_int(stats.track_count));
      json_object_object_add(resp_payload, "artist_count", json_object_new_int(stats.artist_count));
      json_object_object_add(resp_payload, "album_count", json_object_new_int(stats.album_count));

   } else if (strcmp(browse_type, "tracks") == 0) {
      /* Paginated track listing */
      int limit, offset;
      parse_pagination(payload, &limit, &offset);

      music_search_result_t *tracks = malloc(limit * sizeof(music_search_result_t));
      if (!tracks) {
         webui_music_send_error(conn, "MEMORY_ERROR", "Failed to allocate track list");
         json_object_put(response);
         return;
      }
      int count = music_db_list_paged(tracks, limit, offset);

      /* Include total count for pagination */
      music_db_stats_t stats;
      music_db_get_stats(&stats);

      struct json_object *tracks_arr = json_object_new_array();
      for (int i = 0; i < count; i++) {
         struct json_object *track = json_object_new_object();
         json_object_object_add(track, "path", json_object_new_string(tracks[i].path));
         json_object_object_add(track, "title", json_object_new_string(tracks[i].title));
         json_object_object_add(track, "artist", json_object_new_string(tracks[i].artist));
         json_object_object_add(track, "album", json_object_new_string(tracks[i].album));
         json_object_object_add(track, "duration_sec", json_object_new_int(tracks[i].duration_sec));
         json_object_array_add(tracks_arr, track);
      }
      json_object_object_add(resp_payload, "tracks", tracks_arr);
      json_object_object_add(resp_payload, "count", json_object_new_int(count));
      json_object_object_add(resp_payload, "total_count", json_object_new_int(stats.track_count));
      json_object_object_add(resp_payload, "offset", json_object_new_int(offset));
      free(tracks);

   } else if (strcmp(browse_type, "artists") == 0) {
      /* Paginated artist listing */
      int limit, offset;
      parse_pagination(payload, &limit, &offset);

      music_artist_info_t *artists = malloc(limit * sizeof(music_artist_info_t));
      if (!artists) {
         webui_music_send_error(conn, "MEMORY_ERROR", "Failed to allocate artist list");
         json_object_put(response);
         return;
      }
      int count = music_db_list_artists_with_stats(artists, limit, offset);

      /* Include total count for pagination */
      music_db_stats_t stats;
      music_db_get_stats(&stats);

      struct json_object *artists_arr = json_object_new_array();
      for (int i = 0; i < count; i++) {
         struct json_object *artist = json_object_new_object();
         json_object_object_add(artist, "name", json_object_new_string(artists[i].name));
         json_object_object_add(artist, "album_count", json_object_new_int(artists[i].album_count));
         json_object_object_add(artist, "track_count", json_object_new_int(artists[i].track_count));
         json_object_array_add(artists_arr, artist);
      }
      json_object_object_add(resp_payload, "artists", artists_arr);
      json_object_object_add(resp_payload, "count", json_object_new_int(count));
      json_object_object_add(resp_payload, "total_count", json_object_new_int(stats.artist_count));
      json_object_object_add(resp_payload, "offset", json_object_new_int(offset));
      free(artists);

   } else if (strcmp(browse_type, "albums") == 0) {
      /* Paginated album listing */
      int limit, offset;
      parse_pagination(payload, &limit, &offset);

      music_album_info_t *albums = malloc(limit * sizeof(music_album_info_t));
      if (!albums) {
         webui_music_send_error(conn, "MEMORY_ERROR", "Failed to allocate album list");
         json_object_put(response);
         return;
      }
      int count = music_db_list_albums_with_stats(albums, limit, offset);

      /* Include total count for pagination */
      music_db_stats_t stats;
      music_db_get_stats(&stats);

      struct json_object *albums_arr = json_object_new_array();
      for (int i = 0; i < count; i++) {
         struct json_object *album = json_object_new_object();
         json_object_object_add(album, "name", json_object_new_string(albums[i].name));
         json_object_object_add(album, "artist", json_object_new_string(albums[i].artist));
         json_object_object_add(album, "track_count", json_object_new_int(albums[i].track_count));
         json_object_array_add(albums_arr, album);
      }
      json_object_object_add(resp_payload, "albums", albums_arr);
      json_object_object_add(resp_payload, "count", json_object_new_int(count));
      json_object_object_add(resp_payload, "total_count", json_object_new_int(stats.album_count));
      json_object_object_add(resp_payload, "offset", json_object_new_int(offset));
      free(albums);

   } else if (strcmp(browse_type, "tracks_by_artist") == 0) {
      /* Get tracks for a specific artist */
      struct json_object *artist_obj;
      if (!json_object_object_get_ex(payload, "artist", &artist_obj)) {
         webui_music_send_error(conn, "INVALID_REQUEST", "Missing artist name");
         json_object_put(response);
         return;
      }
      const char *artist_name = json_object_get_string(artist_obj);

      music_search_result_t *tracks = malloc(100 * sizeof(music_search_result_t));
      if (!tracks) {
         webui_music_send_error(conn, "MEMORY_ERROR", "Failed to allocate track list");
         json_object_put(response);
         return;
      }
      int count = music_db_get_by_artist(artist_name, tracks, 100);

      struct json_object *tracks_arr = json_object_new_array();
      for (int i = 0; i < count; i++) {
         struct json_object *track = json_object_new_object();
         json_object_object_add(track, "path", json_object_new_string(tracks[i].path));
         json_object_object_add(track, "title", json_object_new_string(tracks[i].title));
         json_object_object_add(track, "artist", json_object_new_string(tracks[i].artist));
         json_object_object_add(track, "album", json_object_new_string(tracks[i].album));
         json_object_object_add(track, "duration_sec", json_object_new_int(tracks[i].duration_sec));
         json_object_array_add(tracks_arr, track);
      }
      json_object_object_add(resp_payload, "tracks", tracks_arr);
      json_object_object_add(resp_payload, "artist", json_object_new_string(artist_name));
      json_object_object_add(resp_payload, "count", json_object_new_int(count));
      free(tracks);

   } else if (strcmp(browse_type, "tracks_by_album") == 0) {
      /* Get tracks for a specific album */
      struct json_object *album_obj;
      if (!json_object_object_get_ex(payload, "album", &album_obj)) {
         webui_music_send_error(conn, "INVALID_REQUEST", "Missing album name");
         json_object_put(response);
         return;
      }
      const char *album_name = json_object_get_string(album_obj);

      music_search_result_t *tracks = malloc(50 * sizeof(music_search_result_t));
      if (!tracks) {
         webui_music_send_error(conn, "MEMORY_ERROR", "Failed to allocate track list");
         json_object_put(response);
         return;
      }
      int count = music_db_get_by_album(album_name, tracks, 50);

      struct json_object *tracks_arr = json_object_new_array();
      for (int i = 0; i < count; i++) {
         struct json_object *track = json_object_new_object();
         json_object_object_add(track, "path", json_object_new_string(tracks[i].path));
         json_object_object_add(track, "title", json_object_new_string(tracks[i].title));
         json_object_object_add(track, "artist", json_object_new_string(tracks[i].artist));
         json_object_object_add(track, "album", json_object_new_string(tracks[i].album));
         json_object_object_add(track, "duration_sec", json_object_new_int(tracks[i].duration_sec));
         json_object_array_add(tracks_arr, track);
      }
      json_object_object_add(resp_payload, "tracks", tracks_arr);
      json_object_object_add(resp_payload, "album", json_object_new_string(album_name));
      json_object_object_add(resp_payload, "count", json_object_new_int(count));
      free(tracks);
   }

   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn->wsi, response);
   json_object_put(response);
}

void handle_music_queue(ws_connection_t *conn, struct json_object *payload) {
   if (!conn_is_satellite_session(conn) && !conn_require_auth(conn)) {
      return;
   }

   session_music_state_t *state = (session_music_state_t *)conn->music_state;
   if (!state) {
      webui_music_send_error(conn, "NOT_INITIALIZED", "Music session not initialized");
      return;
   }

   struct json_object *action_obj;
   if (!json_object_object_get_ex(payload, "action", &action_obj)) {
      webui_music_send_error(conn, "INVALID_REQUEST", "Missing action");
      return;
   }

   const char *action = json_object_get_string(action_obj);

   if (strcmp(action, "list") == 0) {
      /* Return current queue */
      struct json_object *response = json_object_new_object();
      json_object_object_add(response, "type", json_object_new_string("music_queue_response"));

      struct json_object *resp_payload = json_object_new_object();

      pthread_mutex_lock(&state->state_mutex);

      struct json_object *queue_arr = json_object_new_array();
      for (int i = 0; i < state->queue_length; i++) {
         music_queue_entry_t *entry = &state->queue[i];
         struct json_object *track = json_object_new_object();
         json_object_object_add(track, "path", json_object_new_string(entry->path));
         json_object_object_add(track, "title", json_object_new_string(entry->title));
         json_object_object_add(track, "artist", json_object_new_string(entry->artist));
         json_object_object_add(track, "album", json_object_new_string(entry->album));
         json_object_object_add(track, "duration_sec", json_object_new_int(entry->duration_sec));
         json_object_array_add(queue_arr, track);
      }

      json_object_object_add(resp_payload, "queue", queue_arr);
      json_object_object_add(resp_payload, "current_index",
                             json_object_new_int(state->queue_index));
      json_object_object_add(resp_payload, "length", json_object_new_int(state->queue_length));

      pthread_mutex_unlock(&state->state_mutex);

      json_object_object_add(response, "payload", resp_payload);
      send_json_response(conn->wsi, response);
      json_object_put(response);

   } else if (strcmp(action, "clear") == 0) {
      /* Stop streaming thread before closing decoder to prevent use-after-free */
      webui_music_stop_streaming(state);

      pthread_mutex_lock(&state->state_mutex);
      state->queue_length = 0;
      state->queue_index = 0;
      state->playing = false;
      state->position_frames = 0;
      if (state->decoder) {
         audio_decoder_close(state->decoder);
         state->decoder = NULL;
      }
      webui_music_send_state(conn, state);
      pthread_mutex_unlock(&state->state_mutex);

   } else if (strcmp(action, "add") == 0) {
      struct json_object *path_obj;
      if (!json_object_object_get_ex(payload, "path", &path_obj)) {
         webui_music_send_error(conn, "INVALID_REQUEST", "Missing path");
         return;
      }

      const char *path = json_object_get_string(path_obj);

      /* Security: validate path is within music library */
      if (!webui_music_is_path_valid(path)) {
         webui_music_send_error(conn, "INVALID_PATH", "Path not in music library");
         return;
      }

      /* Get track metadata */
      music_search_result_t result;
      if (strncmp(path, "plex:", 5) == 0) {
         extract_plex_track_meta(payload, path, &result);
      } else if (music_db_get_by_path(path, &result) != 0) {
         webui_music_send_error(conn, "NOT_FOUND", "Track not found in database");
         return;
      }

      pthread_mutex_lock(&state->state_mutex);
      if (state->queue_length < WEBUI_MUSIC_MAX_QUEUE) {
         music_queue_entry_t *entry = &state->queue[state->queue_length];
         safe_strncpy(entry->path, result.path, sizeof(entry->path));
         safe_strncpy(entry->title, result.title, sizeof(entry->title));
         safe_strncpy(entry->artist, result.artist, sizeof(entry->artist));
         safe_strncpy(entry->album, result.album, sizeof(entry->album));
         entry->duration_sec = result.duration_sec;
         state->queue_length++;
      }
      webui_music_send_state(conn, state);
      pthread_mutex_unlock(&state->state_mutex);

   } else if (strcmp(action, "remove") == 0) {
      struct json_object *index_obj;
      if (!json_object_object_get_ex(payload, "index", &index_obj)) {
         webui_music_send_error(conn, "INVALID_REQUEST", "Missing index");
         return;
      }

      int index = json_object_get_int(index_obj);

      pthread_mutex_lock(&state->state_mutex);
      if (index >= 0 && index < state->queue_length) {
         /* Shift remaining entries */
         for (int i = index; i < state->queue_length - 1; i++) {
            state->queue[i] = state->queue[i + 1];
         }
         state->queue_length--;

         /* Adjust current index if needed */
         if (state->queue_index > index) {
            state->queue_index--;
         } else if (state->queue_index >= state->queue_length) {
            state->queue_index = state->queue_length > 0 ? state->queue_length - 1 : 0;
         }
      }
      webui_music_send_state(conn, state);
      pthread_mutex_unlock(&state->state_mutex);

   } else {
      webui_music_send_error(conn, "UNKNOWN_ACTION", "Unknown queue action");
   }
}

/* =============================================================================
 * LLM Tool Integration
 *
 * IMPORTANT: webui_music_execute_tool() runs on the LLM worker thread,
 * NOT the LWS service thread. All WebSocket sends in this function must
 * go through thread-safe paths (webui_music_send_state/send_error use
 * queue_response() internally). Never call send_json_response() or
 * lws_write() directly from here.
 * ============================================================================= */

int webui_music_execute_tool(ws_connection_t *conn,
                             const char *action,
                             const char *query,
                             char **result_out) {
   if (!conn || !action) {
      return 1;
   }

   /* Ensure music session is initialized */
   session_music_state_t *state = (session_music_state_t *)conn->music_state;
   if (!state) {
      if (webui_music_session_init(conn) != 0) {
         if (result_out) {
            *result_out = strdup("Failed to initialize music session");
         }
         return 1;
      }
      state = (session_music_state_t *)conn->music_state;
   }

   LOG_INFO("WebUI music tool: action='%s' query='%s'", action, query ? query : "(none)");

   if (strcmp(action, "play") == 0) {
      if (!query || query[0] == '\0') {
         /* Resume if paused */
         if (state->paused) {
            pthread_mutex_lock(&state->state_mutex);
            state->paused = false;
            webui_music_send_state(conn, state);
            pthread_mutex_unlock(&state->state_mutex);
            if (result_out) {
               *result_out = strdup("Resumed playback");
            }
            return 0;
         }
         if (result_out) {
            *result_out = strdup("Please specify what to search for");
         }
         return 1;
      }

      /* Search and play — route through Plex or local */
      bool use_plex = g_music_use_plex;

      /* Stop any current playback */
      webui_music_stop_streaming(state);
      pthread_mutex_lock(&state->state_mutex);
      if (state->decoder) {
         audio_decoder_close(state->decoder);
         state->decoder = NULL;
      }
      state->queue_length = 0;
      state->queue_index = 0;
      pthread_mutex_unlock(&state->state_mutex);

      if (use_plex) {
         if (!plex_client_is_configured()) {
            if (result_out)
               *result_out = strdup("Plex not configured");
            return 1;
         }
         json_object *plex_result = plex_client_search(query, WEBUI_MUSIC_MAX_QUEUE);
         if (!plex_result) {
            if (result_out)
               *result_out = strdup("Plex search failed");
            return 1;
         }
         json_object *results_arr;
         int count = 0;
         if (json_object_object_get_ex(plex_result, "results", &results_arr))
            count = (int)json_object_array_length(results_arr);
         if (count <= 0) {
            json_object_put(plex_result);
            if (result_out) {
               char buf[256];
               snprintf(buf, sizeof(buf), "No music found matching '%s'", query);
               *result_out = strdup(buf);
            }
            return 1;
         }
         pthread_mutex_lock(&state->state_mutex);
         for (int i = 0; i < count && state->queue_length < WEBUI_MUSIC_MAX_QUEUE; i++) {
            json_object *item = json_object_array_get_idx(results_arr, i);
            queue_entry_from_plex_json(&state->queue[state->queue_length], item);
            state->queue_length++;
         }
         pthread_mutex_unlock(&state->state_mutex);
         json_object_put(plex_result);

      } else {
         if (!music_db_is_initialized()) {
            if (result_out)
               *result_out = strdup("Music database not available");
            return 1;
         }
         music_search_result_t *results = malloc(WEBUI_MUSIC_MAX_QUEUE *
                                                 sizeof(music_search_result_t));
         if (!results) {
            if (result_out)
               *result_out = strdup("Memory allocation failed");
            return 1;
         }
         int count = music_db_search(query, results, WEBUI_MUSIC_MAX_QUEUE);
         if (count <= 0) {
            free(results);
            if (result_out) {
               char buf[256];
               snprintf(buf, sizeof(buf), "No music found matching '%s'", query);
               *result_out = strdup(buf);
            }
            return 1;
         }
         pthread_mutex_lock(&state->state_mutex);
         for (int i = 0; i < count && state->queue_length < WEBUI_MUSIC_MAX_QUEUE; i++) {
            music_queue_entry_t *entry = &state->queue[state->queue_length];
            safe_strncpy(entry->path, results[i].path, sizeof(entry->path));
            safe_strncpy(entry->title, results[i].title, sizeof(entry->title));
            safe_strncpy(entry->artist, results[i].artist, sizeof(entry->artist));
            safe_strncpy(entry->album, results[i].album, sizeof(entry->album));
            entry->duration_sec = results[i].duration_sec;
            state->queue_length++;
         }
         pthread_mutex_unlock(&state->state_mutex);
         free(results);
      }

      /* Start playback of first track */
      if (webui_music_start_playback(state, state->queue[0].path) != 0) {
         if (result_out) {
            *result_out = strdup("Failed to start playback");
         }
         return 1;
      }

      /* Send state update to client */
      pthread_mutex_lock(&state->state_mutex);
      webui_music_send_state(conn, state);
      pthread_mutex_unlock(&state->state_mutex);

      if (result_out) {
         char buf[512];
         snprintf(buf, sizeof(buf),
                  "Now playing: %s (track 1 of %d matching '%s') - streaming to WebUI",
                  state->queue[0].title[0] ? state->queue[0].title : "Unknown", state->queue_length,
                  query);
         *result_out = strdup(buf);
      }
      return 0;

   } else if (strcmp(action, "stop") == 0) {
      webui_music_stop_streaming(state);
      pthread_mutex_lock(&state->state_mutex);
      state->playing = false;
      state->paused = false;
      if (state->decoder) {
         audio_decoder_close(state->decoder);
         state->decoder = NULL;
      }
      state->position_frames = 0;
      webui_music_send_state(conn, state);
      pthread_mutex_unlock(&state->state_mutex);

      if (result_out) {
         *result_out = strdup("Music playback stopped");
      }
      return 0;

   } else if (strcmp(action, "pause") == 0) {
      pthread_mutex_lock(&state->state_mutex);
      state->paused = true;
      webui_music_send_state(conn, state);
      pthread_mutex_unlock(&state->state_mutex);

      if (result_out) {
         *result_out = strdup("Music paused");
      }
      return 0;

   } else if (strcmp(action, "resume") == 0) {
      pthread_mutex_lock(&state->state_mutex);
      state->paused = false;
      webui_music_send_state(conn, state);
      pthread_mutex_unlock(&state->state_mutex);

      if (result_out) {
         *result_out = strdup("Music resumed");
      }
      return 0;

   } else if (strcmp(action, "next") == 0) {
      pthread_mutex_lock(&state->state_mutex);
      bool can_advance = false;
      const char *next_title = NULL;
      if (state->queue_length > 0) {
         if (state->shuffle && state->queue_length > 1) {
            state->queue_index = webui_music_pick_random_index(state);
            can_advance = true;
         } else if (state->queue_index < state->queue_length - 1) {
            state->queue_index++;
            can_advance = true;
         } else if (state->repeat_mode == MUSIC_REPEAT_ALL) {
            state->queue_index = 0;
            can_advance = true;
         }
      }
      if (can_advance) {
         const char *next_path = state->queue[state->queue_index].path;
         next_title = state->queue[state->queue_index].title;
         pthread_mutex_unlock(&state->state_mutex);

         webui_music_start_playback(state, next_path);

         pthread_mutex_lock(&state->state_mutex);
         webui_music_send_state(conn, state);
         pthread_mutex_unlock(&state->state_mutex);

         if (result_out) {
            char buf[256];
            snprintf(buf, sizeof(buf), "Playing next: %s", next_title[0] ? next_title : "Unknown");
            *result_out = strdup(buf);
         }
      } else {
         pthread_mutex_unlock(&state->state_mutex);
         if (result_out) {
            *result_out = strdup("Already at end of queue");
         }
      }
      return 0;

   } else if (strcmp(action, "previous") == 0) {
      pthread_mutex_lock(&state->state_mutex);
      bool can_go_back = false;
      const char *prev_title = NULL;
      if (state->queue_length > 0) {
         if (state->shuffle && state->queue_length > 1) {
            state->queue_index = webui_music_pick_random_index(state);
            can_go_back = true;
         } else if (state->queue_index > 0) {
            state->queue_index--;
            can_go_back = true;
         } else if (state->repeat_mode == MUSIC_REPEAT_ALL) {
            state->queue_index = state->queue_length - 1;
            can_go_back = true;
         }
      }
      if (can_go_back) {
         const char *prev_path = state->queue[state->queue_index].path;
         prev_title = state->queue[state->queue_index].title;
         pthread_mutex_unlock(&state->state_mutex);

         webui_music_start_playback(state, prev_path);

         pthread_mutex_lock(&state->state_mutex);
         webui_music_send_state(conn, state);
         pthread_mutex_unlock(&state->state_mutex);

         if (result_out) {
            char buf[256];
            snprintf(buf, sizeof(buf), "Playing previous: %s",
                     prev_title[0] ? prev_title : "Unknown");
            *result_out = strdup(buf);
         }
      } else {
         pthread_mutex_unlock(&state->state_mutex);
         if (result_out) {
            *result_out = strdup("Already at start of queue");
         }
      }
      return 0;

   } else if (strcmp(action, "shuffle") == 0) {
      pthread_mutex_lock(&state->state_mutex);
      state->shuffle = !state->shuffle;
      bool shuffle_on = state->shuffle;
      webui_music_send_state(conn, state);
      pthread_mutex_unlock(&state->state_mutex);

      if (result_out) {
         *result_out = strdup(shuffle_on ? "Shuffle enabled" : "Shuffle disabled");
      }
      return 0;

   } else if (strcmp(action, "repeat") == 0) {
      pthread_mutex_lock(&state->state_mutex);
      state->repeat_mode = (music_repeat_mode_t)((state->repeat_mode + 1) % 3);
      const char *modes[] = { "Repeat off", "Repeat all", "Repeat one" };
      const char *mode_str = modes[state->repeat_mode];
      webui_music_send_state(conn, state);
      pthread_mutex_unlock(&state->state_mutex);

      if (result_out) {
         *result_out = strdup(mode_str);
      }
      return 0;

   } else if (strcmp(action, "list") == 0) {
      pthread_mutex_lock(&state->state_mutex);
      if (state->queue_length == 0) {
         pthread_mutex_unlock(&state->state_mutex);
         if (result_out) {
            *result_out = strdup("No music in queue");
         }
         return 0;
      }

      /* Build queue list */
      size_t buf_size = 1024 + (state->queue_length * 128);
      char *buf = malloc(buf_size);
      if (buf) {
         int offset = snprintf(buf, buf_size, "Queue (%d tracks, currently #%d):\n",
                               state->queue_length, state->queue_index + 1);
         for (int i = 0; i < state->queue_length && offset < (int)buf_size - 128; i++) {
            const char *marker = (i == state->queue_index) ? "▶ " : "  ";
            offset += snprintf(buf + offset, buf_size - offset, "%s%d. %s - %s\n", marker, i + 1,
                               state->queue[i].artist[0] ? state->queue[i].artist : "Unknown",
                               state->queue[i].title[0] ? state->queue[i].title : "Unknown");
         }
         if (result_out) {
            *result_out = buf;
         } else {
            free(buf);
         }
      }
      pthread_mutex_unlock(&state->state_mutex);
      return 0;

   } else if (strcmp(action, "search") == 0 || strcmp(action, "library") == 0) {
      /* These don't change playback state, just return info */
      /* For now, let music_tool handle these since they don't affect streaming */
      if (result_out) {
         *result_out = NULL; /* Signal to fall through to regular handler */
      }
      return -1; /* -1 means "not handled, use default" */
   }

   if (result_out) {
      char buf[128];
      snprintf(buf, sizeof(buf), "Unknown action: %s", action);
      *result_out = strdup(buf);
   }
   return 1;
}

/* =============================================================================
 * Volume Tool Integration (called from volume_tool.c via session routing)
 * ============================================================================= */

char *webui_volume_execute_tool(ws_connection_t *conn,
                                const char *action,
                                const char *value,
                                int *should_respond) {
   *should_respond = 1;

   if (strcmp(action, "get") == 0) {
      char *result = malloc(64);
      if (result)
         snprintf(result, 64, "Volume is at %.0f%%", conn->volume * 100.0f);
      return result;
   }

   /* Action: set */
   if (!value || !*value) {
      return strdup("Error: 'level' parameter is required for 'set' action.");
   }

   float vol = parse_volume_level(value);
   if (vol < 0.0f) {
      char *result = malloc(128);
      if (result)
         snprintf(result, 128, "Invalid volume level '%s'. Use a number 0-100.", value);
      return result;
   }

   conn->volume = vol;
   LOG_INFO("WebUI volume tool: set to %.0f%% for session", vol * 100.0f);

   /* Send state update to client so slider syncs */
   session_music_state_t *state = (session_music_state_t *)conn->music_state;
   if (state) {
      pthread_mutex_lock(&state->state_mutex);
      webui_music_send_state(conn, state);
      pthread_mutex_unlock(&state->state_mutex);
   }

   char *result = malloc(64);
   if (result)
      snprintf(result, 64, "Volume set to %.0f%%", vol * 100.0f);
   return result;
}
