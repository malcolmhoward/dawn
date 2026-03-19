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
 * Unit tests for plan_executor.c — parsing, conditions, interpolation,
 * variable store, safety limits, and tool capability checks.
 *
 * Uses mock tool callbacks registered via tool_registry to test execution
 * without real tool infrastructure. Each test function is self-contained.
 */

#include <json-c/json.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tools/plan_executor.h"

/* ============================================================================
 * Test Harness
 * ============================================================================ */

static int tests_passed = 0;
static int tests_failed = 0;

#define ASSERT(cond, msg)                                  \
   do {                                                    \
      if (cond) {                                          \
         printf("  [PASS] %s\n", msg);                     \
         tests_passed++;                                   \
      } else {                                             \
         printf("  [FAIL] %s (line %d)\n", msg, __LINE__); \
         tests_failed++;                                   \
      }                                                    \
   } while (0)

/* ============================================================================
 * 1. Variable Name Validation
 * ============================================================================ */

static void test_variable_name_validation(void) {
   printf("\n=== test_variable_name_validation ===\n");

   /* Valid names */
   ASSERT(plan_validate_var_name("x") == true, "single char valid");
   ASSERT(plan_validate_var_name("result") == true, "lowercase word valid");
   ASSERT(plan_validate_var_name("my_var") == true, "underscore valid");
   ASSERT(plan_validate_var_name("_private") == true, "leading underscore valid");
   ASSERT(plan_validate_var_name("var123") == true, "trailing digits valid");
   ASSERT(plan_validate_var_name("a1b2c3") == true, "mixed alpha-digits valid");
   ASSERT(plan_validate_var_name("abcdefghijklmnopqrstuvwxyz012345") == true,
          "32-char max length valid");

   /* Invalid names */
   ASSERT(plan_validate_var_name(NULL) == false, "NULL rejected");
   ASSERT(plan_validate_var_name("") == false, "empty string rejected");
   ASSERT(plan_validate_var_name("1start") == false, "leading digit rejected");
   ASSERT(plan_validate_var_name("CamelCase") == false, "uppercase rejected");
   ASSERT(plan_validate_var_name("has space") == false, "space rejected");
   ASSERT(plan_validate_var_name("has-dash") == false, "dash rejected");
   ASSERT(plan_validate_var_name("has.dot") == false, "dot rejected");
   ASSERT(plan_validate_var_name("has$dollar") == false, "dollar rejected");
   ASSERT(plan_validate_var_name("has{brace") == false, "brace rejected");
   ASSERT(plan_validate_var_name("has}brace") == false, "close brace rejected");
   ASSERT(plan_validate_var_name("abcdefghijklmnopqrstuvwxyz0123456") == false,
          "33-char over max rejected");
}

/* ============================================================================
 * 2. Variable Store Operations
 * ============================================================================ */

static void test_variable_store(void) {
   printf("\n=== test_variable_store ===\n");

   plan_context_t ctx = { 0 };

   /* Set and get */
   plan_vars_set(&ctx, "foo", "hello");
   ASSERT(strcmp(plan_vars_get(&ctx, "foo"), "hello") == 0, "set/get basic");
   ASSERT(ctx.var_count == 1, "var_count is 1 after first set");

   /* Overwrite existing */
   plan_vars_set(&ctx, "foo", "world");
   ASSERT(strcmp(plan_vars_get(&ctx, "foo"), "world") == 0, "overwrite works");
   ASSERT(ctx.var_count == 1, "var_count still 1 after overwrite");

   /* Multiple variables */
   plan_vars_set(&ctx, "bar", "value2");
   plan_vars_set(&ctx, "baz", "value3");
   ASSERT(ctx.var_count == 3, "var_count is 3");
   ASSERT(strcmp(plan_vars_get(&ctx, "bar"), "value2") == 0, "second var correct");
   ASSERT(strcmp(plan_vars_get(&ctx, "baz"), "value3") == 0, "third var correct");

   /* Get nonexistent — should return NULL */
   const char *missing = plan_vars_get(&ctx, "nonexistent");
   ASSERT(missing == NULL || missing[0] == '\0', "nonexistent returns NULL or empty");

   /* Success flag */
   plan_vars_set_success(&ctx, "foo", true);
   plan_vars_set_success(&ctx, "bar", false);

   /* Cleanup frees all heap values */
   plan_context_cleanup(&ctx);
   ASSERT(ctx.var_count == 0, "cleanup resets var_count");
}

static void test_variable_store_max_capacity(void) {
   printf("\n=== test_variable_store_max_capacity ===\n");

   plan_context_t ctx = { 0 };

   /* Fill to PLAN_MAX_VARS */
   for (int i = 0; i < PLAN_MAX_VARS; i++) {
      char name[PLAN_VAR_NAME_MAX + 1];
      snprintf(name, sizeof(name), "var_%02d", i);
      plan_vars_set(&ctx, name, "value");
   }
   ASSERT(ctx.var_count == PLAN_MAX_VARS, "filled to max capacity");

   /* One more should silently fail */
   plan_vars_set(&ctx, "overflow_var", "should_fail");
   const char *overflow = plan_vars_get(&ctx, "overflow_var");
   ASSERT(overflow == NULL || overflow[0] == '\0', "variable beyond max capacity not stored");

   plan_context_cleanup(&ctx);
}

