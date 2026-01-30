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
 * Switch LLM Tool - Switch between local and cloud LLM providers
 */

#include "tools/switch_llm_tool.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "core/session_manager.h"
#include "llm/llm_interface.h"
#include "logging.h"
#include "tools/tool_registry.h"

/* ========== Forward Declarations ========== */

static char *switch_llm_tool_callback(const char *action, char *value, int *should_respond);

/* ========== Parameter Definitions ========== */

static const treg_param_t switch_llm_params[] = {
   {
       .name = "target",
       .description = "The LLM target to switch to",
       .type = TOOL_PARAM_TYPE_ENUM,
       .required = true,
       .maps_to = TOOL_MAPS_TO_ACTION,
       .enum_values = { "local", "cloud", "openai", "claude", "gemini" },
       .enum_count = 5,
   },
};

/* ========== Tool Metadata ========== */

static const tool_metadata_t switch_llm_metadata = {
   .name = "switch_llm",
   .device_string = "switch_llm",
   .topic = "dawn",
   .aliases = { "llm", "ai", "model", "provider" },
   .alias_count = 4,

   .description =
       "Switch between local and cloud LLM, or change the cloud provider. Use 'local' to "
       "use the local LLM server, 'cloud' to use cloud AI, or specify a provider name like "
       "'openai', 'claude', or 'gemini'.",
   .params = switch_llm_params,
   .param_count = 1,

   .device_type = TOOL_DEVICE_TYPE_ANALOG,
   .capabilities = TOOL_CAP_NONE,
   .is_getter = false,
   .skip_followup = false,
   .default_remote = true,

   .config = NULL,
   .config_size = 0,
   .config_parser = NULL,
   .config_section = NULL,

   .init = NULL,
   .cleanup = NULL,
   .callback = switch_llm_tool_callback,
};

/* ========== Callback Implementation ========== */

static char *switch_llm_tool_callback(const char *action, char *value, int *should_respond) {
   *should_respond = 1;

   /* For LLM tool calling: target comes in action parameter
    * For voice commands via device_types: action="set", target comes in value parameter */
   const char *target = action;
   if (action && strcasecmp(target, "set") == 0 && value && value[0]) {
      target = value;
   }

   if (!target || target[0] == '\0') {
      return strdup("No LLM target specified. Use 'local', 'cloud', 'openai', 'claude', or "
                    "'gemini'.");
   }

   /* Get session from command context, fall back to local session for external MQTT */
   session_t *session = session_get_command_context();
   if (!session) {
      session = session_get_local();
   }

   session_llm_config_t config;
   session_get_llm_config(session, &config);

   if (strcasecmp(target, "local") == 0 || strcasecmp(target, "llama") == 0) {
      LOG_INFO("Setting AI to local LLM via switch_llm tool.");
      config.type = LLM_LOCAL;
      config.cloud_provider = CLOUD_PROVIDER_NONE;
      config.model[0] = '\0'; /* Clear model to use provider default */
      if (session_set_llm_config(session, &config) != 0) {
         return strdup("Failed to switch to local LLM");
      }
      return strdup("AI switched to local LLM");
   } else if (strcasecmp(target, "cloud") == 0) {
      LOG_INFO("Setting AI to cloud LLM via switch_llm tool.");
      config.type = LLM_CLOUD;
      config.model[0] = '\0'; /* Clear model to use provider default */
      if (session_set_llm_config(session, &config) != 0) {
         return strdup("Failed to switch to cloud LLM. API key not configured.");
      }
      return strdup("AI switched to cloud LLM");
   } else if (strcasecmp(target, "openai") == 0 || strcasecmp(target, "gpt") == 0 ||
              strcasecmp(target, "chatgpt") == 0 || strcasecmp(target, "chat gpt") == 0 ||
              strcasecmp(target, "open ai") == 0) {
      LOG_INFO("Setting AI to OpenAI via switch_llm tool.");
      config.type = LLM_CLOUD;
      config.cloud_provider = CLOUD_PROVIDER_OPENAI;
      config.model[0] = '\0'; /* Clear model to use provider default */
      if (session_set_llm_config(session, &config) != 0) {
         return strdup("Failed to switch to OpenAI. API key not configured.");
      }
      return strdup("AI switched to OpenAI");
   } else if (strcasecmp(target, "claude") == 0 || strcasecmp(target, "clawed") == 0 ||
              strcasecmp(target, "claud") == 0 || strcasecmp(target, "cloud ai") == 0) {
      LOG_INFO("Setting AI to Claude via switch_llm tool.");
      config.type = LLM_CLOUD;
      config.cloud_provider = CLOUD_PROVIDER_CLAUDE;
      config.model[0] = '\0'; /* Clear model to use provider default */
      if (session_set_llm_config(session, &config) != 0) {
         return strdup("Failed to switch to Claude. API key not configured.");
      }
      return strdup("AI switched to Claude");
   } else if (strcasecmp(target, "gemini") == 0 || strcasecmp(target, "jiminy") == 0) {
      LOG_INFO("Setting AI to Gemini via switch_llm tool.");
      config.type = LLM_CLOUD;
      config.cloud_provider = CLOUD_PROVIDER_GEMINI;
      config.model[0] = '\0'; /* Clear model to use provider default */
      if (session_set_llm_config(session, &config) != 0) {
         return strdup("Failed to switch to Gemini. API key not configured.");
      }
      return strdup("AI switched to Gemini");
   } else {
      char *result = malloc(128);
      if (result) {
         snprintf(result, 128,
                  "Unknown LLM target '%s'. Use 'local', 'cloud', 'openai', 'claude', "
                  "or 'gemini'.",
                  target);
      }
      return result;
   }
}

/* ========== Public API ========== */

int switch_llm_tool_register(void) {
   return tool_registry_register(&switch_llm_metadata);
}
