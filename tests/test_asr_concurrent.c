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
 * ASR Concurrent Context Test
 *
 * Verifies that multiple ASR contexts can be created and used concurrently
 * in parallel threads - a prerequisite for multi-client architecture.
 *
 * Tests:
 * 1. Multiple context creation (2 contexts with same model)
 * 2. Independent processing (each context processes audio separately)
 * 3. Parallel thread execution (2 threads processing simultaneously)
 * 4. No interference between contexts
 */

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "asr/asr_interface.h"

#define SAMPLE_RATE 16000
#define AUDIO_CHUNK_SAMPLES 1600  // 100ms at 16kHz
#define NUM_CHUNKS 10             // 1 second of audio per thread

#define TEST_PASS "\033[32m[PASS]\033[0m"
#define TEST_FAIL "\033[31m[FAIL]\033[0m"

static int tests_passed = 0;
static int tests_failed = 0;

// Barrier for synchronizing thread start
static pthread_barrier_t start_barrier;

void test_result(const char *test_name, int passed) {
   if (passed) {
      printf("%s %s\n", TEST_PASS, test_name);
      tests_passed++;
   } else {
      printf("%s %s\n", TEST_FAIL, test_name);
      tests_failed++;
   }
}

/**
 * @brief Generate synthetic audio (sine wave)
 */
static void generate_audio(int16_t *buffer, size_t num_samples, float frequency) {
   for (size_t i = 0; i < num_samples; i++) {
      double t = (double)i / SAMPLE_RATE;
      buffer[i] = (int16_t)(8000.0 * sin(2.0 * M_PI * frequency * t));
   }
}

/**
 * @brief Thread argument structure
 */
typedef struct {
   int thread_id;
   asr_context_t *ctx;
   int success;
   int chunks_processed;
   char error_msg[256];
} thread_arg_t;

/**
 * @brief Thread function that processes audio through ASR context
 */
static void *asr_worker_thread(void *arg) {
   thread_arg_t *targ = (thread_arg_t *)arg;
   int16_t audio[AUDIO_CHUNK_SAMPLES];

   // Generate unique audio for this thread (different frequency)
   float freq = 200.0f + (targ->thread_id * 100.0f);

   targ->success = 1;
   targ->chunks_processed = 0;

   // Wait for all threads to be ready
   pthread_barrier_wait(&start_barrier);

   // Process multiple chunks of audio
   for (int i = 0; i < NUM_CHUNKS; i++) {
      generate_audio(audio, AUDIO_CHUNK_SAMPLES, freq);

      asr_result_t *result = asr_process_partial(targ->ctx, audio, AUDIO_CHUNK_SAMPLES);
      if (!result) {
         snprintf(targ->error_msg, sizeof(targ->error_msg),
                  "Thread %d: asr_process_partial returned NULL on chunk %d", targ->thread_id, i);
         targ->success = 0;
         return NULL;
      }

      // Free partial result
      asr_result_free(result);
      targ->chunks_processed++;
   }

   // Finalize and get final result
   asr_result_t *final = asr_finalize(targ->ctx);
   if (!final) {
      snprintf(targ->error_msg, sizeof(targ->error_msg), "Thread %d: asr_finalize returned NULL",
               targ->thread_id);
      targ->success = 0;
      return NULL;
   }

   printf("  Thread %d: Processed %d chunks, final text: \"%s\"\n", targ->thread_id,
          targ->chunks_processed, final->text ? final->text : "(empty)");

   asr_result_free(final);

   return NULL;
}

/**
 * @brief Get model path based on ASR engine
 */
static const char *get_model_path(asr_engine_type_t engine) {
   static char path[512];
   const char *home = getenv("HOME");

   if (!home) {
      return NULL;
   }

   if (engine == ASR_ENGINE_VOSK) {
      // Vosk model directory
      snprintf(path, sizeof(path), "%s/code/The-OASIS-Project/dawn/vosk-model-en-us-0.22", home);
   } else {
      // Whisper model file - use tiny model for fast testing
      snprintf(path, sizeof(path),
               "%s/code/The-OASIS-Project/dawn/whisper.cpp/models/ggml-tiny.bin", home);
   }

   return path;
}

/**
 * @brief Test 1: Multiple context creation
 */
