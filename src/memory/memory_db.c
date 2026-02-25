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
 * Memory Database Implementation
 *
 * CRUD operations for facts, preferences, and summaries.
 */

#define AUTH_DB_INTERNAL_ALLOWED
#include "memory/memory_db.h"

#include <string.h>
#include <time.h>

#include "auth/auth_db_internal.h"
#include "config/dawn_config.h"
#include "logging.h"
#include "memory/memory_similarity.h"

/* =============================================================================
 * Helper: Build LIKE pattern with wildcards
 * ============================================================================= */

static void build_like_pattern(const char *keywords, char *out_pattern, size_t max_len) {
   if (!keywords || !out_pattern || max_len < 4) {
      if (out_pattern && max_len > 0) {
         out_pattern[0] = '\0';
      }
      return;
   }

   /* Build pattern with escaped LIKE metacharacters (%, _, \) */
   size_t out_idx = 0;
   out_pattern[out_idx++] = '%';

   for (size_t i = 0; keywords[i] != '\0' && out_idx < max_len - 3; i++) {
      char c = keywords[i];
      /* Escape LIKE metacharacters with backslash */
      if (c == '%' || c == '_' || c == '\\') {
         if (out_idx < max_len - 4) {
            out_pattern[out_idx++] = '\\';
            out_pattern[out_idx++] = c;
         }
      } else {
         out_pattern[out_idx++] = c;
      }
   }

   out_pattern[out_idx++] = '%';
   out_pattern[out_idx] = '\0';
}

/* =============================================================================
 * Helper: Populate fact from statement row
 * ============================================================================= */

static void populate_fact_from_row(sqlite3_stmt *stmt, memory_fact_t *fact) {
   fact->id = sqlite3_column_int64(stmt, 0);
   fact->user_id = sqlite3_column_int(stmt, 1);

   const char *text = (const char *)sqlite3_column_text(stmt, 2);
   if (text) {
      strncpy(fact->fact_text, text, MEMORY_FACT_TEXT_MAX - 1);
      fact->fact_text[MEMORY_FACT_TEXT_MAX - 1] = '\0';
   }

   fact->confidence = (float)sqlite3_column_double(stmt, 3);

   const char *source = (const char *)sqlite3_column_text(stmt, 4);
   if (source) {
      strncpy(fact->source, source, MEMORY_SOURCE_MAX - 1);
      fact->source[MEMORY_SOURCE_MAX - 1] = '\0';
   }

   fact->created_at = (time_t)sqlite3_column_int64(stmt, 5);
   fact->last_accessed = (time_t)sqlite3_column_int64(stmt, 6);
   fact->access_count = sqlite3_column_int(stmt, 7);
   fact->superseded_by = sqlite3_column_int64(stmt, 8);
}

/* =============================================================================
 * Helper: Populate preference from statement row
 * ============================================================================= */

static void populate_pref_from_row(sqlite3_stmt *stmt, memory_preference_t *pref) {
   pref->id = sqlite3_column_int64(stmt, 0);
   pref->user_id = sqlite3_column_int(stmt, 1);

   const char *cat = (const char *)sqlite3_column_text(stmt, 2);
   if (cat) {
      strncpy(pref->category, cat, MEMORY_CATEGORY_MAX - 1);
      pref->category[MEMORY_CATEGORY_MAX - 1] = '\0';
   }

   const char *val = (const char *)sqlite3_column_text(stmt, 3);
   if (val) {
      strncpy(pref->value, val, MEMORY_PREF_VALUE_MAX - 1);
      pref->value[MEMORY_PREF_VALUE_MAX - 1] = '\0';
   }

   pref->confidence = (float)sqlite3_column_double(stmt, 4);

   const char *source = (const char *)sqlite3_column_text(stmt, 5);
   if (source) {
      strncpy(pref->source, source, MEMORY_SOURCE_MAX - 1);
      pref->source[MEMORY_SOURCE_MAX - 1] = '\0';
   }

   pref->created_at = (time_t)sqlite3_column_int64(stmt, 6);
   pref->updated_at = (time_t)sqlite3_column_int64(stmt, 7);
   pref->reinforcement_count = sqlite3_column_int(stmt, 8);
}

/* =============================================================================
 * Helper: Populate summary from statement row
 * ============================================================================= */

static void populate_summary_from_row(sqlite3_stmt *stmt, memory_summary_t *summary) {
   summary->id = sqlite3_column_int64(stmt, 0);
   summary->user_id = sqlite3_column_int(stmt, 1);

   const char *sid = (const char *)sqlite3_column_text(stmt, 2);
   if (sid) {
      strncpy(summary->session_id, sid, MEMORY_SESSION_ID_MAX - 1);
      summary->session_id[MEMORY_SESSION_ID_MAX - 1] = '\0';
   }

   const char *sum = (const char *)sqlite3_column_text(stmt, 3);
   if (sum) {
      strncpy(summary->summary, sum, MEMORY_SUMMARY_MAX - 1);
      summary->summary[MEMORY_SUMMARY_MAX - 1] = '\0';
   }

   const char *topics = (const char *)sqlite3_column_text(stmt, 4);
   if (topics) {
      strncpy(summary->topics, topics, MEMORY_TOPICS_MAX - 1);
      summary->topics[MEMORY_TOPICS_MAX - 1] = '\0';
   }

   const char *sentiment = (const char *)sqlite3_column_text(stmt, 5);
   if (sentiment) {
      strncpy(summary->sentiment, sentiment, MEMORY_SENTIMENT_MAX - 1);
      summary->sentiment[MEMORY_SENTIMENT_MAX - 1] = '\0';
   }

   summary->created_at = (time_t)sqlite3_column_int64(stmt, 6);
   summary->message_count = sqlite3_column_int(stmt, 7);
   summary->duration_seconds = sqlite3_column_int(stmt, 8);
   summary->consolidated = sqlite3_column_int(stmt, 9) != 0;
}

