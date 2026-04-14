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
 * Phone LLM tool — voice-controlled phone calls and SMS.
 * Actions: call, confirm_call, answer, hang_up, send_sms, confirm_sms,
 *          read_sms, call_log, sms_log, status
 */

#include "tools/phone_tool.h"

#include <json-c/json.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "logging.h"
#include "tools/phone_db.h"
#include "tools/phone_service.h"
#include "tools/toml.h"
#include "tools/tool_registry.h"

/* =============================================================================
 * Constants
 * ============================================================================= */

#define RESULT_BUF_SIZE 8192

/* =============================================================================
 * Config
 * ============================================================================= */

typedef struct {
   bool enabled;
   bool confirm_outbound;
} phone_tool_config_t;

static phone_tool_config_t s_config = {
   .enabled = true,
   .confirm_outbound = true,
};

/* Pending confirmation state (per-session would be ideal, but single user for now) */
static char s_pending_call_number[24] = "";
static char s_pending_sms_number[24] = "";
static char s_pending_sms_body[1024] = "";

/* =============================================================================
 * JSON Helpers
 * ============================================================================= */

static const char *json_get_str(struct json_object *obj, const char *key) {
   struct json_object *val = NULL;
   if (!json_object_object_get_ex(obj, key, &val))
      return NULL;
   return json_object_get_string(val);
}

static int json_get_int(struct json_object *obj, const char *key, int def) {
   struct json_object *val = NULL;
   if (!json_object_object_get_ex(obj, key, &val))
      return def;
   return json_object_get_int(val);
}

/* =============================================================================
 * Action Handlers
 * ============================================================================= */

static char *handle_call(struct json_object *details, int user_id) {
   const char *target = json_get_str(details, "target");
   if (!target || target[0] == '\0') {
      return strdup("Error: 'target' is required (phone number or contact name)");
   }

   if (s_config.confirm_outbound) {
      /* Store target for confirmation — service layer resolves on confirm */
      snprintf(s_pending_call_number, sizeof(s_pending_call_number), "%s", target);
      char buf[256];
      snprintf(buf, sizeof(buf), "About to call %s. Say 'confirm' to proceed.", target);
      return strdup(buf);
   }

   /* No confirmation — dial immediately */
   char result[RESULT_BUF_SIZE];
   phone_service_call(user_id, target, result, sizeof(result));
   return strdup(result);
}

static char *handle_confirm_call(int user_id) {
   if (s_pending_call_number[0] == '\0') {
      return strdup("Error: no pending call to confirm");
   }

   char result[RESULT_BUF_SIZE];
   phone_service_call(user_id, s_pending_call_number, result, sizeof(result));

   s_pending_call_number[0] = '\0';
   return strdup(result);
}

static char *handle_answer(int user_id) {
   char result[RESULT_BUF_SIZE];
   phone_service_answer(user_id, result, sizeof(result));
   return strdup(result);
}

static char *handle_hang_up(int user_id) {
   char result[RESULT_BUF_SIZE];
   phone_service_hangup(user_id, result, sizeof(result));
   return strdup(result);
}

static char *handle_send_sms(struct json_object *details, int user_id) {
   const char *target = json_get_str(details, "target");
   const char *body = json_get_str(details, "body");

   if (!target || target[0] == '\0') {
      return strdup("Error: 'target' is required (phone number or contact name)");
   }
   if (!body || body[0] == '\0') {
      return strdup("Error: 'body' is required (message text)");
   }

   if (s_config.confirm_outbound) {
      /* Store target and body for confirmation — service layer resolves on confirm */
      snprintf(s_pending_sms_number, sizeof(s_pending_sms_number), "%s", target);
      snprintf(s_pending_sms_body, sizeof(s_pending_sms_body), "%s", body);

      char buf[512];
      snprintf(buf, sizeof(buf), "About to send SMS to %s: \"%s\". Say 'confirm' to send.", target,
               body);
      return strdup(buf);
   }

   char result[RESULT_BUF_SIZE];
   phone_service_send_sms(user_id, target, body, result, sizeof(result));
   return strdup(result);
}

static char *handle_confirm_sms(int user_id) {
   if (s_pending_sms_number[0] == '\0') {
      return strdup("Error: no pending SMS to confirm");
   }

   char result[RESULT_BUF_SIZE];
   phone_service_send_sms(user_id, s_pending_sms_number, s_pending_sms_body, result,
                          sizeof(result));

   s_pending_sms_number[0] = '\0';
   s_pending_sms_body[0] = '\0';
   return strdup(result);
}

