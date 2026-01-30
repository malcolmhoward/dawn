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
 * Shutdown Tool - System shutdown command with security controls
 *
 * This tool provides voice/command-initiated system shutdown with
 * configurable security:
 * - Must be explicitly enabled in config (default: disabled)
 * - Optional passphrase requirement
 * - Constant-time passphrase comparison for security
 */

#ifndef SHUTDOWN_TOOL_H
#define SHUTDOWN_TOOL_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Register the shutdown tool with the tool registry
 *
 * This function must be called during initialization to make the
 * shutdown tool available. It registers the tool metadata, config
 * parser, and callback with the tool registry.
 *
 * @return 0 on success, non-zero on error
 */
int shutdown_tool_register(void);

#ifdef __cplusplus
}
#endif

#endif /* SHUTDOWN_TOOL_H */
