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
 * Authentication database interface.
 * Provides SQLite-backed storage for users, sessions, and audit logs.
 *
 * Thread Safety: All functions acquire s_db_mutex internally.
 * The database is opened with SQLITE_OPEN_FULLMUTEX for additional safety.
 */

#ifndef AUTH_DB_H
#define AUTH_DB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "auth/auth_crypto.h"

/**
 * @brief Default database path
 */
#define AUTH_DB_DEFAULT_PATH "/var/lib/dawn/auth.db"

/**
 * @brief Maximum username length (including null terminator)
 */
#define AUTH_USERNAME_MAX 64

/**
 * @brief Maximum user agent length (truncated if longer)
 */
#define AUTH_USER_AGENT_MAX 128

/**
 * @brief Maximum IP address length (IPv6 with scope)
 */
#define AUTH_IP_MAX 64

/**
 * @brief Session timeout in seconds (24 hours)
 */
#define AUTH_SESSION_TIMEOUT_SEC (24 * 60 * 60)

/**
 * @brief Cleanup interval in seconds (5 minutes)
 *
 * Lazy cleanup runs during auth_db_get_session() if this much time has passed.
 */
#define AUTH_CLEANUP_INTERVAL_SEC 300

/**
 * @brief Database error codes
 */
#define AUTH_DB_SUCCESS 0
#define AUTH_DB_FAILURE 1
#define AUTH_DB_NOT_FOUND 2
#define AUTH_DB_DUPLICATE 3
#define AUTH_DB_INVALID 4
#define AUTH_DB_LOCKED 5

/**
 * @brief User record structure
 */
typedef struct {
   int id;
   char username[AUTH_USERNAME_MAX];
   char password_hash[AUTH_HASH_LEN];
   bool is_admin;
   time_t created_at;
   time_t last_login;
   int failed_attempts;
   time_t lockout_until;
} auth_user_t;

/**
 * @brief Session record structure
 *
 * Note: This is for authentication sessions, distinct from session_t
 * in session_manager.h which manages conversation/client sessions.
 */
typedef struct {
   char token[AUTH_TOKEN_LEN];
   int user_id;
   char username[AUTH_USERNAME_MAX];
   bool is_admin;
   time_t created_at;
   time_t last_activity;
   char ip_address[AUTH_IP_MAX];
   char user_agent[AUTH_USER_AGENT_MAX];
} auth_session_t;

/* ============================================================================
 * Lifecycle Functions
 * ============================================================================ */

/**
 * @brief Initialize the authentication database
 *
 * Opens or creates the SQLite database at the specified path.
 * Creates schema if needed and prepares all statements.
 * Sets secure file permissions (0600) on the database file.
 *
 * @param db_path Path to database file (NULL uses AUTH_DB_DEFAULT_PATH)
 * @return AUTH_DB_SUCCESS or AUTH_DB_FAILURE
 */
int auth_db_init(const char *db_path);

/**
 * @brief Shutdown the authentication database
 *
 * Checkpoints WAL, finalizes statements, and closes database.
 * Safe to call multiple times or if not initialized.
 */
void auth_db_shutdown(void);

/**
 * @brief Check if database is initialized
 *
 * @return true if initialized and ready, false otherwise
 */
bool auth_db_is_ready(void);

/* ============================================================================
 * User Operations
 * ============================================================================ */

/**
 * @brief Create a new user
 *
 * @param username Username (1-63 chars, alphanumeric + underscore)
 * @param password_hash Pre-computed hash from auth_hash_password()
 * @param is_admin Whether user has admin privileges
 * @return AUTH_DB_SUCCESS, AUTH_DB_DUPLICATE, AUTH_DB_INVALID, or AUTH_DB_FAILURE
 */
int auth_db_create_user(const char *username, const char *password_hash, bool is_admin);

/**
 * @brief Get user by username
 *
 * @param username Username to look up
 * @param user_out Buffer to receive user data (can be NULL to check existence)
 * @return AUTH_DB_SUCCESS, AUTH_DB_NOT_FOUND, or AUTH_DB_FAILURE
 */
int auth_db_get_user(const char *username, auth_user_t *user_out);

/**
 * @brief Get total user count
 *
 * Useful for checking if any users exist (first-run detection).
 *
 * @return Number of users, or -1 on error
 */
int auth_db_user_count(void);

