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
 * Calendar service — multi-account routing, background sync, business logic.
 * Coordinates between caldav_client (network) and calendar_db (storage).
 */

#include "tools/calendar_service.h"

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#ifdef HAVE_LIBICAL
#include <libical/ical.h>
#endif

#include <sodium.h>
#include <sys/stat.h>

#include "config/config_parser.h"
#include "config/dawn_config.h"
#include "core/path_utils.h"
#include "logging.h"
#include "tools/caldav_client.h"

/* =============================================================================
 * State
 * ============================================================================= */

static struct {
   bool initialized;
   calendar_config_t config;
   pthread_t sync_thread;
   atomic_bool sync_running;
   pthread_mutex_t mutex;
} s_cal = {
   .initialized = false,
   .sync_running = ATOMIC_VAR_INIT(false),
   .mutex = PTHREAD_MUTEX_INITIALIZER,
};

/* =============================================================================
 * Forward declarations
 * ============================================================================= */

static void *sync_thread_func(void *arg);

/* =============================================================================
 * Password encryption — libsodium crypto_secretbox with key file
 * ============================================================================= */

static unsigned char s_crypto_key[crypto_secretbox_KEYBYTES];
static bool s_crypto_ready = false;
static pthread_once_t s_crypto_once = PTHREAD_ONCE_INIT;
static int s_crypto_error = 0;

/** Build path to caldav.key in the data directory */
static void get_key_path(char *out, size_t out_len) {
   char expanded[512];
   if (g_config.paths.data_dir[0]) {
      path_expand_tilde(g_config.paths.data_dir, expanded, sizeof(expanded));
   } else {
      snprintf(expanded, sizeof(expanded), "/var/lib/dawn");
   }
   snprintf(out, out_len, "%s/caldav.key", expanded);
}

/** Load or generate the encryption key (called via pthread_once) */
static void crypto_init_once(void) {
   if (sodium_init() < 0) {
      LOG_ERROR("calendar: sodium_init failed");
      s_crypto_error = 1;
      return;
   }

   char key_path[512];
   get_key_path(key_path, sizeof(key_path));

   FILE *fp = fopen(key_path, "rb");
   if (fp) {
      size_t n = fread(s_crypto_key, 1, sizeof(s_crypto_key), fp);
      fclose(fp);
      if (n == sizeof(s_crypto_key)) {
         sodium_mlock(s_crypto_key, sizeof(s_crypto_key));
         s_crypto_ready = true;
         return;
      }
      LOG_WARNING("calendar: key file truncated, regenerating");
   }

   /* Generate new key */
   randombytes_buf(s_crypto_key, sizeof(s_crypto_key));

   mode_t old_umask = umask(0077);
   fp = fopen(key_path, "wb");
   umask(old_umask);
   if (!fp) {
      LOG_ERROR("calendar: cannot write key file: %s", strerror(errno));
      s_crypto_error = 1;
      return;
   }
   if (fwrite(s_crypto_key, 1, sizeof(s_crypto_key), fp) != sizeof(s_crypto_key)) {
      LOG_ERROR("calendar: short write to key file");
      fclose(fp);
      s_crypto_error = 1;
      return;
   }
   fclose(fp);
   sodium_mlock(s_crypto_key, sizeof(s_crypto_key));
   LOG_INFO("calendar: generated new encryption key");
   s_crypto_ready = true;
}

/** Thread-safe crypto initialization */
static int crypto_init(void) {
   pthread_once(&s_crypto_once, crypto_init_once);
   return s_crypto_error;
}

/**
 * Encrypt a plaintext password into acct->encrypted_password.
 * Format: [nonce (24 bytes)][ciphertext (len + MAC)]
 * Returns 0 on success.
 */
int calendar_encrypt_password(const char *plaintext, calendar_account_t *acct) {
   if (!plaintext || !acct)
      return 1;
   if (crypto_init() != 0)
      return 1;

   size_t pt_len = strlen(plaintext);
   size_t needed = crypto_secretbox_NONCEBYTES + crypto_secretbox_MACBYTES + pt_len;
   if (needed > sizeof(acct->encrypted_password)) {
      LOG_ERROR("calendar: password too long to encrypt (%zu bytes)", pt_len);
      return 1;
   }

   unsigned char *nonce = acct->encrypted_password;
   unsigned char *ct = acct->encrypted_password + crypto_secretbox_NONCEBYTES;

   randombytes_buf(nonce, crypto_secretbox_NONCEBYTES);
   if (crypto_secretbox_easy(ct, (const unsigned char *)plaintext, pt_len, nonce, s_crypto_key) !=
       0) {
      LOG_ERROR("calendar: encryption failed");
      return 1;
   }

   acct->encrypted_password_len = (int)needed;
   return 0;
}

/**
 * Decrypt acct->encrypted_password into a caller-provided buffer.
 * Returns 0 on success, password is NUL-terminated.
 */
int calendar_decrypt_password(const calendar_account_t *acct, char *out, size_t out_len) {
   if (!acct || !out || out_len == 0)
      return 1;
   if (crypto_init() != 0)
      return 1;

   int total = acct->encrypted_password_len;
   if (total <= (int)(crypto_secretbox_NONCEBYTES + crypto_secretbox_MACBYTES)) {
      LOG_ERROR("calendar: encrypted_password too short");
      return 1;
   }

   const unsigned char *nonce = acct->encrypted_password;
   const unsigned char *ct = acct->encrypted_password + crypto_secretbox_NONCEBYTES;
   size_t ct_len = total - crypto_secretbox_NONCEBYTES;
   size_t pt_len = ct_len - crypto_secretbox_MACBYTES;

   if (pt_len >= out_len) {
      LOG_ERROR("calendar: output buffer too small for decrypted password");
      return 1;
   }

   if (crypto_secretbox_open_easy((unsigned char *)out, ct, ct_len, nonce, s_crypto_key) != 0) {
      LOG_ERROR("calendar: decryption failed (wrong key or corrupt data)");
      return 1;
   }

   out[pt_len] = '\0';
   return 0;
}