static void test_multiple_context_creation(asr_engine_type_t engine) {
   const char *model_path = get_model_path(engine);
   if (!model_path) {
      printf("  (HOME env not set - skipping)\n");
      test_result("Multiple context creation", 0);
      return;
   }

   printf("  Model path: %s\n", model_path);

   // Create first context
   asr_context_t *ctx1 = asr_init(engine, model_path, SAMPLE_RATE);
   if (!ctx1) {
      printf("  (Failed to create first context - model may not exist)\n");
      test_result("Multiple context creation", 0);
      return;
   }

   // Create second context with same model
   asr_context_t *ctx2 = asr_init(engine, model_path, SAMPLE_RATE);
   if (!ctx2) {
      printf("  (Failed to create second context)\n");
      asr_cleanup(ctx1);
      test_result("Multiple context creation", 0);
      return;
   }

   test_result("Multiple context creation", ctx1 != NULL && ctx2 != NULL && ctx1 != ctx2);

   // Cleanup
   asr_cleanup(ctx1);
   asr_cleanup(ctx2);
}

/**
 * @brief Test 2: Independent sequential processing
 */
static void test_independent_processing(asr_engine_type_t engine) {
   const char *model_path = get_model_path(engine);
   if (!model_path) {
      test_result("Independent sequential processing", 0);
      return;
   }

   asr_context_t *ctx1 = asr_init(engine, model_path, SAMPLE_RATE);
   asr_context_t *ctx2 = asr_init(engine, model_path, SAMPLE_RATE);

   if (!ctx1 || !ctx2) {
      if (ctx1)
         asr_cleanup(ctx1);
      if (ctx2)
         asr_cleanup(ctx2);
      test_result("Independent sequential processing", 0);
      return;
   }

   int16_t audio1[AUDIO_CHUNK_SAMPLES];
   int16_t audio2[AUDIO_CHUNK_SAMPLES];

   // Different frequencies for each
   generate_audio(audio1, AUDIO_CHUNK_SAMPLES, 200.0f);
   generate_audio(audio2, AUDIO_CHUNK_SAMPLES, 400.0f);

   // Process on ctx1
   asr_result_t *r1 = asr_process_partial(ctx1, audio1, AUDIO_CHUNK_SAMPLES);
   int success1 = (r1 != NULL);
   if (r1)
      asr_result_free(r1);

   // Process on ctx2
   asr_result_t *r2 = asr_process_partial(ctx2, audio2, AUDIO_CHUNK_SAMPLES);
   int success2 = (r2 != NULL);
   if (r2)
      asr_result_free(r2);

   // Finalize both
   asr_result_t *f1 = asr_finalize(ctx1);
   asr_result_t *f2 = asr_finalize(ctx2);

   int success = success1 && success2 && f1 && f2;

   if (f1)
      asr_result_free(f1);
   if (f2)
      asr_result_free(f2);

   asr_cleanup(ctx1);
   asr_cleanup(ctx2);

   test_result("Independent sequential processing", success);
}

/**
 * @brief Test 3: Parallel thread execution
 */
static void test_parallel_threads(asr_engine_type_t engine) {
   const char *model_path = get_model_path(engine);
   if (!model_path) {
      test_result("Parallel thread execution", 0);
      return;
   }

   // Create two contexts
   asr_context_t *ctx1 = asr_init(engine, model_path, SAMPLE_RATE);
   asr_context_t *ctx2 = asr_init(engine, model_path, SAMPLE_RATE);

   if (!ctx1 || !ctx2) {
      if (ctx1)
         asr_cleanup(ctx1);
      if (ctx2)
         asr_cleanup(ctx2);
      test_result("Parallel thread execution", 0);
      return;
   }

   // Initialize barrier for 2 threads
   pthread_barrier_init(&start_barrier, NULL, 2);

   // Setup thread arguments
   thread_arg_t args[2] = { { .thread_id = 1, .ctx = ctx1, .success = 0, .chunks_processed = 0 },
                            { .thread_id = 2, .ctx = ctx2, .success = 0, .chunks_processed = 0 } };

   // Create threads
   pthread_t threads[2];
   pthread_create(&threads[0], NULL, asr_worker_thread, &args[0]);
   pthread_create(&threads[1], NULL, asr_worker_thread, &args[1]);

   // Wait for both to complete
   pthread_join(threads[0], NULL);
   pthread_join(threads[1], NULL);

   // Check results
   int success = args[0].success && args[1].success;

   if (!args[0].success) {
      printf("  Thread 1 error: %s\n", args[0].error_msg);
   }
   if (!args[1].success) {
      printf("  Thread 2 error: %s\n", args[1].error_msg);
   }

   pthread_barrier_destroy(&start_barrier);
   asr_cleanup(ctx1);
   asr_cleanup(ctx2);

   test_result("Parallel thread execution", success);
}

