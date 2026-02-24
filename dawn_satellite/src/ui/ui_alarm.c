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
 * Alarm/Timer Overlay for DAWN Satellite SDL UI
 *
 * Full-screen modal overlay with fade-in animation, dismiss/snooze buttons.
 * Renders above screensaver. Touch targets are 56px tall for reliability.
 */

#include "ui_alarm.h"

#include <SDL2/SDL2_gfxPrimitives.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "audio_playback.h"
#include "ui_colors.h"
#include "ui_util.h"

/* Log macros (satellite style) */
#ifndef LOG_INFO
#define LOG_INFO(fmt, ...) fprintf(stderr, "[INFO] alarm: " fmt "\n", ##__VA_ARGS__)
#endif
#ifndef LOG_WARNING
#define LOG_WARNING(fmt, ...) fprintf(stderr, "[WARN] alarm: " fmt "\n", ##__VA_ARGS__)
#endif

/* Font paths */
#define FALLBACK_MONO_FONT "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf"
#define FALLBACK_BODY_FONT "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"

/* Animation timing */
#define FADE_IN_DURATION 0.2   /* seconds */
#define FADE_OUT_DURATION 0.15 /* seconds */

/* Button geometry */
#define BTN_HEIGHT 56 /* Touch target height (design spec) */
#define BTN_WIDTH 200
#define BTN_GAP 24
#define BTN_RADIUS 12
#define SCRIM_ALPHA 0.75f   /* 75% opacity background */
#define ALARM_GAP_MS 200    /* Gap between alarm tone repeats (ms) */
#define ALARM_TIMEOUT_S 120 /* Max alarm sound duration (seconds) */

/* =============================================================================
 * Chime Sound Thread
 * ============================================================================= */

typedef struct {
   ui_alarm_t *alarm;
   bool is_alarm;
} chime_thread_arg_t;

static void *chime_sound_thread(void *arg) {
   chime_thread_arg_t *cta = (chime_thread_arg_t *)arg;
   ui_alarm_t *a = cta->alarm;
   bool is_alarm = cta->is_alarm;
   free(cta);

   audio_playback_t *pb = (audio_playback_t *)a->audio_pb;
   if (!pb) {
      a->sound_thread_active = false;
      return NULL;
   }

   /* Get volume and pre-scale into temp buffer */
   int vol_pct = audio_playback_get_volume(pb);
   float vol_scale = (float)vol_pct / 100.0f;

   dawn_chime_buf_t *src = is_alarm ? &a->alarm_tone : &a->chime;
   if (!src->pcm || src->samples == 0) {
      a->sound_thread_active = false;
      return NULL;
   }

   int16_t *scaled = malloc(src->samples * sizeof(int16_t));
   if (!scaled) {
      a->sound_thread_active = false;
      return NULL;
   }
   dawn_chime_apply_volume(scaled, src->pcm, src->samples, vol_scale);

   if (is_alarm) {
      /* Looping alarm tone until dismissed or timeout */
      time_t start = time(NULL);
      while (!atomic_load(&a->sound_stop) && (time(NULL) - start) < ALARM_TIMEOUT_S) {
         audio_playback_play(pb, scaled, src->samples, (unsigned int)src->sample_rate,
                             &a->sound_stop, true);
         if (atomic_load(&a->sound_stop))
            break;
         usleep(ALARM_GAP_MS * 1000);
      }
   } else {
      /* Single chime for timers/reminders */
      audio_playback_play(pb, scaled, src->samples, (unsigned int)src->sample_rate, &a->sound_stop,
                          true);
   }

   free(scaled);
   a->sound_thread_active = false;
   return NULL;
}

/** Stop and join any running sound thread */
static void stop_sound_thread(ui_alarm_t *a) {
   if (!a->sound_thread_active)
      return;
   atomic_store(&a->sound_stop, 1);
   pthread_join(a->sound_thread, NULL);
   a->sound_thread_active = false;
}

