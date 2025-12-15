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
 * DAWN Configuration Environment - Environment variable overrides and dump
 */

#include "config/config_env.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "logging.h"

/* =============================================================================
 * Helper Macros
 * ============================================================================= */
#define SAFE_COPY(dst, src)                   \
   do {                                       \
      strncpy((dst), (src), sizeof(dst) - 1); \
      (dst)[sizeof(dst) - 1] = '\0';          \
   } while (0)

#define ENV_STRING(env_name, dest)                          \
   do {                                                     \
      const char *val = getenv(env_name);                   \
      if (val) {                                            \
         SAFE_COPY(dest, val);                              \
         LOG_INFO("Config override: %s=%s", env_name, val); \
      }                                                     \
   } while (0)

#define ENV_INT(env_name, dest)                              \
   do {                                                      \
      const char *val = getenv(env_name);                    \
      if (val) {                                             \
         dest = atoi(val);                                   \
         LOG_INFO("Config override: %s=%d", env_name, dest); \
      }                                                      \
   } while (0)

#define ENV_FLOAT(env_name, dest)                              \
   do {                                                        \
      const char *val = getenv(env_name);                      \
      if (val) {                                               \
         dest = (float)atof(val);                              \
         LOG_INFO("Config override: %s=%.2f", env_name, dest); \
      }                                                        \
   } while (0)

#define ENV_BOOL(env_name, dest)                                                \
   do {                                                                         \
      const char *val = getenv(env_name);                                       \
      if (val) {                                                                \
         dest = (strcmp(val, "1") == 0 || strcasecmp(val, "true") == 0 ||       \
                 strcasecmp(val, "yes") == 0);                                  \
         LOG_INFO("Config override: %s=%s", env_name, dest ? "true" : "false"); \
      }                                                                         \
   } while (0)

#define ENV_SIZE_T(env_name, dest)                            \
   do {                                                       \
      const char *val = getenv(env_name);                     \
      if (val) {                                              \
         dest = (size_t)atol(val);                            \
         LOG_INFO("Config override: %s=%zu", env_name, dest); \
      }                                                       \
   } while (0)

/* =============================================================================
 * Environment Variable Application
 * ============================================================================= */

