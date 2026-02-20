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
 * Shared SDL2 UI Utilities
 */

#ifndef UI_UTIL_H
#define UI_UTIL_H

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <time.h>

/**
 * @brief Render text as a white texture for later tinting via SDL_SetTextureColorMod.
 *
 * Pattern: ui_build_white_tex() at init/state-change time, then at render time:
 *   SDL_SetTextureColorMod(tex, r, g, b) to apply theme or state color.
 * This avoids per-frame TTF_RenderUTF8_Blended calls.
 */
static inline SDL_Texture *ui_build_white_tex(SDL_Renderer *r,
                                              TTF_Font *font,
                                              const char *text,
                                              int *out_w,
                                              int *out_h) {
   SDL_Color white = { 255, 255, 255, 255 };
   SDL_Surface *s = TTF_RenderUTF8_Blended(font, text, white);
   if (!s)
      return NULL;
   SDL_Texture *tex = SDL_CreateTextureFromSurface(r, s);
   if (out_w)
      *out_w = s->w;
   if (out_h)
      *out_h = s->h;
   SDL_FreeSurface(s);
   return tex;
}

/**
 * @brief Try loading a TTF font from font_dir, then assets/fonts/, then fallback path.
 *
 * @param font_dir  Config-specified font directory (may be NULL)
 * @param filename  Font filename (e.g. "SourceSans3-Bold.ttf")
 * @param fallback  System fallback path (e.g. "/usr/share/fonts/..."), may be NULL
 * @param size      Font point size
 * @return Opened TTF_Font, or NULL if all paths fail
 */
static inline TTF_Font *ui_try_load_font(const char *font_dir,
                                         const char *filename,
                                         const char *fallback,
                                         int size) {
   TTF_Font *font = NULL;
   char path[512];

   if (font_dir && font_dir[0]) {
      snprintf(path, sizeof(path), "%s/%s", font_dir, filename);
      font = TTF_OpenFont(path, size);
      if (font)
         return font;
   }

   snprintf(path, sizeof(path), "assets/fonts/%s", filename);
   font = TTF_OpenFont(path, size);
   if (font)
      return font;

   if (fallback) {
      font = TTF_OpenFont(fallback, size);
      if (font)
         return font;
   }

   return NULL;
}

/**
 * @brief Get current monotonic time in seconds (for animations, frame timing).
 */
static inline double ui_get_time_sec(void) {
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

#endif /* UI_UTIL_H */
