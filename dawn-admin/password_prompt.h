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
 * Secure password prompting for dawn-admin CLI.
 *
 * Security features:
 * - Disables terminal echo during password entry
 * - Installs signal handlers to restore terminal on Ctrl+C
 * - Uses explicit_bzero() to clear sensitive data
 * - Validates password confirmation matches
 */

#ifndef DAWN_ADMIN_PASSWORD_PROMPT_H
#define DAWN_ADMIN_PASSWORD_PROMPT_H

#include <stddef.h>

/**
 * @brief Minimum required password length.
 */
#define PASSWORD_MIN_LENGTH 8

/**
 * @brief Maximum password length.
 */
#define PASSWORD_MAX_LENGTH 256

/**
 * @brief Prompt for a password without echo.
 *
 * Disables terminal echo, prompts for input, and restores terminal.
 * Handles Ctrl+C gracefully by restoring terminal before exiting.
 *
 * @param prompt  Text to display as prompt.
 * @param buf     Buffer to store password.
 * @param buflen  Size of buffer (must be > PASSWORD_MIN_LENGTH).
 *
 * @return 0 on success, -1 on failure or user cancellation.
 */
int prompt_password(const char *prompt, char *buf, size_t buflen);

/**
 * @brief Prompt for a password with confirmation.
 *
 * Prompts for password twice and verifies they match.
 * Returns error if passwords don't match.
 *
 * @param buf     Buffer to store confirmed password.
 * @param buflen  Size of buffer.
 *
 * @return 0 on success, -1 on failure or mismatch.
 */
int prompt_password_confirm(char *buf, size_t buflen);

/**
 * @brief Prompt for a single line of input (with echo).
 *
 * Used for non-sensitive input like setup token.
 *
 * @param prompt  Text to display as prompt.
 * @param buf     Buffer to store input.
 * @param buflen  Size of buffer.
 *
 * @return 0 on success, -1 on failure.
 */
int prompt_input(const char *prompt, char *buf, size_t buflen);

/**
 * @brief Clear sensitive data from memory.
 *
 * Uses explicit_bzero() if available, otherwise volatile memset.
 *
 * @param buf    Buffer to clear.
 * @param buflen Size of buffer.
 */
void secure_clear(void *buf, size_t buflen);

#endif /* DAWN_ADMIN_PASSWORD_PROMPT_H */