void config_apply_env(dawn_config_t *config, secrets_config_t *secrets) {
   if (!config || !secrets)
      return;

   /* Standard API keys (highest priority) */
   ENV_STRING("OPENAI_API_KEY", secrets->openai_api_key);
   ENV_STRING("ANTHROPIC_API_KEY", secrets->claude_api_key);

   /* DAWN_ prefixed environment variables */

   /* [general] */
   ENV_STRING("DAWN_GENERAL_AI_NAME", config->general.ai_name);
   ENV_STRING("DAWN_GENERAL_LOG_FILE", config->general.log_file);

   /* [persona] */
   ENV_STRING("DAWN_PERSONA_DESCRIPTION", config->persona.description);

   /* [localization] */
   ENV_STRING("DAWN_LOCALIZATION_LOCATION", config->localization.location);
   ENV_STRING("DAWN_LOCALIZATION_TIMEZONE", config->localization.timezone);
   ENV_STRING("DAWN_LOCALIZATION_UNITS", config->localization.units);

   /* [audio] */
   ENV_STRING("DAWN_AUDIO_BACKEND", config->audio.backend);
   ENV_STRING("DAWN_AUDIO_CAPTURE_DEVICE", config->audio.capture_device);
   ENV_STRING("DAWN_AUDIO_PLAYBACK_DEVICE", config->audio.playback_device);

   /* [audio.bargein] */
   ENV_BOOL("DAWN_AUDIO_BARGEIN_ENABLED", config->audio.bargein.enabled);
   ENV_INT("DAWN_AUDIO_BARGEIN_COOLDOWN_MS", config->audio.bargein.cooldown_ms);
   ENV_INT("DAWN_AUDIO_BARGEIN_STARTUP_COOLDOWN_MS", config->audio.bargein.startup_cooldown_ms);

   /* [vad] */
   ENV_FLOAT("DAWN_VAD_SPEECH_THRESHOLD", config->vad.speech_threshold);
   ENV_FLOAT("DAWN_VAD_SPEECH_THRESHOLD_TTS", config->vad.speech_threshold_tts);
   ENV_FLOAT("DAWN_VAD_SILENCE_THRESHOLD", config->vad.silence_threshold);
   ENV_FLOAT("DAWN_VAD_END_OF_SPEECH_DURATION", config->vad.end_of_speech_duration);
   ENV_FLOAT("DAWN_VAD_MAX_RECORDING_DURATION", config->vad.max_recording_duration);
   ENV_INT("DAWN_VAD_PREROLL_MS", config->vad.preroll_ms);

   /* [vad.chunking] */
   ENV_BOOL("DAWN_VAD_CHUNKING_ENABLED", config->vad.chunking.enabled);
   ENV_FLOAT("DAWN_VAD_CHUNKING_PAUSE_DURATION", config->vad.chunking.pause_duration);
   ENV_FLOAT("DAWN_VAD_CHUNKING_MIN_DURATION", config->vad.chunking.min_duration);
   ENV_FLOAT("DAWN_VAD_CHUNKING_MAX_DURATION", config->vad.chunking.max_duration);

   /* [asr] */
   ENV_STRING("DAWN_ASR_MODEL", config->asr.model);
   ENV_STRING("DAWN_ASR_MODELS_PATH", config->asr.models_path);

   /* [tts] */
   ENV_STRING("DAWN_TTS_VOICE_MODEL", config->tts.voice_model);
   ENV_FLOAT("DAWN_TTS_LENGTH_SCALE", config->tts.length_scale);

   /* [commands] */
   ENV_STRING("DAWN_COMMANDS_PROCESSING_MODE", config->commands.processing_mode);

   /* [llm] */
   ENV_STRING("DAWN_LLM_TYPE", config->llm.type);
   ENV_INT("DAWN_LLM_MAX_TOKENS", config->llm.max_tokens);

   /* [llm.cloud] */
   ENV_STRING("DAWN_LLM_CLOUD_PROVIDER", config->llm.cloud.provider);
   ENV_STRING("DAWN_LLM_CLOUD_MODEL", config->llm.cloud.model);
   ENV_STRING("DAWN_LLM_CLOUD_ENDPOINT", config->llm.cloud.endpoint);
   ENV_BOOL("DAWN_LLM_CLOUD_VISION_ENABLED", config->llm.cloud.vision_enabled);

   /* [llm.local] */
   ENV_STRING("DAWN_LLM_LOCAL_ENDPOINT", config->llm.local.endpoint);
   ENV_STRING("DAWN_LLM_LOCAL_MODEL", config->llm.local.model);
   ENV_BOOL("DAWN_LLM_LOCAL_VISION_ENABLED", config->llm.local.vision_enabled);

   /* [search] */
   ENV_STRING("DAWN_SEARCH_ENGINE", config->search.engine);
   ENV_STRING("DAWN_SEARCH_ENDPOINT", config->search.endpoint);

   /* [search.summarizer] */
   ENV_STRING("DAWN_SEARCH_SUMMARIZER_BACKEND", config->search.summarizer.backend);
   ENV_SIZE_T("DAWN_SEARCH_SUMMARIZER_THRESHOLD_BYTES", config->search.summarizer.threshold_bytes);
   ENV_SIZE_T("DAWN_SEARCH_SUMMARIZER_TARGET_WORDS", config->search.summarizer.target_words);

   /* [url_fetcher.flaresolverr] */
   ENV_BOOL("DAWN_URL_FETCHER_FLARESOLVERR_ENABLED", config->url_fetcher.flaresolverr.enabled);
   ENV_STRING("DAWN_URL_FETCHER_FLARESOLVERR_ENDPOINT", config->url_fetcher.flaresolverr.endpoint);
   ENV_INT("DAWN_URL_FETCHER_FLARESOLVERR_TIMEOUT_SEC",
           config->url_fetcher.flaresolverr.timeout_sec);
   ENV_SIZE_T("DAWN_URL_FETCHER_FLARESOLVERR_MAX_RESPONSE_BYTES",
              config->url_fetcher.flaresolverr.max_response_bytes);

   /* [mqtt] */
   ENV_BOOL("DAWN_MQTT_ENABLED", config->mqtt.enabled);
   ENV_STRING("DAWN_MQTT_BROKER", config->mqtt.broker);
   ENV_INT("DAWN_MQTT_PORT", config->mqtt.port);
   ENV_STRING("MQTT_USERNAME", secrets->mqtt_username);
   ENV_STRING("MQTT_PASSWORD", secrets->mqtt_password);

   /* [network] */
   ENV_BOOL("DAWN_NETWORK_ENABLED", config->network.enabled);
   ENV_STRING("DAWN_NETWORK_HOST", config->network.host);
   ENV_INT("DAWN_NETWORK_PORT", config->network.port);
   ENV_INT("DAWN_NETWORK_WORKERS", config->network.workers);
   ENV_INT("DAWN_NETWORK_SOCKET_TIMEOUT_SEC", config->network.socket_timeout_sec);
   ENV_INT("DAWN_NETWORK_SESSION_TIMEOUT_SEC", config->network.session_timeout_sec);
   ENV_INT("DAWN_NETWORK_LLM_TIMEOUT_MS", config->network.llm_timeout_ms);

   /* [tui] */
   ENV_BOOL("DAWN_TUI_ENABLED", config->tui.enabled);

   /* [debug] */
   ENV_BOOL("DAWN_DEBUG_MIC_RECORD", config->debug.mic_record);
   ENV_BOOL("DAWN_DEBUG_ASR_RECORD", config->debug.asr_record);
   ENV_BOOL("DAWN_DEBUG_AEC_RECORD", config->debug.aec_record);
   ENV_STRING("DAWN_DEBUG_RECORD_PATH", config->debug.record_path);

   /* [paths] */
   ENV_STRING("DAWN_PATHS_MUSIC_DIR", config->paths.music_dir);
   ENV_STRING("DAWN_PATHS_COMMANDS_CONFIG", config->paths.commands_config);
}

/* =============================================================================
 * Configuration Dump
 * ============================================================================= */

