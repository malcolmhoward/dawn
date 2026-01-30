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
 */

#ifndef MEMORY_TOOL_H
#define MEMORY_TOOL_H

/**
 * @brief Register the memory tool with the tool registry
 *
 * Call this during startup to register the memory storage tool.
 * Supports actions: remember (store fact), search (query), forget (delete), recent (list)
 *
 * @return 0 on success, non-zero on failure
 */
int memory_tool_register(void);

#endif /* MEMORY_TOOL_H */
