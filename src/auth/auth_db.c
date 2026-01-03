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
#include <libgen.h>
#include <pthread.h>
#include <sqlite3.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "logging.h"

/* Current schema version */
#define SCHEMA_VERSION 1

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
    "CREATE INDEX IF NOT EXISTS idx_log_timestamp ON auth_log(timestamp);";

static int create_schema(void) {
   char *errmsg = NULL;

   /* Execute schema SQL (safe - no user input) */
   int rc = sqlite3_exec(s_db.db, SCHEMA_SQL, NULL, NULL, &errmsg);
   if (rc != SQLITE_OK) {
      LOG_ERROR("auth_db: schema creation failed: %s", errmsg ? errmsg : "unknown");
      sqlite3_free(errmsg);
      return AUTH_DB_FAILURE;
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

   /* Clear all pointers */
   memset(&s_db.stmt_create_user, 0,
          (char *)&s_db.stmt_delete_old_logs - (char *)&s_db.stmt_create_user +
              sizeof(s_db.stmt_delete_old_logs));
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
   sqlite3_exec(s_db.db, "PRAGMA cache_size=16", NULL, NULL, NULL);

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

   /* Lazy cleanup: run if cleanup interval has passed */
   time_t now = time(NULL);
   if (now - s_db.last_cleanup > AUTH_CLEANUP_INTERVAL_SEC) {
      /* Run cleanup while we have the lock */
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
   }

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