static char *handle_read_sms(int user_id) {
   phone_sms_log_t entries[20];
   int count = phone_db_sms_get_unread(user_id, entries, 20);

   char *buf = malloc(RESULT_BUF_SIZE);
   if (!buf) {
      return strdup("Error: memory allocation failed");
   }

   int pos = 0;
   if (count <= 0) {
      pos += snprintf(buf, RESULT_BUF_SIZE, "No unread text messages.");
   } else {
      pos += snprintf(buf, RESULT_BUF_SIZE, "Unread text messages (%d):\n", count);
      for (int i = 0; i < count && pos < RESULT_BUF_SIZE - 512; i++) {
         const char *dir = entries[i].direction == PHONE_DIR_INCOMING ? "From" : "To";
         const char *name = entries[i].contact_name[0] ? entries[i].contact_name
                                                       : entries[i].number;

         pos += snprintf(buf + pos, RESULT_BUF_SIZE - pos, "\n%d. %s: %s\n   %s\n", i + 1, dir,
                         name, entries[i].body);

         /* Mark as read */
         phone_db_sms_mark_read(entries[i].id);
      }
   }

   return buf;
}

static char *handle_call_log(struct json_object *details, int user_id) {
   int count = json_get_int(details, "count", 10);
   if (count < 1) {
      count = 10;
   }
   if (count > 20) {
      count = 20;
   }

   phone_call_log_t entries[20];
   int actual = phone_db_call_log_recent(user_id, entries, count);

   char *buf = malloc(RESULT_BUF_SIZE);
   if (!buf) {
      return strdup("Error: memory allocation failed");
   }

   int pos = 0;
   if (actual <= 0) {
      pos += snprintf(buf, RESULT_BUF_SIZE, "No call history.");
   } else {
      pos += snprintf(buf, RESULT_BUF_SIZE, "Recent calls (%d):\n", actual);
      for (int i = 0; i < actual && pos < RESULT_BUF_SIZE - 256; i++) {
         const char *dir = entries[i].direction == PHONE_DIR_INCOMING ? "Incoming" : "Outgoing";
         const char *name = entries[i].contact_name[0] ? entries[i].contact_name
                                                       : entries[i].number;
         const char *status_str[] = { "answered", "missed", "rejected", "failed" };
         const char *status = (entries[i].status >= 0 && entries[i].status <= 3)
                                  ? status_str[entries[i].status]
                                  : "unknown";

         pos += snprintf(buf + pos, RESULT_BUF_SIZE - pos, "\n%d. %s %s — %s (%ds)\n", i + 1, dir,
                         name, status, entries[i].duration_sec);
      }
   }

   return buf;
}

static char *handle_sms_log(struct json_object *details, int user_id) {
   int count = json_get_int(details, "count", 10);
   if (count < 1) {
      count = 10;
   }
   if (count > 20) {
      count = 20;
   }

   phone_sms_log_t entries[20];
   int actual = phone_db_sms_log_recent(user_id, entries, count);

   char *buf = malloc(RESULT_BUF_SIZE);
   if (!buf) {
      return strdup("Error: memory allocation failed");
   }

   int pos = 0;
   if (actual <= 0) {
      pos += snprintf(buf, RESULT_BUF_SIZE, "No text message history.");
   } else {
      pos += snprintf(buf, RESULT_BUF_SIZE, "Recent text messages (%d):\n", actual);
      for (int i = 0; i < actual && pos < RESULT_BUF_SIZE - 512; i++) {
         const char *dir = entries[i].direction == PHONE_DIR_INCOMING ? "From" : "To";
         const char *name = entries[i].contact_name[0] ? entries[i].contact_name
                                                       : entries[i].number;

         /* Truncate body for log display */
         char preview[200];
         size_t blen = strlen(entries[i].body);
         if (blen > 150) {
            memcpy(preview, entries[i].body, 150);
            preview[150] = '\0';
            strncat(preview, "...", sizeof(preview) - strlen(preview) - 1);
         } else {
            memcpy(preview, entries[i].body, blen + 1);
         }

         pos += snprintf(buf + pos, RESULT_BUF_SIZE - pos, "\n%d. %s: %s\n   %s\n", i + 1, dir,
                         name, preview);
      }
   }

   return buf;
}

static char *handle_status(void) {
   phone_state_t state = phone_service_get_state();
   const char *state_str[] = { "idle", "dialing", "ringing (incoming)", "active call",
                               "hanging up" };
   const char *st = (state >= 0 && state <= 4) ? state_str[state] : "unknown";

   char buf[256];
   snprintf(buf, sizeof(buf), "Phone status: %s. Modem: %s.", st,
            phone_service_available() ? "online" : "offline");
   return strdup(buf);
}

/* =============================================================================
 * Main Callback
 * ============================================================================= */