void config_dump(const dawn_config_t *config) {
   if (!config)
      return;

   printf("=== DAWN Configuration ===\n\n");

   printf("[general]\n");
   printf("  ai_name = \"%s\"\n", config->general.ai_name);
   printf("  log_file = \"%s\"\n", config->general.log_file);

   printf("\n[persona]\n");
   printf("  description = \"%.*s%s\"\n", config->persona.description[0] ? 50 : 0,
          config->persona.description, strlen(config->persona.description) > 50 ? "..." : "");

   printf("\n[localization]\n");
   printf("  location = \"%s\"\n", config->localization.location);
   printf("  timezone = \"%s\"\n", config->localization.timezone);
   printf("  units = \"%s\"\n", config->localization.units);

   printf("\n[audio]\n");
   printf("  backend = \"%s\"\n", config->audio.backend);
   printf("  capture_device = \"%s\"\n", config->audio.capture_device);
   printf("  playback_device = \"%s\"\n", config->audio.playback_device);

   printf("\n[audio.bargein]\n");
   printf("  enabled = %s\n", config->audio.bargein.enabled ? "true" : "false");
   printf("  cooldown_ms = %d\n", config->audio.bargein.cooldown_ms);
   printf("  startup_cooldown_ms = %d\n", config->audio.bargein.startup_cooldown_ms);

   printf("\n[vad]\n");
   printf("  speech_threshold = %.2f\n", config->vad.speech_threshold);
   printf("  speech_threshold_tts = %.2f\n", config->vad.speech_threshold_tts);
   printf("  silence_threshold = %.2f\n", config->vad.silence_threshold);
   printf("  end_of_speech_duration = %.1f\n", config->vad.end_of_speech_duration);
   printf("  max_recording_duration = %.1f\n", config->vad.max_recording_duration);
   printf("  preroll_ms = %d\n", config->vad.preroll_ms);

   printf("\n[vad.chunking]\n");
   printf("  enabled = %s\n", config->vad.chunking.enabled ? "true" : "false");
   printf("  pause_duration = %.2f\n", config->vad.chunking.pause_duration);
   printf("  min_duration = %.1f\n", config->vad.chunking.min_duration);
   printf("  max_duration = %.1f\n", config->vad.chunking.max_duration);

   printf("\n[asr]\n");
   printf("  model = \"%s\"\n", config->asr.model);
   printf("  models_path = \"%s\"\n", config->asr.models_path);

   printf("\n[tts]\n");
   printf("  voice_model = \"%s\"\n", config->tts.voice_model);
   printf("  length_scale = %.2f\n", config->tts.length_scale);

   printf("\n[commands]\n");
   printf("  processing_mode = \"%s\"\n", config->commands.processing_mode);

   printf("\n[llm]\n");
   printf("  type = \"%s\"\n", config->llm.type);
   printf("  max_tokens = %d\n", config->llm.max_tokens);

   printf("\n[llm.cloud]\n");
   printf("  provider = \"%s\"\n", config->llm.cloud.provider);
   printf("  model = \"%s\"\n", config->llm.cloud.model);
   printf("  endpoint = \"%s\"\n", config->llm.cloud.endpoint);

   printf("\n[llm.local]\n");
   printf("  endpoint = \"%s\"\n", config->llm.local.endpoint);
   printf("  model = \"%s\"\n", config->llm.local.model);

   printf("\n[search]\n");
   printf("  engine = \"%s\"\n", config->search.engine);
   printf("  endpoint = \"%s\"\n", config->search.endpoint);

   printf("\n[search.summarizer]\n");
   printf("  backend = \"%s\"\n", config->search.summarizer.backend);
   printf("  threshold_bytes = %zu\n", config->search.summarizer.threshold_bytes);
   printf("  target_words = %zu\n", config->search.summarizer.target_words);

   printf("\n[url_fetcher]\n");
   printf("  whitelist_count = %d\n", config->url_fetcher.whitelist_count);

   printf("\n[url_fetcher.flaresolverr]\n");
   printf("  enabled = %s\n", config->url_fetcher.flaresolverr.enabled ? "true" : "false");
   printf("  endpoint = \"%s\"\n", config->url_fetcher.flaresolverr.endpoint);
   printf("  timeout_sec = %d\n", config->url_fetcher.flaresolverr.timeout_sec);
   printf("  max_response_bytes = %zu\n", config->url_fetcher.flaresolverr.max_response_bytes);

   printf("\n[mqtt]\n");
   printf("  enabled = %s\n", config->mqtt.enabled ? "true" : "false");
   printf("  broker = \"%s\"\n", config->mqtt.broker);
   printf("  port = %d\n", config->mqtt.port);

   printf("\n[network]\n");
   printf("  enabled = %s\n", config->network.enabled ? "true" : "false");
   printf("  host = \"%s\"\n", config->network.host);
   printf("  port = %d\n", config->network.port);
   printf("  workers = %d\n", config->network.workers);
   printf("  socket_timeout_sec = %d\n", config->network.socket_timeout_sec);
   printf("  session_timeout_sec = %d\n", config->network.session_timeout_sec);
   printf("  llm_timeout_ms = %d\n", config->network.llm_timeout_ms);

   printf("\n[tui]\n");
   printf("  enabled = %s\n", config->tui.enabled ? "true" : "false");

   printf("\n[debug]\n");
   printf("  mic_record = %s\n", config->debug.mic_record ? "true" : "false");
   printf("  asr_record = %s\n", config->debug.asr_record ? "true" : "false");
   printf("  aec_record = %s\n", config->debug.aec_record ? "true" : "false");
   printf("  record_path = \"%s\"\n", config->debug.record_path);

   printf("\n[paths]\n");
   printf("  music_dir = \"%s\"\n", config->paths.music_dir);
   printf("  commands_config = \"%s\"\n", config->paths.commands_config);
}

/* =============================================================================
 * Settings Dump with Sources
 * ============================================================================= */

/* Source detection: compare against defaults and check env vars */
typedef enum {
   SOURCE_DEFAULT,
   SOURCE_FILE,
   SOURCE_ENV
} setting_source_t;

static const char *source_name(setting_source_t src) {
   switch (src) {
      case SOURCE_DEFAULT:
         return "default";
      case SOURCE_FILE:
         return "file";
      case SOURCE_ENV:
         return "env";
      default:
         return "unknown";
   }
}

/* Helper to detect source of a string setting */
static setting_source_t detect_source_str(const char *current,
                                          const char *default_val,
                                          const char *env_name) {
   if (getenv(env_name))
      return SOURCE_ENV;
   if (strcmp(current, default_val) != 0)
      return SOURCE_FILE;
   return SOURCE_DEFAULT;
}

/* Helper to detect source of an int setting */
static setting_source_t detect_source_int(int current, int default_val, const char *env_name) {
   if (getenv(env_name))
      return SOURCE_ENV;
   if (current != default_val)
      return SOURCE_FILE;
   return SOURCE_DEFAULT;
}

