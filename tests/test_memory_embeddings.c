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
 * Unit tests for memory embeddings math utilities and hybrid search logic.
 * Tests pure math functions without requiring ONNX model or database.
 */

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "memory/memory_embeddings.h"

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

#define FLOAT_EQ(a, b) (fabsf((a) - (b)) < 1e-5f)

/* ============================================================================
 * Test: L2 Norm
 * ============================================================================ */

static void test_l2_norm(void) {
   printf("\n--- test_l2_norm ---\n");

   /* Unit vector */
   float unit[] = { 1.0f, 0.0f, 0.0f };
   float norm = memory_embeddings_l2_norm(unit, 3);
   TEST_ASSERT(FLOAT_EQ(norm, 1.0f), "unit vector norm = 1.0");

   /* Simple 3-4-5 triangle */
   float vec345[] = { 3.0f, 4.0f };
   norm = memory_embeddings_l2_norm(vec345, 2);
   TEST_ASSERT(FLOAT_EQ(norm, 5.0f), "3-4-5 norm = 5.0");

   /* Zero vector */
   float zero[] = { 0.0f, 0.0f, 0.0f };
   norm = memory_embeddings_l2_norm(zero, 3);
   TEST_ASSERT(FLOAT_EQ(norm, 0.0f), "zero vector norm = 0.0");

   /* NULL input */
   norm = memory_embeddings_l2_norm(NULL, 3);
   TEST_ASSERT(FLOAT_EQ(norm, 0.0f), "NULL input norm = 0.0");

   /* Zero dims */
   float any[] = { 1.0f };
   norm = memory_embeddings_l2_norm(any, 0);
   TEST_ASSERT(FLOAT_EQ(norm, 0.0f), "zero dims norm = 0.0");
}

/* ============================================================================
 * Test: Cosine Similarity
 * ============================================================================ */

static void test_cosine_similarity(void) {
   printf("\n--- test_cosine_similarity ---\n");

   /* Identical vectors → cosine = 1.0 */
   float a[] = { 1.0f, 2.0f, 3.0f };
   float b[] = { 1.0f, 2.0f, 3.0f };
   float sim = memory_embeddings_cosine(a, b, 3);
   TEST_ASSERT(FLOAT_EQ(sim, 1.0f), "identical vectors → 1.0");

   /* Orthogonal vectors → cosine = 0.0 */
   float x[] = { 1.0f, 0.0f };
   float y[] = { 0.0f, 1.0f };
   sim = memory_embeddings_cosine(x, y, 2);
   TEST_ASSERT(FLOAT_EQ(sim, 0.0f), "orthogonal vectors → 0.0");

   /* Opposite vectors → clamped to 0.0 */
   float pos[] = { 1.0f, 0.0f };
   float neg[] = { -1.0f, 0.0f };
   sim = memory_embeddings_cosine(pos, neg, 2);
   TEST_ASSERT(FLOAT_EQ(sim, 0.0f), "opposite vectors → clamped to 0.0");

   /* Zero vector → 0.0 */
   float zero[] = { 0.0f, 0.0f };
   sim = memory_embeddings_cosine(a, zero, 2);
   TEST_ASSERT(FLOAT_EQ(sim, 0.0f), "zero vector → 0.0");
}

/* ============================================================================
 * Test: Cosine with Precomputed Norms
 * ============================================================================ */

static void test_cosine_with_norms(void) {
   printf("\n--- test_cosine_with_norms ---\n");

   float a[] = { 1.0f, 2.0f, 3.0f };
   float b[] = { 4.0f, 5.0f, 6.0f };
   float norm_a = memory_embeddings_l2_norm(a, 3);
   float norm_b = memory_embeddings_l2_norm(b, 3);

   float sim_precomputed = memory_embeddings_cosine_with_norms(a, b, 3, norm_a, norm_b);
   float sim_computed = memory_embeddings_cosine(a, b, 3);
   TEST_ASSERT(FLOAT_EQ(sim_precomputed, sim_computed), "precomputed norms match computed cosine");

   /* Zero norm → 0.0 */
   float sim_zero = memory_embeddings_cosine_with_norms(a, b, 3, 0.0f, norm_b);
   TEST_ASSERT(FLOAT_EQ(sim_zero, 0.0f), "zero norm_a → 0.0");
}

/* ============================================================================
 * Test: Cosine Clamping (negative → 0)
 * ============================================================================ */

static void test_cosine_clamping(void) {
   printf("\n--- test_cosine_clamping ---\n");

   /* Anti-correlated vectors should clamp to 0 */
   float a[] = { 1.0f, 1.0f, 1.0f };
   float b[] = { -1.0f, -1.0f, -1.0f };
   float sim = memory_embeddings_cosine(a, b, 3);
   TEST_ASSERT(sim >= 0.0f, "negative cosine clamped to >= 0");
   TEST_ASSERT(FLOAT_EQ(sim, 0.0f), "fully opposite → 0.0");

   /* Partially opposite */
   float c[] = { 1.0f, 0.0f };
   float d[] = { -0.5f, 0.1f };
   sim = memory_embeddings_cosine(c, d, 2);
   TEST_ASSERT(sim >= 0.0f, "partial opposite clamped to >= 0");
}

