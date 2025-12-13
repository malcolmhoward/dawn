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
   ENV_STRING("DAWN_LOCALIZATION_LANGUAGE", config->localization.language);

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

   /* [llm.local] */
   ENV_STRING("DAWN_LLM_LOCAL_ENDPOINT", config->llm.local.endpoint);
   ENV_STRING("DAWN_LLM_LOCAL_MODEL", config->llm.local.model);

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
   printf("  config_version = %d\n", config->general.config_version);
   printf("  ai_name = \"%s\"\n", config->general.ai_name);
   printf("  log_file = \"%s\"\n", config->general.log_file);

   printf("\n[persona]\n");
   printf("  description = \"%.*s%s\"\n", config->persona.description[0] ? 50 : 0,
          config->persona.description, strlen(config->persona.description) > 50 ? "..." : "");

   printf("\n[localization]\n");
   printf("  location = \"%s\"\n", config->localization.location);
   printf("  timezone = \"%s\"\n", config->localization.timezone);
   printf("  units = \"%s\"\n", config->localization.units);
   printf("  language = \"%s\"\n", config->localization.language);

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

void config_dump_toml(const dawn_config_t *config) {
   if (!config)
      return;

   printf("# DAWN Configuration (generated)\n");
   printf("# Save as: ~/.config/dawn/config.toml or ./dawn.toml\n\n");

   printf("[general]\n");
   printf("config_version = %d\n", config->general.config_version);
   printf("ai_name = \"%s\"\n", config->general.ai_name);
   if (config->general.log_file[0])
      printf("log_file = \"%s\"\n", config->general.log_file);

   printf("\n[localization]\n");
   if (config->localization.location[0])
      printf("location = \"%s\"\n", config->localization.location);
   if (config->localization.timezone[0])
      printf("timezone = \"%s\"\n", config->localization.timezone);
   printf("units = \"%s\"\n", config->localization.units);
   printf("language = \"%s\"\n", config->localization.language);

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
