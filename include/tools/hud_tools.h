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
 * HUD Tools - MQTT-based HUD control for OASIS armor system
 *
 * These tools communicate with the OASIS helmet HUD via MQTT.
 * All commands are sent to the "hud" topic.
 */

#ifndef HUD_TOOLS_H
#define HUD_TOOLS_H

/**
 * @brief Register the hud_control meta-tool
 * Controls HUD elements: armor_display, detect, map, info
 * @return 0 on success, non-zero on failure
 */
int hud_control_tool_register(void);

/**
 * @brief Register the hud_mode tool
 * Switches HUD display mode: default, environmental, armor
 * @return 0 on success, non-zero on failure
 */
int hud_mode_tool_register(void);

/**
 * @brief Register the faceplate tool
 * Controls helmet faceplate open/close
 * @return 0 on success, non-zero on failure
 */
int faceplate_tool_register(void);

/**
 * @brief Register the recording meta-tool
 * Controls recording: record, stream, record_and_stream
 * @return 0 on success, non-zero on failure
 */
int recording_tool_register(void);

/**
 * @brief Register the visual_offset tool
 * Adjusts 3D visual offset for stereoscopic display
 * @return 0 on success, non-zero on failure
 */
int visual_offset_tool_register(void);

#endif /* HUD_TOOLS_H */
