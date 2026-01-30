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
 * Calculator Tool - Math evaluation, unit conversion, base conversion, random numbers
 *
 * Supports actions: evaluate, convert, base, random
 */

#include "tools/calculator_tool.h"

#include <string.h>

#include "logging.h"
#include "tools/calculator.h"
#include "tools/tool_registry.h"

/* ========== Forward Declarations ========== */

static char *calculator_tool_callback(const char *action, char *value, int *should_respond);

/* ========== Tool Parameter Definition ========== */

static const treg_param_t calculator_params[] = {
   {
       .name = "action",
       .description = "The calculator action: 'evaluate' (math expressions), "
                      "'convert' (unit conversion), 'base' (number base conversion), "
                      "'random' (random number generation)",
       .type = TOOL_PARAM_TYPE_ENUM,
       .required = true,
       .maps_to = TOOL_MAPS_TO_ACTION,
       .enum_values = { "evaluate", "convert", "base", "random" },
       .enum_count = 4,
   },
   {
       .name = "value",
       .description = "The value to process. For 'evaluate': math expression (e.g., '2+2*3'). "
                      "For 'convert': 'VALUE UNIT to UNIT' (e.g., '5 miles to km'). "
                      "For 'base': 'VALUE to BASE' (e.g., '255 to hex'). "
                      "For 'random': 'MIN to MAX' or just 'MAX' (e.g., '1 to 100').",
       .type = TOOL_PARAM_TYPE_STRING,
       .required = true,
       .maps_to = TOOL_MAPS_TO_VALUE,
   },
};

/* ========== Tool Metadata ========== */

static const tool_metadata_t calculator_metadata = {
   .name = "calculator",
   .device_string = "calculator",
   .topic = "dawn",
   .aliases = { "calc", "math" },
   .alias_count = 2,

   .description = "Perform mathematical calculations. Actions: "
                  "'evaluate' for math expressions (supports +, -, *, /, ^, sqrt, sin, cos, etc.), "
                  "'convert' for unit conversion (length, mass, volume, temperature, time), "
                  "'base' for number base conversion (hex, decimal, octal, binary), "
                  "'random' for random number generation.",
   .params = calculator_params,
   .param_count = 2,

   .device_type = TOOL_DEVICE_TYPE_GETTER,
   .capabilities = TOOL_CAP_NONE,
   .is_getter = true,
   .skip_followup = false,
   .default_remote = true,

   .config = NULL,
   .config_size = 0,
   .config_parser = NULL,
   .config_section = NULL,

   .init = NULL,
   .cleanup = NULL,
   .callback = calculator_tool_callback,
};

/* ========== Callback Implementation ========== */

static char *calculator_tool_callback(const char *action, char *value, int *should_respond) {
   *should_respond = 1; /* Always return results to LLM */

   if (value == NULL || strlen(value) == 0) {
      LOG_WARNING("calculator_tool_callback: No value provided");
      return strdup("Please provide a value for the calculator.");
   }

   if (strcmp(action, "evaluate") == 0) {
      LOG_INFO("calculator_tool_callback: Evaluating '%s'", value);
      calc_result_t result = calculator_evaluate(value);
      char *formatted = calculator_format_result(&result);
      if (formatted) {
         LOG_INFO("calculator_tool_callback: Result = %s", formatted);
         return formatted;
      }
      return strdup("Failed to evaluate expression.");
   }

   if (strcmp(action, "convert") == 0) {
      LOG_INFO("calculator_tool_callback: Converting '%s'", value);
      char *result = calculator_convert(value);
      return result ? result : strdup("Failed to convert units.");
   }

   if (strcmp(action, "base") == 0) {
      LOG_INFO("calculator_tool_callback: Base converting '%s'", value);
      char *result = calculator_base_convert(value);
      return result ? result : strdup("Failed to convert base.");
   }

   if (strcmp(action, "random") == 0) {
      LOG_INFO("calculator_tool_callback: Random number '%s'", value);
      char *result = calculator_random(value);
      return result ? result : strdup("Failed to generate random number.");
   }

   return strdup("Unknown calculator action. Use: evaluate, convert, base, or random.");
}

/* ========== Public API ========== */

int calculator_tool_register(void) {
   return tool_registry_register(&calculator_metadata);
}
