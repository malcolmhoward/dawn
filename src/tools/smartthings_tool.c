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
 * SmartThings Tool - Smart home device control via Samsung SmartThings
 *
 * Supports: list, status, on, off, brightness, color, temperature, lock, unlock
 */

#include "tools/smartthings_tool.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "logging.h"
#include "tools/smartthings_service.h"
#include "tools/tool_registry.h"

/* ========== Forward Declarations ========== */

static char *smartthings_tool_callback(const char *action, char *value, int *should_respond);
static bool smartthings_tool_is_available(void);

/* ========== Tool Parameter Definition ========== */

static const treg_param_t smartthings_params[] = {
   {
       .name = "action",
       .description = "SmartThings action: 'list' (all devices), 'status' (device status), "
                      "'on' (turn on), 'off' (turn off), 'brightness' (set level), "
                      "'color' (set color), 'temperature' (thermostat), 'lock', 'unlock'",
       .type = TOOL_PARAM_TYPE_ENUM,
       .required = true,
       .maps_to = TOOL_MAPS_TO_ACTION,
       .enum_values = { "list", "status", "on", "off", "brightness", "color", "temperature", "lock",
                        "unlock" },
       .enum_count = 9,
   },
   {
       .name = "device",
       .description = "Device name (required for all actions except 'list'). "
                      "For brightness: 'device_name level' (e.g., 'lamp 75'). "
                      "For color: 'device_name color' (e.g., 'lamp red'). "
                      "For temperature: 'device_name temp' (e.g., 'thermostat 72').",
       .type = TOOL_PARAM_TYPE_STRING,
       .required = false,
       .maps_to = TOOL_MAPS_TO_VALUE,
   },
};

/* ========== Tool Metadata ========== */

static const tool_metadata_t smartthings_metadata = {
   .name = "smartthings",
   .device_string = "smartthings",
   .topic = "dawn",
   .aliases = { "smarthome", "iot" },
   .alias_count = 2,

   .description =
       "Control SmartThings smart home devices. Actions: list (show all devices), "
       "status (get device state), on/off (switch power), brightness (set dimmer 0-100), "
       "color (red/orange/yellow/green/cyan/blue/purple/pink/white), "
       "temperature (thermostat 50-90F), lock/unlock (door locks).",
   .params = smartthings_params,
   .param_count = 2,

   .device_type = TOOL_DEVICE_TYPE_GETTER, /* Most actions return status */
   .capabilities = TOOL_CAP_NETWORK | TOOL_CAP_SECRETS | TOOL_CAP_SCHEDULABLE,
   .is_getter = false, /* Has side effects (on/off/etc) */
   .skip_followup = false,
   .default_remote = true,

   .config = NULL,
   .config_size = 0,
   .config_parser = NULL,
   .config_section = NULL,

   .is_available = smartthings_tool_is_available,

   .init = NULL,
   .cleanup = NULL,
   .callback = smartthings_tool_callback,
};

/* ========== Availability Check ========== */

static bool smartthings_tool_is_available(void) {
   return smartthings_is_authenticated();
}

/* ========== Color Name Mappings ========== */

typedef struct {
   const char *name;
   int hue;
   int sat;
} color_map_t;

static const color_map_t color_names[] = {
   { "red", 0, 100 },     { "orange", 8, 100 }, { "yellow", 17, 100 },
   { "green", 33, 100 },  { "cyan", 50, 100 },  { "blue", 67, 100 },
   { "purple", 75, 100 }, { "pink", 92, 80 },   { "white", 0, 0 },
};
static const int color_count = sizeof(color_names) / sizeof(color_names[0]);

/* ========== Helper Functions ========== */

static char *make_error_msg(const char *fmt, const char *arg) {
   char *msg = malloc(256);
   if (msg) {
      snprintf(msg, 256, fmt, arg);
   }
   return msg ? msg : strdup("Error");
}

static char *make_success_msg(const char *fmt, const char *arg) {
   char *msg = malloc(256);
   if (msg) {
      snprintf(msg, 256, fmt, arg);
   }
   return msg ? msg : strdup("Success");
}

