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
 * Unit tests for phone_db.c — call log and SMS log CRUD.
 * Uses an in-memory SQLite database via the stubbed s_db global.
 */

#define AUTH_DB_INTERNAL_ALLOWED

#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "auth/auth_db_internal.h"
#include "tools/phone_db.h"

/* ============================================================================
 * Test Harness
 * ============================================================================ */

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST_ASSERT(condition, msg)    \
   do {                                \
      if (condition) {                 \
         printf("  [PASS] %s\n", msg); \
         tests_passed++;               \
      } else {                         \
         printf("  [FAIL] %s\n", msg); \
         tests_failed++;               \
      }                                \
   } while (0)

/* ============================================================================
 * Setup / Teardown
 * ============================================================================ */

static const char *DDL = "CREATE TABLE IF NOT EXISTS phone_call_log ("
                         "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
                         "  user_id INTEGER NOT NULL,"
                         "  direction INTEGER NOT NULL,"
                         "  number TEXT NOT NULL,"
                         "  contact_name TEXT DEFAULT '',"
                         "  duration_sec INTEGER DEFAULT 0,"
                         "  timestamp INTEGER NOT NULL,"
                         "  status INTEGER NOT NULL"
                         ");"
                         "CREATE INDEX IF NOT EXISTS idx_phone_call_user_ts "
                         "  ON phone_call_log(user_id, timestamp DESC);"
                         "CREATE TABLE IF NOT EXISTS phone_sms_log ("
                         "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
                         "  user_id INTEGER NOT NULL,"
                         "  direction INTEGER NOT NULL,"
                         "  number TEXT NOT NULL,"
                         "  contact_name TEXT DEFAULT '',"
                         "  body TEXT NOT NULL,"
                         "  timestamp INTEGER NOT NULL,"
                         "  read INTEGER DEFAULT 0"
                         ");"
                         "CREATE INDEX IF NOT EXISTS idx_phone_sms_user_ts "
                         "  ON phone_sms_log(user_id, timestamp DESC);"
                         "CREATE INDEX IF NOT EXISTS idx_phone_sms_unread "
                         "  ON phone_sms_log(user_id, read) WHERE read = 0;";

static void setup_db(void) {
   memset(&s_db, 0, sizeof(s_db));
   int rc = sqlite3_open(":memory:", &s_db.db);
   if (rc != SQLITE_OK) {
      fprintf(stderr, "Failed to open in-memory DB: %s\n", sqlite3_errmsg(s_db.db));
      exit(1);
   }
   pthread_mutex_init(&s_db.mutex, NULL);

   char *errmsg = NULL;
   rc = sqlite3_exec(s_db.db, DDL, NULL, NULL, &errmsg);
   if (rc != SQLITE_OK) {
      fprintf(stderr, "DDL failed: %s\n", errmsg);
      sqlite3_free(errmsg);
      exit(1);
   }

   s_db.initialized = true;
}

static void teardown_db(void) {
   if (s_db.db) {
      sqlite3_close(s_db.db);
   }
   pthread_mutex_destroy(&s_db.mutex);
   memset(&s_db, 0, sizeof(s_db));
}

/* ============================================================================
 * Call Log Tests
 * ============================================================================ */

static void test_call_log_insert(void) {
   printf("\n--- test_call_log_insert ---\n");

   time_t now = time(NULL);
   int64_t id = phone_db_call_log_insert(1, PHONE_DIR_OUTGOING, "+15551234567", "Mom", 0, now,
                                         PHONE_CALL_ANSWERED);
   TEST_ASSERT(id >= 1, "call log insert returns positive ID");

   int64_t id2 = phone_db_call_log_insert(1, PHONE_DIR_INCOMING, "+15559876543", "Dad", 0, now + 1,
                                          PHONE_CALL_MISSED);
   TEST_ASSERT(id2 > id, "second insert has higher ID");
}

static void test_call_log_update(void) {
   printf("\n--- test_call_log_update ---\n");

   time_t now = time(NULL);
   int64_t id = phone_db_call_log_insert(1, PHONE_DIR_OUTGOING, "+15551234567", "Mom", 0, now,
                                         PHONE_CALL_ANSWERED);

   int rc = phone_db_call_log_update(id, 120, PHONE_CALL_ANSWERED);
   TEST_ASSERT(rc == 0, "update returns success");

   phone_call_log_t entries[10];
   int count = phone_db_call_log_recent(1, entries, 10);
   TEST_ASSERT(count >= 1, "recent returns at least 1 entry");

   /* Find our entry */
   int found = 0;
   for (int i = 0; i < count; i++) {
      if (entries[i].id == id) {
         TEST_ASSERT(entries[i].duration_sec == 120, "duration updated to 120");
         found = 1;
         break;
      }
   }
   TEST_ASSERT(found, "updated entry found in recent results");
}

static void test_call_log_recent(void) {
   printf("\n--- test_call_log_recent ---\n");

   phone_call_log_t entries[10];
   int count = phone_db_call_log_recent(1, entries, 10);
   TEST_ASSERT(count >= 2, "recent returns multiple entries");
   TEST_ASSERT(entries[0].timestamp >= entries[1].timestamp, "entries sorted by timestamp DESC");
}

static void test_call_log_user_isolation(void) {
   printf("\n--- test_call_log_user_isolation ---\n");

   time_t now = time(NULL);
   phone_db_call_log_insert(2, PHONE_DIR_OUTGOING, "+15551111111", "Other User", 30, now,
                            PHONE_CALL_ANSWERED);

   phone_call_log_t entries[10];
   int count = phone_db_call_log_recent(2, entries, 10);
   TEST_ASSERT(count == 1, "user 2 sees only their own entries");

   int count1 = phone_db_call_log_recent(1, entries, 10);
   TEST_ASSERT(count1 >= 2, "user 1 still has their entries");
}

