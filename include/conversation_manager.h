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
 * @file conversation_manager.h
 * @brief Public interface for conversation history management.
 *
 * Provides thread-safe functions for managing LLM conversation context,
 * including reset operations that save history before clearing.
 */

#ifndef CONVERSATION_MANAGER_H
#define CONVERSATION_MANAGER_H

/**
 * @brief Reset the conversation context.
 *
 * Saves the current conversation history to a JSON file (if non-empty),
 * clears the LLM context, reinitializes with the system prompt, and
 * resets session metrics.
 *
 * @note Thread-safe: Uses internal mutex protection.
 * @note Can be called from any thread (TUI, MQTT callbacks, etc.)
 */
void reset_conversation(void);

#endif /* CONVERSATION_MANAGER_H */