static char *make_success_msg_int(const char *fmt, const char *arg, int val) {
   char *msg = malloc(256);
   if (msg) {
      snprintf(msg, 256, fmt, arg, val);
   }
   return msg ? msg : strdup("Success");
}

static char *make_success_msg_double(const char *fmt, const char *arg, double val) {
   char *msg = malloc(256);
   if (msg) {
      snprintf(msg, 256, fmt, arg, val);
   }
   return msg ? msg : strdup("Success");
}

/* ========== Action Handlers ========== */

static char *handle_list(void) {
   const st_device_list_t *devices;
   st_error_t err = smartthings_list_devices(&devices);
   if (err != ST_OK) {
      return make_error_msg("Failed to list devices: %s", smartthings_error_str(err));
   }

   const size_t buf_size = 8192;
   char *buf = malloc(buf_size);
   if (!buf)
      return strdup("Memory allocation failed");

   size_t len = 0;
   size_t remaining = buf_size;
   int n = snprintf(buf, remaining, "Found %d SmartThings devices:\n", devices->count);
   if (n > 0 && (size_t)n < remaining) {
      len = (size_t)n;
      remaining -= len;
   }

   for (int i = 0; i < devices->count && remaining > 100; i++) {
      const st_device_t *dev = &devices->devices[i];

      /* Build capability string */
      char caps[256] = "";
      size_t caps_len = 0;
      for (int j = 0; j < 15 && caps_len < 240; j++) {
         st_capability_t cap = (st_capability_t)(1 << j);
         if (dev->capabilities & cap) {
            if (caps_len > 0) {
               n = snprintf(caps + caps_len, 256 - caps_len, ", ");
               if (n > 0)
                  caps_len += (size_t)n;
            }
            n = snprintf(caps + caps_len, 256 - caps_len, "%s", smartthings_capability_str(cap));
            if (n > 0)
               caps_len += (size_t)n;
         }
      }

      n = snprintf(buf + len, remaining, "- %s (%s)\n", dev->label, caps);
      if (n > 0 && (size_t)n < remaining) {
         len += (size_t)n;
         remaining -= (size_t)n;
      }
   }
   return buf;
}

static char *handle_status(const char *value) {
   if (!value || !value[0])
      return strdup("Please specify a device name.");

   const st_device_t *device;
   st_error_t err = smartthings_find_device(value, &device);
   if (err != ST_OK) {
      return make_error_msg("Device '%s' not found", value);
   }

   st_device_state_t state;
   err = smartthings_get_device_status(device->id, &state);
   if (err != ST_OK) {
      return make_error_msg("Failed to get status: %s", smartthings_error_str(err));
   }

   const size_t buf_size = 1024;
   char *buf = malloc(buf_size);
   if (!buf)
      return strdup("Memory allocation failed");

   size_t len = 0;
   size_t remaining = buf_size;
   int n;

/* Helper macro for safe snprintf with truncation handling */
#define SAFE_APPEND(...)                               \
   do {                                                \
      n = snprintf(buf + len, remaining, __VA_ARGS__); \
      if (n > 0 && (size_t)n < remaining) {            \
         len += (size_t)n;                             \
         remaining -= (size_t)n;                       \
      }                                                \
   } while (0)

   SAFE_APPEND("Status of '%s':\n", device->label);

   if (device->capabilities & ST_CAP_SWITCH) {
      SAFE_APPEND("- Power: %s\n", state.switch_on ? "on" : "off");
   }
   if (device->capabilities & ST_CAP_SWITCH_LEVEL) {
      SAFE_APPEND("- Brightness: %d%%\n", state.level);
   }
   if (device->capabilities & ST_CAP_COLOR_CONTROL) {
      SAFE_APPEND("- Color: hue=%d, saturation=%d\n", state.hue, state.saturation);
   }
   if (device->capabilities & ST_CAP_COLOR_TEMP) {
      SAFE_APPEND("- Color temp: %dK\n", state.color_temp);
   }
   if (device->capabilities & ST_CAP_TEMPERATURE) {
      SAFE_APPEND("- Temperature: %.1f\n", state.temperature);
   }
   if (device->capabilities & ST_CAP_HUMIDITY) {
      SAFE_APPEND("- Humidity: %.1f%%\n", state.humidity);
   }
   if (device->capabilities & ST_CAP_LOCK) {
      SAFE_APPEND("- Lock: %s\n", state.locked ? "locked" : "unlocked");
   }
   if (device->capabilities & ST_CAP_BATTERY) {
      SAFE_APPEND("- Battery: %d%%\n", state.battery);
   }
   if (device->capabilities & ST_CAP_MOTION) {
      SAFE_APPEND("- Motion: %s\n", state.motion_active ? "detected" : "none");
   }
   if (device->capabilities & ST_CAP_CONTACT) {
      SAFE_APPEND("- Contact: %s\n", state.contact_open ? "open" : "closed");
   }

#undef SAFE_APPEND

   return buf;
}

