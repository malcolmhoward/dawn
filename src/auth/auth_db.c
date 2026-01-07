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
#define SCHEMA_VERSION 9

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

   /* Conversation statements */
   sqlite3_stmt *stmt_conv_create;
   sqlite3_stmt *stmt_conv_get;
   sqlite3_stmt *stmt_conv_list;
   sqlite3_stmt *stmt_conv_list_all;
   sqlite3_stmt *stmt_conv_search;
   sqlite3_stmt *stmt_conv_search_content;
   sqlite3_stmt *stmt_conv_rename;
   sqlite3_stmt *stmt_conv_delete;
   sqlite3_stmt *stmt_conv_delete_admin;
   sqlite3_stmt *stmt_conv_count;
   sqlite3_stmt *stmt_msg_add;
   sqlite3_stmt *stmt_msg_get;
   sqlite3_stmt *stmt_msg_get_admin;
   sqlite3_stmt *stmt_conv_update_meta;
   sqlite3_stmt *stmt_conv_update_context;

   /* Session metrics statements */
   sqlite3_stmt *stmt_metrics_save;
   sqlite3_stmt *stmt_metrics_update;
   sqlite3_stmt *stmt_metrics_delete_old;
   sqlite3_stmt *stmt_provider_metrics_save;
   sqlite3_stmt *stmt_provider_metrics_delete;
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
    "   theme TEXT DEFAULT 'cyan',"
    "   updated_at INTEGER NOT NULL,"
    "   FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE"
    ");"

    /* Conversations table (added in schema v4, context columns in v5, continuation in v7) */
    "CREATE TABLE IF NOT EXISTS conversations ("
    "   id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "   user_id INTEGER NOT NULL,"
    "   title TEXT NOT NULL DEFAULT 'New Conversation',"
    "   created_at INTEGER NOT NULL,"
    "   updated_at INTEGER NOT NULL,"
    "   message_count INTEGER DEFAULT 0,"
    "   is_archived INTEGER DEFAULT 0,"
    "   context_tokens INTEGER DEFAULT 0,"
    "   context_max INTEGER DEFAULT 0,"
    "   continued_from INTEGER DEFAULT NULL,"
    "   compaction_summary TEXT DEFAULT NULL,"
    "   FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE,"
    "   FOREIGN KEY (continued_from) REFERENCES conversations(id) ON DELETE SET NULL"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_conversations_user ON conversations(user_id, updated_at DESC);"
    "CREATE INDEX IF NOT EXISTS idx_conversations_search ON conversations(user_id, title);"
    /* Note: idx_conversations_continued is created during migration or post-init
     * to handle both new databases and upgrades from v6 */

    /* Messages table (added in schema v4) */
    "CREATE TABLE IF NOT EXISTS messages ("
    "   id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "   conversation_id INTEGER NOT NULL,"
    "   role TEXT NOT NULL CHECK(role IN ('system', 'user', 'assistant', 'tool')),"
    "   content TEXT NOT NULL,"
    "   created_at INTEGER NOT NULL,"
    "   FOREIGN KEY (conversation_id) REFERENCES conversations(id) ON DELETE CASCADE"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_messages_conversation ON messages(conversation_id, id ASC);"

    /* Session metrics table (added in schema v8) */
    "CREATE TABLE IF NOT EXISTS session_metrics ("
    "   id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "   session_id INTEGER NOT NULL,"
    "   user_id INTEGER,"
    "   session_type TEXT NOT NULL,"
    "   started_at INTEGER NOT NULL,"
    "   ended_at INTEGER,"
    "   queries_total INTEGER DEFAULT 0,"
    "   queries_cloud INTEGER DEFAULT 0,"
    "   queries_local INTEGER DEFAULT 0,"
    "   errors_count INTEGER DEFAULT 0,"
    "   fallbacks_count INTEGER DEFAULT 0,"
    "   avg_asr_ms REAL,"
    "   avg_llm_ttft_ms REAL,"
    "   avg_llm_total_ms REAL,"
    "   avg_tts_ms REAL,"
    "   avg_pipeline_ms REAL,"
    "   FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE SET NULL"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_session_metrics_user ON session_metrics(user_id, started_at "
    "DESC);"
    "CREATE INDEX IF NOT EXISTS idx_session_metrics_time ON session_metrics(started_at DESC);"

    /* Per-provider token usage breakdown (added in schema v8) */
    "CREATE TABLE IF NOT EXISTS session_metrics_providers ("
    "   id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "   session_metrics_id INTEGER NOT NULL,"
    "   provider TEXT NOT NULL,"
    "   tokens_input INTEGER DEFAULT 0,"
    "   tokens_output INTEGER DEFAULT 0,"
    "   tokens_cached INTEGER DEFAULT 0,"
    "   queries INTEGER DEFAULT 0,"
    "   FOREIGN KEY (session_metrics_id) REFERENCES session_metrics(id) ON DELETE CASCADE"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_metrics_providers_session ON "
    "session_metrics_providers(session_metrics_id);";

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

   /* v5 migration: add context_tokens and context_max columns to conversations
    * Only runs if conversations table already exists (v4+) without these columns */
   if (current_version >= 1 && current_version < 5) {
      rc = sqlite3_exec(s_db.db,
                        "ALTER TABLE conversations ADD COLUMN context_tokens INTEGER DEFAULT 0",
                        NULL, NULL, &errmsg);
      if (rc != SQLITE_OK) {
         LOG_INFO("auth_db: v5 migration note (context_tokens): %s", errmsg ? errmsg : "ok");
         sqlite3_free(errmsg);
         errmsg = NULL;
      }
      rc = sqlite3_exec(s_db.db,
                        "ALTER TABLE conversations ADD COLUMN context_max INTEGER DEFAULT 0", NULL,
                        NULL, &errmsg);
      if (rc != SQLITE_OK) {
         LOG_INFO("auth_db: v5 migration note (context_max): %s", errmsg ? errmsg : "ok");
         sqlite3_free(errmsg);
         errmsg = NULL;
      } else {
         LOG_INFO("auth_db: added context columns to conversations");
      }
   }

   /* v6 migration: update messages table CHECK constraint to include 'tool' role
    * SQLite doesn't support ALTER TABLE to modify constraints, so we recreate the table */
   if (current_version >= 4 && current_version < 6) {
      LOG_INFO("auth_db: migrating messages table to support 'tool' role");
      const char *migration_sql =
          "BEGIN TRANSACTION;"
          "CREATE TABLE messages_new ("
          "   id INTEGER PRIMARY KEY AUTOINCREMENT,"
          "   conversation_id INTEGER NOT NULL,"
          "   role TEXT NOT NULL CHECK(role IN ('system', 'user', 'assistant', 'tool')),"
          "   content TEXT NOT NULL,"
          "   created_at INTEGER NOT NULL,"
          "   FOREIGN KEY (conversation_id) REFERENCES conversations(id) ON DELETE CASCADE"
          ");"
          "INSERT INTO messages_new SELECT * FROM messages;"
          "DROP TABLE messages;"
          "ALTER TABLE messages_new RENAME TO messages;"
          "CREATE INDEX IF NOT EXISTS idx_messages_conversation ON messages(conversation_id, id "
          "ASC);"
          "COMMIT;";

      rc = sqlite3_exec(s_db.db, migration_sql, NULL, NULL, &errmsg);
      if (rc != SQLITE_OK) {
         LOG_ERROR("auth_db: v6 migration failed: %s", errmsg ? errmsg : "unknown");
         sqlite3_free(errmsg);
         errmsg = NULL;
         /* Rollback on failure */
         sqlite3_exec(s_db.db, "ROLLBACK;", NULL, NULL, NULL);
      } else {
         LOG_INFO("auth_db: migrated messages table to v6 (added 'tool' role)");
      }
   }

   /* v7 migration: add continued_from and compaction_summary columns to conversations
    * These support conversation continuation when context compaction occurs */
   if (current_version >= 4 && current_version < 7) {
      rc = sqlite3_exec(s_db.db,
                        "ALTER TABLE conversations ADD COLUMN continued_from INTEGER DEFAULT NULL "
                        "REFERENCES conversations(id) ON DELETE SET NULL",
                        NULL, NULL, &errmsg);
      if (rc != SQLITE_OK) {
         LOG_INFO("auth_db: v7 migration note (continued_from): %s", errmsg ? errmsg : "ok");
         sqlite3_free(errmsg);
         errmsg = NULL;
      }
      rc = sqlite3_exec(s_db.db,
                        "ALTER TABLE conversations ADD COLUMN compaction_summary TEXT DEFAULT NULL",
                        NULL, NULL, &errmsg);
      if (rc != SQLITE_OK) {
         LOG_INFO("auth_db: v7 migration note (compaction_summary): %s", errmsg ? errmsg : "ok");
         sqlite3_free(errmsg);
         errmsg = NULL;
      }
      /* Add index for finding child conversations */
      rc = sqlite3_exec(
          s_db.db,
          "CREATE INDEX IF NOT EXISTS idx_conversations_continued ON conversations(continued_from)",
          NULL, NULL, &errmsg);
      if (rc != SQLITE_OK) {
         LOG_INFO("auth_db: v7 migration note (index): %s", errmsg ? errmsg : "ok");
         sqlite3_free(errmsg);
         errmsg = NULL;
      } else {
         LOG_INFO("auth_db: added continuation columns to conversations (v7)");
      }
   }

   /* v8 migration: session_metrics table
    * The table is created by SCHEMA_SQL with IF NOT EXISTS, so no explicit
    * migration is needed. Just log the upgrade for existing databases. */
   if (current_version >= 1 && current_version < 8) {
      LOG_INFO("auth_db: added session_metrics table (v8)");
   }

   /* v9 migration: add theme column to user_settings */
   if (current_version >= 1 && current_version < 9) {
      rc = sqlite3_exec(s_db.db, "ALTER TABLE user_settings ADD COLUMN theme TEXT DEFAULT 'cyan'",
                        NULL, NULL, &errmsg);
      if (rc != SQLITE_OK) {
         LOG_INFO("auth_db: v9 migration note (theme): %s", errmsg ? errmsg : "ok");
         sqlite3_free(errmsg);
         errmsg = NULL;
      } else {
         LOG_INFO("auth_db: added theme column to user_settings");
      }
   }

   /* Create continuation index (runs for both new databases and migrations) */
   rc = sqlite3_exec(s_db.db,
                     "CREATE INDEX IF NOT EXISTS idx_conversations_continued "
                     "ON conversations(continued_from)",
                     NULL, NULL, &errmsg);
   if (rc != SQLITE_OK) {
      /* Non-fatal - index is just for performance */
      LOG_WARNING("auth_db: could not create continuation index: %s", errmsg ? errmsg : "ok");
      sqlite3_free(errmsg);
      errmsg = NULL;
   }

   /* Log migration if upgrading from an older version */
   if (current_version > 0 && current_version < SCHEMA_VERSION) {
      LOG_INFO("auth_db: migrated schema from v%d to v%d", current_version, SCHEMA_VERSION);
   } else if (current_version == 0) {
      LOG_INFO("auth_db: created schema v%d", SCHEMA_VERSION);
   }

   /* Update schema version (delete old rows first to handle PRIMARY KEY on version) */
   rc = sqlite3_exec(s_db.db, "DELETE FROM schema_version", NULL, NULL, &errmsg);
   if (rc != SQLITE_OK) {
      LOG_WARNING("auth_db: failed to clear schema_version: %s", errmsg ? errmsg : "unknown");
      sqlite3_free(errmsg);
      errmsg = NULL;
   }
   rc = sqlite3_exec(s_db.db,
                     "INSERT INTO schema_version (version) VALUES (" STRINGIFY(SCHEMA_VERSION) ")",
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
       "tts_length_scale, theme FROM user_settings WHERE user_id = ?",
       -1, &s_db.stmt_get_user_settings, NULL);
   if (rc != SQLITE_OK) {
      LOG_ERROR("auth_db: prepare get_user_settings failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(
       s_db.db,
       "INSERT INTO user_settings (user_id, persona_description, persona_mode, location, timezone, "
       "units, tts_voice_model, tts_length_scale, theme, updated_at) "
       "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?) "
       "ON CONFLICT(user_id) DO UPDATE SET "
       "persona_description=excluded.persona_description, persona_mode=excluded.persona_mode, "
       "location=excluded.location, timezone=excluded.timezone, units=excluded.units, "
       "tts_voice_model=excluded.tts_voice_model, tts_length_scale=excluded.tts_length_scale, "
       "theme=excluded.theme, updated_at=excluded.updated_at",
       -1, &s_db.stmt_set_user_settings, NULL);
   if (rc != SQLITE_OK) {
      LOG_ERROR("auth_db: prepare set_user_settings failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   /* Conversation statements */
   rc = sqlite3_prepare_v2(
       s_db.db,
       "INSERT INTO conversations (user_id, title, created_at, updated_at) VALUES (?, ?, ?, ?)", -1,
       &s_db.stmt_conv_create, NULL);
   if (rc != SQLITE_OK) {
      LOG_ERROR("auth_db: prepare conv_create failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(
       s_db.db,
       "SELECT id, user_id, title, created_at, updated_at, message_count, is_archived, "
       "context_tokens, context_max, continued_from, compaction_summary "
       "FROM conversations WHERE id = ?",
       -1, &s_db.stmt_conv_get, NULL);
   if (rc != SQLITE_OK) {
      LOG_ERROR("auth_db: prepare conv_get failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(
       s_db.db,
       "SELECT id, user_id, title, created_at, updated_at, message_count, is_archived, "
       "context_tokens, context_max, continued_from, compaction_summary "
       "FROM conversations WHERE user_id = ? AND (is_archived = 0 OR ? = 1) "
       "ORDER BY updated_at DESC LIMIT ? OFFSET ?",
       -1, &s_db.stmt_conv_list, NULL);
   if (rc != SQLITE_OK) {
      LOG_ERROR("auth_db: prepare conv_list failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   /* Admin-only: list all conversations across all users */
   rc = sqlite3_prepare_v2(
       s_db.db,
       "SELECT c.id, c.user_id, c.title, c.created_at, c.updated_at, c.message_count, "
       "c.is_archived, c.context_tokens, c.context_max, c.continued_from, "
       "c.compaction_summary, u.username "
       "FROM conversations c LEFT JOIN users u ON c.user_id = u.id "
       "WHERE (c.is_archived = 0 OR ? = 1) "
       "ORDER BY c.updated_at DESC LIMIT ? OFFSET ?",
       -1, &s_db.stmt_conv_list_all, NULL);
   if (rc != SQLITE_OK) {
      LOG_ERROR("auth_db: prepare conv_list_all failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(
       s_db.db,
       "SELECT id, user_id, title, created_at, updated_at, message_count, is_archived, "
       "context_tokens, context_max, continued_from, compaction_summary "
       "FROM conversations WHERE user_id = ? AND title LIKE ? ESCAPE '\\' "
       "ORDER BY updated_at DESC LIMIT ? OFFSET ?",
       -1, &s_db.stmt_conv_search, NULL);
   if (rc != SQLITE_OK) {
      LOG_ERROR("auth_db: prepare conv_search failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db,
                           "SELECT DISTINCT c.id, c.user_id, c.title, c.created_at, c.updated_at, "
                           "c.message_count, c.is_archived, c.context_tokens, c.context_max, "
                           "c.continued_from, c.compaction_summary "
                           "FROM conversations c "
                           "INNER JOIN messages m ON m.conversation_id = c.id "
                           "WHERE c.user_id = ? AND m.content LIKE ? ESCAPE '\\' "
                           "ORDER BY c.updated_at DESC LIMIT ? OFFSET ?",
                           -1, &s_db.stmt_conv_search_content, NULL);
   if (rc != SQLITE_OK) {
      LOG_ERROR("auth_db: prepare conv_search_content failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db,
                           "UPDATE conversations SET title = ? WHERE id = ? AND user_id = ?", -1,
                           &s_db.stmt_conv_rename, NULL);
   if (rc != SQLITE_OK) {
      LOG_ERROR("auth_db: prepare conv_rename failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db, "DELETE FROM conversations WHERE id = ? AND user_id = ?", -1,
                           &s_db.stmt_conv_delete, NULL);
   if (rc != SQLITE_OK) {
      LOG_ERROR("auth_db: prepare conv_delete failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   /* Admin-only: delete any conversation without ownership check */
   rc = sqlite3_prepare_v2(s_db.db, "DELETE FROM conversations WHERE id = ?", -1,
                           &s_db.stmt_conv_delete_admin, NULL);
   if (rc != SQLITE_OK) {
      LOG_ERROR("auth_db: prepare conv_delete_admin failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db, "SELECT COUNT(*) FROM conversations WHERE user_id = ?", -1,
                           &s_db.stmt_conv_count, NULL);
   if (rc != SQLITE_OK) {
      LOG_ERROR("auth_db: prepare conv_count failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(
       s_db.db,
       "INSERT INTO messages (conversation_id, role, content, created_at) "
       "SELECT ?, ?, ?, ? WHERE EXISTS (SELECT 1 FROM conversations WHERE id = ? AND user_id = ?)",
       -1, &s_db.stmt_msg_add, NULL);
   if (rc != SQLITE_OK) {
      LOG_ERROR("auth_db: prepare msg_add failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db,
                           "SELECT m.id, m.conversation_id, m.role, m.content, m.created_at "
                           "FROM messages m "
                           "INNER JOIN conversations c ON m.conversation_id = c.id "
                           "WHERE m.conversation_id = ? AND c.user_id = ? ORDER BY m.id ASC",
                           -1, &s_db.stmt_msg_get, NULL);
   if (rc != SQLITE_OK) {
      LOG_ERROR("auth_db: prepare msg_get failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   /* Admin-only: get messages without user ownership check */
   rc = sqlite3_prepare_v2(s_db.db,
                           "SELECT id, conversation_id, role, content, created_at "
                           "FROM messages WHERE conversation_id = ? ORDER BY id ASC",
                           -1, &s_db.stmt_msg_get_admin, NULL);
   if (rc != SQLITE_OK) {
      LOG_ERROR("auth_db: prepare msg_get_admin failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(
       s_db.db,
       "UPDATE conversations SET updated_at = ?, message_count = message_count + 1 WHERE id = ?",
       -1, &s_db.stmt_conv_update_meta, NULL);
   if (rc != SQLITE_OK) {
      LOG_ERROR("auth_db: prepare conv_update_meta failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db,
                           "UPDATE conversations SET context_tokens = ?, context_max = ? "
                           "WHERE id = ? AND user_id = ?",
                           -1, &s_db.stmt_conv_update_context, NULL);
   if (rc != SQLITE_OK) {
      LOG_ERROR("auth_db: prepare conv_update_context failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   /* Session metrics statements - token usage is in separate provider table */
   rc = sqlite3_prepare_v2(
       s_db.db,
       "INSERT INTO session_metrics ("
       "session_id, user_id, session_type, started_at, ended_at, "
       "queries_total, queries_cloud, queries_local, errors_count, fallbacks_count, "
       "avg_asr_ms, avg_llm_ttft_ms, avg_llm_total_ms, avg_tts_ms, avg_pipeline_ms"
       ") VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
       -1, &s_db.stmt_metrics_save, NULL);
   if (rc != SQLITE_OK) {
      LOG_ERROR("auth_db: prepare metrics_save failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   /* UPDATE statement for per-query metrics updates (id is param 16) */
   rc = sqlite3_prepare_v2(
       s_db.db,
       "UPDATE session_metrics SET "
       "ended_at = ?, queries_total = ?, queries_cloud = ?, queries_local = ?, "
       "errors_count = ?, fallbacks_count = ?, avg_asr_ms = ?, avg_llm_ttft_ms = ?, "
       "avg_llm_total_ms = ?, avg_tts_ms = ?, avg_pipeline_ms = ? "
       "WHERE id = ?",
       -1, &s_db.stmt_metrics_update, NULL);
   if (rc != SQLITE_OK) {
      LOG_ERROR("auth_db: prepare metrics_update failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db, "DELETE FROM session_metrics WHERE started_at < ?", -1,
                           &s_db.stmt_metrics_delete_old, NULL);
   if (rc != SQLITE_OK) {
      LOG_ERROR("auth_db: prepare metrics_delete_old failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   /* Provider metrics insert (child table) */
   rc = sqlite3_prepare_v2(s_db.db,
                           "INSERT INTO session_metrics_providers ("
                           "session_metrics_id, provider, tokens_input, tokens_output, "
                           "tokens_cached, queries"
                           ") VALUES (?, ?, ?, ?, ?, ?)",
                           -1, &s_db.stmt_provider_metrics_save, NULL);
   if (rc != SQLITE_OK) {
      LOG_ERROR("auth_db: prepare provider_metrics_save failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   /* Delete provider metrics before re-insert (for per-query updates) */
   rc = sqlite3_prepare_v2(s_db.db,
                           "DELETE FROM session_metrics_providers WHERE session_metrics_id = ?", -1,
                           &s_db.stmt_provider_metrics_delete, NULL);
   if (rc != SQLITE_OK) {
      LOG_ERROR("auth_db: prepare provider_metrics_delete failed: %s", sqlite3_errmsg(s_db.db));
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

   /* Conversation statements */
   if (s_db.stmt_conv_create)
      sqlite3_finalize(s_db.stmt_conv_create);
   if (s_db.stmt_conv_get)
      sqlite3_finalize(s_db.stmt_conv_get);
   if (s_db.stmt_conv_list)
      sqlite3_finalize(s_db.stmt_conv_list);
   if (s_db.stmt_conv_list_all)
      sqlite3_finalize(s_db.stmt_conv_list_all);
   if (s_db.stmt_conv_search)
      sqlite3_finalize(s_db.stmt_conv_search);
   if (s_db.stmt_conv_search_content)
      sqlite3_finalize(s_db.stmt_conv_search_content);
   if (s_db.stmt_conv_rename)
      sqlite3_finalize(s_db.stmt_conv_rename);
   if (s_db.stmt_conv_delete)
      sqlite3_finalize(s_db.stmt_conv_delete);
   if (s_db.stmt_conv_delete_admin)
      sqlite3_finalize(s_db.stmt_conv_delete_admin);
   if (s_db.stmt_conv_count)
      sqlite3_finalize(s_db.stmt_conv_count);
   if (s_db.stmt_msg_add)
      sqlite3_finalize(s_db.stmt_msg_add);
   if (s_db.stmt_msg_get)
      sqlite3_finalize(s_db.stmt_msg_get);
   if (s_db.stmt_msg_get_admin)
      sqlite3_finalize(s_db.stmt_msg_get_admin);
   if (s_db.stmt_conv_update_meta)
      sqlite3_finalize(s_db.stmt_conv_update_meta);
   if (s_db.stmt_conv_update_context)
      sqlite3_finalize(s_db.stmt_conv_update_context);

   /* Session metrics statements */
   if (s_db.stmt_metrics_save)
      sqlite3_finalize(s_db.stmt_metrics_save);
   if (s_db.stmt_metrics_update)
      sqlite3_finalize(s_db.stmt_metrics_update);
   if (s_db.stmt_metrics_delete_old)
      sqlite3_finalize(s_db.stmt_metrics_delete_old);
   if (s_db.stmt_provider_metrics_save)
      sqlite3_finalize(s_db.stmt_provider_metrics_save);
   if (s_db.stmt_provider_metrics_delete)
      sqlite3_finalize(s_db.stmt_provider_metrics_delete);

   /* Clear all pointers */
   memset(&s_db.stmt_create_user, 0,
          (char *)&s_db.stmt_provider_metrics_delete - (char *)&s_db.stmt_create_user +
              sizeof(s_db.stmt_provider_metrics_delete));
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
   strncpy(settings_out->theme, "cyan", AUTH_THEME_MAX - 1);

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

      const char *theme = (const char *)sqlite3_column_text(s_db.stmt_get_user_settings, 7);
      if (theme) {
         strncpy(settings_out->theme, theme, AUTH_THEME_MAX - 1);
      }
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
   sqlite3_bind_text(s_db.stmt_set_user_settings, 9, settings->theme, -1, SQLITE_STATIC);
   sqlite3_bind_int64(s_db.stmt_set_user_settings, 10, (int64_t)time(NULL));

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
   if (!prefix || strlen(prefix) < 16) {
      return AUTH_DB_INVALID;
   }

   pthread_mutex_lock(&s_db.mutex);

   if (!s_db.initialized) {
      pthread_mutex_unlock(&s_db.mutex);
      return AUTH_DB_FAILURE;
   }

   /* Find full token matching prefix (use SUBSTR for exact matching, not LIKE) */
   const char *find_sql = "SELECT token FROM sessions WHERE substr(token, 1, 16) = ? LIMIT 1";
   sqlite3_stmt *stmt = NULL;
   int rc = sqlite3_prepare_v2(s_db.db, find_sql, -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      pthread_mutex_unlock(&s_db.mutex);
      return AUTH_DB_FAILURE;
   }

   /* Bind only the first 16 characters */
   char prefix_buf[17] = { 0 };
   strncpy(prefix_buf, prefix, 16);
   sqlite3_bind_text(stmt, 1, prefix_buf, 16, SQLITE_STATIC);

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

bool auth_db_session_belongs_to_user(const char *prefix, int user_id) {
   if (!prefix || strlen(prefix) < 16 || user_id <= 0) {
      return false;
   }

   pthread_mutex_lock(&s_db.mutex);

   if (!s_db.initialized) {
      pthread_mutex_unlock(&s_db.mutex);
      return false;
   }

   /* Single query to check if session with prefix belongs to user */
   const char *sql =
       "SELECT 1 FROM sessions WHERE substr(token, 1, 16) = ? AND user_id = ? LIMIT 1";
   sqlite3_stmt *stmt = NULL;
   int rc = sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      pthread_mutex_unlock(&s_db.mutex);
      return false;
   }

   char prefix_buf[17] = { 0 };
   strncpy(prefix_buf, prefix, 16);
   sqlite3_bind_text(stmt, 1, prefix_buf, 16, SQLITE_STATIC);
   sqlite3_bind_int(stmt, 2, user_id);

   rc = sqlite3_step(stmt);
   bool belongs = (rc == SQLITE_ROW);

   sqlite3_finalize(stmt);
   pthread_mutex_unlock(&s_db.mutex);

   return belongs;
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
                     "s.last_activity, s.ip_address, s.user_agent "
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
         strncpy(session.token_prefix, tok, 16);
         session.token_prefix[16] = '\0';
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

      const char *ua = (const char *)sqlite3_column_text(stmt, 6);
      if (ua) {
         strncpy(session.user_agent, ua, AUTH_USER_AGENT_MAX - 1);
         session.user_agent[AUTH_USER_AGENT_MAX - 1] = '\0';
      }

      if (callback(&session, ctx) != 0) {
         break; /* Callback requested stop */
      }
   }

   sqlite3_finalize(stmt);
   pthread_mutex_unlock(&s_db.mutex);

   return AUTH_DB_SUCCESS;
}

int auth_db_list_user_sessions(int user_id, auth_session_summary_callback_t callback, void *ctx) {
   if (!callback) {
      return AUTH_DB_INVALID;
   }

   pthread_mutex_lock(&s_db.mutex);

   if (!s_db.initialized) {
      pthread_mutex_unlock(&s_db.mutex);
      return AUTH_DB_FAILURE;
   }

   const char *sql = "SELECT s.token, s.user_id, u.username, s.created_at, "
                     "s.last_activity, s.ip_address, s.user_agent "
                     "FROM sessions s "
                     "JOIN users u ON s.user_id = u.id "
                     "WHERE s.user_id = ? "
                     "ORDER BY s.last_activity DESC";
   sqlite3_stmt *stmt = NULL;
   int rc = sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      pthread_mutex_unlock(&s_db.mutex);
      return AUTH_DB_FAILURE;
   }

   sqlite3_bind_int(stmt, 1, user_id);

   while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
      auth_session_summary_t session = { 0 };

      /* Only copy token prefix (8 chars) for security */
      const char *tok = (const char *)sqlite3_column_text(stmt, 0);
      if (tok) {
         strncpy(session.token_prefix, tok, 16);
         session.token_prefix[16] = '\0';
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

      const char *ua = (const char *)sqlite3_column_text(stmt, 6);
      if (ua) {
         strncpy(session.user_agent, ua, AUTH_USER_AGENT_MAX - 1);
         session.user_agent[AUTH_USER_AGENT_MAX - 1] = '\0';
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

   /* Clean up old session metrics (90 day retention) */
   time_t metrics_cutoff = now - (SESSION_METRICS_RETENTION_DAYS * 24 * 60 * 60);
   sqlite3_reset(s_db.stmt_metrics_delete_old);
   sqlite3_bind_int64(s_db.stmt_metrics_delete_old, 1, (int64_t)metrics_cutoff);
   sqlite3_step(s_db.stmt_metrics_delete_old);
   sqlite3_reset(s_db.stmt_metrics_delete_old);

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

/* ============================================================================
 * Conversation History Functions
 * ============================================================================ */

int conv_db_create(int user_id, const char *title, int64_t *conv_id_out) {
   if (user_id <= 0 || !conv_id_out) {
      return AUTH_DB_INVALID;
   }

   pthread_mutex_lock(&s_db.mutex);

   if (!s_db.initialized) {
      pthread_mutex_unlock(&s_db.mutex);
      return AUTH_DB_FAILURE;
   }

   /* Check conversation limit per user */
   if (CONV_MAX_PER_USER > 0) {
      sqlite3_reset(s_db.stmt_conv_count);
      sqlite3_bind_int(s_db.stmt_conv_count, 1, user_id);
      if (sqlite3_step(s_db.stmt_conv_count) == SQLITE_ROW) {
         int count = sqlite3_column_int(s_db.stmt_conv_count, 0);
         if (count >= CONV_MAX_PER_USER) {
            sqlite3_reset(s_db.stmt_conv_count);
            pthread_mutex_unlock(&s_db.mutex);
            return AUTH_DB_LIMIT_EXCEEDED;
         }
      }
      sqlite3_reset(s_db.stmt_conv_count);
   }

   time_t now = time(NULL);

   /* Use default title if none provided, truncate if too long */
   char safe_title[CONV_TITLE_MAX];
   if (title && title[0] != '\0') {
      strncpy(safe_title, title, CONV_TITLE_MAX - 1);
      safe_title[CONV_TITLE_MAX - 1] = '\0';
   } else {
      strcpy(safe_title, "New Conversation");
   }

   sqlite3_reset(s_db.stmt_conv_create);
   sqlite3_bind_int(s_db.stmt_conv_create, 1, user_id);
   sqlite3_bind_text(s_db.stmt_conv_create, 2, safe_title, -1, SQLITE_TRANSIENT);
   sqlite3_bind_int64(s_db.stmt_conv_create, 3, (int64_t)now);
   sqlite3_bind_int64(s_db.stmt_conv_create, 4, (int64_t)now);

   int rc = sqlite3_step(s_db.stmt_conv_create);
   sqlite3_reset(s_db.stmt_conv_create);

   if (rc != SQLITE_DONE) {
      LOG_ERROR("conv_db_create: insert failed: %s", sqlite3_errmsg(s_db.db));
      pthread_mutex_unlock(&s_db.mutex);
      return AUTH_DB_FAILURE;
   }

   *conv_id_out = sqlite3_last_insert_rowid(s_db.db);

   pthread_mutex_unlock(&s_db.mutex);

   LOG_INFO("Created conversation %lld for user %d", (long long)*conv_id_out, user_id);
   return AUTH_DB_SUCCESS;
}

int conv_db_get(int64_t conv_id, int user_id, conversation_t *conv_out) {
   if (conv_id <= 0 || !conv_out) {
      return AUTH_DB_INVALID;
   }

   pthread_mutex_lock(&s_db.mutex);

   if (!s_db.initialized) {
      pthread_mutex_unlock(&s_db.mutex);
      return AUTH_DB_FAILURE;
   }

   sqlite3_reset(s_db.stmt_conv_get);
   sqlite3_bind_int64(s_db.stmt_conv_get, 1, conv_id);

   int rc = sqlite3_step(s_db.stmt_conv_get);
   if (rc != SQLITE_ROW) {
      sqlite3_reset(s_db.stmt_conv_get);
      pthread_mutex_unlock(&s_db.mutex);
      return (rc == SQLITE_DONE) ? AUTH_DB_NOT_FOUND : AUTH_DB_FAILURE;
   }

   /* Check ownership */
   int owner_id = sqlite3_column_int(s_db.stmt_conv_get, 1);
   if (owner_id != user_id) {
      sqlite3_reset(s_db.stmt_conv_get);
      pthread_mutex_unlock(&s_db.mutex);
      return AUTH_DB_FORBIDDEN;
   }

   memset(conv_out, 0, sizeof(*conv_out));
   conv_out->id = sqlite3_column_int64(s_db.stmt_conv_get, 0);
   conv_out->user_id = owner_id;

   const char *title = (const char *)sqlite3_column_text(s_db.stmt_conv_get, 2);
   if (title) {
      strncpy(conv_out->title, title, CONV_TITLE_MAX - 1);
      conv_out->title[CONV_TITLE_MAX - 1] = '\0';
   }

   conv_out->created_at = (time_t)sqlite3_column_int64(s_db.stmt_conv_get, 3);
   conv_out->updated_at = (time_t)sqlite3_column_int64(s_db.stmt_conv_get, 4);
   conv_out->message_count = sqlite3_column_int(s_db.stmt_conv_get, 5);
   conv_out->is_archived = sqlite3_column_int(s_db.stmt_conv_get, 6) != 0;
   conv_out->context_tokens = sqlite3_column_int(s_db.stmt_conv_get, 7);
   conv_out->context_max = sqlite3_column_int(s_db.stmt_conv_get, 8);

   /* Continuation fields (schema v7+) */
   conv_out->continued_from = sqlite3_column_int64(s_db.stmt_conv_get, 9);
   const char *summary = (const char *)sqlite3_column_text(s_db.stmt_conv_get, 10);
   conv_out->compaction_summary = summary ? strdup(summary) : NULL;

   sqlite3_reset(s_db.stmt_conv_get);
   pthread_mutex_unlock(&s_db.mutex);

   return AUTH_DB_SUCCESS;
}

void conv_free(conversation_t *conv) {
   if (!conv)
      return;
   if (conv->compaction_summary) {
      free(conv->compaction_summary);
      conv->compaction_summary = NULL;
   }
}

int conv_db_create_continuation(int user_id,
                                int64_t parent_id,
                                const char *compaction_summary,
                                int64_t *conv_id_out) {
   if (user_id <= 0 || parent_id <= 0 || !conv_id_out) {
      return AUTH_DB_INVALID;
   }

   pthread_mutex_lock(&s_db.mutex);

   if (!s_db.initialized) {
      pthread_mutex_unlock(&s_db.mutex);
      return AUTH_DB_FAILURE;
   }

   /* Verify parent exists and belongs to user, then archive it */
   const char *sql_archive = "UPDATE conversations SET is_archived = 1, updated_at = ? "
                             "WHERE id = ? AND user_id = ?";
   sqlite3_stmt *stmt = NULL;
   int rc = sqlite3_prepare_v2(s_db.db, sql_archive, -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      LOG_ERROR("conv_db_create_continuation: prepare archive failed: %s", sqlite3_errmsg(s_db.db));
      pthread_mutex_unlock(&s_db.mutex);
      return AUTH_DB_FAILURE;
   }

   time_t now = time(NULL);
   sqlite3_bind_int64(stmt, 1, (int64_t)now);
   sqlite3_bind_int64(stmt, 2, parent_id);
   sqlite3_bind_int(stmt, 3, user_id);

   rc = sqlite3_step(stmt);
   if (rc != SQLITE_DONE) {
      LOG_ERROR("conv_db_create_continuation: archive failed: %s", sqlite3_errmsg(s_db.db));
      sqlite3_finalize(stmt);
      pthread_mutex_unlock(&s_db.mutex);
      return AUTH_DB_FAILURE;
   }

   int changes = sqlite3_changes(s_db.db);
   sqlite3_finalize(stmt);

   if (changes == 0) {
      /* Parent not found or doesn't belong to user */
      pthread_mutex_unlock(&s_db.mutex);
      return AUTH_DB_NOT_FOUND;
   }

   /* Get parent title for the continuation */
   const char *sql_get_title = "SELECT title FROM conversations WHERE id = ?";
   rc = sqlite3_prepare_v2(s_db.db, sql_get_title, -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      pthread_mutex_unlock(&s_db.mutex);
      return AUTH_DB_FAILURE;
   }
   sqlite3_bind_int64(stmt, 1, parent_id);

   char parent_title[CONV_TITLE_MAX] = "Continued";
   if (sqlite3_step(stmt) == SQLITE_ROW) {
      const char *title = (const char *)sqlite3_column_text(stmt, 0);
      if (title) {
         snprintf(parent_title, sizeof(parent_title), "%s (cont.)", title);
      }
   }
   sqlite3_finalize(stmt);

   /* Create continuation conversation */
   const char *sql_create =
       "INSERT INTO conversations (user_id, title, created_at, updated_at, continued_from, "
       "compaction_summary) VALUES (?, ?, ?, ?, ?, ?)";
   rc = sqlite3_prepare_v2(s_db.db, sql_create, -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      LOG_ERROR("conv_db_create_continuation: prepare insert failed: %s", sqlite3_errmsg(s_db.db));
      pthread_mutex_unlock(&s_db.mutex);
      return AUTH_DB_FAILURE;
   }

   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_text(stmt, 2, parent_title, -1, SQLITE_TRANSIENT);
   sqlite3_bind_int64(stmt, 3, (int64_t)now);
   sqlite3_bind_int64(stmt, 4, (int64_t)now);
   sqlite3_bind_int64(stmt, 5, parent_id);
   if (compaction_summary) {
      sqlite3_bind_text(stmt, 6, compaction_summary, -1, SQLITE_TRANSIENT);
   } else {
      sqlite3_bind_null(stmt, 6);
   }

   rc = sqlite3_step(stmt);
   sqlite3_finalize(stmt);

   if (rc != SQLITE_DONE) {
      LOG_ERROR("conv_db_create_continuation: insert failed: %s", sqlite3_errmsg(s_db.db));
      pthread_mutex_unlock(&s_db.mutex);
      return AUTH_DB_FAILURE;
   }

   *conv_id_out = sqlite3_last_insert_rowid(s_db.db);

   pthread_mutex_unlock(&s_db.mutex);

   LOG_INFO("Created continuation conversation %lld from parent %lld for user %d",
            (long long)*conv_id_out, (long long)parent_id, user_id);
   return AUTH_DB_SUCCESS;
}

int conv_db_list(int user_id,
                 bool include_archived,
                 const conv_pagination_t *pagination,
                 conversation_callback_t callback,
                 void *ctx) {
   if (user_id <= 0 || !callback) {
      return AUTH_DB_INVALID;
   }

   int limit = CONV_LIST_DEFAULT_LIMIT;
   int offset = 0;
   if (pagination) {
      limit = (pagination->limit > 0 && pagination->limit <= CONV_LIST_MAX_LIMIT)
                  ? pagination->limit
                  : CONV_LIST_DEFAULT_LIMIT;
      offset = (pagination->offset >= 0) ? pagination->offset : 0;
   }

   pthread_mutex_lock(&s_db.mutex);

   if (!s_db.initialized) {
      pthread_mutex_unlock(&s_db.mutex);
      return AUTH_DB_FAILURE;
   }

   sqlite3_reset(s_db.stmt_conv_list);
   sqlite3_bind_int(s_db.stmt_conv_list, 1, user_id);
   sqlite3_bind_int(s_db.stmt_conv_list, 2, include_archived ? 1 : 0);
   sqlite3_bind_int(s_db.stmt_conv_list, 3, limit);
   sqlite3_bind_int(s_db.stmt_conv_list, 4, offset);

   int rc;
   while ((rc = sqlite3_step(s_db.stmt_conv_list)) == SQLITE_ROW) {
      conversation_t conv = { 0 };

      conv.id = sqlite3_column_int64(s_db.stmt_conv_list, 0);
      conv.user_id = sqlite3_column_int(s_db.stmt_conv_list, 1);

      const char *title = (const char *)sqlite3_column_text(s_db.stmt_conv_list, 2);
      if (title) {
         strncpy(conv.title, title, CONV_TITLE_MAX - 1);
         conv.title[CONV_TITLE_MAX - 1] = '\0';
      }

      conv.created_at = (time_t)sqlite3_column_int64(s_db.stmt_conv_list, 3);
      conv.updated_at = (time_t)sqlite3_column_int64(s_db.stmt_conv_list, 4);
      conv.message_count = sqlite3_column_int(s_db.stmt_conv_list, 5);
      conv.is_archived = sqlite3_column_int(s_db.stmt_conv_list, 6) != 0;
      conv.context_tokens = sqlite3_column_int(s_db.stmt_conv_list, 7);
      conv.context_max = sqlite3_column_int(s_db.stmt_conv_list, 8);

      /* Continuation fields - only load continued_from for list view (chain indicator) */
      conv.continued_from = sqlite3_column_int64(s_db.stmt_conv_list, 9);
      conv.compaction_summary = NULL; /* Load on demand via conv_db_get */

      if (callback(&conv, ctx) != 0) {
         break; /* Callback requested stop */
      }
   }

   sqlite3_reset(s_db.stmt_conv_list);
   pthread_mutex_unlock(&s_db.mutex);

   return AUTH_DB_SUCCESS;
}

int conv_db_list_all(bool include_archived,
                     const conv_pagination_t *pagination,
                     conversation_all_callback_t callback,
                     void *ctx) {
   if (!callback) {
      return AUTH_DB_INVALID;
   }

   int limit = CONV_LIST_DEFAULT_LIMIT;
   int offset = 0;
   if (pagination) {
      limit = (pagination->limit > 0 && pagination->limit <= CONV_LIST_MAX_LIMIT)
                  ? pagination->limit
                  : CONV_LIST_DEFAULT_LIMIT;
      offset = (pagination->offset >= 0) ? pagination->offset : 0;
   }

   pthread_mutex_lock(&s_db.mutex);

   if (!s_db.initialized) {
      pthread_mutex_unlock(&s_db.mutex);
      return AUTH_DB_FAILURE;
   }

   sqlite3_reset(s_db.stmt_conv_list_all);
   sqlite3_bind_int(s_db.stmt_conv_list_all, 1, include_archived ? 1 : 0);
   sqlite3_bind_int(s_db.stmt_conv_list_all, 2, limit);
   sqlite3_bind_int(s_db.stmt_conv_list_all, 3, offset);

   int rc;
   while ((rc = sqlite3_step(s_db.stmt_conv_list_all)) == SQLITE_ROW) {
      conversation_t conv = { 0 };
      char username[AUTH_USERNAME_MAX] = { 0 };

      conv.id = sqlite3_column_int64(s_db.stmt_conv_list_all, 0);
      conv.user_id = sqlite3_column_int(s_db.stmt_conv_list_all, 1);

      const char *title = (const char *)sqlite3_column_text(s_db.stmt_conv_list_all, 2);
      if (title) {
         strncpy(conv.title, title, CONV_TITLE_MAX - 1);
         conv.title[CONV_TITLE_MAX - 1] = '\0';
      }

      conv.created_at = (time_t)sqlite3_column_int64(s_db.stmt_conv_list_all, 3);
      conv.updated_at = (time_t)sqlite3_column_int64(s_db.stmt_conv_list_all, 4);
      conv.message_count = sqlite3_column_int(s_db.stmt_conv_list_all, 5);
      conv.is_archived = sqlite3_column_int(s_db.stmt_conv_list_all, 6) != 0;
      conv.context_tokens = sqlite3_column_int(s_db.stmt_conv_list_all, 7);
      conv.context_max = sqlite3_column_int(s_db.stmt_conv_list_all, 8);
      conv.continued_from = sqlite3_column_int64(s_db.stmt_conv_list_all, 9);
      conv.compaction_summary = NULL;

      const char *uname = (const char *)sqlite3_column_text(s_db.stmt_conv_list_all, 11);
      if (uname) {
         strncpy(username, uname, AUTH_USERNAME_MAX - 1);
         username[AUTH_USERNAME_MAX - 1] = '\0';
      }

      if (callback(&conv, username, ctx) != 0) {
         break;
      }
   }

   sqlite3_reset(s_db.stmt_conv_list_all);
   pthread_mutex_unlock(&s_db.mutex);

   return AUTH_DB_SUCCESS;
}

int conv_db_rename(int64_t conv_id, int user_id, const char *new_title) {
   if (conv_id <= 0 || !new_title || strlen(new_title) == 0) {
      return AUTH_DB_INVALID;
   }

   pthread_mutex_lock(&s_db.mutex);

   if (!s_db.initialized) {
      pthread_mutex_unlock(&s_db.mutex);
      return AUTH_DB_FAILURE;
   }

   sqlite3_reset(s_db.stmt_conv_rename);
   sqlite3_bind_text(s_db.stmt_conv_rename, 1, new_title, -1, SQLITE_STATIC);
   sqlite3_bind_int64(s_db.stmt_conv_rename, 2, conv_id);
   sqlite3_bind_int(s_db.stmt_conv_rename, 3, user_id);

   int rc = sqlite3_step(s_db.stmt_conv_rename);
   int changes = sqlite3_changes(s_db.db);
   sqlite3_reset(s_db.stmt_conv_rename);

   pthread_mutex_unlock(&s_db.mutex);

   if (rc != SQLITE_DONE) {
      return AUTH_DB_FAILURE;
   }

   /* No rows updated means either not found or forbidden */
   return (changes > 0) ? AUTH_DB_SUCCESS : AUTH_DB_NOT_FOUND;
}

int conv_db_delete(int64_t conv_id, int user_id) {
   if (conv_id <= 0) {
      return AUTH_DB_INVALID;
   }

   pthread_mutex_lock(&s_db.mutex);

   if (!s_db.initialized) {
      pthread_mutex_unlock(&s_db.mutex);
      return AUTH_DB_FAILURE;
   }

   /* Messages are deleted automatically via CASCADE */
   sqlite3_reset(s_db.stmt_conv_delete);
   sqlite3_bind_int64(s_db.stmt_conv_delete, 1, conv_id);
   sqlite3_bind_int(s_db.stmt_conv_delete, 2, user_id);

   int rc = sqlite3_step(s_db.stmt_conv_delete);
   int changes = sqlite3_changes(s_db.db);
   sqlite3_reset(s_db.stmt_conv_delete);

   pthread_mutex_unlock(&s_db.mutex);

   if (rc != SQLITE_DONE) {
      return AUTH_DB_FAILURE;
   }

   if (changes > 0) {
      LOG_INFO("Deleted conversation %lld for user %d", (long long)conv_id, user_id);
      return AUTH_DB_SUCCESS;
   }

   return AUTH_DB_NOT_FOUND;
}

int conv_db_delete_admin(int64_t conv_id) {
   if (conv_id <= 0) {
      return AUTH_DB_INVALID;
   }

   pthread_mutex_lock(&s_db.mutex);

   if (!s_db.initialized) {
      pthread_mutex_unlock(&s_db.mutex);
      return AUTH_DB_FAILURE;
   }

   sqlite3_reset(s_db.stmt_conv_delete_admin);
   sqlite3_bind_int64(s_db.stmt_conv_delete_admin, 1, conv_id);

   int rc = sqlite3_step(s_db.stmt_conv_delete_admin);
   int changes = sqlite3_changes(s_db.db);
   sqlite3_reset(s_db.stmt_conv_delete_admin);

   pthread_mutex_unlock(&s_db.mutex);

   if (rc != SQLITE_DONE) {
      return AUTH_DB_FAILURE;
   }

   if (changes > 0) {
      LOG_INFO("Admin deleted conversation %lld", (long long)conv_id);
      return AUTH_DB_SUCCESS;
   }

   return AUTH_DB_NOT_FOUND;
}

/**
 * @brief Build a LIKE pattern with escaped wildcards
 *
 * Escapes SQL LIKE wildcards (%, _, \) in the input and wraps with %...%
 * Uses backslash as the escape character.
 *
 * @param query Input search query
 * @param pattern Output buffer for escaped pattern
 * @param pattern_size Size of output buffer
 */
static void build_like_pattern(const char *query, char *pattern, size_t pattern_size) {
   if (!query || !pattern || pattern_size < 4) {
      if (pattern && pattern_size > 0) {
         pattern[0] = '\0';
      }
      return;
   }

   size_t j = 0;
   pattern[j++] = '%'; /* Leading wildcard */

   for (size_t i = 0; query[i] && j < pattern_size - 2; i++) {
      char c = query[i];
      /* Escape LIKE special characters: % _ \ */
      if (c == '%' || c == '_' || c == '\\') {
         if (j < pattern_size - 3) {
            pattern[j++] = '\\';
            pattern[j++] = c;
         }
      } else {
         pattern[j++] = c;
      }
   }

   if (j < pattern_size - 1) {
      pattern[j++] = '%'; /* Trailing wildcard */
   }
   pattern[j] = '\0';
}

int conv_db_search(int user_id,
                   const char *query,
                   const conv_pagination_t *pagination,
                   conversation_callback_t callback,
                   void *ctx) {
   if (user_id <= 0 || !query || !callback) {
      return AUTH_DB_INVALID;
   }

   int limit = CONV_LIST_DEFAULT_LIMIT;
   int offset = 0;
   if (pagination) {
      limit = (pagination->limit > 0 && pagination->limit <= CONV_LIST_MAX_LIMIT)
                  ? pagination->limit
                  : CONV_LIST_DEFAULT_LIMIT;
      offset = (pagination->offset >= 0) ? pagination->offset : 0;
   }

   /* Build escaped LIKE pattern: %query% with wildcards escaped */
   char pattern[CONV_TITLE_MAX * 2 + 3]; /* Worst case: every char escaped + %...% */
   build_like_pattern(query, pattern, sizeof(pattern));

   pthread_mutex_lock(&s_db.mutex);

   if (!s_db.initialized) {
      pthread_mutex_unlock(&s_db.mutex);
      return AUTH_DB_FAILURE;
   }

   sqlite3_reset(s_db.stmt_conv_search);
   sqlite3_bind_int(s_db.stmt_conv_search, 1, user_id);
   sqlite3_bind_text(s_db.stmt_conv_search, 2, pattern, -1, SQLITE_TRANSIENT);
   sqlite3_bind_int(s_db.stmt_conv_search, 3, limit);
   sqlite3_bind_int(s_db.stmt_conv_search, 4, offset);

   int rc;
   while ((rc = sqlite3_step(s_db.stmt_conv_search)) == SQLITE_ROW) {
      conversation_t conv = { 0 };

      conv.id = sqlite3_column_int64(s_db.stmt_conv_search, 0);
      conv.user_id = sqlite3_column_int(s_db.stmt_conv_search, 1);

      const char *title = (const char *)sqlite3_column_text(s_db.stmt_conv_search, 2);
      if (title) {
         strncpy(conv.title, title, CONV_TITLE_MAX - 1);
         conv.title[CONV_TITLE_MAX - 1] = '\0';
      }

      conv.created_at = (time_t)sqlite3_column_int64(s_db.stmt_conv_search, 3);
      conv.updated_at = (time_t)sqlite3_column_int64(s_db.stmt_conv_search, 4);
      conv.message_count = sqlite3_column_int(s_db.stmt_conv_search, 5);
      conv.is_archived = sqlite3_column_int(s_db.stmt_conv_search, 6) != 0;
      conv.context_tokens = sqlite3_column_int(s_db.stmt_conv_search, 7);
      conv.context_max = sqlite3_column_int(s_db.stmt_conv_search, 8);

      /* Continuation fields - only load continued_from for search results (chain indicator) */
      conv.continued_from = sqlite3_column_int64(s_db.stmt_conv_search, 9);
      conv.compaction_summary = NULL; /* Load on demand via conv_db_get */

      if (callback(&conv, ctx) != 0) {
         break;
      }
   }

   sqlite3_reset(s_db.stmt_conv_search);
   pthread_mutex_unlock(&s_db.mutex);

   return AUTH_DB_SUCCESS;
}

int conv_db_search_content(int user_id,
                           const char *query,
                           const conv_pagination_t *pagination,
                           conversation_callback_t callback,
                           void *ctx) {
   if (user_id <= 0 || !query || !callback) {
      return AUTH_DB_INVALID;
   }

   int limit = CONV_LIST_DEFAULT_LIMIT;
   int offset = 0;
   if (pagination) {
      limit = (pagination->limit > 0 && pagination->limit <= CONV_LIST_MAX_LIMIT)
                  ? pagination->limit
                  : CONV_LIST_DEFAULT_LIMIT;
      offset = (pagination->offset >= 0) ? pagination->offset : 0;
   }

   /* Build escaped LIKE pattern: %query% with wildcards escaped */
   char pattern[CONV_TITLE_MAX * 2 + 3]; /* Worst case: every char escaped + %...% */
   build_like_pattern(query, pattern, sizeof(pattern));

   pthread_mutex_lock(&s_db.mutex);

   if (!s_db.initialized) {
      pthread_mutex_unlock(&s_db.mutex);
      return AUTH_DB_FAILURE;
   }

   /* Use pre-prepared statement for content search */
   sqlite3_reset(s_db.stmt_conv_search_content);
   sqlite3_bind_int(s_db.stmt_conv_search_content, 1, user_id);
   sqlite3_bind_text(s_db.stmt_conv_search_content, 2, pattern, -1, SQLITE_TRANSIENT);
   sqlite3_bind_int(s_db.stmt_conv_search_content, 3, limit);
   sqlite3_bind_int(s_db.stmt_conv_search_content, 4, offset);

   int rc;
   while ((rc = sqlite3_step(s_db.stmt_conv_search_content)) == SQLITE_ROW) {
      conversation_t conv = { 0 };

      conv.id = sqlite3_column_int64(s_db.stmt_conv_search_content, 0);
      conv.user_id = sqlite3_column_int(s_db.stmt_conv_search_content, 1);

      const char *title = (const char *)sqlite3_column_text(s_db.stmt_conv_search_content, 2);
      if (title) {
         strncpy(conv.title, title, CONV_TITLE_MAX - 1);
         conv.title[CONV_TITLE_MAX - 1] = '\0';
      }

      conv.created_at = (time_t)sqlite3_column_int64(s_db.stmt_conv_search_content, 3);
      conv.updated_at = (time_t)sqlite3_column_int64(s_db.stmt_conv_search_content, 4);
      conv.message_count = sqlite3_column_int(s_db.stmt_conv_search_content, 5);
      conv.is_archived = sqlite3_column_int(s_db.stmt_conv_search_content, 6) != 0;
      conv.context_tokens = sqlite3_column_int(s_db.stmt_conv_search_content, 7);
      conv.context_max = sqlite3_column_int(s_db.stmt_conv_search_content, 8);

      /* Continuation fields - only load continued_from for search results (chain indicator) */
      conv.continued_from = sqlite3_column_int64(s_db.stmt_conv_search_content, 9);
      conv.compaction_summary = NULL; /* Load on demand via conv_db_get */

      if (callback(&conv, ctx) != 0) {
         break;
      }
   }

   sqlite3_reset(s_db.stmt_conv_search_content);
   pthread_mutex_unlock(&s_db.mutex);

   return AUTH_DB_SUCCESS;
}

int conv_db_update_context(int64_t conv_id, int user_id, int context_tokens, int context_max) {
   if (conv_id <= 0) {
      return AUTH_DB_INVALID;
   }

   pthread_mutex_lock(&s_db.mutex);

   if (!s_db.initialized) {
      pthread_mutex_unlock(&s_db.mutex);
      return AUTH_DB_FAILURE;
   }

   /* Use prepared statement with ownership check in WHERE clause */
   sqlite3_reset(s_db.stmt_conv_update_context);
   sqlite3_bind_int(s_db.stmt_conv_update_context, 1, context_tokens);
   sqlite3_bind_int(s_db.stmt_conv_update_context, 2, context_max);
   sqlite3_bind_int64(s_db.stmt_conv_update_context, 3, conv_id);
   sqlite3_bind_int(s_db.stmt_conv_update_context, 4, user_id);

   int rc = sqlite3_step(s_db.stmt_conv_update_context);
   sqlite3_reset(s_db.stmt_conv_update_context);

   if (rc != SQLITE_DONE) {
      pthread_mutex_unlock(&s_db.mutex);
      return AUTH_DB_FAILURE;
   }

   int changes = sqlite3_changes(s_db.db);
   pthread_mutex_unlock(&s_db.mutex);

   /* No rows updated = conversation not found or wrong owner */
   return (changes > 0) ? AUTH_DB_SUCCESS : AUTH_DB_NOT_FOUND;
}

int conv_db_add_message(int64_t conv_id, int user_id, const char *role, const char *content) {
   if (conv_id <= 0 || !role || !content) {
      return AUTH_DB_INVALID;
   }

   /* Validate role */
   if (strcmp(role, "system") != 0 && strcmp(role, "user") != 0 && strcmp(role, "assistant") != 0 &&
       strcmp(role, "tool") != 0) {
      return AUTH_DB_INVALID;
   }

   pthread_mutex_lock(&s_db.mutex);

   if (!s_db.initialized) {
      pthread_mutex_unlock(&s_db.mutex);
      return AUTH_DB_FAILURE;
   }

   time_t now = time(NULL);

   /* Insert message with ownership check in single query */
   sqlite3_reset(s_db.stmt_msg_add);
   sqlite3_bind_int64(s_db.stmt_msg_add, 1, conv_id);
   sqlite3_bind_text(s_db.stmt_msg_add, 2, role, -1, SQLITE_STATIC);
   sqlite3_bind_text(s_db.stmt_msg_add, 3, content, -1, SQLITE_STATIC);
   sqlite3_bind_int64(s_db.stmt_msg_add, 4, (int64_t)now);
   sqlite3_bind_int64(s_db.stmt_msg_add, 5, conv_id); /* For ownership check */
   sqlite3_bind_int(s_db.stmt_msg_add, 6, user_id);   /* For ownership check */

   int rc = sqlite3_step(s_db.stmt_msg_add);
   sqlite3_reset(s_db.stmt_msg_add);

   if (rc != SQLITE_DONE) {
      LOG_ERROR("conv_db_add_message: insert failed: %s", sqlite3_errmsg(s_db.db));
      pthread_mutex_unlock(&s_db.mutex);
      return AUTH_DB_FAILURE;
   }

   /* Check if message was actually inserted (ownership verification) */
   int changes = sqlite3_changes(s_db.db);
   if (changes == 0) {
      /* Distinguish between not found and forbidden:
       * Check if conversation exists but belongs to different user */
      const char *check_sql = "SELECT user_id FROM conversations WHERE id = ?";
      sqlite3_stmt *check_stmt;
      int check_rc = sqlite3_prepare_v2(s_db.db, check_sql, -1, &check_stmt, NULL);
      int result = AUTH_DB_NOT_FOUND;

      if (check_rc == SQLITE_OK) {
         sqlite3_bind_int64(check_stmt, 1, conv_id);
         if (sqlite3_step(check_stmt) == SQLITE_ROW) {
            /* Conversation exists but user doesn't own it */
            result = AUTH_DB_FORBIDDEN;
         }
         sqlite3_finalize(check_stmt);
      }

      pthread_mutex_unlock(&s_db.mutex);
      return result;
   }

   /* Update conversation metadata */
   sqlite3_reset(s_db.stmt_conv_update_meta);
   sqlite3_bind_int64(s_db.stmt_conv_update_meta, 1, (int64_t)now);
   sqlite3_bind_int64(s_db.stmt_conv_update_meta, 2, conv_id);

   rc = sqlite3_step(s_db.stmt_conv_update_meta);
   sqlite3_reset(s_db.stmt_conv_update_meta);

   pthread_mutex_unlock(&s_db.mutex);

   return (rc == SQLITE_DONE) ? AUTH_DB_SUCCESS : AUTH_DB_FAILURE;
}

int conv_db_get_messages(int64_t conv_id, int user_id, message_callback_t callback, void *ctx) {
   if (conv_id <= 0 || !callback) {
      return AUTH_DB_INVALID;
   }

   pthread_mutex_lock(&s_db.mutex);

   if (!s_db.initialized) {
      pthread_mutex_unlock(&s_db.mutex);
      return AUTH_DB_FAILURE;
   }

   /* Single query with ownership check via JOIN */
   sqlite3_reset(s_db.stmt_msg_get);
   sqlite3_bind_int64(s_db.stmt_msg_get, 1, conv_id);
   sqlite3_bind_int(s_db.stmt_msg_get, 2, user_id);

   int rc;
   while ((rc = sqlite3_step(s_db.stmt_msg_get)) == SQLITE_ROW) {
      conversation_message_t msg = { 0 };

      msg.id = sqlite3_column_int64(s_db.stmt_msg_get, 0);
      msg.conversation_id = sqlite3_column_int64(s_db.stmt_msg_get, 1);

      const char *role = (const char *)sqlite3_column_text(s_db.stmt_msg_get, 2);
      if (role) {
         strncpy(msg.role, role, CONV_ROLE_MAX - 1);
         msg.role[CONV_ROLE_MAX - 1] = '\0';
      }

      /* Content pointer is only valid during callback */
      msg.content = (char *)sqlite3_column_text(s_db.stmt_msg_get, 3);
      msg.created_at = (time_t)sqlite3_column_int64(s_db.stmt_msg_get, 4);

      if (callback(&msg, ctx) != 0) {
         break;
      }
   }

   sqlite3_reset(s_db.stmt_msg_get);
   pthread_mutex_unlock(&s_db.mutex);

   return AUTH_DB_SUCCESS;
}

int conv_db_get_messages_admin(int64_t conv_id, message_callback_t callback, void *ctx) {
   if (conv_id <= 0 || !callback) {
      return AUTH_DB_INVALID;
   }

   pthread_mutex_lock(&s_db.mutex);

   if (!s_db.initialized) {
      pthread_mutex_unlock(&s_db.mutex);
      return AUTH_DB_FAILURE;
   }

   sqlite3_reset(s_db.stmt_msg_get_admin);
   sqlite3_bind_int64(s_db.stmt_msg_get_admin, 1, conv_id);

   int rc;
   while ((rc = sqlite3_step(s_db.stmt_msg_get_admin)) == SQLITE_ROW) {
      conversation_message_t msg = { 0 };

      msg.id = sqlite3_column_int64(s_db.stmt_msg_get_admin, 0);
      msg.conversation_id = sqlite3_column_int64(s_db.stmt_msg_get_admin, 1);

      const char *role = (const char *)sqlite3_column_text(s_db.stmt_msg_get_admin, 2);
      if (role) {
         strncpy(msg.role, role, CONV_ROLE_MAX - 1);
         msg.role[CONV_ROLE_MAX - 1] = '\0';
      }

      msg.content = (char *)sqlite3_column_text(s_db.stmt_msg_get_admin, 3);
      msg.created_at = (time_t)sqlite3_column_int64(s_db.stmt_msg_get_admin, 4);

      if (callback(&msg, ctx) != 0) {
         break;
      }
   }

   sqlite3_reset(s_db.stmt_msg_get_admin);
   pthread_mutex_unlock(&s_db.mutex);

   return AUTH_DB_SUCCESS;
}

int conv_db_count(int user_id) {
   if (user_id <= 0) {
      return -1;
   }

   pthread_mutex_lock(&s_db.mutex);

   if (!s_db.initialized) {
      pthread_mutex_unlock(&s_db.mutex);
      return -1;
   }

   sqlite3_reset(s_db.stmt_conv_count);
   sqlite3_bind_int(s_db.stmt_conv_count, 1, user_id);

   int count = -1;
   if (sqlite3_step(s_db.stmt_conv_count) == SQLITE_ROW) {
      count = sqlite3_column_int(s_db.stmt_conv_count, 0);
   }

   sqlite3_reset(s_db.stmt_conv_count);
   pthread_mutex_unlock(&s_db.mutex);

   return count;
}

int conv_db_find_continuation(int64_t parent_id, int user_id, int64_t *continuation_id_out) {
   if (parent_id <= 0 || user_id <= 0 || !continuation_id_out) {
      return AUTH_DB_FAILURE;
   }

   *continuation_id_out = 0;

   pthread_mutex_lock(&s_db.mutex);

   if (!s_db.initialized) {
      pthread_mutex_unlock(&s_db.mutex);
      return AUTH_DB_FAILURE;
   }

   /* Find conversation where continued_from = parent_id and user_id matches */
   const char *sql = "SELECT id FROM conversations "
                     "WHERE continued_from = ? AND user_id = ? "
                     "ORDER BY created_at DESC LIMIT 1";

   sqlite3_stmt *stmt = NULL;
   int rc = sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      LOG_ERROR("auth_db: prepare find_continuation failed: %s", sqlite3_errmsg(s_db.db));
      pthread_mutex_unlock(&s_db.mutex);
      return AUTH_DB_FAILURE;
   }

   sqlite3_bind_int64(stmt, 1, parent_id);
   sqlite3_bind_int(stmt, 2, user_id);

   int result = AUTH_DB_NOT_FOUND;
   if (sqlite3_step(stmt) == SQLITE_ROW) {
      *continuation_id_out = sqlite3_column_int64(stmt, 0);
      result = AUTH_DB_SUCCESS;
   }

   sqlite3_finalize(stmt);
   pthread_mutex_unlock(&s_db.mutex);

   return result;
}

void conv_generate_title(const char *content, char *title_out, size_t max_len) {
   if (!content || !title_out || max_len == 0) {
      if (title_out && max_len > 0) {
         title_out[0] = '\0';
      }
      return;
   }

   /* Skip leading whitespace */
   while (*content && (*content == ' ' || *content == '\t' || *content == '\n')) {
      content++;
   }

   /* Target ~50 chars, but truncate at word boundary */
   size_t target_len = 50;
   if (target_len >= max_len) {
      target_len = max_len - 4; /* Leave room for "..." */
   }

   size_t content_len = strlen(content);
   if (content_len <= target_len) {
      /* Content fits entirely */
      strncpy(title_out, content, max_len - 1);
      title_out[max_len - 1] = '\0';

      /* Remove trailing newlines */
      size_t len = strlen(title_out);
      while (len > 0 && (title_out[len - 1] == '\n' || title_out[len - 1] == '\r')) {
         title_out[--len] = '\0';
      }
      return;
   }

   /* Find last word boundary before target_len */
   size_t cut_pos = target_len;
   while (cut_pos > 0 && content[cut_pos] != ' ' && content[cut_pos] != '\t') {
      cut_pos--;
   }

   /* If no word boundary found, just cut at target_len */
   if (cut_pos == 0) {
      cut_pos = target_len;
   }

   /* Copy and add ellipsis */
   strncpy(title_out, content, cut_pos);
   title_out[cut_pos] = '\0';

   /* Trim trailing whitespace before ellipsis */
   while (cut_pos > 0 && (title_out[cut_pos - 1] == ' ' || title_out[cut_pos - 1] == '\t')) {
      title_out[--cut_pos] = '\0';
   }

   /* Add ellipsis if there's room */
   if (cut_pos + 3 < max_len) {
      strcat(title_out, "...");
   }
}

/* ============================================================================
 * Session Metrics Operations
 * ============================================================================ */

int auth_db_save_session_metrics(session_metrics_t *metrics) {
   if (!metrics) {
      return AUTH_DB_INVALID;
   }

   pthread_mutex_lock(&s_db.mutex);

   if (!s_db.initialized) {
      pthread_mutex_unlock(&s_db.mutex);
      return AUTH_DB_FAILURE;
   }

   int rc;

   if (metrics->id > 0) {
      /* UPDATE existing row (per-query update case) */
      sqlite3_reset(s_db.stmt_metrics_update);

      /* Bind 12 parameters: 11 update values + 1 id for WHERE */
      sqlite3_bind_int64(s_db.stmt_metrics_update, 1, (sqlite3_int64)metrics->ended_at);
      sqlite3_bind_int(s_db.stmt_metrics_update, 2, (int)metrics->queries_total);
      sqlite3_bind_int(s_db.stmt_metrics_update, 3, (int)metrics->queries_cloud);
      sqlite3_bind_int(s_db.stmt_metrics_update, 4, (int)metrics->queries_local);
      sqlite3_bind_int(s_db.stmt_metrics_update, 5, (int)metrics->errors_count);
      sqlite3_bind_int(s_db.stmt_metrics_update, 6, (int)metrics->fallbacks_count);
      sqlite3_bind_double(s_db.stmt_metrics_update, 7, metrics->avg_asr_ms);
      sqlite3_bind_double(s_db.stmt_metrics_update, 8, metrics->avg_llm_ttft_ms);
      sqlite3_bind_double(s_db.stmt_metrics_update, 9, metrics->avg_llm_total_ms);
      sqlite3_bind_double(s_db.stmt_metrics_update, 10, metrics->avg_tts_ms);
      sqlite3_bind_double(s_db.stmt_metrics_update, 11, metrics->avg_pipeline_ms);
      sqlite3_bind_int64(s_db.stmt_metrics_update, 12, metrics->id);

      rc = sqlite3_step(s_db.stmt_metrics_update);
      sqlite3_reset(s_db.stmt_metrics_update);

      if (rc != SQLITE_DONE) {
         LOG_ERROR("auth_db: failed to update session metrics: %s", sqlite3_errmsg(s_db.db));
         pthread_mutex_unlock(&s_db.mutex);
         return AUTH_DB_FAILURE;
      }
   } else {
      /* INSERT new row (first query in session) */
      sqlite3_reset(s_db.stmt_metrics_save);

      /* Bind all 15 parameters (token usage is in provider table) */
      sqlite3_bind_int(s_db.stmt_metrics_save, 1, (int)metrics->session_id);
      if (metrics->user_id > 0) {
         sqlite3_bind_int(s_db.stmt_metrics_save, 2, metrics->user_id);
      } else {
         sqlite3_bind_null(s_db.stmt_metrics_save, 2);
      }
      sqlite3_bind_text(s_db.stmt_metrics_save, 3, metrics->session_type, -1, SQLITE_TRANSIENT);
      sqlite3_bind_int64(s_db.stmt_metrics_save, 4, (sqlite3_int64)metrics->started_at);
      sqlite3_bind_int64(s_db.stmt_metrics_save, 5, (sqlite3_int64)metrics->ended_at);

      /* Query counts */
      sqlite3_bind_int(s_db.stmt_metrics_save, 6, (int)metrics->queries_total);
      sqlite3_bind_int(s_db.stmt_metrics_save, 7, (int)metrics->queries_cloud);
      sqlite3_bind_int(s_db.stmt_metrics_save, 8, (int)metrics->queries_local);
      sqlite3_bind_int(s_db.stmt_metrics_save, 9, (int)metrics->errors_count);
      sqlite3_bind_int(s_db.stmt_metrics_save, 10, (int)metrics->fallbacks_count);

      /* Performance averages */
      sqlite3_bind_double(s_db.stmt_metrics_save, 11, metrics->avg_asr_ms);
      sqlite3_bind_double(s_db.stmt_metrics_save, 12, metrics->avg_llm_ttft_ms);
      sqlite3_bind_double(s_db.stmt_metrics_save, 13, metrics->avg_llm_total_ms);
      sqlite3_bind_double(s_db.stmt_metrics_save, 14, metrics->avg_tts_ms);
      sqlite3_bind_double(s_db.stmt_metrics_save, 15, metrics->avg_pipeline_ms);

      rc = sqlite3_step(s_db.stmt_metrics_save);
      sqlite3_reset(s_db.stmt_metrics_save);

      if (rc != SQLITE_DONE) {
         LOG_ERROR("auth_db: failed to save session metrics: %s", sqlite3_errmsg(s_db.db));
         pthread_mutex_unlock(&s_db.mutex);
         return AUTH_DB_FAILURE;
      }

      /* Get the inserted row ID */
      metrics->id = sqlite3_last_insert_rowid(s_db.db);

      LOG_INFO("auth_db: created session metrics (id=%lld, session=%u, type=%s)",
               (long long)metrics->id, metrics->session_id, metrics->session_type);
   }

   pthread_mutex_unlock(&s_db.mutex);
   return AUTH_DB_SUCCESS;
}

int auth_db_save_provider_metrics(int64_t session_metrics_id,
                                  const session_provider_metrics_t *providers,
                                  int count) {
   if (!providers || count <= 0 || session_metrics_id <= 0) {
      return AUTH_DB_INVALID;
   }

   pthread_mutex_lock(&s_db.mutex);

   if (!s_db.initialized) {
      pthread_mutex_unlock(&s_db.mutex);
      return AUTH_DB_FAILURE;
   }

   /* Delete existing provider metrics before re-inserting (for per-query updates) */
   sqlite3_reset(s_db.stmt_provider_metrics_delete);
   sqlite3_bind_int64(s_db.stmt_provider_metrics_delete, 1, session_metrics_id);
   int rc = sqlite3_step(s_db.stmt_provider_metrics_delete);
   sqlite3_reset(s_db.stmt_provider_metrics_delete);
   if (rc != SQLITE_DONE) {
      LOG_WARNING("auth_db: failed to delete old provider metrics: %s", sqlite3_errmsg(s_db.db));
   }

   int saved = 0;
   for (int i = 0; i < count && i < MAX_PROVIDERS_PER_SESSION; i++) {
      const session_provider_metrics_t *p = &providers[i];

      /* Skip entries with no provider name or no data */
      if (!p->provider[0] || (p->tokens_input == 0 && p->tokens_output == 0 && p->queries == 0)) {
         continue;
      }

      sqlite3_reset(s_db.stmt_provider_metrics_save);
      sqlite3_bind_int64(s_db.stmt_provider_metrics_save, 1, session_metrics_id);
      sqlite3_bind_text(s_db.stmt_provider_metrics_save, 2, p->provider, -1, SQLITE_TRANSIENT);
      sqlite3_bind_int64(s_db.stmt_provider_metrics_save, 3, (sqlite3_int64)p->tokens_input);
      sqlite3_bind_int64(s_db.stmt_provider_metrics_save, 4, (sqlite3_int64)p->tokens_output);
      sqlite3_bind_int64(s_db.stmt_provider_metrics_save, 5, (sqlite3_int64)p->tokens_cached);
      sqlite3_bind_int(s_db.stmt_provider_metrics_save, 6, (int)p->queries);

      int rc = sqlite3_step(s_db.stmt_provider_metrics_save);
      sqlite3_reset(s_db.stmt_provider_metrics_save);

      if (rc != SQLITE_DONE) {
         LOG_WARNING("auth_db: failed to save provider metrics for %s: %s", p->provider,
                     sqlite3_errmsg(s_db.db));
      } else {
         saved++;
      }
   }

   if (saved > 0) {
      LOG_INFO("auth_db: saved %d provider metrics for session_metrics_id=%lld", saved,
               (long long)session_metrics_id);
   }

   pthread_mutex_unlock(&s_db.mutex);
   return AUTH_DB_SUCCESS;
}

int auth_db_list_session_metrics(const session_metrics_filter_t *filter,
                                 session_metrics_callback_t callback,
                                 void *ctx) {
   if (!callback) {
      return AUTH_DB_INVALID;
   }

   pthread_mutex_lock(&s_db.mutex);

   if (!s_db.initialized) {
      pthread_mutex_unlock(&s_db.mutex);
      return AUTH_DB_FAILURE;
   }

   /* Build dynamic query with filters (token usage is in provider table) */
   char sql[1024];
   int offset = snprintf(
       sql, sizeof(sql),
       "SELECT id, session_id, user_id, session_type, started_at, ended_at, "
       "queries_total, queries_cloud, queries_local, errors_count, fallbacks_count, "
       "avg_asr_ms, avg_llm_ttft_ms, avg_llm_total_ms, avg_tts_ms, avg_pipeline_ms "
       "FROM session_metrics WHERE 1=1");

   if (filter && filter->user_id > 0) {
      offset += snprintf(sql + offset, sizeof(sql) - offset, " AND user_id = %d", filter->user_id);
   }
   if (filter && filter->type) {
      offset += snprintf(sql + offset, sizeof(sql) - offset, " AND session_type = '%s'",
                         filter->type);
   }
   if (filter && filter->since > 0) {
      offset += snprintf(sql + offset, sizeof(sql) - offset, " AND started_at >= %ld",
                         (long)filter->since);
   }
   if (filter && filter->until > 0) {
      offset += snprintf(sql + offset, sizeof(sql) - offset, " AND started_at <= %ld",
                         (long)filter->until);
   }

   offset += snprintf(sql + offset, sizeof(sql) - offset, " ORDER BY started_at DESC");

   int limit = (filter && filter->limit > 0) ? filter->limit : 20;
   int skip = (filter && filter->offset > 0) ? filter->offset : 0;
   snprintf(sql + offset, sizeof(sql) - offset, " LIMIT %d OFFSET %d", limit, skip);

   sqlite3_stmt *stmt = NULL;
   int rc = sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      LOG_ERROR("auth_db: failed to prepare metrics query: %s", sqlite3_errmsg(s_db.db));
      pthread_mutex_unlock(&s_db.mutex);
      return AUTH_DB_FAILURE;
   }

   int result = AUTH_DB_SUCCESS;
   while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
      session_metrics_t m = { 0 };

      m.id = sqlite3_column_int64(stmt, 0);
      m.session_id = (uint32_t)sqlite3_column_int(stmt, 1);
      m.user_id = sqlite3_column_int(stmt, 2);

      const char *type = (const char *)sqlite3_column_text(stmt, 3);
      if (type) {
         strncpy(m.session_type, type, sizeof(m.session_type) - 1);
      }

      m.started_at = (time_t)sqlite3_column_int64(stmt, 4);
      m.ended_at = (time_t)sqlite3_column_int64(stmt, 5);

      m.queries_total = (uint32_t)sqlite3_column_int(stmt, 6);
      m.queries_cloud = (uint32_t)sqlite3_column_int(stmt, 7);
      m.queries_local = (uint32_t)sqlite3_column_int(stmt, 8);
      m.errors_count = (uint32_t)sqlite3_column_int(stmt, 9);
      m.fallbacks_count = (uint32_t)sqlite3_column_int(stmt, 10);

      /* Performance averages (token usage is queried separately from provider table) */
      m.avg_asr_ms = sqlite3_column_double(stmt, 11);
      m.avg_llm_ttft_ms = sqlite3_column_double(stmt, 12);
      m.avg_llm_total_ms = sqlite3_column_double(stmt, 13);
      m.avg_tts_ms = sqlite3_column_double(stmt, 14);
      m.avg_pipeline_ms = sqlite3_column_double(stmt, 15);

      if (callback(&m, ctx) != 0) {
         break; /* Callback requested stop */
      }
   }

   sqlite3_finalize(stmt);
   pthread_mutex_unlock(&s_db.mutex);
   return result;
}

int auth_db_get_metrics_aggregate(const session_metrics_filter_t *filter,
                                  session_metrics_t *totals) {
   if (!totals) {
      return AUTH_DB_INVALID;
   }

   memset(totals, 0, sizeof(*totals));

   pthread_mutex_lock(&s_db.mutex);

   if (!s_db.initialized) {
      pthread_mutex_unlock(&s_db.mutex);
      return AUTH_DB_FAILURE;
   }

   /* Build aggregate query (token usage must be aggregated from provider table separately) */
   char sql[1024];
   int offset = snprintf(sql, sizeof(sql),
                         "SELECT "
                         "COUNT(*), "
                         "SUM(queries_total), SUM(queries_cloud), SUM(queries_local), "
                         "SUM(errors_count), SUM(fallbacks_count), "
                         "AVG(avg_asr_ms), AVG(avg_llm_ttft_ms), AVG(avg_llm_total_ms), "
                         "AVG(avg_tts_ms), AVG(avg_pipeline_ms) "
                         "FROM session_metrics WHERE 1=1");

   if (filter && filter->user_id > 0) {
      offset += snprintf(sql + offset, sizeof(sql) - offset, " AND user_id = %d", filter->user_id);
   }
   if (filter && filter->type) {
      offset += snprintf(sql + offset, sizeof(sql) - offset, " AND session_type = '%s'",
                         filter->type);
   }
   if (filter && filter->since > 0) {
      offset += snprintf(sql + offset, sizeof(sql) - offset, " AND started_at >= %ld",
                         (long)filter->since);
   }
   if (filter && filter->until > 0) {
      offset += snprintf(sql + offset, sizeof(sql) - offset, " AND started_at <= %ld",
                         (long)filter->until);
   }

   sqlite3_stmt *stmt = NULL;
   int rc = sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      LOG_ERROR("auth_db: failed to prepare metrics aggregate query: %s", sqlite3_errmsg(s_db.db));
      pthread_mutex_unlock(&s_db.mutex);
      return AUTH_DB_FAILURE;
   }

   if (sqlite3_step(stmt) == SQLITE_ROW) {
      /* session_id stores the count of sessions for aggregate */
      totals->session_id = (uint32_t)sqlite3_column_int(stmt, 0);

      totals->queries_total = (uint32_t)sqlite3_column_int(stmt, 1);
      totals->queries_cloud = (uint32_t)sqlite3_column_int(stmt, 2);
      totals->queries_local = (uint32_t)sqlite3_column_int(stmt, 3);
      totals->errors_count = (uint32_t)sqlite3_column_int(stmt, 4);
      totals->fallbacks_count = (uint32_t)sqlite3_column_int(stmt, 5);

      /* Performance averages */
      totals->avg_asr_ms = sqlite3_column_double(stmt, 6);
      totals->avg_llm_ttft_ms = sqlite3_column_double(stmt, 7);
      totals->avg_llm_total_ms = sqlite3_column_double(stmt, 8);
      totals->avg_tts_ms = sqlite3_column_double(stmt, 9);
      totals->avg_pipeline_ms = sqlite3_column_double(stmt, 10);
   }

   sqlite3_finalize(stmt);
   pthread_mutex_unlock(&s_db.mutex);
   return AUTH_DB_SUCCESS;
}

int auth_db_cleanup_session_metrics(int retention_days) {
   if (retention_days <= 0) {
      retention_days = SESSION_METRICS_RETENTION_DAYS;
   }

   pthread_mutex_lock(&s_db.mutex);

   if (!s_db.initialized) {
      pthread_mutex_unlock(&s_db.mutex);
      return -1;
   }

   time_t cutoff = time(NULL) - (retention_days * 24 * 60 * 60);

   sqlite3_reset(s_db.stmt_metrics_delete_old);
   sqlite3_bind_int64(s_db.stmt_metrics_delete_old, 1, (sqlite3_int64)cutoff);

   int rc = sqlite3_step(s_db.stmt_metrics_delete_old);
   sqlite3_reset(s_db.stmt_metrics_delete_old);

   if (rc != SQLITE_DONE) {
      LOG_ERROR("auth_db: failed to cleanup old metrics: %s", sqlite3_errmsg(s_db.db));
      pthread_mutex_unlock(&s_db.mutex);
      return -1;
   }

   int deleted = sqlite3_changes(s_db.db);
   if (deleted > 0) {
      LOG_INFO("auth_db: cleaned up %d old session metrics (older than %d days)", deleted,
               retention_days);
   }

   pthread_mutex_unlock(&s_db.mutex);
   return deleted;
}
