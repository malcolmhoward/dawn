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
 * Admin socket implementation for dawn-admin CLI communication.
 */

#define _GNU_SOURCE

#include "auth/admin_socket.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "auth/auth_crypto.h"
#include "auth/auth_db.h"
#include "logging.h"

/* =============================================================================
 * Module State
 * =============================================================================
 */

static pthread_t s_listener_thread;
static volatile bool s_thread_running = false;
static volatile bool s_shutdown_requested = false;

static int s_listen_fd = -1;
static int s_shutdown_pipe[2] = { -1, -1 };

/* Setup token state */
static struct {
   char token[SETUP_TOKEN_LENGTH];
   time_t generated_at;
   time_t expires_at;
   int failed_attempts;
   bool used;
   bool valid;
} s_token_state = { 0 };

static pthread_mutex_t s_token_mutex = PTHREAD_MUTEX_INITIALIZER;

/* =============================================================================
 * Forward Declarations
 * =============================================================================
 */

static void *listener_thread_func(void *arg);
static int create_listening_socket(void);
static int generate_setup_token(void);
static int load_lockout_state(void);
static int save_lockout_state(void);
static int validate_peer_credentials(int client_fd);
static int handle_client(int client_fd);
static int handle_ping(int client_fd);
static int handle_validate_token(int client_fd, const char *payload, uint16_t payload_len);
static int handle_create_user(int client_fd, const char *payload, uint16_t payload_len);
static int send_response(int client_fd, admin_resp_code_t code);
static int secure_compare(const char *a, const char *b, size_t len);

/* =============================================================================
 * Public API
 * =============================================================================
 */

int admin_socket_init(void) {
   if (s_thread_running) {
      LOG_WARNING("Admin socket already running");
      return 0;
   }

   LOG_INFO("Initializing admin socket...");

   /* Load any existing lockout state from previous run */
   load_lockout_state();

   /* Check if admin users already exist - skip token generation if so */
   int user_count = auth_db_user_count();
   bool skip_token = (user_count > 0);

   if (skip_token) {
      LOG_INFO("Admin user(s) exist, skipping setup token generation");
      /* Mark token as invalid so validation fails */
      pthread_mutex_lock(&s_token_mutex);
      s_token_state.valid = false;
      pthread_mutex_unlock(&s_token_mutex);
   } else {
      /* Generate setup token using getrandom() - no fallback */
      if (generate_setup_token() != 0) {
         LOG_ERROR("Failed to generate setup token - entropy failure");
         return 1;
      }
   }

   /* Create shutdown pipe for reliable signal delivery */
   if (pipe(s_shutdown_pipe) != 0) {
      LOG_ERROR("Failed to create shutdown pipe: %s", strerror(errno));
      return 1;
   }

   /* Create listening socket (abstract namespace on Linux) */
   s_listen_fd = create_listening_socket();
   if (s_listen_fd < 0) {
      LOG_ERROR("Failed to create admin socket");
      close(s_shutdown_pipe[0]);
      close(s_shutdown_pipe[1]);
      s_shutdown_pipe[0] = s_shutdown_pipe[1] = -1;
      return 1;
   }

   /* Reset state */
   s_shutdown_requested = false;

   /* Start listener thread */
   if (pthread_create(&s_listener_thread, NULL, listener_thread_func, NULL) != 0) {
      LOG_ERROR("Failed to create admin socket listener thread");
      close(s_listen_fd);
      s_listen_fd = -1;
      close(s_shutdown_pipe[0]);
      close(s_shutdown_pipe[1]);
      s_shutdown_pipe[0] = s_shutdown_pipe[1] = -1;
      return 1;
   }

   s_thread_running = true;

   /* Print setup token to stderr only if no admin exists (NEVER to log files) */
   if (!skip_token) {
      fprintf(stderr, "\n");
      fprintf(stderr, "╔═══════════════════════════════════════════════════════════╗\n");
      fprintf(stderr, "║              DAWN FIRST-RUN SETUP TOKEN                   ║\n");
      fprintf(stderr, "╠═══════════════════════════════════════════════════════════╣\n");
      fprintf(stderr, "║                                                           ║\n");
      fprintf(stderr, "║   Token: %s                         ║\n", s_token_state.token);
      fprintf(stderr, "║                                                           ║\n");
      fprintf(stderr, "║   Valid for 5 minutes. Use with:                          ║\n");
      fprintf(stderr, "║   dawn-admin user create <username> --admin               ║\n");
      fprintf(stderr, "║                                                           ║\n");
      fprintf(stderr, "╚═══════════════════════════════════════════════════════════╝\n");
      fprintf(stderr, "\n");
      fflush(stderr);
   }

   LOG_INFO("Admin socket initialized successfully");
   return 0;
}