static char *phone_tool_callback(const char *action, char *value, int *should_respond) {
   *should_respond = 1;

   int user_id = tool_get_current_user_id();
   struct json_object *details = NULL;
   if (value && value[0]) {
      details = json_tokener_parse(value);
   }

   char *result = NULL;

   if (strcmp(action, "call") == 0) {
      result = handle_call(details, user_id);
   } else if (strcmp(action, "confirm_call") == 0) {
      result = handle_confirm_call(user_id);
   } else if (strcmp(action, "answer") == 0) {
      result = handle_answer(user_id);
   } else if (strcmp(action, "hang_up") == 0) {
      result = handle_hang_up(user_id);
   } else if (strcmp(action, "send_sms") == 0) {
      result = handle_send_sms(details, user_id);
   } else if (strcmp(action, "confirm_sms") == 0) {
      result = handle_confirm_sms(user_id);
   } else if (strcmp(action, "read_sms") == 0) {
      result = handle_read_sms(user_id);
   } else if (strcmp(action, "call_log") == 0) {
      result = handle_call_log(details, user_id);
   } else if (strcmp(action, "sms_log") == 0) {
      result = handle_sms_log(details, user_id);
   } else if (strcmp(action, "status") == 0) {
      result = handle_status();
   } else {
      char buf[256];
      snprintf(buf, sizeof(buf),
               "Error: unknown action '%s'. Valid: call, confirm_call, answer, hang_up, "
               "send_sms, confirm_sms, read_sms, call_log, sms_log, status",
               action);
      result = strdup(buf);
   }

   if (details) {
      json_object_put(details);
   }
   return result;
}

/* =============================================================================
 * Lifecycle
 * ============================================================================= */

static int phone_tool_init(void) {
   return phone_service_init();
}

static void phone_tool_cleanup(void) {
   phone_service_shutdown();
}

static bool phone_tool_available(void) {
   return s_config.enabled && phone_service_available();
}

/* =============================================================================
 * Config Parser
 * ============================================================================= */

static void phone_tool_config_parse(toml_table_t *table, void *config) {
   phone_tool_config_t *cfg = (phone_tool_config_t *)config;

   if (!table)
      return;

   toml_datum_t enabled = toml_bool_in(table, "enabled");
   if (enabled.ok)
      cfg->enabled = enabled.u.b;

   toml_datum_t confirm = toml_bool_in(table, "confirm_outbound");
   if (confirm.ok)
      cfg->confirm_outbound = confirm.u.b;
}

/* =============================================================================
 * Tool Registration
 * ============================================================================= */

static const treg_param_t phone_params[] = {
   {
       .name = "action",
       .description = "Phone action: 'call' (initiate a phone call), "
                      "'confirm_call' (confirm a pending call), "
                      "'answer' (answer an incoming call), "
                      "'hang_up' (end the current call), "
                      "'send_sms' (compose an SMS for confirmation), "
                      "'confirm_sms' (send confirmed SMS), "
                      "'read_sms' (read unread text messages), "
                      "'call_log' (view recent call history), "
                      "'sms_log' (view recent text messages), "
                      "'status' (check phone/modem status)",
       .type = TOOL_PARAM_TYPE_ENUM,
       .required = true,
       .maps_to = TOOL_MAPS_TO_ACTION,
       .enum_values = { "call", "confirm_call", "answer", "hang_up", "send_sms", "confirm_sms",
                        "read_sms", "call_log", "sms_log", "status" },
       .enum_count = 10,
   },
   {
       .name = "details",
       .description =
           "JSON: "
           "call {target} (target: phone number or contact name), "
           "confirm_call {} (confirms pending call), "
           "answer {} (answers incoming call), "
           "hang_up {} (ends current call), "
           "send_sms {target, body} (target: number or contact name, body: message text), "
           "confirm_sms {} (sends confirmed SMS), "
           "read_sms {} (returns unread messages), "
           "call_log {count?} (recent call history, default 10), "
           "sms_log {count?} (recent text messages, default 10), "
           "status {} (phone and modem status)",
       .type = TOOL_PARAM_TYPE_STRING,
       .required = false,
       .maps_to = TOOL_MAPS_TO_VALUE,
   },
};

static const tool_metadata_t phone_metadata = {
   .name = "phone",
   .device_string = "phone",
   .topic = "dawn",
   .aliases = { "telephone", "call", "sms", "text" },
   .alias_count = 4,

   .description =
       "Make phone calls and send/receive text messages via the cellular modem. "
       "Use 'call' to initiate a call (by contact name or phone number). "
       "Use 'answer' to answer an incoming call. "
       "Use 'hang_up' to end the current call. "
       "Use 'send_sms' to compose a text message (reads back for confirmation). "
       "Use 'read_sms' to check unread text messages. "
       "Use 'call_log' or 'sms_log' to view recent history. "
       "Use 'status' to check phone and modem status. "
       "Calls and SMS require confirmation before executing (say 'confirm' after review). "
       "The 'target' field can be a contact name (resolved via contacts) or a phone number.",
   .params = phone_params,
   .param_count = 2,

   .device_type = TOOL_DEVICE_TYPE_TRIGGER,
   .capabilities = TOOL_CAP_NETWORK | TOOL_CAP_DANGEROUS,
   .is_getter = false,
   .skip_followup = false,
   .default_local = true,
   .default_remote = true,

   .config = &s_config,
   .config_size = sizeof(s_config),
   .config_parser = phone_tool_config_parse,
   .config_section = "phone",

   .is_available = phone_tool_available,
   .init = phone_tool_init,
   .cleanup = phone_tool_cleanup,
   .callback = phone_tool_callback,
};

int phone_tool_register(void) {
   return tool_registry_register(&phone_metadata);
}
