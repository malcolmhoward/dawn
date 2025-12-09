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
 * Implementation of the pending request registry and result delivery system.
 *
 * THREAD SAFETY:
 *   - registry_mutex protects the pending_requests array
 *   - Per-request mutex protects result delivery
 *   - Lock order: registry_mutex -> req->mutex (never reverse)
 *
 * CRITICAL DESIGN NOTES:
 *   - command_router_deliver() holds registry_mutex throughout delivery
 *     to prevent race between find and signal
 *   - Workers never hold registry_mutex while waiting on condition variable
 */

#include "core/command_router.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "logging.h"

// =============================================================================
// Time Conversion Constants
// =============================================================================

#define MS_TO_NS 1000000       // Milliseconds to nanoseconds
#define NS_PER_SEC 1000000000  // Nanoseconds per second

// =============================================================================
// Module State
// =============================================================================

static pending_request_t pending_requests[MAX_PENDING_REQUESTS];
static pthread_mutex_t registry_mutex = PTHREAD_MUTEX_INITIALIZER;
static atomic_uint_fast64_t request_sequence = 0;
static bool router_initialized = false;

// =============================================================================
// Lifecycle
// =============================================================================

int command_router_init(void) {
   if (router_initialized) {
      LOG_WARNING("Command router already initialized");
      return 0;
   }

   pthread_mutex_lock(&registry_mutex);

   // Initialize all slots
   for (int i = 0; i < MAX_PENDING_REQUESTS; i++) {
      pending_request_t *req = &pending_requests[i];

      memset(req->request_id, 0, sizeof(req->request_id));
      req->worker_id = -1;
      req->result = NULL;
      req->in_use = false;
      req->completed = false;
      req->timed_out = false;

      if (pthread_mutex_init(&req->mutex, NULL) != 0) {
         LOG_ERROR("Command router: Failed to init mutex for slot %d", i);
         pthread_mutex_unlock(&registry_mutex);
         return 1;
      }

      if (pthread_cond_init(&req->result_ready, NULL) != 0) {
         LOG_ERROR("Command router: Failed to init cond for slot %d", i);
         pthread_mutex_destroy(&req->mutex);
         pthread_mutex_unlock(&registry_mutex);
         return 1;
      }
   }

   router_initialized = true;
   pthread_mutex_unlock(&registry_mutex);

   LOG_INFO("Command router initialized with %d slots", MAX_PENDING_REQUESTS);
   return 0;
}

void command_router_shutdown(void) {
   if (!router_initialized) {
      return;
   }

   pthread_mutex_lock(&registry_mutex);

   // Wake up any waiting threads and clean up
   for (int i = 0; i < MAX_PENDING_REQUESTS; i++) {
      pending_request_t *req = &pending_requests[i];

      if (req->in_use) {
         pthread_mutex_lock(&req->mutex);
         req->timed_out = true;
         pthread_cond_broadcast(&req->result_ready);
         pthread_mutex_unlock(&req->mutex);
      }

      // Free any pending results (free-and-NULL pattern per coding standards)
      if (req->result) {
         free(req->result);
      }
      req->result = NULL;

      pthread_cond_destroy(&req->result_ready);
      pthread_mutex_destroy(&req->mutex);
   }

   router_initialized = false;
   pthread_mutex_unlock(&registry_mutex);

   LOG_INFO("Command router shutdown complete");
}

// =============================================================================
// Worker API
// =============================================================================

pending_request_t *command_router_register(int worker_id) {
   if (!router_initialized) {
      LOG_ERROR("Command router not initialized");
      return NULL;
   }

   pthread_mutex_lock(&registry_mutex);

   // Find a free slot
   pending_request_t *req = NULL;
   for (int i = 0; i < MAX_PENDING_REQUESTS; i++) {
      if (!pending_requests[i].in_use) {
         req = &pending_requests[i];
         break;
      }
   }

   if (!req) {
      pthread_mutex_unlock(&registry_mutex);
      LOG_WARNING("Command router: Registry full, cannot register request");
      return NULL;
   }

   // Generate unique request ID
   uint64_t seq = atomic_fetch_add(&request_sequence, 1);
   snprintf(req->request_id, sizeof(req->request_id), "worker_%d_%llu", worker_id,
            (unsigned long long)seq);

   // Initialize slot
   req->worker_id = worker_id;
   req->result = NULL;
   req->in_use = true;
   req->completed = false;
   req->timed_out = false;

   pthread_mutex_unlock(&registry_mutex);

   LOG_INFO("Command router: Registered request %s", req->request_id);
   return req;
}

const char *command_router_get_id(pending_request_t *req) {
   if (!req) {
      return NULL;
   }
   return req->request_id;
}

