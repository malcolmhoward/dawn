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
 * @brief Maximum failed login attempts before account lockout
 */
#define AUTH_MAX_LOGIN_ATTEMPTS 5

/**
 * @brief Account lockout duration in seconds (15 minutes)
 */
#define AUTH_LOCKOUT_DURATION_SEC (15 * 60)

/**
 * @brief Database error codes
 */
#define AUTH_DB_SUCCESS 0
#define AUTH_DB_FAILURE 1
#define AUTH_DB_NOT_FOUND 2
#define AUTH_DB_DUPLICATE 3
#define AUTH_DB_INVALID 4
#define AUTH_DB_LOCKED 5
#define AUTH_DB_LAST_ADMIN 6 /**< Cannot delete/demote last admin */

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

/**
 * @brief User summary structure (excludes password hash for security)
 *
 * Used for user enumeration - never exposes password_hash.
 */
typedef struct {
   int id;
   char username[AUTH_USERNAME_MAX];
   bool is_admin;
   time_t created_at;
   time_t last_login;
   int failed_attempts;
   time_t lockout_until;
} auth_user_summary_t;

/**
 * @brief Callback for user enumeration
 *
 * @param user User summary data
 * @param ctx User-provided context
 * @return 0 to continue iteration, non-zero to stop
 */
typedef int (*auth_user_summary_callback_t)(const auth_user_summary_t *user, void *ctx);

/**
 * @brief Session summary structure (excludes full token for security)
 *
 * Used for session enumeration - only exposes token prefix.
 */
typedef struct {
   char token_prefix[9]; /**< First 8 chars of token + null */
   int user_id;
   char username[AUTH_USERNAME_MAX];
   time_t created_at;
   time_t last_activity;
   char ip_address[AUTH_IP_MAX];
} auth_session_summary_t;

/**
 * @brief Callback for session enumeration
 *
 * @param session Session summary data
 * @param ctx User-provided context
 * @return 0 to continue iteration, non-zero to stop
 */
typedef int (*auth_session_summary_callback_t)(const auth_session_summary_t *session, void *ctx);

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
 * @brief Validate username format
 *
 * Valid usernames: 1-63 chars, alphanumeric plus underscore, hyphen, period.
 * Must start with a letter or underscore.
 *
 * @param username Username to validate
 * @return AUTH_DB_SUCCESS if valid, AUTH_DB_INVALID if not
 */
int auth_db_validate_username(const char *username);

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

/**
 * @brief List all users (callback-based, excludes password hashes)
 *
 * @param callback Function called for each user
 * @param ctx User-provided context passed to callback
 * @return AUTH_DB_SUCCESS or AUTH_DB_FAILURE
 */
int auth_db_list_users(auth_user_summary_callback_t callback, void *ctx);

/**
 * @brief Count admin users
 *
 * @return Number of admin users, or -1 on error
 */
int auth_db_count_admins(void);

/**
 * @brief Delete a user account
 *
 * Fails with AUTH_DB_LAST_ADMIN if this is the only admin user.
 * Deletes all sessions for the user as part of the operation.
 *
 * @param username Username to delete
 * @return AUTH_DB_SUCCESS, AUTH_DB_NOT_FOUND, AUTH_DB_LAST_ADMIN, or AUTH_DB_FAILURE
 */
int auth_db_delete_user(const char *username);

/**
 * @brief Update user password (atomically invalidates all sessions)
 *
 * @param username Username
 * @param new_hash New password hash from auth_hash_password()
 * @return AUTH_DB_SUCCESS, AUTH_DB_NOT_FOUND, or AUTH_DB_FAILURE
 */
int auth_db_update_password(const char *username, const char *new_hash);

/**
 * @brief Unlock a user account
 *
 * Wrapper for auth_db_set_lockout(username, 0).
 *
 * @param username Username
 * @return AUTH_DB_SUCCESS, AUTH_DB_NOT_FOUND, or AUTH_DB_FAILURE
 */
int auth_db_unlock_user(const char *username);

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
 * @brief Delete a session by token prefix
 *
 * Finds and deletes the session matching the 8-character token prefix.
 * Returns AUTH_DB_NOT_FOUND if no matching session exists.
 *
 * @param prefix 8-character token prefix
 * @return AUTH_DB_SUCCESS, AUTH_DB_NOT_FOUND, or AUTH_DB_FAILURE
 */
int auth_db_delete_session_by_prefix(const char *prefix);

/**
 * @brief Delete all sessions for a user by username
 *
 * Wrapper around auth_db_delete_user_sessions that looks up user by name.
 *
 * @param username Username
 * @return Number of sessions deleted, or -1 on error
 */
int auth_db_delete_sessions_by_username(const char *username);

/**
 * @brief Delete all sessions for a user
 *
 * Used for password change or account lockout.
 *
 * @param user_id User ID
 * @return Number of sessions deleted, or -1 on error
 */
int auth_db_delete_user_sessions(int user_id);

/**
 * @brief List all active sessions (callback-based, token prefix only)
 *
 * @param callback Function called for each session
 * @param ctx User-provided context passed to callback
 * @return AUTH_DB_SUCCESS or AUTH_DB_FAILURE
 */
