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
 * OAuth 2.0 client — PKCE S256, token exchange, encrypted storage, auto-refresh.
 * Shared module for Google Calendar, Gmail, and future OAuth providers.
 */

#ifndef OAUTH_CLIENT_H
#define OAUTH_CLIENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ============================================================================
 * Provider Configuration
 * ============================================================================ */

typedef struct {
   char provider[32];         /**< "google" */
   char client_id[128];       /**< OAuth client ID */
   char client_secret[256];   /**< OAuth client secret */
   char auth_endpoint[256];   /**< Authorization endpoint URL */
   char token_endpoint[256];  /**< Token exchange endpoint URL */
   char revoke_endpoint[256]; /**< Token revocation endpoint URL */
   char redirect_uri[256];    /**< Redirect URI registered with provider */
   char scopes[512];          /**< Space-separated scopes */
} oauth_provider_config_t;

/* ============================================================================
 * Token Set
 * ============================================================================ */

typedef struct {
   char access_token[2048]; /**< Bearer access token */
   char refresh_token[512]; /**< Refresh token for obtaining new access tokens */
   int64_t expires_at;      /**< Expiry time (epoch seconds) */
   char scopes[512];        /**< Granted scopes */
   char email[256];         /**< User email from token response (if available) */
} oauth_token_set_t;

/* ============================================================================
 * Google Scopes
 * ============================================================================ */

/** Google Calendar + userinfo scopes (used by CalDAV and WebUI OAuth handlers) */
#define GOOGLE_CALENDAR_SCOPE \
   "https://www.googleapis.com/auth/calendar https://www.googleapis.com/auth/userinfo.email"

/* ============================================================================
 * Provider Setup
 * ============================================================================ */

/**
 * Fill provider config with Google OAuth 2.0 endpoints.
 * @return 0 on success, 1 on failure (missing client_id/secret)
 */
int oauth_google_provider(const char *client_id,
                          const char *client_secret,
                          const char *redirect_uri,
                          const char *scopes,
                          oauth_provider_config_t *out);

/**
 * Build Google provider from global config (secrets.toml).
 * Uses google_redirect_url if set, otherwise constructs from webui port.
 *
 * @param scopes  OAuth scopes (e.g., calendar, gmail)
 * @param out     Output provider config
 * @return 0 on success, 1 if not configured
 */
int oauth_build_google_provider(const char *scopes, oauth_provider_config_t *out);

/* ============================================================================
 * OAuth Flow
 * ============================================================================ */

/**
 * Generate authorization URL with PKCE S256 + CSRF state token.
 * Opens a pending state entry (max 2 per user, 5-minute expiry).
 *
 * @param provider   Provider configuration
 * @param user_id    User ID (for state binding)
 * @param url_buf    Output buffer for authorization URL
 * @param url_len    Size of url_buf
 * @param state_buf  Output buffer for state token
 * @param state_len  Size of state_buf
 * @return 0 on success
 */
int oauth_get_auth_url(const oauth_provider_config_t *provider,
                       int user_id,
                       char *url_buf,
                       size_t url_len,
                       char *state_buf,
                       size_t state_len);

/**
 * Exchange authorization code for tokens. Validates state + PKCE verifier.
 *
 * @param provider    Provider configuration
 * @param code        Authorization code from callback
 * @param state       State token from callback
 * @param user_id     User ID (must match pending state)
 * @param out_tokens  Output token set
 * @return 0 on success
 */
int oauth_exchange_code(const oauth_provider_config_t *provider,
                        const char *code,
                        const char *state,
                        int user_id,
                        oauth_token_set_t *out_tokens);

/**
 * Refresh an expired access token using the refresh token.
 *
 * @param provider  Provider configuration
 * @param tokens    Token set (access_token and expires_at updated in place)
 * @return 0 on success
 */
int oauth_refresh(const oauth_provider_config_t *provider, oauth_token_set_t *tokens);

/**
 * Get a valid access token, auto-refreshing if within 300s of expiry.
 * Thread-safe with per-account locking to avoid thundering herd on refresh.
 *
 * @param provider     Provider configuration
 * @param user_id      User ID
 * @param account_key  Account identifier (e.g., email address)
 * @param token_buf    Output buffer for access token
 * @param token_len    Size of token_buf
 * @return 0 on success
 */
int oauth_get_access_token(const oauth_provider_config_t *provider,
                           int user_id,
                           const char *account_key,
                           char *token_buf,
                           size_t token_len);

/* ============================================================================
 * Encrypted DB Persistence (uses crypto_store)
 * ============================================================================ */

/**
 * Store tokens encrypted in the database.
 */
int oauth_store_tokens(int user_id,
                       const char *provider,
                       const char *account_key,
                       const oauth_token_set_t *tokens);

/**
 * Load and decrypt tokens from the database.
 */
int oauth_load_tokens(int user_id,
                      const char *provider,
                      const char *account_key,
                      oauth_token_set_t *out);

/**
 * Delete tokens from the database.
 */
int oauth_delete_tokens(int user_id, const char *provider, const char *account_key);

/**
 * Check if tokens exist for an account.
 */
bool oauth_has_tokens(int user_id, const char *provider, const char *account_key);

/**
 * Revoke tokens at the provider and delete locally.
 * Always deletes locally regardless of revocation success.
 */
int oauth_revoke_and_delete(const oauth_provider_config_t *provider,
                            int user_id,
                            const char *account_key);

#endif /* OAUTH_CLIENT_H */
