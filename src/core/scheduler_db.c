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
 * Scheduler Database Layer - SQLite CRUD for scheduled_events table
 *
 * Accesses s_db directly (same pattern as auth_db_conv.c).
 * All functions acquire the auth_db mutex.
 */

#define AUTH_DB_INTERNAL_ALLOWED
#include "core/scheduler_db.h"

#include <string.h>

#include "auth/auth_db_internal.h"
#include "logging.h"

/* =============================================================================
 * String Conversion Tables
 * ============================================================================= */

static const char *const event_type_strings[] = { "timer", "alarm", "reminder", "task" };
static const char *const status_strings[] = { "pending", "ringing", "fired",     "cancelled",
                                              "snoozed", "missed",  "dismissed", "timed_out" };
static const char *const recurrence_strings[] = { "once",     "daily",  "weekdays",
                                                  "weekends", "weekly", "custom" };

const char *sched_event_type_to_str(sched_event_type_t type) {
   if (type >= 0 && type <= SCHED_EVENT_TASK)
      return event_type_strings[type];
   return "timer";
}

sched_event_type_t sched_event_type_from_str(const char *str) {
   if (!str)
      return SCHED_EVENT_TIMER;
   for (int i = 0; i <= SCHED_EVENT_TASK; i++) {
      if (strcmp(str, event_type_strings[i]) == 0)
         return (sched_event_type_t)i;
   }
   return SCHED_EVENT_TIMER;
}

const char *sched_status_to_str(sched_status_t status) {
   if (status >= 0 && status <= SCHED_STATUS_TIMED_OUT)
      return status_strings[status];
   return "pending";
}

sched_status_t sched_status_from_str(const char *str) {
   if (!str)
      return SCHED_STATUS_PENDING;
   for (int i = 0; i <= SCHED_STATUS_TIMED_OUT; i++) {
      if (strcmp(str, status_strings[i]) == 0)
         return (sched_status_t)i;
   }
   return SCHED_STATUS_PENDING;
}

const char *sched_recurrence_to_str(sched_recurrence_t recurrence) {
   if (recurrence >= 0 && recurrence <= SCHED_RECUR_CUSTOM)
      return recurrence_strings[recurrence];
   return "once";
}

sched_recurrence_t sched_recurrence_from_str(const char *str) {
   if (!str)
      return SCHED_RECUR_ONCE;
   for (int i = 0; i <= SCHED_RECUR_CUSTOM; i++) {
      if (strcmp(str, recurrence_strings[i]) == 0)
         return (sched_recurrence_t)i;
   }
   return SCHED_RECUR_ONCE;
}

/* =============================================================================
 * Internal: Row extraction helper
 * ============================================================================= */

