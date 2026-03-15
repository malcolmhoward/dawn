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
 */

#ifndef WEBUI_OAUTH_H
#define WEBUI_OAUTH_H

#include "webui/webui_internal.h"

/** Generate OAuth authorization URL (scopes from client payload) */
void handle_oauth_get_auth_url(ws_connection_t *conn, json_object *payload);

/** Exchange OAuth authorization code for tokens */
void handle_oauth_exchange_code(ws_connection_t *conn, json_object *payload);

/** Disconnect (revoke + delete) OAuth tokens */
void handle_oauth_disconnect(ws_connection_t *conn, json_object *payload);

/** Check if existing tokens cover the requested scopes */
void handle_oauth_check_scopes(ws_connection_t *conn, json_object *payload);

#endif /* WEBUI_OAUTH_H */
