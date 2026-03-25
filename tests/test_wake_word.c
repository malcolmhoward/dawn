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
 * Unit tests for wake_word.c — normalization, wake word detection,
 * command extraction, goodbye/cancel/ignore phrase matching,
 * and edge cases (NULL, empty, punctuation, mixed case).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/wake_word.h"

/* ============================================================================
 * Test Harness
 * ============================================================================ */

static int tests_passed = 0;
static int tests_failed = 0;
static int assertions_passed = 0;

#define ASSERT(cond, msg)                                 \
   do {                                                   \
      if (cond) {                                         \
         assertions_passed++;                             \
      } else {                                            \
         printf("  FAIL: %s (line %d)\n", msg, __LINE__); \
         tests_failed++;                                  \
         return;                                          \
      }                                                   \
   } while (0)

#define ASSERT_STR_EQ(a, b, msg)                                                                   \
   do {                                                                                            \
      if ((a) && (b) && strcmp((a), (b)) == 0) {                                                   \
         assertions_passed++;                                                                      \
      } else {                                                                                     \
         printf("  FAIL: %s — expected \"%s\", got \"%s\" (line %d)\n", msg, (b) ? (b) : "(null)", \
                (a) ? (a) : "(null)", __LINE__);                                                   \
         tests_failed++;                                                                           \
         return;                                                                                   \
      }                                                                                            \
   } while (0)