static void extract_event_row(sqlite3_stmt *stmt, sched_event_t *event) {
   memset(event, 0, sizeof(*event));
   event->id = sqlite3_column_int64(stmt, 0);
   event->user_id = sqlite3_column_int(stmt, 1);

   const char *type_str = (const char *)sqlite3_column_text(stmt, 2);
   event->event_type = sched_event_type_from_str(type_str);

   const char *status_str = (const char *)sqlite3_column_text(stmt, 3);
   event->status = sched_status_from_str(status_str);

   const char *name = (const char *)sqlite3_column_text(stmt, 4);
   if (name)
      strncpy(event->name, name, SCHED_NAME_MAX - 1);

   const char *msg = (const char *)sqlite3_column_text(stmt, 5);
   if (msg)
      strncpy(event->message, msg, SCHED_MESSAGE_MAX - 1);

   event->fire_at = (time_t)sqlite3_column_int64(stmt, 6);
   event->created_at = (time_t)sqlite3_column_int64(stmt, 7);
   event->duration_sec = sqlite3_column_int(stmt, 8);
   event->snoozed_until = (time_t)sqlite3_column_int64(stmt, 9);

   const char *recur_str = (const char *)sqlite3_column_text(stmt, 10);
   event->recurrence = sched_recurrence_from_str(recur_str);

   const char *recur_days = (const char *)sqlite3_column_text(stmt, 11);
   if (recur_days)
      strncpy(event->recurrence_days, recur_days, SCHED_RECURRENCE_DAYS_MAX - 1);

   const char *orig_time = (const char *)sqlite3_column_text(stmt, 12);
   if (orig_time)
      strncpy(event->original_time, orig_time, SCHED_ORIGINAL_TIME_MAX - 1);

   const char *uuid = (const char *)sqlite3_column_text(stmt, 13);
   if (uuid)
      strncpy(event->source_uuid, uuid, SCHED_UUID_MAX - 1);

   const char *loc = (const char *)sqlite3_column_text(stmt, 14);
   if (loc)
      strncpy(event->source_location, loc, SCHED_LOCATION_MAX - 1);

   event->announce_all = sqlite3_column_int(stmt, 15) != 0;

   const char *tool = (const char *)sqlite3_column_text(stmt, 16);
   if (tool)
      strncpy(event->tool_name, tool, SCHED_TOOL_NAME_MAX - 1);

   const char *tool_act = (const char *)sqlite3_column_text(stmt, 17);
   if (tool_act)
      strncpy(event->tool_action, tool_act, SCHED_TOOL_NAME_MAX - 1);

   const char *tool_val = (const char *)sqlite3_column_text(stmt, 18);
   if (tool_val)
      strncpy(event->tool_value, tool_val, SCHED_TOOL_VALUE_MAX - 1);

   event->fired_at = (time_t)sqlite3_column_int64(stmt, 19);
   event->snooze_count = sqlite3_column_int(stmt, 20);
}

/* Select all columns in consistent order */
#define SCHED_SELECT_COLS                                                      \
   "id, user_id, event_type, status, name, message, fire_at, created_at, "     \
   "duration_sec, snoozed_until, recurrence, recurrence_days, original_time, " \
   "source_uuid, source_location, announce_all, tool_name, tool_action, "      \
   "tool_value, fired_at, snooze_count"

/* =============================================================================
 * CRUD Operations
 * ============================================================================= */

int64_t scheduler_db_insert(sched_event_t *event) {
   AUTH_DB_LOCK_OR_RETURN(-1);

   event->created_at = time(NULL);

   const char *sql = "INSERT INTO scheduled_events "
                     "(user_id, event_type, status, name, message, fire_at, created_at, "
                     "duration_sec, snoozed_until, recurrence, recurrence_days, original_time, "
                     "source_uuid, source_location, announce_all, tool_name, tool_action, "
                     "tool_value, fired_at, snooze_count) "
                     "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)";

   sqlite3_stmt *stmt = NULL;
   int rc = sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      LOG_ERROR("scheduler_db: prepare insert failed: %s", sqlite3_errmsg(s_db.db));
      AUTH_DB_UNLOCK();
      return -1;
   }

   sqlite3_bind_int(stmt, 1, event->user_id);
   sqlite3_bind_text(stmt, 2, sched_event_type_to_str(event->event_type), -1, SQLITE_STATIC);
   sqlite3_bind_text(stmt, 3, sched_status_to_str(event->status), -1, SQLITE_STATIC);
   sqlite3_bind_text(stmt, 4, event->name, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 5, event->message, -1, SQLITE_TRANSIENT);
   sqlite3_bind_int64(stmt, 6, (int64_t)event->fire_at);
   sqlite3_bind_int64(stmt, 7, (int64_t)event->created_at);
   sqlite3_bind_int(stmt, 8, event->duration_sec);
   sqlite3_bind_int64(stmt, 9, (int64_t)event->snoozed_until);
   sqlite3_bind_text(stmt, 10, sched_recurrence_to_str(event->recurrence), -1, SQLITE_STATIC);
   sqlite3_bind_text(stmt, 11, event->recurrence_days, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 12, event->original_time, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 13, event->source_uuid[0] ? event->source_uuid : NULL, -1,
                     SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 14, event->source_location[0] ? event->source_location : NULL, -1,
                     SQLITE_TRANSIENT);
   sqlite3_bind_int(stmt, 15, event->announce_all ? 1 : 0);
   sqlite3_bind_text(stmt, 16, event->tool_name[0] ? event->tool_name : NULL, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 17, event->tool_action[0] ? event->tool_action : NULL, -1,
                     SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 18, event->tool_value[0] ? event->tool_value : NULL, -1,
                     SQLITE_TRANSIENT);
   sqlite3_bind_int64(stmt, 19, 0);
   sqlite3_bind_int(stmt, 20, 0);

   rc = sqlite3_step(stmt);
   int64_t id = -1;
   if (rc == SQLITE_DONE) {
      id = sqlite3_last_insert_rowid(s_db.db);
      event->id = id;
   } else {
      LOG_ERROR("scheduler_db: insert failed: %s", sqlite3_errmsg(s_db.db));
   }

   sqlite3_finalize(stmt);
   AUTH_DB_UNLOCK();
   return id;
}