/** Convenience: decrypt password from an account into a stack buffer */
static int get_account_password(const calendar_account_t *acct, char *buf, size_t buf_len) {
   return calendar_decrypt_password(acct, buf, buf_len);
}

/* =============================================================================
 * Lifecycle
 * ============================================================================= */

int calendar_service_init(const calendar_config_t *config) {
   if (!config || !config->enabled) {
      LOG_INFO("calendar: service disabled");
      return 0;
   }

   pthread_mutex_lock(&s_cal.mutex);
   if (s_cal.initialized) {
      pthread_mutex_unlock(&s_cal.mutex);
      return 0;
   }

   s_cal.config = *config;
   s_cal.initialized = true;

   /* Start background sync thread */
   atomic_store(&s_cal.sync_running, true);
   if (pthread_create(&s_cal.sync_thread, NULL, sync_thread_func, NULL) != 0) {
      LOG_ERROR("calendar: failed to create sync thread");
      atomic_store(&s_cal.sync_running, false);
      s_cal.initialized = false;
      pthread_mutex_unlock(&s_cal.mutex);
      return 1;
   }

   pthread_mutex_unlock(&s_cal.mutex);
   LOG_INFO("calendar: service initialized (sync every %ds)", config->sync_interval_sec);
   return 0;
}

void calendar_service_shutdown(void) {
   pthread_mutex_lock(&s_cal.mutex);
   if (!s_cal.initialized) {
      pthread_mutex_unlock(&s_cal.mutex);
      return;
   }
   atomic_store(&s_cal.sync_running, false);
   s_cal.initialized = false;
   pthread_t thread = s_cal.sync_thread;
   pthread_mutex_unlock(&s_cal.mutex);

   /* Wait for sync thread to exit (max ~sync_interval seconds) */
   pthread_join(thread, NULL);

   /* Zero crypto key from memory */
   sodium_memzero(s_crypto_key, sizeof(s_crypto_key));
   s_crypto_ready = false;

   LOG_INFO("calendar: service shut down");
}

bool calendar_service_available(void) {
   return s_cal.initialized;
}

/* =============================================================================
 * Account Management
 * ============================================================================= */

int calendar_service_add_account(int user_id,
                                 const char *name,
                                 const char *caldav_url,
                                 const char *username,
                                 const char *password,
                                 bool read_only) {
   if (!password || !password[0]) {
      LOG_ERROR("calendar: password is required for account creation");
      return 1;
   }

   calendar_account_t acct = { 0 };
   acct.user_id = user_id;
   acct.enabled = true;
   acct.read_only = read_only;
   acct.sync_interval_sec = s_cal.config.sync_interval_sec;
   snprintf(acct.name, sizeof(acct.name), "%s", name ? name : "Calendar");
   snprintf(acct.caldav_url, sizeof(acct.caldav_url), "%s", caldav_url ? caldav_url : "");
   snprintf(acct.username, sizeof(acct.username), "%s", username ? username : "");
   snprintf(acct.auth_type, sizeof(acct.auth_type), "basic");

   if (calendar_encrypt_password(password, &acct) != 0) {
      LOG_ERROR("calendar: failed to encrypt password for account '%s'", name);
      return 1;
   }

   int64_t id = calendar_db_account_create(&acct);
   if (id < 0) {
      LOG_ERROR("calendar: failed to create account '%s'", name);
      return 1;
   }

   LOG_INFO("calendar: added account '%s' (id=%lld)", name, (long long)id);
   return 0;
}

int calendar_service_remove_account(int64_t account_id) {
   int rc = calendar_db_account_delete(account_id);
   if (rc != 0) {
      LOG_ERROR("calendar: failed to remove account %lld", (long long)account_id);
      return 1;
   }
   LOG_INFO("calendar: removed account %lld", (long long)account_id);
   return 0;
}