/* ============================================================================
 * 3. Condition Evaluation
 * ============================================================================ */

static void test_condition_evaluation(void) {
   printf("\n=== test_condition_evaluation ===\n");

   plan_context_t ctx = { 0 };
   plan_vars_set(&ctx, "full", "some result text");
   plan_vars_set(&ctx, "empty_str", "");
   plan_vars_set(&ctx, "none_str", "none");
   plan_vars_set(&ctx, "no_results", "no results");
   plan_vars_set(&ctx, "array_empty", "[]");
   plan_vars_set(&ctx, "rain_forecast", "Partly cloudy with rain in the afternoon");
   plan_vars_set(&ctx, "clear_forecast", "Sunny and clear all day");
   plan_vars_set(&ctx, "exact_match", "hello");

   plan_vars_set_success(&ctx, "full", true);
   plan_vars_set_success(&ctx, "empty_str", false);

   /* .empty / .notempty */
   ASSERT(plan_eval_condition(&ctx, "full.empty") == false, "non-empty var .empty is false");
   ASSERT(plan_eval_condition(&ctx, "full.notempty") == true, "non-empty var .notempty is true");
   ASSERT(plan_eval_condition(&ctx, "empty_str.empty") == true, "empty string .empty is true");
   ASSERT(plan_eval_condition(&ctx, "empty_str.notempty") == false,
          "empty string .notempty is false");
   ASSERT(plan_eval_condition(&ctx, "none_str.empty") == true, "\"none\" .empty is true");
   ASSERT(plan_eval_condition(&ctx, "no_results.empty") == true, "\"no results\" .empty is true");
   ASSERT(plan_eval_condition(&ctx, "array_empty.empty") == true, "\"[]\" .empty is true");
   ASSERT(plan_eval_condition(&ctx, "undefined_var.empty") == true, "undefined var .empty is true");
   ASSERT(plan_eval_condition(&ctx, "undefined_var.notempty") == false,
          "undefined var .notempty is false");

   /* .contains */
   ASSERT(plan_eval_condition(&ctx, "rain_forecast.contains:rain") == true,
          "contains 'rain' case-insensitive");
   ASSERT(plan_eval_condition(&ctx, "rain_forecast.contains:RAIN") == true,
          "contains 'RAIN' case-insensitive");
   ASSERT(plan_eval_condition(&ctx, "clear_forecast.contains:rain") == false,
          "clear forecast does not contain rain");
   ASSERT(plan_eval_condition(&ctx, "rain_forecast.contains:afternoon") == true,
          "contains 'afternoon'");

   /* .equals */
   ASSERT(plan_eval_condition(&ctx, "exact_match.equals:hello") == true, "equals exact match");
   ASSERT(plan_eval_condition(&ctx, "exact_match.equals:HELLO") == true, "equals case-insensitive");
   ASSERT(plan_eval_condition(&ctx, "exact_match.equals:world") == false, "equals non-match");

   /* .success / .failed */
   ASSERT(plan_eval_condition(&ctx, "full.success") == true, "success var .success is true");
   ASSERT(plan_eval_condition(&ctx, "full.failed") == false, "success var .failed is false");
   ASSERT(plan_eval_condition(&ctx, "empty_str.success") == false, "failed var .success is false");
   ASSERT(plan_eval_condition(&ctx, "empty_str.failed") == true, "failed var .failed is true");

   /* Boolean literals */
   ASSERT(plan_eval_condition(&ctx, "true") == true, "literal 'true' is true");
   ASSERT(plan_eval_condition(&ctx, "false") == false, "literal 'false' is false");

   /* Malformed conditions — should evaluate to false (safe default) */
   ASSERT(plan_eval_condition(&ctx, "") == false, "empty condition is false");
   ASSERT(plan_eval_condition(&ctx, "full.bogus_op") == false, "unknown operator is false");
   ASSERT(plan_eval_condition(&ctx, "noperiod") == false, "no operator (no period) is false");

   plan_context_cleanup(&ctx);
}

/* ============================================================================
 * 4. Variable Interpolation
 * ============================================================================ */

static void test_interpolation(void) {
   printf("\n=== test_interpolation ===\n");

   plan_context_t ctx = { 0 };
   plan_vars_set(&ctx, "name", "Friday");
   plan_vars_set(&ctx, "room", "kitchen");
   plan_vars_set(&ctx, "count", "42");

   char out[256];
   int rc;

   /* Simple substitution */
   rc = plan_interpolate(&ctx, "Hello $name", out, sizeof(out));
   ASSERT(rc == 0 && strcmp(out, "Hello Friday") == 0, "simple $var interpolation");

   /* Braced substitution */
   rc = plan_interpolate(&ctx, "${room}_light", out, sizeof(out));
   ASSERT(rc == 0 && strcmp(out, "kitchen_light") == 0, "${var} brace interpolation");

   /* Multiple variables */
   rc = plan_interpolate(&ctx, "$name is in the $room ($count)", out, sizeof(out));
   ASSERT(rc == 0 && strcmp(out, "Friday is in the kitchen (42)") == 0,
          "multiple variables interpolated");

   /* Undefined variable — expands to empty */
   rc = plan_interpolate(&ctx, "value is $undefined here", out, sizeof(out));
   ASSERT(rc == 0 && strcmp(out, "value is  here") == 0, "undefined var expands to empty");

   /* No variables — passthrough */
   rc = plan_interpolate(&ctx, "no variables here", out, sizeof(out));
   ASSERT(rc == 0 && strcmp(out, "no variables here") == 0, "no-var passthrough");

   /* Lone dollar sign (not a variable) */
   rc = plan_interpolate(&ctx, "costs $5 each", out, sizeof(out));
   ASSERT(rc == 0, "lone dollar doesn't crash");

   /* Adjacent dollar signs */
   rc = plan_interpolate(&ctx, "$$name", out, sizeof(out));
   ASSERT(rc == 0, "double dollar doesn't crash");

   /* Empty braces */
   rc = plan_interpolate(&ctx, "x ${}y", out, sizeof(out));
   ASSERT(rc == 0, "empty braces don't crash");

   plan_context_cleanup(&ctx);
}

