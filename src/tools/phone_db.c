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
 * Phone Database — SQLite CRUD for call and SMS logs.
 * Uses the shared auth_db handle (Pattern A, same as calendar_db.c).
 */

#define AUTH_DB_INTERNAL_ALLOWED
#include "tools/phone_db.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "auth/auth_db_internal.h"
#include "logging.h"

/* =============================================================================
 * Helper: safe string copy from SQLite column
 * ============================================================================= */

static void col_str(char *dst, size_t dst_size, sqlite3_stmt *stmt, int col) {
   const char *src = (const char *)sqlite3_column_text(stmt, col);
   if (src) {
      size_t len = (size_t)sqlite3_column_bytes(stmt, col);
      if (len >= dst_size)
         len = dst_size - 1;
      memcpy(dst, src, len);
      dst[len] = '\0';
   } else {
      dst[0] = '\0';
   }
}

/* =============================================================================
 * Row Mappers
 * ============================================================================= */

static void row_to_call_log(sqlite3_stmt *stmt, phone_call_log_t *entry) {
   entry->id = sqlite3_column_int64(stmt, 0);
   entry->user_id = sqlite3_column_int(stmt, 1);
   entry->direction = sqlite3_column_int(stmt, 2);
   col_str(entry->number, sizeof(entry->number), stmt, 3);
   col_str(entry->contact_name, sizeof(entry->contact_name), stmt, 4);
   entry->duration_sec = sqlite3_column_int(stmt, 5);
   entry->timestamp = (time_t)sqlite3_column_int64(stmt, 6);
   entry->status = sqlite3_column_int(stmt, 7);
}

static void row_to_sms_log(sqlite3_stmt *stmt, phone_sms_log_t *entry) {
   entry->id = sqlite3_column_int64(stmt, 0);
   entry->user_id = sqlite3_column_int(stmt, 1);
   entry->direction = sqlite3_column_int(stmt, 2);
   col_str(entry->number, sizeof(entry->number), stmt, 3);
   col_str(entry->contact_name, sizeof(entry->contact_name), stmt, 4);
   col_str(entry->body, sizeof(entry->body), stmt, 5);
   entry->timestamp = (time_t)sqlite3_column_int64(stmt, 6);
   entry->read = sqlite3_column_int(stmt, 7);
}

/* =============================================================================
 * Call Log
 * ============================================================================= */

int64_t phone_db_call_log_insert(int user_id,
                                 int direction,
                                 const char *number,
                                 const char *contact_name,
                                 int duration_sec,
                                 time_t timestamp,
                                 int status) {
   if (!s_db.initialized) {
      return -1;
   }

   pthread_mutex_lock(&s_db.mutex);

   const char *sql = "INSERT INTO phone_call_log "
                     "(user_id, direction, number, contact_name, duration_sec, timestamp, status) "
                     "VALUES (?, ?, ?, ?, ?, ?, ?)";

   sqlite3_stmt *stmt;
   int rc = sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("phone_db: call_log insert prepare failed: %s", sqlite3_errmsg(s_db.db));
      pthread_mutex_unlock(&s_db.mutex);
      return -1;
   }

   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_int(stmt, 2, direction);
   sqlite3_bind_text(stmt, 3, number ? number : "", -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 4, contact_name ? contact_name : "", -1, SQLITE_TRANSIENT);
   sqlite3_bind_int(stmt, 5, duration_sec);
   sqlite3_bind_int64(stmt, 6, (int64_t)timestamp);
   sqlite3_bind_int(stmt, 7, status);

   rc = sqlite3_step(stmt);
   int64_t row_id = -1;
   if (rc == SQLITE_DONE) {
      row_id = sqlite3_last_insert_rowid(s_db.db);
   } else {
      OLOG_ERROR("phone_db: call_log insert failed: %s", sqlite3_errmsg(s_db.db));
   }

   sqlite3_finalize(stmt);
   pthread_mutex_unlock(&s_db.mutex);
   return row_id;
}