/** Start chime playback in background thread */
static void start_sound_thread(ui_alarm_t *a, bool is_alarm) {
   if (!a->audio_pb || !a->chime.pcm)
      return;

   stop_sound_thread(a);
   atomic_store(&a->sound_stop, 0);

   chime_thread_arg_t *cta = malloc(sizeof(chime_thread_arg_t));
   if (!cta)
      return;
   cta->alarm = a;
   cta->is_alarm = is_alarm;

   a->sound_thread_active = true;
   if (pthread_create(&a->sound_thread, NULL, chime_sound_thread, cta) != 0) {
      a->sound_thread_active = false;
      free(cta);
   }
}

/* =============================================================================
 * Helpers
 * ============================================================================= */

static void draw_rounded_rect(SDL_Renderer *r,
                              SDL_Rect *rect,
                              uint8_t cr,
                              uint8_t cg,
                              uint8_t cb,
                              uint8_t ca) {
   roundedBoxRGBA(r, rect->x, rect->y, rect->x + rect->w - 1, rect->y + rect->h - 1, BTN_RADIUS, cr,
                  cg, cb, ca);
}

/* =============================================================================
 * Lifecycle
 * ============================================================================= */

int ui_alarm_init(ui_alarm_t *a, SDL_Renderer *r, int w, int h, const char *font_dir) {
   memset(a, 0, sizeof(*a));
   pthread_mutex_init(&a->mutex, NULL);

   a->renderer = r;
   a->screen_w = w;
   a->screen_h = h;
   a->state = ALARM_STATE_IDLE;

   /* Load fonts */
   a->title_font = ui_try_load_font(font_dir, "SourceSans3-Bold.ttf", FALLBACK_BODY_FONT, 42);
   a->label_font = ui_try_load_font(font_dir, "SourceSans3-Medium.ttf", FALLBACK_BODY_FONT, 24);
   a->btn_font = ui_try_load_font(font_dir, "SourceSans3-SemiBold.ttf", FALLBACK_BODY_FONT, 22);

   if (!a->title_font || !a->label_font || !a->btn_font) {
      LOG_WARNING("Failed to load alarm overlay fonts");
      return 1;
   }

   /* Pre-cache button labels */
   a->dismiss_tex = ui_build_white_tex(r, a->btn_font, "Dismiss", &a->dismiss_w, &a->dismiss_h);
   a->snooze_tex = ui_build_white_tex(r, a->btn_font, "Snooze", &a->snooze_w, &a->snooze_h);
   a->static_cache_ready = (a->dismiss_tex != NULL && a->snooze_tex != NULL);

   /* Generate chime PCM buffers */
   dawn_chime_generate(&a->chime);
   dawn_alarm_tone_generate(&a->alarm_tone);
   atomic_init(&a->sound_stop, 0);

   LOG_INFO("initialized (%dx%d)", w, h);
   return 0;
}

void ui_alarm_cleanup(ui_alarm_t *a) {
   stop_sound_thread(a);
   dawn_chime_free(&a->chime);
   dawn_chime_free(&a->alarm_tone);

   if (a->title_tex)
      SDL_DestroyTexture(a->title_tex);
   if (a->label_tex)
      SDL_DestroyTexture(a->label_tex);
   if (a->dismiss_tex)
      SDL_DestroyTexture(a->dismiss_tex);
   if (a->snooze_tex)
      SDL_DestroyTexture(a->snooze_tex);
   if (a->title_font)
      TTF_CloseFont(a->title_font);
   if (a->label_font)
      TTF_CloseFont(a->label_font);
   if (a->btn_font)
      TTF_CloseFont(a->btn_font);
   pthread_mutex_destroy(&a->mutex);
   memset(a, 0, sizeof(*a));
}

/* =============================================================================
 * Trigger / Dismiss (thread-safe)
 * ============================================================================= */