static void test_interpolation_size_limit(void) {
   printf("\n=== test_interpolation_size_limit ===\n");

   plan_context_t ctx = { 0 };

   /* Set a long variable value */
   char long_value[512];
   memset(long_value, 'A', sizeof(long_value) - 1);
   long_value[sizeof(long_value) - 1] = '\0';
   plan_vars_set(&ctx, "big", long_value);

   /* Interpolate into a small buffer — should truncate, not overflow */
   char small_out[32];
   int rc = plan_interpolate(&ctx, "prefix $big suffix", small_out, sizeof(small_out));
   ASSERT(rc == 0, "interpolation into small buffer succeeds");
   ASSERT(strlen(small_out) < sizeof(small_out), "output fits in buffer");

   plan_context_cleanup(&ctx);
}

/* ============================================================================
 * 5. Plan Parsing
 * ============================================================================ */

static void test_parse_valid_plan(void) {
   printf("\n=== test_parse_valid_plan ===\n");

   /* Minimal valid plan with a single log step */
   const char *json = "[{\"type\": \"log\", \"message\": \"hello\"}]";
   struct json_object *plan = NULL;
   int rc = plan_parse(json, &plan);
   ASSERT(rc == 0, "valid single-step plan parses");
   ASSERT(plan != NULL, "plan array is non-NULL");
   ASSERT(json_object_array_length(plan) == 1, "plan has 1 step");
   json_object_put(plan);
}

static void test_parse_multi_step_plan(void) {
   printf("\n=== test_parse_multi_step_plan ===\n");

   const char *json = "["
                      "  {\"type\": \"set\", \"var\": \"x\", \"value\": \"hello\"},"
                      "  {\"type\": \"log\", \"message\": \"$x world\"},"
                      "  {\"type\": \"if\", \"condition\": \"x.notempty\", \"then\": ["
                      "    {\"type\": \"log\", \"message\": \"x is set\"}"
                      "  ]}"
                      "]";
   struct json_object *plan = NULL;
   int rc = plan_parse(json, &plan);
   ASSERT(rc == 0, "multi-step plan with if/set/log parses");
   ASSERT(json_object_array_length(plan) == 3, "plan has 3 top-level steps");
   json_object_put(plan);
}

static void test_parse_invalid_json(void) {
   printf("\n=== test_parse_invalid_json ===\n");

   struct json_object *plan = NULL;

   /* Not JSON at all */
   ASSERT(plan_parse("this is not json", &plan) != 0, "garbage text rejected");
   ASSERT(plan == NULL, "plan is NULL on failure");

   /* JSON but not an array */
   ASSERT(plan_parse("{\"type\": \"log\"}", &plan) != 0, "JSON object rejected");

   /* Empty array */
   int rc = plan_parse("[]", &plan);
   if (rc == 0) {
      ASSERT(json_object_array_length(plan) == 0, "empty plan has 0 steps");
      json_object_put(plan);
   } else {
      ASSERT(1, "empty plan rejected (also acceptable)");
   }
}

static void test_parse_null_input(void) {
   printf("\n=== test_parse_null_input ===\n");

   struct json_object *plan = NULL;
   ASSERT(plan_parse(NULL, &plan) != 0, "NULL input rejected");
   ASSERT(plan_parse("", &plan) != 0, "empty string rejected");
}

static void test_parse_oversized_plan(void) {
   printf("\n=== test_parse_oversized_plan ===\n");

   /* Build a JSON string larger than PLAN_MAX_PLAN_SIZE (16384) */
   size_t big_size = PLAN_MAX_PLAN_SIZE + 1024;
   char *big_json = malloc(big_size);
   ASSERT(big_json != NULL, "allocated oversized buffer");
   if (!big_json)
      return;

   /* Fill with valid-ish JSON: [{"type":"log","message":"AAAA..."}] */
   strcpy(big_json, "[{\"type\":\"log\",\"message\":\"");
   size_t prefix_len = strlen(big_json);
   memset(big_json + prefix_len, 'A', big_size - prefix_len - 4);
   strcpy(big_json + big_size - 4, "\"}]");

   struct json_object *plan = NULL;
   ASSERT(plan_parse(big_json, &plan) != 0, "oversized plan rejected");
   free(big_json);
}

