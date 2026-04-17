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
 * Memory Embeddings Core
 *
 * Provider abstraction, math utilities, in-memory cache, hybrid search,
 * and background backfill for semantic memory search.
 */

#include "memory/memory_embeddings.h"

#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "config/dawn_config.h"
#include "core/embedding_engine.h"
#include "logging.h"
#include "memory/memory_db.h"

/* In-memory embedding cache for fast cosine search */
static struct {
   pthread_mutex_t mutex;
   int user_id;
   int64_t *ids;
   float *embeddings; /* flat: count * dims */
   float *norms;
   int count;
   int capacity;
   int dims;
   bool valid;
   atomic_bool dirty; /* set by backfill after each store */
} s_cache = {
   .mutex = PTHREAD_MUTEX_INITIALIZER,
};

/* Entity embedding cache (separate from fact cache) */
#define ENTITY_CACHE_CAP 500

static struct {
   pthread_mutex_t mutex;
   int user_id;
   int64_t *ids;
   char (*names)[MEMORY_ENTITY_NAME_MAX];
   char (*types)[MEMORY_ENTITY_TYPE_MAX];
   float *embeddings; /* flat: count * dims */
   float *norms;
   int count;
   int dims;
   bool valid;
   atomic_bool dirty;
} s_entity_cache = {
   .mutex = PTHREAD_MUTEX_INITIALIZER,
};

/* Forward declaration */
static void entity_cache_free(void);

/* Backfill thread state */
static pthread_t s_backfill_thread;
static atomic_bool s_backfill_running;
static atomic_bool s_backfill_shutdown;
static int s_backfill_user_id;

/* =============================================================================
 * Math Utilities — delegate to shared embedding engine
 * ============================================================================= */

float memory_embeddings_l2_norm(const float *vec, int dims) {
   return embedding_engine_l2_norm(vec, dims);
}

float memory_embeddings_cosine_with_norms(const float *a,
                                          const float *b,
                                          int dims,
                                          float norm_a,
                                          float norm_b) {
   return embedding_engine_cosine_with_norms(a, b, dims, norm_a, norm_b);
}

float memory_embeddings_cosine(const float *a, const float *b, int dims) {
   return embedding_engine_cosine(a, b, dims);
}

/* =============================================================================
 * Cache Management
 * ============================================================================= */

static void cache_free_data(void) {
   free(s_cache.ids);
   free(s_cache.embeddings);
   free(s_cache.norms);
   s_cache.ids = NULL;
   s_cache.embeddings = NULL;
   s_cache.norms = NULL;
   s_cache.count = 0;
   s_cache.capacity = 0;
   s_cache.valid = false;
}

static int cache_load(int user_id) {
   /* Already valid for this user? */
   if (s_cache.valid && s_cache.user_id == user_id && !atomic_load(&s_cache.dirty))
      return 0;

   cache_free_data();

   int dims = embedding_engine_dims();
   if (dims <= 0)
      return -1;

   /* Allocate for EMBEDDING_SEARCH_CAP entries */
   int cap = EMBEDDING_SEARCH_CAP;
   s_cache.ids = malloc(cap * sizeof(int64_t));
   s_cache.embeddings = malloc((size_t)cap * (size_t)dims * sizeof(float));
   s_cache.norms = malloc(cap * sizeof(float));

   if (!s_cache.ids || !s_cache.embeddings || !s_cache.norms) {
      cache_free_data();
      return -1;
   }

   int loaded = memory_db_fact_get_embeddings(user_id, dims, s_cache.ids, s_cache.embeddings,
                                              s_cache.norms, cap);
   if (loaded < 0) {
      cache_free_data();
      return -1;
   }

   s_cache.count = loaded;
   s_cache.capacity = cap;
   s_cache.dims = dims;
   s_cache.user_id = user_id;
   s_cache.valid = true;
   atomic_store(&s_cache.dirty, false);

   OLOG_INFO("memory_embeddings: loaded %d embeddings into cache for user %d", loaded, user_id);
   return 0;
}

void memory_embeddings_invalidate_cache(void) {
   atomic_store(&s_cache.dirty, true);
}

/* =============================================================================
 * Init / Cleanup — delegate provider management to shared embedding engine
 * ============================================================================= */

int memory_embeddings_init(void) {
   return embedding_engine_init();
}

void memory_embeddings_cleanup(void) {
   /* Stop backfill thread */
   if (atomic_load(&s_backfill_running)) {
      atomic_store(&s_backfill_shutdown, true);
      pthread_join(s_backfill_thread, NULL);
      atomic_store(&s_backfill_running, false);
   }

   /* Free caches */
   pthread_mutex_lock(&s_cache.mutex);
   cache_free_data();
   pthread_mutex_unlock(&s_cache.mutex);

   pthread_mutex_lock(&s_entity_cache.mutex);
   entity_cache_free();
   pthread_mutex_unlock(&s_entity_cache.mutex);

   /* Provider cleanup handled by embedding_engine_cleanup() in dawn.c shutdown */
}

