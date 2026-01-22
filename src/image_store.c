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
 * Image Store Module
 *
 * Provides SQLite BLOB storage for uploaded images.
 * Images are stored directly in the database.
 *
 * Thread Safety: Uses auth_db mutex for all database operations.
 */

#define AUTH_DB_INTERNAL_ALLOWED
#include "auth/auth_db_internal.h"
#undef AUTH_DB_INTERNAL_ALLOWED

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>

#include "image_store.h"
#include "logging.h"

/* =============================================================================
 * Static State
 * ============================================================================= */

static struct {
   bool initialized;
   size_t max_size;
   int max_per_user;
   int retention_days;
} s_store = {
   .initialized = false,
   .max_size = IMAGE_MAX_SIZE_DEFAULT,
   .max_per_user = IMAGE_MAX_PER_USER_DEFAULT,
   .retention_days = IMAGE_RETENTION_DAYS_DEFAULT,
};

/* =============================================================================
 * Internal Helpers
 * ============================================================================= */

/**
 * @brief Generate a random image ID
 *
 * Format: "img_" + 12 alphanumeric characters
 */
static void generate_image_id(char *out) {
   static const char charset[] = "0123456789"
                                 "abcdefghijklmnopqrstuvwxyz"
                                 "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
   unsigned char random_bytes[12];

   /* Use getrandom for cryptographically secure random bytes */
   if (getrandom(random_bytes, sizeof(random_bytes), 0) != sizeof(random_bytes)) {
      /* Fallback to less secure random if getrandom fails */
      for (size_t i = 0; i < sizeof(random_bytes); i++) {
         random_bytes[i] = (unsigned char)rand();
      }
   }

   out[0] = 'i';
   out[1] = 'm';
   out[2] = 'g';
   out[3] = '_';

   for (int i = 0; i < 12; i++) {
      out[4 + i] = charset[random_bytes[i] % 62];
   }
   out[16] = '\0';
}

/* =============================================================================
 * Lifecycle
 * ============================================================================= */

int image_store_init(const image_store_config_t *config) {
   if (s_store.initialized) {
      return IMAGE_STORE_SUCCESS;
   }

   /* Check that auth_db is ready */
   if (!auth_db_is_ready()) {
      LOG_ERROR("Image store: auth_db not initialized");
      return IMAGE_STORE_FAILURE;
   }

   /* Apply configuration */
   if (config) {
      if (config->max_size > 0) {
         s_store.max_size = config->max_size;
      }
      if (config->max_per_user > 0) {
         s_store.max_per_user = config->max_per_user;
      }
      s_store.retention_days = config->retention_days;
   }

   s_store.initialized = true;
   LOG_INFO("Image store initialized: max_size=%zu, max_per_user=%d, retention=%d days",
            s_store.max_size, s_store.max_per_user, s_store.retention_days);

   return IMAGE_STORE_SUCCESS;
}

void image_store_shutdown(void) {
   s_store.initialized = false;
   LOG_INFO("Image store shutdown");
}

bool image_store_is_ready(void) {
   return s_store.initialized;
}

/* =============================================================================
 * Validation
 * ============================================================================= */

bool image_store_validate_id(const char *id) {
   if (!id)
      return false;

   /* Must start with "img_" */
   if (strncmp(id, "img_", 4) != 0)
      return false;

   /* Must be exactly 16 characters (img_ + 12 alphanumeric) */
   if (strlen(id) != 16)
      return false;

   /* Characters 4-15 must be alphanumeric */
   for (int i = 4; i < 16; i++) {
      if (!isalnum((unsigned char)id[i]))
         return false;
   }

   return true;
}

bool image_store_validate_mime(const char *mime_type) {
   if (!mime_type)
      return false;

   /* Allowed types - NO SVG (XSS risk) */
   return (strcmp(mime_type, "image/jpeg") == 0 || strcmp(mime_type, "image/png") == 0 ||
           strcmp(mime_type, "image/gif") == 0 || strcmp(mime_type, "image/webp") == 0);
}

/* =============================================================================
 * Image Operations
 * ============================================================================= */