static void test_parse_invalid_step_type(void) {
   printf("\n=== test_parse_invalid_step_type ===\n");

   const char *json = "[{\"type\": \"execute_shell\", \"cmd\": \"rm -rf /\"}]";
   struct json_object *plan = NULL;
   int rc = plan_parse(json, &plan);
   if (rc == 0) {
      ASSERT(plan != NULL, "plan parsed (will fail at execution)");
      json_object_put(plan);
   } else {
      ASSERT(1, "unknown step type rejected at parse time");
   }
}

static void test_parse_invalid_variable_names(void) {
   printf("\n=== test_parse_invalid_variable_names ===\n");

   /* 'store' with invalid name in call step */
   const char *json1 = "[{\"type\": \"call\", \"tool\": \"weather\", "
                       "\"args\": {\"action\": \"forecast\"}, \"store\": \"BAD_NAME\"}]";
   struct json_object *plan = NULL;
   int rc = plan_parse(json1, &plan);
   ASSERT(rc != 0, "uppercase store variable rejected");
   if (plan)
      json_object_put(plan);

   /* 'var' with injection attempt in set step */
   const char *json2 = "[{\"type\": \"set\", \"var\": \"$inject\", \"value\": \"bad\"}]";
   plan = NULL;
   rc = plan_parse(json2, &plan);
   ASSERT(rc != 0, "dollar in variable name rejected");
   if (plan)
      json_object_put(plan);

   /* 'as' with dot in loop step */
   const char *json3 = "[{\"type\": \"loop\", \"over\": [\"a\"], \"as\": \"item.name\", "
                       "\"steps\": [{\"type\": \"log\", \"message\": \"hi\"}]}]";
   plan = NULL;
   rc = plan_parse(json3, &plan);
   ASSERT(rc != 0, "dot in loop variable rejected");
   if (plan)
      json_object_put(plan);
}

/* ============================================================================
 * 6. Plan Execution — Pure Logic (set, log, if)
 * ============================================================================ */

static void test_execute_set_and_log(void) {
   printf("\n=== test_execute_set_and_log ===\n");

   const char *json = "["
                      "  {\"type\": \"set\", \"var\": \"greeting\", \"value\": \"Hello world\"},"
                      "  {\"type\": \"log\", \"message\": \"$greeting\"}"
                      "]";

   struct json_object *plan = NULL;
   int rc = plan_parse(json, &plan);
   ASSERT(rc == 0, "plan parses");

   plan_context_t ctx = { 0 };
   ctx.timeout_s = 5;
   clock_gettime(CLOCK_MONOTONIC, &ctx.start_time);

   rc = plan_execute_steps(&ctx, plan);
   ASSERT(rc == 0, "execution succeeds");
   ASSERT(strstr(ctx.output, "Hello world") != NULL, "output contains interpolated greeting");
   ASSERT(ctx.total_steps_executed == 2, "2 steps executed");

   plan_context_cleanup(&ctx);
   json_object_put(plan);
}

static void test_execute_if_then(void) {
   printf("\n=== test_execute_if_then ===\n");

   const char *json = "["
                      "  {\"type\": \"set\", \"var\": \"data\", \"value\": \"something\"},"
                      "  {\"type\": \"if\", \"condition\": \"data.notempty\", \"then\": ["
                      "    {\"type\": \"log\", \"message\": \"then branch\"}"
                      "  ], \"else\": ["
                      "    {\"type\": \"log\", \"message\": \"else branch\"}"
                      "  ]}"
                      "]";

   struct json_object *plan = NULL;
   int rc = plan_parse(json, &plan);
   ASSERT(rc == 0, "plan parses");

   plan_context_t ctx = { 0 };
   ctx.timeout_s = 5;
   clock_gettime(CLOCK_MONOTONIC, &ctx.start_time);

   rc = plan_execute_steps(&ctx, plan);
   ASSERT(rc == 0, "execution succeeds");
   ASSERT(strstr(ctx.output, "then branch") != NULL, "then branch executed");
   ASSERT(strstr(ctx.output, "else branch") == NULL, "else branch not executed");

   plan_context_cleanup(&ctx);
   json_object_put(plan);
}

static void test_execute_if_else(void) {
   printf("\n=== test_execute_if_else ===\n");

   const char *json = "["
                      "  {\"type\": \"if\", \"condition\": \"undefined_var.notempty\", \"then\": ["
                      "    {\"type\": \"log\", \"message\": \"then branch\"}"
                      "  ], \"else\": ["
                      "    {\"type\": \"log\", \"message\": \"else branch\"}"
                      "  ]}"
                      "]";

   struct json_object *plan = NULL;
   int rc = plan_parse(json, &plan);
   ASSERT(rc == 0, "plan parses");

   plan_context_t ctx = { 0 };
   ctx.timeout_s = 5;
   clock_gettime(CLOCK_MONOTONIC, &ctx.start_time);

   rc = plan_execute_steps(&ctx, plan);
   ASSERT(rc == 0, "execution succeeds");
   ASSERT(strstr(ctx.output, "else branch") != NULL, "else branch executed");
   ASSERT(strstr(ctx.output, "then branch") == NULL, "then branch not executed");

   plan_context_cleanup(&ctx);
   json_object_put(plan);
}

