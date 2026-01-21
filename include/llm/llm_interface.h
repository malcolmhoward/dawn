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
 */

#ifndef LLM_INTERFACE_H
#define LLM_INTERFACE_H

#include <json-c/json.h>
#include <stdbool.h>
#include <stddef.h>

/**
 * @brief Cloud provider types
 *
 * Automatically detected based on API keys in secrets.toml.
 * If both providers are configured, provider can be selected via
 * --cloud-provider command-line argument or dawn.toml config.
 */
typedef enum {
   CLOUD_PROVIDER_OPENAI, /**< OpenAI (GPT models) */
   CLOUD_PROVIDER_CLAUDE, /**< Anthropic Claude models */
   CLOUD_PROVIDER_GEMINI, /**< Google Gemini models (OpenAI-compatible API) */
   CLOUD_PROVIDER_NONE    /**< No cloud provider configured */
} cloud_provider_t;

/**
 * @brief LLM type (local vs cloud)
 */
typedef enum {
   LLM_LOCAL,    /**< Local LLM server (e.g., llama.cpp) */
   LLM_CLOUD,    /**< Cloud LLM provider (OpenAI or Claude) */
   LLM_UNDEFINED /**< Not yet initialized / inherit from global */
} llm_type_t;

/** Maximum length for LLM model names */
#define LLM_MODEL_NAME_MAX 64

/** Maximum length for tool mode strings */
#define LLM_TOOL_MODE_MAX 16

/** Maximum length for thinking mode strings */
#define LLM_THINKING_MODE_MAX 16

/**
 * @brief Per-session LLM configuration
 *
 * Each session (WebUI, DAP, local) owns its own LLM settings.
 * Sessions are initialized with a copy of defaults at creation time.
 * Changes to one session's config do not affect other sessions.
 */
typedef struct {
   llm_type_t type;                              /**< LLM type (local or cloud) */
   cloud_provider_t cloud_provider;              /**< Cloud provider (OpenAI, Claude, etc.) */
   char endpoint[128];                           /**< Endpoint URL (empty = use provider default) */
   char model[LLM_MODEL_NAME_MAX];               /**< Model name (empty = use provider default) */
   char tool_mode[LLM_TOOL_MODE_MAX];            /**< Tool mode: native, command_tags, disabled */
   char thinking_mode[LLM_THINKING_MODE_MAX];    /**< Thinking: disabled, auto, enabled */
   char reasoning_effort[LLM_THINKING_MODE_MAX]; /**< Reasoning effort: low, medium, high */
} session_llm_config_t;

/**
 * @brief Resolved LLM configuration for making requests
 *
 * Created by llm_resolve_config() by merging session overrides with global config.
 *
 * WARNING: Pointers reference internal/stack memory that may become invalid
 * after llm_resolve_config() returns. Callers MUST copy string fields to local
 * buffers immediately using LLM_COPY_MODEL_SAFE() before any function calls.
 */
typedef struct {
   llm_type_t type;                              /**< Resolved LLM type */
   cloud_provider_t cloud_provider;              /**< Resolved cloud provider */
   const char *endpoint;                         /**< Endpoint URL (not owned, may be dangling) */
   const char *api_key;                          /**< API key for cloud providers (not owned) */
   const char *model;                            /**< Model name (not owned, may be dangling) */
   char tool_mode[LLM_TOOL_MODE_MAX];            /**< Tool mode: native, command_tags, disabled */
   char thinking_mode[LLM_THINKING_MODE_MAX];    /**< Thinking: disabled, auto, enabled */
   char reasoning_effort[LLM_THINKING_MODE_MAX]; /**< Reasoning effort: low, medium, high */
} llm_resolved_config_t;

/**
 * @brief Safely copy a model name to a local buffer
 *
 * Use this macro immediately after llm_resolve_config() to copy string fields
 * that may become dangling pointers. The macro handles NULL and empty strings.
 *
 * @param dst    Destination buffer (char array)
 * @param src    Source string (may be NULL)
 *
 * Example:
 *   llm_resolved_config_t resolved;
 *   char model_buf[LLM_MODEL_NAME_MAX];
 *   llm_resolve_config(&session_config, &resolved);
 *   LLM_COPY_MODEL_SAFE(model_buf, resolved.model);
 */
#define LLM_COPY_MODEL_SAFE(dst, src)            \
   do {                                          \
      if ((src) && (src)[0] != '\0') {           \
         strncpy((dst), (src), sizeof(dst) - 1); \
         (dst)[sizeof(dst) - 1] = '\0';          \
      } else {                                   \
         (dst)[0] = '\0';                        \
      }                                          \
   } while (0)

