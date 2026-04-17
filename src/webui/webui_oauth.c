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
 * WebUI OAuth Handlers — shared Google OAuth for Calendar, Email, etc.
 * Extracted from webui_calendar.c to support unified OAuth across services.
 */

#include "webui/webui_oauth.h"

#include <json-c/json.h>
#include <sodium.h>
#include <string.h>

#include "logging.h"
#include "tools/oauth_client.h"
#include "webui/webui_internal.h"

/* =============================================================================
 * Known Google scope prefixes for validation
 * ============================================================================= */

static bool validate_google_scopes(const char *scopes) {
   if (!scopes || !scopes[0])
      return false;

   /* Must contain at least one known Google scope prefix */
   static const char *known_prefixes[] = {
      "https://www.googleapis.com/auth/",
      "https://mail.google.com/",
      NULL,
   };

   char buf[1024];
   snprintf(buf, sizeof(buf), "%s", scopes);

   char *saveptr = NULL;
   char *token = strtok_r(buf, " ", &saveptr);
   while (token) {
      bool valid = false;
      for (const char **p = known_prefixes; *p; p++) {
         if (strncmp(token, *p, strlen(*p)) == 0) {
            valid = true;
            break;
         }
      }
      if (!valid) {
         OLOG_WARNING("oauth: rejected unknown scope: %s", token);
         return false;
      }
      token = strtok_r(NULL, " ", &saveptr);
   }
   return true;
}

/** Ensure userinfo.email is included in the scope string */
static void ensure_userinfo_email(const char *input, char *out, size_t out_len) {
   const char *email_scope = "https://www.googleapis.com/auth/userinfo.email";
   if (strstr(input, email_scope)) {
      snprintf(out, out_len, "%s", input);
   } else {
      snprintf(out, out_len, "%s %s", input, email_scope);
   }
}

/* =============================================================================
 * Get Auth URL
 * ============================================================================= */

void handle_oauth_get_auth_url(ws_connection_t *conn, json_object *payload) {
   if (!conn_require_auth(conn))
      return;

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("oauth_get_auth_url_response"));
   json_object *resp_payload = json_object_new_object();

   /* Read scopes from client payload */
   json_object *scopes_obj = NULL;
   const char *client_scopes = NULL;
   if (payload)
      json_object_object_get_ex(payload, "scopes", &scopes_obj);
   if (scopes_obj)
      client_scopes = json_object_get_string(scopes_obj);

   /* Require explicit scopes — shared module doesn't assume a default service */
   if (!client_scopes || !client_scopes[0]) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error", json_object_new_string("Missing OAuth scopes"));
      goto done;
   }

   /* Validate scopes */
   if (!validate_google_scopes(client_scopes)) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error", json_object_new_string("Invalid OAuth scopes"));
      goto done;
   }

   /* Ensure userinfo.email is always included */
   char scopes_with_email[1024];
   ensure_userinfo_email(client_scopes, scopes_with_email, sizeof(scopes_with_email));

   /* Merge with existing granted scopes to prevent scope loss when a second
    * service (e.g. email) re-authorizes an account already used by another
    * service (e.g. calendar). Without this, the new token overwrites the old
    * one with only the new service's scopes. */
   char account_keys[8][256];
   int acct_count = oauth_list_accounts(conn->auth_user_id, "google", account_keys, 8);
   for (int i = 0; i < acct_count; i++) {
      oauth_token_set_t existing;
      if (oauth_load_tokens(conn->auth_user_id, "google", account_keys[i], &existing) == 0) {
         if (existing.scopes[0]) {
            char merged[1024];
            if (oauth_merge_scopes(scopes_with_email, existing.scopes, merged, sizeof(merged)) ==
                0) {
               snprintf(scopes_with_email, sizeof(scopes_with_email), "%s", merged);
            }
         }
         sodium_memzero(&existing, sizeof(existing));
      }
   }

   oauth_provider_config_t google;
   if (oauth_build_google_provider(scopes_with_email, &google) != 0) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Google OAuth not configured"));
   } else {
      char url[2048];
      char state[128];
      if (oauth_get_auth_url(&google, conn->auth_user_id, url, sizeof(url), state, sizeof(state)) !=
          0) {
         json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
         json_object_object_add(resp_payload, "error",
                                json_object_new_string("Failed to generate auth URL"));
      } else {
         json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
         json_object_object_add(resp_payload, "url", json_object_new_string(url));
      }
   }
   sodium_memzero(&google, sizeof(google));

done:
   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn, response);
   json_object_put(response);
}

/* =============================================================================
 * Exchange Code
 * ============================================================================= */

