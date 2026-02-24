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
 * Memory Tool - Persistent memory for facts and preferences
 *
 * Supports: remember (store), search (query), forget (delete), recent (list)
 * Implementation is in memory/memory_callback.c, this file handles tool registration.
 */

#include "tools/memory_tool.h"

#include "config/dawn_config.h"
#include "logging.h"
#include "tools/tool_registry.h"

/* Forward declaration of callback from memory/memory_callback.c */
char *memoryCallback(const char *actionName, char *value, int *should_respond);

/* Forward declaration for availability check */
static bool memory_tool_is_available(void);

/* ========== Tool Parameter Definition ========== */

static const treg_param_t memory_params[] = {
   {
       .name = "action",
       .description = "Memory action: 'remember' (store a fact), 'search' (find memories), "
                      "'forget' (delete a memory by ID), 'recent' (list recent memories)",
       .type = TOOL_PARAM_TYPE_ENUM,
       .required = true,
       .maps_to = TOOL_MAPS_TO_ACTION,
       .enum_values = { "remember", "search", "forget", "recent" },
       .enum_count = 4,
   },
   {
       .name = "query",
       .description = "For 'remember': the fact to store (e.g., 'User prefers dark mode'). "
                      "For 'search': keywords to find relevant memories. "
                      "For 'forget': memory ID to delete. "
                      "For 'recent': optional limit number (default 10).",
       .type = TOOL_PARAM_TYPE_STRING,
       .required = false,
       .maps_to = TOOL_MAPS_TO_VALUE,
   },
   {
       .name = "time_range",
       .description = "Optional time filter for 'search'. Limit results to memories from this "
                      "period. Examples: '24h', '7d', '2w', '30d'. Only used with 'search'.",
       .type = TOOL_PARAM_TYPE_STRING,
       .required = false,
       .maps_to = TOOL_MAPS_TO_CUSTOM,
       .field_name = "time_range",
   },
};

/* ========== Tool Metadata ========== */

static const tool_metadata_t memory_metadata = {
   .name = "memory",
   .device_string = "memory",
   .topic = "dawn",
   .aliases = { "remember", "recall" },
   .alias_count = 2,

   .description = "Store and retrieve persistent memories about the user. "
                  "Use 'remember' to store facts (preferences, information shared by user). "
                  "Use 'search' to find relevant stored memories (optionally filtered by "
                  "time_range like '24h', '7d', '2w'). "
                  "Use 'forget' to delete a specific memory by ID. "
                  "Use 'recent' to list recently stored memories. "
                  "Memories persist across sessions and are private to each user.",
   .params = memory_params,
   .param_count = 3,

   .device_type = TOOL_DEVICE_TYPE_GETTER,
   .capabilities = TOOL_CAP_FILESYSTEM,
   .is_getter = false, /* remember/forget have side effects */
   .skip_followup = false,
   .default_remote = true,

   .config = NULL,
   .config_size = 0,
   .config_parser = NULL,
   .config_section = NULL, /* Uses [memory] section from main config */

   .is_available = memory_tool_is_available,

   .init = NULL,
   .cleanup = NULL,
   .callback = memoryCallback, /* Use existing callback from memory_callback.c */
};

/* ========== Availability Check ========== */

static bool memory_tool_is_available(void) {
   return g_config.memory.enabled;
}

/* ========== Public API ========== */

int memory_tool_register(void) {
   return tool_registry_register(&memory_metadata);
}