bool memory_embeddings_available(void) {
   return embedding_engine_available();
}

int memory_embeddings_dims(void) {
   return embedding_engine_dims();
}

/* =============================================================================
 * Embedding Generation
 * ============================================================================= */

int memory_embeddings_embed(const char *text, float *out, int *out_dims) {
   if (!text || !out || !out_dims)
      return -1;

   return embedding_engine_embed(text, out, MAX_EMBEDDING_DIMS, out_dims);
}

int memory_embeddings_embed_and_store(int user_id, int64_t fact_id, const char *text) {
   if (!embedding_engine_available() || !text)
      return -1;

   float embedding[MAX_EMBEDDING_DIMS];
   int dims = 0;

   int rc = embedding_engine_embed(text, embedding, MAX_EMBEDDING_DIMS, &dims);
   if (rc != 0 || dims <= 0)
      return rc;

   float norm = memory_embeddings_l2_norm(embedding, dims);

   rc = memory_db_fact_update_embedding(user_id, fact_id, embedding, dims, norm);
   if (rc == MEMORY_DB_SUCCESS) {
      memory_embeddings_invalidate_cache();
   }
   return rc;
}

/* =============================================================================
 * Entity Embedding Support
 * ============================================================================= */

static void entity_cache_free(void) {
   free(s_entity_cache.ids);
   free(s_entity_cache.names);
   free(s_entity_cache.types);
   free(s_entity_cache.embeddings);
   free(s_entity_cache.norms);
   s_entity_cache.ids = NULL;
   s_entity_cache.names = NULL;
   s_entity_cache.types = NULL;
   s_entity_cache.embeddings = NULL;
   s_entity_cache.norms = NULL;
   s_entity_cache.count = 0;
   s_entity_cache.valid = false;
}

static int entity_cache_load(int user_id) {
   if (s_entity_cache.valid && s_entity_cache.user_id == user_id &&
       !atomic_load(&s_entity_cache.dirty))
      return 0;

   entity_cache_free();

   int dims = embedding_engine_dims();
   if (dims <= 0)
      return -1;

   s_entity_cache.ids = malloc(ENTITY_CACHE_CAP * sizeof(int64_t));
   s_entity_cache.names = malloc(ENTITY_CACHE_CAP * sizeof(*s_entity_cache.names));
   s_entity_cache.types = malloc(ENTITY_CACHE_CAP * sizeof(*s_entity_cache.types));
   s_entity_cache.embeddings = malloc((size_t)ENTITY_CACHE_CAP * (size_t)dims * sizeof(float));
   s_entity_cache.norms = malloc(ENTITY_CACHE_CAP * sizeof(float));

   if (!s_entity_cache.ids || !s_entity_cache.names || !s_entity_cache.types ||
       !s_entity_cache.embeddings || !s_entity_cache.norms) {
      entity_cache_free();
      return -1;
   }

   int loaded = memory_db_entity_get_embeddings(user_id, dims, s_entity_cache.ids,
                                                s_entity_cache.names, s_entity_cache.types,
                                                s_entity_cache.embeddings, s_entity_cache.norms,
                                                ENTITY_CACHE_CAP);
   if (loaded < 0) {
      entity_cache_free();
      return -1;
   }

   s_entity_cache.count = loaded;
   s_entity_cache.dims = dims;
   s_entity_cache.user_id = user_id;
   s_entity_cache.valid = true;
   atomic_store(&s_entity_cache.dirty, false);

   OLOG_INFO("memory_embeddings: loaded %d entity embeddings into cache for user %d", loaded,
             user_id);
   return 0;
}

int memory_embeddings_embed_and_store_entity(int64_t entity_id, int user_id, const char *text) {
   if (!embedding_engine_available() || !text)
      return -1;

   float embedding[MAX_EMBEDDING_DIMS];
   int dims = 0;

   int rc = embedding_engine_embed(text, embedding, MAX_EMBEDDING_DIMS, &dims);
   if (rc != 0 || dims <= 0)
      return rc;

   float norm = memory_embeddings_l2_norm(embedding, dims);

   return memory_db_entity_update_embedding(entity_id, user_id, embedding, dims, norm);
}

void memory_embeddings_invalidate_entity_cache(void) {
   atomic_store(&s_entity_cache.dirty, true);
}