/* Helper to detect source of a float setting */
static setting_source_t detect_source_float(float current,
                                            float default_val,
                                            const char *env_name) {
   if (getenv(env_name))
      return SOURCE_ENV;
   /* Use epsilon for float comparison */
   if (current < default_val - 0.001f || current > default_val + 0.001f)
      return SOURCE_FILE;
   return SOURCE_DEFAULT;
}

/* Helper to detect source of a size_t setting */
static setting_source_t detect_source_size(size_t current,
                                           size_t default_val,
                                           const char *env_name) {
   if (getenv(env_name))
      return SOURCE_ENV;
   if (current != default_val)
      return SOURCE_FILE;
   return SOURCE_DEFAULT;
}

/* Helper to detect source of a bool setting */
static setting_source_t detect_source_bool(bool current, bool default_val, const char *env_name) {
   if (getenv(env_name))
      return SOURCE_ENV;
   if (current != default_val)
      return SOURCE_FILE;
   return SOURCE_DEFAULT;
}

/* Print macros for consistent formatting */
#define PRINT_SETTING_STR(name, value, env_name, source) \
   printf("  %-40s = \"%s\"\n", name, value);            \
   printf("    %-38s   (%s)\n\n", env_name, source_name(source))

#define PRINT_SETTING_INT(name, value, env_name, source) \
   printf("  %-40s = %d\n", name, value);                \
   printf("    %-38s   (%s)\n\n", env_name, source_name(source))

#define PRINT_SETTING_FLOAT(name, value, env_name, source) \
   printf("  %-40s = %.2f\n", name, (double)(value));      \
   printf("    %-38s   (%s)\n\n", env_name, source_name(source))

#define PRINT_SETTING_SIZE(name, value, env_name, source) \
   printf("  %-40s = %zu\n", name, value);                \
   printf("    %-38s   (%s)\n\n", env_name, source_name(source))

#define PRINT_SETTING_BOOL(name, value, env_name, source)      \
   printf("  %-40s = %s\n", name, (value) ? "true" : "false"); \
   printf("    %-38s   (%s)\n\n", env_name, source_name(source))