int calendar_service_test_connection(int64_t account_id) {
   calendar_account_t acct;
   if (calendar_db_account_get(account_id, &acct) != 0) {
      LOG_ERROR("calendar: account %lld not found", (long long)account_id);
      return 1;
   }

   char password[384];
   if (get_account_password(&acct, password, sizeof(password)) != 0) {
      LOG_ERROR("calendar: failed to decrypt password for account '%s'", acct.name);
      return 1;
   }

   caldav_auth_t auth = { .username = acct.username, .password = password };
   caldav_discovery_result_t disc = { 0 };

   caldav_error_t err = caldav_discover(acct.caldav_url, &auth, &disc);
   sodium_memzero(password, sizeof(password));
   if (err != CALDAV_OK) {
      LOG_ERROR("calendar: discovery failed for '%s': %s", acct.name, caldav_strerror(err));
      return 1;
   }

   /* Cache discovery results */
   calendar_db_account_update_discovery(account_id, disc.principal_url, disc.calendar_home_url);

   /* Upsert discovered calendars (skip existing by caldav_path) */
   calendar_calendar_t existing_cals[32];
   int existing_count = calendar_db_calendar_list(account_id, existing_cals, 32);
   time_t now = time(NULL);

   for (int i = 0; i < disc.calendar_count; i++) {
      caldav_calendar_info_t *ci = &disc.calendars[i];

      /* Check if this path already exists for the account */
      bool found = false;
      for (int j = 0; j < existing_count; j++) {
         if (strcmp(existing_cals[j].caldav_path, ci->path) == 0) {
            /* Update ctag if changed */
            if (ci->ctag[0] && strcmp(existing_cals[j].ctag, ci->ctag) != 0)
               calendar_db_calendar_update_ctag(existing_cals[j].id, ci->ctag);
            found = true;
            break;
         }
      }
      if (found)
         continue;

      calendar_calendar_t cal = { 0 };
      cal.account_id = account_id;
      snprintf(cal.caldav_path, sizeof(cal.caldav_path), "%s", ci->path);
      snprintf(cal.display_name, sizeof(cal.display_name), "%s",
               ci->display_name[0] ? ci->display_name : "Calendar");
      snprintf(cal.color, sizeof(cal.color), "%s", ci->color);
      cal.is_active = !ci->read_only;
      snprintf(cal.ctag, sizeof(cal.ctag), "%s", ci->ctag);
      cal.created_at = now;

      int64_t cal_id = calendar_db_calendar_create(&cal);
      if (cal_id < 0)
         LOG_WARNING("calendar: failed to create calendar '%s'", ci->display_name);
      else
         LOG_INFO("calendar: discovered calendar '%s' (id=%lld)", ci->display_name,
                  (long long)cal_id);
   }

   int found_count = disc.calendar_count;
   caldav_discovery_free(&disc);
   LOG_INFO("calendar: connection test OK for '%s', found %d calendars", acct.name, found_count);
   return 0;
}

/* =============================================================================
 * RRULE Expansion Helper
 * ============================================================================= */

/** Insert a single base occurrence from a caldav event (non-recurring or fallback) */
static void insert_base_occurrence(int64_t event_id, const caldav_event_t *ce) {
   calendar_occurrence_t occ = { 0 };
   occ.event_id = event_id;
   occ.dtstart = ce->dtstart;
   occ.dtend = ce->dtend;
   occ.all_day = ce->all_day;
   snprintf(occ.dtstart_date, sizeof(occ.dtstart_date), "%s", ce->dtstart_date);
   snprintf(occ.dtend_date, sizeof(occ.dtend_date), "%s", ce->dtend_date);
   snprintf(occ.summary, sizeof(occ.summary), "%s", ce->summary);
   snprintf(occ.location, sizeof(occ.location), "%s", ce->location);
   calendar_db_occurrence_insert(&occ);
}

#ifdef HAVE_LIBICAL
/**
 * Expand a recurring event into occurrences within the cache window.
 * Uses libical's icalrecur_iterator for RFC 5545 recurrence rules.
 */
static void expand_rrule(int64_t event_id,
                         const caldav_event_t *ce,
                         time_t range_start,
                         time_t range_end) {
   /* Parse the RRULE string */
   struct icalrecurrencetype recur = icalrecurrencetype_from_string(ce->rrule);
   if (recur.freq == ICAL_NO_RECURRENCE) {
      LOG_WARNING("calendar: invalid RRULE '%s' for event '%s'", ce->rrule, ce->uid);
      return;
   }

   /* Build start time for iterator */
   struct icaltimetype dtstart;
   if (ce->all_day) {
      dtstart = icaltime_from_string(ce->dtstart_date[0] ? ce->dtstart_date : "19700101");
      dtstart.is_date = 1;
   } else {
      struct tm st;
      gmtime_r(&ce->dtstart, &st);
      dtstart.year = st.tm_year + 1900;
      dtstart.month = st.tm_mon + 1;
      dtstart.day = st.tm_mday;
      dtstart.hour = st.tm_hour;
      dtstart.minute = st.tm_min;
      dtstart.second = st.tm_sec;
      dtstart.is_date = 0;
      dtstart.zone = icaltimezone_get_utc_timezone();
   }

   int duration = ce->duration_sec > 0 ? ce->duration_sec : (int)(ce->dtend - ce->dtstart);
   if (duration <= 0)
      duration = 3600; /* fallback 1 hour */

   icalrecur_iterator *iter = icalrecur_iterator_new(recur, dtstart);
   if (!iter)
      return;

   int max_occurrences = 500; /* Safety limit */
   int count = 0;

   for (struct icaltimetype next = icalrecur_iterator_next(iter);
        !icaltime_is_null_time(next) && count < max_occurrences;
        next = icalrecur_iterator_next(iter)) {
      time_t occ_start = icaltime_as_timet_with_zone(next, icaltimezone_get_utc_timezone());
      time_t occ_end = occ_start + duration;

      /* Skip occurrences outside our cache window */
      if (occ_end < range_start)
         continue;
      if (occ_start > range_end)
         break;

      calendar_occurrence_t occ = { 0 };
      occ.event_id = event_id;
      occ.dtstart = occ_start;
      occ.dtend = occ_end;
      occ.all_day = ce->all_day;
      snprintf(occ.summary, sizeof(occ.summary), "%s", ce->summary);
      snprintf(occ.location, sizeof(occ.location), "%s", ce->location);

      if (ce->all_day) {
         struct tm occ_tm;
         gmtime_r(&occ_start, &occ_tm);
         snprintf(occ.dtstart_date, sizeof(occ.dtstart_date), "%04d-%02d-%02d",
                  occ_tm.tm_year + 1900, occ_tm.tm_mon + 1, occ_tm.tm_mday);
         time_t end_t = occ_start + duration;
         struct tm end_tm;
         gmtime_r(&end_t, &end_tm);
         snprintf(occ.dtend_date, sizeof(occ.dtend_date), "%04d-%02d-%02d", end_tm.tm_year + 1900,
                  end_tm.tm_mon + 1, end_tm.tm_mday);
      }

      calendar_db_occurrence_insert(&occ);
      count++;
   }

   icalrecur_iterator_free(iter);
   LOG_INFO("calendar: expanded RRULE for '%s': %d occurrences", ce->summary, count);
}
#endif /* HAVE_LIBICAL */