int64_t scheduler_db_insert_checked(sched_event_t *event, int max_per_user, int max_total) {
   AUTH_DB_LOCK_OR_RETURN(-1);

   /* Check per-user limit under the same lock as insert */
   const char *count_user_sql = "SELECT COUNT(*) FROM scheduled_events "
                                "WHERE user_id = ? AND status IN ('pending', 'snoozed', 'ringing')";
   sqlite3_stmt *cnt_stmt = NULL;
   int rc = sqlite3_prepare_v2(s_db.db, count_user_sql, -1, &cnt_stmt, NULL);
   if (rc == SQLITE_OK) {
      sqlite3_bind_int(cnt_stmt, 1, event->user_id);
      if (sqlite3_step(cnt_stmt) == SQLITE_ROW) {
         int user_count = sqlite3_column_int(cnt_stmt, 0);
         if (user_count >= max_per_user) {
            sqlite3_finalize(cnt_stmt);
            AUTH_DB_UNLOCK();
            return -2; /* per-user limit exceeded */
         }
      }
      sqlite3_finalize(cnt_stmt);
   }

   /* Check global limit */
   const char *count_total_sql = "SELECT COUNT(*) FROM scheduled_events "
                                 "WHERE status IN ('pending', 'snoozed', 'ringing')";
   cnt_stmt = NULL;
   rc = sqlite3_prepare_v2(s_db.db, count_total_sql, -1, &cnt_stmt, NULL);
   if (rc == SQLITE_OK) {
      if (sqlite3_step(cnt_stmt) == SQLITE_ROW) {
         int total_count = sqlite3_column_int(cnt_stmt, 0);
         if (total_count >= max_total) {
            sqlite3_finalize(cnt_stmt);
            AUTH_DB_UNLOCK();
            return -3; /* global limit exceeded */
         }
      }
      sqlite3_finalize(cnt_stmt);
   }

   /* Perform insert under the same lock */
   event->created_at = time(NULL);

   const char *sql = "INSERT INTO scheduled_events "
                     "(user_id, event_type, status, name, message, fire_at, created_at, "
                     "duration_sec, snoozed_until, recurrence, recurrence_days, original_time, "
                     "source_uuid, source_location, announce_all, tool_name, tool_action, "
                     "tool_value, fired_at, snooze_count) "
                     "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)";

   sqlite3_stmt *stmt = NULL;
   rc = sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      LOG_ERROR("scheduler_db: prepare insert failed: %s", sqlite3_errmsg(s_db.db));
      AUTH_DB_UNLOCK();
      return -1;
   }

   sqlite3_bind_int(stmt, 1, event->user_id);
   sqlite3_bind_text(stmt, 2, sched_event_type_to_str(event->event_type), -1, SQLITE_STATIC);
   sqlite3_bind_text(stmt, 3, sched_status_to_str(event->status), -1, SQLITE_STATIC);
   sqlite3_bind_text(stmt, 4, event->name, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 5, event->message, -1, SQLITE_TRANSIENT);
   sqlite3_bind_int64(stmt, 6, (int64_t)event->fire_at);
   sqlite3_bind_int64(stmt, 7, (int64_t)event->created_at);
   sqlite3_bind_int(stmt, 8, event->duration_sec);
   sqlite3_bind_int64(stmt, 9, (int64_t)event->snoozed_until);
   sqlite3_bind_text(stmt, 10, sched_recurrence_to_str(event->recurrence), -1, SQLITE_STATIC);
   sqlite3_bind_text(stmt, 11, event->recurrence_days, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 12, event->original_time, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 13, event->source_uuid[0] ? event->source_uuid : NULL, -1,
                     SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 14, event->source_location[0] ? event->source_location : NULL, -1,
                     SQLITE_TRANSIENT);
   sqlite3_bind_int(stmt, 15, event->announce_all ? 1 : 0);
   sqlite3_bind_text(stmt, 16, event->tool_name[0] ? event->tool_name : NULL, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 17, event->tool_action[0] ? event->tool_action : NULL, -1,
                     SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 18, event->tool_value[0] ? event->tool_value : NULL, -1,
                     SQLITE_TRANSIENT);
   sqlite3_bind_int64(stmt, 19, 0);
   sqlite3_bind_int(stmt, 20, 0);

   rc = sqlite3_step(stmt);
   int64_t id = -1;
   if (rc == SQLITE_DONE) {
      id = sqlite3_last_insert_rowid(s_db.db);
      event->id = id;
   } else {
      LOG_ERROR("scheduler_db: insert failed: %s", sqlite3_errmsg(s_db.db));
   }

   sqlite3_finalize(stmt);
   AUTH_DB_UNLOCK();
   return id;
}