static void test_execute_loop(void) {
   printf("\n=== test_execute_loop ===\n");

   const char *json = "["
                      "  {\"type\": \"loop\", \"over\": [\"kitchen\", \"bedroom\", \"office\"], "
                      "   \"as\": \"room\", \"steps\": ["
                      "    {\"type\": \"log\", \"message\": \"Processing $room\"}"
                      "  ]}"
                      "]";

   struct json_object *plan = NULL;
   int rc = plan_parse(json, &plan);
   ASSERT(rc == 0, "plan parses");

   plan_context_t ctx = { 0 };
   ctx.timeout_s = 5;
   clock_gettime(CLOCK_MONOTONIC, &ctx.start_time);

   rc = plan_execute_steps(&ctx, plan);
   ASSERT(rc == 0, "execution succeeds");
   ASSERT(strstr(ctx.output, "Processing kitchen") != NULL, "loop iteration 1");
   ASSERT(strstr(ctx.output, "Processing bedroom") != NULL, "loop iteration 2");
   ASSERT(strstr(ctx.output, "Processing office") != NULL, "loop iteration 3");
   /* 1 loop step + 3 log steps = 4 total executions */
   ASSERT(ctx.total_steps_executed == 4, "4 total steps (1 loop + 3 logs)");

   plan_context_cleanup(&ctx);
   json_object_put(plan);
}

static void test_execute_nested_if_in_loop(void) {
   printf("\n=== test_execute_nested_if_in_loop ===\n");

   const char *json = "["
                      "  {\"type\": \"set\", \"var\": \"target\", \"value\": \"bedroom\"},"
                      "  {\"type\": \"loop\", \"over\": [\"kitchen\", \"bedroom\", \"office\"], "
                      "   \"as\": \"room\", \"steps\": ["
                      "    {\"type\": \"if\", \"condition\": \"room.equals:bedroom\", \"then\": ["
                      "      {\"type\": \"log\", \"message\": \"Found target: $room\"}"
                      "    ]}"
                      "  ]}"
                      "]";

   struct json_object *plan = NULL;
   int rc = plan_parse(json, &plan);
   ASSERT(rc == 0, "plan parses");

   plan_context_t ctx = { 0 };
   ctx.timeout_s = 5;
   clock_gettime(CLOCK_MONOTONIC, &ctx.start_time);

   rc = plan_execute_steps(&ctx, plan);
   ASSERT(rc == 0, "execution succeeds");
   ASSERT(strstr(ctx.output, "Found target: bedroom") != NULL,
          "conditional in loop matched correctly");

   plan_context_cleanup(&ctx);
   json_object_put(plan);
}

/* ============================================================================
 * 7. Safety Limits
 * ============================================================================ */

static void test_max_steps_exceeded(void) {
   printf("\n=== test_max_steps_exceeded ===\n");

   /* Build a plan with PLAN_MAX_STEPS + 5 log steps using json-c */
   struct json_object *plan = json_object_new_array();
   for (int i = 0; i < PLAN_MAX_STEPS + 5; i++) {
      struct json_object *step = json_object_new_object();
      json_object_object_add(step, "type", json_object_new_string("log"));
      json_object_object_add(step, "message", json_object_new_string("step"));
      json_object_array_add(plan, step);
   }

   plan_context_t ctx = { 0 };
   ctx.timeout_s = 30;
   clock_gettime(CLOCK_MONOTONIC, &ctx.start_time);

   int rc = plan_execute_steps(&ctx, plan);
   ASSERT(rc != 0, "plan exceeding max steps returns error");
   ASSERT(ctx.total_steps_executed <= PLAN_MAX_STEPS, "execution stopped at or before max steps");

   plan_context_cleanup(&ctx);
   json_object_put(plan);
}

static void test_max_depth_exceeded(void) {
   printf("\n=== test_max_depth_exceeded ===\n");

   /* Build deeply nested if statements exceeding PLAN_MAX_DEPTH (5) */
   const char *json = "[{\"type\": \"if\", \"condition\": \"true\", \"then\": ["
                      "  {\"type\": \"if\", \"condition\": \"true\", \"then\": ["
                      "    {\"type\": \"if\", \"condition\": \"true\", \"then\": ["
                      "      {\"type\": \"if\", \"condition\": \"true\", \"then\": ["
                      "        {\"type\": \"if\", \"condition\": \"true\", \"then\": ["
                      "          {\"type\": \"if\", \"condition\": \"true\", \"then\": ["
                      "            {\"type\": \"log\", \"message\": \"too deep\"}"
                      "          ]}"
                      "        ]}"
                      "      ]}"
                      "    ]}"
                      "  ]}"
                      "]}]";

   struct json_object *plan = NULL;
   int rc = plan_parse(json, &plan);
   ASSERT(rc == 0, "deeply nested plan parses");

   plan_context_t ctx = { 0 };
   ctx.timeout_s = 5;
   clock_gettime(CLOCK_MONOTONIC, &ctx.start_time);

   rc = plan_execute_steps(&ctx, plan);
   ASSERT(rc != 0, "deeply nested plan fails with depth error");

   plan_context_cleanup(&ctx);
   json_object_put(plan);
}

