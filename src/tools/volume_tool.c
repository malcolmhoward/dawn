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
 * Volume Tool - Control audio volume level for TTS and music playback
 */

#include "tools/volume_tool.h"

#include <stdlib.h>
#include <string.h>

#include "audio/flac_playback.h"
#include "dawn.h"
#include "logging.h"
#include "tools/tool_registry.h"
#include "word_to_number.h"

/* ========== Forward Declarations ========== */

static char *volume_tool_callback(const char *action, char *value, int *should_respond);

/* ========== Parameter Definitions ========== */

static const treg_param_t volume_params[] = {
   {
       .name = "level",
       .description = "Volume level from 0 (silent) to 100 (maximum)",
       .type = TOOL_PARAM_TYPE_INT,
       .required = true,
       .maps_to = TOOL_MAPS_TO_VALUE,
   },
};

/* ========== Tool Metadata ========== */

static const tool_metadata_t volume_metadata = {
   .name = "volume",
   .device_string = "volume",
   .topic = "dawn",
   .aliases = { NULL },
   .alias_count = 0,

   .description = "Set the audio volume level for TTS and music playback.",
   .params = volume_params,
   .param_count = 1,

   .device_type = TOOL_DEVICE_TYPE_ANALOG,
   .capabilities = TOOL_CAP_NONE,
   .is_getter = false,
   .skip_followup = false,
   .default_remote = false,

   .config = NULL,
   .config_size = 0,
   .config_parser = NULL,
   .config_section = NULL,

   .init = NULL,
   .cleanup = NULL,
   .callback = volume_tool_callback,
};

/* ========== Callback Implementation ========== */

static char *volume_tool_callback(const char *action, char *value, int *should_respond) {
   (void)action;

   char *result = NULL;
   float floatVol = -1.0f;

   /* Try to parse as numeric string first (handles "100", "50", "0.5", etc) */
   char *endptr;
   double parsed = strtod(value, &endptr);
   if (endptr != value && *endptr == '\0') {
      /* Successfully parsed as number */
      floatVol = (float)parsed;
      /* If value > 2.0, assume it's a percentage (0-100) and convert to 0.0-1.0 */
      if (floatVol > 2.0f) {
         floatVol = floatVol / 100.0f;
      }
   } else {
      /* Fall back to word parsing ("fifty", "one hundred", etc) */
      floatVol = (float)wordToNumber(value);
      /* wordToNumber returns 0-100 for percentages, convert to 0.0-1.0 */
      if (floatVol > 2.0f) {
         floatVol = floatVol / 100.0f;
      }
   }

   LOG_INFO("Volume: %s -> %.2f", value, floatVol);

   if (floatVol >= 0 && floatVol <= 2.0) {
      setMusicVolume(floatVol);

      if (command_processing_mode == CMD_MODE_DIRECT_ONLY) {
         *should_respond = 0; /* No response in direct mode for volume */
         return NULL;
      } else {
         /* AI modes: return confirmation */
         result = malloc(64);
         if (result) {
            snprintf(result, 64, "Volume set to %.0f%%", floatVol * 100.0f);
         }
         *should_respond = 1;
         return result;
      }
   } else {
      if (command_processing_mode == CMD_MODE_DIRECT_ONLY) {
         int local_should_respond = 0;
         textToSpeechCallback(NULL, "Invalid volume level requested.", &local_should_respond);
         *should_respond = 0;
         return NULL;
      } else {
         result = malloc(128);
         if (result) {
            snprintf(result, 128,
                     "Invalid volume level %.1f requested. Volume must be between 0 and 2.",
                     floatVol);
         }
         *should_respond = 1;
         return result;
      }
   }
}

/* ========== Public API ========== */

int volume_tool_register(void) {
   return tool_registry_register(&volume_metadata);
}
