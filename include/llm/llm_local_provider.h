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
 * Local LLM Provider Detection and Management
 *
 * This module handles local LLM server providers (Ollama, llama.cpp, etc.):
 * - Auto-detects provider type from endpoint responses
 * - Queries context size using provider-specific endpoints
 * - Lists available models from the local server
 * - Provides caching for detection results and model lists
 */

#ifndef LLM_LOCAL_PROVIDER_H
#define LLM_LOCAL_PROVIDER_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =============================================================================
 * Constants
 * ============================================================================= */

#define LLM_LOCAL_MAX_MODELS 50         /* Maximum cached models */
#define LLM_LOCAL_MODEL_NAME_MAX 64     /* Maximum model name length */
#define LLM_LOCAL_PROBE_TIMEOUT_MS 1000 /* Detection probe timeout */
#define LLM_LOCAL_MODEL_CACHE_TTL 300   /* Model list cache TTL (5 minutes) */

/* =============================================================================
 * Types
 * ============================================================================= */

/**
 * @brief Local LLM provider types
 *
 * Detected automatically by probing provider-specific endpoints,
 * or can be set explicitly via configuration.
 */
typedef enum {
   LOCAL_PROVIDER_UNKNOWN,   /**< Not yet detected */
   LOCAL_PROVIDER_LLAMA_CPP, /**< llama.cpp server (/props endpoint) */
   LOCAL_PROVIDER_OLLAMA,    /**< Ollama server (/api/tags endpoint) */
   LOCAL_PROVIDER_GENERIC    /**< Generic OpenAI-compatible (fallback) */
} local_provider_t;

/**
 * @brief Model information from local LLM server
 */
typedef struct {
   char name[LLM_LOCAL_MODEL_NAME_MAX]; /**< Model name/identifier */
   bool loaded;                         /**< True if currently loaded in memory */
} llm_local_model_t;

/* =============================================================================
 * Lifecycle Functions
 * ============================================================================= */

/**
 * @brief Initialize the local provider module
 *
 * Sets up mutex and clears cached state.
 *
 * @return 0 on success, non-zero on failure
 */
int llm_local_provider_init(void);

/**
 * @brief Clean up local provider module resources
 */
void llm_local_provider_cleanup(void);

/* =============================================================================
 * Provider Detection Functions
 * ============================================================================= */

/**
 * @brief Detect local LLM provider type from endpoint
 *
 * Probes the endpoint to determine the provider type:
 * 1. Checks config override first (if provider explicitly set)
 * 2. Probes /api/tags for Ollama (1s timeout)
 * 3. Probes /props for llama.cpp (1s timeout)
 * 4. Falls back to GENERIC if neither responds
 *
 * Result is cached for subsequent calls. Use llm_local_invalidate_cache()
 * to force re-detection.
 *
 * @param endpoint Local LLM endpoint URL (e.g., "http://127.0.0.1:11434")
 * @return Detected provider type
 */
local_provider_t llm_local_detect_provider(const char *endpoint);

/**
 * @brief Get cached provider type (without re-detection)
 *
 * Returns the last detected provider, or LOCAL_PROVIDER_UNKNOWN if
 * detection hasn't been performed yet.
 *
 * @return Cached provider type
 */
local_provider_t llm_local_get_provider(void);

/**
 * @brief Invalidate cached provider detection
 *
 * Forces re-detection on next call to llm_local_detect_provider().
 * Call this when the endpoint configuration changes.
 */
void llm_local_invalidate_cache(void);

/**
 * @brief Get human-readable provider name
 *
 * @param provider Provider enum value
 * @return String name (e.g., "Ollama", "llama.cpp", "Generic")
 */
const char *llm_local_provider_name(local_provider_t provider);

/* =============================================================================
 * Context Size Functions
 * ============================================================================= */

/**
 * @brief Query context size from local LLM server
 *
 * Uses provider-specific endpoints:
 * - Ollama: POST /api/show with model name
 * - llama.cpp: GET /props
 *
 * Falls back to LLM_CONTEXT_DEFAULT_LOCAL (8192) if query fails.
 *
 * @param endpoint Local LLM endpoint URL
 * @param model Model name (required for Ollama, optional for llama.cpp)
 * @return Context size in tokens
 */
int llm_local_query_context_size(const char *endpoint, const char *model);

/* =============================================================================
 * Model Listing Functions
 * ============================================================================= */

/**
 * @brief List available models from local LLM server
 *
 * Queries the server for available models:
 * - Ollama: GET /api/tags -> models[].name
 * - llama.cpp/Generic: GET /v1/models -> data[].id
 *
 * Results are cached with LLM_LOCAL_MODEL_CACHE_TTL (5 minutes).
 * Model names are validated to contain only safe characters.
 *
 * @param endpoint Local LLM endpoint URL
 * @param models Output array of model info structs
 * @param max_models Maximum number of models to return
 * @param out_count Output: number of models found (can be 0 if no models)
 * @return 0 on success, non-zero on error (invalid params, connection failure)
 */
int llm_local_list_models(const char *endpoint,
                          llm_local_model_t *models,
                          size_t max_models,
                          size_t *out_count);

/**
 * @brief Invalidate cached model list
 *
 * Forces re-fetch on next call to llm_local_list_models().
 */
void llm_local_invalidate_models_cache(void);

/**
 * @brief Check if a model name contains only valid characters
 *
 * Valid characters: a-z, A-Z, 0-9, _, -, ., :
 * Used to prevent XSS and injection attacks.
 *
 * @param name Model name to validate
 * @return true if valid, false otherwise
 */
bool llm_local_is_valid_model_name(const char *name);

#ifdef __cplusplus
}
#endif

#endif /* LLM_LOCAL_PROVIDER_H */