/* =============================================================================
 * Fact Operations
 * ============================================================================= */

int64_t memory_db_fact_create(int user_id,
                              const char *fact_text,
                              float confidence,
                              const char *source) {
   if (!fact_text || !source) {
      return -1;
   }

   /* Compute normalized hash for deduplication */
   uint32_t normalized_hash = memory_normalize_and_hash(fact_text);

   AUTH_DB_LOCK_OR_RETURN(-1);

   sqlite3_stmt *stmt = s_db.stmt_memory_fact_create;
   sqlite3_reset(stmt);
   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_text(stmt, 2, fact_text, -1, SQLITE_STATIC);
   sqlite3_bind_double(stmt, 3, confidence);
   sqlite3_bind_text(stmt, 4, source, -1, SQLITE_STATIC);
   sqlite3_bind_int64(stmt, 5, (int64_t)time(NULL));
   sqlite3_bind_int64(stmt, 6, (int64_t)normalized_hash);

   int rc = sqlite3_step(stmt);
   sqlite3_reset(stmt);

   if (rc != SQLITE_DONE) {
      LOG_ERROR("memory_db: fact_create failed: %s", sqlite3_errmsg(s_db.db));
      AUTH_DB_UNLOCK();
      return -1;
   }

   int64_t id = sqlite3_last_insert_rowid(s_db.db);
   AUTH_DB_UNLOCK();

   LOG_INFO("memory_db: created fact %ld for user %d (hash=%u)", (long)id, user_id,
            normalized_hash);
   return id;
}

int memory_db_fact_get(int64_t fact_id, memory_fact_t *out_fact) {
   if (!out_fact) {
      return MEMORY_DB_FAILURE;
   }

   AUTH_DB_LOCK_OR_RETURN(MEMORY_DB_FAILURE);

   sqlite3_stmt *stmt = s_db.stmt_memory_fact_get;
   sqlite3_reset(stmt);
   sqlite3_bind_int64(stmt, 1, fact_id);

   int result = MEMORY_DB_NOT_FOUND;
   if (sqlite3_step(stmt) == SQLITE_ROW) {
      populate_fact_from_row(stmt, out_fact);
      result = MEMORY_DB_SUCCESS;
   }

   sqlite3_reset(stmt);
   AUTH_DB_UNLOCK();
   return result;
}

int memory_db_fact_list(int user_id, memory_fact_t *out_facts, int max_facts, int offset) {
   if (!out_facts || max_facts <= 0) {
      return -1;
   }

   AUTH_DB_LOCK_OR_RETURN(-1);

   sqlite3_stmt *stmt = s_db.stmt_memory_fact_list;
   sqlite3_reset(stmt);
   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_int(stmt, 2, max_facts);
   sqlite3_bind_int(stmt, 3, offset);

   int count = 0;
   while (count < max_facts && sqlite3_step(stmt) == SQLITE_ROW) {
      populate_fact_from_row(stmt, &out_facts[count]);
      count++;
   }

   sqlite3_reset(stmt);
   AUTH_DB_UNLOCK();
   return count;
}

int memory_db_fact_search(int user_id,
                          const char *keywords,
                          memory_fact_t *out_facts,
                          int max_facts) {
   if (!keywords || !out_facts || max_facts <= 0) {
      return -1;
   }

   char pattern[MEMORY_FACT_TEXT_MAX];
   build_like_pattern(keywords, pattern, sizeof(pattern));

   AUTH_DB_LOCK_OR_RETURN(-1);

   sqlite3_stmt *stmt = s_db.stmt_memory_fact_search;
   sqlite3_reset(stmt);
   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_text(stmt, 2, pattern, -1, SQLITE_STATIC);
   sqlite3_bind_int(stmt, 3, max_facts);

   int count = 0;
   while (count < max_facts && sqlite3_step(stmt) == SQLITE_ROW) {
      populate_fact_from_row(stmt, &out_facts[count]);
      count++;
   }

   sqlite3_reset(stmt);
   AUTH_DB_UNLOCK();
   return count;
}

int memory_db_fact_update_access(int64_t fact_id, int user_id) {
   AUTH_DB_LOCK_OR_RETURN(MEMORY_DB_FAILURE);

   sqlite3_stmt *stmt = s_db.stmt_memory_fact_update_access;
   sqlite3_reset(stmt);
   sqlite3_bind_int64(stmt, 1, (int64_t)time(NULL));
   sqlite3_bind_double(stmt, 2, (double)g_config.memory.access_reinforcement_boost);
   sqlite3_bind_int64(stmt, 3, fact_id);
   sqlite3_bind_int(stmt, 4, user_id);

   int rc = sqlite3_step(stmt);
   sqlite3_reset(stmt);

   AUTH_DB_UNLOCK();
   return (rc == SQLITE_DONE) ? MEMORY_DB_SUCCESS : MEMORY_DB_FAILURE;
}

int memory_db_fact_update_confidence(int64_t fact_id, float confidence) {
   AUTH_DB_LOCK_OR_RETURN(MEMORY_DB_FAILURE);

   sqlite3_stmt *stmt = s_db.stmt_memory_fact_update_confidence;
   sqlite3_reset(stmt);
   sqlite3_bind_double(stmt, 1, confidence);
   sqlite3_bind_int64(stmt, 2, fact_id);

   int rc = sqlite3_step(stmt);
   sqlite3_reset(stmt);

   AUTH_DB_UNLOCK();
   return (rc == SQLITE_DONE) ? MEMORY_DB_SUCCESS : MEMORY_DB_FAILURE;
}

