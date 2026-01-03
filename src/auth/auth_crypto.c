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
 * Cryptographic utilities for authentication system.
 */

#include "auth/auth_crypto.h"

#include <errno.h>
#include <semaphore.h>
#include <string.h>
#include <time.h>

#include "logging.h"

/* Hash semaphore to limit concurrent Argon2id operations */
static sem_t s_hash_semaphore;
static bool s_initialized = false;

/* CSRF secret key - generated at init time, used for HMAC signing */
static unsigned char s_csrf_secret[crypto_auth_KEYBYTES];

/* CSRF token binary structure (before hex encoding) */
#define CSRF_TIMESTAMP_LEN 8           /* uint64_t timestamp */
#define CSRF_NONCE_LEN 16              /* Random nonce */
#define CSRF_MAC_LEN crypto_auth_BYTES /* HMAC tag (32 bytes) */
#define CSRF_BINARY_LEN (CSRF_TIMESTAMP_LEN + CSRF_NONCE_LEN + CSRF_MAC_LEN)

int auth_crypto_init(void) {
   if (s_initialized) {
      LOG_WARNING("auth_crypto_init: already initialized");
      return AUTH_CRYPTO_SUCCESS;
   }

   /* Initialize libsodium */
   if (sodium_init() < 0) {
      LOG_ERROR("auth_crypto_init: sodium_init() failed");
      return AUTH_CRYPTO_FAILURE;
   }

   /* Initialize hash semaphore */
   if (sem_init(&s_hash_semaphore, 0, AUTH_CONCURRENT_HASH_LIMIT) != 0) {
      LOG_ERROR("auth_crypto_init: sem_init() failed: %s", strerror(errno));
      return AUTH_CRYPTO_FAILURE;
   }

   /* Generate CSRF secret key (ephemeral - regenerated each startup) */
   crypto_auth_keygen(s_csrf_secret);

   s_initialized = true;

#ifdef PLATFORM_RPI
   LOG_INFO("auth_crypto_init: initialized (Pi mode: %dMB/%d iterations)",
            AUTH_MEMLIMIT / (1024 * 1024), AUTH_OPSLIMIT);
#else
   LOG_INFO("auth_crypto_init: initialized (Jetson mode: %dMB/%d iterations)",
            AUTH_MEMLIMIT / (1024 * 1024), AUTH_OPSLIMIT);
#endif

   return AUTH_CRYPTO_SUCCESS;
}

void auth_crypto_shutdown(void) {
   if (!s_initialized) {
      return;
   }

   sem_destroy(&s_hash_semaphore);

   /* Clear CSRF secret from memory */
   sodium_memzero(s_csrf_secret, sizeof(s_csrf_secret));

   s_initialized = false;

   LOG_INFO("auth_crypto_shutdown: complete");
}

int auth_hash_password(const char *password, char hash_out[AUTH_HASH_LEN]) {
   if (!s_initialized) {
      LOG_ERROR("auth_hash_password: not initialized");
      return AUTH_CRYPTO_FAILURE;
   }

   if (!password || !hash_out) {
      LOG_ERROR("auth_hash_password: NULL argument");
      return AUTH_CRYPTO_FAILURE;
   }

   /* Clear output buffer immediately */
   sodium_memzero(hash_out, AUTH_HASH_LEN);

   /* Try to acquire hash slot with timeout */
   struct timespec timeout;
   if (clock_gettime(CLOCK_REALTIME, &timeout) != 0) {
      LOG_ERROR("auth_hash_password: clock_gettime() failed");
      return AUTH_CRYPTO_FAILURE;
   }
   timeout.tv_sec += AUTH_HASH_TIMEOUT_SEC;

   if (sem_timedwait(&s_hash_semaphore, &timeout) != 0) {
      if (errno == ETIMEDOUT) {
         LOG_WARNING("auth_hash_password: semaphore timeout - system under load");
         return AUTH_CRYPTO_BUSY;
      }
      LOG_ERROR("auth_hash_password: sem_timedwait() failed: %s", strerror(errno));
      return AUTH_CRYPTO_FAILURE;
   }

   /* Perform the hash operation */
   int result = crypto_pwhash_str(hash_out, password, strlen(password), AUTH_OPSLIMIT,
                                  AUTH_MEMLIMIT);

   /* Release hash slot */
   sem_post(&s_hash_semaphore);

   if (result != 0) {
      LOG_ERROR("auth_hash_password: crypto_pwhash_str() failed (OOM likely)");
      sodium_memzero(hash_out, AUTH_HASH_LEN);
      return AUTH_CRYPTO_OOM;
   }

   return AUTH_CRYPTO_SUCCESS;
}

