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
 * Memory Extraction API
 *
 * Triggers memory extraction at session end to store facts, preferences,
 * and conversation summaries.
 */

#ifndef MEMORY_EXTRACTION_H
#define MEMORY_EXTRACTION_H

#include <json-c/json.h>
#include <stdbool.h>

#include "memory/memory_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Trigger memory extraction for a session
 *
 * Spawns a detached thread to extract facts, preferences, and a summary
 * from the conversation history. The extraction runs asynchronously so
 * it doesn't block session cleanup.
 *
 * Supports incremental extraction: if conversation_id is provided, only
 * processes messages since the last extraction (tracked via last_extracted_msg_count).
 *
 * @param user_id User ID (must be > 0)
 * @param conversation_id Conversation ID for tracking (0 to skip tracking)
 * @param session_id_str Session identifier string (for summary)
 * @param conversation_history JSON array of messages (copied internally)
 * @param message_count Number of messages in conversation
 * @param duration_seconds Session duration
 * @return 0 on success, 1 on failure
 */
int memory_trigger_extraction(int user_id,
                              int64_t conversation_id,
                              const char *session_id_str,
                              struct json_object *conversation_history,
                              int message_count,
                              int duration_seconds);

/**
 * @brief Check if extraction is in progress for a user
 *
 * @param user_id User ID to check
 * @return true if extraction is running, false otherwise
 */
bool memory_extraction_in_progress(int user_id);

#ifdef __cplusplus
}
#endif

#endif /* MEMORY_EXTRACTION_H */