int phone_db_call_log_update(int64_t id, int duration_sec, int status) {
   if (!s_db.initialized) {
      return 1;
   }

   pthread_mutex_lock(&s_db.mutex);

   const char *sql = "UPDATE phone_call_log SET duration_sec = ?, status = ? WHERE id = ?";

   sqlite3_stmt *stmt;
   int rc = sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("phone_db: call_log update prepare failed: %s", sqlite3_errmsg(s_db.db));
      pthread_mutex_unlock(&s_db.mutex);
      return 1;
   }

   sqlite3_bind_int(stmt, 1, duration_sec);
   sqlite3_bind_int(stmt, 2, status);
   sqlite3_bind_int64(stmt, 3, id);

   rc = sqlite3_step(stmt);
   int result = (rc == SQLITE_DONE) ? 0 : 1;
   if (rc != SQLITE_DONE) {
      OLOG_ERROR("phone_db: call_log update failed: %s", sqlite3_errmsg(s_db.db));
   }

   sqlite3_finalize(stmt);
   pthread_mutex_unlock(&s_db.mutex);
   return result;
}

int phone_db_call_log_recent(int user_id, phone_call_log_t *out, int max) {
   if (!s_db.initialized || !out || max <= 0) {
      return -1;
   }

   pthread_mutex_lock(&s_db.mutex);

   const char *sql = "SELECT id, user_id, direction, number, contact_name, duration_sec, "
                     "timestamp, status FROM phone_call_log "
                     "WHERE user_id = ? ORDER BY timestamp DESC LIMIT ?";

   sqlite3_stmt *stmt;
   int rc = sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("phone_db: call_log query prepare failed: %s", sqlite3_errmsg(s_db.db));
      pthread_mutex_unlock(&s_db.mutex);
      return -1;
   }

   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_int(stmt, 2, max);

   int count = 0;
   while (sqlite3_step(stmt) == SQLITE_ROW && count < max) {
      row_to_call_log(stmt, &out[count]);
      count++;
   }

   sqlite3_finalize(stmt);
   pthread_mutex_unlock(&s_db.mutex);
   return count;
}

/* =============================================================================
 * SMS Log
 * ============================================================================= */

int64_t phone_db_sms_log_insert(int user_id,
                                int direction,
                                const char *number,
                                const char *contact_name,
                                const char *body,
                                time_t timestamp) {
   if (!s_db.initialized) {
      return -1;
   }

   pthread_mutex_lock(&s_db.mutex);

   const char *sql = "INSERT INTO phone_sms_log "
                     "(user_id, direction, number, contact_name, body, timestamp, read) "
                     "VALUES (?, ?, ?, ?, ?, ?, 0)";

   sqlite3_stmt *stmt;
   int rc = sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("phone_db: sms_log insert prepare failed: %s", sqlite3_errmsg(s_db.db));
      pthread_mutex_unlock(&s_db.mutex);
      return -1;
   }

   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_int(stmt, 2, direction);
   sqlite3_bind_text(stmt, 3, number ? number : "", -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 4, contact_name ? contact_name : "", -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 5, body ? body : "", -1, SQLITE_TRANSIENT);
   sqlite3_bind_int64(stmt, 6, (int64_t)timestamp);

   rc = sqlite3_step(stmt);
   int64_t row_id = -1;
   if (rc == SQLITE_DONE) {
      row_id = sqlite3_last_insert_rowid(s_db.db);
   } else {
      OLOG_ERROR("phone_db: sms_log insert failed: %s", sqlite3_errmsg(s_db.db));
   }

   sqlite3_finalize(stmt);
   pthread_mutex_unlock(&s_db.mutex);
   return row_id;
}