void ui_alarm_trigger(ui_alarm_t *a, int64_t event_id, const char *label, const char *type) {
   pthread_mutex_lock(&a->mutex);

   a->event_id = event_id;
   if (label)
      snprintf(a->label, ALARM_LABEL_MAX, "%s", label);
   if (type)
      snprintf(a->type, sizeof(a->type), "%s", type);

   bool started_fade = false;
   if (a->state == ALARM_STATE_IDLE || a->state == ALARM_STATE_FADING_OUT) {
      a->state = ALARM_STATE_FADING_IN;
      a->fade_start = ui_get_time_sec();
      a->fade_alpha = 0.0f;
      started_fade = true;
   }

   pthread_mutex_unlock(&a->mutex);
   LOG_INFO("triggered: [%s] %s (id=%lld)", type ? type : "?", label ? label : "?",
            (long long)event_id);

   /* Start chime sound if overlay just activated */
   if (started_fade && a->audio_pb) {
      bool is_alarm = (type && strcmp(type, "alarm") == 0);
      start_sound_thread(a, is_alarm);
   }
}

void ui_alarm_dismiss(ui_alarm_t *a) {
   /* Stop sound (non-blocking signal; thread self-exits) */
   atomic_store(&a->sound_stop, 1);

   pthread_mutex_lock(&a->mutex);
   if (a->state == ALARM_STATE_FADING_IN || a->state == ALARM_STATE_ACTIVE) {
      a->state = ALARM_STATE_FADING_OUT;
      a->fade_start = ui_get_time_sec();
   }
   pthread_mutex_unlock(&a->mutex);
}

bool ui_alarm_is_active(const ui_alarm_t *a) {
   return a->state != ALARM_STATE_IDLE;
}

/* =============================================================================
 * Render
 * ============================================================================= */

