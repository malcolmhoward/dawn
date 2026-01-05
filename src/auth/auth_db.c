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
 * Authentication database implementation.
 *
 * SECURITY: All database operations use prepared statements.
 * NEVER use sqlite3_exec() or sqlite3_mprintf() with user input.
 * See: CWE-89, OWASP SQL Injection Prevention Cheat Sheet
 */

#include "auth/auth_db.h"

#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <limits.h>
#include <pthread.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "logging.h"

/* Current schema version */
#define SCHEMA_VERSION 3

/* Retention periods */
#define LOGIN_ATTEMPT_RETENTION_SEC (7 * 24 * 60 * 60) /* 7 days */
#define AUTH_LOG_RETENTION_SEC (30 * 24 * 60 * 60)     /* 30 days */

/* Helper macro for stringifying values in SQL */
#define STRINGIFY_HELPER(x) #x
#define STRINGIFY(x) STRINGIFY_HELPER(x)

/* Database state */
typedef struct {
   sqlite3 *db;
   pthread_mutex_t mutex;
   bool initialized;
   time_t last_cleanup;

   /* Prepared statements */
   sqlite3_stmt *stmt_create_user;
   sqlite3_stmt *stmt_get_user;
   sqlite3_stmt *stmt_count_users;
   sqlite3_stmt *stmt_inc_failed_attempts;
   sqlite3_stmt *stmt_reset_failed_attempts;
   sqlite3_stmt *stmt_update_last_login;
   sqlite3_stmt *stmt_set_lockout;

   sqlite3_stmt *stmt_create_session;
   sqlite3_stmt *stmt_get_session;
   sqlite3_stmt *stmt_update_session_activity;
   sqlite3_stmt *stmt_delete_session;
   sqlite3_stmt *stmt_delete_user_sessions;
   sqlite3_stmt *stmt_delete_expired_sessions;

   sqlite3_stmt *stmt_count_recent_failures;
   sqlite3_stmt *stmt_log_attempt;
   sqlite3_stmt *stmt_delete_old_attempts;

   sqlite3_stmt *stmt_log_event;
   sqlite3_stmt *stmt_delete_old_logs;

   sqlite3_stmt *stmt_get_user_settings;
   sqlite3_stmt *stmt_set_user_settings;
} auth_db_state_t;

static auth_db_state_t s_db = {
   .db = NULL,
   .mutex = PTHREAD_MUTEX_INITIALIZER,
   .initialized = false,
   .last_cleanup = 0,
};

/* Forward declarations */
static int create_schema(void);
static int prepare_statements(void);
static void finalize_statements(void);
static int verify_file_permissions(const char *path);
static int create_parent_directory(const char *path);

/* ============================================================================
 * Schema Creation
 * ============================================================================ */

static const char *SCHEMA_SQL =
    /* Schema version tracking */
    "CREATE TABLE IF NOT EXISTS schema_version ("
    "   version INTEGER PRIMARY KEY"
    ");"

    /* Users table */
    "CREATE TABLE IF NOT EXISTS users ("
    "   id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "   username TEXT UNIQUE NOT NULL,"
    "   password_hash TEXT NOT NULL,"
    "   is_admin INTEGER DEFAULT 0,"
    "   created_at INTEGER NOT NULL,"
    "   last_login INTEGER,"
    "   failed_attempts INTEGER DEFAULT 0,"
    "   lockout_until INTEGER DEFAULT 0"
    ");"

    /* Sessions table */
    "CREATE TABLE IF NOT EXISTS sessions ("
    "   token TEXT PRIMARY KEY,"
    "   user_id INTEGER NOT NULL,"
    "   created_at INTEGER NOT NULL,"
    "   last_activity INTEGER NOT NULL,"
    "   ip_address TEXT,"
    "   user_agent TEXT,"
    "   FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_sessions_user ON sessions(user_id);"
    "CREATE INDEX IF NOT EXISTS idx_sessions_activity ON sessions(last_activity);"

    /* Login attempts for rate limiting */
    "CREATE TABLE IF NOT EXISTS login_attempts ("
    "   id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "   ip_address TEXT NOT NULL,"
    "   username TEXT,"
    "   timestamp INTEGER NOT NULL,"
    "   success INTEGER DEFAULT 0"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_attempts_ip ON login_attempts(ip_address, timestamp);"

    /* Audit log */
    "CREATE TABLE IF NOT EXISTS auth_log ("
    "   id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "   timestamp INTEGER NOT NULL,"
    "   event TEXT NOT NULL,"
    "   username TEXT,"
    "   ip_address TEXT,"
    "   details TEXT"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_log_timestamp ON auth_log(timestamp);"

    /* Per-user settings (added in schema v2, persona_mode added in v3) */
    "CREATE TABLE IF NOT EXISTS user_settings ("
    "   user_id INTEGER PRIMARY KEY,"
    "   persona_description TEXT,"
    "   persona_mode TEXT DEFAULT 'append',"
    "   location TEXT,"
    "   timezone TEXT DEFAULT 'UTC',"
    "   units TEXT DEFAULT 'metric',"
    "   tts_voice_model TEXT,"
    "   tts_length_scale REAL DEFAULT 1.0,"
    "   updated_at INTEGER NOT NULL,"
    "   FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE"
    ");";

static int get_current_schema_version(void) {
   sqlite3_stmt *stmt = NULL;
   int version = 0;

   int rc = sqlite3_prepare_v2(s_db.db, "SELECT version FROM schema_version LIMIT 1", -1, &stmt,
                               NULL);
   if (rc == SQLITE_OK) {
      if (sqlite3_step(stmt) == SQLITE_ROW) {
         version = sqlite3_column_int(stmt, 0);
      }
      sqlite3_finalize(stmt);
   }
   return version;
}

static int create_schema(void) {
   char *errmsg = NULL;

   /* Check current schema version (0 if fresh install) */
   int current_version = get_current_schema_version();

   /* Execute schema SQL - all tables use IF NOT EXISTS for idempotency */
   int rc = sqlite3_exec(s_db.db, SCHEMA_SQL, NULL, NULL, &errmsg);
   if (rc != SQLITE_OK) {
      LOG_ERROR("auth_db: schema creation failed: %s", errmsg ? errmsg : "unknown");
      sqlite3_free(errmsg);
      return AUTH_DB_FAILURE;
   }

   /* v3 migration: add persona_mode column to user_settings if missing
    * This handles upgrades from v1 or v2 where the table may exist without this column */
   if (current_version >= 1 && current_version < 3) {
      rc = sqlite3_exec(s_db.db,
                        "ALTER TABLE user_settings ADD COLUMN persona_mode TEXT DEFAULT 'append'",
                        NULL, NULL, &errmsg);
      if (rc != SQLITE_OK) {
         /* Column might already exist or table might not exist yet - not fatal */
         LOG_INFO("auth_db: v3 migration note: %s (may be normal)", errmsg ? errmsg : "ok");
         sqlite3_free(errmsg);
         errmsg = NULL;
      } else {
         LOG_INFO("auth_db: added persona_mode column to user_settings");
      }
   }

   /* Log migration if upgrading from an older version */
   if (current_version > 0 && current_version < SCHEMA_VERSION) {
      LOG_INFO("auth_db: migrated schema from v%d to v%d", current_version, SCHEMA_VERSION);
   } else if (current_version == 0) {
      LOG_INFO("auth_db: created schema v%d", SCHEMA_VERSION);
   }

   /* Insert or update schema version */
   rc = sqlite3_exec(s_db.db,
                     "INSERT OR REPLACE INTO schema_version (version) VALUES (" STRINGIFY(
                         SCHEMA_VERSION) ")",
                     NULL, NULL, &errmsg);
   if (rc != SQLITE_OK) {
      LOG_ERROR("auth_db: failed to set schema version: %s", errmsg ? errmsg : "unknown");
      sqlite3_free(errmsg);
      return AUTH_DB_FAILURE;
   }

   return AUTH_DB_SUCCESS;
}

/* ============================================================================
 * Prepared Statement Management
 * ============================================================================ */

