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
 * Music Scanner - Background metadata indexing
 *
 * Runs periodic scans of the music directory to keep the metadata
 * database up to date. Designed to run in a low-priority background
 * thread to avoid impacting playback or system responsiveness.
 *
 * Thread Safety:
 *   - start/stop are NOT thread-safe (call from main thread)
 *   - Internally manages its own worker thread
 */

#ifndef MUSIC_SCANNER_H
#define MUSIC_SCANNER_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =============================================================================
 * Configuration
 * ============================================================================= */

/** Default scan interval in minutes (1 hour) */
#define MUSIC_SCANNER_DEFAULT_INTERVAL_MIN 60

/** Minimum scan interval in minutes */
#define MUSIC_SCANNER_MIN_INTERVAL_MIN 5

/* =============================================================================
 * Scanner Control
 * ============================================================================= */

/**
 * @brief Start the background music scanner
 *
 * Launches a background thread that periodically scans the music directory
 * and updates the metadata database. The first scan runs immediately after
 * starting.
 *
 * @param music_dir Directory to scan for music files
 * @param scan_interval_min Interval between scans in minutes (0 to disable periodic scanning)
 * @return 0 on success, non-zero on failure
 */
int music_scanner_start(const char *music_dir, int scan_interval_min);

/**
 * @brief Stop the background music scanner
 *
 * Signals the scanner thread to stop and waits for it to finish.
 * Safe to call if scanner is not running (no-op).
 */
void music_scanner_stop(void);

/**
 * @brief Check if the scanner is running
 *
 * @return true if scanner thread is active
 */
bool music_scanner_is_running(void);

/**
 * @brief Trigger an immediate rescan
 *
 * Wakes up the scanner thread to perform an immediate scan instead of
 * waiting for the next scheduled interval. Has no effect if scanner
 * is not running.
 */
void music_scanner_trigger_rescan(void);

/**
 * @brief Check if initial scan has completed
 *
 * Useful for displaying "indexing" status in UI.
 *
 * @return true if at least one full scan has completed
 */
bool music_scanner_initial_scan_complete(void);

#ifdef __cplusplus
}
#endif

#endif /* MUSIC_SCANNER_H */