int memory_embeddings_entity_search(int user_id,
                                    const char *query,
                                    const char *type_filter,
                                    int64_t *out_ids,
                                    char out_names[][MEMORY_ENTITY_NAME_MAX],
                                    char out_types[][MEMORY_ENTITY_TYPE_MAX],
                                    float *out_scores,
                                    int max_results) {
   if (!memory_embeddings_available() || !query || !out_ids || max_results <= 0)
      return 0;

   /* Embed the query */
   float query_emb[MAX_EMBEDDING_DIMS];
   int dims = 0;
   if (memory_embeddings_embed(query, query_emb, &dims) != 0 || dims != embedding_engine_dims())
      return 0;

   float query_norm = memory_embeddings_l2_norm(query_emb, dims);

   pthread_mutex_lock(&s_entity_cache.mutex);
   if (entity_cache_load(user_id) != 0) {
      pthread_mutex_unlock(&s_entity_cache.mutex);
      return 0;
   }

   /* Score all cached entities by cosine similarity */
   typedef struct {
      int idx;
      float score;
   } scored_t;
   scored_t scored[ENTITY_CACHE_CAP];
   int scored_count = 0;

   for (int i = 0; i < s_entity_cache.count; i++) {
      /* Apply type filter if specified */
      if (type_filter && type_filter[0] != '\0' &&
          strcmp(s_entity_cache.types[i], type_filter) != 0) {
         continue;
      }

      float cosine = memory_embeddings_cosine_with_norms(query_emb,
                                                         s_entity_cache.embeddings + i * dims, dims,
                                                         query_norm, s_entity_cache.norms[i]);

      if (cosine > 0.4f) {
         scored[scored_count].idx = i;
         scored[scored_count].score = cosine;
         scored_count++;
      }
   }

   /* Sort by score descending (insertion sort — small N) */
   for (int i = 1; i < scored_count; i++) {
      scored_t tmp = scored[i];
      int j = i - 1;
      while (j >= 0 && scored[j].score < tmp.score) {
         scored[j + 1] = scored[j];
         j--;
      }
      scored[j + 1] = tmp;
   }

   /* Copy top results */
   int result_count = scored_count > max_results ? max_results : scored_count;
   for (int i = 0; i < result_count; i++) {
      int idx = scored[i].idx;
      out_ids[i] = s_entity_cache.ids[idx];
      if (out_names) {
         strncpy(out_names[i], s_entity_cache.names[idx], MEMORY_ENTITY_NAME_MAX - 1);
         out_names[i][MEMORY_ENTITY_NAME_MAX - 1] = '\0';
      }
      if (out_types) {
         strncpy(out_types[i], s_entity_cache.types[idx], MEMORY_ENTITY_TYPE_MAX - 1);
         out_types[i][MEMORY_ENTITY_TYPE_MAX - 1] = '\0';
      }
      if (out_scores)
         out_scores[i] = scored[i].score;
   }

   pthread_mutex_unlock(&s_entity_cache.mutex);
   return result_count;
}

/* =============================================================================
 * Hybrid Search
 * ============================================================================= */

