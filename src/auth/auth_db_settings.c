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
 * Authentication Database - User Settings Module
 *
 * Handles per-user settings storage and retrieval:
 * - Persona description and mode (append/replace)
 * - Location and timezone
 * - Units preference (metric/imperial)
 * - TTS voice and speed settings
 * - UI theme
 */

#define AUTH_DB_INTERNAL_ALLOWED
#include <string.h>
#include <time.h>

#include "auth/auth_db_internal.h"
#include "logging.h"

/* =============================================================================
 * User Settings Operations
 * ============================================================================= */

int auth_db_get_user_settings(int user_id, auth_user_settings_t *settings_out) {
   if (!settings_out) {
      return AUTH_DB_INVALID;
   }

   /* Initialize with defaults */
   memset(settings_out, 0, sizeof(*settings_out));
   settings_out->tts_length_scale = 1.0f;
   strncpy(settings_out->persona_mode, "append", AUTH_PERSONA_MODE_MAX - 1);
   strncpy(settings_out->timezone, "UTC", AUTH_TIMEZONE_MAX - 1);
   strncpy(settings_out->units, "metric", AUTH_UNITS_MAX - 1);
   strncpy(settings_out->theme, "cyan", AUTH_THEME_MAX - 1);

   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_reset(s_db.stmt_get_user_settings);
   sqlite3_bind_int(s_db.stmt_get_user_settings, 1, user_id);

   int rc = sqlite3_step(s_db.stmt_get_user_settings);

   if (rc == SQLITE_ROW) {
      /* Copy non-null values from database */
      const char *persona = (const char *)sqlite3_column_text(s_db.stmt_get_user_settings, 0);
      const char *persona_mode = (const char *)sqlite3_column_text(s_db.stmt_get_user_settings, 1);
      const char *location = (const char *)sqlite3_column_text(s_db.stmt_get_user_settings, 2);
      const char *timezone = (const char *)sqlite3_column_text(s_db.stmt_get_user_settings, 3);
      const char *units = (const char *)sqlite3_column_text(s_db.stmt_get_user_settings, 4);
      const char *voice = (const char *)sqlite3_column_text(s_db.stmt_get_user_settings, 5);

      if (persona) {
         strncpy(settings_out->persona_description, persona, AUTH_PERSONA_DESC_MAX - 1);
      }
      if (persona_mode) {
         strncpy(settings_out->persona_mode, persona_mode, AUTH_PERSONA_MODE_MAX - 1);
      }
      if (location) {
         strncpy(settings_out->location, location, AUTH_LOCATION_MAX - 1);
      }
      if (timezone) {
         strncpy(settings_out->timezone, timezone, AUTH_TIMEZONE_MAX - 1);
      }
      if (units) {
         strncpy(settings_out->units, units, AUTH_UNITS_MAX - 1);
      }
      if (voice) {
         strncpy(settings_out->tts_voice_model, voice, AUTH_TTS_VOICE_MAX - 1);
      }
      settings_out->tts_length_scale = (float)sqlite3_column_double(s_db.stmt_get_user_settings, 6);

      const char *theme = (const char *)sqlite3_column_text(s_db.stmt_get_user_settings, 7);
      if (theme) {
         strncpy(settings_out->theme, theme, AUTH_THEME_MAX - 1);
      }
   } else if (rc != SQLITE_DONE) {
      /* Unexpected error */
      LOG_ERROR("auth_db: get_user_settings failed: %s", sqlite3_errmsg(s_db.db));
      sqlite3_reset(s_db.stmt_get_user_settings);
      AUTH_DB_UNLOCK();
      return AUTH_DB_FAILURE;
   }
   /* SQLITE_DONE means no row found - return defaults */

   sqlite3_reset(s_db.stmt_get_user_settings);
   AUTH_DB_UNLOCK();
   return AUTH_DB_SUCCESS;
}

int auth_db_set_user_settings(int user_id, const auth_user_settings_t *settings) {
   if (!settings) {
      return AUTH_DB_INVALID;
   }

   /* Clamp tts_length_scale to valid bounds */
   float length_scale = settings->tts_length_scale;
   if (length_scale < AUTH_TTS_LENGTH_SCALE_MIN) {
      length_scale = AUTH_TTS_LENGTH_SCALE_MIN;
   } else if (length_scale > AUTH_TTS_LENGTH_SCALE_MAX) {
      length_scale = AUTH_TTS_LENGTH_SCALE_MAX;
   }

   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_reset(s_db.stmt_set_user_settings);
   sqlite3_bind_int(s_db.stmt_set_user_settings, 1, user_id);
   sqlite3_bind_text(s_db.stmt_set_user_settings, 2, settings->persona_description, -1,
                     SQLITE_STATIC);
   sqlite3_bind_text(s_db.stmt_set_user_settings, 3, settings->persona_mode, -1, SQLITE_STATIC);
   sqlite3_bind_text(s_db.stmt_set_user_settings, 4, settings->location, -1, SQLITE_STATIC);
   sqlite3_bind_text(s_db.stmt_set_user_settings, 5, settings->timezone, -1, SQLITE_STATIC);
   sqlite3_bind_text(s_db.stmt_set_user_settings, 6, settings->units, -1, SQLITE_STATIC);
   sqlite3_bind_text(s_db.stmt_set_user_settings, 7, settings->tts_voice_model, -1, SQLITE_STATIC);
   sqlite3_bind_double(s_db.stmt_set_user_settings, 8, (double)length_scale);
   sqlite3_bind_text(s_db.stmt_set_user_settings, 9, settings->theme, -1, SQLITE_STATIC);
   sqlite3_bind_int64(s_db.stmt_set_user_settings, 10, (int64_t)time(NULL));

   int rc = sqlite3_step(s_db.stmt_set_user_settings);
   sqlite3_reset(s_db.stmt_set_user_settings);

   AUTH_DB_UNLOCK();

   if (rc != SQLITE_DONE) {
      LOG_ERROR("auth_db: set_user_settings failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   return AUTH_DB_SUCCESS;
}

int auth_db_init_user_settings(int user_id) {
   auth_user_settings_t defaults;
   memset(&defaults, 0, sizeof(defaults));
   defaults.tts_length_scale = 1.0f;
   strncpy(defaults.timezone, "UTC", AUTH_TIMEZONE_MAX - 1);
   strncpy(defaults.units, "metric", AUTH_UNITS_MAX - 1);
   return auth_db_set_user_settings(user_id, &defaults);
}
