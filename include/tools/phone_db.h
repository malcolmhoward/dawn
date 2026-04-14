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
 * Uses the shared auth_db handle (Pattern A).
 */

#ifndef PHONE_DB_H
#define PHONE_DB_H

#include <stdint.h>
#include <time.h>

/* Call direction */
typedef enum {
   PHONE_DIR_OUTGOING = 0,
   PHONE_DIR_INCOMING = 1,
} phone_direction_t;

/* Call status */
typedef enum {
   PHONE_CALL_ANSWERED = 0,
   PHONE_CALL_MISSED = 1,
   PHONE_CALL_REJECTED = 2,
   PHONE_CALL_FAILED = 3,
} phone_call_status_t;

/* Call log entry */
typedef struct {
   int64_t id;
   int user_id;
   int direction; /* PHONE_DIR_OUTGOING or PHONE_DIR_INCOMING */
   char number[24];
   char contact_name[64];
   int duration_sec;
   time_t timestamp;
   int status; /* PHONE_CALL_ANSWERED, MISSED, REJECTED, FAILED */
} phone_call_log_t;

/* SMS log entry */
typedef struct {
   int64_t id;
   int user_id;
   int direction;
   char number[24];
   char contact_name[64];
   char body[1024];
   time_t timestamp;
   int read;
} phone_sms_log_t;

/**
 * @brief Insert a call log entry.
 * @return Row ID on success, -1 on error.
 */
int64_t phone_db_call_log_insert(int user_id,
                                 int direction,
                                 const char *number,
                                 const char *contact_name,
                                 int duration_sec,
                                 time_t timestamp,
                                 int status);

/**
 * @brief Update a call log entry (e.g., set duration after call ends).
 * @return 0 on success, 1 on error.
 */
int phone_db_call_log_update(int64_t id, int duration_sec, int status);

/**
 * @brief Query recent call log entries.
 * @param user_id  User ID for isolation.
 * @param out      Output array.
 * @param max      Maximum entries to return.
 * @return Number of entries, or -1 on error.
 */
int phone_db_call_log_recent(int user_id, phone_call_log_t *out, int max);

/**
 * @brief Insert an SMS log entry.
 * @return Row ID on success, -1 on error.
 */
int64_t phone_db_sms_log_insert(int user_id,
                                int direction,
                                const char *number,
                                const char *contact_name,
                                const char *body,
                                time_t timestamp);

/**
 * @brief Get unread SMS entries for a user.
 * @return Number of entries, or -1 on error.
 */
int phone_db_sms_get_unread(int user_id, phone_sms_log_t *out, int max);

/**
 * @brief Query recent SMS log entries.
 * @return Number of entries, or -1 on error.
 */
int phone_db_sms_log_recent(int user_id, phone_sms_log_t *out, int max);

/**
 * @brief Mark an SMS as read.
 * @return 0 on success, 1 on error.
 */
int phone_db_sms_mark_read(int64_t id);

/**
 * @brief Delete old log entries beyond retention period.
 * @return 0 on success, 1 on error.
 */
int phone_db_cleanup(int call_retention_days, int sms_retention_days);

#endif /* PHONE_DB_H */