int calendar_service_sync_now(int64_t account_id) {
   calendar_account_t acct;
   if (calendar_db_account_get(account_id, &acct) != 0)
      return 1;

   char password[384];
   if (get_account_password(&acct, password, sizeof(password)) != 0)
      return 1;

   caldav_auth_t auth = { .username = acct.username, .password = password };

   /* Get calendars for this account */
   calendar_calendar_t cals[32];
   int cal_count = calendar_db_calendar_list(account_id, cals, 32);
   if (cal_count <= 0) {
      LOG_WARNING("calendar: no calendars for account '%s'", acct.name);
      return 1;
   }

   time_t now = time(NULL);
   time_t range_start = now - (s_cal.config.cache_past_days * 86400);
   time_t range_end = now + (s_cal.config.cache_future_days * 86400);

   int synced = 0;
   for (int i = 0; i < cal_count; i++) {
      if (!cals[i].is_active)
         continue;

      /* Check ctag for changes */
      char new_ctag[128] = { 0 };
      caldav_error_t err = caldav_get_ctag(cals[i].caldav_path, &auth, new_ctag, sizeof(new_ctag));
      if (err != CALDAV_OK) {
         LOG_WARNING("calendar: ctag check failed for '%s'", cals[i].display_name);
         continue;
      }

      if (new_ctag[0] && strcmp(new_ctag, cals[i].ctag) == 0) {
         continue; /* No changes */
      }

      /* Fetch events */
      caldav_event_list_t events = { 0 };
      err = caldav_fetch_events(cals[i].caldav_path, &auth, range_start, range_end, &events);
      if (err != CALDAV_OK) {
         LOG_WARNING("calendar: fetch failed for '%s': %s", cals[i].display_name,
                     caldav_strerror(err));
         continue;
      }

      /* Upsert events and expand occurrences */
      for (int j = 0; j < events.count; j++) {
         caldav_event_t *ce = &events.events[j];

         calendar_event_t evt = { 0 };
         evt.calendar_id = cals[i].id;
         snprintf(evt.uid, sizeof(evt.uid), "%s", ce->uid);
         snprintf(evt.etag, sizeof(evt.etag), "%s", ce->etag);
         snprintf(evt.summary, sizeof(evt.summary), "%s", ce->summary);
         snprintf(evt.description, sizeof(evt.description), "%s", ce->description);
         snprintf(evt.location, sizeof(evt.location), "%s", ce->location);
         evt.dtstart = ce->dtstart;
         evt.dtend = ce->dtend;
         evt.duration_sec = ce->duration_sec;
         evt.all_day = ce->all_day;
         snprintf(evt.dtstart_date, sizeof(evt.dtstart_date), "%s", ce->dtstart_date);
         snprintf(evt.dtend_date, sizeof(evt.dtend_date), "%s", ce->dtend_date);
         snprintf(evt.rrule, sizeof(evt.rrule), "%s", ce->rrule);
         evt.raw_ical = ce->raw_ical;
         evt.last_synced = now;

         int64_t event_id = calendar_db_event_upsert(&evt);
         if (event_id < 0)
            continue;

         /* Delete old occurrences and re-expand */
         calendar_db_occurrence_delete_for_event(event_id);

         if (ce->rrule[0]) {
#ifdef HAVE_LIBICAL
            expand_rrule(event_id, ce, range_start, range_end);
#else
            insert_base_occurrence(event_id, ce);
#endif
         } else {
            insert_base_occurrence(event_id, ce);
         }
      }

      caldav_event_list_free(&events);

      /* Update ctag */
      if (new_ctag[0])
         calendar_db_calendar_update_ctag(cals[i].id, new_ctag);

      synced++;
   }

   sodium_memzero(password, sizeof(password));
   calendar_db_account_update_sync(account_id, now);
   LOG_INFO("calendar: synced %d calendars for '%s'", synced, acct.name);
   return 0;
}

/* =============================================================================
 * Query Operations
 * ============================================================================= */

/** Get user's active calendar IDs (helper) */
static int get_user_calendar_ids(int user_id, int64_t *ids, int max_count) {
   calendar_calendar_t cals[32];
   int count = calendar_db_active_calendars_for_user(user_id, cals,
                                                     max_count < 32 ? max_count : 32);
   for (int i = 0; i < count; i++)
      ids[i] = cals[i].id;
   return count;
}

