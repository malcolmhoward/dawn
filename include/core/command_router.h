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
 * Command Router - MQTT request/response for worker threads
 *
 * Enables worker threads to receive command callback results via MQTT.
 * Workers register pending requests before publishing commands, then wait
 * on condition variables for results. The main thread's on_message handler
 * executes callbacks and delivers results to waiting workers.
 *
 * Thread Safety:
 *   - registry_mutex protects the pending request array
 *   - Per-request mutex + condition variable for result delivery
 *
 * Lock Acquisition Order (MUST be followed to prevent deadlocks):
 *   1. registry_mutex (outermost, brief hold for slot operations)
 *   2. req->mutex (can be held longer during wait/signal)
 *   NEVER acquire registry_mutex while holding req->mutex
 *
 * Ownership:
 *   - command_router_deliver() copies result via strdup() (caller retains ownership)
 *   - command_router_wait() transfers ownership to caller (caller must free)
 */

#ifndef COMMAND_ROUTER_H
#define COMMAND_ROUTER_H

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_PENDING_REQUESTS 16
#define COMMAND_RESULT_TIMEOUT_MS 5000
#define REQUEST_ID_MAX_LEN 48

/**
 * @brief Pending request entry for worker waiting on command result
 *
 * @ownership Command router owns all pending requests
 * @thread_safety Protected by registry_mutex for lifecycle, req->mutex for result
 */
typedef struct {
   char request_id[REQUEST_ID_MAX_LEN];  // "worker_<id>_<sequence>"
   int worker_id;                        // Worker that owns this request
   pthread_mutex_t mutex;                // Protects result + flags
   pthread_cond_t result_ready;          // Signaled when result available
   char *result;                         // Callback result (set by main thread)
   bool in_use;                          // Slot is active
   bool completed;                       // Result ready flag
   bool timed_out;                       // Timeout flag
} pending_request_t;

// =============================================================================
// Lifecycle
// =============================================================================

/**
 * @brief Initialize command router (call at startup)
 *
 * @return 0 on success, 1 on failure
 */
int command_router_init(void);

/**
 * @brief Shutdown command router and cleanup resources
 */
void command_router_shutdown(void);

// =============================================================================
// Worker API (called from worker threads)
// =============================================================================

/**
 * @brief Register a pending request (called by worker before MQTT publish)
 *
 * Allocates a slot in the pending request registry and generates a unique
 * request_id. The worker must call command_router_wait() or
 * command_router_cancel() after this.
 *
 * @param worker_id Worker ID for request_id generation
 * @return Allocated pending_request_t, or NULL if registry full
 *
 * @note Thread-safe: acquires registry_mutex briefly
 * @note Caller owns the returned request until wait/cancel
 */
pending_request_t *command_router_register(int worker_id);

/**
 * @brief Get the request_id string for a pending request
 *
 * @param req Pending request from command_router_register()
 * @return Request ID string (valid until request is released)
 */
const char *command_router_get_id(pending_request_t *req);

/**
 * @brief Wait for command result with timeout
 *
 * Blocks until the main thread delivers a result or timeout expires.
 * Automatically unregisters the request after return.
 *
 * @param req Pending request from command_router_register()
 * @param timeout_ms Timeout in milliseconds (use COMMAND_RESULT_TIMEOUT_MS)
 * @return Result string (caller must free), or NULL on timeout/error
 *
 * @note Thread-safe: acquires req->mutex during wait
 * @note After return, req is invalid (slot released)
 */
char *command_router_wait(pending_request_t *req, int timeout_ms);

/**
 * @brief Cancel a pending request (called on worker disconnect)
 *
 * Releases the request slot without waiting for a result.
 * Use when worker needs to abort before result arrives.
 *
 * @param req Pending request to cancel
 *
 * @note Thread-safe
 * @note After return, req is invalid (slot released)
 */
void command_router_cancel(pending_request_t *req);

/**
 * @brief Cancel all pending requests for a worker (called on shutdown)
 *
 * Wakes up any waiting threads and releases all slots for the worker.
 *
 * @param worker_id Worker ID to cancel requests for
 */
void command_router_cancel_all_for_worker(int worker_id);

// =============================================================================
// Main Thread API (called from on_message handler)
// =============================================================================

/**
 * @brief Route a command result to waiting worker (called by main thread)
 *
 * Finds the pending request by ID and delivers the result. The worker's
 * command_router_wait() will return with this result.
 *
 * @param request_id Request ID from command JSON
 * @param result Result string from callback (will be strdup'd)
 * @return true if request found and signaled, false otherwise
 *
 * @note Thread-safe: acquires registry_mutex + req->mutex
 * @note Result is copied; caller retains ownership of input
 */
bool command_router_deliver(const char *request_id, const char *result);

// =============================================================================
// Metrics
// =============================================================================

/**
 * @brief Get number of active pending requests
 *
 * @return Number of slots currently in use
 */
int command_router_active_count(void);

#ifdef __cplusplus
}
#endif

#endif  // COMMAND_ROUTER_H
