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
 * the project author(s).
 *
 * LLM-based fact recategorization for memory facts still labeled "general".
 */

#ifndef MEMORY_RECATEGORIZE_H
#define MEMORY_RECATEGORIZE_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start LLM-based recategorization of 'general' facts for a user.
 *
 * Spawns a detached background thread that fetches general-category facts in
 * batches, sends each batch to the extraction LLM for classification, and
 * updates each fact's category.  Non-blocking; progress logged via OLOG_INFO.
 * Idempotent: only processes facts with category='general'.
 *
 * @param user_id User whose facts to recategorize
 * @return 0 if thread spawned successfully, non-zero on error
 */
int memory_recategorize_start(int user_id);

/**
 * @brief Check if recategorization is currently running.
 * @return true if a recategorization thread is active
 */
bool memory_recategorize_is_running(void);

/**
 * @brief Signal the recategorization thread to stop and wait briefly.
 *
 * Called during daemon shutdown to avoid mid-write termination.
 */
void memory_recategorize_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* MEMORY_RECATEGORIZE_H */
