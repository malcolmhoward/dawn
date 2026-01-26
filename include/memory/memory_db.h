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
 * Memory Database API
 *
 * Provides CRUD operations for memory facts, preferences, and summaries.
 * Uses the auth_db module's SQLite database and prepared statements.
 * All functions are thread-safe via auth_db's mutex protection.
 */

#ifndef MEMORY_DB_H
#define MEMORY_DB_H

#include "memory/memory_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Return codes */
#define MEMORY_DB_SUCCESS 0
#define MEMORY_DB_FAILURE 1
#define MEMORY_DB_NOT_FOUND 2
#define MEMORY_DB_DUPLICATE 3

/* =============================================================================
 * Fact Operations
 * ============================================================================= */

/**
 * @brief Create a new memory fact
 *
 * @param user_id User who owns this fact
 * @param fact_text The fact content
 * @param confidence Confidence level (0.0-1.0)
 * @param source Source of fact ("explicit" or "inferred")
 * @return Fact ID on success, -1 on failure
 */
int64_t memory_db_fact_create(int user_id,
                              const char *fact_text,
                              float confidence,
                              const char *source);

/**
 * @brief Get a fact by ID
 *
 * @param fact_id Fact ID to retrieve
 * @param out_fact Output: populated fact structure
 * @return MEMORY_DB_SUCCESS, MEMORY_DB_NOT_FOUND, or MEMORY_DB_FAILURE
 */
int memory_db_fact_get(int64_t fact_id, memory_fact_t *out_fact);

/**
 * @brief List facts for a user (non-superseded only)
 *
 * @param user_id User ID
 * @param out_facts Output: array of facts (caller allocates)
 * @param max_facts Maximum facts to return
 * @param offset Starting offset for pagination
 * @return Number of facts returned, -1 on error
 */
int memory_db_fact_list(int user_id, memory_fact_t *out_facts, int max_facts, int offset);

/**
 * @brief Search facts by keyword
 *
 * @param user_id User ID
 * @param keywords Search terms (will be wrapped in %...%)
 * @param out_facts Output: array of matching facts
 * @param max_facts Maximum facts to return
 * @return Number of facts found, -1 on error
 */
int memory_db_fact_search(int user_id,
                          const char *keywords,
                          memory_fact_t *out_facts,
                          int max_facts);

/**
 * @brief Update fact access time and count
 *
 * Called when a fact is retrieved for context injection.
 *
 * @param fact_id Fact ID to update
 * @return MEMORY_DB_SUCCESS or MEMORY_DB_FAILURE
 */
int memory_db_fact_update_access(int64_t fact_id);

/**
 * @brief Update fact confidence
 *
 * @param fact_id Fact ID
 * @param confidence New confidence value
 * @return MEMORY_DB_SUCCESS or MEMORY_DB_FAILURE
 */
int memory_db_fact_update_confidence(int64_t fact_id, float confidence);

/**
 * @brief Mark a fact as superseded by another
 *
 * Used when a fact is corrected or updated.
 *
 * @param old_fact_id Fact being superseded
 * @param new_fact_id Fact that supersedes it
 * @return MEMORY_DB_SUCCESS or MEMORY_DB_FAILURE
 */
int memory_db_fact_supersede(int64_t old_fact_id, int64_t new_fact_id);

/**
 * @brief Delete a fact
 *
 * @param fact_id Fact ID
 * @param user_id User ID (for ownership check)
 * @return MEMORY_DB_SUCCESS, MEMORY_DB_NOT_FOUND, or MEMORY_DB_FAILURE
 */
int memory_db_fact_delete(int64_t fact_id, int user_id);

/**
 * @brief Find similar facts (for duplicate detection)
 *
 * Uses LIKE pattern matching on fact text.
 *
 * @param user_id User ID
 * @param fact_text Text to search for
 * @param out_facts Output: array of similar facts
 * @param max_facts Maximum facts to return
 * @return Number of similar facts found, -1 on error
 */
int memory_db_fact_find_similar(int user_id,
                                const char *fact_text,
                                memory_fact_t *out_facts,
                                int max_facts);

/**
 * @brief Find facts by normalized hash (fast duplicate detection)
 *
 * Looks up facts by their normalized text hash for O(1) exact duplicate
 * detection. Hash collisions are expected; callers should verify with
 * Jaccard similarity.
 *
 * @param user_id User ID
 * @param hash Normalized text hash from memory_normalize_and_hash()
 * @param out_facts Output: array of facts with matching hash
 * @param max_facts Maximum facts to return
 * @return Number of facts found, -1 on error
 */
int memory_db_fact_find_by_hash(int user_id,
                                uint32_t hash,
                                memory_fact_t *out_facts,
                                int max_facts);

/**
 * @brief Prune old superseded facts
 *
 * Deletes facts that have been superseded by newer facts and are older
 * than the retention period.
 *
 * @param user_id User ID
 * @param retention_days Keep superseded facts for this many days
 * @return Number of facts deleted, -1 on error
 */
int memory_db_fact_prune_superseded(int user_id, int retention_days);