void ui_alarm_render(ui_alarm_t *a, SDL_Renderer *r, double time_sec) {
   if (a->state == ALARM_STATE_IDLE)
      return;

   pthread_mutex_lock(&a->mutex);

   /* Update fade animation */
   double elapsed = time_sec - a->fade_start;
   switch (a->state) {
      case ALARM_STATE_FADING_IN:
         a->fade_alpha = (float)(elapsed / FADE_IN_DURATION);
         if (a->fade_alpha >= 1.0f) {
            a->fade_alpha = 1.0f;
            a->state = ALARM_STATE_ACTIVE;
         }
         break;
      case ALARM_STATE_ACTIVE:
         a->fade_alpha = 1.0f;
         break;
      case ALARM_STATE_FADING_OUT:
         a->fade_alpha = 1.0f - (float)(elapsed / FADE_OUT_DURATION);
         if (a->fade_alpha <= 0.0f) {
            a->fade_alpha = 0.0f;
            a->state = ALARM_STATE_IDLE;
            pthread_mutex_unlock(&a->mutex);
            return;
         }
         break;
      default:
         break;
   }

   float alpha = a->fade_alpha;
   uint8_t scrim_a = (uint8_t)(alpha * SCRIM_ALPHA * 255.0f);
   uint8_t text_a = (uint8_t)(alpha * 255.0f);

   /* Copy alarm data under mutex */
   char label[ALARM_LABEL_MAX];
   char type[16];
   snprintf(label, sizeof(label), "%s", a->label);
   snprintf(type, sizeof(type), "%s", a->type);
   pthread_mutex_unlock(&a->mutex);

   /* 1. Draw scrim */
   SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
   SDL_SetRenderDrawColor(r, 0, 0, 0, scrim_a);
   SDL_Rect full = { 0, 0, a->screen_w, a->screen_h };
   SDL_RenderFillRect(r, &full);

   /* Determine colors by type */
   uint8_t title_r, title_g, title_b;
   const char *title_text;
   if (strcmp(type, "alarm") == 0) {
      title_r = COLOR_THINKING_R; /* Amber for alarms */
      title_g = COLOR_THINKING_G;
      title_b = COLOR_THINKING_B;
      title_text = "ALARM";
   } else if (strcmp(type, "reminder") == 0) {
      title_r = 0x64; /* Teal-ish for reminders */
      title_g = 0xB5;
      title_b = 0xF6;
      title_text = "REMINDER";
   } else if (strcmp(type, "task") == 0) {
      title_r = 0x22; /* Green for tasks */
      title_g = 0xC5;
      title_b = 0x5E;
      title_text = "TASK COMPLETE";
   } else {
      title_r = 0x4C; /* Green for timers */
      title_g = 0xAF;
      title_b = 0x50;
      title_text = "TIMER";
   }

   /* 2. Rebuild cached textures if type/label changed */
   if (strcmp(a->cached_type, type) != 0) {
      if (a->title_tex)
         SDL_DestroyTexture(a->title_tex);
      a->title_tex = ui_build_white_tex(r, a->title_font, title_text, &a->title_w, &a->title_h);
      snprintf(a->cached_type, sizeof(a->cached_type), "%s", type);
   }
   if (strcmp(a->cached_label, label) != 0) {
      if (a->label_tex)
         SDL_DestroyTexture(a->label_tex);
      a->label_tex = ui_build_white_tex(r, a->label_font, label, &a->label_w, &a->label_h);
      snprintf(a->cached_label, sizeof(a->cached_label), "%s", label);
   }

   int cx = a->screen_w / 2;
   int cy = a->screen_h / 2;

   /* 3. Draw title */
   if (a->title_tex) {
      SDL_SetTextureColorMod(a->title_tex, title_r, title_g, title_b);
      SDL_SetTextureAlphaMod(a->title_tex, text_a);
      SDL_Rect dst = { cx - a->title_w / 2, cy - 80, a->title_w, a->title_h };
      SDL_RenderCopy(r, a->title_tex, NULL, &dst);
   }

   /* 4. Draw label */
   if (a->label_tex) {
      SDL_SetTextureColorMod(a->label_tex, 0xEE, 0xEE, 0xEE);
      SDL_SetTextureAlphaMod(a->label_tex, text_a);
      int max_w = a->screen_w - 40;
      int draw_w = a->label_w > max_w ? max_w : a->label_w;
      int draw_h = a->label_h;
      SDL_Rect dst = { cx - draw_w / 2, cy - 20, draw_w, draw_h };
      SDL_RenderCopy(r, a->label_tex, NULL, &dst);
   }

   /* 5. Draw buttons — snooze only for alarms */
   bool can_snooze = (strcmp(type, "alarm") == 0);
   int btn_y = cy + 40;

   if (can_snooze) {
      int total_w = BTN_WIDTH * 2 + BTN_GAP;
      int btn_x_dismiss = cx - total_w / 2;
      int btn_x_snooze = btn_x_dismiss + BTN_WIDTH + BTN_GAP;

      /* Dismiss button (red tinted) */
      a->dismiss_btn = (SDL_Rect){ btn_x_dismiss, btn_y, BTN_WIDTH, BTN_HEIGHT };
      draw_rounded_rect(r, &a->dismiss_btn, COLOR_ERROR_R, COLOR_ERROR_G, COLOR_ERROR_B,
                        (uint8_t)(alpha * 200));
      if (a->dismiss_tex) {
         SDL_SetTextureColorMod(a->dismiss_tex, 0xFF, 0xFF, 0xFF);
         SDL_SetTextureAlphaMod(a->dismiss_tex, text_a);
         SDL_Rect tdst = { btn_x_dismiss + (BTN_WIDTH - a->dismiss_w) / 2,
                           btn_y + (BTN_HEIGHT - a->dismiss_h) / 2, a->dismiss_w, a->dismiss_h };
         SDL_RenderCopy(r, a->dismiss_tex, NULL, &tdst);
      }

      /* Snooze button (darker, subtle) */
      a->snooze_btn = (SDL_Rect){ btn_x_snooze, btn_y, BTN_WIDTH, BTN_HEIGHT };
      draw_rounded_rect(r, &a->snooze_btn, 0x40, 0x40, 0x50, (uint8_t)(alpha * 200));
      if (a->snooze_tex) {
         SDL_SetTextureColorMod(a->snooze_tex, 0xCC, 0xCC, 0xCC);
         SDL_SetTextureAlphaMod(a->snooze_tex, text_a);
         SDL_Rect tdst = { btn_x_snooze + (BTN_WIDTH - a->snooze_w) / 2,
                           btn_y + (BTN_HEIGHT - a->snooze_h) / 2, a->snooze_w, a->snooze_h };
         SDL_RenderCopy(r, a->snooze_tex, NULL, &tdst);
      }
   } else {
      /* Single centered dismiss button for timers/reminders */
      int btn_x_dismiss = cx - BTN_WIDTH / 2;
      a->dismiss_btn = (SDL_Rect){ btn_x_dismiss, btn_y, BTN_WIDTH, BTN_HEIGHT };
      draw_rounded_rect(r, &a->dismiss_btn, COLOR_ERROR_R, COLOR_ERROR_G, COLOR_ERROR_B,
                        (uint8_t)(alpha * 200));
      if (a->dismiss_tex) {
         SDL_SetTextureColorMod(a->dismiss_tex, 0xFF, 0xFF, 0xFF);
         SDL_SetTextureAlphaMod(a->dismiss_tex, text_a);
         SDL_Rect tdst = { btn_x_dismiss + (BTN_WIDTH - a->dismiss_w) / 2,
                           btn_y + (BTN_HEIGHT - a->dismiss_h) / 2, a->dismiss_w, a->dismiss_h };
         SDL_RenderCopy(r, a->dismiss_tex, NULL, &tdst);
      }
      a->snooze_btn = (SDL_Rect){ 0, 0, 0, 0 }; /* No snooze hit area */
   }

   /* 6. Pulsing border for alarms */
   if (strcmp(type, "alarm") == 0 && a->state == ALARM_STATE_ACTIVE) {
      float pulse = 0.5f + 0.5f * sinf((float)time_sec * 6.2832f); /* 1Hz */
      uint8_t border_a = (uint8_t)(pulse * 120.0f);
      SDL_SetRenderDrawColor(r, title_r, title_g, title_b, border_a);
      for (int i = 0; i < 3; i++) {
         SDL_Rect border = { i, i, a->screen_w - 2 * i, a->screen_h - 2 * i };
         SDL_RenderDrawRect(r, &border);
      }
   }
}