int scheduler_db_get(int64_t id, sched_event_t *event) {
   AUTH_DB_LOCK_OR_RETURN(-1);

   const char *sql = "SELECT " SCHED_SELECT_COLS " FROM scheduled_events WHERE id = ?";
   sqlite3_stmt *stmt = NULL;
   int rc = sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return -1;
   }

   sqlite3_bind_int64(stmt, 1, id);
   rc = sqlite3_step(stmt);

   int result = -1;
   if (rc == SQLITE_ROW) {
      extract_event_row(stmt, event);
      result = 0;
   }

   sqlite3_finalize(stmt);
   AUTH_DB_UNLOCK();
   return result;
}

int scheduler_db_update_status(int64_t id, sched_status_t status) {
   AUTH_DB_LOCK_OR_RETURN(-1);

   const char *sql = "UPDATE scheduled_events SET status = ? WHERE id = ?";
   sqlite3_stmt *stmt = NULL;
   int rc = sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return -1;
   }

   sqlite3_bind_text(stmt, 1, sched_status_to_str(status), -1, SQLITE_STATIC);
   sqlite3_bind_int64(stmt, 2, id);

   rc = sqlite3_step(stmt);
   int result = (rc == SQLITE_DONE) ? 0 : -1;

   sqlite3_finalize(stmt);
   AUTH_DB_UNLOCK();
   return result;
}

int scheduler_db_update_status_fired(int64_t id, sched_status_t status, time_t fired_at) {
   AUTH_DB_LOCK_OR_RETURN(-1);

   const char *sql = "UPDATE scheduled_events SET status = ?, fired_at = ? WHERE id = ?";
   sqlite3_stmt *stmt = NULL;
   int rc = sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return -1;
   }

   sqlite3_bind_text(stmt, 1, sched_status_to_str(status), -1, SQLITE_STATIC);
   sqlite3_bind_int64(stmt, 2, (int64_t)fired_at);
   sqlite3_bind_int64(stmt, 3, id);

   rc = sqlite3_step(stmt);
   int result = (rc == SQLITE_DONE) ? 0 : -1;

   sqlite3_finalize(stmt);
   AUTH_DB_UNLOCK();
   return result;
}

