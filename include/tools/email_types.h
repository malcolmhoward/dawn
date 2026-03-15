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
 * Shared email types — used by both IMAP (email_client) and Gmail API (gmail_client).
 */

#ifndef EMAIL_TYPES_H
#define EMAIL_TYPES_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

/* Maximum emails to fetch in a single request (bounds stack usage).
 * 50 entries * ~1.3KB = ~65KB stack — well within Jetson limits. */
#define EMAIL_MAX_FETCH_RESULTS 50

typedef struct {
   uint32_t uid;
   char message_id[192]; /* Gmail hex ID or IMAP folder:uid composite */
   char from_name[64];
   char from_addr[256];
   char subject[256];
   char date_str[32];
   time_t date;
   char preview[512];
} email_summary_t;

typedef struct {
   uint32_t uid;
   char message_id[192]; /* Gmail hex ID or IMAP folder:uid composite */
   char from_name[64];
   char from_addr[256];
   char to[256];
   char subject[256];
   char date_str[32];
   char *body; /* Heap-allocated, caller frees via email_message_free() */
   int body_len;
   int attachment_count;
   bool truncated;
} email_message_t;

typedef struct {
   char from[128];
   char subject[128];
   char text[128];
   char since[16]; /* YYYY-MM-DD, validated via strptime/strftime */
   char before[16];
   bool unread_only;     /* Only match UNSEEN messages */
   char folder[256];     /* Folder/label or normalized Gmail query fragment */
   char page_token[256]; /* Gmail pagination token (empty = first page) */
} email_search_params_t;

/**
 * @brief Free heap-allocated fields in email_message_t.
 * Nulls the body pointer after freeing.
 */
void email_message_free(email_message_t *msg);

#endif /* EMAIL_TYPES_H */
