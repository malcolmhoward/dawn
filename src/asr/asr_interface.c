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

#include "asr/asr_interface.h"

#include <stdlib.h>
#include <string.h>

#ifdef ENABLE_VOSK
#include "asr/asr_vosk.h"
#else
#include "asr/asr_whisper.h"
#endif
#include "logging.h"

/**
 * @brief ASR context structure
 *
 * Holds engine-specific context and function pointers for polymorphic dispatch.
 */
struct asr_context {
   asr_engine_type_t engine_type;
   void *engine_context;  // Opaque pointer to engine-specific context

   // Function pointers for polymorphic dispatch
   asr_result_t *(*process_partial)(void *ctx, const int16_t *audio, size_t samples);
   asr_result_t *(*finalize)(void *ctx);
   int (*reset)(void *ctx);
   void (*cleanup)(void *ctx);
};

asr_context_t *asr_init(asr_engine_type_t engine_type, const char *model_path, int sample_rate) {
   if (!model_path) {
      LOG_ERROR("ASR: model_path cannot be NULL");
      return NULL;
   }

   asr_context_t *ctx = (asr_context_t *)calloc(1, sizeof(asr_context_t));
   if (!ctx) {
      LOG_ERROR("ASR: Failed to allocate context");
      return NULL;
   }

   ctx->engine_type = engine_type;

   switch (engine_type) {
#ifdef ENABLE_VOSK
      case ASR_ENGINE_VOSK:
         LOG_INFO("ASR: Initializing Vosk engine (model: %s, sample_rate: %d)", model_path,
                  sample_rate);
         ctx->engine_context = asr_vosk_init(model_path, sample_rate);
         if (!ctx->engine_context) {
            LOG_ERROR("ASR: Vosk initialization failed");
            free(ctx);
            return NULL;
         }
         ctx->process_partial = asr_vosk_process_partial;
         ctx->finalize = asr_vosk_finalize;
         ctx->reset = asr_vosk_reset;
         ctx->cleanup = asr_vosk_cleanup;
         break;
#else
      case ASR_ENGINE_WHISPER:
         LOG_INFO("ASR: Initializing Whisper engine (model: %s, sample_rate: %d)", model_path,
                  sample_rate);
         ctx->engine_context = asr_whisper_init(model_path, sample_rate);
         if (!ctx->engine_context) {
            LOG_ERROR("ASR: Whisper initialization failed");
            free(ctx);
            return NULL;
         }
         ctx->process_partial = asr_whisper_process_partial;
         ctx->finalize = asr_whisper_finalize;
         ctx->reset = asr_whisper_reset;
         ctx->cleanup = asr_whisper_cleanup;
         break;
#endif

      default:
         LOG_ERROR("ASR: Unknown engine type: %d", engine_type);
         free(ctx);
         return NULL;
   }

   LOG_INFO("ASR: %s engine initialized successfully", asr_engine_name(engine_type));
   return ctx;
}

asr_result_t *asr_process_partial(asr_context_t *ctx,
                                  const int16_t *audio_data,
                                  size_t num_samples) {
   if (!ctx || !audio_data) {
      LOG_ERROR("ASR: Invalid parameters to asr_process_partial");
      return NULL;
   }

   return ctx->process_partial(ctx->engine_context, audio_data, num_samples);
}

asr_result_t *asr_finalize(asr_context_t *ctx) {
   if (!ctx) {
      LOG_ERROR("ASR: Invalid context in asr_finalize");
      return NULL;
   }

   return ctx->finalize(ctx->engine_context);
}

int asr_reset(asr_context_t *ctx) {
   if (!ctx) {
      LOG_ERROR("ASR: Invalid context in asr_reset");
      return ASR_FAILURE;
   }

   return ctx->reset(ctx->engine_context);
}

void asr_result_free(asr_result_t *result) {
   if (!result) {
      return;
   }

   if (result->text) {
      free(result->text);
   }

   free(result);
}

void asr_cleanup(asr_context_t *ctx) {
   if (!ctx) {
      return;
   }

   LOG_INFO("ASR: Cleaning up %s engine", asr_engine_name(ctx->engine_type));

   if (ctx->cleanup && ctx->engine_context) {
      ctx->cleanup(ctx->engine_context);
   }

   free(ctx);
}

const char *asr_engine_name(asr_engine_type_t engine_type) {
   switch (engine_type) {
#ifdef ENABLE_VOSK
      case ASR_ENGINE_VOSK:
         return "Vosk";
#else
      case ASR_ENGINE_WHISPER:
         return "Whisper";
#endif
      default:
         return "Unknown";
   }
}

asr_engine_type_t asr_get_engine_type(asr_context_t *ctx) {
   if (!ctx) {
      LOG_ERROR("ASR: asr_get_engine_type() called with NULL context");
      return (asr_engine_type_t)-1;
   }
   return ctx->engine_type;
}
