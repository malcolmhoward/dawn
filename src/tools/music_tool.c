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
 * Music Tool - Audio playback with playlist management
 *
 * Supports actions: play, stop, next, previous
 * Searches music directory recursively for matching files.
 */

#define _GNU_SOURCE
#include "tools/music_tool.h"

#include <dirent.h>
#include <fnmatch.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio/audio_decoder.h"
#include "audio/flac_playback.h"
#include "config/dawn_config.h"
#include "dawn.h"
#include "logging.h"
#include "mosquitto_comms.h"
#include "tools/tool_registry.h"

/* ========== Constants ========== */

#define MAX_FILENAME_LENGTH 1024
#define MAX_PLAYLIST_LENGTH 100
#define MUSIC_CALLBACK_BUFFER_SIZE 512

/* ========== Types ========== */

/**
 * @struct Playlist
 * @brief Structure to hold the list of matching filenames
 */
typedef struct {
   char filenames[MAX_PLAYLIST_LENGTH][MAX_FILENAME_LENGTH];
   int count;
} Playlist;

/* ========== Static State ========== */

static Playlist s_playlist = { .count = 0 };
static int s_current_track = 0;
static pthread_t s_music_thread = -1;
static char *s_custom_music_dir = NULL;

/* ========== Forward Declarations ========== */

static char *music_tool_callback(const char *action, char *value, int *should_respond);
static void music_tool_cleanup(void);

/* ========== Tool Parameter Definition ========== */

static const treg_param_t music_params[] = {
   {
       .name = "action",
       .description = "The playback action: 'play' (start/search), 'stop' (halt playback), "
                      "'next' (skip forward), 'previous' (skip backward)",
       .type = TOOL_PARAM_TYPE_ENUM,
       .required = true,
       .maps_to = TOOL_MAPS_TO_ACTION,
       .enum_values = { "play", "stop", "next", "previous" },
       .enum_count = 4,
   },
   {
       .name = "query",
       .description = "Search terms for finding music (used with 'play' action). "
                      "Matches artist, album, or track name.",
       .type = TOOL_PARAM_TYPE_STRING,
       .required = false,
       .maps_to = TOOL_MAPS_TO_VALUE,
   },
};

/* ========== Tool Metadata ========== */

static const tool_metadata_t music_metadata = {
   .name = "music",
   .device_string = "music",
   .topic = "dawn",
   .aliases = { "audio", "player" },
   .alias_count = 2,

   .description = "Control music playback. Use 'play' with a search query to find and play "
                  "matching tracks. Use 'stop' to halt playback, 'next' for next track, "
                  "'previous' for previous track.",
   .params = music_params,
   .param_count = 2,

   .device_type = TOOL_DEVICE_TYPE_MUSIC,
   .capabilities = TOOL_CAP_FILESYSTEM,
   .is_getter = false,
   .skip_followup = false,
   .default_remote = true,

   .config = NULL,
   .config_size = 0,
   .config_parser = NULL,
   .config_section = NULL,

   .init = NULL,
   .cleanup = music_tool_cleanup,
   .callback = music_tool_callback,
};

/* ========== Helper Functions ========== */

/**
 * @brief Retrieves the current user's home directory
 */
static const char *get_user_home_directory(void) {
   const char *home_dir = getenv("HOME");
   if (!home_dir) {
      LOG_ERROR("Error: HOME environment variable not set.");
      return NULL;
   }
   return home_dir;
}

/**
 * @brief Resolves a path that may be absolute, tilde-prefixed, or relative to home
 *
 * @param path The path to resolve
 * @return Dynamically allocated resolved path (caller must free), or NULL on error
 */