int phone_db_sms_get_unread(int user_id, phone_sms_log_t *out, int max) {
   if (!s_db.initialized || !out || max <= 0) {
      return -1;
   }

   pthread_mutex_lock(&s_db.mutex);

   const char *sql = "SELECT id, user_id, direction, number, contact_name, body, "
                     "timestamp, read FROM phone_sms_log "
                     "WHERE user_id = ? AND read = 0 ORDER BY timestamp DESC LIMIT ?";

   sqlite3_stmt *stmt;
   int rc = sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("phone_db: sms unread query failed: %s", sqlite3_errmsg(s_db.db));
      pthread_mutex_unlock(&s_db.mutex);
      return -1;
   }

   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_int(stmt, 2, max);

   int count = 0;
   while (sqlite3_step(stmt) == SQLITE_ROW && count < max) {
      row_to_sms_log(stmt, &out[count]);
      count++;
   }

   sqlite3_finalize(stmt);
   pthread_mutex_unlock(&s_db.mutex);
   return count;
}

int phone_db_sms_log_recent(int user_id, phone_sms_log_t *out, int max) {
   if (!s_db.initialized || !out || max <= 0) {
      return -1;
   }

   pthread_mutex_lock(&s_db.mutex);

   const char *sql = "SELECT id, user_id, direction, number, contact_name, body, "
                     "timestamp, read FROM phone_sms_log "
                     "WHERE user_id = ? ORDER BY timestamp DESC LIMIT ?";

   sqlite3_stmt *stmt;
   int rc = sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("phone_db: sms recent query failed: %s", sqlite3_errmsg(s_db.db));
      pthread_mutex_unlock(&s_db.mutex);
      return -1;
   }

   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_int(stmt, 2, max);

   int count = 0;
   while (sqlite3_step(stmt) == SQLITE_ROW && count < max) {
      row_to_sms_log(stmt, &out[count]);
      count++;
   }

   sqlite3_finalize(stmt);
   pthread_mutex_unlock(&s_db.mutex);
   return count;
}

int phone_db_sms_mark_read(int64_t id) {
   if (!s_db.initialized) {
      return 1;
   }

   pthread_mutex_lock(&s_db.mutex);

   const char *sql = "UPDATE phone_sms_log SET read = 1 WHERE id = ?";

   sqlite3_stmt *stmt;
   int rc = sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      pthread_mutex_unlock(&s_db.mutex);
      return 1;
   }

   sqlite3_bind_int64(stmt, 1, id);
   rc = sqlite3_step(stmt);
   int result = (rc == SQLITE_DONE) ? 0 : 1;

   sqlite3_finalize(stmt);
   pthread_mutex_unlock(&s_db.mutex);
   return result;
}

int phone_db_cleanup(int call_retention_days, int sms_retention_days) {
   if (!s_db.initialized) {
      return 1;
   }

   pthread_mutex_lock(&s_db.mutex);

   time_t now = time(NULL);
   int result = 0;

   if (call_retention_days > 0) {
      time_t call_cutoff = now - (time_t)call_retention_days * 86400;
      sqlite3_stmt *stmt;
      int rc = sqlite3_prepare_v2(s_db.db, "DELETE FROM phone_call_log WHERE timestamp < ?", -1,
                                  &stmt, NULL);
      if (rc == SQLITE_OK) {
         sqlite3_bind_int64(stmt, 1, (int64_t)call_cutoff);
         if (sqlite3_step(stmt) != SQLITE_DONE) {
            OLOG_ERROR("phone_db: call cleanup failed: %s", sqlite3_errmsg(s_db.db));
            result = 1;
         }
         sqlite3_finalize(stmt);
      }
   }

   if (sms_retention_days > 0) {
      time_t sms_cutoff = now - (time_t)sms_retention_days * 86400;
      sqlite3_stmt *stmt;
      int rc = sqlite3_prepare_v2(s_db.db, "DELETE FROM phone_sms_log WHERE timestamp < ?", -1,
                                  &stmt, NULL);
      if (rc == SQLITE_OK) {
         sqlite3_bind_int64(stmt, 1, (int64_t)sms_cutoff);
         if (sqlite3_step(stmt) != SQLITE_DONE) {
            OLOG_ERROR("phone_db: sms cleanup failed: %s", sqlite3_errmsg(s_db.db));
            result = 1;
         }
         sqlite3_finalize(stmt);
      }
   }

   pthread_mutex_unlock(&s_db.mutex);
   return result;
}