void admin_socket_shutdown(void) {
   if (!s_thread_running) {
      return;
   }

   LOG_INFO("Stopping admin socket...");

   /* Signal shutdown */
   s_shutdown_requested = true;

   /* Write to shutdown pipe to wake up select() */
   if (s_shutdown_pipe[1] >= 0) {
      char byte = 1;
      ssize_t written = write(s_shutdown_pipe[1], &byte, 1);
      if (written != 1) {
         LOG_WARNING("Shutdown pipe write failed: %s", strerror(errno));
      }
   }

   /* Wait for listener thread with timeout */
#ifdef __GLIBC__
   struct timespec timeout;
   clock_gettime(CLOCK_REALTIME, &timeout);
   timeout.tv_sec += 2;

   int join_result = pthread_timedjoin_np(s_listener_thread, NULL, &timeout);
   if (join_result == ETIMEDOUT) {
      LOG_WARNING("Admin socket thread did not exit in time, cancelling...");
      pthread_cancel(s_listener_thread);
      pthread_join(s_listener_thread, NULL);
   }
#else
   pthread_join(s_listener_thread, NULL);
#endif

   s_thread_running = false;

   /* Close listening socket */
   if (s_listen_fd >= 0) {
      close(s_listen_fd);
      s_listen_fd = -1;
   }

   /* Close shutdown pipe */
   if (s_shutdown_pipe[0] >= 0) {
      close(s_shutdown_pipe[0]);
      s_shutdown_pipe[0] = -1;
   }
   if (s_shutdown_pipe[1] >= 0) {
      close(s_shutdown_pipe[1]);
      s_shutdown_pipe[1] = -1;
   }

   /* Clear sensitive token data */
   pthread_mutex_lock(&s_token_mutex);
   explicit_bzero(s_token_state.token, sizeof(s_token_state.token));
   s_token_state.valid = false;
   pthread_mutex_unlock(&s_token_mutex);

   LOG_INFO("Admin socket stopped");
}

bool admin_socket_is_running(void) {
   return s_thread_running;
}

int admin_socket_get_setup_token(char *buf, size_t buflen) {
   if (!buf || buflen < SETUP_TOKEN_LENGTH) {
      return 1;
   }

   pthread_mutex_lock(&s_token_mutex);
   if (!s_token_state.valid) {
      pthread_mutex_unlock(&s_token_mutex);
      return 1;
   }
   memcpy(buf, s_token_state.token, SETUP_TOKEN_LENGTH);
   pthread_mutex_unlock(&s_token_mutex);

   return 0;
}

/* =============================================================================
 * Socket Creation
 * =============================================================================
 */