int memory_db_fact_supersede(int64_t old_fact_id, int64_t new_fact_id) {
   AUTH_DB_LOCK_OR_RETURN(MEMORY_DB_FAILURE);

   sqlite3_stmt *stmt = s_db.stmt_memory_fact_supersede;
   sqlite3_reset(stmt);
   sqlite3_bind_int64(stmt, 1, new_fact_id);
   sqlite3_bind_int64(stmt, 2, old_fact_id);

   int rc = sqlite3_step(stmt);
   sqlite3_reset(stmt);

   AUTH_DB_UNLOCK();
   return (rc == SQLITE_DONE) ? MEMORY_DB_SUCCESS : MEMORY_DB_FAILURE;
}

int memory_db_fact_delete(int64_t fact_id, int user_id) {
   AUTH_DB_LOCK_OR_RETURN(MEMORY_DB_FAILURE);

   sqlite3_stmt *stmt = s_db.stmt_memory_fact_delete;
   sqlite3_reset(stmt);
   sqlite3_bind_int64(stmt, 1, fact_id);
   sqlite3_bind_int(stmt, 2, user_id);

   int rc = sqlite3_step(stmt);
   int changes = sqlite3_changes(s_db.db);
   sqlite3_reset(stmt);

   AUTH_DB_UNLOCK();

   if (rc != SQLITE_DONE) {
      return MEMORY_DB_FAILURE;
   }
   return (changes > 0) ? MEMORY_DB_SUCCESS : MEMORY_DB_NOT_FOUND;
}

int memory_db_fact_find_similar(int user_id,
                                const char *fact_text,
                                memory_fact_t *out_facts,
                                int max_facts) {
   if (!fact_text || !out_facts || max_facts <= 0) {
      return -1;
   }

   /* Extract key words from fact for similarity search */
   char pattern[MEMORY_FACT_TEXT_MAX];
   build_like_pattern(fact_text, pattern, sizeof(pattern));

   AUTH_DB_LOCK_OR_RETURN(-1);

   sqlite3_stmt *stmt = s_db.stmt_memory_fact_find_similar;
   sqlite3_reset(stmt);
   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_text(stmt, 2, pattern, -1, SQLITE_STATIC);

   int count = 0;
   while (count < max_facts && sqlite3_step(stmt) == SQLITE_ROW) {
      out_facts[count].id = sqlite3_column_int64(stmt, 0);
      const char *text = (const char *)sqlite3_column_text(stmt, 1);
      if (text) {
         strncpy(out_facts[count].fact_text, text, MEMORY_FACT_TEXT_MAX - 1);
         out_facts[count].fact_text[MEMORY_FACT_TEXT_MAX - 1] = '\0';
      }
      out_facts[count].confidence = (float)sqlite3_column_double(stmt, 2);
      count++;
   }

   sqlite3_reset(stmt);
   AUTH_DB_UNLOCK();
   return count;
}

int memory_db_fact_find_by_hash(int user_id,
                                uint32_t hash,
                                memory_fact_t *out_facts,
                                int max_facts) {
   if (!out_facts || max_facts <= 0 || hash == 0) {
      return -1;
   }

   AUTH_DB_LOCK_OR_RETURN(-1);

   sqlite3_stmt *stmt = s_db.stmt_memory_fact_find_by_hash;
   sqlite3_reset(stmt);
   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_int64(stmt, 2, (int64_t)hash);

   int count = 0;
   while (count < max_facts && sqlite3_step(stmt) == SQLITE_ROW) {
      out_facts[count].id = sqlite3_column_int64(stmt, 0);
      const char *text = (const char *)sqlite3_column_text(stmt, 1);
      if (text) {
         strncpy(out_facts[count].fact_text, text, MEMORY_FACT_TEXT_MAX - 1);
         out_facts[count].fact_text[MEMORY_FACT_TEXT_MAX - 1] = '\0';
      }
      out_facts[count].confidence = (float)sqlite3_column_double(stmt, 2);
      count++;
   }

   sqlite3_reset(stmt);
   AUTH_DB_UNLOCK();
   return count;
}

int memory_db_fact_prune_superseded(int user_id, int retention_days) {
   AUTH_DB_LOCK_OR_RETURN(-1);

   time_t cutoff = time(NULL) - (retention_days * 24 * 60 * 60);

   sqlite3_stmt *stmt = s_db.stmt_memory_fact_prune_superseded;
   sqlite3_reset(stmt);
   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_int64(stmt, 2, (int64_t)cutoff);

   int rc = sqlite3_step(stmt);
   int deleted = sqlite3_changes(s_db.db);
   sqlite3_reset(stmt);

   AUTH_DB_UNLOCK();

   if (rc != SQLITE_DONE) {
      LOG_ERROR("memory_db: prune_superseded failed: %s", sqlite3_errmsg(s_db.db));
      return -1;
   }

   if (deleted > 0) {
      LOG_INFO("memory_db: pruned %d superseded facts for user %d", deleted, user_id);
   }
   return deleted;
}

int memory_db_fact_prune_stale(int user_id, int stale_days, float min_confidence) {
   AUTH_DB_LOCK_OR_RETURN(-1);

   time_t cutoff = time(NULL) - (stale_days * 24 * 60 * 60);

   sqlite3_stmt *stmt = s_db.stmt_memory_fact_prune_stale;
   sqlite3_reset(stmt);
   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_int64(stmt, 2, (int64_t)cutoff);
   sqlite3_bind_double(stmt, 3, min_confidence);

   int rc = sqlite3_step(stmt);
   int deleted = sqlite3_changes(s_db.db);
   sqlite3_reset(stmt);

   AUTH_DB_UNLOCK();

   if (rc != SQLITE_DONE) {
      LOG_ERROR("memory_db: prune_stale failed: %s", sqlite3_errmsg(s_db.db));
      return -1;
   }

   if (deleted > 0) {
      LOG_INFO("memory_db: pruned %d stale facts for user %d", deleted, user_id);
   }
   return deleted;
}