/**
 * @brief Initialize the LLM system
 *
 * Detects available cloud providers based on API keys in secrets.toml.
 * If command-line override provided, validates and uses it.
 * If both providers available and no override, defaults to OpenAI.
 *
 * @param cloud_provider_override Optional provider override from command line
 *                                 ("openai", "claude", or NULL to auto-detect)
 */
void llm_init(const char *cloud_provider_override);

/**
 * @brief Re-detect available cloud providers at runtime
 *
 * Call this after API keys are updated (e.g., via WebUI) to refresh
 * provider availability without restarting. Safe to call at any time.
 *
 * @return 1 if at least one cloud provider is now available, 0 otherwise
 */
int llm_refresh_providers(void);

/**
 * @brief Callback function type for streaming text chunks from LLM
 *
 * Called for each incremental text chunk received from the LLM during streaming.
 * The text should be processed immediately (e.g., sent to TTS).
 *
 * @param chunk Incremental text chunk
 * @param userdata User-provided context pointer
 */
typedef void (*llm_text_chunk_callback)(const char *chunk, void *userdata);

/**
 * @brief Callback function type for complete sentences from streaming
 *
 * Called for each complete sentence extracted from the LLM stream.
 * Use this for TTS to ensure natural speech boundaries.
 *
 * @param sentence Complete sentence text
 * @param userdata User-provided context pointer
 */
typedef void (*llm_sentence_callback)(const char *sentence, void *userdata);

/**
 * @brief Get chat completion from configured LLM (non-streaming)
 *
 * Routes to appropriate provider based on current configuration.
 * Handles local/cloud fallback automatically on connection failure if allow_fallback is true.
 * Conversation history is always stored in OpenAI format internally,
 * but converted as needed for Claude API calls.
 *
 * @param conversation_history JSON array of conversation messages (OpenAI format)
 * @param input_text User's input text
 * @param vision_images Array of base64 images for vision models (NULL if not used)
 * @param vision_image_sizes Array of image sizes (NULL if not used)
 * @param vision_image_count Number of images (0 if not used)
 * @param allow_fallback If true, falls back to local LLM on cloud failure
 * @return Response text (caller must free), or NULL on error
 */
char *llm_chat_completion(struct json_object *conversation_history,
                          const char *input_text,
                          const char **vision_images,
                          const size_t *vision_image_sizes,
                          int vision_image_count,
                          bool allow_fallback);

/**
 * @brief Get chat completion from configured LLM with streaming
 *
 * Routes to appropriate provider based on current configuration.
 * Calls chunk_callback for each incremental text chunk as it arrives.
 * The complete accumulated response is returned when streaming completes.
 * Handles local/cloud fallback automatically on connection failure if allow_fallback is true.
 *
 * @param conversation_history JSON array of conversation messages (OpenAI format)
 * @param input_text User's input text
 * @param vision_images Array of base64 images for vision models (NULL if not used)
 * @param vision_image_sizes Array of image sizes (NULL if not used)
 * @param vision_image_count Number of images (0 if not used)
 * @param chunk_callback Function to call for each text chunk (NULL for non-streaming)
 * @param callback_userdata User context passed to chunk_callback
 * @param allow_fallback If true, falls back to local LLM on cloud failure
 * @return Complete response text (caller must free), or NULL on error
 */
char *llm_chat_completion_streaming(struct json_object *conversation_history,
                                    const char *input_text,
                                    const char **vision_images,
                                    const size_t *vision_image_sizes,
                                    int vision_image_count,
                                    llm_text_chunk_callback chunk_callback,
                                    void *callback_userdata,
                                    bool allow_fallback);

/**
 * @brief Get chat completion with streaming and sentence-boundary buffering for TTS
 *
 * Similar to llm_chat_completion_streaming, but buffers chunks and sends complete
 * sentences to the callback. This ensures TTS receives natural speech boundaries
 * (sentences ending with ., !, ?, :) for better prosody and intonation.
 *
 * @param conversation_history JSON array of conversation messages (OpenAI format)
 * @param input_text User's input text
 * @param vision_images Array of base64 images for vision models (NULL if not used)
 * @param vision_image_sizes Array of image sizes (NULL if not used)
 * @param vision_image_count Number of images (0 if not used)
 * @param sentence_callback Function to call for each complete sentence
 * @param callback_userdata User context passed to sentence_callback
 * @param allow_fallback If true, falls back to local LLM on cloud failure
 * @return Complete response text (caller must free), or NULL on error
 */
char *llm_chat_completion_streaming_tts(struct json_object *conversation_history,
                                        const char *input_text,
                                        const char **vision_images,
                                        const size_t *vision_image_sizes,
                                        int vision_image_count,
                                        llm_sentence_callback sentence_callback,
                                        void *callback_userdata,
                                        bool allow_fallback);