bool auth_verify_password(const char *stored_hash, const char *password) {
   if (!s_initialized) {
      LOG_ERROR("auth_verify_password: not initialized");
      return false;
   }

   if (!stored_hash || !password) {
      LOG_ERROR("auth_verify_password: NULL argument");
      return false;
   }

   /*
    * crypto_pwhash_str_verify() is constant-time internally.
    * It extracts algorithm, params, and salt from stored_hash
    * and performs the comparison.
    */
   int result = crypto_pwhash_str_verify(stored_hash, password, strlen(password));

   return (result == 0);
}

int auth_generate_token(char token_out[AUTH_TOKEN_LEN]) {
   if (!s_initialized) {
      LOG_ERROR("auth_generate_token: not initialized");
      return AUTH_CRYPTO_FAILURE;
   }

   if (!token_out) {
      LOG_ERROR("auth_generate_token: NULL argument");
      return AUTH_CRYPTO_FAILURE;
   }

   /* Clear output buffer immediately */
   sodium_memzero(token_out, AUTH_TOKEN_LEN);

   /* Generate 32 random bytes (256 bits of entropy) */
   unsigned char random_bytes[32];
   randombytes_buf(random_bytes, sizeof(random_bytes));

   /* Convert to hex string */
   sodium_bin2hex(token_out, AUTH_TOKEN_LEN, random_bytes, sizeof(random_bytes));

   /* Clear random bytes from memory */
   sodium_memzero(random_bytes, sizeof(random_bytes));

   return AUTH_CRYPTO_SUCCESS;
}

bool auth_token_compare(const char *a, const char *b) {
   if (!a || !b) {
      return false;
   }

   size_t len_a = strlen(a);
   size_t len_b = strlen(b);

   /* Both must be exactly token length (64 chars) */
   if (len_a != AUTH_TOKEN_LEN - 1 || len_b != AUTH_TOKEN_LEN - 1) {
      return false;
   }

   /*
    * sodium_memcmp() is constant-time.
    * Returns 0 if equal, -1 if different.
    */
   return (sodium_memcmp(a, b, AUTH_TOKEN_LEN - 1) == 0);
}

void auth_secure_zero(void *buf, size_t len) {
   if (buf && len > 0) {
      sodium_memzero(buf, len);
   }
}

int auth_generate_csrf_token(char token_out[AUTH_CSRF_TOKEN_LEN]) {
   if (!s_initialized) {
      LOG_ERROR("auth_generate_csrf_token: not initialized");
      return AUTH_CRYPTO_FAILURE;
   }

   if (!token_out) {
      LOG_ERROR("auth_generate_csrf_token: NULL argument");
      return AUTH_CRYPTO_FAILURE;
   }

   /* Clear output buffer */
   sodium_memzero(token_out, AUTH_CSRF_TOKEN_LEN);

   /* Build binary token: timestamp || nonce */
   unsigned char msg[CSRF_TIMESTAMP_LEN + CSRF_NONCE_LEN];
   uint64_t timestamp = (uint64_t)time(NULL);

   /* Store timestamp in network byte order (big-endian) */
   for (int i = 7; i >= 0; i--) {
      msg[i] = (unsigned char)(timestamp & 0xFF);
      timestamp >>= 8;
   }

   /* Generate random nonce */
   randombytes_buf(msg + CSRF_TIMESTAMP_LEN, CSRF_NONCE_LEN);

   /* Compute HMAC */
   unsigned char mac[CSRF_MAC_LEN];
   crypto_auth(mac, msg, sizeof(msg), s_csrf_secret);

   /* Build full binary token: timestamp || nonce || mac */
   unsigned char binary_token[CSRF_BINARY_LEN];
   memcpy(binary_token, msg, sizeof(msg));
   memcpy(binary_token + sizeof(msg), mac, sizeof(mac));

   /* Convert to hex string */
   sodium_bin2hex(token_out, AUTH_CSRF_TOKEN_LEN, binary_token, sizeof(binary_token));

   /* Clear sensitive data */
   sodium_memzero(msg, sizeof(msg));
   sodium_memzero(mac, sizeof(mac));
   sodium_memzero(binary_token, sizeof(binary_token));

   return AUTH_CRYPTO_SUCCESS;
}