/* =============================================================================
 * Date-Filtered Queries
 * ============================================================================= */

int memory_db_fact_search_since(int user_id,
                                const char *keywords,
                                time_t since_ts,
                                memory_fact_t *out_facts,
                                int max_facts) {
   if (!keywords || !out_facts || max_facts <= 0) {
      return -1;
   }

   char pattern[MEMORY_FACT_TEXT_MAX];
   build_like_pattern(keywords, pattern, sizeof(pattern));

   AUTH_DB_LOCK_OR_RETURN(-1);

   sqlite3_stmt *stmt = s_db.stmt_memory_fact_search_since;
   sqlite3_reset(stmt);
   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_text(stmt, 2, pattern, -1, SQLITE_STATIC);
   sqlite3_bind_int64(stmt, 3, (int64_t)since_ts);
   sqlite3_bind_int(stmt, 4, max_facts);

   int count = 0;
   while (count < max_facts && sqlite3_step(stmt) == SQLITE_ROW) {
      populate_fact_from_row(stmt, &out_facts[count]);
      count++;
   }

   sqlite3_reset(stmt);
   AUTH_DB_UNLOCK();
   return count;
}

int memory_db_summary_search_since(int user_id,
                                   const char *keywords,
                                   time_t since_ts,
                                   memory_summary_t *out_summaries,
                                   int max_summaries) {
   if (!keywords || !out_summaries || max_summaries <= 0) {
      return -1;
   }

   char pattern[MEMORY_SUMMARY_MAX];
   build_like_pattern(keywords, pattern, sizeof(pattern));

   AUTH_DB_LOCK_OR_RETURN(-1);

   sqlite3_stmt *stmt = s_db.stmt_memory_summary_search_since;
   sqlite3_reset(stmt);
   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_text(stmt, 2, pattern, -1, SQLITE_STATIC);
   sqlite3_bind_text(stmt, 3, pattern, -1, SQLITE_STATIC);
   sqlite3_bind_int64(stmt, 4, (int64_t)since_ts);
   sqlite3_bind_int(stmt, 5, max_summaries);

   int count = 0;
   while (count < max_summaries && sqlite3_step(stmt) == SQLITE_ROW) {
      populate_summary_from_row(stmt, &out_summaries[count]);
      count++;
   }

   sqlite3_reset(stmt);
   AUTH_DB_UNLOCK();
   return count;
}

int memory_db_fact_list_since(int user_id,
                              time_t since_ts,
                              memory_fact_t *out_facts,
                              int max_facts) {
   if (!out_facts || max_facts <= 0) {
      return -1;
   }

   AUTH_DB_LOCK_OR_RETURN(-1);

   sqlite3_stmt *stmt = s_db.stmt_memory_fact_list_since;
   sqlite3_reset(stmt);
   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_int64(stmt, 2, (int64_t)since_ts);
   sqlite3_bind_int(stmt, 3, max_facts);

   int count = 0;
   while (count < max_facts && sqlite3_step(stmt) == SQLITE_ROW) {
      populate_fact_from_row(stmt, &out_facts[count]);
      count++;
   }

   sqlite3_reset(stmt);
   AUTH_DB_UNLOCK();
   return count;
}

int memory_db_summary_list_since(int user_id,
                                 time_t since_ts,
                                 memory_summary_t *out_summaries,
                                 int max_summaries) {
   if (!out_summaries || max_summaries <= 0) {
      return -1;
   }

   AUTH_DB_LOCK_OR_RETURN(-1);

   sqlite3_stmt *stmt = s_db.stmt_memory_summary_list_since;
   sqlite3_reset(stmt);
   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_int64(stmt, 2, (int64_t)since_ts);
   sqlite3_bind_int(stmt, 3, max_summaries);

   int count = 0;
   while (count < max_summaries && sqlite3_step(stmt) == SQLITE_ROW) {
      populate_summary_from_row(stmt, &out_summaries[count]);
      count++;
   }

   sqlite3_reset(stmt);
   AUTH_DB_UNLOCK();
   return count;
}

/* =============================================================================
 * Preference Operations
 * ============================================================================= */

int memory_db_pref_upsert(int user_id,
                          const char *category,
                          const char *value,
                          float confidence,
                          const char *source) {
   if (!category || !value || !source) {
      return MEMORY_DB_FAILURE;
   }

   AUTH_DB_LOCK_OR_RETURN(MEMORY_DB_FAILURE);

   time_t now = time(NULL);
   sqlite3_stmt *stmt = s_db.stmt_memory_pref_upsert;
   sqlite3_reset(stmt);
   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_text(stmt, 2, category, -1, SQLITE_STATIC);
   sqlite3_bind_text(stmt, 3, value, -1, SQLITE_STATIC);
   sqlite3_bind_double(stmt, 4, confidence);
   sqlite3_bind_text(stmt, 5, source, -1, SQLITE_STATIC);
   sqlite3_bind_int64(stmt, 6, (int64_t)now);
   sqlite3_bind_int64(stmt, 7, (int64_t)now);

   int rc = sqlite3_step(stmt);
   sqlite3_reset(stmt);

   AUTH_DB_UNLOCK();

   if (rc != SQLITE_DONE) {
      LOG_ERROR("memory_db: pref_upsert failed: %s", sqlite3_errmsg(s_db.db));
      return MEMORY_DB_FAILURE;
   }

   LOG_INFO("memory_db: upserted preference %s=%s for user %d", category, value, user_id);
   return MEMORY_DB_SUCCESS;
}

