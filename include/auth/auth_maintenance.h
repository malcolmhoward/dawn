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
 * Background maintenance thread for authentication database.
 * Handles periodic cleanup of expired sessions, failed login attempts,
 * and old audit log entries.
 */

#ifndef AUTH_MAINTENANCE_H
#define AUTH_MAINTENANCE_H

/**
 * @brief Maintenance interval in seconds.
 *
 * Cleanup runs every 15 minutes to balance freshness with resource usage.
 */
#define AUTH_MAINTENANCE_INTERVAL_SEC 900

/**
 * @brief Start the background maintenance thread.
 *
 * The thread runs at reduced priority (nice +10) to avoid impacting
 * voice processing. It performs:
 * - Cleanup of expired sessions
 * - Cleanup of old failed login attempts
 * - Cleanup of old audit log entries
 * - Passive WAL checkpointing
 *
 * @return 0 on success, non-zero on failure.
 */
int auth_maintenance_start(void);

/**
 * @brief Stop the background maintenance thread.
 *
 * Signals the thread to stop and waits for it to exit cleanly.
 * Safe to call even if the thread was never started.
 */
void auth_maintenance_stop(void);

/**
 * @brief Check if maintenance thread is running.
 *
 * @return true if running, false otherwise.
 */
int auth_maintenance_is_running(void);

#endif /* AUTH_MAINTENANCE_H */