int auth_db_list_sessions(auth_session_summary_callback_t callback, void *ctx);

/**
 * @brief Count active sessions
 *
 * @return Number of active sessions, or -1 on error
 */
int auth_db_count_sessions(void);

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

/**
 * @brief Clear login attempts for an IP address
 *
 * Used to unblock an IP that has been rate-limited.
 * If ip_address is NULL, clears all login attempts.
 *
 * @param ip_address Client IP address to unblock (NULL for all)
 * @return Number of deleted entries, or -1 on error
 */
int auth_db_clear_login_attempts(const char *ip_address);

/**
 * @brief IP rate limit status entry
 */
typedef struct {
   char ip_address[AUTH_IP_MAX];
   int failed_attempts;
   time_t last_attempt;
} auth_ip_status_t;

/**
 * @brief Callback for IP status enumeration
 *
 * @param status IP status entry
 * @param ctx User-provided context
 * @return 0 to continue iteration, non-zero to stop
 */
typedef int (*auth_ip_status_callback_t)(const auth_ip_status_t *status, void *ctx);

/**
 * @brief List IPs with recent failed login attempts
 *
 * Returns IPs that have failed attempts within the rate limit window.
 *
 * @param since Only include attempts after this timestamp
 * @param callback Function called for each IP
 * @param ctx User-provided context
 * @return AUTH_DB_SUCCESS or AUTH_DB_FAILURE
 */
int auth_db_list_blocked_ips(time_t since, auth_ip_status_callback_t callback, void *ctx);

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

/**
 * @brief Audit log query filter.
 */
typedef struct {
   time_t since;         /**< Only entries after this time (0 = no limit) */
   time_t until;         /**< Only entries before this time (0 = no limit) */
   const char *event;    /**< Filter by event type (NULL = all) */
   const char *username; /**< Filter by username (NULL = all) */
   int limit;            /**< Max entries to return (0 = default 100) */
   int offset;           /**< Skip first N entries (for pagination) */
} auth_log_filter_t;

/**
 * @brief Audit log entry.
 */
typedef struct {
   time_t timestamp;
   char event[32];
   char username[AUTH_USERNAME_MAX];
   char ip_address[AUTH_IP_MAX];
   char details[256];
} auth_log_entry_t;

/**
 * @brief Callback for audit log enumeration.
 */
typedef int (*auth_log_callback_t)(const auth_log_entry_t *entry, void *ctx);

/**
 * @brief Query audit log with optional filters.
 *
 * @param filter Query filters (can be NULL for defaults)
 * @param callback Function called for each matching entry
 * @param ctx User-provided context passed to callback
 * @return AUTH_DB_SUCCESS or AUTH_DB_FAILURE
 */
int auth_db_query_audit_log(const auth_log_filter_t *filter,
                            auth_log_callback_t callback,
                            void *ctx);

/**
 * @brief Default limit for audit log queries.
 */
#define AUTH_LOG_DEFAULT_LIMIT 100

/**
 * @brief Maximum limit for audit log queries.
 */
#define AUTH_LOG_MAX_LIMIT 1000

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

/**
 * @brief Passive WAL checkpoint (non-blocking).
 *
 * Checkpoints as much of the WAL as possible without waiting.
 * Suitable for background maintenance as it won't block other operations.
 *
 * @return AUTH_DB_SUCCESS or AUTH_DB_FAILURE
 */
int auth_db_checkpoint_passive(void);

/* ============================================================================
 * Statistics and Database Management
 * ============================================================================ */

/**
 * @brief Database statistics structure.
 */
typedef struct {
   int user_count;          /**< Total number of users */
   int admin_count;         /**< Number of admin users */
   int session_count;       /**< Number of active sessions */
   int locked_user_count;   /**< Number of locked accounts */
   int failed_attempts_24h; /**< Failed login attempts in last 24 hours */
   int audit_log_count;     /**< Total audit log entries */
   int64_t db_size_bytes;   /**< Database file size in bytes */
} auth_db_stats_t;

/**
 * @brief Get database statistics.
 *
 * @param stats Pointer to stats structure to populate.
 * @return AUTH_DB_SUCCESS or AUTH_DB_FAILURE
 */
int auth_db_get_stats(auth_db_stats_t *stats);

/**
 * @brief Vacuum (compact) the database.
 *
 * Rate-limited to once per 24 hours to prevent excessive I/O.
 *
 * @return AUTH_DB_SUCCESS, AUTH_DB_RATE_LIMITED, or AUTH_DB_FAILURE
 */
int auth_db_vacuum(void);

/**
 * @brief Backup the database to a file.
 *
 * Creates a backup with secure permissions (0600).
 * Uses SQLite stepped backup for minimal lock time.
 *
 * @param dest_path Destination file path (must not exist)
 * @return AUTH_DB_SUCCESS or AUTH_DB_FAILURE
 */
int auth_db_backup(const char *dest_path);

/**
 * @brief Error code for rate-limited operations.
 */
#define AUTH_DB_RATE_LIMITED 7

#endif /* AUTH_DB_H */