/* =============================================================================
 * Touch Handling
 * ============================================================================= */

bool ui_alarm_handle_tap(ui_alarm_t *a, int x, int y) {
   if (a->state != ALARM_STATE_ACTIVE && a->state != ALARM_STATE_FADING_IN)
      return false;

   SDL_Point p = { x, y };

   if (SDL_PointInRect(&p, &a->dismiss_btn)) {
      LOG_INFO("dismiss tapped (event_id=%lld)", (long long)a->event_id);
      if (a->on_dismiss) {
         a->on_dismiss(a->event_id, a->cb_userdata);
      }
      ui_alarm_dismiss(a);
      return true;
   }

   if (SDL_PointInRect(&p, &a->snooze_btn)) {
      LOG_INFO("snooze tapped (event_id=%lld)", (long long)a->event_id);
      if (a->on_snooze) {
         a->on_snooze(a->event_id, 0, a->cb_userdata); /* 0 = use default */
      }
      ui_alarm_dismiss(a);
      return true;
   }

   return true; /* Consume tap even outside buttons (modal) */
}

/* =============================================================================
 * Audio Playback Wiring
 * ============================================================================= */

void ui_alarm_set_audio_playback(ui_alarm_t *a, struct audio_playback *pb) {
   if (a)
      a->audio_pb = pb;
}
