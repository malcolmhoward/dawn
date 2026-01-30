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
 * Tools Initialization - Central registration point for all modular tools
 */

#include "tools/tools_init.h"

#include "logging.h"

/* ========== Tool Headers (conditionally included) ========== */

#ifdef DAWN_ENABLE_SHUTDOWN_TOOL
#include "tools/shutdown_tool.h"
#endif
#ifdef DAWN_ENABLE_MUSIC_TOOL
#include "tools/music_tool.h"
#endif
#ifdef DAWN_ENABLE_CALCULATOR_TOOL
#include "tools/calculator_tool.h"
#endif
#ifdef DAWN_ENABLE_WEATHER_TOOL
#include "tools/weather_tool.h"
#endif
#ifdef DAWN_ENABLE_SEARCH_TOOL
#include "tools/search_tool.h"
#endif
#ifdef DAWN_ENABLE_URL_TOOL
#include "tools/url_tool.h"
#endif
#ifdef DAWN_ENABLE_SMARTTHINGS_TOOL
#include "tools/smartthings_tool.h"
#endif
#ifdef DAWN_ENABLE_MEMORY_TOOL
#include "tools/memory_tool.h"
#endif
#ifdef DAWN_ENABLE_DATETIME_TOOL
#include "tools/datetime_tool.h"
#endif
#ifdef DAWN_ENABLE_VOLUME_TOOL
#include "tools/volume_tool.h"
#endif
#ifdef DAWN_ENABLE_LLM_STATUS_TOOL
#include "tools/llm_status_tool.h"
#endif
#ifdef DAWN_ENABLE_SWITCH_LLM_TOOL
#include "tools/switch_llm_tool.h"
#endif
#ifdef DAWN_ENABLE_RESET_CONVERSATION_TOOL
#include "tools/reset_conversation_tool.h"
#endif
#ifdef DAWN_ENABLE_VIEWING_TOOL
#include "tools/viewing_tool.h"
#endif
#ifdef DAWN_ENABLE_HUD_TOOLS
#include "tools/hud_tools.h"
#endif
#ifdef DAWN_ENABLE_AUDIO_TOOLS
#include "tools/audio_tools.h"
#endif

/* ========== Registration ========== */

int tools_register_all(void) {
   LOG_INFO("Registering modular tools...");

#ifdef DAWN_ENABLE_SHUTDOWN_TOOL
   if (shutdown_tool_register() != 0) {
      LOG_WARNING("Failed to register shutdown tool");
   }
#endif

#ifdef DAWN_ENABLE_MUSIC_TOOL
   if (music_tool_register() != 0) {
      LOG_WARNING("Failed to register music tool");
   }
#endif

#ifdef DAWN_ENABLE_CALCULATOR_TOOL
   if (calculator_tool_register() != 0) {
      LOG_WARNING("Failed to register calculator tool");
   }
#endif

#ifdef DAWN_ENABLE_WEATHER_TOOL
   if (weather_tool_register() != 0) {
      LOG_WARNING("Failed to register weather tool");
   }
#endif

#ifdef DAWN_ENABLE_SEARCH_TOOL
   if (search_tool_register() != 0) {
      LOG_WARNING("Failed to register search tool");
   }
#endif

#ifdef DAWN_ENABLE_URL_TOOL
   if (url_tool_register() != 0) {
      LOG_WARNING("Failed to register url_fetch tool");
   }
#endif

#ifdef DAWN_ENABLE_SMARTTHINGS_TOOL
   if (smartthings_tool_register() != 0) {
      LOG_WARNING("Failed to register smartthings tool");
   }
#endif

#ifdef DAWN_ENABLE_MEMORY_TOOL
   if (memory_tool_register() != 0) {
      LOG_WARNING("Failed to register memory tool");
   }
#endif

#ifdef DAWN_ENABLE_DATETIME_TOOL
   if (date_tool_register() != 0) {
      LOG_WARNING("Failed to register date tool");
   }
   if (time_tool_register() != 0) {
      LOG_WARNING("Failed to register time tool");
   }
#endif

#ifdef DAWN_ENABLE_VOLUME_TOOL
   if (volume_tool_register() != 0) {
      LOG_WARNING("Failed to register volume tool");
   }
#endif

#ifdef DAWN_ENABLE_LLM_STATUS_TOOL
   if (llm_status_tool_register() != 0) {
      LOG_WARNING("Failed to register llm_status tool");
   }
#endif

#ifdef DAWN_ENABLE_SWITCH_LLM_TOOL
   if (switch_llm_tool_register() != 0) {
      LOG_WARNING("Failed to register switch_llm tool");
   }
#endif

#ifdef DAWN_ENABLE_RESET_CONVERSATION_TOOL
   if (reset_conversation_tool_register() != 0) {
      LOG_WARNING("Failed to register reset_conversation tool");
   }
#endif

#ifdef DAWN_ENABLE_VIEWING_TOOL
   if (viewing_tool_register() != 0) {
      LOG_WARNING("Failed to register viewing tool");
   }
#endif

#ifdef DAWN_ENABLE_HUD_TOOLS
   if (hud_control_tool_register() != 0) {
      LOG_WARNING("Failed to register hud_control tool");
   }
   if (hud_mode_tool_register() != 0) {
      LOG_WARNING("Failed to register hud_mode tool");
   }
   if (faceplate_tool_register() != 0) {
      LOG_WARNING("Failed to register faceplate tool");
   }
   if (recording_tool_register() != 0) {
      LOG_WARNING("Failed to register recording tool");
   }
   if (visual_offset_tool_register() != 0) {
      LOG_WARNING("Failed to register visual_offset tool");
   }
#endif

#ifdef DAWN_ENABLE_AUDIO_TOOLS
   if (voice_amplifier_tool_register() != 0) {
      LOG_WARNING("Failed to register voice_amplifier tool");
   }
   if (audio_device_tool_register() != 0) {
      LOG_WARNING("Failed to register audio_device tool");
   }
#endif

   LOG_INFO("Tool registration complete");
   return 0;
}
