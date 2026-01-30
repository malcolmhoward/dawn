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
 * Weather Tool - Weather forecasts via Open-Meteo API
 */

#ifndef WEATHER_TOOL_H
#define WEATHER_TOOL_H

/**
 * @brief Register the weather tool with the tool registry
 *
 * Call this during startup to register the weather forecast tool.
 * Supports: today, tomorrow, week forecasts for any location.
 *
 * @return 0 on success, non-zero on failure
 */
int weather_tool_register(void);

#endif /* WEATHER_TOOL_H */