static int create_listening_socket(void) {
   int fd = socket(AF_UNIX, SOCK_STREAM, 0);
   if (fd < 0) {
      LOG_ERROR("Failed to create Unix socket: %s", strerror(errno));
      return -1;
   }

   struct sockaddr_un addr;
   memset(&addr, 0, sizeof(addr));
   addr.sun_family = AF_UNIX;

   /* Use abstract socket namespace (Linux-specific) */
   /* First byte is null, followed by the name */
   addr.sun_path[0] = '\0';
   strncpy(addr.sun_path + 1, ADMIN_SOCKET_ABSTRACT_NAME, sizeof(addr.sun_path) - 2);

   /* Calculate address length for abstract socket */
   socklen_t addr_len = offsetof(struct sockaddr_un, sun_path) + 1 +
                        strlen(ADMIN_SOCKET_ABSTRACT_NAME);

   if (bind(fd, (struct sockaddr *)&addr, addr_len) != 0) {
      LOG_ERROR("Failed to bind admin socket: %s", strerror(errno));
      close(fd);
      return -1;
   }

   /* Only allow one connection at a time */
   if (listen(fd, ADMIN_MAX_CONNECTIONS) != 0) {
      LOG_ERROR("Failed to listen on admin socket: %s", strerror(errno));
      close(fd);
      return -1;
   }

   LOG_INFO("Admin socket listening (abstract: @%s)", ADMIN_SOCKET_ABSTRACT_NAME);
   return fd;
}

/* =============================================================================
 * Setup Token Generation
 * =============================================================================
 */

static int generate_setup_token(void) {
   unsigned char entropy[SETUP_TOKEN_RANDOM_CHARS];

   /* Use getrandom() with no fallback - fail closed on entropy failure */
   ssize_t ret = getrandom(entropy, sizeof(entropy), 0);
   if (ret != sizeof(entropy)) {
      LOG_ERROR("getrandom() failed: %s (got %zd bytes, expected %zu)", strerror(errno), ret,
                sizeof(entropy));
      return 1;
   }

   pthread_mutex_lock(&s_token_mutex);

   /* Format: DAWN-XXXX-XXXX-XXXX-XXXX */
   char *p = s_token_state.token;
   memcpy(p, "DAWN-", 5);
   p += 5;

   for (int i = 0; i < SETUP_TOKEN_RANDOM_CHARS; i++) {
      if (i > 0 && i % 4 == 0) {
         *p++ = '-';
      }
      *p++ = SETUP_TOKEN_CHARSET[entropy[i] % SETUP_TOKEN_CHARSET_LEN];
   }
   *p = '\0';

   /* Set validity period */
   s_token_state.generated_at = time(NULL);
   s_token_state.expires_at = s_token_state.generated_at + SETUP_TOKEN_VALIDITY_SEC;
   s_token_state.used = false;
   s_token_state.valid = true;

   /* Don't reset failed_attempts here - loaded from persistent state */

   pthread_mutex_unlock(&s_token_mutex);

   /* Clear entropy from stack */
   explicit_bzero(entropy, sizeof(entropy));

   return 0;
}

/* =============================================================================
 * Rate Limit Persistence
 * =============================================================================
 */

typedef struct __attribute__((packed)) {
   uint8_t attempt_count;
   time_t first_attempt_time;
   time_t lockout_until;
} lockout_state_file_t;

static int load_lockout_state(void) {
   int fd = open(SETUP_TOKEN_LOCKOUT_FILE, O_RDONLY);
   if (fd < 0) {
      /* No prior state - start fresh */
      s_token_state.failed_attempts = 0;
      return 0;
   }

   lockout_state_file_t state;
   ssize_t n = read(fd, &state, sizeof(state));
   close(fd);

   if (n != sizeof(state)) {
      /* Corrupted, reset */
      s_token_state.failed_attempts = 0;
      unlink(SETUP_TOKEN_LOCKOUT_FILE);
      return 0;
   }

   /* Check if lockout has expired */
   time_t now = time(NULL);
   if (state.lockout_until > 0 && now >= state.lockout_until) {
      /* Lockout expired, reset */
      s_token_state.failed_attempts = 0;
      unlink(SETUP_TOKEN_LOCKOUT_FILE);
      return 0;
   }

   s_token_state.failed_attempts = state.attempt_count;
   LOG_INFO("Loaded lockout state: %d failed attempts", s_token_state.failed_attempts);
   return 0;
}