/**
 * @brief Test 4: No interference between contexts
 *
 * Verifies that resetting one context doesn't affect another
 */
static void test_no_interference(asr_engine_type_t engine) {
   const char *model_path = get_model_path(engine);
   if (!model_path) {
      test_result("No interference between contexts", 0);
      return;
   }

   asr_context_t *ctx1 = asr_init(engine, model_path, SAMPLE_RATE);
   asr_context_t *ctx2 = asr_init(engine, model_path, SAMPLE_RATE);

   if (!ctx1 || !ctx2) {
      if (ctx1)
         asr_cleanup(ctx1);
      if (ctx2)
         asr_cleanup(ctx2);
      test_result("No interference between contexts", 0);
      return;
   }

   int16_t audio[AUDIO_CHUNK_SAMPLES];
   generate_audio(audio, AUDIO_CHUNK_SAMPLES, 300.0f);

   // Process audio on both
   asr_result_t *r1 = asr_process_partial(ctx1, audio, AUDIO_CHUNK_SAMPLES);
   asr_result_t *r2 = asr_process_partial(ctx2, audio, AUDIO_CHUNK_SAMPLES);

   if (r1)
      asr_result_free(r1);
   if (r2)
      asr_result_free(r2);

   // Reset ctx1 only
   int reset_result = asr_reset(ctx1);

   // ctx2 should still be able to finalize with accumulated audio
   asr_result_t *final2 = asr_finalize(ctx2);

   // ctx1 should finalize with empty/reset state
   asr_result_t *final1 = asr_finalize(ctx1);

   int success = (reset_result == ASR_SUCCESS) && final1 && final2;

   if (final1)
      asr_result_free(final1);
   if (final2)
      asr_result_free(final2);

   asr_cleanup(ctx1);
   asr_cleanup(ctx2);

   test_result("No interference between contexts", success);
}

int main(int argc, char *argv[]) {
   // Determine which engine to test
   asr_engine_type_t engine = ASR_ENGINE_WHISPER;  // Default to Whisper

#ifdef ENABLE_VOSK
   engine = ASR_ENGINE_VOSK;
#endif

   // Allow override via command line
   if (argc > 1) {
      if (strcmp(argv[1], "vosk") == 0) {
         engine = ASR_ENGINE_VOSK;
      } else if (strcmp(argv[1], "whisper") == 0) {
         engine = ASR_ENGINE_WHISPER;
      }
   }

   printf("\n=== ASR Concurrent Context Tests ===\n");
   printf("Engine: %s\n\n", asr_engine_name(engine));

   // Run tests
   printf("Test 1: Multiple context creation\n");
   test_multiple_context_creation(engine);

   printf("\nTest 2: Independent sequential processing\n");
   test_independent_processing(engine);

   printf("\nTest 3: Parallel thread execution\n");
   test_parallel_threads(engine);

   printf("\nTest 4: No interference between contexts\n");
   test_no_interference(engine);

   // Summary
   printf("\n=== Test Summary ===\n");
   printf("Passed: %d\n", tests_passed);
   printf("Failed: %d\n", tests_failed);
   printf("Total:  %d\n", tests_passed + tests_failed);

   if (tests_failed == 0) {
      printf("\n\033[32mAll tests passed! ASR multi-context support verified.\033[0m\n");
      printf("Multi-client architecture can proceed to Phase 2.\n\n");
      return 0;
   } else {
      printf("\n\033[31m%d test(s) failed.\033[0m\n", tests_failed);
      printf("WARNING: Multi-client architecture may have issues with concurrent ASR.\n\n");
      return 1;
   }
}