void config_dump_settings(const dawn_config_t *config,
                          const secrets_config_t *secrets,
                          const char *config_file_loaded) {
   if (!config)
      return;

   /* Create defaults for comparison */
   dawn_config_t defaults;
   config_set_defaults(&defaults);

   printf("================================================================================\n");
   printf("DAWN Settings - Current Values, Environment Variables, and Sources\n");
   printf("================================================================================\n\n");
   printf("Source priority: defaults -> config file -> environment variables -> CLI\n\n");

   if (config_file_loaded) {
      printf("Config file loaded: %s\n\n", config_file_loaded);
   } else {
      printf("Config file loaded: (none - using defaults)\n\n");
   }

   printf("Legend:\n");
   printf("  default = compile-time default value\n");
   printf("  file    = loaded from config file\n");
   printf("  env     = overridden by environment variable\n\n");

   /* [general] */
   printf("[general]\n");
   PRINT_SETTING_STR("ai_name", config->general.ai_name, "DAWN_GENERAL_AI_NAME",
                     detect_source_str(config->general.ai_name, defaults.general.ai_name,
                                       "DAWN_GENERAL_AI_NAME"));
   PRINT_SETTING_STR("log_file", config->general.log_file, "DAWN_GENERAL_LOG_FILE",
                     detect_source_str(config->general.log_file, defaults.general.log_file,
                                       "DAWN_GENERAL_LOG_FILE"));

   /* [persona] */
   printf("[persona]\n");
   printf("  %-40s = \"%.*s%s\"\n", "description", config->persona.description[0] ? 50 : 0,
          config->persona.description, strlen(config->persona.description) > 50 ? "..." : "");
   printf("    %-38s   (%s)\n\n", "DAWN_PERSONA_DESCRIPTION",
          source_name(detect_source_str(config->persona.description, defaults.persona.description,
                                        "DAWN_PERSONA_DESCRIPTION")));

   /* [localization] */
   printf("[localization]\n");
   PRINT_SETTING_STR("location", config->localization.location, "DAWN_LOCALIZATION_LOCATION",
                     detect_source_str(config->localization.location,
                                       defaults.localization.location,
                                       "DAWN_LOCALIZATION_LOCATION"));
   PRINT_SETTING_STR("timezone", config->localization.timezone, "DAWN_LOCALIZATION_TIMEZONE",
                     detect_source_str(config->localization.timezone,
                                       defaults.localization.timezone,
                                       "DAWN_LOCALIZATION_TIMEZONE"));
   PRINT_SETTING_STR("units", config->localization.units, "DAWN_LOCALIZATION_UNITS",
                     detect_source_str(config->localization.units, defaults.localization.units,
                                       "DAWN_LOCALIZATION_UNITS"));

   /* [audio] */
   printf("[audio]\n");
   PRINT_SETTING_STR("backend", config->audio.backend, "DAWN_AUDIO_BACKEND",
                     detect_source_str(config->audio.backend, defaults.audio.backend,
                                       "DAWN_AUDIO_BACKEND"));
   PRINT_SETTING_STR("capture_device", config->audio.capture_device, "DAWN_AUDIO_CAPTURE_DEVICE",
                     detect_source_str(config->audio.capture_device, defaults.audio.capture_device,
                                       "DAWN_AUDIO_CAPTURE_DEVICE"));
   PRINT_SETTING_STR("playback_device", config->audio.playback_device, "DAWN_AUDIO_PLAYBACK_DEVICE",
                     detect_source_str(config->audio.playback_device,
                                       defaults.audio.playback_device,
                                       "DAWN_AUDIO_PLAYBACK_DEVICE"));

   /* [audio.bargein] */
   printf("[audio.bargein]\n");
   PRINT_SETTING_BOOL("enabled", config->audio.bargein.enabled, "DAWN_AUDIO_BARGEIN_ENABLED",
                      detect_source_bool(config->audio.bargein.enabled,
                                         defaults.audio.bargein.enabled,
                                         "DAWN_AUDIO_BARGEIN_ENABLED"));
   PRINT_SETTING_INT("cooldown_ms", config->audio.bargein.cooldown_ms,
                     "DAWN_AUDIO_BARGEIN_COOLDOWN_MS",
                     detect_source_int(config->audio.bargein.cooldown_ms,
                                       defaults.audio.bargein.cooldown_ms,
                                       "DAWN_AUDIO_BARGEIN_COOLDOWN_MS"));
   PRINT_SETTING_INT("startup_cooldown_ms", config->audio.bargein.startup_cooldown_ms,
                     "DAWN_AUDIO_BARGEIN_STARTUP_COOLDOWN_MS",
                     detect_source_int(config->audio.bargein.startup_cooldown_ms,
                                       defaults.audio.bargein.startup_cooldown_ms,
                                       "DAWN_AUDIO_BARGEIN_STARTUP_COOLDOWN_MS"));

   /* [vad] */
   printf("[vad]\n");
   PRINT_SETTING_FLOAT("speech_threshold", config->vad.speech_threshold,
                       "DAWN_VAD_SPEECH_THRESHOLD",
                       detect_source_float(config->vad.speech_threshold,
                                           defaults.vad.speech_threshold,
                                           "DAWN_VAD_SPEECH_THRESHOLD"));
   PRINT_SETTING_FLOAT("speech_threshold_tts", config->vad.speech_threshold_tts,
                       "DAWN_VAD_SPEECH_THRESHOLD_TTS",
                       detect_source_float(config->vad.speech_threshold_tts,
                                           defaults.vad.speech_threshold_tts,
                                           "DAWN_VAD_SPEECH_THRESHOLD_TTS"));
   PRINT_SETTING_FLOAT("silence_threshold", config->vad.silence_threshold,
                       "DAWN_VAD_SILENCE_THRESHOLD",
                       detect_source_float(config->vad.silence_threshold,
                                           defaults.vad.silence_threshold,
                                           "DAWN_VAD_SILENCE_THRESHOLD"));
   PRINT_SETTING_FLOAT("end_of_speech_duration", config->vad.end_of_speech_duration,
                       "DAWN_VAD_END_OF_SPEECH_DURATION",
                       detect_source_float(config->vad.end_of_speech_duration,
                                           defaults.vad.end_of_speech_duration,
                                           "DAWN_VAD_END_OF_SPEECH_DURATION"));
   PRINT_SETTING_FLOAT("max_recording_duration", config->vad.max_recording_duration,
                       "DAWN_VAD_MAX_RECORDING_DURATION",
                       detect_source_float(config->vad.max_recording_duration,
                                           defaults.vad.max_recording_duration,
                                           "DAWN_VAD_MAX_RECORDING_DURATION"));
   PRINT_SETTING_INT("preroll_ms", config->vad.preroll_ms, "DAWN_VAD_PREROLL_MS",
                     detect_source_int(config->vad.preroll_ms, defaults.vad.preroll_ms,
                                       "DAWN_VAD_PREROLL_MS"));

   /* [vad.chunking] */
   printf("[vad.chunking]\n");
   PRINT_SETTING_BOOL("enabled", config->vad.chunking.enabled, "DAWN_VAD_CHUNKING_ENABLED",
                      detect_source_bool(config->vad.chunking.enabled,
                                         defaults.vad.chunking.enabled,
                                         "DAWN_VAD_CHUNKING_ENABLED"));
   PRINT_SETTING_FLOAT("pause_duration", config->vad.chunking.pause_duration,
                       "DAWN_VAD_CHUNKING_PAUSE_DURATION",
                       detect_source_float(config->vad.chunking.pause_duration,
                                           defaults.vad.chunking.pause_duration,
                                           "DAWN_VAD_CHUNKING_PAUSE_DURATION"));
   PRINT_SETTING_FLOAT("min_duration", config->vad.chunking.min_duration,
                       "DAWN_VAD_CHUNKING_MIN_DURATION",
                       detect_source_float(config->vad.chunking.min_duration,
                                           defaults.vad.chunking.min_duration,
                                           "DAWN_VAD_CHUNKING_MIN_DURATION"));
   PRINT_SETTING_FLOAT("max_duration", config->vad.chunking.max_duration,
                       "DAWN_VAD_CHUNKING_MAX_DURATION",
                       detect_source_float(config->vad.chunking.max_duration,
                                           defaults.vad.chunking.max_duration,
                                           "DAWN_VAD_CHUNKING_MAX_DURATION"));

   /* [asr] */
   printf("[asr]\n");
   PRINT_SETTING_STR("model", config->asr.model, "DAWN_ASR_MODEL",
                     detect_source_str(config->asr.model, defaults.asr.model, "DAWN_ASR_MODEL"));
   PRINT_SETTING_STR("models_path", config->asr.models_path, "DAWN_ASR_MODELS_PATH",
                     detect_source_str(config->asr.models_path, defaults.asr.models_path,
                                       "DAWN_ASR_MODELS_PATH"));

   /* [tts] */
   printf("[tts]\n");
   PRINT_SETTING_STR("voice_model", config->tts.voice_model, "DAWN_TTS_VOICE_MODEL",
                     detect_source_str(config->tts.voice_model, defaults.tts.voice_model,
                                       "DAWN_TTS_VOICE_MODEL"));
   PRINT_SETTING_FLOAT("length_scale", config->tts.length_scale, "DAWN_TTS_LENGTH_SCALE",
                       detect_source_float(config->tts.length_scale, defaults.tts.length_scale,
                                           "DAWN_TTS_LENGTH_SCALE"));

   /* [commands] */
   printf("[commands]\n");
   PRINT_SETTING_STR("processing_mode", config->commands.processing_mode,
                     "DAWN_COMMANDS_PROCESSING_MODE",
                     detect_source_str(config->commands.processing_mode,
                                       defaults.commands.processing_mode,
                                       "DAWN_COMMANDS_PROCESSING_MODE"));

   /* [llm] */
   printf("[llm]\n");
   PRINT_SETTING_STR("type", config->llm.type, "DAWN_LLM_TYPE",
                     detect_source_str(config->llm.type, defaults.llm.type, "DAWN_LLM_TYPE"));
   PRINT_SETTING_INT("max_tokens", config->llm.max_tokens, "DAWN_LLM_MAX_TOKENS",
                     detect_source_int(config->llm.max_tokens, defaults.llm.max_tokens,
                                       "DAWN_LLM_MAX_TOKENS"));

   /* [llm.cloud] */
   printf("[llm.cloud]\n");
   PRINT_SETTING_STR("provider", config->llm.cloud.provider, "DAWN_LLM_CLOUD_PROVIDER",
                     detect_source_str(config->llm.cloud.provider, defaults.llm.cloud.provider,
                                       "DAWN_LLM_CLOUD_PROVIDER"));
   PRINT_SETTING_STR("model", config->llm.cloud.model, "DAWN_LLM_CLOUD_MODEL",
                     detect_source_str(config->llm.cloud.model, defaults.llm.cloud.model,
                                       "DAWN_LLM_CLOUD_MODEL"));
   PRINT_SETTING_STR("endpoint", config->llm.cloud.endpoint, "DAWN_LLM_CLOUD_ENDPOINT",
                     detect_source_str(config->llm.cloud.endpoint, defaults.llm.cloud.endpoint,
                                       "DAWN_LLM_CLOUD_ENDPOINT"));
   PRINT_SETTING_BOOL("vision_enabled", config->llm.cloud.vision_enabled,
                      "DAWN_LLM_CLOUD_VISION_ENABLED",
                      detect_source_bool(config->llm.cloud.vision_enabled,
                                         defaults.llm.cloud.vision_enabled,
                                         "DAWN_LLM_CLOUD_VISION_ENABLED"));

   /* [llm.local] */
   printf("[llm.local]\n");
   PRINT_SETTING_STR("endpoint", config->llm.local.endpoint, "DAWN_LLM_LOCAL_ENDPOINT",
                     detect_source_str(config->llm.local.endpoint, defaults.llm.local.endpoint,
                                       "DAWN_LLM_LOCAL_ENDPOINT"));
   PRINT_SETTING_STR("model", config->llm.local.model, "DAWN_LLM_LOCAL_MODEL",
                     detect_source_str(config->llm.local.model, defaults.llm.local.model,
                                       "DAWN_LLM_LOCAL_MODEL"));
   PRINT_SETTING_BOOL("vision_enabled", config->llm.local.vision_enabled,
                      "DAWN_LLM_LOCAL_VISION_ENABLED",
                      detect_source_bool(config->llm.local.vision_enabled,
                                         defaults.llm.local.vision_enabled,
                                         "DAWN_LLM_LOCAL_VISION_ENABLED"));

   /* [search] */
   printf("[search]\n");
   PRINT_SETTING_STR("engine", config->search.engine, "DAWN_SEARCH_ENGINE",
                     detect_source_str(config->search.engine, defaults.search.engine,
                                       "DAWN_SEARCH_ENGINE"));
   PRINT_SETTING_STR("endpoint", config->search.endpoint, "DAWN_SEARCH_ENDPOINT",
                     detect_source_str(config->search.endpoint, defaults.search.endpoint,
                                       "DAWN_SEARCH_ENDPOINT"));

   /* [search.summarizer] */
   printf("[search.summarizer]\n");
   PRINT_SETTING_STR("backend", config->search.summarizer.backend, "DAWN_SEARCH_SUMMARIZER_BACKEND",
                     detect_source_str(config->search.summarizer.backend,
                                       defaults.search.summarizer.backend,
                                       "DAWN_SEARCH_SUMMARIZER_BACKEND"));
   PRINT_SETTING_SIZE("threshold_bytes", config->search.summarizer.threshold_bytes,
                      "DAWN_SEARCH_SUMMARIZER_THRESHOLD_BYTES",
                      detect_source_size(config->search.summarizer.threshold_bytes,
                                         defaults.search.summarizer.threshold_bytes,
                                         "DAWN_SEARCH_SUMMARIZER_THRESHOLD_BYTES"));
   PRINT_SETTING_SIZE("target_words", config->search.summarizer.target_words,
                      "DAWN_SEARCH_SUMMARIZER_TARGET_WORDS",
                      detect_source_size(config->search.summarizer.target_words,
                                         defaults.search.summarizer.target_words,
                                         "DAWN_SEARCH_SUMMARIZER_TARGET_WORDS"));

   /* [url_fetcher.flaresolverr] */
   printf("[url_fetcher.flaresolverr]\n");
   PRINT_SETTING_BOOL("enabled", config->url_fetcher.flaresolverr.enabled,
                      "DAWN_URL_FETCHER_FLARESOLVERR_ENABLED",
                      detect_source_bool(config->url_fetcher.flaresolverr.enabled,
                                         defaults.url_fetcher.flaresolverr.enabled,
                                         "DAWN_URL_FETCHER_FLARESOLVERR_ENABLED"));
   PRINT_SETTING_STR("endpoint", config->url_fetcher.flaresolverr.endpoint,
                     "DAWN_URL_FETCHER_FLARESOLVERR_ENDPOINT",
                     detect_source_str(config->url_fetcher.flaresolverr.endpoint,
                                       defaults.url_fetcher.flaresolverr.endpoint,
                                       "DAWN_URL_FETCHER_FLARESOLVERR_ENDPOINT"));
   PRINT_SETTING_INT("timeout_sec", config->url_fetcher.flaresolverr.timeout_sec,
                     "DAWN_URL_FETCHER_FLARESOLVERR_TIMEOUT_SEC",
                     detect_source_int(config->url_fetcher.flaresolverr.timeout_sec,
                                       defaults.url_fetcher.flaresolverr.timeout_sec,
                                       "DAWN_URL_FETCHER_FLARESOLVERR_TIMEOUT_SEC"));
   PRINT_SETTING_SIZE("max_response_bytes", config->url_fetcher.flaresolverr.max_response_bytes,
                      "DAWN_URL_FETCHER_FLARESOLVERR_MAX_RESPONSE_BYTES",
                      detect_source_size(config->url_fetcher.flaresolverr.max_response_bytes,
                                         defaults.url_fetcher.flaresolverr.max_response_bytes,
                                         "DAWN_URL_FETCHER_FLARESOLVERR_MAX_RESPONSE_BYTES"));

   /* [mqtt] */
   printf("[mqtt]\n");
   PRINT_SETTING_BOOL("enabled", config->mqtt.enabled, "DAWN_MQTT_ENABLED",
                      detect_source_bool(config->mqtt.enabled, defaults.mqtt.enabled,
                                         "DAWN_MQTT_ENABLED"));
   PRINT_SETTING_STR("broker", config->mqtt.broker, "DAWN_MQTT_BROKER",
                     detect_source_str(config->mqtt.broker, defaults.mqtt.broker,
                                       "DAWN_MQTT_BROKER"));
   PRINT_SETTING_INT("port", config->mqtt.port, "DAWN_MQTT_PORT",
                     detect_source_int(config->mqtt.port, defaults.mqtt.port, "DAWN_MQTT_PORT"));

   /* [network] */
   printf("[network]\n");
   PRINT_SETTING_BOOL("enabled", config->network.enabled, "DAWN_NETWORK_ENABLED",
                      detect_source_bool(config->network.enabled, defaults.network.enabled,
                                         "DAWN_NETWORK_ENABLED"));
   PRINT_SETTING_STR("host", config->network.host, "DAWN_NETWORK_HOST",
                     detect_source_str(config->network.host, defaults.network.host,
                                       "DAWN_NETWORK_HOST"));
   PRINT_SETTING_INT("port", config->network.port, "DAWN_NETWORK_PORT",
                     detect_source_int(config->network.port, defaults.network.port,
                                       "DAWN_NETWORK_PORT"));
   PRINT_SETTING_INT("workers", config->network.workers, "DAWN_NETWORK_WORKERS",
                     detect_source_int(config->network.workers, defaults.network.workers,
                                       "DAWN_NETWORK_WORKERS"));
   PRINT_SETTING_INT("socket_timeout_sec", config->network.socket_timeout_sec,
                     "DAWN_NETWORK_SOCKET_TIMEOUT_SEC",
                     detect_source_int(config->network.socket_timeout_sec,
                                       defaults.network.socket_timeout_sec,
                                       "DAWN_NETWORK_SOCKET_TIMEOUT_SEC"));
   PRINT_SETTING_INT("session_timeout_sec", config->network.session_timeout_sec,
                     "DAWN_NETWORK_SESSION_TIMEOUT_SEC",
                     detect_source_int(config->network.session_timeout_sec,
                                       defaults.network.session_timeout_sec,
                                       "DAWN_NETWORK_SESSION_TIMEOUT_SEC"));
   PRINT_SETTING_INT("llm_timeout_ms", config->network.llm_timeout_ms,
                     "DAWN_NETWORK_LLM_TIMEOUT_MS",
                     detect_source_int(config->network.llm_timeout_ms,
                                       defaults.network.llm_timeout_ms,
                                       "DAWN_NETWORK_LLM_TIMEOUT_MS"));

   /* [tui] */
   printf("[tui]\n");
   PRINT_SETTING_BOOL("enabled", config->tui.enabled, "DAWN_TUI_ENABLED",
                      detect_source_bool(config->tui.enabled, defaults.tui.enabled,
                                         "DAWN_TUI_ENABLED"));

   /* [debug] */
   printf("[debug]\n");
   PRINT_SETTING_BOOL("mic_record", config->debug.mic_record, "DAWN_DEBUG_MIC_RECORD",
                      detect_source_bool(config->debug.mic_record, defaults.debug.mic_record,
                                         "DAWN_DEBUG_MIC_RECORD"));
   PRINT_SETTING_BOOL("asr_record", config->debug.asr_record, "DAWN_DEBUG_ASR_RECORD",
                      detect_source_bool(config->debug.asr_record, defaults.debug.asr_record,
                                         "DAWN_DEBUG_ASR_RECORD"));
   PRINT_SETTING_BOOL("aec_record", config->debug.aec_record, "DAWN_DEBUG_AEC_RECORD",
                      detect_source_bool(config->debug.aec_record, defaults.debug.aec_record,
                                         "DAWN_DEBUG_AEC_RECORD"));
   PRINT_SETTING_STR("record_path", config->debug.record_path, "DAWN_DEBUG_RECORD_PATH",
                     detect_source_str(config->debug.record_path, defaults.debug.record_path,
                                       "DAWN_DEBUG_RECORD_PATH"));

   /* [paths] */
   printf("[paths]\n");
   PRINT_SETTING_STR("music_dir", config->paths.music_dir, "DAWN_PATHS_MUSIC_DIR",
                     detect_source_str(config->paths.music_dir, defaults.paths.music_dir,
                                       "DAWN_PATHS_MUSIC_DIR"));
   PRINT_SETTING_STR("commands_config", config->paths.commands_config, "DAWN_PATHS_COMMANDS_CONFIG",
                     detect_source_str(config->paths.commands_config,
                                       defaults.paths.commands_config,
                                       "DAWN_PATHS_COMMANDS_CONFIG"));

   /* Secrets (only show env var names, not values) */
   printf("================================================================================\n");
   printf("Secrets (values hidden)\n");
   printf("================================================================================\n\n");
   printf("  OPENAI_API_KEY                           %s\n",
          (secrets && secrets->openai_api_key[0]) ? "[set]" : "[not set]");
   printf("  ANTHROPIC_API_KEY                        %s\n",
          (secrets && secrets->claude_api_key[0]) ? "[set]" : "[not set]");
   printf("  MQTT_USERNAME                            %s\n",
          (secrets && secrets->mqtt_username[0]) ? "[set]" : "[not set]");
   printf("  MQTT_PASSWORD                            %s\n\n",
          (secrets && secrets->mqtt_password[0]) ? "[set]" : "[not set]");
}