static void test_loop_iteration_cap(void) {
   printf("\n=== test_loop_iteration_cap ===\n");

   /* Build a loop with more than PLAN_MAX_LOOP_ITERATIONS (10) items */
   struct json_object *plan = json_object_new_array();
   struct json_object *loop_step = json_object_new_object();
   json_object_object_add(loop_step, "type", json_object_new_string("loop"));
   json_object_object_add(loop_step, "as", json_object_new_string("item"));

   struct json_object *over = json_object_new_array();
   for (int i = 0; i < 20; i++) { /* 20 > PLAN_MAX_LOOP_ITERATIONS */
      char val[8];
      snprintf(val, sizeof(val), "item%d", i);
      json_object_array_add(over, json_object_new_string(val));
   }
   json_object_object_add(loop_step, "over", over);

   struct json_object *steps = json_object_new_array();
   struct json_object *log_step = json_object_new_object();
   json_object_object_add(log_step, "type", json_object_new_string("log"));
   json_object_object_add(log_step, "message", json_object_new_string("$item"));
   json_object_array_add(steps, log_step);
   json_object_object_add(loop_step, "steps", steps);

   json_object_array_add(plan, loop_step);

   plan_context_t ctx = { 0 };
   ctx.timeout_s = 5;
   clock_gettime(CLOCK_MONOTONIC, &ctx.start_time);

   int rc = plan_execute_steps(&ctx, plan);
   /* Should cap at 10 iterations: 1 loop step + 10 log steps = 11 total */
   ASSERT(ctx.total_steps_executed <= PLAN_MAX_LOOP_ITERATIONS + 1, "loop iterations capped");
   (void)rc;

   plan_context_cleanup(&ctx);
   json_object_put(plan);
}

static void test_output_buffer_overflow(void) {
   printf("\n=== test_output_buffer_overflow ===\n");

   /* Generate many log messages that would overflow the output buffer */
   struct json_object *plan = json_object_new_array();
   char long_msg[256];
   memset(long_msg, 'X', sizeof(long_msg) - 1);
   long_msg[sizeof(long_msg) - 1] = '\0';

   /* LLM_TOOLS_RESULT_LEN is 8192; each message is 255 bytes; ~40 messages to overflow */
   for (int i = 0; i < 50; i++) {
      struct json_object *step = json_object_new_object();
      json_object_object_add(step, "type", json_object_new_string("log"));
      json_object_object_add(step, "message", json_object_new_string(long_msg));
      json_object_array_add(plan, step);
   }

   plan_context_t ctx = { 0 };
   ctx.timeout_s = 5;
   clock_gettime(CLOCK_MONOTONIC, &ctx.start_time);

   int rc = plan_execute_steps(&ctx, plan);
   /* Should succeed (fail-forward) but output should be truncated */
   ASSERT(rc == 0, "plan completes despite output overflow");
   ASSERT(ctx.output_len <= (int)sizeof(ctx.output), "output stays within buffer");
   ASSERT(strstr(ctx.output, "[truncated]") != NULL || ctx.output_len > 0,
          "output truncated or present");

   plan_context_cleanup(&ctx);
   json_object_put(plan);
}

/* ============================================================================
 * 8. Tool Capability Checks
 * ============================================================================ */

static void test_tool_capability_whitelist(void) {
   printf("\n=== test_tool_capability_whitelist ===\n");

   tool_metadata_t allowed_tool = { 0 };
   allowed_tool.name = "weather";
   allowed_tool.capabilities = TOOL_CAP_NETWORK | TOOL_CAP_SCHEDULABLE;

   tool_metadata_t dangerous_tool = { 0 };
   dangerous_tool.name = "shutdown";
   dangerous_tool.capabilities = TOOL_CAP_DANGEROUS | TOOL_CAP_SCHEDULABLE;

   tool_metadata_t no_schedule_tool = { 0 };
   no_schedule_tool.name = "custom_tool";
   no_schedule_tool.capabilities = TOOL_CAP_NETWORK;

   tool_metadata_t self_ref = { 0 };
   self_ref.name = "execute_plan";
   self_ref.capabilities = TOOL_CAP_SCHEDULABLE;

   ASSERT(plan_tool_is_allowed(&allowed_tool) == true, "SCHEDULABLE tool allowed");
   ASSERT(plan_tool_is_allowed(&dangerous_tool) == false,
          "DANGEROUS tool blocked despite SCHEDULABLE");
   ASSERT(plan_tool_is_allowed(&no_schedule_tool) == false, "non-SCHEDULABLE tool blocked");
   ASSERT(plan_tool_is_allowed(&self_ref) == false, "execute_plan self-reference blocked");
}

/* ============================================================================
 * 9. JSON String vs Object Plan Parameter
 * ============================================================================ */

static void test_parse_json_string_form(void) {
   printf("\n=== test_parse_json_string_form ===\n");

   /* Test the direct array form (string form depends on tool callback) */
   const char *direct = "[{\"type\": \"log\", \"message\": \"hello\"}]";
   struct json_object *plan = NULL;
   int rc = plan_parse(direct, &plan);
   ASSERT(rc == 0, "direct array form parses");
   if (plan)
      json_object_put(plan);
}

/* ============================================================================
 * 10. Edge Cases
 * ============================================================================ */