int memory_db_pref_get(int user_id, const char *category, memory_preference_t *out_pref) {
   if (!category || !out_pref) {
      return MEMORY_DB_FAILURE;
   }

   AUTH_DB_LOCK_OR_RETURN(MEMORY_DB_FAILURE);

   sqlite3_stmt *stmt = s_db.stmt_memory_pref_get;
   sqlite3_reset(stmt);
   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_text(stmt, 2, category, -1, SQLITE_STATIC);

   int result = MEMORY_DB_NOT_FOUND;
   if (sqlite3_step(stmt) == SQLITE_ROW) {
      populate_pref_from_row(stmt, out_pref);
      result = MEMORY_DB_SUCCESS;
   }

   sqlite3_reset(stmt);
   AUTH_DB_UNLOCK();
   return result;
}

int memory_db_pref_list(int user_id, memory_preference_t *out_prefs, int max_prefs) {
   if (!out_prefs || max_prefs <= 0) {
      return -1;
   }

   AUTH_DB_LOCK_OR_RETURN(-1);

   sqlite3_stmt *stmt = s_db.stmt_memory_pref_list;
   sqlite3_reset(stmt);
   sqlite3_bind_int(stmt, 1, user_id);

   int count = 0;
   while (count < max_prefs && sqlite3_step(stmt) == SQLITE_ROW) {
      populate_pref_from_row(stmt, &out_prefs[count]);
      count++;
   }

   sqlite3_reset(stmt);
   AUTH_DB_UNLOCK();
   return count;
}

int memory_db_pref_search(int user_id,
                          const char *keywords,
                          memory_preference_t *out_prefs,
                          int max_prefs) {
   if (!keywords || !out_prefs || max_prefs <= 0) {
      return -1;
   }

   char pattern[256];
   build_like_pattern(keywords, pattern, sizeof(pattern));

   AUTH_DB_LOCK_OR_RETURN(-1);

   sqlite3_stmt *stmt = s_db.stmt_memory_pref_search;
   sqlite3_reset(stmt);
   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_text(stmt, 2, pattern, -1, SQLITE_STATIC);
   sqlite3_bind_text(stmt, 3, pattern, -1, SQLITE_STATIC);
   sqlite3_bind_int(stmt, 4, max_prefs);

   int count = 0;
   while (count < max_prefs && sqlite3_step(stmt) == SQLITE_ROW) {
      populate_pref_from_row(stmt, &out_prefs[count]);
      count++;
   }

   sqlite3_reset(stmt);
   AUTH_DB_UNLOCK();
   return count;
}

int memory_db_pref_delete(int user_id, const char *category) {
   if (!category) {
      return MEMORY_DB_FAILURE;
   }

   AUTH_DB_LOCK_OR_RETURN(MEMORY_DB_FAILURE);

   sqlite3_stmt *stmt = s_db.stmt_memory_pref_delete;
   sqlite3_reset(stmt);
   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_text(stmt, 2, category, -1, SQLITE_STATIC);

   int rc = sqlite3_step(stmt);
   int changes = sqlite3_changes(s_db.db);
   sqlite3_reset(stmt);

   AUTH_DB_UNLOCK();

   if (rc != SQLITE_DONE) {
      return MEMORY_DB_FAILURE;
   }
   return (changes > 0) ? MEMORY_DB_SUCCESS : MEMORY_DB_NOT_FOUND;
}

/* =============================================================================
 * Summary Operations
 * ============================================================================= */

int64_t memory_db_summary_create(int user_id,
                                 const char *session_id,
                                 const char *summary,
                                 const char *topics,
                                 const char *sentiment,
                                 int message_count,
                                 int duration_seconds) {
   if (!session_id || !summary) {
      return -1;
   }

   AUTH_DB_LOCK_OR_RETURN(-1);

   sqlite3_stmt *stmt = s_db.stmt_memory_summary_create;
   sqlite3_reset(stmt);
   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_text(stmt, 2, session_id, -1, SQLITE_STATIC);
   sqlite3_bind_text(stmt, 3, summary, -1, SQLITE_STATIC);
   sqlite3_bind_text(stmt, 4, topics ? topics : "", -1, SQLITE_STATIC);
   sqlite3_bind_text(stmt, 5, sentiment ? sentiment : "neutral", -1, SQLITE_STATIC);
   sqlite3_bind_int64(stmt, 6, (int64_t)time(NULL));
   sqlite3_bind_int(stmt, 7, message_count);
   sqlite3_bind_int(stmt, 8, duration_seconds);

   int rc = sqlite3_step(stmt);
   sqlite3_reset(stmt);

   if (rc != SQLITE_DONE) {
      LOG_ERROR("memory_db: summary_create failed: %s", sqlite3_errmsg(s_db.db));
      AUTH_DB_UNLOCK();
      return -1;
   }

   int64_t id = sqlite3_last_insert_rowid(s_db.db);
   AUTH_DB_UNLOCK();

   LOG_INFO("memory_db: created summary %ld for user %d", (long)id, user_id);
   return id;
}

int memory_db_summary_list(int user_id, memory_summary_t *out_summaries, int max_summaries) {
   if (!out_summaries || max_summaries <= 0) {
      return -1;
   }

   AUTH_DB_LOCK_OR_RETURN(-1);

   sqlite3_stmt *stmt = s_db.stmt_memory_summary_list;
   sqlite3_reset(stmt);
   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_int(stmt, 2, max_summaries);

   int count = 0;
   while (count < max_summaries && sqlite3_step(stmt) == SQLITE_ROW) {
      populate_summary_from_row(stmt, &out_summaries[count]);
      count++;
   }

   sqlite3_reset(stmt);
   AUTH_DB_UNLOCK();
   return count;
}