static char *handle_on(const char *value) {
   if (!value || !value[0])
      return strdup("Please specify a device name.");

   const st_device_t *device;
   st_error_t err = smartthings_find_device(value, &device);
   if (err != ST_OK) {
      return make_error_msg("Device '%s' not found", value);
   }

   err = smartthings_switch_on(device->id);
   if (err != ST_OK) {
      return make_error_msg("Failed to turn on: %s", smartthings_error_str(err));
   }

   return make_success_msg("Turned on '%s'", device->label);
}

static char *handle_off(const char *value) {
   if (!value || !value[0])
      return strdup("Please specify a device name.");

   const st_device_t *device;
   st_error_t err = smartthings_find_device(value, &device);
   if (err != ST_OK) {
      return make_error_msg("Device '%s' not found", value);
   }

   err = smartthings_switch_off(device->id);
   if (err != ST_OK) {
      return make_error_msg("Failed to turn off: %s", smartthings_error_str(err));
   }

   return make_success_msg("Turned off '%s'", device->label);
}

static char *handle_brightness(const char *value) {
   if (!value || !value[0])
      return strdup("Please specify device name and brightness level (e.g., 'lamp 75').");

   /* Parse "device_name level" from value */
   char device_name[128];
   int level = -1;

   /* Find last number in string */
   const char *last_space = strrchr(value, ' ');
   if (last_space && last_space[1]) {
      level = atoi(last_space + 1);
      size_t name_len = last_space - value;
      if (name_len >= sizeof(device_name))
         name_len = sizeof(device_name) - 1;
      strncpy(device_name, value, name_len);
      device_name[name_len] = '\0';
   }

   if (level < 0 || level > 100) {
      return strdup("Please specify device name and brightness (0-100).");
   }

   const st_device_t *device;
   st_error_t err = smartthings_find_device(device_name, &device);
   if (err != ST_OK) {
      return make_error_msg("Device '%s' not found", device_name);
   }

   err = smartthings_set_level(device->id, level);
   if (err != ST_OK) {
      return make_error_msg("Failed to set brightness: %s", smartthings_error_str(err));
   }

   return make_success_msg_int("Set '%s' brightness to %d%%", device->label, level);
}

static char *handle_color(const char *value) {
   if (!value || !value[0])
      return strdup("Please specify device name and color (e.g., 'lamp red' or 'lamp blue').");

   /* Try to parse color name */
   char device_name[128];
   int hue = -1, sat = -1;

   /* Check for color name */
   const char *last_word = strrchr(value, ' ');
   if (last_word) {
      for (int i = 0; i < color_count; i++) {
         if (strcasecmp(last_word + 1, color_names[i].name) == 0) {
            hue = color_names[i].hue;
            sat = color_names[i].sat;
            size_t name_len = last_word - value;
            if (name_len >= sizeof(device_name))
               name_len = sizeof(device_name) - 1;
            strncpy(device_name, value, name_len);
            device_name[name_len] = '\0';
            break;
         }
      }
   }

   if (hue < 0) {
      return strdup(
          "Unknown color. Try: red, orange, yellow, green, cyan, blue, purple, pink, white");
   }

   const st_device_t *device;
   st_error_t err = smartthings_find_device(device_name, &device);
   if (err != ST_OK) {
      return make_error_msg("Device '%s' not found", device_name);
   }

   err = smartthings_set_color(device->id, hue, sat);
   if (err != ST_OK) {
      return make_error_msg("Failed to set color: %s", smartthings_error_str(err));
   }

   return make_success_msg("Set '%s' color", device->label);
}