/**
 * @brief Switch between local and cloud LLM
 *
 * @param type LLM_LOCAL or LLM_CLOUD
 * @return 0 on success, non-zero on failure (e.g., API key not configured)
 */
int llm_set_type(llm_type_t type);

/**
 * @brief Get current LLM type
 *
 * @return Current LLM type (LLM_LOCAL, LLM_CLOUD, or LLM_UNDEFINED)
 */
llm_type_t llm_get_type(void);

/**
 * @brief Get current cloud provider name (for display/logging)
 *
 * @return String name of provider ("OpenAI", "Claude", "Gemini", "None")
 */
const char *llm_get_cloud_provider_name(void);

/**
 * @brief Convert cloud provider enum to lowercase string
 *
 * Use this instead of inline ternary chains for provider-to-string conversion.
 *
 * @param provider Cloud provider enum value
 * @return Lowercase string ("openai", "claude", "gemini", "none")
 */
const char *cloud_provider_to_string(cloud_provider_t provider);

/**
 * @brief Set the cloud provider at runtime
 *
 * Switches between OpenAI and Claude. Validates that the required API key
 * is available before switching. Updates the endpoint URL if currently in cloud mode.
 *
 * @param provider CLOUD_PROVIDER_OPENAI or CLOUD_PROVIDER_CLAUDE
 * @return 0 on success, 1 on failure (API key not configured)
 */
int llm_set_cloud_provider(cloud_provider_t provider);

/**
 * @brief Get current cloud provider enum value
 *
 * @return Current cloud provider (CLOUD_PROVIDER_OPENAI, CLOUD_PROVIDER_CLAUDE, or
 * CLOUD_PROVIDER_NONE)
 */
cloud_provider_t llm_get_cloud_provider(void);

/**
 * @brief Get current LLM model name (for display/logging)
 *
 * @return String name of model (e.g., "gpt-4o", "claude-sonnet-4-5-20250929")
 */
const char *llm_get_model_name(void);

/**
 * @brief Get the default OpenAI model name from config
 *
 * Returns the model name at openai_default_model_idx in the openai_models array.
 * Falls back to the first model if index is out of bounds or no models configured.
 *
 * @return Model name string (pointer to config memory, do not free)
 */
const char *llm_get_default_openai_model(void);

/**
 * @brief Get the default Claude model name from config
 *
 * Returns the model name at claude_default_model_idx in the claude_models array.
 * Falls back to the first model if index is out of bounds or no models configured.
 *
 * @return Model name string (pointer to config memory, do not free)
 */
const char *llm_get_default_claude_model(void);

/**
 * @brief Get the default Gemini model name from config
 *
 * Returns the model name at gemini_default_model_idx in the gemini_models array.
 * Falls back to the first model if index is out of bounds or no models configured.
 *
 * @return Model name string (pointer to config memory, do not free)
 */
const char *llm_get_default_gemini_model(void);

/**
 * @brief Check internet connectivity to LLM endpoint
 *
 * @param url URL to check
 * @param timeout_seconds Timeout in seconds
 * @return 1 if reachable, 0 otherwise
 */
int llm_check_connection(const char *url, int timeout_seconds);

/**
 * @brief Request interruption of current LLM transfer
 *
 * Sets a flag that will cause the next CURL progress callback to abort
 * the transfer. Safe to call from signal handlers.
 */
void llm_request_interrupt(void);

/**
 * @brief Clear the LLM interrupt flag
 *
 * Should be called after handling an interrupted LLM call.
 */
void llm_clear_interrupt(void);

/**
 * @brief Check if LLM interrupt was requested
 *
 * @return 1 if interrupt requested, 0 otherwise
 */
int llm_is_interrupt_requested(void);

/**
 * @brief Set thread-local cancel flag for per-session cancellation
 *
 * Call this before starting an LLM request to enable per-session cancellation.
 * The cancel flag should point to a session-owned atomic_bool that gets set to
 * true when cancellation is requested. Set to NULL after the request completes.
 *
 * Implementation note: The flag is cast internally to _Atomic bool* for proper
 * atomic reads. Callers should pass &session->disconnected (an atomic_bool).
 *
 * @param flag Pointer to session's cancel flag (atomic_bool cast to void*), or NULL
 */
void llm_set_cancel_flag(void *flag);

/**
 * @brief Get current thread-local cancel flag
 *
 * @return Current cancel flag pointer, or NULL if not set
 */
void *llm_get_cancel_flag(void);