static int save_lockout_state(void) {
   /* Create directory if needed */
   mkdir(ADMIN_SOCKET_DIR, 0700);

   int fd = open(SETUP_TOKEN_LOCKOUT_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0600);
   if (fd < 0) {
      LOG_WARNING("Failed to save lockout state: %s", strerror(errno));
      return 1;
   }

   lockout_state_file_t state = { 0 };
   state.attempt_count = (uint8_t)s_token_state.failed_attempts;
   state.first_attempt_time = time(NULL);
   if (s_token_state.failed_attempts >= SETUP_TOKEN_MAX_ATTEMPTS) {
      /* Lockout for 1 hour */
      state.lockout_until = state.first_attempt_time + 3600;
   }

   ssize_t written = write(fd, &state, sizeof(state));
   close(fd);

   if (written != sizeof(state)) {
      LOG_WARNING("Failed to write complete lockout state");
      return 1;
   }

   return 0;
}

/* =============================================================================
 * Peer Credential Validation
 * =============================================================================
 */

static int validate_peer_credentials(int client_fd) {
   struct ucred cred;
   socklen_t len = sizeof(cred);

   if (getsockopt(client_fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0) {
      LOG_ERROR("Failed to get peer credentials: %s", strerror(errno));
      return 1;
   }

   /* Allow only root (uid 0) or the daemon's own user */
   uid_t daemon_uid = getuid();

   if (cred.uid != 0 && cred.uid != daemon_uid) {
      LOG_WARNING("Admin socket connection rejected: unauthorized UID %d (expected 0 or %d)",
                  cred.uid, daemon_uid);
      return 1;
   }

   LOG_INFO("Admin socket client authenticated: uid=%d, pid=%d", cred.uid, cred.pid);
   return 0;
}

/* =============================================================================
 * Constant-Time Comparison
 * =============================================================================
 */

static int secure_compare(const char *a, const char *b, size_t len) {
   volatile unsigned char result = 0;
   volatile const unsigned char *va = (volatile const unsigned char *)a;
   volatile const unsigned char *vb = (volatile const unsigned char *)b;

   for (size_t i = 0; i < len; i++) {
      result |= va[i] ^ vb[i];
   }

   return result;
}

/* =============================================================================
 * Message Handlers
 * =============================================================================
 */

static int send_response(int client_fd, admin_resp_code_t code) {
   admin_msg_response_t resp = { 0 };
   resp.version = ADMIN_PROTOCOL_VERSION;
   resp.response_code = (uint8_t)code;
   resp.reserved = 0;

   ssize_t sent = write(client_fd, &resp, sizeof(resp));
   return (sent == sizeof(resp)) ? 0 : 1;
}

static int handle_ping(int client_fd) {
   return send_response(client_fd, ADMIN_RESP_SUCCESS);
}

static int handle_validate_token(int client_fd, const char *payload, uint16_t payload_len) {
   pthread_mutex_lock(&s_token_mutex);

   /* Check rate limit first */
   if (s_token_state.failed_attempts >= SETUP_TOKEN_MAX_ATTEMPTS) {
      pthread_mutex_unlock(&s_token_mutex);
      LOG_WARNING("Setup token rate limited");
      return send_response(client_fd, ADMIN_RESP_RATE_LIMITED);
   }

   /* Check if token is still valid */
   if (!s_token_state.valid) {
      pthread_mutex_unlock(&s_token_mutex);
      LOG_WARNING("Setup token not available");
      return send_response(client_fd, ADMIN_RESP_FAILURE);
   }

   /* Check expiry */
   time_t now = time(NULL);
   if (now > s_token_state.expires_at) {
      pthread_mutex_unlock(&s_token_mutex);
      LOG_WARNING("Setup token expired");
      return send_response(client_fd, ADMIN_RESP_FAILURE);
   }

   /* Check if already used */
   if (s_token_state.used) {
      pthread_mutex_unlock(&s_token_mutex);
      LOG_INFO("Setup token already used");
      return send_response(client_fd, ADMIN_RESP_FAILURE);
   }

   /* Verify payload length matches expected token length */
   size_t expected_len = strlen(s_token_state.token);
   if (payload_len != expected_len) {
      s_token_state.failed_attempts++;
      save_lockout_state();
      pthread_mutex_unlock(&s_token_mutex);
      LOG_WARNING("Invalid token length: %u (expected %zu)", payload_len, expected_len);
      return send_response(client_fd, ADMIN_RESP_FAILURE);
   }

   /* Constant-time comparison */
   if (secure_compare(payload, s_token_state.token, expected_len) != 0) {
      s_token_state.failed_attempts++;
      save_lockout_state();
      pthread_mutex_unlock(&s_token_mutex);
      LOG_WARNING("Invalid setup token attempt (%d/%d)", s_token_state.failed_attempts,
                  SETUP_TOKEN_MAX_ATTEMPTS);
      return send_response(client_fd, ADMIN_RESP_FAILURE);
   }

   /* Token valid - mark as used */
   s_token_state.used = true;
   pthread_mutex_unlock(&s_token_mutex);

   LOG_INFO("Setup token validated successfully");
   return send_response(client_fd, ADMIN_RESP_SUCCESS);
}

/**
 * @brief Handle ADMIN_MSG_CREATE_USER - atomic token validation + user creation.
 *
 * This combines token validation and user creation into a single atomic operation
 * to prevent the TOCTOU race condition identified in security review.
 *
 * The token is only marked as used after successful user creation.
 */
static int handle_create_user(int client_fd, const char *payload, uint16_t payload_len) {
   /* Minimum payload size: header struct */
   if (payload_len < sizeof(admin_create_user_payload_t)) {
      LOG_WARNING("CREATE_USER payload too small: %u", payload_len);
      return send_response(client_fd, ADMIN_RESP_FAILURE);
   }

   const admin_create_user_payload_t *hdr = (const admin_create_user_payload_t *)payload;

   /* Validate lengths */
   if (hdr->username_len == 0 || hdr->username_len > ADMIN_USERNAME_MAX_LEN) {
      LOG_WARNING("CREATE_USER invalid username length: %u", hdr->username_len);
      return send_response(client_fd, ADMIN_RESP_FAILURE);
   }

   if (hdr->password_len < ADMIN_PASSWORD_MIN_LEN || hdr->password_len > ADMIN_PASSWORD_MAX_LEN) {
      LOG_WARNING("CREATE_USER invalid password length: %u", hdr->password_len);
      return send_response(client_fd, ADMIN_RESP_FAILURE);
   }

   /* Verify total payload size matches header + strings */
   size_t expected_len = sizeof(admin_create_user_payload_t) + hdr->username_len +
                         hdr->password_len;
   if (payload_len != expected_len) {
      LOG_WARNING("CREATE_USER payload size mismatch: got %u, expected %zu", payload_len,
                  expected_len);
      return send_response(client_fd, ADMIN_RESP_FAILURE);
   }

   /* Extract username and password with explicit null termination */
   char username[ADMIN_USERNAME_MAX_LEN + 1] = { 0 };
   char password[ADMIN_PASSWORD_MAX_LEN + 1] = { 0 };

   const char *username_ptr = payload + sizeof(admin_create_user_payload_t);
   const char *password_ptr = username_ptr + hdr->username_len;

   memcpy(username, username_ptr, hdr->username_len);
   memcpy(password, password_ptr, hdr->password_len);

   bool is_admin = (hdr->is_admin != 0);

   /* Validate setup token first (under mutex) */
   pthread_mutex_lock(&s_token_mutex);

   /* Check rate limit */
   if (s_token_state.failed_attempts >= SETUP_TOKEN_MAX_ATTEMPTS) {
      pthread_mutex_unlock(&s_token_mutex);
      auth_secure_zero(password, sizeof(password));
      LOG_WARNING("CREATE_USER rate limited");
      return send_response(client_fd, ADMIN_RESP_RATE_LIMITED);
   }

   /* Check if token is valid */
   if (!s_token_state.valid) {
      pthread_mutex_unlock(&s_token_mutex);
      auth_secure_zero(password, sizeof(password));
      LOG_WARNING("CREATE_USER: setup token not available");
      return send_response(client_fd, ADMIN_RESP_FAILURE);
   }

   /* Check expiry */
   time_t now = time(NULL);
   if (now > s_token_state.expires_at) {
      pthread_mutex_unlock(&s_token_mutex);
      auth_secure_zero(password, sizeof(password));
      LOG_WARNING("CREATE_USER: setup token expired");
      return send_response(client_fd, ADMIN_RESP_FAILURE);
   }

   /* Check if already used */
   if (s_token_state.used) {
      pthread_mutex_unlock(&s_token_mutex);
      auth_secure_zero(password, sizeof(password));
      LOG_INFO("CREATE_USER: setup token already used");
      return send_response(client_fd, ADMIN_RESP_FAILURE);
   }

   /* Null-terminate the token from payload for comparison */
   char provided_token[SETUP_TOKEN_LENGTH] = { 0 };
   memcpy(provided_token, hdr->setup_token, SETUP_TOKEN_LENGTH - 1);

   /* Constant-time token comparison */
   size_t token_len = strlen(s_token_state.token);
   if (secure_compare(provided_token, s_token_state.token, token_len) != 0) {
      s_token_state.failed_attempts++;
      save_lockout_state();
      pthread_mutex_unlock(&s_token_mutex);
      auth_secure_zero(password, sizeof(password));
      LOG_WARNING("CREATE_USER: invalid token (%d/%d)", s_token_state.failed_attempts,
                  SETUP_TOKEN_MAX_ATTEMPTS);
      return send_response(client_fd, ADMIN_RESP_FAILURE);
   }

   /* Token is valid - now create the user */
   /* Hash password */
   char password_hash[AUTH_HASH_LEN];
   int hash_result = auth_hash_password(password, password_hash);

   /* Clear password from memory immediately */
   auth_secure_zero(password, sizeof(password));

   if (hash_result != AUTH_CRYPTO_SUCCESS) {
      pthread_mutex_unlock(&s_token_mutex);
      LOG_ERROR("CREATE_USER: password hashing failed: %d", hash_result);
      return send_response(client_fd, ADMIN_RESP_SERVICE_ERROR);
   }

   /* Create user in database */
   int db_result = auth_db_create_user(username, password_hash, is_admin);

   /* Clear hash from memory */
   auth_secure_zero(password_hash, sizeof(password_hash));

   if (db_result != AUTH_DB_SUCCESS) {
      pthread_mutex_unlock(&s_token_mutex);
      if (db_result == AUTH_DB_DUPLICATE) {
         LOG_WARNING("CREATE_USER: username already exists: %s", username);
      } else {
         LOG_ERROR("CREATE_USER: database error: %d", db_result);
      }
      return send_response(client_fd, ADMIN_RESP_FAILURE);
   }

   /* User created successfully - NOW mark token as used */
   s_token_state.used = true;

   /* Log the event */
   auth_db_log_event("USER_CREATED", username, NULL,
                     is_admin ? "{\"is_admin\":true}" : "{\"is_admin\":false}");

   pthread_mutex_unlock(&s_token_mutex);

   LOG_INFO("CREATE_USER: user '%s' created successfully (admin=%d)", username, is_admin);
   return send_response(client_fd, ADMIN_RESP_SUCCESS);
}

/* =============================================================================
 * Client Handler
 * =============================================================================
 */

static int handle_client(int client_fd) {
   /* Set receive timeout */
   struct timeval tv;
   tv.tv_sec = ADMIN_CONN_TIMEOUT_SEC;
   tv.tv_usec = 0;
   setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

   /* Validate peer credentials */
   if (validate_peer_credentials(client_fd) != 0) {
      send_response(client_fd, ADMIN_RESP_UNAUTHORIZED);
      return 1;
   }

   /* Read message header */
   admin_msg_header_t header;
   ssize_t n = read(client_fd, &header, sizeof(header));
   if (n != sizeof(header)) {
      LOG_WARNING("Failed to read admin message header");
      return 1;
   }

   /* Validate protocol version */
   if (header.version != ADMIN_PROTOCOL_VERSION) {
      LOG_WARNING("Protocol version mismatch: got 0x%02x, expected 0x%02x", header.version,
                  ADMIN_PROTOCOL_VERSION);
      send_response(client_fd, ADMIN_RESP_VERSION_MISMATCH);
      return 1;
   }

   /* Validate payload length */
   if (header.payload_len > ADMIN_MSG_MAX_PAYLOAD) {
      LOG_WARNING("Payload too large: %u (max %d)", header.payload_len, ADMIN_MSG_MAX_PAYLOAD);
      send_response(client_fd, ADMIN_RESP_FAILURE);
      return 1;
   }

   /* Read payload if present */
   char payload[ADMIN_MSG_MAX_PAYLOAD + 1] = { 0 };
   if (header.payload_len > 0) {
      n = read(client_fd, payload, header.payload_len);
      if (n != header.payload_len) {
         LOG_WARNING("Failed to read payload: got %zd, expected %u", n, header.payload_len);
         return 1;
      }
   }

   /* Dispatch by message type */
   switch (header.msg_type) {
      case ADMIN_MSG_PING:
         return handle_ping(client_fd);

      case ADMIN_MSG_VALIDATE_SETUP_TOKEN:
         return handle_validate_token(client_fd, payload, header.payload_len);

      case ADMIN_MSG_CREATE_USER:
         return handle_create_user(client_fd, payload, header.payload_len);

      default:
         LOG_WARNING("Unknown message type: 0x%02x", header.msg_type);
         send_response(client_fd, ADMIN_RESP_FAILURE);
         return 1;
   }
}

/* =============================================================================
 * Listener Thread
 * =============================================================================
 */

static void *listener_thread_func(void *arg) {
   (void)arg;

   LOG_INFO("Admin socket listener thread running");

   pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
   pthread_setcanceltype(PTHREAD_CANCEL_DEFERRED, NULL);

   while (!s_shutdown_requested) {
      pthread_testcancel();

      int fd = s_listen_fd;
      int pipe_fd = s_shutdown_pipe[0];

      if (fd < 0 || pipe_fd < 0 || s_shutdown_requested) {
         break;
      }

      fd_set read_fds;
      FD_ZERO(&read_fds);
      FD_SET(fd, &read_fds);
      FD_SET(pipe_fd, &read_fds);

      int max_fd = (fd > pipe_fd) ? fd : pipe_fd;

      struct timeval timeout;
      timeout.tv_sec = 60;
      timeout.tv_usec = 0;

      int result = select(max_fd + 1, &read_fds, NULL, NULL, &timeout);

      if (result < 0) {
         if (errno == EINTR || errno == EBADF || s_shutdown_requested) {
            continue;
         }
         LOG_ERROR("Admin socket select() failed: %s", strerror(errno));
         break;
      }

      if (result == 0) {
         /* Timeout - just continue */
         continue;
      }

      /* Check shutdown pipe */
      if (FD_ISSET(pipe_fd, &read_fds)) {
         LOG_INFO("Admin socket received shutdown signal");
         break;
      }

      /* Accept connection */
      if (FD_ISSET(fd, &read_fds)) {
         struct sockaddr_un client_addr;
         socklen_t addr_len = sizeof(client_addr);

         int client_fd = accept(fd, (struct sockaddr *)&client_addr, &addr_len);
         if (client_fd < 0) {
            if (errno == EINTR || s_shutdown_requested) {
               continue;
            }
            LOG_ERROR("Admin socket accept failed: %s", strerror(errno));
            continue;
         }

         /* Handle client (single-threaded, one at a time) */
         handle_client(client_fd);
         close(client_fd);
      }
   }

   LOG_INFO("Admin socket listener thread exiting");
   return NULL;
}
