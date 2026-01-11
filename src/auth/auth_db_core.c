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
 * Authentication Database Core Module
 *
 * Provides database lifecycle management, schema creation, and prepared
 * statement initialization. This module defines the shared s_db state
 * used by all other auth_db modules.
 *
 * SECURITY: All database operations use prepared statements.
 * NEVER use sqlite3_exec() or sqlite3_mprintf() with user input.
 * See: CWE-89, OWASP SQL Injection Prevention Cheat Sheet
 */

#define AUTH_DB_INTERNAL_ALLOWED
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <limits.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "auth/auth_db_internal.h"
#include "logging.h"

/* =============================================================================
 * Shared State Definition
 * ============================================================================= */

auth_db_state_t s_db = {
   .db = NULL,
   .mutex = PTHREAD_MUTEX_INITIALIZER,
   .initialized = false,
   .last_cleanup = 0,
   .last_vacuum = 0,
};

/* =============================================================================
 * Forward Declarations
 * ============================================================================= */

static int create_schema(void);
static int prepare_statements(void);
static void finalize_statements(void);

/* =============================================================================
 * Schema SQL
 * ============================================================================= */

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

/* =============================================================================
 * Schema Version and Migration
 * ============================================================================= */

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
   if (current_version > 0 && current_version < AUTH_DB_SCHEMA_VERSION) {
      LOG_INFO("auth_db: migrated schema from v%d to v%d", current_version, AUTH_DB_SCHEMA_VERSION);
   } else if (current_version == 0) {
      LOG_INFO("auth_db: created schema v%d", AUTH_DB_SCHEMA_VERSION);
   }

   /* Update schema version (delete old rows first to handle PRIMARY KEY on version) */
   rc = sqlite3_exec(s_db.db, "DELETE FROM schema_version", NULL, NULL, &errmsg);
   if (rc != SQLITE_OK) {
      LOG_WARNING("auth_db: failed to clear schema_version: %s", errmsg ? errmsg : "unknown");
      sqlite3_free(errmsg);
      errmsg = NULL;
   }
   rc = sqlite3_exec(s_db.db,
                     "INSERT INTO schema_version (version) VALUES (" STRINGIFY(
                         AUTH_DB_SCHEMA_VERSION) ")",
                     NULL, NULL, &errmsg);
   if (rc != SQLITE_OK) {
      LOG_ERROR("auth_db: failed to set schema version: %s", errmsg ? errmsg : "unknown");
      sqlite3_free(errmsg);
      return AUTH_DB_FAILURE;
   }

   return AUTH_DB_SUCCESS;
}

/* =============================================================================
 * Prepared Statement Management
 * ============================================================================= */

static int prepare_statements(void) {
   int rc;

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

   /* Clear all statement pointers using offsetof for safety
    * MAINTENANCE: If statements are reordered, update first/last_stmt names */
   size_t first_stmt_offset = offsetof(auth_db_state_t, stmt_create_user);
   size_t last_stmt_end = offsetof(auth_db_state_t, stmt_provider_metrics_delete) +
                          sizeof(sqlite3_stmt *);
   memset((char *)&s_db + first_stmt_offset, 0, last_stmt_end - first_stmt_offset);
}

/* =============================================================================
 * File Permission Helpers
 * ============================================================================= */

int auth_db_internal_create_parent_dir(const char *path) {
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

int auth_db_internal_verify_permissions(const char *path) {
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

/* =============================================================================
 * Lifecycle Functions
 * ============================================================================= */

int auth_db_init(const char *db_path) {
   pthread_mutex_lock(&s_db.mutex);

   if (s_db.initialized) {
      LOG_WARNING("auth_db_init: already initialized");
      pthread_mutex_unlock(&s_db.mutex);
      return AUTH_DB_SUCCESS;
   }

   const char *path = db_path ? db_path : AUTH_DB_DEFAULT_PATH;

   /* Create parent directory with secure permissions */
   if (auth_db_internal_create_parent_dir(path) != AUTH_DB_SUCCESS) {
      pthread_mutex_unlock(&s_db.mutex);
      return AUTH_DB_FAILURE;
   }

   /* Check existing file permissions */
   if (auth_db_internal_verify_permissions(path) != AUTH_DB_SUCCESS) {
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
   if (auth_db_internal_verify_permissions(path) != AUTH_DB_SUCCESS) {
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

   /* Set conservative cache size for embedded systems (64 pages × 4KB = 256KB) */
   sqlite3_exec(s_db.db, "PRAGMA cache_size=64", NULL, NULL, NULL);

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