char *command_router_wait(pending_request_t *req, int timeout_ms) {
   if (!req || !router_initialized) {
      return NULL;
   }

   char *result = NULL;

   // Calculate absolute timeout
   struct timespec abstime;
   clock_gettime(CLOCK_REALTIME, &abstime);
   abstime.tv_sec += timeout_ms / 1000;
   abstime.tv_nsec += (timeout_ms % 1000) * MS_TO_NS;
   if (abstime.tv_nsec >= NS_PER_SEC) {
      abstime.tv_sec += 1;
      abstime.tv_nsec -= NS_PER_SEC;
   }

   // Wait for result (only hold req->mutex, NOT registry_mutex)
   pthread_mutex_lock(&req->mutex);

   while (!req->completed && !req->timed_out) {
      int rc = pthread_cond_timedwait(&req->result_ready, &req->mutex, &abstime);
      if (rc == ETIMEDOUT) {
         req->timed_out = true;
         LOG_WARNING("Command router: Request %s timed out after %dms", req->request_id,
                     timeout_ms);
         break;
      } else if (rc != 0 && rc != EINTR) {
         LOG_ERROR("Command router: pthread_cond_timedwait failed: %d", rc);
         req->timed_out = true;
         break;
      }
   }

   // Get result if available
   if (req->completed && req->result) {
      result = req->result;
      req->result = NULL;  // Transfer ownership to caller
      LOG_INFO("Command router: Request %s completed with result", req->request_id);
   }

   pthread_mutex_unlock(&req->mutex);

   // Unregister the slot (acquire registry_mutex)
   pthread_mutex_lock(&registry_mutex);
   req->in_use = false;
   req->worker_id = -1;
   memset(req->request_id, 0, sizeof(req->request_id));
   pthread_mutex_unlock(&registry_mutex);

   return result;
}

void command_router_cancel(pending_request_t *req) {
   if (!req || !router_initialized) {
      return;
   }

   // Mark as timed out to prevent future delivery
   pthread_mutex_lock(&req->mutex);
   req->timed_out = true;

   // Free any pending result
   if (req->result) {
      free(req->result);
      req->result = NULL;
   }
   pthread_mutex_unlock(&req->mutex);

   // Unregister the slot
   pthread_mutex_lock(&registry_mutex);
   req->in_use = false;
   req->worker_id = -1;
   memset(req->request_id, 0, sizeof(req->request_id));
   pthread_mutex_unlock(&registry_mutex);

   LOG_INFO("Command router: Request cancelled");
}

void command_router_cancel_all_for_worker(int worker_id) {
   if (!router_initialized) {
      return;
   }

   pthread_mutex_lock(&registry_mutex);

   for (int i = 0; i < MAX_PENDING_REQUESTS; i++) {
      pending_request_t *req = &pending_requests[i];

      if (req->in_use && req->worker_id == worker_id) {
         pthread_mutex_lock(&req->mutex);

         req->timed_out = true;
         pthread_cond_broadcast(&req->result_ready);

         if (req->result) {
            free(req->result);
            req->result = NULL;
         }

         pthread_mutex_unlock(&req->mutex);

         req->in_use = false;
         req->worker_id = -1;
         memset(req->request_id, 0, sizeof(req->request_id));

         LOG_INFO("Command router: Cancelled request for worker %d", worker_id);
      }
   }

   pthread_mutex_unlock(&registry_mutex);
}

// =============================================================================
// Main Thread API
// =============================================================================

bool command_router_deliver(const char *request_id, const char *result) {
   if (!request_id || !router_initialized) {
      return false;
   }

   // Hold registry_mutex throughout delivery to prevent race condition
   // between finding the request and signaling it
   pthread_mutex_lock(&registry_mutex);

   // Find request by ID
   pending_request_t *req = NULL;
   for (int i = 0; i < MAX_PENDING_REQUESTS; i++) {
      if (pending_requests[i].in_use && strcmp(pending_requests[i].request_id, request_id) == 0) {
         req = &pending_requests[i];
         break;
      }
   }

   if (!req) {
      pthread_mutex_unlock(&registry_mutex);
      LOG_WARNING("Command router: Request %s not found (timed out or never existed)", request_id);
      return false;
   }

   // Acquire req->mutex while still holding registry_mutex
   // This prevents the worker from unregistering between find and signal
   pthread_mutex_lock(&req->mutex);

   if (!req->timed_out) {
      // Deliver result
      if (result) {
         req->result = strdup(result);
         if (!req->result) {
            LOG_ERROR("Command router: Failed to strdup result");
         }
      }
      req->completed = true;
      pthread_cond_signal(&req->result_ready);
      LOG_INFO("Command router: Delivered result for request %s", request_id);
   } else {
      // Worker already timed out, discard result
      LOG_WARNING("Command router: Request %s already timed out, discarding result", request_id);
   }

   pthread_mutex_unlock(&req->mutex);
   pthread_mutex_unlock(&registry_mutex);

   return true;
}

// =============================================================================
// Metrics
// =============================================================================

int command_router_active_count(void) {
   if (!router_initialized) {
      return 0;
   }

   int count = 0;

   pthread_mutex_lock(&registry_mutex);
   for (int i = 0; i < MAX_PENDING_REQUESTS; i++) {
      if (pending_requests[i].in_use) {
         count++;
      }
   }
   pthread_mutex_unlock(&registry_mutex);

   return count;
}