int calendar_service_today(int user_id,
                           const char *tz_name,
                           calendar_occurrence_t *out,
                           int max_count) {
   int64_t cal_ids[32];
   int cal_count = get_user_calendar_ids(user_id, cal_ids, 32);
   if (cal_count <= 0)
      return 0;

   /* Compute today's boundaries in user timezone using tm_gmtoff.
    * Avoids process-global setenv("TZ") which races with other threads. */
   time_t now = time(NULL);
   struct tm local_tm;
   localtime_r(&now, &local_tm);
   long gmtoff = local_tm.tm_gmtoff; /* System timezone offset (GNU extension) */

   /* If caller specified a different timezone, we use the system offset as a
    * reasonable approximation. For exact IANA timezone support, libical or ICU
    * would be needed. The system timezone matches dawn.toml [general].timezone
    * in the common deployment case (single-user embedded device). */
   (void)tz_name;

   /* Day boundaries: midnight to 23:59:59 in local time */
   time_t local_now = now + gmtoff;
   time_t local_midnight = local_now - (local_now % 86400);
   time_t day_start = local_midnight - gmtoff;
   time_t day_end = day_start + 86399;

   /* Query timed events */
   int count = calendar_db_occurrences_in_range(cal_ids, cal_count, day_start, day_end, out,
                                                max_count);

   /* Query all-day events for today's date */
   char date_str[16];
   snprintf(date_str, sizeof(date_str), "%04d-%02d-%02d", local_tm.tm_year + 1900,
            local_tm.tm_mon + 1, local_tm.tm_mday);

   /* Next day for exclusive end */
   char next_date[16];
   struct tm next_tm = local_tm;
   next_tm.tm_mday += 1;
   mktime(&next_tm); /* normalize */
   snprintf(next_date, sizeof(next_date), "%04d-%02d-%02d", next_tm.tm_year + 1900,
            next_tm.tm_mon + 1, next_tm.tm_mday);

   if (count >= 0 && count < max_count) {
      int allday = calendar_db_allday_occurrences_in_range(cal_ids, cal_count, date_str, next_date,
                                                           out + count, max_count - count);
      if (allday > 0)
         count += allday;
   }

   return count >= 0 ? count : 0;
}

int calendar_service_range(int user_id,
                           time_t start,
                           time_t end,
                           calendar_occurrence_t *out,
                           int max_count) {
   int64_t cal_ids[32];
   int cal_count = get_user_calendar_ids(user_id, cal_ids, 32);
   if (cal_count <= 0)
      return 0;

   return calendar_db_occurrences_in_range(cal_ids, cal_count, start, end, out, max_count);
}

int calendar_service_next(int user_id, calendar_occurrence_t *out) {
   int64_t cal_ids[32];
   int cal_count = get_user_calendar_ids(user_id, cal_ids, 32);
   if (cal_count <= 0)
      return 1;

   return calendar_db_next_occurrence(cal_ids, cal_count, time(NULL), out);
}

int calendar_service_search(int user_id,
                            const char *query,
                            calendar_occurrence_t *out,
                            int max_count) {
   int64_t cal_ids[32];
   int cal_count = get_user_calendar_ids(user_id, cal_ids, 32);
   if (cal_count <= 0)
      return 0;

   return calendar_db_occurrences_search(cal_ids, cal_count, query, out, max_count);
}

/* =============================================================================
 * iCalendar Input Sanitization
 * ============================================================================= */

/**
 * Sanitize a string for use as an iCalendar TEXT property value (RFC 5545 §3.3.11).
 * Strips CR/LF (prevents property injection) and escapes backslash, semicolon,
 * and comma as required by the spec. Modifies in-place; buffer must have room
 * for potential expansion (each special char becomes 2 chars).
 */
static void sanitize_ical_value(char *s) {
   if (!s || !s[0])
      return;

   /* First pass: strip CR/LF in-place */
   char *dst = s;
   for (const char *src = s; *src; src++) {
      if (*src != '\r' && *src != '\n')
         *dst++ = *src;
   }
   *dst = '\0';

   /* Second pass: escape \, ;, and , per RFC 5545.
    * Work on a temporary copy since escaping expands the string. */
   size_t len = strlen(s);
   if (len == 0)
      return;
   char tmp[2048];
   size_t j = 0;
   for (size_t i = 0; i < len && j < sizeof(tmp) - 2; i++) {
      if (s[i] == '\\' || s[i] == ';' || s[i] == ',') {
         tmp[j++] = '\\';
      }
      tmp[j++] = s[i];
   }
   tmp[j] = '\0';

   /* Copy back — caller buffers are at least 256 bytes */
   memcpy(s, tmp, j);
   s[j] = '\0';
}

/** Sanitize a string into a stack buffer (caller provides buf). Returns buf. */
static const char *ical_safe(const char *input, char *buf, size_t buf_len) {
   if (!input || !input[0]) {
      buf[0] = '\0';
      return buf;
   }
   snprintf(buf, buf_len, "%s", input);
   sanitize_ical_value(buf);
   return buf;
}

/* =============================================================================
 * Mutation Operations
 * ============================================================================= */