int scheduler_db_snooze(int64_t id, time_t new_fire_at) {
   AUTH_DB_LOCK_OR_RETURN(-1);

   const char *sql = "UPDATE scheduled_events SET status = 'snoozed', fire_at = ?, "
                     "snoozed_until = ?, snooze_count = snooze_count + 1 "
                     "WHERE id = ? AND status IN ('ringing', 'pending', 'snoozed')";
   sqlite3_stmt *stmt = NULL;
   int rc = sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return -1;
   }

   sqlite3_bind_int64(stmt, 1, (int64_t)new_fire_at);
   sqlite3_bind_int64(stmt, 2, (int64_t)new_fire_at);
   sqlite3_bind_int64(stmt, 3, id);

   rc = sqlite3_step(stmt);
   int changes = sqlite3_changes(s_db.db);
   int result = (rc == SQLITE_DONE && changes > 0) ? 0 : -1;

   sqlite3_finalize(stmt);
   AUTH_DB_UNLOCK();
   return result;
}

int scheduler_db_cancel(int64_t id) {
   AUTH_DB_LOCK_OR_RETURN(-1);

   const char *sql = "UPDATE scheduled_events SET status = 'cancelled' "
                     "WHERE id = ? AND status IN ('pending', 'snoozed')";
   sqlite3_stmt *stmt = NULL;
   int rc = sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return -1;
   }

   sqlite3_bind_int64(stmt, 1, id);
   rc = sqlite3_step(stmt);
   int changes = sqlite3_changes(s_db.db);
   int result = (rc == SQLITE_DONE && changes > 0) ? 0 : -1;

   sqlite3_finalize(stmt);
   AUTH_DB_UNLOCK();
   return result;
}

int scheduler_db_dismiss(int64_t id) {
   AUTH_DB_LOCK_OR_RETURN(-1);

   const char *sql = "UPDATE scheduled_events SET status = 'dismissed', fired_at = ? "
                     "WHERE id = ? AND status = 'ringing'";
   sqlite3_stmt *stmt = NULL;
   int rc = sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return -1;
   }

   sqlite3_bind_int64(stmt, 1, (int64_t)time(NULL));
   sqlite3_bind_int64(stmt, 2, id);

   rc = sqlite3_step(stmt);
   int changes = sqlite3_changes(s_db.db);
   int result = (rc == SQLITE_DONE && changes > 0) ? 0 : -1;

   sqlite3_finalize(stmt);
   AUTH_DB_UNLOCK();
   return result;
}

/* =============================================================================
 * Query Operations
 * ============================================================================= */

time_t scheduler_db_next_fire_time(void) {
   AUTH_DB_LOCK_OR_RETURN(0);

   const char *sql = "SELECT MIN(fire_at) FROM scheduled_events "
                     "WHERE status IN ('pending', 'snoozed')";
   sqlite3_stmt *stmt = NULL;
   int rc = sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return 0;
   }

   time_t result = 0;
   if (sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_type(stmt, 0) != SQLITE_NULL) {
      result = (time_t)sqlite3_column_int64(stmt, 0);
   }

   sqlite3_finalize(stmt);
   AUTH_DB_UNLOCK();
   return result;
}