int memory_db_summary_mark_consolidated(int64_t summary_id) {
   AUTH_DB_LOCK_OR_RETURN(MEMORY_DB_FAILURE);

   sqlite3_stmt *stmt = s_db.stmt_memory_summary_mark_consolidated;
   sqlite3_reset(stmt);
   sqlite3_bind_int64(stmt, 1, summary_id);

   int rc = sqlite3_step(stmt);
   sqlite3_reset(stmt);

   AUTH_DB_UNLOCK();
   return (rc == SQLITE_DONE) ? MEMORY_DB_SUCCESS : MEMORY_DB_FAILURE;
}

int memory_db_summary_search(int user_id,
                             const char *keywords,
                             memory_summary_t *out_summaries,
                             int max_summaries) {
   if (!keywords || !out_summaries || max_summaries <= 0) {
      return -1;
   }

   char pattern[MEMORY_SUMMARY_MAX];
   build_like_pattern(keywords, pattern, sizeof(pattern));

   AUTH_DB_LOCK_OR_RETURN(-1);

   sqlite3_stmt *stmt = s_db.stmt_memory_summary_search;
   sqlite3_reset(stmt);
   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_text(stmt, 2, pattern, -1, SQLITE_STATIC);
   sqlite3_bind_text(stmt, 3, pattern, -1, SQLITE_STATIC);
   sqlite3_bind_int(stmt, 4, max_summaries);

   int count = 0;
   while (count < max_summaries && sqlite3_step(stmt) == SQLITE_ROW) {
      populate_summary_from_row(stmt, &out_summaries[count]);
      count++;
   }

   sqlite3_reset(stmt);
   AUTH_DB_UNLOCK();
   return count;
}

int memory_db_summary_delete(int64_t summary_id, int user_id) {
   if (summary_id <= 0 || user_id <= 0) {
      return MEMORY_DB_FAILURE;
   }

   AUTH_DB_LOCK_OR_RETURN(MEMORY_DB_FAILURE);

   sqlite3_stmt *stmt = NULL;
   int rc = sqlite3_prepare_v2(s_db.db, "DELETE FROM memory_summaries WHERE id = ? AND user_id = ?",
                               -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      LOG_ERROR("memory_db: summary_delete prepare failed: %s", sqlite3_errmsg(s_db.db));
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }

   sqlite3_bind_int64(stmt, 1, summary_id);
   sqlite3_bind_int(stmt, 2, user_id);

   rc = sqlite3_step(stmt);
   int changes = sqlite3_changes(s_db.db);
   sqlite3_finalize(stmt);

   AUTH_DB_UNLOCK();

   if (rc != SQLITE_DONE) {
      return MEMORY_DB_FAILURE;
   }
   return (changes > 0) ? MEMORY_DB_SUCCESS : MEMORY_DB_NOT_FOUND;
}

/* =============================================================================
 * Utility Operations
 * ============================================================================= */

int memory_db_delete_user_memories(int user_id) {
   if (user_id <= 0) {
      return MEMORY_DB_FAILURE;
   }

   AUTH_DB_LOCK_OR_RETURN(MEMORY_DB_FAILURE);

   sqlite3_stmt *stmt = NULL;
   int rc;

   /* Delete facts using prepared statement */
   rc = sqlite3_prepare_v2(s_db.db, "DELETE FROM memory_facts WHERE user_id = ?", -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      LOG_ERROR("memory_db: delete_user_memories (facts) prepare failed: %s",
                sqlite3_errmsg(s_db.db));
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }
   sqlite3_bind_int(stmt, 1, user_id);
   rc = sqlite3_step(stmt);
   sqlite3_finalize(stmt);
   if (rc != SQLITE_DONE) {
      LOG_ERROR("memory_db: delete_user_memories (facts) failed: %s", sqlite3_errmsg(s_db.db));
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }

   /* Delete preferences using prepared statement */
   rc = sqlite3_prepare_v2(s_db.db, "DELETE FROM memory_preferences WHERE user_id = ?", -1, &stmt,
                           NULL);
   if (rc != SQLITE_OK) {
      LOG_ERROR("memory_db: delete_user_memories (prefs) prepare failed: %s",
                sqlite3_errmsg(s_db.db));
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }
   sqlite3_bind_int(stmt, 1, user_id);
   rc = sqlite3_step(stmt);
   sqlite3_finalize(stmt);
   if (rc != SQLITE_DONE) {
      LOG_ERROR("memory_db: delete_user_memories (prefs) failed: %s", sqlite3_errmsg(s_db.db));
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }

   /* Delete summaries using prepared statement */
   rc = sqlite3_prepare_v2(s_db.db, "DELETE FROM memory_summaries WHERE user_id = ?", -1, &stmt,
                           NULL);
   if (rc != SQLITE_OK) {
      LOG_ERROR("memory_db: delete_user_memories (summaries) prepare failed: %s",
                sqlite3_errmsg(s_db.db));
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }
   sqlite3_bind_int(stmt, 1, user_id);
   rc = sqlite3_step(stmt);
   sqlite3_finalize(stmt);
   if (rc != SQLITE_DONE) {
      LOG_ERROR("memory_db: delete_user_memories (summaries) failed: %s", sqlite3_errmsg(s_db.db));
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }

   AUTH_DB_UNLOCK();
   LOG_INFO("memory_db: deleted all memories for user %d", user_id);
   return MEMORY_DB_SUCCESS;
}