static void test_empty_loop(void) {
   printf("\n=== test_empty_loop ===\n");

   const char *json = "[{\"type\": \"loop\", \"over\": [], \"as\": \"item\", \"steps\": ["
                      "  {\"type\": \"log\", \"message\": \"should not appear\"}"
                      "]}]";

   struct json_object *plan = NULL;
   int rc = plan_parse(json, &plan);
   ASSERT(rc == 0, "empty loop plan parses");

   plan_context_t ctx = { 0 };
   ctx.timeout_s = 5;
   clock_gettime(CLOCK_MONOTONIC, &ctx.start_time);

   rc = plan_execute_steps(&ctx, plan);
   ASSERT(rc == 0, "empty loop succeeds");
   ASSERT(ctx.output[0] == '\0' || strstr(ctx.output, "should not appear") == NULL,
          "empty loop body never executed");

   plan_context_cleanup(&ctx);
   json_object_put(plan);
}

static void test_if_without_else(void) {
   printf("\n=== test_if_without_else ===\n");

   const char *json = "["
                      "  {\"type\": \"if\", \"condition\": \"false\", \"then\": ["
                      "    {\"type\": \"log\", \"message\": \"should not appear\"}"
                      "  ]}"
                      "]";

   struct json_object *plan = NULL;
   int rc = plan_parse(json, &plan);
   ASSERT(rc == 0, "if-without-else parses");

   plan_context_t ctx = { 0 };
   ctx.timeout_s = 5;
   clock_gettime(CLOCK_MONOTONIC, &ctx.start_time);

   rc = plan_execute_steps(&ctx, plan);
   ASSERT(rc == 0, "if-without-else succeeds when condition is false");
   ASSERT(ctx.output[0] == '\0' || strstr(ctx.output, "should not appear") == NULL,
          "then branch not executed");

   plan_context_cleanup(&ctx);
   json_object_put(plan);
}

static void test_set_overwrite_in_loop(void) {
   printf("\n=== test_set_overwrite_in_loop ===\n");

   /* The loop variable should be updated each iteration */
   const char *json = "["
                      "  {\"type\": \"loop\", \"over\": [\"first\", \"second\", \"third\"], "
                      "   \"as\": \"item\", \"steps\": ["
                      "    {\"type\": \"set\", \"var\": \"last_seen\", \"value\": \"$item\"}"
                      "  ]},"
                      "  {\"type\": \"log\", \"message\": \"last=$last_seen\"}"
                      "]";

   struct json_object *plan = NULL;
   int rc = plan_parse(json, &plan);
   ASSERT(rc == 0, "plan parses");

   plan_context_t ctx = { 0 };
   ctx.timeout_s = 5;
   clock_gettime(CLOCK_MONOTONIC, &ctx.start_time);

   rc = plan_execute_steps(&ctx, plan);
   ASSERT(rc == 0, "execution succeeds");
   ASSERT(strstr(ctx.output, "last=third") != NULL, "loop variable overwrite leaves final value");

   plan_context_cleanup(&ctx);
   json_object_put(plan);
}

/* ============================================================================
 * 11. Call Step Argument Construction
 * ============================================================================ */

static void test_arg_interpolation_safe(void) {
   printf("\n=== test_arg_interpolation_safe ===\n");

   plan_context_t ctx = { 0 };
   ctx.timeout_s = 5;
   clock_gettime(CLOCK_MONOTONIC, &ctx.start_time);

   /* Set a variable containing JSON-hostile characters */
   plan_vars_set(&ctx, "user_input", "value with \"quotes\" and \\backslash");

   /* Build args template */
   struct json_object *args = json_object_new_object();
   json_object_object_add(args, "query", json_object_new_string("$user_input"));
   json_object_object_add(args, "action", json_object_new_string("search"));

   char out_json[1024];
   int rc = plan_build_args_json(&ctx, args, out_json, sizeof(out_json));
   ASSERT(rc == 0, "arg construction succeeds");

   /* The output should be valid JSON */
   struct json_object *parsed = json_tokener_parse(out_json);
   ASSERT(parsed != NULL, "interpolated args produce valid JSON");

   /* The interpolated value should preserve the original string content */
   if (parsed) {
      struct json_object *query_obj;
      bool has_query = json_object_object_get_ex(parsed, "query", &query_obj);
      ASSERT(has_query && json_object_is_type(query_obj, json_type_string), "query field exists");
      if (has_query) {
         const char *query_str = json_object_get_string(query_obj);
         ASSERT(strstr(query_str, "quotes") != NULL,
                "original content preserved through interpolation");
      }
      json_object_put(parsed);
   }

   json_object_put(args);
   plan_context_cleanup(&ctx);
}

static void test_arg_delimiter_injection(void) {
   printf("\n=== test_arg_delimiter_injection ===\n");

   plan_context_t ctx = { 0 };
   ctx.timeout_s = 5;
   clock_gettime(CLOCK_MONOTONIC, &ctx.start_time);

   /* Variable containing :: delimiter pattern used by llm_tools */
   plan_vars_set(&ctx, "evil", "value::action::delete_all");

   struct json_object *args = json_object_new_object();
   json_object_object_add(args, "query", json_object_new_string("$evil"));

   char out_json[1024];
   int rc = plan_build_args_json(&ctx, args, out_json, sizeof(out_json));
   ASSERT(rc == 0, "arg construction succeeds with :: in value");

   /* Parse and verify — the :: should be in the string value, not interpreted */
   struct json_object *parsed = json_tokener_parse(out_json);
   ASSERT(parsed != NULL, "output is valid JSON despite :: in value");
   if (parsed)
      json_object_put(parsed);

   json_object_put(args);
   plan_context_cleanup(&ctx);
}