int scheduler_db_get_due_events(sched_event_t *events, int max_count) {
   AUTH_DB_LOCK_OR_RETURN(0);

   const char *sql = "SELECT " SCHED_SELECT_COLS " FROM scheduled_events "
                     "WHERE fire_at <= ? AND status IN ('pending', 'snoozed') "
                     "ORDER BY fire_at ASC LIMIT ?";
   sqlite3_stmt *stmt = NULL;
   int rc = sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return 0;
   }

   sqlite3_bind_int64(stmt, 1, (int64_t)time(NULL));
   sqlite3_bind_int(stmt, 2, max_count);

   int count = 0;
   while (sqlite3_step(stmt) == SQLITE_ROW && count < max_count) {
      extract_event_row(stmt, &events[count]);
      count++;
   }

   sqlite3_finalize(stmt);
   AUTH_DB_UNLOCK();
   return count;
}

int scheduler_db_list_user_events(int user_id, int type, sched_event_t *events, int max_count) {
   AUTH_DB_LOCK_OR_RETURN(0);

   const char *sql;
   if (type >= 0) {
      sql = "SELECT " SCHED_SELECT_COLS " FROM scheduled_events "
            "WHERE user_id = ? AND status IN ('pending', 'snoozed', 'ringing') "
            "AND event_type = ? ORDER BY fire_at ASC LIMIT ?";
   } else {
      sql = "SELECT " SCHED_SELECT_COLS " FROM scheduled_events "
            "WHERE user_id = ? AND status IN ('pending', 'snoozed', 'ringing') "
            "ORDER BY fire_at ASC LIMIT ?";
   }

   sqlite3_stmt *stmt = NULL;
   int rc = sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return 0;
   }

   sqlite3_bind_int(stmt, 1, user_id);
   if (type >= 0) {
      sqlite3_bind_text(stmt, 2, sched_event_type_to_str((sched_event_type_t)type), -1,
                        SQLITE_STATIC);
      sqlite3_bind_int(stmt, 3, max_count);
   } else {
      sqlite3_bind_int(stmt, 2, max_count);
   }

   int count = 0;
   while (sqlite3_step(stmt) == SQLITE_ROW && count < max_count) {
      extract_event_row(stmt, &events[count]);
      count++;
   }

   sqlite3_finalize(stmt);
   AUTH_DB_UNLOCK();
   return count;
}

int scheduler_db_find_by_name(int user_id, const char *name, sched_event_t *event) {
   AUTH_DB_LOCK_OR_RETURN(-1);

   const char *sql = "SELECT " SCHED_SELECT_COLS " FROM scheduled_events "
                     "WHERE user_id = ? AND name = ? COLLATE NOCASE "
                     "AND status IN ('pending', 'snoozed', 'ringing') "
                     "ORDER BY created_at DESC LIMIT 1";
   sqlite3_stmt *stmt = NULL;
   int rc = sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return -1;
   }

   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_text(stmt, 2, name, -1, SQLITE_TRANSIENT);

   int result = -1;
   if (sqlite3_step(stmt) == SQLITE_ROW) {
      extract_event_row(stmt, event);
      result = 0;
   }

   sqlite3_finalize(stmt);
   AUTH_DB_UNLOCK();
   return result;
}

int scheduler_db_count_user_events(int user_id) {
   AUTH_DB_LOCK_OR_RETURN(-1);

   const char *sql = "SELECT COUNT(*) FROM scheduled_events "
                     "WHERE user_id = ? AND status IN ('pending', 'snoozed', 'ringing')";
   sqlite3_stmt *stmt = NULL;
   int rc = sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return -1;
   }

   sqlite3_bind_int(stmt, 1, user_id);

   int count = -1;
   if (sqlite3_step(stmt) == SQLITE_ROW)
      count = sqlite3_column_int(stmt, 0);

   sqlite3_finalize(stmt);
   AUTH_DB_UNLOCK();
   return count;
}

int scheduler_db_count_total_events(void) {
   AUTH_DB_LOCK_OR_RETURN(-1);

   const char *sql = "SELECT COUNT(*) FROM scheduled_events "
                     "WHERE status IN ('pending', 'snoozed', 'ringing')";
   sqlite3_stmt *stmt = NULL;
   int rc = sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return -1;
   }

   int count = -1;
   if (sqlite3_step(stmt) == SQLITE_ROW)
      count = sqlite3_column_int(stmt, 0);

   sqlite3_finalize(stmt);
   AUTH_DB_UNLOCK();
   return count;
}

