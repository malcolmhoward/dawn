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
 * Admin socket infrastructure for dawn-admin CLI communication.
 *
 * This module provides a Unix domain socket interface for the dawn-admin CLI
 * tool to communicate with the Dawn daemon. It handles setup token validation
 * for first-run bootstrap and will support user/device management in Phase 1+.
 *
 * Security considerations:
 * - Uses abstract socket namespace on Linux (no filesystem permissions)
 * - Validates peer credentials via SO_PEERCRED (root or daemon UID only)
 * - Constant-time token comparison to prevent timing attacks
 * - Rate limiting with persistent state to survive restarts
 * - No fallback from getrandom() - fails closed on entropy failure
 */

#ifndef DAWN_AUTH_ADMIN_SOCKET_H
#define DAWN_AUTH_ADMIN_SOCKET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * =============================================================================
 * Socket Configuration
 * =============================================================================
 */

/**
 * @brief Abstract socket name (Linux-specific, no filesystem cleanup needed).
 *
 * The leading null byte indicates abstract namespace. The actual name follows.
 * This avoids TOCTOU race conditions with filesystem-based sockets.
 */
#define ADMIN_SOCKET_ABSTRACT_NAME "dawn-admin"

/**
 * @brief Fallback filesystem socket path for non-Linux systems.
 *
 * Used only if abstract sockets are unavailable. Requires proper umask
 * handling and permission verification after bind().
 */
#define ADMIN_SOCKET_PATH "/run/dawn/admin.sock"

/**
 * @brief Directory for socket and state files.
 */
#define ADMIN_SOCKET_DIR "/run/dawn"

/**
 * @brief Maximum concurrent admin connections.
 *
 * Set to 1 to prevent DoS and simplify state management.
 * Only one admin tool should be connected at a time.
 */
#define ADMIN_MAX_CONNECTIONS 1

/**
 * @brief Connection timeout in seconds.
 *
 * Stalled connections are terminated after this period.
 */
#define ADMIN_CONN_TIMEOUT_SEC 30

/*
 * =============================================================================
 * Protocol Definition
 * =============================================================================
 */

/**
 * @brief Protocol version for wire format compatibility.
 *
 * Increment when making breaking changes to the protocol.
 * Clients with mismatched versions receive ADMIN_RESP_VERSION_MISMATCH.
 */
#define ADMIN_PROTOCOL_VERSION 0x01

/**
 * @brief Message types for admin socket protocol.
 *
 * Phase 0 implements only PING and VALIDATE_SETUP_TOKEN.
 * Additional message types will be added in Phase 1+.
 */
typedef enum {
   ADMIN_MSG_PING = 0x01,                 /**< Health check / keepalive */
   ADMIN_MSG_VALIDATE_SETUP_TOKEN = 0x02, /**< Validate first-run setup token */

   /* Phase 1+ message types (reserved) */
   ADMIN_MSG_CREATE_USER = 0x10, /**< Create user account (Phase 1) */
   ADMIN_MSG_LIST_USERS = 0x11,  /**< List user accounts (Phase 2) */
   ADMIN_MSG_DELETE_USER = 0x12, /**< Delete user account (Phase 2) */
} admin_msg_type_t;

/**
 * @brief Response codes for admin socket protocol.
 *
 * Uses generic failure codes to prevent information leakage about
 * token validity, expiration, or usage status.
 */
typedef enum {
   ADMIN_RESP_SUCCESS = 0x00,          /**< Operation succeeded */
   ADMIN_RESP_FAILURE = 0x01,          /**< Generic failure (invalid/expired/used) */
   ADMIN_RESP_RATE_LIMITED = 0x02,     /**< Too many failed attempts */
   ADMIN_RESP_SERVICE_ERROR = 0x03,    /**< Internal error */
   ADMIN_RESP_VERSION_MISMATCH = 0x04, /**< Protocol version incompatible */
   ADMIN_RESP_UNAUTHORIZED = 0x05,     /**< Peer credentials rejected */
} admin_resp_code_t;

/**
 * @brief Maximum payload size in bytes.
 *
 * Setup token is 24 bytes (DAWN-XXXX-XXXX-XXXX-XXXX).
 * 256 bytes provides room for future expansion.
 */
#define ADMIN_MSG_MAX_PAYLOAD 256

/**
 * @brief Message header size in bytes.
 */
#define ADMIN_MSG_HEADER_SIZE 4

/**
 * @brief Message header structure (wire format).
 *
 * All multi-byte fields are little-endian.
 */
typedef struct __attribute__((packed)) {
   uint8_t version;      /**< Protocol version (ADMIN_PROTOCOL_VERSION) */
   uint8_t msg_type;     /**< Message type (admin_msg_type_t) */
   uint16_t payload_len; /**< Payload length in bytes (max ADMIN_MSG_MAX_PAYLOAD) */
} admin_msg_header_t;

/**
 * @brief Response structure (wire format).
 *
 * Fixed 4-byte response for all message types.
 */