#define RUN_TEST(fn)                \
   do {                             \
      int before = tests_failed;    \
      printf("  %s...", #fn);       \
      fn();                         \
      if (tests_failed == before) { \
         tests_passed++;            \
         printf(" OK\n");           \
      }                             \
   } while (0)

/* ============================================================================
 * Normalize Tests
 * ============================================================================ */

static void test_normalize_basic(void) {
   char *r = wake_word_normalize("Hello World");
   ASSERT(r != NULL, "normalize returns non-NULL");
   ASSERT_STR_EQ(r, "hello world", "lowercase conversion");
   free(r);
}

static void test_normalize_punctuation(void) {
   char *r = wake_word_normalize("Hey, Friday! What's up?");
   ASSERT(r != NULL, "normalize returns non-NULL");
   ASSERT_STR_EQ(r, "hey friday whats up", "punctuation stripped");
   free(r);
}

static void test_normalize_digits(void) {
   char *r = wake_word_normalize("Set timer for 5 minutes");
   ASSERT(r != NULL, "normalize returns non-NULL");
   ASSERT_STR_EQ(r, "set timer for 5 minutes", "digits preserved");
   free(r);
}

static void test_normalize_null(void) {
   char *r = wake_word_normalize(NULL);
   ASSERT(r == NULL, "NULL input returns NULL");
}

static void test_normalize_empty(void) {
   char *r = wake_word_normalize("");
   ASSERT(r != NULL, "empty input returns non-NULL");
   ASSERT_STR_EQ(r, "", "empty input returns empty string");
   free(r);
}

static void test_normalize_only_punctuation(void) {
   char *r = wake_word_normalize("...!!??");
   ASSERT(r != NULL, "only punctuation returns non-NULL");
   ASSERT_STR_EQ(r, "", "only punctuation returns empty string");
   free(r);
}

/* ============================================================================
 * Wake Word Detection Tests
 * ============================================================================ */

static void test_wake_hey_friday(void) {
   wake_word_result_t r = wake_word_check("hey friday");
   ASSERT(r.detected, "hey friday detected");
   ASSERT(!r.has_command, "no command after wake word");
}

static void test_wake_hello_friday(void) {
   wake_word_result_t r = wake_word_check("hello friday");
   ASSERT(r.detected, "hello friday detected");
   ASSERT(!r.has_command, "no command");
}

static void test_wake_okay_friday(void) {
   wake_word_result_t r = wake_word_check("okay friday");
   ASSERT(r.detected, "okay friday detected");
}

static void test_wake_good_morning_friday(void) {
   wake_word_result_t r = wake_word_check("good morning friday");
   ASSERT(r.detected, "good morning friday detected");
}

static void test_wake_with_command(void) {
   wake_word_result_t r = wake_word_check("hey friday what time is it");
   ASSERT(r.detected, "wake word detected");
   ASSERT(r.has_command, "command detected after wake word");
   ASSERT(r.command != NULL, "command pointer non-NULL");
   /* Command should point to clean text (no leading whitespace/punctuation) */
   ASSERT_STR_EQ(r.command, "what time is it", "command text correct");
}

static void test_wake_with_punctuation(void) {
   wake_word_result_t r = wake_word_check("Hey, Friday! Turn on the lights.");
   ASSERT(r.detected, "wake word detected through punctuation");
   ASSERT(r.has_command, "command detected after punctuated wake word");
}

static void test_wake_mixed_case(void) {
   wake_word_result_t r = wake_word_check("HEY FRIDAY");
   ASSERT(r.detected, "uppercase wake word detected");
}

static void test_wake_not_detected(void) {
   wake_word_result_t r = wake_word_check("what is the weather today");
   ASSERT(!r.detected, "no wake word in random speech");
   ASSERT(!r.has_command, "no command without wake word");
}

static void test_wake_partial_name(void) {
   /* "fri" is not "friday" — should not match */
   wake_word_result_t r = wake_word_check("hey fri");
   ASSERT(!r.detected, "partial wake word not detected");
}

static void test_wake_name_embedded(void) {
   /* "friday" embedded in a longer word — strstr would match, but it's
    * preceded by "hey " prefix so "hey fridaynight" matches "hey friday" */
   wake_word_result_t r = wake_word_check("hey fridaynight plans");
   ASSERT(r.detected, "wake word detected (prefix match)");
   ASSERT(r.has_command, "trailing text is command");
}

static void test_wake_null_input(void) {
   wake_word_result_t r = wake_word_check(NULL);
   ASSERT(!r.detected, "NULL input not detected");
}

static void test_wake_empty_input(void) {
   wake_word_result_t r = wake_word_check("");
   ASSERT(!r.detected, "empty input not detected");
}

static void test_wake_only_whitespace(void) {
   wake_word_result_t r = wake_word_check("   ");
   ASSERT(!r.detected, "whitespace-only not detected");
}

static void test_wake_word_only_trailing_punct(void) {
   /* "hey friday." — wake word with trailing punctuation, no real command */
   wake_word_result_t r = wake_word_check("hey friday.");
   ASSERT(r.detected, "wake word detected");
   ASSERT(!r.has_command, "trailing punctuation is not a command");
}

static void test_wake_all_prefixes(void) {
   const char *prefixes[] = { "hello",        "okay",     "alright",      "hey",  "hi",
                              "good evening", "good day", "good morning", "yeah", "k" };
   int num_prefixes = 10;

   for (int i = 0; i < num_prefixes; i++) {
      char buf[128];
      snprintf(buf, sizeof(buf), "%s friday", prefixes[i]);
      wake_word_result_t r = wake_word_check(buf);
      char msg[128];
      snprintf(msg, sizeof(msg), "'%s friday' detected", prefixes[i]);
      ASSERT(r.detected, msg);
   }
}

/* ============================================================================
 * Goodbye / Cancel / Ignore Tests
 * ============================================================================ */

static void test_goodbye_detected(void) {
   wake_word_result_t r = wake_word_check("goodbye");
   ASSERT(r.is_goodbye, "goodbye detected");
   ASSERT(!r.detected, "goodbye is not a wake word");
}

static void test_goodbye_good_night(void) {
   wake_word_result_t r = wake_word_check("Good Night");
   ASSERT(r.is_goodbye, "good night detected (case insensitive)");
}

static void test_cancel_stop(void) {
   wake_word_result_t r = wake_word_check("stop");
   ASSERT(r.is_cancel, "stop detected as cancel");
}

static void test_cancel_never_mind(void) {
   wake_word_result_t r = wake_word_check("never mind");
   ASSERT(r.is_cancel, "never mind detected as cancel");
}

static void test_cancel_with_punctuation(void) {
   wake_word_result_t r = wake_word_check("Stop!");
   ASSERT(r.is_cancel, "stop with punctuation detected");
}

static void test_ignore_empty(void) {
   /* Empty string after normalization matches the "" ignore word,
    * but wake_word_check returns early for empty input */
   wake_word_result_t r = wake_word_check("the");
   ASSERT(r.is_ignore, "'the' detected as ignore word");
}

static void test_ignore_nevermind(void) {
   wake_word_result_t r = wake_word_check("nevermind");
   ASSERT(r.is_ignore, "nevermind detected as ignore");
}

static void test_not_goodbye(void) {
   wake_word_result_t r = wake_word_check("what is the weather");
   ASSERT(!r.is_goodbye, "random text is not goodbye");
   ASSERT(!r.is_cancel, "random text is not cancel");
   ASSERT(!r.is_ignore, "random text is not ignore");
}

/* ============================================================================
 * Command Extraction Edge Cases
 * ============================================================================ */

static void test_command_with_leading_spaces(void) {
   wake_word_result_t r = wake_word_check("hey friday   what time is it");
   ASSERT(r.detected, "detected with extra spaces");
   ASSERT(r.has_command, "command detected");
}

static void test_command_with_commas(void) {
   wake_word_result_t r = wake_word_check("Hey Friday, please turn on the lights");
   ASSERT(r.detected, "detected through comma");
   ASSERT(r.has_command, "command after comma");
   /* wake_word_check now strips leading punctuation/whitespace from command */
   ASSERT(r.command[0] == 'p' || r.command[0] == 'P', "command starts with actual text");
}

/* ============================================================================
 * Main
 * ============================================================================ */

int main(void) {
   printf("=== Wake Word Unit Tests ===\n\n");

   /* Initialize with "friday" as the AI name */
   wake_word_init("friday");

   printf("--- Normalization ---\n");
   RUN_TEST(test_normalize_basic);
   RUN_TEST(test_normalize_punctuation);
   RUN_TEST(test_normalize_digits);
   RUN_TEST(test_normalize_null);
   RUN_TEST(test_normalize_empty);
   RUN_TEST(test_normalize_only_punctuation);

   printf("\n--- Wake Word Detection ---\n");
   RUN_TEST(test_wake_hey_friday);
   RUN_TEST(test_wake_hello_friday);
   RUN_TEST(test_wake_okay_friday);
   RUN_TEST(test_wake_good_morning_friday);
   RUN_TEST(test_wake_with_command);
   RUN_TEST(test_wake_with_punctuation);
   RUN_TEST(test_wake_mixed_case);
   RUN_TEST(test_wake_not_detected);
   RUN_TEST(test_wake_partial_name);
   RUN_TEST(test_wake_name_embedded);
   RUN_TEST(test_wake_null_input);
   RUN_TEST(test_wake_empty_input);
   RUN_TEST(test_wake_only_whitespace);
   RUN_TEST(test_wake_word_only_trailing_punct);
   RUN_TEST(test_wake_all_prefixes);

   printf("\n--- Goodbye / Cancel / Ignore ---\n");
   RUN_TEST(test_goodbye_detected);
   RUN_TEST(test_goodbye_good_night);
   RUN_TEST(test_cancel_stop);
   RUN_TEST(test_cancel_never_mind);
   RUN_TEST(test_cancel_with_punctuation);
   RUN_TEST(test_ignore_empty);
   RUN_TEST(test_ignore_nevermind);
   RUN_TEST(test_not_goodbye);

   printf("\n--- Command Extraction Edge Cases ---\n");
   RUN_TEST(test_command_with_leading_spaces);
   RUN_TEST(test_command_with_commas);

   printf("\n=== Results: %d passed, %d failed (%d assertions) ===\n", tests_passed, tests_failed,
          assertions_passed);

   return tests_failed > 0 ? 1 : 0;
}
