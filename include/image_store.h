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
 * Images are stored directly in the database as BLOBs.
 *
 * Thread Safety: All functions are thread-safe via auth_db mutex.
 */

#ifndef IMAGE_STORE_H
#define IMAGE_STORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

/* =============================================================================
 * Constants
 * ============================================================================= */

/** @brief Image ID format: "img_" + 12 alphanumeric chars */
#define IMAGE_ID_LEN 17 /* 4 + 12 + 1 null */

/** @brief Maximum MIME type length */
#define IMAGE_MIME_MAX 32

/** @brief Default max image size (4MB) */
#define IMAGE_MAX_SIZE_DEFAULT (4 * 1024 * 1024)

/** @brief Default max images per user */
#define IMAGE_MAX_PER_USER_DEFAULT 1000

/** @brief Default retention days (90 days, 0 = forever) */
#define IMAGE_RETENTION_DAYS_DEFAULT 90

/* =============================================================================
 * Error Codes
 * ============================================================================= */

#define IMAGE_STORE_SUCCESS 0
#define IMAGE_STORE_FAILURE 1
#define IMAGE_STORE_NOT_FOUND 2
#define IMAGE_STORE_FORBIDDEN 3
#define IMAGE_STORE_LIMIT_EXCEEDED 4
#define IMAGE_STORE_INVALID 5
#define IMAGE_STORE_TOO_LARGE 6

/* =============================================================================
 * Types
 * ============================================================================= */

/**
 * @brief Image metadata structure
 */
typedef struct {
   char id[IMAGE_ID_LEN];
   int user_id;
   char mime_type[IMAGE_MIME_MAX];
   size_t size;
   time_t created_at;
   time_t last_accessed;
} image_metadata_t;

/**
 * @brief Image store configuration
 */
typedef struct {
   size_t max_size;    /**< Maximum image size in bytes */
   int max_per_user;   /**< Maximum images per user */
   int retention_days; /**< Auto-delete after N days (0 = forever) */
} image_store_config_t;

/* =============================================================================
 * Lifecycle
 * ============================================================================= */

/**
 * @brief Initialize the image store
 *
 * Must be called after auth_db_init().
 *
 * @param config Configuration (NULL for defaults)
 * @return IMAGE_STORE_SUCCESS or IMAGE_STORE_FAILURE
 */
int image_store_init(const image_store_config_t *config);

/**
 * @brief Shutdown the image store
 *
 * Safe to call multiple times or if not initialized.
 */
void image_store_shutdown(void);

/**
 * @brief Check if image store is initialized
 *
 * @return true if ready, false otherwise
 */
bool image_store_is_ready(void);

/* =============================================================================
 * Image Operations
 * ============================================================================= */

/**
 * @brief Save an image
 *
 * Generates a unique ID and stores image as BLOB in database.
 *
 * @param user_id User who owns the image
 * @param data Image binary data
 * @param size Size of image data in bytes
 * @param mime_type MIME type (e.g., "image/jpeg")
 * @param id_out Buffer to receive generated ID (must be IMAGE_ID_LEN bytes)
 * @return IMAGE_STORE_SUCCESS, IMAGE_STORE_TOO_LARGE, IMAGE_STORE_LIMIT_EXCEEDED,
 *         IMAGE_STORE_INVALID, or IMAGE_STORE_FAILURE
 */
int image_store_save(int user_id,
                     const void *data,
                     size_t size,
                     const char *mime_type,
                     char *id_out);

/**
 * @brief Load an image
 *
 * Retrieves image BLOB from database. Updates last_accessed timestamp.
 *
 * @param id Image ID
 * @param user_id User ID for access check (0 to skip check)
 * @param data_out Pointer to receive allocated data (caller must free)
 * @param size_out Receives size of data
 * @param mime_out Buffer for MIME type (must be IMAGE_MIME_MAX bytes, can be NULL)
 * @return IMAGE_STORE_SUCCESS, IMAGE_STORE_NOT_FOUND, IMAGE_STORE_FORBIDDEN,
 *         or IMAGE_STORE_FAILURE
 */
int image_store_load(const char *id,
                     int user_id,
                     void **data_out,
                     size_t *size_out,
                     char *mime_out);

/**
 * @brief Get image metadata without loading data
 *
 * @param id Image ID
 * @param metadata_out Buffer to receive metadata
 * @return IMAGE_STORE_SUCCESS, IMAGE_STORE_NOT_FOUND, or IMAGE_STORE_FAILURE
 */
int image_store_get_metadata(const char *id, image_metadata_t *metadata_out);

/**
 * @brief Delete an image
 *
 * Removes image from database.
 *
 * @param id Image ID
 * @param user_id User ID for access check (0 to skip check, admin only)
 * @return IMAGE_STORE_SUCCESS, IMAGE_STORE_NOT_FOUND, IMAGE_STORE_FORBIDDEN,
 *         or IMAGE_STORE_FAILURE
 */
int image_store_delete(const char *id, int user_id);

/**
 * @brief Count images for a user
 *
 * @param user_id User ID
 * @return Number of images, or -1 on error
 */
int image_store_count_user(int user_id);

/* =============================================================================
 * Validation
 * ============================================================================= */

/**
 * @brief Validate an image ID format
 *
 * Valid format: "img_" followed by 12 alphanumeric characters.
 *
 * @param id Image ID to validate
 * @return true if valid, false otherwise
 */
bool image_store_validate_id(const char *id);

/**
 * @brief Check if MIME type is allowed
 *
 * Allowed: image/jpeg, image/png, image/gif, image/webp
 * NOT allowed: image/svg+xml (XSS risk)
 *
 * @param mime_type MIME type to check
 * @return true if allowed, false otherwise
 */
bool image_store_validate_mime(const char *mime_type);

/* =============================================================================
 * Maintenance
 * ============================================================================= */

/**
 * @brief Run cleanup of old images
 *
 * Deletes images older than retention period.
 *
 * @return Number of images deleted, or -1 on error
 */
int image_store_cleanup(void);

/**
 * @brief Get storage statistics
 *
 * @param total_count Output: total number of images
 * @param total_bytes Output: total storage used in bytes
 * @return IMAGE_STORE_SUCCESS or IMAGE_STORE_FAILURE
 */
int image_store_stats(int *total_count, int64_t *total_bytes);

#endif /* IMAGE_STORE_H */