/**
 * @brief Prune stale low-confidence facts
 *
 * Deletes facts that haven't been accessed in a long time and have
 * low confidence scores.
 *
 * @param user_id User ID
 * @param stale_days Prune facts not accessed in this many days
 * @param min_confidence Only prune facts with confidence below this
 * @return Number of facts deleted, -1 on error
 */
int memory_db_fact_prune_stale(int user_id, int stale_days, float min_confidence);

/* =============================================================================
 * Preference Operations
 * ============================================================================= */

/**
 * @brief Upsert a preference (insert or update if exists)
 *
 * If a preference with the same category exists for this user,
 * it will be updated with the new value and its reinforcement_count
 * will be incremented.
 *
 * @param user_id User ID
 * @param category Preference category
 * @param value Preference value
 * @param confidence Confidence level
 * @param source Source ("explicit" or "inferred")
 * @return MEMORY_DB_SUCCESS or MEMORY_DB_FAILURE
 */
int memory_db_pref_upsert(int user_id,
                          const char *category,
                          const char *value,
                          float confidence,
                          const char *source);

/**
 * @brief Get a preference by category
 *
 * @param user_id User ID
 * @param category Category to look up
 * @param out_pref Output: populated preference
 * @return MEMORY_DB_SUCCESS, MEMORY_DB_NOT_FOUND, or MEMORY_DB_FAILURE
 */
int memory_db_pref_get(int user_id, const char *category, memory_preference_t *out_pref);

/**
 * @brief List all preferences for a user
 *
 * @param user_id User ID
 * @param out_prefs Output: array of preferences
 * @param max_prefs Maximum to return
 * @return Number of preferences, -1 on error
 */
int memory_db_pref_list(int user_id, memory_preference_t *out_prefs, int max_prefs);

/**
 * @brief Delete a preference
 *
 * @param user_id User ID
 * @param category Category to delete
 * @return MEMORY_DB_SUCCESS, MEMORY_DB_NOT_FOUND, or MEMORY_DB_FAILURE
 */
int memory_db_pref_delete(int user_id, const char *category);

/* =============================================================================
 * Summary Operations
 * ============================================================================= */

/**
 * @brief Create a conversation summary
 *
 * @param user_id User ID
 * @param session_id Session identifier
 * @param summary Summary text
 * @param topics Comma-separated topics
 * @param sentiment Overall sentiment
 * @param message_count Number of messages in conversation
 * @param duration_seconds Session duration
 * @return Summary ID on success, -1 on failure
 */
int64_t memory_db_summary_create(int user_id,
                                 const char *session_id,
                                 const char *summary,
                                 const char *topics,
                                 const char *sentiment,
                                 int message_count,
                                 int duration_seconds);

/**
 * @brief List recent summaries for a user
 *
 * Only returns non-consolidated summaries.
 *
 * @param user_id User ID
 * @param out_summaries Output: array of summaries
 * @param max_summaries Maximum to return
 * @return Number of summaries, -1 on error
 */
int memory_db_summary_list(int user_id, memory_summary_t *out_summaries, int max_summaries);

/**
 * @brief Mark a summary as consolidated
 *
 * @param summary_id Summary ID
 * @return MEMORY_DB_SUCCESS or MEMORY_DB_FAILURE
 */
int memory_db_summary_mark_consolidated(int64_t summary_id);

/**
 * @brief Search summaries by keyword
 *
 * Searches both summary text and topics.
 *
 * @param user_id User ID
 * @param keywords Search terms
 * @param out_summaries Output: array of matching summaries
 * @param max_summaries Maximum to return
 * @return Number of matches, -1 on error
 */
int memory_db_summary_search(int user_id,
                             const char *keywords,
                             memory_summary_t *out_summaries,
                             int max_summaries);

/* =============================================================================
 * Utility Operations
 * ============================================================================= */

/**
 * @brief Delete all memories for a user
 *
 * Used when a user requests to be forgotten.
 *
 * @param user_id User ID
 * @return MEMORY_DB_SUCCESS or MEMORY_DB_FAILURE
 */
int memory_db_delete_user_memories(int user_id);

/**
 * @brief Get memory statistics for a user
 *
 * @param user_id User ID
 * @param out_stats Output: statistics
 * @return MEMORY_DB_SUCCESS or MEMORY_DB_FAILURE
 */
int memory_db_get_stats(int user_id, memory_stats_t *out_stats);

/* =============================================================================
 * Extraction Tracking
 * ============================================================================= */

/**
 * @brief Get last extracted message count for a conversation
 *
 * Used to track which messages have already been processed for
 * memory extraction, enabling incremental extraction.
 *
 * @param conversation_id Conversation ID
 * @return Last extracted message count, or -1 on error
 */
int memory_db_get_last_extracted(int64_t conversation_id);

/**
 * @brief Set last extracted message count for a conversation
 *
 * @param conversation_id Conversation ID
 * @param message_count Message count to record
 * @return MEMORY_DB_SUCCESS or MEMORY_DB_FAILURE
 */
int memory_db_set_last_extracted(int64_t conversation_id, int message_count);

#ifdef __cplusplus
}
#endif

#endif /* MEMORY_DB_H */