int memory_db_get_stats(int user_id, memory_stats_t *out_stats) {
   if (!out_stats) {
      return MEMORY_DB_FAILURE;
   }

   memset(out_stats, 0, sizeof(memory_stats_t));

   AUTH_DB_LOCK_OR_RETURN(MEMORY_DB_FAILURE);

   sqlite3_stmt *stmt = NULL;
   int rc;

   /* Count facts */
   rc = sqlite3_prepare_v2(s_db.db,
                           "SELECT COUNT(*), MIN(created_at), MAX(created_at) "
                           "FROM memory_facts WHERE user_id = ? AND superseded_by IS NULL",
                           -1, &stmt, NULL);
   if (rc == SQLITE_OK) {
      sqlite3_bind_int(stmt, 1, user_id);
      if (sqlite3_step(stmt) == SQLITE_ROW) {
         out_stats->fact_count = sqlite3_column_int(stmt, 0);
         out_stats->oldest_fact = (time_t)sqlite3_column_int64(stmt, 1);
         out_stats->newest_fact = (time_t)sqlite3_column_int64(stmt, 2);
      }
      sqlite3_finalize(stmt);
   }

   /* Count preferences */
   rc = sqlite3_prepare_v2(s_db.db, "SELECT COUNT(*) FROM memory_preferences WHERE user_id = ?", -1,
                           &stmt, NULL);
   if (rc == SQLITE_OK) {
      sqlite3_bind_int(stmt, 1, user_id);
      if (sqlite3_step(stmt) == SQLITE_ROW) {
         out_stats->pref_count = sqlite3_column_int(stmt, 0);
      }
      sqlite3_finalize(stmt);
   }

   /* Count summaries */
   rc = sqlite3_prepare_v2(s_db.db, "SELECT COUNT(*) FROM memory_summaries WHERE user_id = ?", -1,
                           &stmt, NULL);
   if (rc == SQLITE_OK) {
      sqlite3_bind_int(stmt, 1, user_id);
      if (sqlite3_step(stmt) == SQLITE_ROW) {
         out_stats->summary_count = sqlite3_column_int(stmt, 0);
      }
      sqlite3_finalize(stmt);
   }

   AUTH_DB_UNLOCK();
   return MEMORY_DB_SUCCESS;
}

/* =============================================================================
 * Extraction Tracking
 * ============================================================================= */

int memory_db_get_last_extracted(int64_t conversation_id) {
   AUTH_DB_LOCK_OR_RETURN(-1);

   sqlite3_stmt *stmt = s_db.stmt_conv_get_last_extracted;
   sqlite3_reset(stmt);
   sqlite3_bind_int64(stmt, 1, conversation_id);

   int result = -1;
   if (sqlite3_step(stmt) == SQLITE_ROW) {
      result = sqlite3_column_int(stmt, 0);
   }

   sqlite3_reset(stmt);
   AUTH_DB_UNLOCK();
   return result;
}

int memory_db_set_last_extracted(int64_t conversation_id, int message_count) {
   AUTH_DB_LOCK_OR_RETURN(MEMORY_DB_FAILURE);

   sqlite3_stmt *stmt = s_db.stmt_conv_set_last_extracted;
   sqlite3_reset(stmt);
   sqlite3_bind_int(stmt, 1, message_count);
   sqlite3_bind_int64(stmt, 2, conversation_id);

   int rc = sqlite3_step(stmt);
   sqlite3_reset(stmt);

   AUTH_DB_UNLOCK();
   return (rc == SQLITE_DONE) ? MEMORY_DB_SUCCESS : MEMORY_DB_FAILURE;
}

/* =============================================================================
 * Decay and Maintenance Operations (Phase 5)
 *
 * These use ad-hoc prepared statements (acceptable for once-daily execution).
 * ============================================================================= */

int memory_db_apply_fact_decay(int user_id,
                               float inferred_rate,
                               float explicit_rate,
                               float inferred_floor,
                               float explicit_floor) {
   AUTH_DB_LOCK_OR_RETURN(-1);

   sqlite3_stmt *stmt = NULL;
   int rc = sqlite3_prepare_v2(
       s_db.db,
       "UPDATE memory_facts SET confidence ="
       "  CASE WHEN source = 'explicit'"
       "    THEN MAX(?, confidence * powf(?, "
       "         (CAST(strftime('%s','now') AS REAL) - last_accessed) / 604800.0))"
       "    ELSE MAX(?, confidence * powf(?, "
       "         (CAST(strftime('%s','now') AS REAL) - last_accessed) / 604800.0))"
       "  END "
       "WHERE user_id = ? AND superseded_by IS NULL AND last_accessed IS NOT NULL",
       -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      LOG_ERROR("memory_db: apply_fact_decay prepare failed: %s", sqlite3_errmsg(s_db.db));
      AUTH_DB_UNLOCK();
      return -1;
   }

   sqlite3_bind_double(stmt, 1, (double)explicit_floor);
   sqlite3_bind_double(stmt, 2, (double)explicit_rate);
   sqlite3_bind_double(stmt, 3, (double)inferred_floor);
   sqlite3_bind_double(stmt, 4, (double)inferred_rate);
   sqlite3_bind_int(stmt, 5, user_id);

   rc = sqlite3_step(stmt);
   int affected = sqlite3_changes(s_db.db);
   sqlite3_finalize(stmt);

   AUTH_DB_UNLOCK();

   if (rc != SQLITE_DONE) {
      LOG_ERROR("memory_db: apply_fact_decay failed: %s", sqlite3_errmsg(s_db.db));
      return -1;
   }

   return affected;
}