int calendar_service_add(int user_id,
                         const char *summary,
                         time_t start,
                         time_t end,
                         const char *location,
                         const char *description,
                         bool all_day,
                         const char *calendar_name,
                         const char *rrule,
                         const char *tz_name,
                         char *uid_out,
                         size_t uid_out_len) {
   /* Find target calendar */
   calendar_calendar_t cals[32];
   int cal_count = calendar_db_active_calendars_for_user(user_id, cals, 32);
   if (cal_count <= 0) {
      LOG_ERROR("calendar: no active calendars for user %d", user_id);
      return 1;
   }

   /* Select target: match by name or use first writable */
   int target = -1;
   if (calendar_name) {
      for (int i = 0; i < cal_count; i++) {
         if (strcasecmp(cals[i].display_name, calendar_name) == 0) {
            if (cals[i].account_read_only)
               return 2; /* User explicitly named a read-only calendar */
            target = i;
            break;
         }
      }
   }
   if (target < 0) {
      /* No name given or name didn't match — find first writable calendar */
      for (int i = 0; i < cal_count; i++) {
         if (!cals[i].account_read_only) {
            target = i;
            break;
         }
      }
   }
   if (target < 0)
      return 2; /* All calendars read-only */

   /* Get account for auth */
   calendar_account_t acct;
   if (calendar_db_account_get(cals[target].account_id, &acct) != 0)
      return 1;

   char password[384];
   if (get_account_password(&acct, password, sizeof(password)) != 0)
      return 1;

   /* Generate UID with random component for unpredictability */
   char uid[256];
   unsigned char uid_rand[8];
   randombytes_buf(uid_rand, sizeof(uid_rand));
   snprintf(uid, sizeof(uid), "dawn-%lx-%02x%02x%02x%02x%02x%02x%02x%02x-%d@oasis",
            (long)time(NULL), uid_rand[0], uid_rand[1], uid_rand[2], uid_rand[3], uid_rand[4],
            uid_rand[5], uid_rand[6], uid_rand[7], user_id);

   /* Default end time */
   if (end == 0)
      end = start + (s_cal.config.default_event_duration_min * 60);

   /* Build iCalendar */
   char ical[8192];
   int pos = 0;
   pos += snprintf(ical + pos, sizeof(ical) - pos,
                   "BEGIN:VCALENDAR\r\n"
                   "VERSION:2.0\r\n"
                   "PRODID:-//DAWN//OASIS//EN\r\n"
                   "BEGIN:VEVENT\r\n"
                   "UID:%s\r\n",
                   uid);

   if (all_day) {
      struct tm st;
      gmtime_r(&start, &st);
      pos += snprintf(ical + pos, sizeof(ical) - pos, "DTSTART;VALUE=DATE:%04d%02d%02d\r\n",
                      st.tm_year + 1900, st.tm_mon + 1, st.tm_mday);
      struct tm et;
      gmtime_r(&end, &et);
      pos += snprintf(ical + pos, sizeof(ical) - pos, "DTEND;VALUE=DATE:%04d%02d%02d\r\n",
                      et.tm_year + 1900, et.tm_mon + 1, et.tm_mday);
   } else {
      struct tm st;
      gmtime_r(&start, &st);
      pos += snprintf(ical + pos, sizeof(ical) - pos, "DTSTART:%04d%02d%02dT%02d%02d%02dZ\r\n",
                      st.tm_year + 1900, st.tm_mon + 1, st.tm_mday, st.tm_hour, st.tm_min,
                      st.tm_sec);
      struct tm et;
      gmtime_r(&end, &et);
      pos += snprintf(ical + pos, sizeof(ical) - pos, "DTEND:%04d%02d%02dT%02d%02d%02dZ\r\n",
                      et.tm_year + 1900, et.tm_mon + 1, et.tm_mday, et.tm_hour, et.tm_min,
                      et.tm_sec);
   }

   /* Sanitize text fields against CRLF injection */
   char safe_summary[512], safe_loc[256], safe_desc[1024], safe_rrule[256];
   pos += snprintf(ical + pos, sizeof(ical) - pos, "SUMMARY:%s\r\n",
                   ical_safe(summary, safe_summary, sizeof(safe_summary)));

   if (location && location[0])
      pos += snprintf(ical + pos, sizeof(ical) - pos, "LOCATION:%s\r\n",
                      ical_safe(location, safe_loc, sizeof(safe_loc)));
   if (description && description[0])
      pos += snprintf(ical + pos, sizeof(ical) - pos, "DESCRIPTION:%s\r\n",
                      ical_safe(description, safe_desc, sizeof(safe_desc)));
   if (rrule && rrule[0])
      pos += snprintf(ical + pos, sizeof(ical) - pos, "RRULE:%s\r\n",
                      ical_safe(rrule, safe_rrule, sizeof(safe_rrule)));

   time_t now = time(NULL);
   struct tm nt;
   gmtime_r(&now, &nt);
   pos += snprintf(ical + pos, sizeof(ical) - pos, "DTSTAMP:%04d%02d%02dT%02d%02d%02dZ\r\n",
                   nt.tm_year + 1900, nt.tm_mon + 1, nt.tm_mday, nt.tm_hour, nt.tm_min, nt.tm_sec);

   snprintf(ical + pos, sizeof(ical) - pos,
            "END:VEVENT\r\n"
            "END:VCALENDAR\r\n");

   /* PUT to server */
   caldav_auth_t auth = { .username = acct.username, .password = password };
   char etag[128] = { 0 };
   caldav_error_t err = caldav_create_event(cals[target].caldav_path, &auth, uid, ical, etag,
                                            sizeof(etag));
   sodium_memzero(password, sizeof(password));
   if (err != CALDAV_OK) {
      LOG_ERROR("calendar: create event failed: %s", caldav_strerror(err));
      return 1;
   }

   /* Cache locally */
   calendar_event_t evt = { 0 };
   evt.calendar_id = cals[target].id;
   snprintf(evt.uid, sizeof(evt.uid), "%s", uid);
   snprintf(evt.etag, sizeof(evt.etag), "%s", etag);
   snprintf(evt.summary, sizeof(evt.summary), "%s", summary);
   if (description)
      snprintf(evt.description, sizeof(evt.description), "%s", description);
   if (location)
      snprintf(evt.location, sizeof(evt.location), "%s", location);
   evt.dtstart = start;
   evt.dtend = end;
   evt.all_day = all_day;
   if (rrule)
      snprintf(evt.rrule, sizeof(evt.rrule), "%s", rrule);
   evt.raw_ical = (char *)ical; /* temp pointer, copied by upsert */
   evt.last_synced = now;

   int64_t event_id = calendar_db_event_upsert(&evt);
   if (event_id > 0) {
      calendar_occurrence_t occ = { 0 };
      occ.event_id = event_id;
      occ.dtstart = start;
      occ.dtend = end;
      occ.all_day = all_day;
      snprintf(occ.summary, sizeof(occ.summary), "%s", summary);
      if (location)
         snprintf(occ.location, sizeof(occ.location), "%s", location);
      calendar_db_occurrence_insert(&occ);
   }

   if (uid_out && uid_out_len > 0)
      snprintf(uid_out, uid_out_len, "%s", uid);

   LOG_INFO("calendar: created event '%s' (uid=%s)", summary, uid);
   return 0;
}