static char *construct_path_with_subdirectory(const char *path) {
   if (!path || !*path) {
      LOG_ERROR("Error: path is NULL or empty.");
      return NULL;
   }

   /* Absolute path - return as-is */
   if (path[0] == '/') {
      return strdup(path);
   }

   const char *home_dir = get_user_home_directory();
   if (!home_dir) {
      return NULL;
   }

   char *full_path = NULL;

   /* Tilde expansion: ~/path -> /home/user/path */
   if (path[0] == '~' && (path[1] == '/' || path[1] == '\0')) {
      const char *suffix = (path[1] == '/') ? path + 2 : "";
      size_t full_path_size = strlen(home_dir) + 1 + strlen(suffix) + 1;
      full_path = malloc(full_path_size);
      if (!full_path) {
         LOG_ERROR("Error: Memory allocation failed for full path.");
         return NULL;
      }
      if (*suffix) {
         snprintf(full_path, full_path_size, "%s/%s", home_dir, suffix);
      } else {
         snprintf(full_path, full_path_size, "%s", home_dir);
      }
   } else {
      /* Relative path - prepend home directory */
      size_t full_path_size = strlen(home_dir) + 1 + strlen(path) + 1;
      full_path = malloc(full_path_size);
      if (!full_path) {
         LOG_ERROR("Error: Memory allocation failed for full path.");
         return NULL;
      }
      snprintf(full_path, full_path_size, "%s/%s", home_dir, path);
   }

   return full_path;
}

/**
 * @brief Escape glob metacharacters in a string for safe use with fnmatch()
 */
static int escape_glob_chars(const char *src, char *dst, size_t dst_size) {
   if (!src || !dst || dst_size == 0) {
      return -1;
   }

   size_t j = 0;
   for (size_t i = 0; src[i] != '\0'; i++) {
      bool needs_escape = (src[i] == '*' || src[i] == '?' || src[i] == '[' || src[i] == ']' ||
                           src[i] == '\\');

      if (needs_escape) {
         if (j + 2 >= dst_size) {
            dst[j] = '\0';
            return -1;
         }
         dst[j++] = '\\';
      } else {
         if (j + 1 >= dst_size) {
            dst[j] = '\0';
            return -1;
         }
      }
      dst[j++] = src[i];
   }
   dst[j] = '\0';
   return (int)j;
}

/**
 * @brief Check if a filename matches a base pattern and any of the given extensions
 */
static bool matches_pattern_with_extensions(const char *filename,
                                            const char *base_pattern,
                                            const char **extensions) {
   if (!filename || !base_pattern || !extensions) {
      return false;
   }

   char full_pattern[MAX_FILENAME_LENGTH];
   for (int i = 0; extensions[i] != NULL; i++) {
      int written = snprintf(full_pattern, sizeof(full_pattern), "%s%s", base_pattern,
                             extensions[i]);
      if (written >= (int)sizeof(full_pattern)) {
         continue;
      }
      if (fnmatch(full_pattern, filename, FNM_CASEFOLD) == 0) {
         return true;
      }
   }
   return false;
}

/**
 * @brief Recursively searches a directory for files matching a pattern with multiple extensions
 */
static void search_directory_multi_ext(const char *root_dir,
                                       const char *base_pattern,
                                       const char **extensions,
                                       Playlist *playlist) {
   DIR *dir = opendir(root_dir);
   if (!dir) {
      LOG_ERROR("Error opening directory: %s", root_dir);
      return;
   }

   struct dirent *entry;
   while ((entry = readdir(dir)) != NULL) {
      if (playlist->count >= MAX_PLAYLIST_LENGTH) {
         LOG_WARNING("Playlist is full.");
         closedir(dir);
         return;
      }

      if (entry->d_type == DT_REG) {
         if (matches_pattern_with_extensions(entry->d_name, base_pattern, extensions)) {
            char file_path[MAX_FILENAME_LENGTH];
            int written = snprintf(file_path, sizeof(file_path), "%s/%s", root_dir, entry->d_name);
            if (written < (int)sizeof(file_path)) {
               strncpy(playlist->filenames[playlist->count], file_path, MAX_FILENAME_LENGTH - 1);
               playlist->filenames[playlist->count][MAX_FILENAME_LENGTH - 1] = '\0';
               playlist->count++;
            }
         }
      } else if (entry->d_type == DT_DIR) {
         if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0) {
            char sub_path[MAX_FILENAME_LENGTH];
            int written = snprintf(sub_path, sizeof(sub_path), "%s/%s", root_dir, entry->d_name);
            if (written < (int)sizeof(sub_path)) {
               search_directory_multi_ext(sub_path, base_pattern, extensions, playlist);
            }
         }
      }
   }

   closedir(dir);
}

/**
 * @brief Comparison function for qsort
 */
