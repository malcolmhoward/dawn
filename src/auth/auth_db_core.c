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
#include <math.h>
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
    "   expires_at INTEGER,"
    "   ip_address TEXT,"
    "   user_agent TEXT,"
    "   FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_sessions_user ON sessions(user_id);"
    "CREATE INDEX IF NOT EXISTS idx_sessions_activity ON sessions(last_activity);"
    /* idx_sessions_expires created by v10 migration after expires_at column is added */

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

    /* Conversations table (added in schema v4, context columns in v5, continuation in v7,
     * LLM settings in v11, extraction tracking in v15, privacy in v16, origin in v17) */
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
    "   llm_type TEXT DEFAULT NULL,"
    "   cloud_provider TEXT DEFAULT NULL,"
    "   model TEXT DEFAULT NULL,"
    "   tools_mode TEXT DEFAULT NULL,"
    "   thinking_mode TEXT DEFAULT NULL,"
    "   last_extracted_msg_count INTEGER DEFAULT 0,"
    "   is_private INTEGER DEFAULT 0,"
    "   origin TEXT DEFAULT 'webui',"
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
    "session_metrics_providers(session_metrics_id);"

    /* Images table for vision uploads (added in schema v12) */
    "CREATE TABLE IF NOT EXISTS images ("
    "   id TEXT PRIMARY KEY,"
    "   user_id INTEGER NOT NULL,"
    "   mime_type TEXT NOT NULL,"
    "   size INTEGER NOT NULL,"
    "   data BLOB NOT NULL,"
    "   created_at INTEGER NOT NULL,"
    "   last_accessed INTEGER,"
    "   FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_images_user ON images(user_id);"
    "CREATE INDEX IF NOT EXISTS idx_images_created ON images(created_at);";

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

   /* v10 migration: add expires_at column to sessions for "Remember Me" feature
    * Existing sessions get expires_at = last_activity + 24 hours */
   if (current_version >= 1 && current_version < 10) {
      rc = sqlite3_exec(s_db.db, "ALTER TABLE sessions ADD COLUMN expires_at INTEGER", NULL, NULL,
                        &errmsg);
      if (rc != SQLITE_OK) {
         LOG_INFO("auth_db: v10 migration note (expires_at): %s", errmsg ? errmsg : "ok");
         sqlite3_free(errmsg);
         errmsg = NULL;
      } else {
         /* Set default expires_at for existing sessions (last_activity + 24h) */
         char update_sql[128];
         snprintf(update_sql, sizeof(update_sql),
                  "UPDATE sessions SET expires_at = last_activity + %d WHERE expires_at IS NULL",
                  AUTH_SESSION_TIMEOUT_SEC);
         rc = sqlite3_exec(s_db.db, update_sql, NULL, NULL, &errmsg);
         if (rc != SQLITE_OK) {
            LOG_WARNING("auth_db: v10 migration (set defaults): %s", errmsg ? errmsg : "ok");
            sqlite3_free(errmsg);
            errmsg = NULL;
         }
         LOG_INFO("auth_db: added expires_at column to sessions (v10)");
      }
      /* Create index for efficient cleanup queries */
      rc = sqlite3_exec(s_db.db,
                        "CREATE INDEX IF NOT EXISTS idx_sessions_expires ON sessions(expires_at)",
                        NULL, NULL, &errmsg);
      if (rc != SQLITE_OK) {
         LOG_INFO("auth_db: v10 migration (index): %s", errmsg ? errmsg : "ok");
         sqlite3_free(errmsg);
         errmsg = NULL;
      }
   }

   /* v11 migration: add per-conversation LLM settings columns */
   if (current_version >= 4 && current_version < 11) {
      const char *cols[] = {
         "ALTER TABLE conversations ADD COLUMN llm_type TEXT DEFAULT NULL",
         "ALTER TABLE conversations ADD COLUMN cloud_provider TEXT DEFAULT NULL",
         "ALTER TABLE conversations ADD COLUMN model TEXT DEFAULT NULL",
         "ALTER TABLE conversations ADD COLUMN tools_mode TEXT DEFAULT NULL",
         "ALTER TABLE conversations ADD COLUMN thinking_mode TEXT DEFAULT NULL"
      };
      for (int i = 0; i < 5; i++) {
         rc = sqlite3_exec(s_db.db, cols[i], NULL, NULL, &errmsg);
         if (rc != SQLITE_OK) {
            LOG_INFO("auth_db: v11 migration note: %s", errmsg ? errmsg : "ok");
            sqlite3_free(errmsg);
            errmsg = NULL;
         }
      }
      LOG_INFO("auth_db: added LLM settings columns to conversations (v11)");
   }

   /* v12 migration: images table for vision uploads (now superseded by v13) */
   if (current_version >= 1 && current_version < 12) {
      LOG_INFO("auth_db: added images table for vision uploads (v12)");
   }

   /* v13 migration: add data BLOB column to images table
    * Since v12 images table didn't have the data column, we need to recreate it.
    * Drop existing table (likely empty) and let SCHEMA_SQL recreate with data column. */
   if (current_version == 12) {
      rc = sqlite3_exec(s_db.db, "DROP TABLE IF EXISTS images", NULL, NULL, &errmsg);
      if (rc != SQLITE_OK) {
         LOG_WARNING("auth_db: v13 migration - failed to drop images: %s", errmsg ? errmsg : "ok");
         sqlite3_free(errmsg);
         errmsg = NULL;
      }
      /* Recreate with data column (from SCHEMA_SQL) */
      const char *images_sql =
          "CREATE TABLE IF NOT EXISTS images ("
          "   id TEXT PRIMARY KEY,"
          "   user_id INTEGER NOT NULL,"
          "   mime_type TEXT NOT NULL,"
          "   size INTEGER NOT NULL,"
          "   data BLOB NOT NULL,"
          "   created_at INTEGER NOT NULL,"
          "   last_accessed INTEGER,"
          "   FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE"
          ");"
          "CREATE INDEX IF NOT EXISTS idx_images_user ON images(user_id);"
          "CREATE INDEX IF NOT EXISTS idx_images_created ON images(created_at);";
      rc = sqlite3_exec(s_db.db, images_sql, NULL, NULL, &errmsg);
      if (rc != SQLITE_OK) {
         LOG_ERROR("auth_db: v13 migration - failed to create images: %s", errmsg ? errmsg : "ok");
         sqlite3_free(errmsg);
         return AUTH_DB_FAILURE;
      }
      LOG_INFO("auth_db: migrated images table to include BLOB storage (v13)");
   }

   /* v14 migration: add memory system tables
    * Creates memory_facts, memory_preferences, and memory_summaries tables */
   if (current_version >= 1 && current_version < 14) {
      const char *memory_sql =
          /* memory_facts table */
          "CREATE TABLE IF NOT EXISTS memory_facts ("
          "   id INTEGER PRIMARY KEY AUTOINCREMENT,"
          "   user_id INTEGER NOT NULL,"
          "   fact_text TEXT NOT NULL,"
          "   confidence REAL DEFAULT 1.0,"
          "   source TEXT DEFAULT 'inferred',"
          "   created_at INTEGER NOT NULL DEFAULT (strftime('%s','now')),"
          "   last_accessed INTEGER,"
          "   access_count INTEGER DEFAULT 0,"
          "   superseded_by INTEGER,"
          "   FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE,"
          "   FOREIGN KEY (superseded_by) REFERENCES memory_facts(id) ON DELETE SET NULL"
          ");"
          "CREATE INDEX IF NOT EXISTS idx_memory_facts_user ON memory_facts(user_id);"
          "CREATE INDEX IF NOT EXISTS idx_memory_facts_confidence ON "
          "memory_facts(user_id, confidence DESC);"

          /* memory_preferences table */
          "CREATE TABLE IF NOT EXISTS memory_preferences ("
          "   id INTEGER PRIMARY KEY AUTOINCREMENT,"
          "   user_id INTEGER NOT NULL,"
          "   category TEXT NOT NULL,"
          "   value TEXT NOT NULL,"
          "   confidence REAL DEFAULT 0.5,"
          "   source TEXT DEFAULT 'inferred',"
          "   created_at INTEGER NOT NULL DEFAULT (strftime('%s','now')),"
          "   updated_at INTEGER NOT NULL DEFAULT (strftime('%s','now')),"
          "   reinforcement_count INTEGER DEFAULT 1,"
          "   FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE,"
          "   UNIQUE(user_id, category)"
          ");"

          /* memory_summaries table */
          "CREATE TABLE IF NOT EXISTS memory_summaries ("
          "   id INTEGER PRIMARY KEY AUTOINCREMENT,"
          "   user_id INTEGER NOT NULL,"
          "   session_id TEXT NOT NULL,"
          "   summary TEXT NOT NULL,"
          "   topics TEXT,"
          "   sentiment TEXT,"
          "   created_at INTEGER NOT NULL DEFAULT (strftime('%s','now')),"
          "   message_count INTEGER,"
          "   duration_seconds INTEGER,"
          "   consolidated INTEGER DEFAULT 0,"
          "   FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE"
          ");"
          "CREATE INDEX IF NOT EXISTS idx_memory_summaries_user ON "
          "memory_summaries(user_id, created_at DESC);";

      rc = sqlite3_exec(s_db.db, memory_sql, NULL, NULL, &errmsg);
      if (rc != SQLITE_OK) {
         LOG_ERROR("auth_db: v14 migration - failed to create memory tables: %s",
                   errmsg ? errmsg : "unknown");
         sqlite3_free(errmsg);
         return AUTH_DB_FAILURE;
      }
      LOG_INFO("auth_db: added memory system tables (v14)");
   }

   /* v15 migration: add deduplication and extraction tracking
    * - normalized_hash for fast duplicate detection in memory_facts
    * - last_extracted_msg_count for incremental extraction in conversations */
   if (current_version >= 1 && current_version < 15) {
      const char *v15_sql =
          "ALTER TABLE memory_facts ADD COLUMN normalized_hash INTEGER DEFAULT 0;"
          "CREATE INDEX IF NOT EXISTS idx_memory_facts_hash ON memory_facts(user_id, "
          "normalized_hash);"
          "ALTER TABLE conversations ADD COLUMN last_extracted_msg_count INTEGER DEFAULT 0;";

      rc = sqlite3_exec(s_db.db, v15_sql, NULL, NULL, &errmsg);
      if (rc != SQLITE_OK) {
         LOG_ERROR("auth_db: v15 migration failed: %s", errmsg ? errmsg : "unknown");
         sqlite3_free(errmsg);
         return AUTH_DB_FAILURE;
      }
      LOG_INFO("auth_db: added deduplication and extraction tracking (v15)");
   }

   /* v16 migration: add is_private flag to conversations for privacy mode */
   if (current_version >= 1 && current_version < 16) {
      const char *v16_sql = "ALTER TABLE conversations ADD COLUMN is_private INTEGER DEFAULT 0;";

      rc = sqlite3_exec(s_db.db, v16_sql, NULL, NULL, &errmsg);
      if (rc != SQLITE_OK) {
         LOG_ERROR("auth_db: v16 migration failed: %s", errmsg ? errmsg : "unknown");
         sqlite3_free(errmsg);
         return AUTH_DB_FAILURE;
      }
      LOG_INFO("auth_db: added conversation privacy flag (v16)");
   }

   /* v17 migration: add origin column to conversations for voice/webui distinction */
   if (current_version >= 1 && current_version < 17) {
      const char *v17_sql = "ALTER TABLE conversations ADD COLUMN origin TEXT DEFAULT 'webui';";

      rc = sqlite3_exec(s_db.db, v17_sql, NULL, NULL, &errmsg);
      if (rc != SQLITE_OK) {
         LOG_ERROR("auth_db: v17 migration failed: %s", errmsg ? errmsg : "unknown");
         sqlite3_free(errmsg);
         return AUTH_DB_FAILURE;
      }
      LOG_INFO("auth_db: added conversation origin column (v17)");
   }

   /* v18 migration: scheduler events table */
   if (current_version >= 1 && current_version < 18) {
      const char *v18_sql = "CREATE TABLE IF NOT EXISTS scheduled_events ("
                            "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
                            "  user_id INTEGER NOT NULL,"
                            "  event_type TEXT NOT NULL DEFAULT 'timer',"
                            "  status TEXT NOT NULL DEFAULT 'pending',"
                            "  name TEXT NOT NULL,"
                            "  message TEXT,"
                            "  fire_at INTEGER NOT NULL,"
                            "  created_at INTEGER NOT NULL,"
                            "  duration_sec INTEGER DEFAULT 0,"
                            "  snoozed_until INTEGER DEFAULT 0,"
                            "  recurrence TEXT DEFAULT 'once',"
                            "  recurrence_days TEXT,"
                            "  original_time TEXT,"
                            "  source_uuid TEXT,"
                            "  source_location TEXT,"
                            "  announce_all INTEGER DEFAULT 0,"
                            "  tool_name TEXT,"
                            "  tool_action TEXT,"
                            "  tool_value TEXT,"
                            "  fired_at INTEGER DEFAULT 0,"
                            "  snooze_count INTEGER DEFAULT 0,"
                            "  FOREIGN KEY (user_id) REFERENCES users(id)"
                            ");"
                            "CREATE INDEX IF NOT EXISTS idx_sched_status_fire "
                            "  ON scheduled_events(status, fire_at);"
                            "CREATE INDEX IF NOT EXISTS idx_sched_user "
                            "  ON scheduled_events(user_id, status);"
                            "CREATE INDEX IF NOT EXISTS idx_sched_user_name "
                            "  ON scheduled_events(user_id, status, name);"
                            "CREATE INDEX IF NOT EXISTS idx_sched_source "
                            "  ON scheduled_events(source_uuid);";

      rc = sqlite3_exec(s_db.db, v18_sql, NULL, NULL, &errmsg);
      if (rc != SQLITE_OK) {
         LOG_ERROR("auth_db: v18 migration failed: %s", errmsg ? errmsg : "unknown");
         sqlite3_free(errmsg);
         return AUTH_DB_FAILURE;
      }
      LOG_INFO("auth_db: added scheduled_events table (v18)");
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
                           "expires_at, ip_address, user_agent) VALUES (?, ?, ?, ?, ?, ?, ?)",
                           -1, &s_db.stmt_create_session, NULL);
   if (rc != SQLITE_OK) {
      LOG_ERROR("auth_db: prepare create_session failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db,
                           "SELECT s.token, s.user_id, u.username, u.is_admin, s.created_at, "
                           "s.last_activity, s.expires_at, s.ip_address, s.user_agent "
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

   rc = sqlite3_prepare_v2(s_db.db,
                           "DELETE FROM sessions WHERE expires_at IS NOT NULL AND expires_at < ?",
                           -1, &s_db.stmt_delete_expired_sessions, NULL);
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
       "context_tokens, context_max, continued_from, compaction_summary, "
       "llm_type, cloud_provider, model, tools_mode, thinking_mode, is_private, origin "
       "FROM conversations WHERE id = ?",
       -1, &s_db.stmt_conv_get, NULL);
   if (rc != SQLITE_OK) {
      LOG_ERROR("auth_db: prepare conv_get failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(
       s_db.db,
       "SELECT id, user_id, title, created_at, updated_at, message_count, is_archived, "
       "context_tokens, context_max, continued_from, compaction_summary, is_private, origin "
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
       "c.compaction_summary, c.is_private, c.origin, u.username "
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
       "context_tokens, context_max, continued_from, compaction_summary, is_private, origin "
       "FROM conversations WHERE user_id = ? AND title LIKE ? "
       "ORDER BY updated_at DESC LIMIT ? OFFSET ?",
       -1, &s_db.stmt_conv_search, NULL);
   if (rc != SQLITE_OK) {
      LOG_ERROR("auth_db: prepare conv_search failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db,
                           "SELECT DISTINCT c.id, c.user_id, c.title, c.created_at, c.updated_at, "
                           "c.message_count, c.is_archived, c.context_tokens, c.context_max, "
                           "c.continued_from, c.compaction_summary, c.is_private, c.origin "
                           "FROM conversations c "
                           "INNER JOIN messages m ON m.conversation_id = c.id "
                           "WHERE c.user_id = ? AND m.content LIKE ? "
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

   rc = sqlite3_prepare_v2(
       s_db.db,
       "INSERT INTO conversations (user_id, title, created_at, updated_at, origin) "
       "VALUES (?, ?, ?, ?, ?)",
       -1, &s_db.stmt_conv_create_origin, NULL);
   if (rc != SQLITE_OK) {
      LOG_ERROR("auth_db: prepare conv_create_origin failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db, "UPDATE conversations SET user_id = ? WHERE id = ?", -1,
                           &s_db.stmt_conv_reassign, NULL);
   if (rc != SQLITE_OK) {
      LOG_ERROR("auth_db: prepare conv_reassign failed: %s", sqlite3_errmsg(s_db.db));
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

   /* Image statements */
   rc = sqlite3_prepare_v2(s_db.db,
                           "INSERT INTO images (id, user_id, mime_type, size, data, created_at) "
                           "VALUES (?, ?, ?, ?, ?, ?)",
                           -1, &s_db.stmt_image_create, NULL);
   if (rc != SQLITE_OK) {
      LOG_ERROR("auth_db: prepare image_create failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db,
                           "SELECT id, user_id, mime_type, size, created_at, last_accessed "
                           "FROM images WHERE id = ?",
                           -1, &s_db.stmt_image_get, NULL);
   if (rc != SQLITE_OK) {
      LOG_ERROR("auth_db: prepare image_get failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db, "SELECT user_id, mime_type, data FROM images WHERE id = ?", -1,
                           &s_db.stmt_image_get_data, NULL);
   if (rc != SQLITE_OK) {
      LOG_ERROR("auth_db: prepare image_get_data failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db, "DELETE FROM images WHERE id = ? AND user_id = ?", -1,
                           &s_db.stmt_image_delete, NULL);
   if (rc != SQLITE_OK) {
      LOG_ERROR("auth_db: prepare image_delete failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db, "UPDATE images SET last_accessed = ? WHERE id = ?", -1,
                           &s_db.stmt_image_update_access, NULL);
   if (rc != SQLITE_OK) {
      LOG_ERROR("auth_db: prepare image_update_access failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db, "SELECT COUNT(*) FROM images WHERE user_id = ?", -1,
                           &s_db.stmt_image_count_user, NULL);
   if (rc != SQLITE_OK) {
      LOG_ERROR("auth_db: prepare image_count_user failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db, "DELETE FROM images WHERE created_at < ?", -1,
                           &s_db.stmt_image_delete_old, NULL);
   if (rc != SQLITE_OK) {
      LOG_ERROR("auth_db: prepare image_delete_old failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   /* Memory fact statements */
   rc = sqlite3_prepare_v2(s_db.db,
                           "INSERT INTO memory_facts (user_id, fact_text, confidence, source, "
                           "created_at, normalized_hash) "
                           "VALUES (?, ?, ?, ?, ?, ?)",
                           -1, &s_db.stmt_memory_fact_create, NULL);
   if (rc != SQLITE_OK) {
      LOG_ERROR("auth_db: prepare memory_fact_create failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(
       s_db.db,
       "SELECT id, user_id, fact_text, confidence, source, created_at, last_accessed, "
       "access_count, superseded_by FROM memory_facts WHERE id = ?",
       -1, &s_db.stmt_memory_fact_get, NULL);
   if (rc != SQLITE_OK) {
      LOG_ERROR("auth_db: prepare memory_fact_get failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(
       s_db.db,
       "SELECT id, user_id, fact_text, confidence, source, created_at, last_accessed, "
       "access_count, superseded_by FROM memory_facts "
       "WHERE user_id = ? AND superseded_by IS NULL "
       "ORDER BY confidence DESC LIMIT ? OFFSET ?",
       -1, &s_db.stmt_memory_fact_list, NULL);
   if (rc != SQLITE_OK) {
      LOG_ERROR("auth_db: prepare memory_fact_list failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(
       s_db.db,
       "SELECT id, user_id, fact_text, confidence, source, created_at, last_accessed, "
       "access_count, superseded_by FROM memory_facts "
       "WHERE user_id = ? AND superseded_by IS NULL AND fact_text LIKE ? ESCAPE '\\' "
       "ORDER BY confidence DESC LIMIT ?",
       -1, &s_db.stmt_memory_fact_search, NULL);
   if (rc != SQLITE_OK) {
      LOG_ERROR("auth_db: prepare memory_fact_search failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db,
                           "UPDATE memory_facts SET last_accessed = ?,"
                           "  access_count = access_count + 1,"
                           "  confidence = CASE"
                           "    WHEN (CAST(strftime('%s','now') AS REAL) - last_accessed) > 3600"
                           "    THEN MIN(1.0, confidence + ?)"
                           "    ELSE confidence"
                           "  END "
                           "WHERE id = ? AND user_id = ?",
                           -1, &s_db.stmt_memory_fact_update_access, NULL);
   if (rc != SQLITE_OK) {
      LOG_ERROR("auth_db: prepare memory_fact_update_access failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db, "UPDATE memory_facts SET confidence = ? WHERE id = ?", -1,
                           &s_db.stmt_memory_fact_update_confidence, NULL);
   if (rc != SQLITE_OK) {
      LOG_ERROR("auth_db: prepare memory_fact_update_confidence failed: %s",
                sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db, "UPDATE memory_facts SET superseded_by = ? WHERE id = ?", -1,
                           &s_db.stmt_memory_fact_supersede, NULL);
   if (rc != SQLITE_OK) {
      LOG_ERROR("auth_db: prepare memory_fact_supersede failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db, "DELETE FROM memory_facts WHERE id = ? AND user_id = ?", -1,
                           &s_db.stmt_memory_fact_delete, NULL);
   if (rc != SQLITE_OK) {
      LOG_ERROR("auth_db: prepare memory_fact_delete failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db,
                           "SELECT id, fact_text, confidence FROM memory_facts "
                           "WHERE user_id = ? AND superseded_by IS NULL "
                           "AND fact_text LIKE ? ESCAPE '\\' "
                           "ORDER BY confidence DESC LIMIT 5",
                           -1, &s_db.stmt_memory_fact_find_similar, NULL);
   if (rc != SQLITE_OK) {
      LOG_ERROR("auth_db: prepare memory_fact_find_similar failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db,
                           "SELECT id, fact_text, confidence FROM memory_facts "
                           "WHERE user_id = ? AND normalized_hash = ? AND superseded_by IS NULL",
                           -1, &s_db.stmt_memory_fact_find_by_hash, NULL);
   if (rc != SQLITE_OK) {
      LOG_ERROR("auth_db: prepare memory_fact_find_by_hash failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(
       s_db.db,
       "DELETE FROM memory_facts WHERE user_id = ? AND superseded_by IS NOT NULL "
       "AND created_at < ?",
       -1, &s_db.stmt_memory_fact_prune_superseded, NULL);
   if (rc != SQLITE_OK) {
      LOG_ERROR("auth_db: prepare memory_fact_prune_superseded failed: %s",
                sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db,
                           "DELETE FROM memory_facts WHERE user_id = ? AND superseded_by IS NULL "
                           "AND last_accessed < ? AND confidence < ?",
                           -1, &s_db.stmt_memory_fact_prune_stale, NULL);
   if (rc != SQLITE_OK) {
      LOG_ERROR("auth_db: prepare memory_fact_prune_stale failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   /* Memory preference statements */
   rc = sqlite3_prepare_v2(
       s_db.db,
       "INSERT INTO memory_preferences (user_id, category, value, confidence, source, created_at, "
       "updated_at) VALUES (?, ?, ?, ?, ?, ?, ?) "
       "ON CONFLICT(user_id, category) DO UPDATE SET "
       "value=excluded.value, confidence=excluded.confidence, updated_at=excluded.updated_at, "
       "reinforcement_count=reinforcement_count+1",
       -1, &s_db.stmt_memory_pref_upsert, NULL);
   if (rc != SQLITE_OK) {
      LOG_ERROR("auth_db: prepare memory_pref_upsert failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(
       s_db.db,
       "SELECT id, user_id, category, value, confidence, source, created_at, updated_at, "
       "reinforcement_count FROM memory_preferences WHERE user_id = ? AND category = ?",
       -1, &s_db.stmt_memory_pref_get, NULL);
   if (rc != SQLITE_OK) {
      LOG_ERROR("auth_db: prepare memory_pref_get failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(
       s_db.db,
       "SELECT id, user_id, category, value, confidence, source, created_at, updated_at, "
       "reinforcement_count FROM memory_preferences WHERE user_id = ? ORDER BY category",
       -1, &s_db.stmt_memory_pref_list, NULL);
   if (rc != SQLITE_OK) {
      LOG_ERROR("auth_db: prepare memory_pref_list failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(
       s_db.db,
       "SELECT id, user_id, category, value, confidence, source, created_at, updated_at, "
       "reinforcement_count FROM memory_preferences "
       "WHERE user_id = ? AND (category LIKE ? ESCAPE '\\' OR value LIKE ? ESCAPE '\\') "
       "ORDER BY confidence DESC LIMIT ?",
       -1, &s_db.stmt_memory_pref_search, NULL);
   if (rc != SQLITE_OK) {
      LOG_ERROR("auth_db: prepare memory_pref_search failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db,
                           "DELETE FROM memory_preferences WHERE user_id = ? AND category = ?", -1,
                           &s_db.stmt_memory_pref_delete, NULL);
   if (rc != SQLITE_OK) {
      LOG_ERROR("auth_db: prepare memory_pref_delete failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   /* Memory summary statements */
   rc = sqlite3_prepare_v2(
       s_db.db,
       "INSERT INTO memory_summaries (user_id, session_id, summary, topics, sentiment, "
       "created_at, message_count, duration_seconds) VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
       -1, &s_db.stmt_memory_summary_create, NULL);
   if (rc != SQLITE_OK) {
      LOG_ERROR("auth_db: prepare memory_summary_create failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(
       s_db.db,
       "SELECT id, user_id, session_id, summary, topics, sentiment, created_at, "
       "message_count, duration_seconds, consolidated FROM memory_summaries "
       "WHERE user_id = ? AND consolidated = 0 ORDER BY created_at DESC LIMIT ?",
       -1, &s_db.stmt_memory_summary_list, NULL);
   if (rc != SQLITE_OK) {
      LOG_ERROR("auth_db: prepare memory_summary_list failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db, "UPDATE memory_summaries SET consolidated = 1 WHERE id = ?", -1,
                           &s_db.stmt_memory_summary_mark_consolidated, NULL);
   if (rc != SQLITE_OK) {
      LOG_ERROR("auth_db: prepare memory_summary_mark_consolidated failed: %s",
                sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(
       s_db.db,
       "SELECT id, user_id, session_id, summary, topics, sentiment, created_at, "
       "message_count, duration_seconds, consolidated FROM memory_summaries "
       "WHERE user_id = ? AND (summary LIKE ? ESCAPE '\\' OR topics LIKE ? ESCAPE '\\') "
       "ORDER BY created_at DESC LIMIT ?",
       -1, &s_db.stmt_memory_summary_search, NULL);
   if (rc != SQLITE_OK) {
      LOG_ERROR("auth_db: prepare memory_summary_search failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   /* Date-filtered memory queries (for time_range search and fixed recent) */
   rc = sqlite3_prepare_v2(
       s_db.db,
       "SELECT id, user_id, fact_text, confidence, source, created_at, last_accessed, "
       "access_count, superseded_by FROM memory_facts "
       "WHERE user_id = ? AND superseded_by IS NULL AND fact_text LIKE ? ESCAPE '\\' "
       "AND created_at >= ? ORDER BY confidence DESC LIMIT ?",
       -1, &s_db.stmt_memory_fact_search_since, NULL);
   if (rc != SQLITE_OK) {
      LOG_ERROR("auth_db: prepare memory_fact_search_since failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(
       s_db.db,
       "SELECT id, user_id, session_id, summary, topics, sentiment, created_at, "
       "message_count, duration_seconds, consolidated FROM memory_summaries "
       "WHERE user_id = ? AND (summary LIKE ? ESCAPE '\\' OR topics LIKE ? ESCAPE '\\') "
       "AND created_at >= ? ORDER BY created_at DESC LIMIT ?",
       -1, &s_db.stmt_memory_summary_search_since, NULL);
   if (rc != SQLITE_OK) {
      LOG_ERROR("auth_db: prepare memory_summary_search_since failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(
       s_db.db,
       "SELECT id, user_id, fact_text, confidence, source, created_at, last_accessed, "
       "access_count, superseded_by FROM memory_facts "
       "WHERE user_id = ? AND superseded_by IS NULL AND created_at >= ? "
       "ORDER BY created_at DESC LIMIT ?",
       -1, &s_db.stmt_memory_fact_list_since, NULL);
   if (rc != SQLITE_OK) {
      LOG_ERROR("auth_db: prepare memory_fact_list_since failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(
       s_db.db,
       "SELECT id, user_id, session_id, summary, topics, sentiment, created_at, "
       "message_count, duration_seconds, consolidated FROM memory_summaries "
       "WHERE user_id = ? AND created_at >= ? "
       "ORDER BY created_at DESC LIMIT ?",
       -1, &s_db.stmt_memory_summary_list_since, NULL);
   if (rc != SQLITE_OK) {
      LOG_ERROR("auth_db: prepare memory_summary_list_since failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   /* Conversation extraction tracking statements */
   rc = sqlite3_prepare_v2(s_db.db,
                           "SELECT last_extracted_msg_count FROM conversations WHERE id = ?", -1,
                           &s_db.stmt_conv_get_last_extracted, NULL);
   if (rc != SQLITE_OK) {
      LOG_ERROR("auth_db: prepare conv_get_last_extracted failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db,
                           "UPDATE conversations SET last_extracted_msg_count = ? WHERE id = ?", -1,
                           &s_db.stmt_conv_set_last_extracted, NULL);
   if (rc != SQLITE_OK) {
      LOG_ERROR("auth_db: prepare conv_set_last_extracted failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   /* Conversation privacy statement */
   rc = sqlite3_prepare_v2(s_db.db,
                           "UPDATE conversations SET is_private = ? "
                           "WHERE id = ? AND user_id = ?",
                           -1, &s_db.stmt_conv_set_private, NULL);
   if (rc != SQLITE_OK) {
      LOG_ERROR("auth_db: prepare conv_set_private failed: %s", sqlite3_errmsg(s_db.db));
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
   if (s_db.stmt_conv_create_origin)
      sqlite3_finalize(s_db.stmt_conv_create_origin);
   if (s_db.stmt_conv_reassign)
      sqlite3_finalize(s_db.stmt_conv_reassign);

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

   /* Image statements */
   if (s_db.stmt_image_create)
      sqlite3_finalize(s_db.stmt_image_create);
   if (s_db.stmt_image_get)
      sqlite3_finalize(s_db.stmt_image_get);
   if (s_db.stmt_image_get_data)
      sqlite3_finalize(s_db.stmt_image_get_data);
   if (s_db.stmt_image_delete)
      sqlite3_finalize(s_db.stmt_image_delete);
   if (s_db.stmt_image_update_access)
      sqlite3_finalize(s_db.stmt_image_update_access);
   if (s_db.stmt_image_count_user)
      sqlite3_finalize(s_db.stmt_image_count_user);
   if (s_db.stmt_image_delete_old)
      sqlite3_finalize(s_db.stmt_image_delete_old);

   /* Memory statements */
   if (s_db.stmt_memory_fact_create)
      sqlite3_finalize(s_db.stmt_memory_fact_create);
   if (s_db.stmt_memory_fact_get)
      sqlite3_finalize(s_db.stmt_memory_fact_get);
   if (s_db.stmt_memory_fact_list)
      sqlite3_finalize(s_db.stmt_memory_fact_list);
   if (s_db.stmt_memory_fact_search)
      sqlite3_finalize(s_db.stmt_memory_fact_search);
   if (s_db.stmt_memory_fact_update_access)
      sqlite3_finalize(s_db.stmt_memory_fact_update_access);
   if (s_db.stmt_memory_fact_update_confidence)
      sqlite3_finalize(s_db.stmt_memory_fact_update_confidence);
   if (s_db.stmt_memory_fact_supersede)
      sqlite3_finalize(s_db.stmt_memory_fact_supersede);
   if (s_db.stmt_memory_fact_delete)
      sqlite3_finalize(s_db.stmt_memory_fact_delete);
   if (s_db.stmt_memory_fact_find_similar)
      sqlite3_finalize(s_db.stmt_memory_fact_find_similar);
   if (s_db.stmt_memory_pref_upsert)
      sqlite3_finalize(s_db.stmt_memory_pref_upsert);
   if (s_db.stmt_memory_pref_get)
      sqlite3_finalize(s_db.stmt_memory_pref_get);
   if (s_db.stmt_memory_pref_list)
      sqlite3_finalize(s_db.stmt_memory_pref_list);
   if (s_db.stmt_memory_pref_search)
      sqlite3_finalize(s_db.stmt_memory_pref_search);
   if (s_db.stmt_memory_pref_delete)
      sqlite3_finalize(s_db.stmt_memory_pref_delete);
   if (s_db.stmt_memory_summary_create)
      sqlite3_finalize(s_db.stmt_memory_summary_create);
   if (s_db.stmt_memory_summary_list)
      sqlite3_finalize(s_db.stmt_memory_summary_list);
   if (s_db.stmt_memory_summary_mark_consolidated)
      sqlite3_finalize(s_db.stmt_memory_summary_mark_consolidated);
   if (s_db.stmt_memory_summary_search)
      sqlite3_finalize(s_db.stmt_memory_summary_search);

   /* Date-filtered memory statements */
   if (s_db.stmt_memory_fact_search_since)
      sqlite3_finalize(s_db.stmt_memory_fact_search_since);
   if (s_db.stmt_memory_summary_search_since)
      sqlite3_finalize(s_db.stmt_memory_summary_search_since);
   if (s_db.stmt_memory_fact_list_since)
      sqlite3_finalize(s_db.stmt_memory_fact_list_since);
   if (s_db.stmt_memory_summary_list_since)
      sqlite3_finalize(s_db.stmt_memory_summary_list_since);

   /* Deduplication and pruning statements */
   if (s_db.stmt_memory_fact_find_by_hash)
      sqlite3_finalize(s_db.stmt_memory_fact_find_by_hash);
   if (s_db.stmt_memory_fact_prune_superseded)
      sqlite3_finalize(s_db.stmt_memory_fact_prune_superseded);
   if (s_db.stmt_memory_fact_prune_stale)
      sqlite3_finalize(s_db.stmt_memory_fact_prune_stale);

   /* Extraction tracking statements */
   if (s_db.stmt_conv_get_last_extracted)
      sqlite3_finalize(s_db.stmt_conv_get_last_extracted);
   if (s_db.stmt_conv_set_last_extracted)
      sqlite3_finalize(s_db.stmt_conv_set_last_extracted);

   /* Privacy statement */
   if (s_db.stmt_conv_set_private)
      sqlite3_finalize(s_db.stmt_conv_set_private);

   /* Clear all statement pointers using offsetof for safety
    * MAINTENANCE: If statements are reordered, update first/last_stmt names */
   size_t first_stmt_offset = offsetof(auth_db_state_t, stmt_create_user);
   size_t last_stmt_end = offsetof(auth_db_state_t, stmt_conv_set_private) + sizeof(sqlite3_stmt *);
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
 * Custom SQLite Functions
 * ============================================================================= */

/**
 * @brief SQLite custom function: powf(base, exp)
 *
 * Enables atomic confidence decay in UPDATE statements without
 * a SELECT-compute-UPDATE loop. Used by memory decay system.
 */
static void sqlite_powf(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
   (void)argc;
   double base = sqlite3_value_double(argv[0]);
   double exp = sqlite3_value_double(argv[1]);
   double result = pow(base, exp);
   /* Guard against NaN/Inf from edge cases (negative base, huge exponent) */
   if (isnan(result) || isinf(result)) {
      sqlite3_result_double(ctx, 0.0);
   } else {
      sqlite3_result_double(ctx, result);
   }
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

   /* Register custom SQL functions */
   sqlite3_create_function(s_db.db, "powf", 2, SQLITE_UTF8, NULL, sqlite_powf, NULL, NULL);

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
