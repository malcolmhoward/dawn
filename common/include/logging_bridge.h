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
 * the project author(s).
 *
 * DAWN-only bridge that routes DAWN_LOG_* calls from the common library's
 * callback abstraction (logging_common.h) into the canonical log_message()
 * pipeline (logging.h).  Kept separate from logging.c so the canonical
 * logging files remain byte-identical across OASIS repos.
 */

#ifndef DAWN_LOGGING_BRIDGE_H
#define DAWN_LOGGING_BRIDGE_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Register the DAWN logging bridge with the common-library dispatcher.
 *
 * Must be called once after init_logging().  Subsequent DAWN_LOG_* calls
 * from common-library code route through log_message().
 */
void logging_bridge_install(void);

#ifdef __cplusplus
}
#endif

#endif /* DAWN_LOGGING_BRIDGE_H */
