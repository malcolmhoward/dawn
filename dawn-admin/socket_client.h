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
 * Socket client for dawn-admin CLI to communicate with Dawn daemon.
 */

#ifndef DAWN_ADMIN_SOCKET_CLIENT_H
#define DAWN_ADMIN_SOCKET_CLIENT_H

#include <stdbool.h>
#include <stdint.h>

#include "auth/admin_socket.h"

/**
 * @brief Connect to the Dawn admin socket.
 *
 * Attempts to connect to the Dawn daemon's admin socket using the
 * abstract socket namespace (Linux) or filesystem socket fallback.
 *
 * @return Socket file descriptor on success, -1 on failure.
 */
int admin_client_connect(void);

/**
 * @brief Disconnect from the admin socket.
 *
 * @param fd Socket file descriptor to close.
 */
void admin_client_disconnect(int fd);

/**
 * @brief Send a ping message to verify daemon connectivity.
 *
 * @param fd Socket file descriptor.
 *
 * @return 0 on success (daemon responded), non-zero on failure.
 */
int admin_client_ping(int fd);

/**
 * @brief Validate a setup token with the daemon.
 *
 * @param fd    Socket file descriptor.
 * @param token The setup token to validate (DAWN-XXXX-XXXX-XXXX-XXXX format).
 *
 * @return Response code from daemon (ADMIN_RESP_SUCCESS on success).
 */
admin_resp_code_t admin_client_validate_token(int fd, const char *token);

/**
 * @brief Create a user account (atomic token validation + user creation).
 *
 * This combines setup token validation and user creation into a single
 * atomic operation to prevent race conditions.
 *
 * @param fd       Socket file descriptor.
 * @param token    The setup token (DAWN-XXXX-XXXX-XXXX-XXXX format).
 * @param username Username for the new account.
 * @param password Password for the new account.
 * @param is_admin Whether the user should have admin privileges.
 *
 * @return Response code from daemon (ADMIN_RESP_SUCCESS on success).
 */
admin_resp_code_t admin_client_create_user(int fd,
                                           const char *token,
                                           const char *username,
                                           const char *password,
                                           bool is_admin);

/**
 * @brief Get a human-readable error message for a response code.
 *
 * @param code Response code from daemon.
 *
 * @return Static string describing the error.
 */
const char *admin_resp_strerror(admin_resp_code_t code);

#endif /* DAWN_ADMIN_SOCKET_CLIENT_H */