static void test_interpolation_dot_access(void) {
   printf("\n=== test_interpolation_dot_access ===\n");

   plan_context_t ctx = { 0 };
   ctx.timeout_s = 5;
   clock_gettime(CLOCK_MONOTONIC, &ctx.start_time);

   /* Simulate a loop variable set to a serialized JSON object */
   plan_vars_set(&ctx, "step", "{\"color\":\"red\",\"room\":\"bedroom\"}");

   char out[256];

   /* $step.color should extract "red" */
   plan_interpolate(&ctx, "$step.color", out, sizeof(out));
   ASSERT(strcmp(out, "red") == 0, "$step.color extracts field value");

   /* $step.room should extract "bedroom" */
   plan_interpolate(&ctx, "$step.room", out, sizeof(out));
   ASSERT(strcmp(out, "bedroom") == 0, "$step.room extracts field value");

   /* $step without dot should output the raw JSON */
   plan_interpolate(&ctx, "$step", out, sizeof(out));
   ASSERT(strstr(out, "color") != NULL, "$step without dot outputs raw JSON");

   /* $step.missing should output nothing (field doesn't exist) */
   plan_interpolate(&ctx, "$step.missing", out, sizeof(out));
   ASSERT(out[0] == '\0', "$step.missing produces empty for unknown field");

   /* Non-JSON variable with dot access falls through to raw value */
   plan_vars_set(&ctx, "plain", "hello");
   plan_interpolate(&ctx, "$plain.field", out, sizeof(out));
   ASSERT(strcmp(out, "hello") == 0, "dot-access on non-JSON falls back to raw value");

   /* Dot access in a larger template */
   plan_interpolate(&ctx, "Set $step.room to $step.color", out, sizeof(out));
   ASSERT(strcmp(out, "Set bedroom to red") == 0, "dot-access in mixed template");

   plan_context_cleanup(&ctx);
}

static void test_loop_with_object_items(void) {
   printf("\n=== test_loop_with_object_items ===\n");

   plan_context_t ctx = { 0 };
   ctx.timeout_s = 5;
   clock_gettime(CLOCK_MONOTONIC, &ctx.start_time);

   /* Simulate what the loop executor does with JSON objects:
    * over: [{"color":"red"}, {"color":"blue"}], as: "item" */
   const char *plan_json =
       "[{\"type\":\"loop\",\"over\":[{\"color\":\"red\"},{\"color\":\"blue\"}],"
       "\"as\":\"item\",\"steps\":[{\"type\":\"log\",\"message\":\"$item.color\"}]}]";

   struct json_object *plan = NULL;
   int rc = plan_parse(plan_json, &plan);
   ASSERT(rc == PLAN_OK, "object-loop plan parses");

   rc = plan_execute_steps(&ctx, plan);
   ASSERT(rc == PLAN_OK, "object-loop executes");

   /* Output should contain both colors */
   ASSERT(strstr(ctx.output, "red") != NULL, "loop output contains first object field");
   ASSERT(strstr(ctx.output, "blue") != NULL, "loop output contains second object field");

   json_object_put(plan);
   plan_context_cleanup(&ctx);
}

/* ============================================================================
 * Main
 * ============================================================================ */

int main(void) {
   printf("Plan Executor Unit Tests\n");
   printf("========================\n");

   /* 1. Variable name validation */
   test_variable_name_validation();

   /* 2. Variable store */
   test_variable_store();
   test_variable_store_max_capacity();

   /* 3. Condition evaluation */
   test_condition_evaluation();

   /* 4. Interpolation */
   test_interpolation();
   test_interpolation_size_limit();

   /* 5. Plan parsing */
   test_parse_valid_plan();
   test_parse_multi_step_plan();
   test_parse_invalid_json();
   test_parse_null_input();
   test_parse_oversized_plan();
   test_parse_invalid_step_type();
   test_parse_invalid_variable_names();

   /* 6. Plan execution — pure logic */
   test_execute_set_and_log();
   test_execute_if_then();
   test_execute_if_else();
   test_execute_loop();
   test_execute_nested_if_in_loop();

   /* 7. Safety limits */
   test_max_steps_exceeded();
   test_max_depth_exceeded();
   test_loop_iteration_cap();
   test_output_buffer_overflow();

   /* 8. Tool capability checks */
   test_tool_capability_whitelist();

   /* 9. JSON format handling */
   test_parse_json_string_form();

   /* 10. Edge cases */
   test_empty_loop();
   test_if_without_else();
   test_set_overwrite_in_loop();

   /* 11. Argument construction */
   test_arg_interpolation_safe();
   test_arg_delimiter_injection();

   /* 12. Dot-access interpolation and object loops */
   test_interpolation_dot_access();
   test_loop_with_object_items();

   /* Summary */
   printf("\n========================\n");
   printf("Results: %d passed, %d failed (total: %d)\n", tests_passed, tests_failed,
          tests_passed + tests_failed);

   return tests_failed > 0 ? 1 : 0;
}