int image_store_save(int user_id,
                     const void *data,
                     size_t size,
                     const char *mime_type,
                     char *id_out) {
   if (!data || size == 0 || !mime_type || !id_out) {
      return IMAGE_STORE_INVALID;
   }

   if (!s_store.initialized) {
      return IMAGE_STORE_FAILURE;
   }

   /* Validate MIME type */
   if (!image_store_validate_mime(mime_type)) {
      return IMAGE_STORE_INVALID;
   }

   /* Check size limit */
   if (size > s_store.max_size) {
      return IMAGE_STORE_TOO_LARGE;
   }

   /* Check user image count */
   int count = image_store_count_user(user_id);
   if (count < 0) {
      return IMAGE_STORE_FAILURE;
   }
   if (count >= s_store.max_per_user) {
      return IMAGE_STORE_LIMIT_EXCEEDED;
   }

   /* Generate unique ID */
   generate_image_id(id_out);

   /* Insert into database */
   AUTH_DB_LOCK_OR_FAIL();

   time_t now = time(NULL);
   sqlite3_reset(s_db.stmt_image_create);
   sqlite3_bind_text(s_db.stmt_image_create, 1, id_out, -1, SQLITE_STATIC);
   sqlite3_bind_int(s_db.stmt_image_create, 2, user_id);
   sqlite3_bind_text(s_db.stmt_image_create, 3, mime_type, -1, SQLITE_STATIC);
   sqlite3_bind_int64(s_db.stmt_image_create, 4, (int64_t)size);
   sqlite3_bind_blob(s_db.stmt_image_create, 5, data, (int)size, SQLITE_STATIC);
   sqlite3_bind_int64(s_db.stmt_image_create, 6, (int64_t)now);

   int rc = sqlite3_step(s_db.stmt_image_create);
   sqlite3_reset(s_db.stmt_image_create);

   AUTH_DB_UNLOCK();

   if (rc != SQLITE_DONE) {
      LOG_ERROR("Image store: failed to save %s: %s", id_out, sqlite3_errmsg(s_db.db));
      return IMAGE_STORE_FAILURE;
   }

   LOG_INFO("Image store: saved %s (%zu bytes, %s) for user %d", id_out, size, mime_type, user_id);
   return IMAGE_STORE_SUCCESS;
}

int image_store_load(const char *id,
                     int user_id,
                     void **data_out,
                     size_t *size_out,
                     char *mime_out) {
   if (!id || !data_out || !size_out) {
      return IMAGE_STORE_INVALID;
   }

   /* Validate ID format (prevents injection) */
   if (!image_store_validate_id(id)) {
      return IMAGE_STORE_INVALID;
   }

   *data_out = NULL;
   *size_out = 0;

   /* Single query for metadata + data (reduces lock contention) */
   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_reset(s_db.stmt_image_get_data);
   sqlite3_bind_text(s_db.stmt_image_get_data, 1, id, -1, SQLITE_STATIC);

   int rc = sqlite3_step(s_db.stmt_image_get_data);
   if (rc != SQLITE_ROW) {
      sqlite3_reset(s_db.stmt_image_get_data);
      AUTH_DB_UNLOCK();
      return IMAGE_STORE_NOT_FOUND;
   }

   /* Extract user_id for access check (column 0) */
   int owner_id = sqlite3_column_int(s_db.stmt_image_get_data, 0);

   /* Check access permission */
   if (user_id != 0 && owner_id != user_id) {
      sqlite3_reset(s_db.stmt_image_get_data);
      AUTH_DB_UNLOCK();
      return IMAGE_STORE_FORBIDDEN;
   }

   /* Extract mime_type (column 1) */
   const char *mime = (const char *)sqlite3_column_text(s_db.stmt_image_get_data, 1);
   char mime_buffer[IMAGE_MIME_MAX] = { 0 };
   if (mime) {
      strncpy(mime_buffer, mime, IMAGE_MIME_MAX - 1);
   }

   /* Get BLOB pointer and size (column 2) */
   const void *blob = sqlite3_column_blob(s_db.stmt_image_get_data, 2);
   int blob_size = sqlite3_column_bytes(s_db.stmt_image_get_data, 2);

   if (!blob || blob_size <= 0) {
      sqlite3_reset(s_db.stmt_image_get_data);
      AUTH_DB_UNLOCK();
      return IMAGE_STORE_FAILURE;
   }

   /* Copy data (must be done while holding lock) */
   void *buffer = malloc((size_t)blob_size);
   if (!buffer) {
      sqlite3_reset(s_db.stmt_image_get_data);
      AUTH_DB_UNLOCK();
      return IMAGE_STORE_FAILURE;
   }
   memcpy(buffer, blob, (size_t)blob_size);

   sqlite3_reset(s_db.stmt_image_get_data);

   /* Update last_accessed timestamp */
   time_t now = time(NULL);
   sqlite3_reset(s_db.stmt_image_update_access);
   sqlite3_bind_int64(s_db.stmt_image_update_access, 1, (int64_t)now);
   sqlite3_bind_text(s_db.stmt_image_update_access, 2, id, -1, SQLITE_STATIC);
   sqlite3_step(s_db.stmt_image_update_access);
   sqlite3_reset(s_db.stmt_image_update_access);

   AUTH_DB_UNLOCK();

   /* Return data */
   *data_out = buffer;
   *size_out = (size_t)blob_size;
   if (mime_out) {
      strncpy(mime_out, mime_buffer, IMAGE_MIME_MAX - 1);
      mime_out[IMAGE_MIME_MAX - 1] = '\0';
   }

   return IMAGE_STORE_SUCCESS;
}

