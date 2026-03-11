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
 * WebUI Home Assistant Handler - WebSocket handlers for HA admin panel
 */

#include <json-c/json.h>
#include <string.h>

#include "logging.h"
#include "tools/homeassistant_service.h"
#include "webui/webui_internal.h"

/* =============================================================================
 * Helper: serialize entity list into a JSON payload
 * ============================================================================= */
static void serialize_entity_list(struct json_object *payload, const ha_entity_list_t *entities) {
   json_object_object_add(payload, "success", json_object_new_boolean(1));
   json_object_object_add(payload, "count", json_object_new_int(entities->count));
   json_object_object_add(payload, "cached_at", json_object_new_int64(entities->cached_at));

   struct json_object *arr = json_object_new_array();
   for (int i = 0; i < entities->count; i++) {
      const ha_entity_t *ent = &entities->entities[i];
      struct json_object *obj = json_object_new_object();
      json_object_object_add(obj, "entity_id", json_object_new_string(ent->entity_id));
      json_object_object_add(obj, "friendly_name", json_object_new_string(ent->friendly_name));
      json_object_object_add(obj, "domain", json_object_new_string(ent->domain_str));
      json_object_object_add(obj, "state", json_object_new_string(ent->state));
      if (ent->area_name[0]) {
         json_object_object_add(obj, "area", json_object_new_string(ent->area_name));
      }
      json_object_array_add(arr, obj);
   }
   json_object_object_add(payload, "entities", arr);
}

/* =============================================================================
 * Handler: ha_status — returns configured, connected, entity_count, version
 * ============================================================================= */
void handle_ha_status(ws_connection_t *conn) {
   if (!conn_require_admin(conn))
      return;

   struct json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("ha_status_response"));
   struct json_object *payload = json_object_new_object();

   json_object_object_add(payload, "configured",
                          json_object_new_boolean(homeassistant_is_configured()));
   json_object_object_add(payload, "connected",
                          json_object_new_boolean(homeassistant_is_connected()));

   ha_status_t status;
   if (homeassistant_get_status(&status) == HA_OK) {
      json_object_object_add(payload, "entity_count", json_object_new_int(status.entity_count));
      json_object_object_add(payload, "version", json_object_new_string(status.version));
      json_object_object_add(payload, "url", json_object_new_string(status.url));
   }

   json_object_object_add(response, "payload", payload);
   send_json_response(conn, response);
   json_object_put(response);
}

/* =============================================================================
 * Handler: ha_test_connection — test HA connection
 * ============================================================================= */
void handle_ha_test_connection(ws_connection_t *conn) {
   if (!conn_require_admin(conn))
      return;

   struct json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("ha_test_connection_response"));
   struct json_object *payload = json_object_new_object();

   ha_error_t err = homeassistant_test_connection();
   json_object_object_add(payload, "success", json_object_new_boolean(err == HA_OK));
   if (err != HA_OK) {
      json_object_object_add(payload, "error",
                             json_object_new_string(homeassistant_error_str(err)));
   } else {
      ha_status_t status;
      homeassistant_get_status(&status);
      json_object_object_add(payload, "version", json_object_new_string(status.version));
   }

   json_object_object_add(response, "payload", payload);
   send_json_response(conn, response);
   json_object_put(response);
}

/* =============================================================================
 * Handler: ha_list_entities — serializes cached entity list
 * ============================================================================= */
void handle_ha_list_entities(ws_connection_t *conn) {
   if (!conn_require_admin(conn))
      return;

   struct json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("ha_entities_response"));
   struct json_object *payload = json_object_new_object();

   if (!homeassistant_is_connected()) {
      json_object_object_add(payload, "success", json_object_new_boolean(0));
      json_object_object_add(payload, "error", json_object_new_string("Not connected"));
   } else {
      const ha_entity_list_t *entities;
      ha_error_t err = homeassistant_list_entities(&entities);
      if (err == HA_OK) {
         serialize_entity_list(payload, entities);
      } else {
         json_object_object_add(payload, "success", json_object_new_boolean(0));
         json_object_object_add(payload, "error",
                                json_object_new_string(homeassistant_error_str(err)));
      }
   }

   json_object_object_add(response, "payload", payload);
   send_json_response(conn, response);
   json_object_put(response);
}

/* =============================================================================
 * Handler: ha_refresh_entities — force cache refresh
 * ============================================================================= */
void handle_ha_refresh_entities(ws_connection_t *conn) {
   if (!conn_require_admin(conn))
      return;

   struct json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("ha_entities_response"));
   struct json_object *payload = json_object_new_object();

   if (!homeassistant_is_connected()) {
      json_object_object_add(payload, "success", json_object_new_boolean(0));
      json_object_object_add(payload, "error", json_object_new_string("Not connected"));
   } else {
      const ha_entity_list_t *entities;
      ha_error_t err = homeassistant_refresh_entities(&entities);
      if (err == HA_OK) {
         serialize_entity_list(payload, entities);
      } else {
         json_object_object_add(payload, "success", json_object_new_boolean(0));
         json_object_object_add(payload, "error",
                                json_object_new_string(homeassistant_error_str(err)));
      }
   }

   json_object_object_add(response, "payload", payload);
   send_json_response(conn, response);
   json_object_put(response);
}
