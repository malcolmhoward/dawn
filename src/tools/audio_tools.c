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
 * Audio Tools - Voice amplifier and audio device control
 */

#include "tools/audio_tools.h"

#include <stdlib.h>
#include <string.h>

#include "logging.h"
#include "tools/tool_registry.h"

/* ========== Forward Declarations for Existing Callbacks ========== */

/* From mosquitto_comms.c */
char *voiceAmplifierCallback(const char *actionName, char *value, int *should_respond);

/* From dawn.c */
char *setPcmPlaybackDevice(const char *actionName, char *value, int *should_respond);
char *setPcmCaptureDevice(const char *actionName, char *value, int *should_respond);

/* ========== Internal Callbacks ========== */

/**
 * @brief Audio device callback that routes to capture or playback based on action
 *
 * The audio_device meta-tool uses the "device" parameter mapped to action to
 * determine whether to call capture or playback callback.
 */
static char *audio_device_callback(const char *action, char *value, int *should_respond) {
   if (!action) {
      *should_respond = 1;
      return strdup("No device type specified. Use 'capture' or 'playback'.");
   }

   if (strcmp(action, "capture") == 0) {
      return setPcmCaptureDevice("set", value, should_respond);
   } else if (strcmp(action, "playback") == 0) {
      return setPcmPlaybackDevice("set", value, should_respond);
   } else {
      *should_respond = 1;
      char *result = malloc(128);
      if (result) {
         snprintf(result, 128, "Unknown device type '%s'. Use 'capture' or 'playback'.", action);
      }
      return result;
   }
}

/* =============================================================================
 * Voice Amplifier Tool
 * ============================================================================= */

static const treg_param_t voice_amplifier_params[] = {
   {
       .name = "action",
       .description = "Whether to enable or disable voice amplification",
       .type = TOOL_PARAM_TYPE_ENUM,
       .required = true,
       .maps_to = TOOL_MAPS_TO_ACTION,
       .enum_values = { "enable", "disable" },
       .enum_count = 2,
   },
};

static const tool_metadata_t voice_amplifier_metadata = {
   .name = "voice_amplifier",
   .device_string = "voice amplifier",
   .topic = "dawn",
   .aliases = { "pa", "pa system", "bullhorn" },
   .alias_count = 3,

   .description =
       "Control the voice amplifier/PA system for projecting voice through external speakers.",
   .params = voice_amplifier_params,
   .param_count = 1,

   .device_type = TOOL_DEVICE_TYPE_BOOLEAN,
   .capabilities = TOOL_CAP_ARMOR_FEATURE,
   .is_getter = false,
   .skip_followup = false,
   .mqtt_only = false,
   .sync_wait = false,
   .default_remote = false,

   .config = NULL,
   .config_size = 0,
   .config_parser = NULL,
   .config_section = NULL,

   .init = NULL,
   .cleanup = NULL,
   .callback = voiceAmplifierCallback,
};

int voice_amplifier_tool_register(void) {
   return tool_registry_register(&voice_amplifier_metadata);
}

/* =============================================================================
 * Audio Device Meta-tool
 * ============================================================================= */

static const treg_param_t audio_device_params[] = {
   {
       .name = "type",
       .description = "The type of audio device to change",
       .type = TOOL_PARAM_TYPE_ENUM,
       .required = true,
       .maps_to = TOOL_MAPS_TO_ACTION,
       .enum_values = { "capture", "playback" },
       .enum_count = 2,
   },
   {
       .name = "device",
       .description = "Device name (e.g., 'microphone', 'headphones', 'speakers')",
       .type = TOOL_PARAM_TYPE_STRING,
       .required = true,
       .maps_to = TOOL_MAPS_TO_VALUE,
   },
};

static const tool_device_map_t audio_device_map[] = {
   { "capture", "audio capture device" },
   { "playback", "audio playback device" },
};

static const tool_metadata_t audio_device_metadata = {
   .name = "audio_device",
   .device_string = "audio_device",
   .topic = "dawn",
   .aliases = { NULL },
   .alias_count = 0,

   .description = "Switch audio input or output devices.",
   .params = audio_device_params,
   .param_count = 2,

   .device_map = audio_device_map,
   .device_map_count = 2,

   .device_type = TOOL_DEVICE_TYPE_ANALOG,
   .capabilities = TOOL_CAP_NONE,
   .is_getter = false,
   .skip_followup = false,
   .mqtt_only = false,
   .sync_wait = false,
   .default_remote = false,

   .config = NULL,
   .config_size = 0,
   .config_parser = NULL,
   .config_section = NULL,

   .init = NULL,
   .cleanup = NULL,
   .callback = audio_device_callback,
};

int audio_device_tool_register(void) {
   return tool_registry_register(&audio_device_metadata);
}
