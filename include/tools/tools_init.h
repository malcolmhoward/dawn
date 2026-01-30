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
 * Tools Initialization - Central registration point for all modular tools
 *
 * This module provides a single entry point for registering all tools,
 * keeping tool-specific includes and ifdef guards out of dawn.c.
 */

#ifndef TOOLS_INIT_H
#define TOOLS_INIT_H

/**
 * @brief Register all enabled tools with the tool registry
 *
 * This function registers all tools that are enabled via DAWN_ENABLE_X_TOOL
 * compile-time flags. It should be called after tool_registry_init() and
 * before tool_registry_parse_configs().
 *
 * @return 0 on success (warnings logged for individual tool failures)
 */
int tools_register_all(void);

#endif /* TOOLS_INIT_H */