/**
 * @brief Increment failed login attempts for user
 *
 * @param username Username
 * @return AUTH_DB_SUCCESS or AUTH_DB_FAILURE
 */
int auth_db_increment_failed_attempts(const char *username);

/**
 * @brief Reset failed login attempts for user
 *
 * Called after successful login.
 *
 * @param username Username
 * @return AUTH_DB_SUCCESS or AUTH_DB_FAILURE
 */
int auth_db_reset_failed_attempts(const char *username);

/**
 * @brief Update user's last login timestamp
 *
 * @param username Username
 * @return AUTH_DB_SUCCESS or AUTH_DB_FAILURE
 */
int auth_db_update_last_login(const char *username);

/**
 * @brief Set lockout time for user
 *
 * @param username Username
 * @param lockout_until Unix timestamp when lockout expires
 * @return AUTH_DB_SUCCESS or AUTH_DB_FAILURE
 */
int auth_db_set_lockout(const char *username, time_t lockout_until);

/* ============================================================================
 * Session Operations
 * ============================================================================ */

/**
 * @brief Create a new session
 *
 * @param user_id User ID from auth_user_t
 * @param token Pre-generated token from auth_generate_token()
 * @param ip_address Client IP address (can be NULL)
 * @param user_agent Client user agent (can be NULL, truncated to AUTH_USER_AGENT_MAX)
 * @return AUTH_DB_SUCCESS or AUTH_DB_FAILURE
 */
int auth_db_create_session(int user_id,
                           const char *token,
                           const char *ip_address,
                           const char *user_agent);

/**
 * @brief Get session by token
 *
 * Also performs lazy cleanup of expired sessions if cleanup interval has passed.
 *
 * @param token Session token
 * @param session_out Buffer to receive session data
 * @return AUTH_DB_SUCCESS, AUTH_DB_NOT_FOUND, or AUTH_DB_FAILURE
 */
int auth_db_get_session(const char *token, auth_session_t *session_out);

/**
 * @brief Update session last activity timestamp
 *
 * @param token Session token
 * @return AUTH_DB_SUCCESS or AUTH_DB_FAILURE
 */
int auth_db_update_session_activity(const char *token);

/**
 * @brief Delete a session (logout)
 *
 * @param token Session token
 * @return AUTH_DB_SUCCESS or AUTH_DB_FAILURE
 */
int auth_db_delete_session(const char *token);

/**
 * @brief Delete all sessions for a user
 *
 * Used for password change or account lockout.
 *
 * @param user_id User ID
 * @return Number of sessions deleted, or -1 on error
 */
int auth_db_delete_user_sessions(int user_id);

/* ============================================================================
 * Rate Limiting
 * ============================================================================ */

/**
 * @brief Count recent failed login attempts from IP
 *
 * @param ip_address Client IP address
 * @param since Only count attempts after this timestamp
 * @return Number of failed attempts, or -1 on error
 */
int auth_db_count_recent_failures(const char *ip_address, time_t since);

/**
 * @brief Log a login attempt
 *
 * @param ip_address Client IP address
 * @param username Username attempted (can be NULL)
 * @param success Whether login succeeded
 * @return AUTH_DB_SUCCESS or AUTH_DB_FAILURE
 */
int auth_db_log_attempt(const char *ip_address, const char *username, bool success);

/* ============================================================================
 * Audit Logging
 * ============================================================================ */

/**
 * @brief Log an authentication event
 *
 * @param event Event type (e.g., "USER_CREATED", "LOGIN_SUCCESS")
 * @param username Associated username (can be NULL)
 * @param ip_address Client IP address (can be NULL)
 * @param details Additional details as JSON or text (can be NULL)
 */
void auth_db_log_event(const char *event,
                       const char *username,
                       const char *ip_address,
                       const char *details);

/* ============================================================================
 * Maintenance
 * ============================================================================ */

/**
 * @brief Run cleanup of expired data
 *
 * Deletes expired sessions, old login attempts, and old audit logs.
 * Normally called lazily during auth_db_get_session(), but can be
 * called manually if needed.
 *
 * @return AUTH_DB_SUCCESS or AUTH_DB_FAILURE
 */
int auth_db_run_cleanup(void);

/**
 * @brief Checkpoint WAL to main database
 *
 * Useful before backup or to reclaim disk space.
 *
 * @return AUTH_DB_SUCCESS or AUTH_DB_FAILURE
 */
int auth_db_checkpoint(void);

#endif /* AUTH_DB_H */
