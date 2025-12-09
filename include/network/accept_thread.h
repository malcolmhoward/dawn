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
 * Accept Thread for multi-client support.
 * Handles TCP connection acceptance and dispatches clients to worker pool.
 *
 * DESIGN:
 *   - Uses select() with 60s timeout to allow periodic cleanup
 *   - Accepts connections, creates sessions, assigns to workers
 *   - Sends NACK if all workers are busy
 *   - Calls session_cleanup_expired() on timeout
 *
 * INTEGRATION:
 *   - Runs alongside existing dawn_server (or replaces it in Phase 2)
 *   - Works with worker_pool for client dispatch
 *   - Works with session_manager for session lifecycle
 */

#ifndef ACCEPT_THREAD_H
#define ACCEPT_THREAD_H

#include <stdbool.h>
#include <stdint.h>

#include "asr/asr_interface.h"

#ifdef __cplusplus
extern "C" {
#endif

// Configuration
#define ACCEPT_THREAD_PORT 5000       // TCP port (same as current server)
#define ACCEPT_THREAD_TIMEOUT_SEC 60  // Select timeout for periodic cleanup
#define ACCEPT_THREAD_BACKLOG 10      // Listen backlog for pending connections

// =============================================================================
// Lifecycle Functions
// =============================================================================

/**
 * @brief Initialize and start the accept thread
 *
 * Creates listening socket, initializes worker pool, and starts accept thread.
 *
 * @param engine_type ASR engine to use for workers (ASR_ENGINE_VOSK or ASR_ENGINE_WHISPER)
 * @param model_path Path to ASR model (Vosk model dir or Whisper .bin file)
 * @return 0 on success, 1 on failure
 *
 * @note This initializes both the accept thread AND the worker pool
 * @note Call accept_thread_stop() to shut down both
 */
int accept_thread_start(asr_engine_type_t engine_type, const char *model_path);

/**
 * @brief Stop accept thread and worker pool
 *
 * Shutdown sequence:
 * 1. Signal accept thread to stop
 * 2. Close listening socket (breaks select())
 * 3. Wait for accept thread to exit
 * 4. Shutdown worker pool
 * 5. Cleanup session manager
 */
void accept_thread_stop(void);

/**
 * @brief Check if accept thread is running
 *
 * @return true if running, false otherwise
 */
bool accept_thread_is_running(void);

// =============================================================================
// Configuration Functions
// =============================================================================

/**
 * @brief Set the listening port for the accept thread
 *
 * @param port TCP port number (default: 5000)
 * @note Must be called before accept_thread_start()
 */
void accept_thread_set_port(uint16_t port);

/**
 * @brief Get the listening port
 *
 * @return Current TCP port number
 */
uint16_t accept_thread_get_port(void);

// =============================================================================
// Metrics
// =============================================================================

/**
 * @brief Get total connections accepted since start
 *
 * @return Number of connections accepted
 */
uint32_t accept_thread_connections_accepted(void);

/**
 * @brief Get total connections rejected (workers busy)
 *
 * @return Number of connections rejected with NACK
 */
uint32_t accept_thread_connections_rejected(void);

#ifdef __cplusplus
}
#endif

#endif  // ACCEPT_THREAD_H