/* ============================================================================
 * SMS Log Tests
 * ============================================================================ */

static void test_sms_log_insert(void) {
   printf("\n--- test_sms_log_insert ---\n");

   time_t now = time(NULL);
   int64_t id = phone_db_sms_log_insert(1, PHONE_DIR_INCOMING, "+15551234567", "Mom",
                                        "Hello from Mom!", now);
   TEST_ASSERT(id >= 1, "sms insert returns positive ID");
}

static void test_sms_get_unread(void) {
   printf("\n--- test_sms_get_unread ---\n");

   phone_sms_log_t entries[10];
   int count = phone_db_sms_get_unread(1, entries, 10);
   TEST_ASSERT(count >= 1, "unread returns at least 1 entry");
   TEST_ASSERT(entries[0].read == 0, "entry is unread");
   TEST_ASSERT(strcmp(entries[0].body, "Hello from Mom!") == 0, "body matches");
}

static void test_sms_mark_read(void) {
   printf("\n--- test_sms_mark_read ---\n");

   phone_sms_log_t entries[10];
   int count = phone_db_sms_get_unread(1, entries, 10);
   TEST_ASSERT(count >= 1, "has unread before mark");

   int rc = phone_db_sms_mark_read(entries[0].id);
   TEST_ASSERT(rc == 0, "mark_read returns success");

   int count2 = phone_db_sms_get_unread(1, entries, 10);
   TEST_ASSERT(count2 < count, "fewer unread after mark");
}

static void test_sms_log_recent(void) {
   printf("\n--- test_sms_log_recent ---\n");

   /* Add another SMS */
   phone_db_sms_log_insert(1, PHONE_DIR_OUTGOING, "+15559876543", "Dad", "Hey Dad", time(NULL));

   phone_sms_log_t entries[10];
   int count = phone_db_sms_log_recent(1, entries, 10);
   TEST_ASSERT(count >= 2, "recent returns multiple SMS entries");
   TEST_ASSERT(entries[0].timestamp >= entries[1].timestamp,
               "SMS entries sorted by timestamp DESC");
}

static void test_sms_user_isolation(void) {
   printf("\n--- test_sms_user_isolation ---\n");

   phone_db_sms_log_insert(3, PHONE_DIR_INCOMING, "+15552222222", "Jane", "Hi there", time(NULL));

   phone_sms_log_t entries[10];
   int count = phone_db_sms_log_recent(3, entries, 10);
   TEST_ASSERT(count == 1, "user 3 sees only their SMS");
}

/* ============================================================================
 * Cleanup Tests
 * ============================================================================ */

static void test_cleanup(void) {
   printf("\n--- test_cleanup ---\n");

   /* Insert old entries (200 days ago) */
   time_t old_time = time(NULL) - (200 * 86400);
   phone_db_call_log_insert(1, PHONE_DIR_OUTGOING, "+15550000000", "Old", 10, old_time,
                            PHONE_CALL_ANSWERED);
   phone_db_sms_log_insert(1, PHONE_DIR_INCOMING, "+15550000000", "Old", "Old message", old_time);

   /* Verify they exist */
   phone_call_log_t calls[20];
   int call_count_before = phone_db_call_log_recent(1, calls, 20);
   phone_sms_log_t sms[20];
   int sms_count_before = phone_db_sms_log_recent(1, sms, 20);

   /* Cleanup with 90-day retention */
   int rc = phone_db_cleanup(90, 90);
   TEST_ASSERT(rc == 0, "cleanup returns success");

   /* Old entries should be gone */
   int call_count_after = phone_db_call_log_recent(1, calls, 20);
   int sms_count_after = phone_db_sms_log_recent(1, sms, 20);

   TEST_ASSERT(call_count_after < call_count_before, "old call log entries cleaned up");
   TEST_ASSERT(sms_count_after < sms_count_before, "old SMS log entries cleaned up");
}

/* ============================================================================
 * Edge Cases
 * ============================================================================ */

static void test_null_params(void) {
   printf("\n--- test_null_params ---\n");

   int64_t id = phone_db_call_log_insert(1, PHONE_DIR_OUTGOING, NULL, NULL, 0, time(NULL),
                                         PHONE_CALL_ANSWERED);
   TEST_ASSERT(id >= 1, "insert with NULL strings succeeds (defaults to empty)");

   int count = phone_db_call_log_recent(1, NULL, 10);
   TEST_ASSERT(count == -1, "recent with NULL out returns -1");

   count = phone_db_sms_get_unread(1, NULL, 10);
   TEST_ASSERT(count == -1, "unread with NULL out returns -1");
}

/* ============================================================================
 * Main
 * ============================================================================ */

int main(void) {
   printf("=== Phone DB Unit Tests ===\n");

   setup_db();

   /* Call log */
   test_call_log_insert();
   test_call_log_update();
   test_call_log_recent();
   test_call_log_user_isolation();

   /* SMS log */
   test_sms_log_insert();
   test_sms_get_unread();
   test_sms_mark_read();
   test_sms_log_recent();
   test_sms_user_isolation();

   /* Cleanup */
   test_cleanup();

   /* Edge cases */
   test_null_params();

   teardown_db();

   printf("\n=== Results: %d passed, %d failed ===\n", tests_passed, tests_failed);
   return tests_failed > 0 ? 1 : 0;
}