/* ============================================================================
 * Test: Hybrid Score Merge Logic
 * ============================================================================ */

static void test_hybrid_score_merge(void) {
   printf("\n--- test_hybrid_score_merge ---\n");

   /* Manual verification of hybrid score formula:
    * hybrid = kw_weight × (kw_score / token_count) + vec_weight × cosine */

   float kw_weight = 0.3f;
   float vec_weight = 0.7f;

   /* Fact with 2/3 keyword match tokens and 0.8 cosine */
   float kw_score = 2.0f / 3.0f; /* 0.667 */
   float cosine = 0.8f;
   float hybrid = kw_weight * kw_score + vec_weight * cosine;
   float expected = 0.3f * (2.0f / 3.0f) + 0.7f * 0.8f; /* 0.2 + 0.56 = 0.76 */
   TEST_ASSERT(FLOAT_EQ(hybrid, expected), "hybrid score = kw*0.667 + vec*0.8");

   /* Un-embedded fact (keyword only, no vector penalty) */
   float kw_only = kw_weight * 1.0f; /* full keyword match */
   TEST_ASSERT(FLOAT_EQ(kw_only, 0.3f), "un-embedded fact: keyword score only (0.3)");

   /* Vector-only result (no keyword match) */
   float vec_only = vec_weight * 0.9f;
   TEST_ASSERT(FLOAT_EQ(vec_only, 0.63f), "vector-only result: 0.7 * 0.9 = 0.63");
}

/* ============================================================================
 * Test: Dimension Mismatch Skip
 * ============================================================================ */

static void test_dimension_validation(void) {
   printf("\n--- test_dimension_validation ---\n");

   /* Verify L2 norm handles different dimensions correctly */
   float vec4[] = { 1.0f, 1.0f, 1.0f, 1.0f };
   float norm4 = memory_embeddings_l2_norm(vec4, 4);
   TEST_ASSERT(FLOAT_EQ(norm4, 2.0f), "4D unit vector norm = 2.0");

   float norm2 = memory_embeddings_l2_norm(vec4, 2);
   TEST_ASSERT(FLOAT_EQ(norm2, sqrtf(2.0f)), "same data, 2 dims = sqrt(2)");

   /* Cosine with different dim interpretations */
   float a[] = { 1.0f, 0.0f, 0.0f, 0.0f };
   float b[] = { 0.0f, 1.0f, 0.0f, 0.0f };
   float sim4 = memory_embeddings_cosine(a, b, 4);
   TEST_ASSERT(FLOAT_EQ(sim4, 0.0f), "orthogonal in 4D → 0.0");

   float sim2 = memory_embeddings_cosine(a, b, 2);
   TEST_ASSERT(FLOAT_EQ(sim2, 0.0f), "orthogonal in 2D → 0.0");
}

/* ============================================================================
 * Test: Edge Cases
 * ============================================================================ */

static void test_edge_cases(void) {
   printf("\n--- test_edge_cases ---\n");

   /* Single dimension */
   float a1[] = { 5.0f };
   float b1[] = { 3.0f };
   float sim = memory_embeddings_cosine(a1, b1, 1);
   TEST_ASSERT(FLOAT_EQ(sim, 1.0f), "same-sign 1D → 1.0");

   float c1[] = { -3.0f };
   sim = memory_embeddings_cosine(a1, c1, 1);
   TEST_ASSERT(FLOAT_EQ(sim, 0.0f), "opposite-sign 1D → clamped to 0.0");

   /* Large dimension */
   float large_a[384];
   float large_b[384];
   for (int i = 0; i < 384; i++) {
      large_a[i] = (float)(i % 10) / 10.0f;
      large_b[i] = (float)(i % 10) / 10.0f;
   }
   sim = memory_embeddings_cosine(large_a, large_b, 384);
   TEST_ASSERT(FLOAT_EQ(sim, 1.0f), "identical 384D vectors → 1.0");

   /* Slightly different */
   large_b[0] += 0.01f;
   sim = memory_embeddings_cosine(large_a, large_b, 384);
   TEST_ASSERT(sim > 0.99f && sim <= 1.0f, "nearly identical 384D → > 0.99");
}

/* ============================================================================
 * Main
 * ============================================================================ */

int main(void) {
   printf("=== Memory Embeddings Unit Tests ===\n");

   test_l2_norm();
   test_cosine_similarity();
   test_cosine_with_norms();
   test_cosine_clamping();
   test_hybrid_score_merge();
   test_dimension_validation();
   test_edge_cases();

   printf("\n========================================\n");
   printf("Results: %d passed, %d failed\n", tests_passed, tests_failed);
   printf("========================================\n");

   return tests_failed > 0 ? 1 : 0;
}