void config_dump_toml(const dawn_config_t *config) {
   if (!config)
      return;

   printf("# DAWN Configuration (generated)\n");
   printf("# Save as: ~/.config/dawn/config.toml or ./dawn.toml\n\n");

   printf("[general]\n");
   printf("ai_name = \"%s\"\n", config->general.ai_name);
   if (config->general.log_file[0])
      printf("log_file = \"%s\"\n", config->general.log_file);

   printf("\n[localization]\n");
   if (config->localization.location[0])
      printf("location = \"%s\"\n", config->localization.location);
   if (config->localization.timezone[0])
      printf("timezone = \"%s\"\n", config->localization.timezone);
   printf("units = \"%s\"\n", config->localization.units);

   printf("\n[audio]\n");
   printf("backend = \"%s\"\n", config->audio.backend);
   printf("capture_device = \"%s\"\n", config->audio.capture_device);
   printf("playback_device = \"%s\"\n", config->audio.playback_device);

   printf("\n[audio.bargein]\n");
   printf("enabled = %s\n", config->audio.bargein.enabled ? "true" : "false");
   printf("cooldown_ms = %d\n", config->audio.bargein.cooldown_ms);
   printf("startup_cooldown_ms = %d\n", config->audio.bargein.startup_cooldown_ms);

   printf("\n[vad]\n");
   printf("speech_threshold = %.2f\n", config->vad.speech_threshold);
   printf("speech_threshold_tts = %.2f\n", config->vad.speech_threshold_tts);
   printf("silence_threshold = %.2f\n", config->vad.silence_threshold);
   printf("end_of_speech_duration = %.1f\n", config->vad.end_of_speech_duration);
   printf("max_recording_duration = %.1f\n", config->vad.max_recording_duration);
   printf("preroll_ms = %d\n", config->vad.preroll_ms);

   printf("\n[vad.chunking]\n");
   printf("enabled = %s\n", config->vad.chunking.enabled ? "true" : "false");
   printf("pause_duration = %.2f\n", config->vad.chunking.pause_duration);
   printf("min_chunk_duration = %.1f\n", config->vad.chunking.min_duration);
   printf("max_chunk_duration = %.1f\n", config->vad.chunking.max_duration);

   printf("\n[asr]\n");
   printf("model = \"%s\"\n", config->asr.model);
   printf("models_path = \"%s\"\n", config->asr.models_path);

   printf("\n[tts]\n");
   printf("voice_model = \"%s\"\n", config->tts.voice_model);
   printf("length_scale = %.2f\n", config->tts.length_scale);

   printf("\n[commands]\n");
   printf("processing_mode = \"%s\"\n", config->commands.processing_mode);

   printf("\n[llm]\n");
   printf("type = \"%s\"\n", config->llm.type);
   printf("max_tokens = %d\n", config->llm.max_tokens);

   printf("\n[llm.cloud]\n");
   printf("provider = \"%s\"\n", config->llm.cloud.provider);
   printf("model = \"%s\"\n", config->llm.cloud.model);
   if (config->llm.cloud.endpoint[0])
      printf("endpoint = \"%s\"\n", config->llm.cloud.endpoint);

   printf("\n[llm.local]\n");
   printf("endpoint = \"%s\"\n", config->llm.local.endpoint);
   if (config->llm.local.model[0])
      printf("model = \"%s\"\n", config->llm.local.model);

   printf("\n[search]\n");
   printf("engine = \"%s\"\n", config->search.engine);
   printf("endpoint = \"%s\"\n", config->search.endpoint);

   printf("\n[search.summarizer]\n");
   printf("backend = \"%s\"\n", config->search.summarizer.backend);
   printf("threshold_bytes = %zu\n", config->search.summarizer.threshold_bytes);
   printf("target_words = %zu\n", config->search.summarizer.target_words);

   printf("\n[mqtt]\n");
   printf("enabled = %s\n", config->mqtt.enabled ? "true" : "false");
   printf("broker = \"%s\"\n", config->mqtt.broker);
   printf("port = %d\n", config->mqtt.port);

   printf("\n[network]\n");
   printf("enabled = %s\n", config->network.enabled ? "true" : "false");
   printf("host = \"%s\"\n", config->network.host);
   printf("port = %d\n", config->network.port);
   printf("workers = %d\n", config->network.workers);

   printf("\n[tui]\n");
   printf("enabled = %s\n", config->tui.enabled ? "true" : "false");

   printf("\n[debug]\n");
   printf("mic_record = %s\n", config->debug.mic_record ? "true" : "false");
   printf("asr_record = %s\n", config->debug.asr_record ? "true" : "false");
   printf("aec_record = %s\n", config->debug.aec_record ? "true" : "false");
   printf("record_path = \"%s\"\n", config->debug.record_path);

   printf("\n[paths]\n");
   printf("music_dir = \"%s\"\n", config->paths.music_dir);
   printf("commands_config = \"%s\"\n", config->paths.commands_config);
}