int calendar_service_update(int user_id,
                            const char *uid,
                            const char *summary,
                            time_t start,
                            time_t end,
                            const char *location,
                            const char *description) {
   /* Look up event by UID with user access check */
   calendar_event_t evt = { 0 };
   evt.calendar_id = user_id; /* overloaded for get_by_uid */
   if (calendar_db_event_get_by_uid(uid, &evt) != 0) {
      LOG_ERROR("calendar: event '%s' not found for user %d", uid, user_id);
      return 1;
   }

   /* Get calendar and account for auth */
   calendar_calendar_t cal = { 0 };
   if (calendar_db_calendar_get(evt.calendar_id, &cal) != 0) {
      free(evt.raw_ical);
      return 1;
   }

   calendar_account_t acct = { 0 };
   if (calendar_db_account_get(cal.account_id, &acct) != 0) {
      free(evt.raw_ical);
      return 1;
   }

   if (acct.read_only) {
      LOG_WARNING("calendar: account '%s' is read-only, cannot modify event", acct.name);
      if (evt.raw_ical)
         free(evt.raw_ical);
      return 2;
   }

   char password[384];
   if (get_account_password(&acct, password, sizeof(password)) != 0) {
      free(evt.raw_ical);
      return 1;
   }

   /* Update fields and sanitize against CRLF injection */
   if (summary)
      snprintf(evt.summary, sizeof(evt.summary), "%s", summary);
   if (start > 0)
      evt.dtstart = start;
   if (end > 0)
      evt.dtend = end;
   if (location)
      snprintf(evt.location, sizeof(evt.location), "%s", location);
   if (description)
      snprintf(evt.description, sizeof(evt.description), "%s", description);
   sanitize_ical_value(evt.summary);
   sanitize_ical_value(evt.location);
   sanitize_ical_value(evt.description);

   /* Build updated iCal (simplified — full impl would modify raw_ical) */
   char ical[8192];
   int pos = 0;
   pos += snprintf(ical + pos, sizeof(ical) - pos,
                   "BEGIN:VCALENDAR\r\n"
                   "VERSION:2.0\r\n"
                   "PRODID:-//DAWN//OASIS//EN\r\n"
                   "BEGIN:VEVENT\r\n"
                   "UID:%s\r\n",
                   evt.uid);

   struct tm st;
   gmtime_r(&evt.dtstart, &st);
   pos += snprintf(ical + pos, sizeof(ical) - pos, "DTSTART:%04d%02d%02dT%02d%02d%02dZ\r\n",
                   st.tm_year + 1900, st.tm_mon + 1, st.tm_mday, st.tm_hour, st.tm_min, st.tm_sec);
   struct tm et;
   gmtime_r(&evt.dtend, &et);
   pos += snprintf(ical + pos, sizeof(ical) - pos, "DTEND:%04d%02d%02dT%02d%02d%02dZ\r\n",
                   et.tm_year + 1900, et.tm_mon + 1, et.tm_mday, et.tm_hour, et.tm_min, et.tm_sec);

   pos += snprintf(ical + pos, sizeof(ical) - pos, "SUMMARY:%s\r\n", evt.summary);
   if (evt.location[0])
      pos += snprintf(ical + pos, sizeof(ical) - pos, "LOCATION:%s\r\n", evt.location);
   if (evt.description[0])
      pos += snprintf(ical + pos, sizeof(ical) - pos, "DESCRIPTION:%s\r\n", evt.description);

   time_t now = time(NULL);
   struct tm nt;
   gmtime_r(&now, &nt);
   pos += snprintf(ical + pos, sizeof(ical) - pos, "DTSTAMP:%04d%02d%02dT%02d%02d%02dZ\r\n",
                   nt.tm_year + 1900, nt.tm_mon + 1, nt.tm_mday, nt.tm_hour, nt.tm_min, nt.tm_sec);

   snprintf(ical + pos, sizeof(ical) - pos,
            "END:VEVENT\r\n"
            "END:VCALENDAR\r\n");

   /* Build href from calendar path + uid */
   char href[1024];
   snprintf(href, sizeof(href), "%s%s.ics", cal.caldav_path, evt.uid);
   caldav_auth_t auth = { .username = acct.username, .password = password };
   char new_etag[128] = { 0 };
   caldav_error_t err = caldav_update_event(href, &auth, evt.etag, ical, new_etag,
                                            sizeof(new_etag));
   sodium_memzero(password, sizeof(password));
   if (err != CALDAV_OK) {
      LOG_ERROR("calendar: update event failed: %s", caldav_strerror(err));
      if (evt.raw_ical)
         free(evt.raw_ical);
      return 1;
   }

   /* Update cache */
   snprintf(evt.etag, sizeof(evt.etag), "%s", new_etag);
   free(evt.raw_ical);
   evt.raw_ical = (char *)ical; /* temp pointer, copied by upsert */
   evt.last_synced = now;
   int64_t event_id = calendar_db_event_upsert(&evt);
   evt.raw_ical = NULL; /* stack buffer, do not free */

   if (event_id > 0) {
      calendar_db_occurrence_delete_for_event(event_id);
      calendar_occurrence_t occ = { 0 };
      occ.event_id = event_id;
      occ.dtstart = evt.dtstart;
      occ.dtend = evt.dtend;
      occ.all_day = evt.all_day;
      snprintf(occ.summary, sizeof(occ.summary), "%s", evt.summary);
      snprintf(occ.location, sizeof(occ.location), "%s", evt.location);
      calendar_db_occurrence_insert(&occ);
   }

   LOG_INFO("calendar: updated event '%s'", evt.summary);
   return 0;
}