static char *handle_temperature(const char *value) {
   if (!value || !value[0])
      return strdup("Please specify device name and temperature (e.g., 'thermostat 72').");

   char device_name[128];
   double temp = -1;

   const char *last_space = strrchr(value, ' ');
   if (last_space && last_space[1]) {
      temp = atof(last_space + 1);
      size_t name_len = last_space - value;
      if (name_len >= sizeof(device_name))
         name_len = sizeof(device_name) - 1;
      strncpy(device_name, value, name_len);
      device_name[name_len] = '\0';
   }

   if (temp < 50 || temp > 90) {
      return strdup("Please specify a valid temperature (50-90F).");
   }

   const st_device_t *device;
   st_error_t err = smartthings_find_device(device_name, &device);
   if (err != ST_OK) {
      return make_error_msg("Device '%s' not found", device_name);
   }

   err = smartthings_set_thermostat(device->id, temp);
   if (err != ST_OK) {
      return make_error_msg("Failed to set temperature: %s", smartthings_error_str(err));
   }

   return make_success_msg_double("Set '%s' to %.0f°F", device->label, temp);
}

static char *handle_lock(const char *value) {
   if (!value || !value[0])
      return strdup("Please specify a lock device name.");

   const st_device_t *device;
   st_error_t err = smartthings_find_device(value, &device);
   if (err != ST_OK) {
      return make_error_msg("Device '%s' not found", value);
   }

   err = smartthings_lock(device->id);
   if (err != ST_OK) {
      return make_error_msg("Failed to lock: %s", smartthings_error_str(err));
   }

   return make_success_msg("Locked '%s'", device->label);
}

static char *handle_unlock(const char *value) {
   if (!value || !value[0])
      return strdup("Please specify a lock device name.");

   const st_device_t *device;
   st_error_t err = smartthings_find_device(value, &device);
   if (err != ST_OK) {
      return make_error_msg("Device '%s' not found", value);
   }

   err = smartthings_unlock(device->id);
   if (err != ST_OK) {
      return make_error_msg("Failed to unlock: %s", smartthings_error_str(err));
   }

   return make_success_msg("Unlocked '%s'", device->label);
}

/* ========== Callback Implementation ========== */

static char *smartthings_tool_callback(const char *action, char *value, int *should_respond) {
   *should_respond = 1;

   /* Check if service is configured */
   if (!smartthings_is_configured()) {
      return strdup("SmartThings is not configured. Please add client_id and client_secret to "
                    "secrets.toml.");
   }

   /* Check if authenticated */
   if (!smartthings_is_authenticated()) {
      return strdup("SmartThings is not connected. Please authorize via the WebUI settings.");
   }

   /* Dispatch to action handlers */
   if (strcmp(action, "list") == 0) {
      return handle_list();
   } else if (strcmp(action, "status") == 0) {
      return handle_status(value);
   } else if (strcmp(action, "on") == 0) {
      return handle_on(value);
   } else if (strcmp(action, "off") == 0) {
      return handle_off(value);
   } else if (strcmp(action, "brightness") == 0) {
      return handle_brightness(value);
   } else if (strcmp(action, "color") == 0) {
      return handle_color(value);
   } else if (strcmp(action, "temperature") == 0) {
      return handle_temperature(value);
   } else if (strcmp(action, "lock") == 0) {
      return handle_lock(value);
   } else if (strcmp(action, "unlock") == 0) {
      return handle_unlock(value);
   }

   /* Unknown action */
   char *msg = malloc(256);
   snprintf(msg, 256,
            "Unknown SmartThings action '%s'. Supported: list, status, on, off, brightness, "
            "color, temperature, lock, unlock",
            action);
   return msg;
}

/* ========== Public API ========== */

int smartthings_tool_register(void) {
   return tool_registry_register(&smartthings_metadata);
}