/**
 * @brief Check if OpenAI API key is available
 *
 * Checks runtime config (secrets.toml)
 *
 * @return true if API key is available, false otherwise
 */
bool llm_has_openai_key(void);

/**
 * @brief Check if Claude API key is available
 *
 * Checks runtime config (secrets.toml)
 *
 * @return true if API key is available, false otherwise
 */
bool llm_has_claude_key(void);

/**
 * @brief Check if Gemini API key is available
 *
 * Checks runtime config (secrets.toml)
 *
 * @return true if API key is available, false otherwise
 */
bool llm_has_gemini_key(void);

/* ============================================================================
 * Per-Session LLM Configuration Support
 * ============================================================================ */

/**
 * @brief Resolve session LLM config to final request config
 *
 * Resolves session config to final configuration with endpoints and API keys.
 * If session config specifies invalid settings (e.g., provider without API key),
 * returns error.
 *
 * @param session_config Session's LLM config
 * @param resolved Output: resolved configuration for LLM call
 * @return 0 on success, 1 on error (invalid config)
 */
int llm_resolve_config(const session_llm_config_t *session_config, llm_resolved_config_t *resolved);

/**
 * @brief Get default LLM configuration from dawn.toml settings
 *
 * Returns a session_llm_config_t populated with the default settings
 * from the global configuration. Used to initialize new sessions.
 *
 * @param config Output: default configuration
 */
void llm_get_default_config(session_llm_config_t *config);

/**
 * @brief Chat completion with explicit configuration (non-streaming)
 *
 * Same as llm_chat_completion() but uses provided config instead of global.
 *
 * @param conversation_history JSON array of conversation messages (OpenAI format)
 * @param input_text User's input text
 * @param vision_images Array of base64 images for vision models (NULL if not used)
 * @param vision_image_sizes Array of image sizes (NULL if not used)
 * @param vision_image_count Number of images (0 if not used)
 * @param config Resolved LLM configuration to use
 * @return Response text (caller must free), or NULL on error
 */
char *llm_chat_completion_with_config(struct json_object *conversation_history,
                                      const char *input_text,
                                      const char **vision_images,
                                      const size_t *vision_image_sizes,
                                      int vision_image_count,
                                      const llm_resolved_config_t *config);

/**
 * @brief Chat completion with explicit configuration (streaming)
 *
 * Same as llm_chat_completion_streaming() but uses provided config.
 *
 * @param conversation_history JSON array of conversation messages (OpenAI format)
 * @param input_text User's input text
 * @param vision_images Array of base64 images for vision models (NULL if not used)
 * @param vision_image_sizes Array of image sizes (NULL if not used)
 * @param vision_image_count Number of images (0 if not used)
 * @param chunk_callback Function to call for each text chunk
 * @param callback_userdata User context passed to chunk_callback
 * @param config Resolved LLM configuration to use
 * @return Complete response text (caller must free), or NULL on error
 */
char *llm_chat_completion_streaming_with_config(struct json_object *conversation_history,
                                                const char *input_text,
                                                const char **vision_images,
                                                const size_t *vision_image_sizes,
                                                int vision_image_count,
                                                llm_text_chunk_callback chunk_callback,
                                                void *callback_userdata,
                                                const llm_resolved_config_t *config);

/**
 * @brief Chat completion with explicit configuration (streaming TTS)
 *
 * Same as llm_chat_completion_streaming_tts() but uses provided config.
 *
 * @param conversation_history JSON array of conversation messages (OpenAI format)
 * @param input_text User's input text
 * @param vision_images Array of base64 images for vision models (NULL if not used)
 * @param vision_image_sizes Array of image sizes (NULL if not used)
 * @param vision_image_count Number of images (0 if not used)
 * @param sentence_callback Function to call for each complete sentence
 * @param callback_userdata User context passed to sentence_callback
 * @param config Resolved LLM configuration to use
 * @return Complete response text (caller must free), or NULL on error
 */
char *llm_chat_completion_streaming_tts_with_config(struct json_object *conversation_history,
                                                    const char *input_text,
                                                    const char **vision_images,
                                                    const size_t *vision_image_sizes,
                                                    int vision_image_count,
                                                    llm_sentence_callback sentence_callback,
                                                    void *callback_userdata,
                                                    const llm_resolved_config_t *config);

/**
 * @brief Get full resolved LLM config for current session
 *
 * Returns the complete resolved config including type, cloud_provider,
 * endpoint, and API key. Used to detect provider changes after switch_llm.
 *
 * @param config_out Output: resolved config (caller owns, do not free strings)
 * @return 0 on success, 1 on failure
 */
int llm_get_current_resolved_config(llm_resolved_config_t *config_out);

#endif  // LLM_INTERFACE_H
