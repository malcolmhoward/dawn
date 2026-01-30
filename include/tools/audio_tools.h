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

#ifndef AUDIO_TOOLS_H
#define AUDIO_TOOLS_H

/**
 * @brief Register the voice_amplifier tool
 * Controls the PA system for voice passthrough
 * @return 0 on success, non-zero on failure
 */
int voice_amplifier_tool_register(void);

/**
 * @brief Register the audio_device meta-tool
 * Switches audio input/output devices
 * @return 0 on success, non-zero on failure
 */
int audio_device_tool_register(void);

#endif /* AUDIO_TOOLS_H */