bool auth_verify_csrf_token(const char *token) {
   if (!s_initialized) {
      LOG_ERROR("auth_verify_csrf_token: not initialized");
      return false;
   }

   if (!token) {
      return false;
   }

   /* Check token length */
   size_t token_len = strlen(token);
   if (token_len != AUTH_CSRF_TOKEN_LEN - 1) {
      LOG_WARNING("auth_verify_csrf_token: invalid token length");
      return false;
   }

   /* Decode from hex */
   unsigned char binary_token[CSRF_BINARY_LEN];
   if (sodium_hex2bin(binary_token, sizeof(binary_token), token, token_len, NULL, NULL, NULL) !=
       0) {
      LOG_WARNING("auth_verify_csrf_token: invalid hex encoding");
      return false;
   }

   /* Extract components */
   unsigned char *msg = binary_token; /* timestamp || nonce */
   unsigned char *received_mac = binary_token + CSRF_TIMESTAMP_LEN + CSRF_NONCE_LEN;

   /* Verify HMAC (constant-time) */
   if (crypto_auth_verify(received_mac, msg, CSRF_TIMESTAMP_LEN + CSRF_NONCE_LEN, s_csrf_secret) !=
       0) {
      LOG_WARNING("auth_verify_csrf_token: HMAC verification failed");
      sodium_memzero(binary_token, sizeof(binary_token));
      return false;
   }

   /* Extract and check timestamp */
   uint64_t timestamp = 0;
   for (int i = 0; i < 8; i++) {
      timestamp = (timestamp << 8) | msg[i];
   }

   uint64_t now = (uint64_t)time(NULL);
   if (timestamp > now || (now - timestamp) > AUTH_CSRF_TIMEOUT_SEC) {
      LOG_WARNING("auth_verify_csrf_token: token expired");
      sodium_memzero(binary_token, sizeof(binary_token));
      return false;
   }

   /* Clear sensitive data */
   sodium_memzero(binary_token, sizeof(binary_token));

   return true;
}

bool auth_verify_csrf_token_extract_nonce(const char *token,
                                          unsigned char nonce_out[AUTH_CSRF_NONCE_SIZE]) {
   if (!s_initialized) {
      LOG_ERROR("auth_verify_csrf_token_extract_nonce: not initialized");
      return false;
   }

   if (!token) {
      return false;
   }

   /* Check token length */
   size_t token_len = strlen(token);
   if (token_len != AUTH_CSRF_TOKEN_LEN - 1) {
      LOG_WARNING("auth_verify_csrf_token_extract_nonce: invalid token length");
      return false;
   }

   /* Decode from hex */
   unsigned char binary_token[CSRF_BINARY_LEN];
   if (sodium_hex2bin(binary_token, sizeof(binary_token), token, token_len, NULL, NULL, NULL) !=
       0) {
      LOG_WARNING("auth_verify_csrf_token_extract_nonce: invalid hex encoding");
      return false;
   }

   /* Extract components */
   unsigned char *msg = binary_token; /* timestamp || nonce */
   unsigned char *nonce = binary_token + CSRF_TIMESTAMP_LEN;
   unsigned char *received_mac = binary_token + CSRF_TIMESTAMP_LEN + CSRF_NONCE_LEN;

   /* Verify HMAC (constant-time) */
   if (crypto_auth_verify(received_mac, msg, CSRF_TIMESTAMP_LEN + CSRF_NONCE_LEN, s_csrf_secret) !=
       0) {
      LOG_WARNING("auth_verify_csrf_token_extract_nonce: HMAC verification failed");
      sodium_memzero(binary_token, sizeof(binary_token));
      return false;
   }

   /* Extract and check timestamp
    * Note: timestamp > now check prevents underflow in subtraction */
   uint64_t timestamp = 0;
   for (int i = 0; i < 8; i++) {
      timestamp = (timestamp << 8) | msg[i];
   }

   uint64_t now = (uint64_t)time(NULL);
   if (timestamp > now || (now - timestamp) > AUTH_CSRF_TIMEOUT_SEC) {
      LOG_WARNING("auth_verify_csrf_token_extract_nonce: token expired");
      sodium_memzero(binary_token, sizeof(binary_token));
      return false;
   }

   /* Extract nonce for caller to track single-use */
   if (nonce_out) {
      memcpy(nonce_out, nonce, AUTH_CSRF_NONCE_SIZE);
   }

   /* Clear sensitive data */
   sodium_memzero(binary_token, sizeof(binary_token));

   return true;
}