int memory_embeddings_hybrid_search(int user_id,
                                    const char *query,
                                    const int64_t *keyword_facts,
                                    const int *keyword_scores,
                                    int keyword_count,
                                    int token_count,
                                    embedding_search_result_t *out_results,
                                    int max_results) {
   if (!out_results || max_results <= 0)
      return 0;

   float kw_weight = g_config.memory.embedding_keyword_weight;
   float vec_weight = g_config.memory.embedding_vector_weight;

   /* If no embeddings available, return keyword results directly */
   if (!memory_embeddings_available() || !query) {
      int count = keyword_count > max_results ? max_results : keyword_count;
      for (int i = 0; i < count; i++) {
         out_results[i].fact_id = keyword_facts[i];
         out_results[i].score = (token_count > 0) ? (float)keyword_scores[i] / (float)token_count
                                                  : 1.0f;
      }
      return count;
   }

   /* Embed the query */
   float query_emb[MAX_EMBEDDING_DIMS];
   int dims = 0;
   if (memory_embeddings_embed(query, query_emb, &dims) != 0 || dims != embedding_engine_dims()) {
      /* Embedding failed — fall back to keyword only */
      int count = keyword_count > max_results ? max_results : keyword_count;
      for (int i = 0; i < count; i++) {
         out_results[i].fact_id = keyword_facts[i];
         out_results[i].score = (token_count > 0) ? (float)keyword_scores[i] / (float)token_count
                                                  : 1.0f;
      }
      return count;
   }

   float query_norm = memory_embeddings_l2_norm(query_emb, dims);

   /* Load cache under lock */
   pthread_mutex_lock(&s_cache.mutex);
   if (cache_load(user_id) != 0) {
      pthread_mutex_unlock(&s_cache.mutex);
      /* Cache load failed — keyword only */
      int count = keyword_count > max_results ? max_results : keyword_count;
      for (int i = 0; i < count; i++) {
         out_results[i].fact_id = keyword_facts[i];
         out_results[i].score = (token_count > 0) ? (float)keyword_scores[i] / (float)token_count
                                                  : 1.0f;
      }
      return count;
   }

   /* Build result set: start with keyword facts, add vector matches */
   /* Temporary merged set */
   int merged_cap = keyword_count + s_cache.count;
   if (merged_cap > EMBEDDING_SEARCH_CAP)
      merged_cap = EMBEDDING_SEARCH_CAP;

   /* Stack-allocated — EMBEDDING_SEARCH_CAP * 12 bytes = 24KB, well within 8MB stack */
   embedding_search_result_t merged[EMBEDDING_SEARCH_CAP];
   int merged_count = 0;

   /* Score all cached embeddings by vector similarity */
   for (int i = 0; i < s_cache.count && merged_count < merged_cap; i++) {
      float cosine = memory_embeddings_cosine_with_norms(query_emb, s_cache.embeddings + i * dims,
                                                         dims, query_norm, s_cache.norms[i]);

      /* Find keyword score for this fact (if any) */
      float kw_score = 0.0f;
      for (int k = 0; k < keyword_count; k++) {
         if (keyword_facts[k] == s_cache.ids[i]) {
            kw_score = (token_count > 0) ? (float)keyword_scores[k] / (float)token_count : 1.0f;
            break;
         }
      }

      float hybrid = kw_weight * kw_score + vec_weight * cosine;
      if (hybrid > 0.01f) { /* Skip near-zero results */
         merged[merged_count].fact_id = s_cache.ids[i];
         merged[merged_count].score = hybrid;
         merged_count++;
      }
   }

   /* Add keyword-only results (facts without embeddings) */
   for (int k = 0; k < keyword_count; k++) {
      bool found = false;
      for (int m = 0; m < merged_count; m++) {
         if (merged[m].fact_id == keyword_facts[k]) {
            found = true;
            break;
         }
      }
      if (!found && merged_count < merged_cap) {
         float kw_score = (token_count > 0) ? (float)keyword_scores[k] / (float)token_count : 1.0f;
         merged[merged_count].fact_id = keyword_facts[k];
         merged[merged_count].score = kw_weight * kw_score; /* keyword only, no vector penalty */
         merged_count++;
      }
   }

   pthread_mutex_unlock(&s_cache.mutex);

   /* Sort by score descending (insertion sort — small N) */
   for (int i = 1; i < merged_count; i++) {
      embedding_search_result_t tmp = merged[i];
      int j = i - 1;
      while (j >= 0 && merged[j].score < tmp.score) {
         merged[j + 1] = merged[j];
         j--;
      }
      merged[j + 1] = tmp;
   }

   /* Copy top results */
   int result_count = merged_count > max_results ? max_results : merged_count;
   memcpy(out_results, merged, result_count * sizeof(embedding_search_result_t));

   return result_count;
}

/* =============================================================================
 * Background Backfill
 * ============================================================================= */

static void *backfill_thread_fn(void *arg) {
   (void)arg;
   int user_id = s_backfill_user_id;
   int dims = embedding_engine_dims();

   OLOG_INFO("memory_embeddings: backfill started for user %d", user_id);

   int total_embedded = 0;
   int batch_size = 50;

   while (!atomic_load(&s_backfill_shutdown)) {
      int64_t ids[50];
      char texts[50][512];

      int count = memory_db_fact_list_without_embedding(user_id, dims, ids, texts, batch_size);
      if (count <= 0)
         break;

      for (int i = 0; i < count && !atomic_load(&s_backfill_shutdown); i++) {
         if (texts[i][0] == '\0')
            continue;

         if (memory_embeddings_embed_and_store(user_id, ids[i], texts[i]) == 0) {
            total_embedded++;
         }

         /* Throttle: 50ms between embeddings to avoid hogging CPU */
         usleep(50000);
      }
   }

   OLOG_INFO("memory_embeddings: backfill complete, embedded %d facts", total_embedded);
   atomic_store(&s_backfill_running, false);
   return NULL;
}

void memory_embeddings_start_backfill(int user_id) {
   if (!memory_embeddings_available())
      return;

   if (atomic_load(&s_backfill_running)) {
      OLOG_INFO("memory_embeddings: backfill already running");
      return;
   }

   s_backfill_user_id = user_id;
   atomic_store(&s_backfill_shutdown, false);
   atomic_store(&s_backfill_running, true);

   if (pthread_create(&s_backfill_thread, NULL, backfill_thread_fn, NULL) != 0) {
      OLOG_ERROR("memory_embeddings: failed to create backfill thread");
      atomic_store(&s_backfill_running, false);
   }
}