int memory_db_apply_pref_decay(int user_id, float pref_rate, float pref_floor) {
   AUTH_DB_LOCK_OR_RETURN(-1);

   sqlite3_stmt *stmt = NULL;
   int rc = sqlite3_prepare_v2(
       s_db.db,
       "UPDATE memory_preferences SET confidence ="
       "  MAX(?, confidence * powf(?, "
       "      (CAST(strftime('%s','now') AS REAL) - updated_at) / 604800.0))"
       " WHERE user_id = ?",
       -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      LOG_ERROR("memory_db: apply_pref_decay prepare failed: %s", sqlite3_errmsg(s_db.db));
      AUTH_DB_UNLOCK();
      return -1;
   }

   sqlite3_bind_double(stmt, 1, (double)pref_floor);
   sqlite3_bind_double(stmt, 2, (double)pref_rate);
   sqlite3_bind_int(stmt, 3, user_id);

   rc = sqlite3_step(stmt);
   int affected = sqlite3_changes(s_db.db);
   sqlite3_finalize(stmt);

   AUTH_DB_UNLOCK();

   if (rc != SQLITE_DONE) {
      LOG_ERROR("memory_db: apply_pref_decay failed: %s", sqlite3_errmsg(s_db.db));
      return -1;
   }

   return affected;
}

int memory_db_prune_low_confidence(int user_id, float threshold) {
   AUTH_DB_LOCK_OR_RETURN(-1);

   /* Wrap audit log + delete in a transaction for consistency */
   sqlite3_exec(s_db.db, "BEGIN IMMEDIATE", NULL, NULL, NULL);

   /* Log facts that will be pruned (audit trail for irreversible operation) */
   sqlite3_stmt *log_stmt = NULL;
   int rc = sqlite3_prepare_v2(s_db.db,
                               "SELECT id, fact_text, confidence, source FROM memory_facts "
                               "WHERE user_id = ? AND confidence < ? AND superseded_by IS NULL",
                               -1, &log_stmt, NULL);
   if (rc == SQLITE_OK) {
      sqlite3_bind_int(log_stmt, 1, user_id);
      sqlite3_bind_double(log_stmt, 2, (double)threshold);

      while (sqlite3_step(log_stmt) == SQLITE_ROW) {
         LOG_INFO("memory_decay: pruning fact %ld (%.2f, %s): %.60s",
                  (long)sqlite3_column_int64(log_stmt, 0), sqlite3_column_double(log_stmt, 2),
                  (const char *)sqlite3_column_text(log_stmt, 3),
                  (const char *)sqlite3_column_text(log_stmt, 1));
      }
      sqlite3_finalize(log_stmt);
   }

   /* Delete low-confidence facts */
   sqlite3_stmt *stmt = NULL;
   rc = sqlite3_prepare_v2(
       s_db.db,
       "DELETE FROM memory_facts WHERE user_id = ? AND confidence < ? AND superseded_by IS NULL",
       -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      LOG_ERROR("memory_db: prune_low_confidence prepare failed: %s", sqlite3_errmsg(s_db.db));
      sqlite3_exec(s_db.db, "ROLLBACK", NULL, NULL, NULL);
      AUTH_DB_UNLOCK();
      return -1;
   }

   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_double(stmt, 2, (double)threshold);

   rc = sqlite3_step(stmt);
   int deleted = sqlite3_changes(s_db.db);
   sqlite3_finalize(stmt);

   if (rc != SQLITE_DONE) {
      LOG_ERROR("memory_db: prune_low_confidence failed: %s", sqlite3_errmsg(s_db.db));
      sqlite3_exec(s_db.db, "ROLLBACK", NULL, NULL, NULL);
      AUTH_DB_UNLOCK();
      return -1;
   }

   sqlite3_exec(s_db.db, "COMMIT", NULL, NULL, NULL);
   AUTH_DB_UNLOCK();

   return deleted;
}

int memory_db_prune_old_summaries(int user_id, int retention_days) {
   AUTH_DB_LOCK_OR_RETURN(-1);

   time_t cutoff = time(NULL) - ((time_t)retention_days * 24 * 60 * 60);

   sqlite3_stmt *stmt = NULL;
   int rc = sqlite3_prepare_v2(s_db.db,
                               "DELETE FROM memory_summaries WHERE user_id = ? AND created_at < ?",
                               -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      LOG_ERROR("memory_db: prune_old_summaries prepare failed: %s", sqlite3_errmsg(s_db.db));
      AUTH_DB_UNLOCK();
      return -1;
   }

   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_int64(stmt, 2, (int64_t)cutoff);

   rc = sqlite3_step(stmt);
   int deleted = sqlite3_changes(s_db.db);
   sqlite3_finalize(stmt);

   AUTH_DB_UNLOCK();

   if (rc != SQLITE_DONE) {
      LOG_ERROR("memory_db: prune_old_summaries failed: %s", sqlite3_errmsg(s_db.db));
      return -1;
   }

   if (deleted > 0) {
      LOG_INFO("memory_db: pruned %d old summaries for user %d", deleted, user_id);
   }
   return deleted;
}

int memory_db_get_all_user_ids(int *out_ids, int max_ids) {
   if (!out_ids || max_ids <= 0) {
      return -1;
   }

   AUTH_DB_LOCK_OR_RETURN(-1);

   sqlite3_stmt *stmt = NULL;
   int rc = sqlite3_prepare_v2(s_db.db,
                               "SELECT user_id FROM memory_facts "
                               "UNION SELECT user_id FROM memory_preferences",
                               -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      LOG_ERROR("memory_db: get_all_user_ids prepare failed: %s", sqlite3_errmsg(s_db.db));
      AUTH_DB_UNLOCK();
      return -1;
   }

   int count = 0;
   while (count < max_ids && sqlite3_step(stmt) == SQLITE_ROW) {
      out_ids[count++] = sqlite3_column_int(stmt, 0);
   }

   sqlite3_finalize(stmt);
   AUTH_DB_UNLOCK();
   return count;
}
