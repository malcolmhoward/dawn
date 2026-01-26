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
 * Memory Context API
 *
 * Builds memory context blocks to inject into LLM system prompts.
 */

#ifndef MEMORY_CONTEXT_H
#define MEMORY_CONTEXT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Build memory context for a user
 *
 * Constructs a formatted string containing the user's preferences, known facts,
 * and recent conversation summaries. This is designed to be injected into the
 * LLM system prompt to provide personalization.
 *
 * The output is formatted in sections:
 * - USER PREFERENCES: category: value (from memory_preferences)
 * - KNOWN FACTS: bullet points (from memory_facts)
 * - RECENT CONVERSATIONS: summaries with topics (from memory_summaries)
 *
 * @param user_id User ID to build context for
 * @param buffer Output buffer for the context string
 * @param buffer_size Size of the output buffer
 * @param token_budget Approximate token budget (chars/4, default ~800)
 * @return Number of characters written, or 0 if no memories found or error
 */
int memory_build_context(int user_id, char *buffer, size_t buffer_size, int token_budget);

#ifdef __cplusplus
}
#endif

#endif /* MEMORY_CONTEXT_H */
