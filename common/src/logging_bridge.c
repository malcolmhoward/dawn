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
 * DAWN logging bridge — routes DAWN_LOG_* → log_message_v().  DAWN-only;
 * kept out of logging.c so the canonical logging files stay byte-identical
 * across OASIS repos.
 */

#include "logging_bridge.h"

#include <stdarg.h>

#include "logging.h"
#include "logging_common.h"

/*
 * Common-library log levels map to canonical log levels by position.  The
 * _Static_asserts pin the enum values so the bridge still routes correctly
 * if the enum is ever extended (new entries must be appended, not inserted).
 */
_Static_assert(DAWN_LOG_INFO == 0, "DAWN_LOG_INFO must be 0 for level_map indexing");
_Static_assert(DAWN_LOG_WARNING == 1, "DAWN_LOG_WARNING must be 1 for level_map indexing");
_Static_assert(DAWN_LOG_ERROR == 2, "DAWN_LOG_ERROR must be 2 for level_map indexing");

static const log_level_t level_map[] = {
   [DAWN_LOG_INFO] = LOGLEVEL_INFO,
   [DAWN_LOG_WARNING] = LOGLEVEL_WARNING,
   [DAWN_LOG_ERROR] = LOGLEVEL_ERROR,
};

/*
 * Bridge callback.  Forwards the va_list directly to log_message_v() so the
 * message is formatted exactly once.  The `func` parameter is accepted (for
 * ABI compatibility with the common-library callback signature) but not
 * forwarded — log_message_v() does not display it.
 */
static void bridge_callback(dawn_log_level_t level,
                            const char *file,
                            int line,
                            const char *func,
                            const char *fmt,
                            va_list args) {
   (void)func;

   log_level_t mapped = (level <= DAWN_LOG_ERROR) ? level_map[level] : LOGLEVEL_INFO;
   log_message_v(mapped, file, line, fmt, args);
}

void logging_bridge_install(void) {
   dawn_common_set_logger(bridge_callback);
}