static int compare_filenames(const void *p1, const void *p2) {
   return strcmp((const char *)p1, (const char *)p2);
}

/**
 * @brief Extract just the filename from a full path
 */
static const char *extract_filename(const char *path) {
   const char *filename = strrchr(path, '/');
   return filename ? filename + 1 : path;
}

/**
 * @brief Stop current playback and wait for thread to finish
 */
static void stop_current_playback(void) {
   if (s_music_thread != (pthread_t)-1) {
      setMusicPlay(0);
      int join_result = pthread_join(s_music_thread, NULL);
      if (join_result == 0) {
         s_music_thread = -1;
      }
   }
}

/**
 * @brief Start playback of current track
 * @return Allocated result string, or NULL
 */
static char *start_playback(bool report_result) {
   PlaybackArgs *args = malloc(sizeof(PlaybackArgs));
   if (!args) {
      LOG_ERROR("Failed to allocate PlaybackArgs");
      return report_result ? strdup("Failed to start music playback") : NULL;
   }

   args->sink_name = getPcmPlaybackDevice();
   args->file_name = s_playlist.filenames[s_current_track];
   args->start_time = 0;

   LOG_INFO("Playing: %s %s %d", args->sink_name, args->file_name, args->start_time);

   if (pthread_create(&s_music_thread, NULL, playFlacAudio, args)) {
      LOG_ERROR("Error creating music thread");
      free(args);
      return report_result ? strdup("Failed to start music playback") : NULL;
   }

   if (!report_result) {
      return NULL;
   }

   const char *filename = extract_filename(s_playlist.filenames[s_current_track]);
   char *result = malloc(MUSIC_CALLBACK_BUFFER_SIZE);
   if (result) {
      snprintf(result, MUSIC_CALLBACK_BUFFER_SIZE, "Playing: %s", filename);
   }
   return result;
}

/* ========== Callback Implementation ========== */

