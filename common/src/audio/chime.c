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
 * Chime/alarm tone generation for scheduler notifications
 *
 * Pure math sine-wave synthesis with ADSR envelope. No platform dependencies.
 */

#include "audio/chime.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define CHIME_NOTE_DURATION_MS 250
#define ALARM_TONE_DURATION_MS 500

/**
 * Generate a sine tone with ADSR envelope, mixing into an existing buffer.
 * 10% attack, 10% decay, 60% sustain at 0.7, 20% release.
 */
static void generate_sine_tone(int16_t *buf, size_t samples, float freq, int sample_rate) {
   for (size_t i = 0; i < samples; i++) {
      float t = (float)i / (float)samples;
      float env;
      if (t < 0.1f)
         env = t / 0.1f;
      else if (t < 0.2f)
         env = 1.0f - 0.3f * ((t - 0.1f) / 0.1f);
      else if (t < 0.8f)
         env = 0.7f;
      else
         env = 0.7f * (1.0f - (t - 0.8f) / 0.2f);

      float sample = sinf(2.0f * (float)M_PI * freq * (float)i / (float)sample_rate) * env;
      /* Mix with existing content (for multi-tone) */
      float existing = (float)buf[i] / 32767.0f;
      float mixed = existing + sample * 0.5f;
      if (mixed > 1.0f)
         mixed = 1.0f;
      if (mixed < -1.0f)
         mixed = -1.0f;
      buf[i] = (int16_t)(mixed * 32767.0f);
   }
}

int dawn_chime_generate(dawn_chime_buf_t *out) {
   if (!out)
      return 1;

   memset(out, 0, sizeof(*out));

   /* 3-tone ascending chime: C5 (523Hz), E5 (659Hz), G5 (784Hz) */
   size_t note_samples = (DAWN_CHIME_SAMPLE_RATE * CHIME_NOTE_DURATION_MS) / 1000;
   out->samples = note_samples * 3;
   out->sample_rate = DAWN_CHIME_SAMPLE_RATE;
   out->pcm = calloc(out->samples, sizeof(int16_t));
   if (!out->pcm) {
      out->samples = 0;
      return 1;
   }

   static const float notes[] = { 523.25f, 659.25f, 783.99f };
   for (int i = 0; i < 3; i++) {
      generate_sine_tone(&out->pcm[i * note_samples], note_samples, notes[i],
                         DAWN_CHIME_SAMPLE_RATE);
   }

   return 0;
}

int dawn_alarm_tone_generate(dawn_chime_buf_t *out) {
   if (!out)
      return 1;

   memset(out, 0, sizeof(*out));

   /* Attention-getting tone: alternating A5 (880Hz) and E5 (659Hz) */
   size_t tone_samples = (DAWN_CHIME_SAMPLE_RATE * ALARM_TONE_DURATION_MS) / 1000;
   out->samples = tone_samples;
   out->sample_rate = DAWN_CHIME_SAMPLE_RATE;
   out->pcm = calloc(out->samples, sizeof(int16_t));
   if (!out->pcm) {
      out->samples = 0;
      return 1;
   }

   generate_sine_tone(out->pcm, out->samples / 2, 880.0f, DAWN_CHIME_SAMPLE_RATE);
   generate_sine_tone(&out->pcm[out->samples / 2], out->samples / 2, 659.25f,
                      DAWN_CHIME_SAMPLE_RATE);

   return 0;
}

void dawn_chime_free(dawn_chime_buf_t *buf) {
   if (!buf)
      return;
   free(buf->pcm);
   buf->pcm = NULL;
   buf->samples = 0;
}

void dawn_chime_apply_volume(int16_t *dst, const int16_t *src, size_t samples, float vol_scale) {
   for (size_t i = 0; i < samples; i++) {
      float val = (float)src[i] * vol_scale;
      if (val > 32767.0f)
         val = 32767.0f;
      if (val < -32767.0f)
         val = -32767.0f;
      dst[i] = (int16_t)val;
   }
}