void handle_oauth_exchange_code(ws_connection_t *conn, json_object *payload) {
   if (!conn_require_auth(conn))
      return;

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("oauth_exchange_code_response"));
   json_object *resp_payload = json_object_new_object();

   json_object *code_obj, *state_obj;
   if (!json_object_object_get_ex(payload, "code", &code_obj) ||
       !json_object_object_get_ex(payload, "state", &state_obj)) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Missing code or state"));
   } else {
      /* Scope field is unused in token exchange — only endpoints/credentials matter */
      oauth_provider_config_t google;
      if (oauth_build_google_provider("" /* unused for exchange */, &google) != 0) {
         json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
         json_object_object_add(resp_payload, "error",
                                json_object_new_string("Google OAuth not configured"));
      } else {
         oauth_token_set_t tokens;
         const char *code = json_object_get_string(code_obj);
         const char *state = json_object_get_string(state_obj);

         if (oauth_exchange_code(&google, code, state, conn->auth_user_id, &tokens) != 0) {
            json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
            json_object_object_add(resp_payload, "error",
                                   json_object_new_string("Token exchange failed"));
         } else {
            /* Use email from token exchange, fall back to generic key */
            char account_key[256];
            if (tokens.email[0]) {
               snprintf(account_key, sizeof(account_key), "%s", tokens.email);
            } else {
               snprintf(account_key, sizeof(account_key), "google_%d", conn->auth_user_id);
               OLOG_WARNING("oauth: no email in token response, using fallback key '%s'",
                            account_key);
            }

            /* Store tokens — account creation is separate (frontend does it) */
            if (oauth_store_tokens(conn->auth_user_id, "google", account_key, &tokens) != 0) {
               json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
               json_object_object_add(resp_payload, "error",
                                      json_object_new_string("Failed to store tokens"));
            } else {
               json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
               json_object_object_add(resp_payload, "account_key",
                                      json_object_new_string(account_key));
               json_object_object_add(resp_payload, "scopes",
                                      json_object_new_string(tokens.scopes));
            }
            sodium_memzero(&tokens, sizeof(tokens));
         }
      }
      sodium_memzero(&google, sizeof(google));
   }

   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn, response);
   json_object_put(response);
}

/* =============================================================================
 * Disconnect
 * ============================================================================= */

void handle_oauth_disconnect(ws_connection_t *conn, json_object *payload) {
   if (!conn_require_auth(conn))
      return;

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("oauth_disconnect_response"));
   json_object *resp_payload = json_object_new_object();

   json_object *key_obj;
   if (!json_object_object_get_ex(payload, "account_key", &key_obj)) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error", json_object_new_string("Missing account_key"));
   } else {
      /* Scope field is unused for revocation — only endpoints/credentials matter */
      oauth_provider_config_t google;
      if (oauth_build_google_provider("" /* unused for revoke */, &google) != 0) {
         json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
         json_object_object_add(resp_payload, "error",
                                json_object_new_string("Google OAuth not configured"));
      } else {
         const char *account_key = json_object_get_string(key_obj);
         int rc = oauth_revoke_and_delete(&google, conn->auth_user_id, account_key);
         if (rc != 0) {
            OLOG_WARNING("OAuth revocation failed for user %d key %s (tokens deleted locally)",
                         conn->auth_user_id, account_key);
         }
         json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
      }
      sodium_memzero(&google, sizeof(google));
   }

   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn, response);
   json_object_put(response);
}

/* =============================================================================
 * Check Scopes
 * ============================================================================= */

void handle_oauth_check_scopes(ws_connection_t *conn, json_object *payload) {
   if (!conn_require_auth(conn))
      return;

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("oauth_check_scopes_response"));
   json_object *resp_payload = json_object_new_object();

   json_object *scopes_obj;
   if (!payload || !json_object_object_get_ex(payload, "scopes", &scopes_obj)) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error", json_object_new_string("Missing scopes"));
      goto done;
   }

   const char *required_scopes = json_object_get_string(scopes_obj);

   /* List all OAuth accounts for this user */
   char account_keys[8][256];
   int count = oauth_list_accounts(conn->auth_user_id, "google", account_keys, 8);

   bool found = false;
   for (int i = 0; i < count; i++) {
      oauth_token_set_t tokens;
      if (oauth_load_tokens(conn->auth_user_id, "google", account_keys[i], &tokens) == 0) {
         if (oauth_has_scopes(tokens.scopes, required_scopes)) {
            json_object_object_add(resp_payload, "has_scopes", json_object_new_boolean(1));
            json_object_object_add(resp_payload, "account_key",
                                   json_object_new_string(account_keys[i]));
            sodium_memzero(&tokens, sizeof(tokens));
            found = true;
            break;
         }
         sodium_memzero(&tokens, sizeof(tokens));
      }
   }

   if (!found) {
      json_object_object_add(resp_payload, "has_scopes", json_object_new_boolean(0));
      /* Include account_key if any account exists (for scope upgrade flow) */
      if (count > 0) {
         json_object_object_add(resp_payload, "account_key",
                                json_object_new_string(account_keys[0]));
      }
   }
   json_object_object_add(resp_payload, "success", json_object_new_boolean(1));

done:
   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn, response);
   json_object_put(response);
}