typedef struct __attribute__((packed)) {
   uint8_t version;       /**< Protocol version echo */
   uint8_t response_code; /**< Response code (admin_resp_code_t) */
   uint16_t reserved;     /**< Reserved for future use (set to 0) */
} admin_msg_response_t;

/*
 * =============================================================================
 * Setup Token Configuration (defined before payload structs that use them)
 * =============================================================================
 */

/**
 * @brief Setup token format: DAWN-XXXX-XXXX-XXXX-XXXX
 *
 * Total length including null terminator.
 */
#define SETUP_TOKEN_LENGTH 25

/**
 * @brief Number of random characters in setup token.
 */
#define SETUP_TOKEN_RANDOM_CHARS 16

/**
 * @brief Character set for setup token generation.
 *
 * Excludes ambiguous characters: I, O, 1, 0
 * 32 characters = 5 bits of entropy per character
 * 16 characters = 80 bits total entropy
 */
#define SETUP_TOKEN_CHARSET "ABCDEFGHJKLMNPQRSTUVWXYZ23456789"
#define SETUP_TOKEN_CHARSET_LEN 32

/**
 * @brief Setup token validity period in seconds.
 */
#define SETUP_TOKEN_VALIDITY_SEC (5 * 60)

/**
 * @brief Maximum failed token validation attempts before lockout.
 */
#define SETUP_TOKEN_MAX_ATTEMPTS 5

/**
 * @brief Rate limit state file path.
 *
 * Persisted to survive daemon restarts and prevent lockout bypass.
 */
#define SETUP_TOKEN_LOCKOUT_FILE "/run/dawn/token_lockout.state"

/**
 * @brief CREATE_USER payload structure (wire format).
 *
 * Combined token validation and user creation for atomicity.
 * Prevents race condition between token validation and user creation.
 *
 * Wire format:
 *   Bytes 0-23:  setup_token (24 bytes, DAWN-XXXX-XXXX-XXXX-XXXX format)
 *   Byte 24:     username_len (1-63)
 *   Byte 25:     password_len (8-128)
 *   Byte 26:     is_admin (0 or 1)
 *   Bytes 27+:   username (username_len bytes, no null)
 *   Following:   password (password_len bytes, no null)
 *
 * Total max: 24 + 1 + 1 + 1 + 63 + 128 = 218 bytes (within ADMIN_MSG_MAX_PAYLOAD)
 */
typedef struct __attribute__((packed)) {
   char setup_token[SETUP_TOKEN_LENGTH - 1]; /**< Setup token without null terminator */
   uint8_t username_len;                     /**< Username length (1-63) */
   uint8_t password_len;                     /**< Password length (8-128) */
   uint8_t is_admin;                         /**< 1 for admin, 0 for regular user */
   /* Followed by: username[username_len] + password[password_len] */
} admin_create_user_payload_t;

/**
 * @brief Minimum password length for user creation.
 */
#define ADMIN_PASSWORD_MIN_LEN 8

/**
 * @brief Maximum password length for user creation.
 */
#define ADMIN_PASSWORD_MAX_LEN 128

/**
 * @brief Maximum username length for user creation.
 */
#define ADMIN_USERNAME_MAX_LEN 63

/*
 * =============================================================================
 * Public API
 * =============================================================================
 */

/**
 * @brief Initialize the admin socket listener.
 *
 * Creates the Unix domain socket (abstract namespace on Linux), generates
 * a setup token, and starts the listener thread. The setup token is printed
 * to stderr (never logged to files) for the administrator to use with
 * dawn-admin.
 *
 * This function is safe to call even if initialization fails - it will
 * log a warning but not prevent daemon startup (graceful degradation).
 *
 * Thread safety: Call only once during daemon initialization.
 *
 * @return 0 on success, non-zero on failure.
 */
int admin_socket_init(void);

/**
 * @brief Shutdown the admin socket listener.
 *
 * Signals the listener thread to exit, waits for it to complete, closes
 * the socket, and cleans up resources. Uses the self-pipe trick for
 * reliable shutdown signaling.
 *
 * IMPORTANT: Must be called BEFORE accept_thread_stop() to ensure admin
 * connections are closed before network resources are torn down.
 *
 * Thread safety: Call only once during daemon shutdown.
 */
void admin_socket_shutdown(void);

/**
 * @brief Check if the admin socket is currently running.
 *
 * Useful for status reporting and debugging.
 *
 * @return true if listener thread is active, false otherwise.
 */
bool admin_socket_is_running(void);

/**
 * @brief Get the current setup token (for testing only).
 *
 * WARNING: This function exists for testing purposes only.
 * Do not use in production code - the token should only be
 * displayed to stderr during startup.
 *
 * @param buf    Buffer to receive the token.
 * @param buflen Size of buffer (must be >= SETUP_TOKEN_LENGTH).
 *
 * @return 0 on success, non-zero if token not available or buffer too small.
 */
int admin_socket_get_setup_token(char *buf, size_t buflen);

#ifdef __cplusplus
}
#endif

#endif /* DAWN_AUTH_ADMIN_SOCKET_H */