int image_store_get_metadata(const char *id, image_metadata_t *metadata_out) {
   if (!id || !metadata_out) {
      return IMAGE_STORE_INVALID;
   }

   if (!image_store_validate_id(id)) {
      return IMAGE_STORE_INVALID;
   }

   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_reset(s_db.stmt_image_get);
   sqlite3_bind_text(s_db.stmt_image_get, 1, id, -1, SQLITE_STATIC);

   int rc = sqlite3_step(s_db.stmt_image_get);
   if (rc == SQLITE_ROW) {
      strncpy(metadata_out->id, (const char *)sqlite3_column_text(s_db.stmt_image_get, 0),
              IMAGE_ID_LEN - 1);
      metadata_out->id[IMAGE_ID_LEN - 1] = '\0';

      metadata_out->user_id = sqlite3_column_int(s_db.stmt_image_get, 1);

      const char *mime = (const char *)sqlite3_column_text(s_db.stmt_image_get, 2);
      if (mime) {
         strncpy(metadata_out->mime_type, mime, IMAGE_MIME_MAX - 1);
         metadata_out->mime_type[IMAGE_MIME_MAX - 1] = '\0';
      } else {
         metadata_out->mime_type[0] = '\0';
      }

      metadata_out->size = (size_t)sqlite3_column_int64(s_db.stmt_image_get, 3);
      metadata_out->created_at = (time_t)sqlite3_column_int64(s_db.stmt_image_get, 4);
      metadata_out->last_accessed = (time_t)sqlite3_column_int64(s_db.stmt_image_get, 5);

      sqlite3_reset(s_db.stmt_image_get);
      AUTH_DB_UNLOCK();
      return IMAGE_STORE_SUCCESS;
   }

   sqlite3_reset(s_db.stmt_image_get);
   AUTH_DB_UNLOCK();
   return IMAGE_STORE_NOT_FOUND;
}

int image_store_delete(const char *id, int user_id) {
   if (!id) {
      return IMAGE_STORE_INVALID;
   }

   if (!image_store_validate_id(id)) {
      return IMAGE_STORE_INVALID;
   }

   /* Get metadata to check ownership */
   image_metadata_t metadata;
   int result = image_store_get_metadata(id, &metadata);
   if (result != IMAGE_STORE_SUCCESS) {
      return result;
   }

   /* Check access permission (user_id=0 bypasses check for admin) */
   if (user_id != 0 && metadata.user_id != user_id) {
      return IMAGE_STORE_FORBIDDEN;
   }

   /* Delete from database */
   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_reset(s_db.stmt_image_delete);
   sqlite3_bind_text(s_db.stmt_image_delete, 1, id, -1, SQLITE_STATIC);
   sqlite3_bind_int(s_db.stmt_image_delete, 2, metadata.user_id);
   int rc = sqlite3_step(s_db.stmt_image_delete);
   sqlite3_reset(s_db.stmt_image_delete);

   AUTH_DB_UNLOCK();

   if (rc != SQLITE_DONE) {
      return IMAGE_STORE_FAILURE;
   }

   LOG_INFO("Image store: deleted %s", id);
   return IMAGE_STORE_SUCCESS;
}

int image_store_count_user(int user_id) {
   AUTH_DB_LOCK_OR_RETURN(-1);

   sqlite3_reset(s_db.stmt_image_count_user);
   sqlite3_bind_int(s_db.stmt_image_count_user, 1, user_id);

   int count = -1;
   if (sqlite3_step(s_db.stmt_image_count_user) == SQLITE_ROW) {
      count = sqlite3_column_int(s_db.stmt_image_count_user, 0);
   }

   sqlite3_reset(s_db.stmt_image_count_user);
   AUTH_DB_UNLOCK();

   return count;
}

/* =============================================================================
 * Maintenance
 * ============================================================================= */

int image_store_cleanup(void) {
   if (!s_store.initialized || s_store.retention_days <= 0) {
      return 0; /* Nothing to do */
   }

   time_t cutoff = time(NULL) - (s_store.retention_days * 86400);

   AUTH_DB_LOCK_OR_RETURN(-1);

   sqlite3_reset(s_db.stmt_image_delete_old);
   sqlite3_bind_int64(s_db.stmt_image_delete_old, 1, (int64_t)cutoff);

   int rc = sqlite3_step(s_db.stmt_image_delete_old);
   int deleted = 0;
   if (rc == SQLITE_DONE) {
      deleted = sqlite3_changes(s_db.db);
   }
   sqlite3_reset(s_db.stmt_image_delete_old);

   AUTH_DB_UNLOCK();

   if (deleted > 0) {
      LOG_INFO("Image store: cleaned up %d old images", deleted);
   }

   return deleted;
}

int image_store_stats(int *total_count, int64_t *total_bytes) {
   if (!total_count || !total_bytes) {
      return IMAGE_STORE_INVALID;
   }

   *total_count = 0;
   *total_bytes = 0;

   AUTH_DB_LOCK_OR_FAIL();

   const char *sql = "SELECT COUNT(*), COALESCE(SUM(size), 0) FROM images";
   sqlite3_stmt *stmt = NULL;

   if (sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL) != SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return IMAGE_STORE_FAILURE;
   }

   if (sqlite3_step(stmt) == SQLITE_ROW) {
      *total_count = sqlite3_column_int(stmt, 0);
      *total_bytes = sqlite3_column_int64(stmt, 1);
   }

   sqlite3_finalize(stmt);
   AUTH_DB_UNLOCK();

   return IMAGE_STORE_SUCCESS;
}