static int prepare_statements(void) {
   int rc;

#define PREPARE(stmt, sql)                                                               \
   do {                                                                                  \
      rc = sqlite3_prepare_v2(s_db.db, sql, -1, &s_db.stmt, NULL);                       \
      if (rc != SQLITE_OK) {                                                             \
         LOG_ERROR("auth_db: failed to prepare %s: %s", #stmt, sqlite3_errmsg(s_db.db)); \
         return AUTH_DB_FAILURE;                                                         \
      }                                                                                  \
   } while (0)

   /* User statements */
   rc = sqlite3_prepare_v2(
       s_db.db,
       "INSERT INTO users (username, password_hash, is_admin, created_at) VALUES (?, ?, ?, ?)", -1,
       &s_db.stmt_create_user, NULL);
   if (rc != SQLITE_OK) {
      LOG_ERROR("auth_db: prepare create_user failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(
       s_db.db,
       "SELECT id, username, password_hash, is_admin, created_at, "
       "last_login, failed_attempts, lockout_until FROM users WHERE username = ?",
       -1, &s_db.stmt_get_user, NULL);
   if (rc != SQLITE_OK) {
      LOG_ERROR("auth_db: prepare get_user failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db, "SELECT COUNT(*) FROM users", -1, &s_db.stmt_count_users, NULL);
   if (rc != SQLITE_OK) {
      LOG_ERROR("auth_db: prepare count_users failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(
       s_db.db, "UPDATE users SET failed_attempts = failed_attempts + 1 WHERE username = ?", -1,
       &s_db.stmt_inc_failed_attempts, NULL);
   if (rc != SQLITE_OK) {
      LOG_ERROR("auth_db: prepare inc_failed_attempts failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db, "UPDATE users SET failed_attempts = 0 WHERE username = ?", -1,
                           &s_db.stmt_reset_failed_attempts, NULL);
   if (rc != SQLITE_OK) {
      LOG_ERROR("auth_db: prepare reset_failed_attempts failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db, "UPDATE users SET last_login = ? WHERE username = ?", -1,
                           &s_db.stmt_update_last_login, NULL);
   if (rc != SQLITE_OK) {
      LOG_ERROR("auth_db: prepare update_last_login failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db, "UPDATE users SET lockout_until = ? WHERE username = ?", -1,
                           &s_db.stmt_set_lockout, NULL);
   if (rc != SQLITE_OK) {
      LOG_ERROR("auth_db: prepare set_lockout failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   /* Session statements */
   rc = sqlite3_prepare_v2(s_db.db,
                           "INSERT INTO sessions (token, user_id, created_at, last_activity, "
                           "ip_address, user_agent) VALUES (?, ?, ?, ?, ?, ?)",
                           -1, &s_db.stmt_create_session, NULL);
   if (rc != SQLITE_OK) {
      LOG_ERROR("auth_db: prepare create_session failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db,
                           "SELECT s.token, s.user_id, u.username, u.is_admin, s.created_at, "
                           "s.last_activity, s.ip_address, s.user_agent "
                           "FROM sessions s JOIN users u ON s.user_id = u.id WHERE s.token = ?",
                           -1, &s_db.stmt_get_session, NULL);
   if (rc != SQLITE_OK) {
      LOG_ERROR("auth_db: prepare get_session failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db, "UPDATE sessions SET last_activity = ? WHERE token = ?", -1,
                           &s_db.stmt_update_session_activity, NULL);
   if (rc != SQLITE_OK) {
      LOG_ERROR("auth_db: prepare update_session_activity failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db, "DELETE FROM sessions WHERE token = ?", -1,
                           &s_db.stmt_delete_session, NULL);
   if (rc != SQLITE_OK) {
      LOG_ERROR("auth_db: prepare delete_session failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db, "DELETE FROM sessions WHERE user_id = ?", -1,
                           &s_db.stmt_delete_user_sessions, NULL);
   if (rc != SQLITE_OK) {
      LOG_ERROR("auth_db: prepare delete_user_sessions failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db, "DELETE FROM sessions WHERE last_activity < ?", -1,
                           &s_db.stmt_delete_expired_sessions, NULL);
   if (rc != SQLITE_OK) {
      LOG_ERROR("auth_db: prepare delete_expired_sessions failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   /* Rate limiting statements */
   rc = sqlite3_prepare_v2(
       s_db.db,
       "SELECT COUNT(*) FROM login_attempts WHERE ip_address = ? AND timestamp > ? AND success = 0",
       -1, &s_db.stmt_count_recent_failures, NULL);
   if (rc != SQLITE_OK) {
      LOG_ERROR("auth_db: prepare count_recent_failures failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(
       s_db.db,
       "INSERT INTO login_attempts (ip_address, username, timestamp, success) VALUES (?, ?, ?, ?)",
       -1, &s_db.stmt_log_attempt, NULL);
   if (rc != SQLITE_OK) {
      LOG_ERROR("auth_db: prepare log_attempt failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db, "DELETE FROM login_attempts WHERE timestamp < ?", -1,
                           &s_db.stmt_delete_old_attempts, NULL);
   if (rc != SQLITE_OK) {
      LOG_ERROR("auth_db: prepare delete_old_attempts failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   /* Audit log statements */
   rc = sqlite3_prepare_v2(s_db.db,
                           "INSERT INTO auth_log (timestamp, event, username, ip_address, details) "
                           "VALUES (?, ?, ?, ?, ?)",
                           -1, &s_db.stmt_log_event, NULL);
   if (rc != SQLITE_OK) {
      LOG_ERROR("auth_db: prepare log_event failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db, "DELETE FROM auth_log WHERE timestamp < ?", -1,
                           &s_db.stmt_delete_old_logs, NULL);
   if (rc != SQLITE_OK) {
      LOG_ERROR("auth_db: prepare delete_old_logs failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   /* User settings statements */
   rc = sqlite3_prepare_v2(
       s_db.db,
       "SELECT persona_description, persona_mode, location, timezone, units, tts_voice_model, "
       "tts_length_scale FROM user_settings WHERE user_id = ?",
       -1, &s_db.stmt_get_user_settings, NULL);
   if (rc != SQLITE_OK) {
      LOG_ERROR("auth_db: prepare get_user_settings failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(
       s_db.db,
       "INSERT INTO user_settings (user_id, persona_description, persona_mode, location, timezone, "
       "units, tts_voice_model, tts_length_scale, updated_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?) "
       "ON CONFLICT(user_id) DO UPDATE SET "
       "persona_description=excluded.persona_description, persona_mode=excluded.persona_mode, "
       "location=excluded.location, timezone=excluded.timezone, units=excluded.units, "
       "tts_voice_model=excluded.tts_voice_model, "
       "tts_length_scale=excluded.tts_length_scale, updated_at=excluded.updated_at",
       -1, &s_db.stmt_set_user_settings, NULL);
   if (rc != SQLITE_OK) {
      LOG_ERROR("auth_db: prepare set_user_settings failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

#undef PREPARE

   return AUTH_DB_SUCCESS;
}

static void finalize_statements(void) {
   if (s_db.stmt_create_user)
      sqlite3_finalize(s_db.stmt_create_user);
   if (s_db.stmt_get_user)
      sqlite3_finalize(s_db.stmt_get_user);
   if (s_db.stmt_count_users)
      sqlite3_finalize(s_db.stmt_count_users);
   if (s_db.stmt_inc_failed_attempts)
      sqlite3_finalize(s_db.stmt_inc_failed_attempts);
   if (s_db.stmt_reset_failed_attempts)
      sqlite3_finalize(s_db.stmt_reset_failed_attempts);
   if (s_db.stmt_update_last_login)
      sqlite3_finalize(s_db.stmt_update_last_login);
   if (s_db.stmt_set_lockout)
      sqlite3_finalize(s_db.stmt_set_lockout);

   if (s_db.stmt_create_session)
      sqlite3_finalize(s_db.stmt_create_session);
   if (s_db.stmt_get_session)
      sqlite3_finalize(s_db.stmt_get_session);
   if (s_db.stmt_update_session_activity)
      sqlite3_finalize(s_db.stmt_update_session_activity);
   if (s_db.stmt_delete_session)
      sqlite3_finalize(s_db.stmt_delete_session);
   if (s_db.stmt_delete_user_sessions)
      sqlite3_finalize(s_db.stmt_delete_user_sessions);
   if (s_db.stmt_delete_expired_sessions)
      sqlite3_finalize(s_db.stmt_delete_expired_sessions);

   if (s_db.stmt_count_recent_failures)
      sqlite3_finalize(s_db.stmt_count_recent_failures);
   if (s_db.stmt_log_attempt)
      sqlite3_finalize(s_db.stmt_log_attempt);
   if (s_db.stmt_delete_old_attempts)
      sqlite3_finalize(s_db.stmt_delete_old_attempts);

   if (s_db.stmt_log_event)
      sqlite3_finalize(s_db.stmt_log_event);
   if (s_db.stmt_delete_old_logs)
      sqlite3_finalize(s_db.stmt_delete_old_logs);

   if (s_db.stmt_get_user_settings)
      sqlite3_finalize(s_db.stmt_get_user_settings);
   if (s_db.stmt_set_user_settings)
      sqlite3_finalize(s_db.stmt_set_user_settings);

   /* Clear all pointers */
   memset(&s_db.stmt_create_user, 0,
          (char *)&s_db.stmt_set_user_settings - (char *)&s_db.stmt_create_user +
              sizeof(s_db.stmt_set_user_settings));
}

/* ============================================================================
 * File Permission Helpers
 * ============================================================================ */

static int create_parent_directory(const char *path) {
   char *path_copy = strdup(path);
   if (!path_copy) {
      return AUTH_DB_FAILURE;
   }

   char *dir = dirname(path_copy);

   /* Set restrictive umask for directory creation */
   mode_t old_umask = umask(0077);

   int rc = AUTH_DB_SUCCESS;
   if (mkdir(dir, 0700) != 0 && errno != EEXIST) {
      LOG_ERROR("auth_db: failed to create directory %s: %s", dir, strerror(errno));
      rc = AUTH_DB_FAILURE;
   }

   umask(old_umask);
   free(path_copy);
   return rc;
}

static int verify_file_permissions(const char *path) {
   struct stat st;

   if (stat(path, &st) != 0) {
      /* File doesn't exist yet, that's OK */
      if (errno == ENOENT) {
         return AUTH_DB_SUCCESS;
      }
      LOG_ERROR("auth_db: stat(%s) failed: %s", path, strerror(errno));
      return AUTH_DB_FAILURE;
   }

   /* Check for world/group readable or writable */
   if ((st.st_mode & 0077) != 0) {
      LOG_WARNING("auth_db: SECURITY: %s has unsafe permissions %04o, fixing to 0600", path,
                  st.st_mode & 0777);

      if (chmod(path, 0600) != 0) {
         LOG_ERROR("auth_db: failed to fix permissions on %s: %s", path, strerror(errno));
         return AUTH_DB_FAILURE;
      }
   }

   return AUTH_DB_SUCCESS;
}

/* ============================================================================
 * Lifecycle Functions
 * ============================================================================ */

int auth_db_init(const char *db_path) {
   pthread_mutex_lock(&s_db.mutex);

   if (s_db.initialized) {
      LOG_WARNING("auth_db_init: already initialized");
      pthread_mutex_unlock(&s_db.mutex);
      return AUTH_DB_SUCCESS;
   }

   const char *path = db_path ? db_path : AUTH_DB_DEFAULT_PATH;

   /* Create parent directory with secure permissions */
   if (create_parent_directory(path) != AUTH_DB_SUCCESS) {
      pthread_mutex_unlock(&s_db.mutex);
      return AUTH_DB_FAILURE;
   }

   /* Check existing file permissions */
   if (verify_file_permissions(path) != AUTH_DB_SUCCESS) {
      pthread_mutex_unlock(&s_db.mutex);
      return AUTH_DB_FAILURE;
   }

   /* Set restrictive umask for database file creation */
   mode_t old_umask = umask(0077);

   /* Open database with FULLMUTEX for thread safety */
   int rc = sqlite3_open_v2(path, &s_db.db,
                            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                            NULL);

   umask(old_umask);

   if (rc != SQLITE_OK) {
      LOG_ERROR("auth_db_init: failed to open %s: %s", path,
                s_db.db ? sqlite3_errmsg(s_db.db) : "unknown");
      if (s_db.db) {
         sqlite3_close(s_db.db);
         s_db.db = NULL;
      }
      pthread_mutex_unlock(&s_db.mutex);
      return AUTH_DB_FAILURE;
   }

   /* Verify permissions after creation */
   if (verify_file_permissions(path) != AUTH_DB_SUCCESS) {
      sqlite3_close(s_db.db);
      s_db.db = NULL;
      pthread_mutex_unlock(&s_db.mutex);
      return AUTH_DB_FAILURE;
   }

   /* Enable WAL mode for better concurrency */
   char *errmsg = NULL;
   rc = sqlite3_exec(s_db.db, "PRAGMA journal_mode=WAL", NULL, NULL, &errmsg);
   if (rc != SQLITE_OK) {
      LOG_WARNING("auth_db: failed to enable WAL mode: %s", errmsg ? errmsg : "unknown");
      sqlite3_free(errmsg);
      /* Continue anyway - DELETE mode works too */
   }

   /* Set conservative cache size for embedded systems (64KB) */
   sqlite3_exec(s_db.db, "PRAGMA cache_size=64", NULL, NULL, NULL); /* 256KB cache */

   /* Enable foreign keys */
   sqlite3_exec(s_db.db, "PRAGMA foreign_keys=ON", NULL, NULL, NULL);

   /* Create schema if needed */
   if (create_schema() != AUTH_DB_SUCCESS) {
      sqlite3_close(s_db.db);
      s_db.db = NULL;
      pthread_mutex_unlock(&s_db.mutex);
      return AUTH_DB_FAILURE;
   }

   /* Prepare statements */
   if (prepare_statements() != AUTH_DB_SUCCESS) {
      finalize_statements();
      sqlite3_close(s_db.db);
      s_db.db = NULL;
      pthread_mutex_unlock(&s_db.mutex);
      return AUTH_DB_FAILURE;
   }

   s_db.initialized = true;
   s_db.last_cleanup = time(NULL);

   LOG_INFO("auth_db_init: initialized at %s", path);

   pthread_mutex_unlock(&s_db.mutex);
   return AUTH_DB_SUCCESS;
}

void auth_db_shutdown(void) {
   pthread_mutex_lock(&s_db.mutex);

   if (!s_db.initialized) {
      pthread_mutex_unlock(&s_db.mutex);
      return;
   }

   /* Checkpoint WAL to main database */
   if (s_db.db) {
      sqlite3_wal_checkpoint_v2(s_db.db, NULL, SQLITE_CHECKPOINT_TRUNCATE, NULL, NULL);
   }

   /* Finalize all statements */
   finalize_statements();

   /* Close database */
   if (s_db.db) {
      sqlite3_close(s_db.db);
      s_db.db = NULL;
   }

   s_db.initialized = false;

   LOG_INFO("auth_db_shutdown: complete");

   pthread_mutex_unlock(&s_db.mutex);
}

bool auth_db_is_ready(void) {
   pthread_mutex_lock(&s_db.mutex);
   bool ready = s_db.initialized;
   pthread_mutex_unlock(&s_db.mutex);
   return ready;
}

/* ============================================================================
 * User Operations
 * ============================================================================ */

int auth_db_create_user(const char *username, const char *password_hash, bool is_admin) {
   if (!username || !password_hash) {
      return AUTH_DB_INVALID;
   }

   size_t ulen = strlen(username);
   if (ulen == 0 || ulen >= AUTH_USERNAME_MAX) {
      return AUTH_DB_INVALID;
   }

   pthread_mutex_lock(&s_db.mutex);

   if (!s_db.initialized) {
      pthread_mutex_unlock(&s_db.mutex);
      return AUTH_DB_FAILURE;
   }

   sqlite3_reset(s_db.stmt_create_user);
   sqlite3_bind_text(s_db.stmt_create_user, 1, username, -1, SQLITE_STATIC);
   sqlite3_bind_text(s_db.stmt_create_user, 2, password_hash, -1, SQLITE_STATIC);
   sqlite3_bind_int(s_db.stmt_create_user, 3, is_admin ? 1 : 0);
   sqlite3_bind_int64(s_db.stmt_create_user, 4, (int64_t)time(NULL));

   int rc = sqlite3_step(s_db.stmt_create_user);
   sqlite3_reset(s_db.stmt_create_user);

   pthread_mutex_unlock(&s_db.mutex);

   if (rc == SQLITE_CONSTRAINT) {
      return AUTH_DB_DUPLICATE;
   } else if (rc != SQLITE_DONE) {
      LOG_ERROR("auth_db_create_user: failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   return AUTH_DB_SUCCESS;
}

int auth_db_get_user(const char *username, auth_user_t *user_out) {
   if (!username) {
      return AUTH_DB_INVALID;
   }

   pthread_mutex_lock(&s_db.mutex);

   if (!s_db.initialized) {
      pthread_mutex_unlock(&s_db.mutex);
      return AUTH_DB_FAILURE;
   }

   sqlite3_reset(s_db.stmt_get_user);
   sqlite3_bind_text(s_db.stmt_get_user, 1, username, -1, SQLITE_STATIC);

   int rc = sqlite3_step(s_db.stmt_get_user);

   if (rc == SQLITE_ROW) {
      if (user_out) {
         user_out->id = sqlite3_column_int(s_db.stmt_get_user, 0);

         const char *uname = (const char *)sqlite3_column_text(s_db.stmt_get_user, 1);
         if (uname) {
            strncpy(user_out->username, uname, AUTH_USERNAME_MAX - 1);
            user_out->username[AUTH_USERNAME_MAX - 1] = '\0';
         }

         const char *hash = (const char *)sqlite3_column_text(s_db.stmt_get_user, 2);
         if (hash) {
            strncpy(user_out->password_hash, hash, AUTH_HASH_LEN - 1);
            user_out->password_hash[AUTH_HASH_LEN - 1] = '\0';
         }

         user_out->is_admin = sqlite3_column_int(s_db.stmt_get_user, 3) != 0;
         user_out->created_at = (time_t)sqlite3_column_int64(s_db.stmt_get_user, 4);
         user_out->last_login = (time_t)sqlite3_column_int64(s_db.stmt_get_user, 5);
         user_out->failed_attempts = sqlite3_column_int(s_db.stmt_get_user, 6);
         user_out->lockout_until = (time_t)sqlite3_column_int64(s_db.stmt_get_user, 7);
      }
      sqlite3_reset(s_db.stmt_get_user);
      pthread_mutex_unlock(&s_db.mutex);
      return AUTH_DB_SUCCESS;
   }

   sqlite3_reset(s_db.stmt_get_user);
   pthread_mutex_unlock(&s_db.mutex);

   if (rc == SQLITE_DONE) {
      return AUTH_DB_NOT_FOUND;
   }

   LOG_ERROR("auth_db_get_user: failed: %s", sqlite3_errmsg(s_db.db));
   return AUTH_DB_FAILURE;
}

int auth_db_user_count(void) {
   pthread_mutex_lock(&s_db.mutex);

   if (!s_db.initialized) {
      pthread_mutex_unlock(&s_db.mutex);
      return -1;
   }

   sqlite3_reset(s_db.stmt_count_users);
   int rc = sqlite3_step(s_db.stmt_count_users);

   int count = -1;
   if (rc == SQLITE_ROW) {
      count = sqlite3_column_int(s_db.stmt_count_users, 0);
   }

   sqlite3_reset(s_db.stmt_count_users);
   pthread_mutex_unlock(&s_db.mutex);

   return count;
}

int auth_db_validate_username(const char *username) {
   if (!username) {
      return AUTH_DB_INVALID;
   }

   size_t len = strlen(username);
   if (len == 0 || len >= AUTH_USERNAME_MAX) {
      return AUTH_DB_INVALID;
   }

   /* First character must be letter or underscore */
   char c = username[0];
   if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_')) {
      return AUTH_DB_INVALID;
   }

   /* Remaining characters: alphanumeric, underscore, hyphen, period */
   for (size_t i = 1; i < len; i++) {
      c = username[i];
      if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '_' || c == '-' || c == '.')) {
         return AUTH_DB_INVALID;
      }
   }

   return AUTH_DB_SUCCESS;
}

int auth_db_increment_failed_attempts(const char *username) {
   if (!username) {
      return AUTH_DB_INVALID;
   }

   pthread_mutex_lock(&s_db.mutex);

   if (!s_db.initialized) {
      pthread_mutex_unlock(&s_db.mutex);
      return AUTH_DB_FAILURE;
   }

   sqlite3_reset(s_db.stmt_inc_failed_attempts);
   sqlite3_bind_text(s_db.stmt_inc_failed_attempts, 1, username, -1, SQLITE_STATIC);

   int rc = sqlite3_step(s_db.stmt_inc_failed_attempts);
   sqlite3_reset(s_db.stmt_inc_failed_attempts);

   pthread_mutex_unlock(&s_db.mutex);

   return (rc == SQLITE_DONE) ? AUTH_DB_SUCCESS : AUTH_DB_FAILURE;
}

int auth_db_reset_failed_attempts(const char *username) {
   if (!username) {
      return AUTH_DB_INVALID;
   }

   pthread_mutex_lock(&s_db.mutex);

   if (!s_db.initialized) {
      pthread_mutex_unlock(&s_db.mutex);
      return AUTH_DB_FAILURE;
   }

   sqlite3_reset(s_db.stmt_reset_failed_attempts);
   sqlite3_bind_text(s_db.stmt_reset_failed_attempts, 1, username, -1, SQLITE_STATIC);

   int rc = sqlite3_step(s_db.stmt_reset_failed_attempts);
   sqlite3_reset(s_db.stmt_reset_failed_attempts);

   pthread_mutex_unlock(&s_db.mutex);

   return (rc == SQLITE_DONE) ? AUTH_DB_SUCCESS : AUTH_DB_FAILURE;
}

int auth_db_update_last_login(const char *username) {
   if (!username) {
      return AUTH_DB_INVALID;
   }

   pthread_mutex_lock(&s_db.mutex);

   if (!s_db.initialized) {
      pthread_mutex_unlock(&s_db.mutex);
      return AUTH_DB_FAILURE;
   }

   sqlite3_reset(s_db.stmt_update_last_login);
   sqlite3_bind_int64(s_db.stmt_update_last_login, 1, (int64_t)time(NULL));
   sqlite3_bind_text(s_db.stmt_update_last_login, 2, username, -1, SQLITE_STATIC);

   int rc = sqlite3_step(s_db.stmt_update_last_login);
   sqlite3_reset(s_db.stmt_update_last_login);

   pthread_mutex_unlock(&s_db.mutex);

   return (rc == SQLITE_DONE) ? AUTH_DB_SUCCESS : AUTH_DB_FAILURE;
}

int auth_db_set_lockout(const char *username, time_t lockout_until) {
   if (!username) {
      return AUTH_DB_INVALID;
   }

   pthread_mutex_lock(&s_db.mutex);

   if (!s_db.initialized) {
      pthread_mutex_unlock(&s_db.mutex);
      return AUTH_DB_FAILURE;
   }

   sqlite3_reset(s_db.stmt_set_lockout);
   sqlite3_bind_int64(s_db.stmt_set_lockout, 1, (int64_t)lockout_until);
   sqlite3_bind_text(s_db.stmt_set_lockout, 2, username, -1, SQLITE_STATIC);

   int rc = sqlite3_step(s_db.stmt_set_lockout);
   sqlite3_reset(s_db.stmt_set_lockout);

   pthread_mutex_unlock(&s_db.mutex);

   return (rc == SQLITE_DONE) ? AUTH_DB_SUCCESS : AUTH_DB_FAILURE;
}

int auth_db_list_users(auth_user_summary_callback_t callback, void *ctx) {
   if (!callback) {
      return AUTH_DB_INVALID;
   }

   pthread_mutex_lock(&s_db.mutex);

   if (!s_db.initialized) {
      pthread_mutex_unlock(&s_db.mutex);
      return AUTH_DB_FAILURE;
   }

   const char *sql = "SELECT id, username, is_admin, created_at, last_login, "
                     "failed_attempts, lockout_until FROM users ORDER BY id";
   sqlite3_stmt *stmt = NULL;
   int rc = sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      pthread_mutex_unlock(&s_db.mutex);
      return AUTH_DB_FAILURE;
   }

   while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
      auth_user_summary_t user = { 0 };
      user.id = sqlite3_column_int(stmt, 0);

      const char *uname = (const char *)sqlite3_column_text(stmt, 1);
      if (uname) {
         strncpy(user.username, uname, AUTH_USERNAME_MAX - 1);
         user.username[AUTH_USERNAME_MAX - 1] = '\0';
      }

      user.is_admin = sqlite3_column_int(stmt, 2) != 0;
      user.created_at = (time_t)sqlite3_column_int64(stmt, 3);
      user.last_login = (time_t)sqlite3_column_int64(stmt, 4);
      user.failed_attempts = sqlite3_column_int(stmt, 5);
      user.lockout_until = (time_t)sqlite3_column_int64(stmt, 6);

      if (callback(&user, ctx) != 0) {
         break; /* Callback requested stop */
      }
   }

   sqlite3_finalize(stmt);
   pthread_mutex_unlock(&s_db.mutex);

   return AUTH_DB_SUCCESS;
}

int auth_db_count_admins(void) {
   pthread_mutex_lock(&s_db.mutex);

   if (!s_db.initialized) {
      pthread_mutex_unlock(&s_db.mutex);
      return -1;
   }

   const char *sql = "SELECT COUNT(*) FROM users WHERE is_admin = 1";
   sqlite3_stmt *stmt = NULL;
   int rc = sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      pthread_mutex_unlock(&s_db.mutex);
      return -1;
   }

   int count = -1;
   if (sqlite3_step(stmt) == SQLITE_ROW) {
      count = sqlite3_column_int(stmt, 0);
   }

   sqlite3_finalize(stmt);
   pthread_mutex_unlock(&s_db.mutex);

   return count;
}

int auth_db_delete_user(const char *username) {
   if (!username) {
      return AUTH_DB_INVALID;
   }

   pthread_mutex_lock(&s_db.mutex);

   if (!s_db.initialized) {
      pthread_mutex_unlock(&s_db.mutex);
      return AUTH_DB_FAILURE;
   }

   /* Start transaction for atomicity */
   sqlite3_exec(s_db.db, "BEGIN IMMEDIATE", NULL, NULL, NULL);

   /* Get user info (is_admin, id) */
   const char *sql_get = "SELECT id, is_admin FROM users WHERE username = ?";
   sqlite3_stmt *stmt_get = NULL;
   int rc = sqlite3_prepare_v2(s_db.db, sql_get, -1, &stmt_get, NULL);
   if (rc != SQLITE_OK) {
      sqlite3_exec(s_db.db, "ROLLBACK", NULL, NULL, NULL);
      pthread_mutex_unlock(&s_db.mutex);
      return AUTH_DB_FAILURE;
   }

   sqlite3_bind_text(stmt_get, 1, username, -1, SQLITE_STATIC);
   rc = sqlite3_step(stmt_get);

   if (rc != SQLITE_ROW) {
      sqlite3_finalize(stmt_get);
      sqlite3_exec(s_db.db, "ROLLBACK", NULL, NULL, NULL);
      pthread_mutex_unlock(&s_db.mutex);
      return AUTH_DB_NOT_FOUND;
   }

   int user_id = sqlite3_column_int(stmt_get, 0);
   bool is_admin = sqlite3_column_int(stmt_get, 1) != 0;
   sqlite3_finalize(stmt_get);

   /* If admin, check if this is the last admin */
   if (is_admin) {
      const char *sql_count = "SELECT COUNT(*) FROM users WHERE is_admin = 1";
      sqlite3_stmt *stmt_count = NULL;
      sqlite3_prepare_v2(s_db.db, sql_count, -1, &stmt_count, NULL);
      int admin_count = 0;
      if (sqlite3_step(stmt_count) == SQLITE_ROW) {
         admin_count = sqlite3_column_int(stmt_count, 0);
      }
      sqlite3_finalize(stmt_count);

      if (admin_count <= 1) {
         sqlite3_exec(s_db.db, "ROLLBACK", NULL, NULL, NULL);
         pthread_mutex_unlock(&s_db.mutex);
         return AUTH_DB_LAST_ADMIN;
      }
   }

   /* Delete user's sessions first */
   const char *sql_del_sessions = "DELETE FROM sessions WHERE user_id = ?";
   sqlite3_stmt *stmt_del_sessions = NULL;
   sqlite3_prepare_v2(s_db.db, sql_del_sessions, -1, &stmt_del_sessions, NULL);
   sqlite3_bind_int(stmt_del_sessions, 1, user_id);
   sqlite3_step(stmt_del_sessions);
   sqlite3_finalize(stmt_del_sessions);

   /* Delete the user */
   const char *sql_del = "DELETE FROM users WHERE username = ?";
   sqlite3_stmt *stmt_del = NULL;
   sqlite3_prepare_v2(s_db.db, sql_del, -1, &stmt_del, NULL);
   sqlite3_bind_text(stmt_del, 1, username, -1, SQLITE_STATIC);
   rc = sqlite3_step(stmt_del);
   sqlite3_finalize(stmt_del);

   if (rc != SQLITE_DONE) {
      sqlite3_exec(s_db.db, "ROLLBACK", NULL, NULL, NULL);
      pthread_mutex_unlock(&s_db.mutex);
      return AUTH_DB_FAILURE;
   }

   sqlite3_exec(s_db.db, "COMMIT", NULL, NULL, NULL);
   pthread_mutex_unlock(&s_db.mutex);

   return AUTH_DB_SUCCESS;
}

int auth_db_update_password(const char *username, const char *new_hash) {
   if (!username || !new_hash) {
      return AUTH_DB_INVALID;
   }

   pthread_mutex_lock(&s_db.mutex);

   if (!s_db.initialized) {
      pthread_mutex_unlock(&s_db.mutex);
      return AUTH_DB_FAILURE;
   }

   /* Start transaction for atomicity (password + session invalidation) */
   sqlite3_exec(s_db.db, "BEGIN IMMEDIATE", NULL, NULL, NULL);

   /* Get user ID */
   const char *sql_get = "SELECT id FROM users WHERE username = ?";
   sqlite3_stmt *stmt_get = NULL;
   int rc = sqlite3_prepare_v2(s_db.db, sql_get, -1, &stmt_get, NULL);
   if (rc != SQLITE_OK) {
      sqlite3_exec(s_db.db, "ROLLBACK", NULL, NULL, NULL);
      pthread_mutex_unlock(&s_db.mutex);
      return AUTH_DB_FAILURE;
   }

   sqlite3_bind_text(stmt_get, 1, username, -1, SQLITE_STATIC);
   rc = sqlite3_step(stmt_get);

   if (rc != SQLITE_ROW) {
      sqlite3_finalize(stmt_get);
      sqlite3_exec(s_db.db, "ROLLBACK", NULL, NULL, NULL);
      pthread_mutex_unlock(&s_db.mutex);
      return AUTH_DB_NOT_FOUND;
   }

   int user_id = sqlite3_column_int(stmt_get, 0);
   sqlite3_finalize(stmt_get);

   /* Update password */
   const char *sql_update = "UPDATE users SET password_hash = ? WHERE username = ?";
   sqlite3_stmt *stmt_update = NULL;
   sqlite3_prepare_v2(s_db.db, sql_update, -1, &stmt_update, NULL);
   sqlite3_bind_text(stmt_update, 1, new_hash, -1, SQLITE_STATIC);
   sqlite3_bind_text(stmt_update, 2, username, -1, SQLITE_STATIC);
   rc = sqlite3_step(stmt_update);
   sqlite3_finalize(stmt_update);

   if (rc != SQLITE_DONE) {
      sqlite3_exec(s_db.db, "ROLLBACK", NULL, NULL, NULL);
      pthread_mutex_unlock(&s_db.mutex);
      return AUTH_DB_FAILURE;
   }

   /* Invalidate all sessions for this user */
   const char *sql_del = "DELETE FROM sessions WHERE user_id = ?";
   sqlite3_stmt *stmt_del = NULL;
   sqlite3_prepare_v2(s_db.db, sql_del, -1, &stmt_del, NULL);
   sqlite3_bind_int(stmt_del, 1, user_id);
   sqlite3_step(stmt_del);
   sqlite3_finalize(stmt_del);

   sqlite3_exec(s_db.db, "COMMIT", NULL, NULL, NULL);
   pthread_mutex_unlock(&s_db.mutex);

   return AUTH_DB_SUCCESS;
}

int auth_db_unlock_user(const char *username) {
   if (!username) {
      return AUTH_DB_INVALID;
   }

   pthread_mutex_lock(&s_db.mutex);

   if (!s_db.initialized) {
      pthread_mutex_unlock(&s_db.mutex);
      return AUTH_DB_FAILURE;
   }

   /* Check if user exists first */
   const char *sql_check = "SELECT 1 FROM users WHERE username = ?";
   sqlite3_stmt *stmt_check = NULL;
   int rc = sqlite3_prepare_v2(s_db.db, sql_check, -1, &stmt_check, NULL);
   if (rc != SQLITE_OK) {
      pthread_mutex_unlock(&s_db.mutex);
      return AUTH_DB_FAILURE;
   }

   sqlite3_bind_text(stmt_check, 1, username, -1, SQLITE_STATIC);
   rc = sqlite3_step(stmt_check);
   sqlite3_finalize(stmt_check);

   if (rc != SQLITE_ROW) {
      pthread_mutex_unlock(&s_db.mutex);
      return AUTH_DB_NOT_FOUND;
   }

   /* Unlock: set lockout_until to 0 and reset failed_attempts */
   const char *sql_unlock =
       "UPDATE users SET lockout_until = 0, failed_attempts = 0 WHERE username = ?";
   sqlite3_stmt *stmt_unlock = NULL;
   sqlite3_prepare_v2(s_db.db, sql_unlock, -1, &stmt_unlock, NULL);
   sqlite3_bind_text(stmt_unlock, 1, username, -1, SQLITE_STATIC);
   rc = sqlite3_step(stmt_unlock);
   sqlite3_finalize(stmt_unlock);

   pthread_mutex_unlock(&s_db.mutex);

   return (rc == SQLITE_DONE) ? AUTH_DB_SUCCESS : AUTH_DB_FAILURE;
}

/* ============================================================================
 * User Settings Operations
 * ============================================================================ */

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

   pthread_mutex_lock(&s_db.mutex);

   if (!s_db.initialized) {
      pthread_mutex_unlock(&s_db.mutex);
      return AUTH_DB_FAILURE;
   }

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
   } else if (rc != SQLITE_DONE) {
      /* Unexpected error */
      LOG_ERROR("auth_db: get_user_settings failed: %s", sqlite3_errmsg(s_db.db));
      sqlite3_reset(s_db.stmt_get_user_settings);
      pthread_mutex_unlock(&s_db.mutex);
      return AUTH_DB_FAILURE;
   }
   /* SQLITE_DONE means no row found - return defaults */

   sqlite3_reset(s_db.stmt_get_user_settings);
   pthread_mutex_unlock(&s_db.mutex);
   return AUTH_DB_SUCCESS;
}

int auth_db_set_user_settings(int user_id, const auth_user_settings_t *settings) {
   if (!settings) {
      return AUTH_DB_INVALID;
   }

   pthread_mutex_lock(&s_db.mutex);

   if (!s_db.initialized) {
      pthread_mutex_unlock(&s_db.mutex);
      return AUTH_DB_FAILURE;
   }

   sqlite3_reset(s_db.stmt_set_user_settings);
   sqlite3_bind_int(s_db.stmt_set_user_settings, 1, user_id);
   sqlite3_bind_text(s_db.stmt_set_user_settings, 2, settings->persona_description, -1,
                     SQLITE_STATIC);
   sqlite3_bind_text(s_db.stmt_set_user_settings, 3, settings->persona_mode, -1, SQLITE_STATIC);
   sqlite3_bind_text(s_db.stmt_set_user_settings, 4, settings->location, -1, SQLITE_STATIC);
   sqlite3_bind_text(s_db.stmt_set_user_settings, 5, settings->timezone, -1, SQLITE_STATIC);
   sqlite3_bind_text(s_db.stmt_set_user_settings, 6, settings->units, -1, SQLITE_STATIC);
   sqlite3_bind_text(s_db.stmt_set_user_settings, 7, settings->tts_voice_model, -1, SQLITE_STATIC);
   sqlite3_bind_double(s_db.stmt_set_user_settings, 8, (double)settings->tts_length_scale);
   sqlite3_bind_int64(s_db.stmt_set_user_settings, 9, (int64_t)time(NULL));

   int rc = sqlite3_step(s_db.stmt_set_user_settings);
   sqlite3_reset(s_db.stmt_set_user_settings);

   pthread_mutex_unlock(&s_db.mutex);

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

/* ============================================================================
 * Session Operations
 * ============================================================================ */

int auth_db_create_session(int user_id,
                           const char *token,
                           const char *ip_address,
                           const char *user_agent) {
   if (!token) {
      return AUTH_DB_INVALID;
   }

   pthread_mutex_lock(&s_db.mutex);

   if (!s_db.initialized) {
      pthread_mutex_unlock(&s_db.mutex);
      return AUTH_DB_FAILURE;
   }

   time_t now = time(NULL);

   sqlite3_reset(s_db.stmt_create_session);
   sqlite3_bind_text(s_db.stmt_create_session, 1, token, -1, SQLITE_STATIC);
   sqlite3_bind_int(s_db.stmt_create_session, 2, user_id);
   sqlite3_bind_int64(s_db.stmt_create_session, 3, (int64_t)now);
   sqlite3_bind_int64(s_db.stmt_create_session, 4, (int64_t)now);

   if (ip_address) {
      sqlite3_bind_text(s_db.stmt_create_session, 5, ip_address, -1, SQLITE_STATIC);
   } else {
      sqlite3_bind_null(s_db.stmt_create_session, 5);
   }

   if (user_agent) {
      /* Truncate user agent to max length */
      size_t ua_len = strlen(user_agent);
      if (ua_len >= AUTH_USER_AGENT_MAX) {
         ua_len = AUTH_USER_AGENT_MAX - 1;
      }
      sqlite3_bind_text(s_db.stmt_create_session, 6, user_agent, (int)ua_len, SQLITE_STATIC);
   } else {
      sqlite3_bind_null(s_db.stmt_create_session, 6);
   }

   int rc = sqlite3_step(s_db.stmt_create_session);
   sqlite3_reset(s_db.stmt_create_session);

   pthread_mutex_unlock(&s_db.mutex);

   return (rc == SQLITE_DONE) ? AUTH_DB_SUCCESS : AUTH_DB_FAILURE;
}

int auth_db_get_session(const char *token, auth_session_t *session_out) {
   if (!token || !session_out) {
      return AUTH_DB_INVALID;
   }

   pthread_mutex_lock(&s_db.mutex);

   if (!s_db.initialized) {
      pthread_mutex_unlock(&s_db.mutex);
      return AUTH_DB_FAILURE;
   }

   /* Note: Cleanup is now handled by the background maintenance thread
    * (auth_maintenance.c) rather than lazily during session lookups.
    * This avoids conflicts between concurrent cleanup attempts. */

   sqlite3_reset(s_db.stmt_get_session);
   sqlite3_bind_text(s_db.stmt_get_session, 1, token, -1, SQLITE_STATIC);

   int rc = sqlite3_step(s_db.stmt_get_session);

   if (rc == SQLITE_ROW) {
      const char *tok = (const char *)sqlite3_column_text(s_db.stmt_get_session, 0);
      if (tok) {
         strncpy(session_out->token, tok, AUTH_TOKEN_LEN - 1);
         session_out->token[AUTH_TOKEN_LEN - 1] = '\0';
      }

      session_out->user_id = sqlite3_column_int(s_db.stmt_get_session, 1);

      const char *uname = (const char *)sqlite3_column_text(s_db.stmt_get_session, 2);
      if (uname) {
         strncpy(session_out->username, uname, AUTH_USERNAME_MAX - 1);
         session_out->username[AUTH_USERNAME_MAX - 1] = '\0';
      }

      session_out->is_admin = sqlite3_column_int(s_db.stmt_get_session, 3) != 0;
      session_out->created_at = (time_t)sqlite3_column_int64(s_db.stmt_get_session, 4);
      session_out->last_activity = (time_t)sqlite3_column_int64(s_db.stmt_get_session, 5);

      const char *ip = (const char *)sqlite3_column_text(s_db.stmt_get_session, 6);
      if (ip) {
         strncpy(session_out->ip_address, ip, AUTH_IP_MAX - 1);
         session_out->ip_address[AUTH_IP_MAX - 1] = '\0';
      } else {
         session_out->ip_address[0] = '\0';
      }

      const char *ua = (const char *)sqlite3_column_text(s_db.stmt_get_session, 7);
      if (ua) {
         strncpy(session_out->user_agent, ua, AUTH_USER_AGENT_MAX - 1);
         session_out->user_agent[AUTH_USER_AGENT_MAX - 1] = '\0';
      } else {
         session_out->user_agent[0] = '\0';
      }

      sqlite3_reset(s_db.stmt_get_session);
      pthread_mutex_unlock(&s_db.mutex);
      return AUTH_DB_SUCCESS;
   }

   sqlite3_reset(s_db.stmt_get_session);
   pthread_mutex_unlock(&s_db.mutex);

   if (rc == SQLITE_DONE) {
      return AUTH_DB_NOT_FOUND;
   }

   LOG_ERROR("auth_db_get_session: failed: %s", sqlite3_errmsg(s_db.db));
   return AUTH_DB_FAILURE;
}

int auth_db_update_session_activity(const char *token) {
   if (!token) {
      return AUTH_DB_INVALID;
   }

   pthread_mutex_lock(&s_db.mutex);

   if (!s_db.initialized) {
      pthread_mutex_unlock(&s_db.mutex);
      return AUTH_DB_FAILURE;
   }

   sqlite3_reset(s_db.stmt_update_session_activity);
   sqlite3_bind_int64(s_db.stmt_update_session_activity, 1, (int64_t)time(NULL));
   sqlite3_bind_text(s_db.stmt_update_session_activity, 2, token, -1, SQLITE_STATIC);

   int rc = sqlite3_step(s_db.stmt_update_session_activity);
   sqlite3_reset(s_db.stmt_update_session_activity);

   pthread_mutex_unlock(&s_db.mutex);

   return (rc == SQLITE_DONE) ? AUTH_DB_SUCCESS : AUTH_DB_FAILURE;
}

int auth_db_delete_session(const char *token) {
   if (!token) {
      return AUTH_DB_INVALID;
   }

   pthread_mutex_lock(&s_db.mutex);

   if (!s_db.initialized) {
      pthread_mutex_unlock(&s_db.mutex);
      return AUTH_DB_FAILURE;
   }

   sqlite3_reset(s_db.stmt_delete_session);
   sqlite3_bind_text(s_db.stmt_delete_session, 1, token, -1, SQLITE_STATIC);

   int rc = sqlite3_step(s_db.stmt_delete_session);
   sqlite3_reset(s_db.stmt_delete_session);

   pthread_mutex_unlock(&s_db.mutex);

   return (rc == SQLITE_DONE) ? AUTH_DB_SUCCESS : AUTH_DB_FAILURE;
}

int auth_db_delete_session_by_prefix(const char *prefix) {
   if (!prefix || strlen(prefix) < 8) {
      return AUTH_DB_INVALID;
   }

   pthread_mutex_lock(&s_db.mutex);

   if (!s_db.initialized) {
      pthread_mutex_unlock(&s_db.mutex);
      return AUTH_DB_FAILURE;
   }

   /* Find full token matching prefix (use SUBSTR for exact matching, not LIKE) */
   const char *find_sql = "SELECT token FROM sessions WHERE substr(token, 1, 8) = ? LIMIT 1";
   sqlite3_stmt *stmt = NULL;
   int rc = sqlite3_prepare_v2(s_db.db, find_sql, -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      pthread_mutex_unlock(&s_db.mutex);
      return AUTH_DB_FAILURE;
   }

   /* Bind only the first 8 characters */
   char prefix_buf[9] = { 0 };
   strncpy(prefix_buf, prefix, 8);
   sqlite3_bind_text(stmt, 1, prefix_buf, 8, SQLITE_STATIC);

   rc = sqlite3_step(stmt);
   if (rc != SQLITE_ROW) {
      sqlite3_finalize(stmt);
      pthread_mutex_unlock(&s_db.mutex);
      return AUTH_DB_NOT_FOUND;
   }

   /* Get full token and delete it */
   const char *full_token = (const char *)sqlite3_column_text(stmt, 0);
   if (!full_token) {
      sqlite3_finalize(stmt);
      pthread_mutex_unlock(&s_db.mutex);
      return AUTH_DB_FAILURE;
   }

   /* Delete the session */
   sqlite3_reset(s_db.stmt_delete_session);
   sqlite3_bind_text(s_db.stmt_delete_session, 1, full_token, -1, SQLITE_TRANSIENT);
   rc = sqlite3_step(s_db.stmt_delete_session);
   sqlite3_reset(s_db.stmt_delete_session);
   sqlite3_finalize(stmt);

   pthread_mutex_unlock(&s_db.mutex);

   return (rc == SQLITE_DONE) ? AUTH_DB_SUCCESS : AUTH_DB_FAILURE;
}

int auth_db_delete_sessions_by_username(const char *username) {
   if (!username) {
      return -1;
   }

   /* Look up user to get their ID */
   auth_user_t user;
   int rc = auth_db_get_user(username, &user);
   if (rc != AUTH_DB_SUCCESS) {
      return (rc == AUTH_DB_NOT_FOUND) ? 0 : -1;
   }

   return auth_db_delete_user_sessions(user.id);
}

int auth_db_delete_user_sessions(int user_id) {
   pthread_mutex_lock(&s_db.mutex);

   if (!s_db.initialized) {
      pthread_mutex_unlock(&s_db.mutex);
      return -1;
   }

   sqlite3_reset(s_db.stmt_delete_user_sessions);
   sqlite3_bind_int(s_db.stmt_delete_user_sessions, 1, user_id);

   int rc = sqlite3_step(s_db.stmt_delete_user_sessions);
   int changes = sqlite3_changes(s_db.db);
   sqlite3_reset(s_db.stmt_delete_user_sessions);

   pthread_mutex_unlock(&s_db.mutex);

   return (rc == SQLITE_DONE) ? changes : -1;
}

int auth_db_list_sessions(auth_session_summary_callback_t callback, void *ctx) {
   if (!callback) {
      return AUTH_DB_INVALID;
   }

   pthread_mutex_lock(&s_db.mutex);

   if (!s_db.initialized) {
      pthread_mutex_unlock(&s_db.mutex);
      return AUTH_DB_FAILURE;
   }

   const char *sql = "SELECT s.token, s.user_id, u.username, s.created_at, "
                     "s.last_activity, s.ip_address "
                     "FROM sessions s "
                     "JOIN users u ON s.user_id = u.id "
                     "ORDER BY s.last_activity DESC";
   sqlite3_stmt *stmt = NULL;
   int rc = sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      pthread_mutex_unlock(&s_db.mutex);
      return AUTH_DB_FAILURE;
   }

   while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
      auth_session_summary_t session = { 0 };

      /* Only copy token prefix (8 chars) for security */
      const char *tok = (const char *)sqlite3_column_text(stmt, 0);
      if (tok) {
         strncpy(session.token_prefix, tok, 8);
         session.token_prefix[8] = '\0';
      }

      session.user_id = sqlite3_column_int(stmt, 1);

      const char *uname = (const char *)sqlite3_column_text(stmt, 2);
      if (uname) {
         strncpy(session.username, uname, AUTH_USERNAME_MAX - 1);
         session.username[AUTH_USERNAME_MAX - 1] = '\0';
      }

      session.created_at = (time_t)sqlite3_column_int64(stmt, 3);
      session.last_activity = (time_t)sqlite3_column_int64(stmt, 4);

      const char *ip = (const char *)sqlite3_column_text(stmt, 5);
      if (ip) {
         strncpy(session.ip_address, ip, AUTH_IP_MAX - 1);
         session.ip_address[AUTH_IP_MAX - 1] = '\0';
      }

      if (callback(&session, ctx) != 0) {
         break; /* Callback requested stop */
      }
   }

   sqlite3_finalize(stmt);
   pthread_mutex_unlock(&s_db.mutex);

   return AUTH_DB_SUCCESS;
}

int auth_db_count_sessions(void) {
   pthread_mutex_lock(&s_db.mutex);

   if (!s_db.initialized) {
      pthread_mutex_unlock(&s_db.mutex);
      return -1;
   }

   const char *sql = "SELECT COUNT(*) FROM sessions";
   sqlite3_stmt *stmt = NULL;
   int rc = sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      pthread_mutex_unlock(&s_db.mutex);
      return -1;
   }

   int count = -1;
   if (sqlite3_step(stmt) == SQLITE_ROW) {
      count = sqlite3_column_int(stmt, 0);
   }

   sqlite3_finalize(stmt);
   pthread_mutex_unlock(&s_db.mutex);

   return count;
}

/* ============================================================================
 * Rate Limiting
 * ============================================================================ */

int auth_db_count_recent_failures(const char *ip_address, time_t since) {
   if (!ip_address) {
      return -1;
   }

   pthread_mutex_lock(&s_db.mutex);

   if (!s_db.initialized) {
      pthread_mutex_unlock(&s_db.mutex);
      return -1;
   }

   sqlite3_reset(s_db.stmt_count_recent_failures);
   sqlite3_bind_text(s_db.stmt_count_recent_failures, 1, ip_address, -1, SQLITE_STATIC);
   sqlite3_bind_int64(s_db.stmt_count_recent_failures, 2, (int64_t)since);

   int count = -1;
   int rc = sqlite3_step(s_db.stmt_count_recent_failures);
   if (rc == SQLITE_ROW) {
      count = sqlite3_column_int(s_db.stmt_count_recent_failures, 0);
   }

   sqlite3_reset(s_db.stmt_count_recent_failures);
   pthread_mutex_unlock(&s_db.mutex);

   return count;
}

int auth_db_log_attempt(const char *ip_address, const char *username, bool success) {
   if (!ip_address) {
      return AUTH_DB_INVALID;
   }

   pthread_mutex_lock(&s_db.mutex);

   if (!s_db.initialized) {
      pthread_mutex_unlock(&s_db.mutex);
      return AUTH_DB_FAILURE;
   }

   sqlite3_reset(s_db.stmt_log_attempt);
   sqlite3_bind_text(s_db.stmt_log_attempt, 1, ip_address, -1, SQLITE_STATIC);

   if (username) {
      sqlite3_bind_text(s_db.stmt_log_attempt, 2, username, -1, SQLITE_STATIC);
   } else {
      sqlite3_bind_null(s_db.stmt_log_attempt, 2);
   }

   sqlite3_bind_int64(s_db.stmt_log_attempt, 3, (int64_t)time(NULL));
   sqlite3_bind_int(s_db.stmt_log_attempt, 4, success ? 1 : 0);

   int rc = sqlite3_step(s_db.stmt_log_attempt);
   sqlite3_reset(s_db.stmt_log_attempt);

   pthread_mutex_unlock(&s_db.mutex);

   return (rc == SQLITE_DONE) ? AUTH_DB_SUCCESS : AUTH_DB_FAILURE;
}

int auth_db_clear_login_attempts(const char *ip_address) {
   pthread_mutex_lock(&s_db.mutex);

   if (!s_db.initialized) {
      pthread_mutex_unlock(&s_db.mutex);
      return -1;
   }

   int deleted = 0;
   sqlite3_stmt *stmt = NULL;
   int rc;

   if (ip_address) {
      /* Delete attempts for specific IP */
      rc = sqlite3_prepare_v2(s_db.db, "DELETE FROM login_attempts WHERE ip_address = ?", -1, &stmt,
                              NULL);
      if (rc != SQLITE_OK) {
         LOG_ERROR("auth_db: prepare clear_login_attempts failed: %s", sqlite3_errmsg(s_db.db));
         pthread_mutex_unlock(&s_db.mutex);
         return -1;
      }
      sqlite3_bind_text(stmt, 1, ip_address, -1, SQLITE_STATIC);
   } else {
      /* Delete all attempts */
      rc = sqlite3_prepare_v2(s_db.db, "DELETE FROM login_attempts", -1, &stmt, NULL);
      if (rc != SQLITE_OK) {
         LOG_ERROR("auth_db: prepare clear_all_login_attempts failed: %s", sqlite3_errmsg(s_db.db));
         pthread_mutex_unlock(&s_db.mutex);
         return -1;
      }
   }

   rc = sqlite3_step(stmt);
   if (rc == SQLITE_DONE) {
      deleted = sqlite3_changes(s_db.db);
   }
   sqlite3_finalize(stmt);

   pthread_mutex_unlock(&s_db.mutex);

   LOG_INFO("auth_db: Cleared %d login attempts for IP: %s", deleted,
            ip_address ? ip_address : "all");
   return deleted;
}

int auth_db_list_blocked_ips(time_t since, auth_ip_status_callback_t callback, void *ctx) {
   if (!callback) {
      return AUTH_DB_INVALID;
   }

   pthread_mutex_lock(&s_db.mutex);

   if (!s_db.initialized) {
      pthread_mutex_unlock(&s_db.mutex);
      return AUTH_DB_FAILURE;
   }

   /* Query IPs with failed attempts, grouped by IP, ordered by attempt count descending */
   const char *sql = "SELECT ip_address, COUNT(*) as attempt_count, MAX(timestamp) as last_attempt "
                     "FROM login_attempts "
                     "WHERE success = 0 AND timestamp > ? "
                     "GROUP BY ip_address "
                     "ORDER BY attempt_count DESC "
                     "LIMIT 100";

   sqlite3_stmt *stmt = NULL;
   int rc = sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      LOG_ERROR("auth_db: prepare list_blocked_ips failed: %s", sqlite3_errmsg(s_db.db));
      pthread_mutex_unlock(&s_db.mutex);
      return AUTH_DB_FAILURE;
   }

   sqlite3_bind_int64(stmt, 1, (int64_t)since);

   while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
      auth_ip_status_t status = { 0 };

      const char *ip = (const char *)sqlite3_column_text(stmt, 0);
      if (ip) {
         strncpy(status.ip_address, ip, sizeof(status.ip_address) - 1);
      }
      status.failed_attempts = sqlite3_column_int(stmt, 1);
      status.last_attempt = (time_t)sqlite3_column_int64(stmt, 2);

      /* Call callback with mutex still held - callback should be quick */
      if (callback(&status, ctx) != 0) {
         break;
      }
   }

   sqlite3_finalize(stmt);
   pthread_mutex_unlock(&s_db.mutex);

   return AUTH_DB_SUCCESS;
}

/* ============================================================================
 * Audit Logging
 * ============================================================================ */

void auth_db_log_event(const char *event,
                       const char *username,
                       const char *ip_address,
                       const char *details) {
   if (!event) {
      return;
   }

   pthread_mutex_lock(&s_db.mutex);

   if (!s_db.initialized) {
      pthread_mutex_unlock(&s_db.mutex);
      return;
   }

   sqlite3_reset(s_db.stmt_log_event);
   sqlite3_bind_int64(s_db.stmt_log_event, 1, (int64_t)time(NULL));
   sqlite3_bind_text(s_db.stmt_log_event, 2, event, -1, SQLITE_STATIC);

   if (username) {
      sqlite3_bind_text(s_db.stmt_log_event, 3, username, -1, SQLITE_STATIC);
   } else {
      sqlite3_bind_null(s_db.stmt_log_event, 3);
   }

   if (ip_address) {
      sqlite3_bind_text(s_db.stmt_log_event, 4, ip_address, -1, SQLITE_STATIC);
   } else {
      sqlite3_bind_null(s_db.stmt_log_event, 4);
   }

   if (details) {
      sqlite3_bind_text(s_db.stmt_log_event, 5, details, -1, SQLITE_STATIC);
   } else {
      sqlite3_bind_null(s_db.stmt_log_event, 5);
   }

   sqlite3_step(s_db.stmt_log_event);
   sqlite3_reset(s_db.stmt_log_event);

   pthread_mutex_unlock(&s_db.mutex);
}

int auth_db_query_audit_log(const auth_log_filter_t *filter,
                            auth_log_callback_t callback,
                            void *ctx) {
   if (!callback) {
      return AUTH_DB_INVALID;
   }

   pthread_mutex_lock(&s_db.mutex);

   if (!s_db.initialized || !s_db.db) {
      pthread_mutex_unlock(&s_db.mutex);
      return AUTH_DB_FAILURE;
   }

   /* Build dynamic SQL query based on filters */
   char sql[512];
   int sql_len = snprintf(sql, sizeof(sql),
                          "SELECT timestamp, event, username, ip_address, details "
                          "FROM auth_log WHERE 1=1");

   /* Apply filters */
   time_t since = 0, until = 0;
   const char *event_filter = NULL;
   const char *user_filter = NULL;
   int limit = AUTH_LOG_DEFAULT_LIMIT;
   int offset = 0;

   if (filter) {
      since = filter->since;
      until = filter->until;
      event_filter = filter->event;
      user_filter = filter->username;
      limit = (filter->limit > 0) ? filter->limit : AUTH_LOG_DEFAULT_LIMIT;
      if (limit > AUTH_LOG_MAX_LIMIT)
         limit = AUTH_LOG_MAX_LIMIT;
      offset = (filter->offset > 0) ? filter->offset : 0;
   }

   if (since > 0) {
      sql_len += snprintf(sql + sql_len, sizeof(sql) - sql_len, " AND timestamp >= ?");
   }
   if (until > 0) {
      sql_len += snprintf(sql + sql_len, sizeof(sql) - sql_len, " AND timestamp <= ?");
   }
   if (event_filter) {
      sql_len += snprintf(sql + sql_len, sizeof(sql) - sql_len, " AND event = ?");
   }
   if (user_filter) {
      sql_len += snprintf(sql + sql_len, sizeof(sql) - sql_len, " AND username = ?");
   }

   sql_len += snprintf(sql + sql_len, sizeof(sql) - sql_len,
                       " ORDER BY timestamp DESC LIMIT ? OFFSET ?");

   sqlite3_stmt *stmt = NULL;
   int rc = sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      pthread_mutex_unlock(&s_db.mutex);
      return AUTH_DB_FAILURE;
   }

   /* Bind parameters */
   int param = 1;
   if (since > 0) {
      sqlite3_bind_int64(stmt, param++, (int64_t)since);
   }
   if (until > 0) {
      sqlite3_bind_int64(stmt, param++, (int64_t)until);
   }
   if (event_filter) {
      sqlite3_bind_text(stmt, param++, event_filter, -1, SQLITE_STATIC);
   }
   if (user_filter) {
      sqlite3_bind_text(stmt, param++, user_filter, -1, SQLITE_STATIC);
   }
   sqlite3_bind_int(stmt, param++, limit);
   sqlite3_bind_int(stmt, param++, offset);

   /* Execute and call callback for each row */
   while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
      auth_log_entry_t entry = { 0 };

      entry.timestamp = (time_t)sqlite3_column_int64(stmt, 0);

      const char *ev = (const char *)sqlite3_column_text(stmt, 1);
      if (ev) {
         strncpy(entry.event, ev, sizeof(entry.event) - 1);
      }

      const char *user = (const char *)sqlite3_column_text(stmt, 2);
      if (user) {
         strncpy(entry.username, user, sizeof(entry.username) - 1);
      }

      const char *ip = (const char *)sqlite3_column_text(stmt, 3);
      if (ip) {
         strncpy(entry.ip_address, ip, sizeof(entry.ip_address) - 1);
      }

      const char *details = (const char *)sqlite3_column_text(stmt, 4);
      if (details) {
         strncpy(entry.details, details, sizeof(entry.details) - 1);
      }

      if (callback(&entry, ctx) != 0) {
         break;
      }
   }

   sqlite3_finalize(stmt);
   pthread_mutex_unlock(&s_db.mutex);

   return AUTH_DB_SUCCESS;
}

/* ============================================================================
 * Maintenance
 * ============================================================================ */

int auth_db_run_cleanup(void) {
   pthread_mutex_lock(&s_db.mutex);

   if (!s_db.initialized) {
      pthread_mutex_unlock(&s_db.mutex);
      return AUTH_DB_FAILURE;
   }

   time_t now = time(NULL);
   time_t session_cutoff = now - AUTH_SESSION_TIMEOUT_SEC;
   time_t attempt_cutoff = now - LOGIN_ATTEMPT_RETENTION_SEC;
   time_t log_cutoff = now - AUTH_LOG_RETENTION_SEC;

   sqlite3_reset(s_db.stmt_delete_expired_sessions);
   sqlite3_bind_int64(s_db.stmt_delete_expired_sessions, 1, (int64_t)session_cutoff);
   sqlite3_step(s_db.stmt_delete_expired_sessions);
   sqlite3_reset(s_db.stmt_delete_expired_sessions);

   sqlite3_reset(s_db.stmt_delete_old_attempts);
   sqlite3_bind_int64(s_db.stmt_delete_old_attempts, 1, (int64_t)attempt_cutoff);
   sqlite3_step(s_db.stmt_delete_old_attempts);
   sqlite3_reset(s_db.stmt_delete_old_attempts);

   sqlite3_reset(s_db.stmt_delete_old_logs);
   sqlite3_bind_int64(s_db.stmt_delete_old_logs, 1, (int64_t)log_cutoff);
   sqlite3_step(s_db.stmt_delete_old_logs);
   sqlite3_reset(s_db.stmt_delete_old_logs);

   s_db.last_cleanup = now;

   pthread_mutex_unlock(&s_db.mutex);

   return AUTH_DB_SUCCESS;
}

int auth_db_checkpoint(void) {
   pthread_mutex_lock(&s_db.mutex);

   if (!s_db.initialized || !s_db.db) {
      pthread_mutex_unlock(&s_db.mutex);
      return AUTH_DB_FAILURE;
   }

   int rc = sqlite3_wal_checkpoint_v2(s_db.db, NULL, SQLITE_CHECKPOINT_TRUNCATE, NULL, NULL);

   pthread_mutex_unlock(&s_db.mutex);

   return (rc == SQLITE_OK) ? AUTH_DB_SUCCESS : AUTH_DB_FAILURE;
}

int auth_db_checkpoint_passive(void) {
   pthread_mutex_lock(&s_db.mutex);

   if (!s_db.initialized || !s_db.db) {
      pthread_mutex_unlock(&s_db.mutex);
      return AUTH_DB_FAILURE;
   }

   /* PASSIVE: Checkpoint as much as possible without waiting */
   int rc = sqlite3_wal_checkpoint_v2(s_db.db, NULL, SQLITE_CHECKPOINT_PASSIVE, NULL, NULL);

   pthread_mutex_unlock(&s_db.mutex);

   return (rc == SQLITE_OK) ? AUTH_DB_SUCCESS : AUTH_DB_FAILURE;
}

/* ============================================================================
 * Statistics and Database Management
 * ============================================================================ */

/* Vacuum rate limit: once per 24 hours */
#define VACUUM_COOLDOWN_SEC (24 * 60 * 60)
static time_t s_last_vacuum = 0;

int auth_db_get_stats(auth_db_stats_t *stats) {
   if (!stats) {
      return AUTH_DB_INVALID;
   }

   memset(stats, 0, sizeof(*stats));

   pthread_mutex_lock(&s_db.mutex);

   if (!s_db.initialized || !s_db.db) {
      pthread_mutex_unlock(&s_db.mutex);
      return AUTH_DB_FAILURE;
   }

   /* Combined query for all stats - reduces database round trips */
   const char *sql = "SELECT "
                     "(SELECT COUNT(*) FROM users), "
                     "(SELECT COUNT(*) FROM users WHERE is_admin = 1), "
                     "(SELECT COUNT(*) FROM users WHERE lockout_until > strftime('%s','now')), "
                     "(SELECT COUNT(*) FROM sessions), "
                     "(SELECT COUNT(*) FROM login_attempts "
                     " WHERE success = 0 AND timestamp > strftime('%s','now') - 86400), "
                     "(SELECT COUNT(*) FROM auth_log), "
                     "(SELECT page_count * page_size FROM pragma_page_count(), pragma_page_size())";

   sqlite3_stmt *stmt = NULL;
   int rc = sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      pthread_mutex_unlock(&s_db.mutex);
      LOG_ERROR("Failed to prepare stats query: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   if (sqlite3_step(stmt) == SQLITE_ROW) {
      stats->user_count = sqlite3_column_int(stmt, 0);
      stats->admin_count = sqlite3_column_int(stmt, 1);
      stats->locked_user_count = sqlite3_column_int(stmt, 2);
      stats->session_count = sqlite3_column_int(stmt, 3);
      stats->failed_attempts_24h = sqlite3_column_int(stmt, 4);
      stats->audit_log_count = sqlite3_column_int(stmt, 5);
      stats->db_size_bytes = sqlite3_column_int64(stmt, 6);
   }
   sqlite3_finalize(stmt);

   pthread_mutex_unlock(&s_db.mutex);

   return AUTH_DB_SUCCESS;
}

int auth_db_vacuum(void) {
   pthread_mutex_lock(&s_db.mutex);

   if (!s_db.initialized || !s_db.db) {
      pthread_mutex_unlock(&s_db.mutex);
      return AUTH_DB_FAILURE;
   }

   /* Rate limit: once per 24 hours */
   time_t now = time(NULL);
   if (s_last_vacuum > 0 && (now - s_last_vacuum) < VACUUM_COOLDOWN_SEC) {
      pthread_mutex_unlock(&s_db.mutex);
      return AUTH_DB_RATE_LIMITED;
   }

   int rc = sqlite3_exec(s_db.db, "VACUUM", NULL, NULL, NULL);
   if (rc == SQLITE_OK) {
      s_last_vacuum = now;
   }

   pthread_mutex_unlock(&s_db.mutex);

   return (rc == SQLITE_OK) ? AUTH_DB_SUCCESS : AUTH_DB_FAILURE;
}

/**
 * @brief Check if a path is within one of the allowed backup directories.
 *
 * Resolves the parent directory of the path to its canonical form and checks
 * against a list of allowed directory prefixes.
 *
 * @param path Path to validate
 * @return 0 if path is allowed, non-zero otherwise
 */
static int validate_backup_path(const char *path) {
   /* Allowed backup directory prefixes */
   static const char *allowed_prefixes[] = { "/var/lib/dawn/", /* Main Dawn data directory */
                                             "/tmp/",          /* Temporary files */
                                             "/home/",         /* User home directories */
                                             NULL };

   if (!path || path[0] != '/') {
      /* Only absolute paths allowed */
      return -1;
   }

   /* Check for ".." path traversal */
   if (strstr(path, "..") != NULL) {
      return -1;
   }

   /* Get parent directory of the target path */
   char parent[PATH_MAX];
   strncpy(parent, path, sizeof(parent) - 1);
   parent[sizeof(parent) - 1] = '\0';

   char *last_slash = strrchr(parent, '/');
   if (!last_slash || last_slash == parent) {
      /* Root directory or invalid path */
      return -1;
   }
   *last_slash = '\0';

   /* Resolve to canonical path (follow symlinks) */
   char resolved[PATH_MAX];
   if (!realpath(parent, resolved)) {
      /* Parent directory doesn't exist or error resolving */
      return -1;
   }

   /* Add trailing slash for prefix matching */
   size_t len = strlen(resolved);
   if (len < sizeof(resolved) - 1 && resolved[len - 1] != '/') {
      resolved[len] = '/';
      resolved[len + 1] = '\0';
   }

   /* Check against allowlist */
   for (const char **prefix = allowed_prefixes; *prefix != NULL; prefix++) {
      if (strncmp(resolved, *prefix, strlen(*prefix)) == 0) {
         return 0; /* Path is allowed */
      }
      /* Also check if resolved path equals the prefix without trailing slash */
      size_t prefix_len = strlen(*prefix);
      if (prefix_len > 0 && (*prefix)[prefix_len - 1] == '/') {
         if (strncmp(resolved, *prefix, prefix_len - 1) == 0 &&
             (resolved[prefix_len - 1] == '/' || resolved[prefix_len - 1] == '\0')) {
            return 0;
         }
      }
   }

   return -1; /* Path not in allowlist */
}

int auth_db_backup(const char *dest_path) {
   if (!dest_path) {
      return AUTH_DB_INVALID;
   }

   /* Validate path against allowlist */
   if (validate_backup_path(dest_path) != 0) {
      LOG_WARNING("Backup path not in allowed directories: %s", dest_path);
      return AUTH_DB_FAILURE;
   }

   pthread_mutex_lock(&s_db.mutex);

   if (!s_db.initialized || !s_db.db) {
      pthread_mutex_unlock(&s_db.mutex);
      return AUTH_DB_FAILURE;
   }

   /* Create destination file with secure permissions.
    * O_NOFOLLOW prevents symlink attacks (TOCTOU race condition). */
   mode_t old_umask = umask(0077);
   int fd = open(dest_path, O_CREAT | O_EXCL | O_WRONLY | O_NOFOLLOW, 0600);
   umask(old_umask);

   if (fd < 0) {
      LOG_WARNING("Failed to create backup file: %s (%s)", dest_path, strerror(errno));
      pthread_mutex_unlock(&s_db.mutex);
      return AUTH_DB_FAILURE;
   }
   close(fd);

   /* Open destination database */
   sqlite3 *dest_db = NULL;
   int rc = sqlite3_open(dest_path, &dest_db);
   if (rc != SQLITE_OK) {
      LOG_WARNING("Failed to open backup database: %s", sqlite3_errmsg(dest_db));
      unlink(dest_path);
      sqlite3_close(dest_db);
      pthread_mutex_unlock(&s_db.mutex);
      return AUTH_DB_FAILURE;
   }

   /* Start backup */
   sqlite3_backup *backup = sqlite3_backup_init(dest_db, "main", s_db.db, "main");
   if (!backup) {
      LOG_WARNING("Failed to initialize backup: %s", sqlite3_errmsg(dest_db));
      sqlite3_close(dest_db);
      unlink(dest_path);
      pthread_mutex_unlock(&s_db.mutex);
      return AUTH_DB_FAILURE;
   }

   /* Step backup: 100 pages at a time, 10ms yield between steps */
   do {
      rc = sqlite3_backup_step(backup, 100);
      if (rc == SQLITE_BUSY || rc == SQLITE_LOCKED) {
         sqlite3_sleep(10);
      }
   } while (rc == SQLITE_OK || rc == SQLITE_BUSY || rc == SQLITE_LOCKED);

   sqlite3_backup_finish(backup);

   int result = AUTH_DB_SUCCESS;
   if (rc != SQLITE_DONE) {
      LOG_WARNING("Backup failed: %s", sqlite3_errmsg(dest_db));
      unlink(dest_path);
      result = AUTH_DB_FAILURE;
   }

   sqlite3_close(dest_db);

   /* Verify final permissions */
   struct stat st;
   if (result == AUTH_DB_SUCCESS && stat(dest_path, &st) == 0) {
      if ((st.st_mode & 0777) != 0600) {
         chmod(dest_path, 0600);
      }
   }

   pthread_mutex_unlock(&s_db.mutex);

   return result;
}