static char *music_tool_callback(const char *action, char *value, int *should_respond) {
   char *result = NULL;

   *should_respond = 1;

   /* Get command processing mode from global */
   bool direct_mode = (command_processing_mode == CMD_MODE_DIRECT_ONLY);

   if (strcmp(action, "play") == 0) {
      /* Stop any current playback */
      stop_current_playback();

      /* Reset playlist */
      s_playlist.count = 0;
      s_current_track = 0;

      /* Validate search term length */
      if ((strlen(value) + 8) > MAX_FILENAME_LENGTH) {
         LOG_ERROR("\"%s\" is too long to search for.", value);
         if (direct_mode) {
            *should_respond = 0;
            return NULL;
         }
         result = malloc(MUSIC_CALLBACK_BUFFER_SIZE);
         if (result) {
            snprintf(result, MUSIC_CALLBACK_BUFFER_SIZE, "Search term '%s' is too long", value);
         }
         return result;
      }

      /* Determine music directory */
      char *music_dir = NULL;
      if (s_custom_music_dir) {
         music_dir = strdup(s_custom_music_dir);
      } else {
         music_dir = construct_path_with_subdirectory(g_config.paths.music_dir);
      }

      if (!music_dir) {
         LOG_ERROR("Error constructing music path.");
         if (direct_mode) {
            *should_respond = 0;
            return NULL;
         }
         return strdup("Failed to access music directory");
      }

      /* Escape glob characters in search value */
      char escaped_value[MAX_FILENAME_LENGTH];
      if (escape_glob_chars(value, escaped_value, sizeof(escaped_value)) < 0) {
         LOG_WARNING("Search term too long after escaping, using original");
         strncpy(escaped_value, value, sizeof(escaped_value) - 1);
         escaped_value[sizeof(escaped_value) - 1] = '\0';
      }

      /* Build wildcard pattern: *term1*term2* */
      char wildcards[MAX_FILENAME_LENGTH];
      wildcards[0] = '*';
      int j = 1;
      for (int i = 0; escaped_value[i] != '\0' && j < MAX_FILENAME_LENGTH - 2; i++) {
         wildcards[j++] = (escaped_value[i] == ' ') ? '*' : escaped_value[i];
      }
      wildcards[j++] = '*';
      wildcards[j] = '\0';

      /* Search for matching files */
      const char **extensions = audio_decoder_get_extensions();
      search_directory_multi_ext(music_dir, wildcards, extensions, &s_playlist);
      free(music_dir);

      /* Sort results */
      qsort(s_playlist.filenames, s_playlist.count, MAX_FILENAME_LENGTH, compare_filenames);

      LOG_INFO("New playlist (%d tracks):", s_playlist.count);
      for (int i = 0; i < s_playlist.count; i++) {
         LOG_INFO("\t%s", s_playlist.filenames[i]);
      }

      if (s_playlist.count > 0) {
         if (direct_mode) {
            *should_respond = 0;
            start_playback(false);
            return NULL;
         }

         start_playback(false);
         const char *filename = extract_filename(s_playlist.filenames[s_current_track]);
         result = malloc(MUSIC_CALLBACK_BUFFER_SIZE);
         if (result) {
            snprintf(result, MUSIC_CALLBACK_BUFFER_SIZE,
                     "Now playing: %s (track 1 of %d matching '%s')", filename, s_playlist.count,
                     value);
         }
         return result;
      } else {
         LOG_WARNING("No music matching that description was found.");
         if (direct_mode) {
            *should_respond = 0;
            return NULL;
         }
         result = malloc(MUSIC_CALLBACK_BUFFER_SIZE);
         if (result) {
            snprintf(result, MUSIC_CALLBACK_BUFFER_SIZE, "No music found matching '%s'", value);
         }
         return result;
      }

   } else if (strcmp(action, "stop") == 0) {
      LOG_INFO("Stopping music playback.");
      stop_current_playback();

      if (direct_mode) {
         *should_respond = 0;
         return NULL;
      }
      return strdup("Music playback stopped");

   } else if (strcmp(action, "next") == 0) {
      stop_current_playback();

      if (s_playlist.count > 0) {
         s_current_track++;
         if (s_current_track >= s_playlist.count) {
            s_current_track = 0;
         }

         if (direct_mode) {
            *should_respond = 0;
            start_playback(false);
            return NULL;
         }

         start_playback(false);
         const char *filename = extract_filename(s_playlist.filenames[s_current_track]);
         result = malloc(MUSIC_CALLBACK_BUFFER_SIZE);
         if (result) {
            snprintf(result, MUSIC_CALLBACK_BUFFER_SIZE, "Playing next track: %s", filename);
         }
         return result;
      } else {
         if (direct_mode) {
            *should_respond = 0;
            return NULL;
         }
         return strdup("No playlist available");
      }

   } else if (strcmp(action, "previous") == 0) {
      stop_current_playback();

      if (s_playlist.count > 0) {
         s_current_track--;
         if (s_current_track < 0) {
            s_current_track = s_playlist.count - 1;
         }

         if (direct_mode) {
            *should_respond = 0;
            start_playback(false);
            return NULL;
         }

         start_playback(false);
         const char *filename = extract_filename(s_playlist.filenames[s_current_track]);
         result = malloc(MUSIC_CALLBACK_BUFFER_SIZE);
         if (result) {
            snprintf(result, MUSIC_CALLBACK_BUFFER_SIZE, "Playing previous track: %s", filename);
         }
         return result;
      } else {
         if (direct_mode) {
            *should_respond = 0;
            return NULL;
         }
         return strdup("No playlist available");
      }
   }

   return NULL;
}

/* ========== Lifecycle Functions ========== */

static void music_tool_cleanup(void) {
   stop_current_playback();

   if (s_custom_music_dir) {
      free(s_custom_music_dir);
      s_custom_music_dir = NULL;
   }

   s_playlist.count = 0;
   s_current_track = 0;
}

/* ========== Public API ========== */

void set_music_directory(const char *path) {
   if (s_custom_music_dir) {
      free(s_custom_music_dir);
      s_custom_music_dir = NULL;
   }

   if (path) {
      s_custom_music_dir = strdup(path);
      if (!s_custom_music_dir) {
         LOG_ERROR("Failed to allocate memory for custom music directory");
      } else {
         LOG_INFO("Music directory set to: %s", s_custom_music_dir);
      }
   }
}

int music_tool_register(void) {
   return tool_registry_register(&music_metadata);
}