int calendar_service_delete(int user_id, const char *uid) {
   calendar_event_t evt = { 0 };
   evt.calendar_id = user_id; /* overloaded */
   if (calendar_db_event_get_by_uid(uid, &evt) != 0) {
      LOG_ERROR("calendar: event '%s' not found", uid);
      return 1;
   }

   calendar_calendar_t cal;
   if (calendar_db_calendar_get(evt.calendar_id, &cal) != 0) {
      free(evt.raw_ical);
      return 1;
   }

   calendar_account_t acct;
   if (calendar_db_account_get(cal.account_id, &acct) != 0) {
      free(evt.raw_ical);
      return 1;
   }

   if (acct.read_only) {
      LOG_WARNING("calendar: account '%s' is read-only, cannot delete event", acct.name);
      free(evt.raw_ical);
      return 2;
   }

   char password[384];
   if (get_account_password(&acct, password, sizeof(password)) != 0) {
      free(evt.raw_ical);
      return 1;
   }

   char href[1024];
   snprintf(href, sizeof(href), "%s%s.ics", cal.caldav_path, evt.uid);

   caldav_auth_t auth = { .username = acct.username, .password = password };
   caldav_error_t err = caldav_delete_event(href, &auth, evt.etag);
   sodium_memzero(password, sizeof(password));
   if (err != CALDAV_OK && err != CALDAV_ERR_NOT_FOUND) {
      LOG_ERROR("calendar: delete event failed: %s", caldav_strerror(err));
      free(evt.raw_ical);
      return 1;
   }

   /* Remove from cache */
   calendar_db_occurrence_delete_for_event(evt.id);
   calendar_db_event_delete(evt.id);

   free(evt.raw_ical);
   LOG_INFO("calendar: deleted event '%s'", uid);
   return 0;
}

/* =============================================================================
 * Access Summary
 * ============================================================================= */

int calendar_service_get_access_summary(int user_id,
                                        char *writable,
                                        size_t w_len,
                                        char *read_only_out,
                                        size_t r_len) {
   calendar_calendar_t cals[32];
   int count = calendar_db_active_calendars_for_user(user_id, cals, 32);
   if (count <= 0)
      return 0;

   int w_pos = 0, r_pos = 0;
   bool has_read_only = false;
   writable[0] = '\0';
   read_only_out[0] = '\0';

   for (int i = 0; i < count; i++) {
      if (cals[i].account_read_only) {
         has_read_only = true;
         r_pos += snprintf(read_only_out + r_pos, r_len - r_pos, "%s%s", r_pos ? ", " : "",
                           cals[i].display_name);
      } else {
         w_pos += snprintf(writable + w_pos, w_len - w_pos, "%s%s", w_pos ? ", " : "",
                           cals[i].display_name);
      }
   }
   return has_read_only ? 1 : 0;
}

/* =============================================================================
 * Background Sync Thread
 * ============================================================================= */

static void *sync_thread_func(void *arg) {
   (void)arg;
   LOG_INFO("calendar: sync thread started");

   bool first_run = true;

   while (atomic_load(&s_cal.sync_running)) {
      /* Skip sleep on first run — sync immediately at startup */
      if (!first_run) {
         for (int i = 0; i < s_cal.config.sync_interval_sec && atomic_load(&s_cal.sync_running);
              i++) {
            struct timespec ts = { .tv_sec = 1, .tv_nsec = 0 };
            nanosleep(&ts, NULL);
         }

         if (!atomic_load(&s_cal.sync_running))
            break;
      }
      first_run = false;

      /* Sync all enabled accounts that are due */
      calendar_account_t accounts[16];
      int acct_count = calendar_db_account_list_enabled(accounts, 16);

      time_t now = time(NULL);
      int synced = 0;
      for (int i = 0; i < acct_count; i++) {
         int interval = accounts[i].sync_interval_sec > 0 ? accounts[i].sync_interval_sec
                                                          : s_cal.config.sync_interval_sec;
         if (now - accounts[i].last_sync < interval)
            continue;

         if (calendar_service_sync_now(accounts[i].id) == 0)
            synced++;
      }

      if (synced > 0)
         LOG_INFO("calendar: background sync completed %d/%d accounts", synced, acct_count);
   }

   LOG_INFO("calendar: sync thread exiting");
   return NULL;
}