int scheduler_db_get_ringing(sched_event_t *events, int max_count) {
   AUTH_DB_LOCK_OR_RETURN(0);

   const char *sql = "SELECT " SCHED_SELECT_COLS " FROM scheduled_events "
                     "WHERE status = 'ringing' ORDER BY fired_at ASC LIMIT ?";
   sqlite3_stmt *stmt = NULL;
   int rc = sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return 0;
   }

   sqlite3_bind_int(stmt, 1, max_count);

   int count = 0;
   while (sqlite3_step(stmt) == SQLITE_ROW && count < max_count) {
      extract_event_row(stmt, &events[count]);
      count++;
   }

   sqlite3_finalize(stmt);
   AUTH_DB_UNLOCK();
   return count;
}

int scheduler_db_get_active_by_uuid(const char *uuid, sched_event_t *events, int max_count) {
   AUTH_DB_LOCK_OR_RETURN(0);

   const char *sql = "SELECT " SCHED_SELECT_COLS " FROM scheduled_events "
                     "WHERE source_uuid = ? AND status IN ('pending', 'snoozed') "
                     "AND event_type = 'timer' ORDER BY fire_at ASC LIMIT ?";
   sqlite3_stmt *stmt = NULL;
   int rc = sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return 0;
   }

   sqlite3_bind_text(stmt, 1, uuid, -1, SQLITE_TRANSIENT);
   sqlite3_bind_int(stmt, 2, max_count);

   int count = 0;
   while (sqlite3_step(stmt) == SQLITE_ROW && count < max_count) {
      extract_event_row(stmt, &events[count]);
      count++;
   }

   sqlite3_finalize(stmt);
   AUTH_DB_UNLOCK();
   return count;
}

int scheduler_db_cleanup_old_events(int retention_days) {
   AUTH_DB_LOCK_OR_RETURN(-1);

   time_t cutoff = time(NULL) - (time_t)retention_days * 86400;

   const char *sql = "DELETE FROM scheduled_events "
                     "WHERE status IN ('fired', 'cancelled', 'missed', 'dismissed', 'timed_out') "
                     "AND ((fired_at > 0 AND fired_at < ?) OR "
                     "(fired_at = 0 AND created_at < ?))";
   sqlite3_stmt *stmt = NULL;
   int rc = sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return -1;
   }

   sqlite3_bind_int64(stmt, 1, (int64_t)cutoff);
   sqlite3_bind_int64(stmt, 2, (int64_t)cutoff);

   rc = sqlite3_step(stmt);
   int deleted = (rc == SQLITE_DONE) ? sqlite3_changes(s_db.db) : -1;

   sqlite3_finalize(stmt);
   AUTH_DB_UNLOCK();
   return deleted;
}

int scheduler_db_get_missed_events(sched_event_t *events, int max_count) {
   AUTH_DB_LOCK_OR_RETURN(0);

   const char *sql = "SELECT " SCHED_SELECT_COLS " FROM scheduled_events "
                     "WHERE fire_at < ? AND status IN ('pending', 'snoozed') "
                     "ORDER BY fire_at ASC LIMIT ?";
   sqlite3_stmt *stmt = NULL;
   int rc = sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return 0;
   }

   sqlite3_bind_int64(stmt, 1, (int64_t)time(NULL));
   sqlite3_bind_int(stmt, 2, max_count);

   int count = 0;
   while (sqlite3_step(stmt) == SQLITE_ROW && count < max_count) {
      extract_event_row(stmt, &events[count]);
      count++;
   }

   sqlite3_finalize(stmt);
   AUTH_DB_UNLOCK();
   return count;
}
