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
 * Full-screen overlay for alarm/timer/reminder notifications with
 * dismiss and snooze buttons. Renders above screensaver.
 */

#ifndef UI_ALARM_H
#define UI_ALARM_H

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>

#include "audio/chime.h"

struct audio_playback; /* forward declare */

#define ALARM_LABEL_MAX 128
#define ALARM_ID_MAX 64

typedef enum {
   ALARM_STATE_IDLE,
   ALARM_STATE_FADING_IN,
   ALARM_STATE_ACTIVE,
   ALARM_STATE_FADING_OUT,
} alarm_overlay_state_t;

typedef struct {
   alarm_overlay_state_t state;

   /* Alarm payload (protected by mutex) */
   char alarm_id[ALARM_ID_MAX];
   char label[ALARM_LABEL_MAX];
   char type[16]; /* "alarm", "timer", "reminder" */
   int64_t event_id;

   /* Thread safety (alarm callback runs on lws thread) */
   pthread_mutex_t mutex;

   /* SDL resources (render thread only) */
   SDL_Renderer *renderer;
   TTF_Font *title_font;
   TTF_Font *label_font;
   TTF_Font *btn_font;
   int screen_w, screen_h;

   /* Cached textures */
   SDL_Texture *title_tex;
   int title_w, title_h;
   SDL_Texture *label_tex;
   int label_w, label_h;
   char cached_label[ALARM_LABEL_MAX];
   char cached_type[16];

   SDL_Texture *dismiss_tex;
   int dismiss_w, dismiss_h;
   SDL_Texture *snooze_tex;
   int snooze_w, snooze_h;
   bool static_cache_ready;

   /* Button hit areas (set during render) */
   SDL_Rect dismiss_btn;
   SDL_Rect snooze_btn;

   /* Fade animation */
   double fade_start;
   float fade_alpha; /* 0.0–1.0 */

   /* Callbacks (set by sdl_ui.c) */
   void (*on_dismiss)(int64_t event_id, void *userdata);
   void (*on_snooze)(int64_t event_id, int snooze_minutes, void *userdata);
   void *cb_userdata;

   /* Audio playback for chime sounds */
   struct audio_playback *audio_pb;
   dawn_chime_buf_t chime;
   dawn_chime_buf_t alarm_tone;
   pthread_t sound_thread;
   atomic_int sound_stop;
   bool sound_thread_active;
} ui_alarm_t;

/**
 * @brief Initialize alarm overlay
 * @param a Alarm context (embedded in sdl_ui)
 * @param r SDL renderer
 * @param w Screen width
 * @param h Screen height
 * @param font_dir Path to fonts directory
 * @return 0 on success
 */
int ui_alarm_init(ui_alarm_t *a, SDL_Renderer *r, int w, int h, const char *font_dir);

/**
 * @brief Cleanup alarm overlay
 */
void ui_alarm_cleanup(ui_alarm_t *a);

/**
 * @brief Trigger alarm overlay (thread-safe, called from ws callback)
 * @param a Alarm context
 * @param event_id Server event ID
 * @param label Display text (e.g., "Your pasta timer is done!")
 * @param type Event type string ("alarm", "timer", "reminder")
 */
void ui_alarm_trigger(ui_alarm_t *a, int64_t event_id, const char *label, const char *type);

/**
 * @brief Dismiss the alarm overlay (thread-safe)
 */
void ui_alarm_dismiss(ui_alarm_t *a);

/**
 * @brief Render alarm overlay (render thread only)
 */
void ui_alarm_render(ui_alarm_t *a, SDL_Renderer *r, double time_sec);

/**
 * @brief Check if alarm overlay is active
 */
bool ui_alarm_is_active(const ui_alarm_t *a);

/**
 * @brief Handle tap on alarm overlay
 * @return true if tap was consumed
 */
bool ui_alarm_handle_tap(ui_alarm_t *a, int x, int y);

/**
 * @brief Set audio playback context for chime sounds
 * @param a Alarm context
 * @param pb Audio playback context (from satellite main)
 */
void ui_alarm_set_audio_playback(ui_alarm_t *a, struct audio_playback *pb);

#endif /* UI_ALARM_H */
