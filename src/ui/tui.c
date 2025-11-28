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

/**
 * @file tui.c
 * @brief Terminal User Interface implementation for DAWN
 *
 * Provides ncurses-based dashboard for real-time DAWN monitoring.
 */

#include "ui/tui.h"

#include <fcntl.h>
#include <locale.h>
#include <ncurses.h>
#include <string.h>
#include <unistd.h>

#include "logging.h"
#include "ui/metrics.h"

/* ============================================================================
 * Color Pair Definitions
 * ============================================================================ */

/* Color pairs for each theme element */
#define COLOR_PAIR_BORDER 1
#define COLOR_PAIR_TITLE 2
#define COLOR_PAIR_TEXT 3
#define COLOR_PAIR_HIGHLIGHT 4
#define COLOR_PAIR_DIM 5
#define COLOR_PAIR_VALUE 6
#define COLOR_PAIR_BAR 7
#define COLOR_PAIR_WARNING 8

/* ============================================================================
 * Static Variables
 * ============================================================================ */

static int g_tui_initialized = 0;
static tui_mode_t g_tui_mode = TUI_MODE_OFF;
static tui_theme_t g_current_theme = TUI_THEME_BLUE;
static int g_show_help = 0;
static int g_term_rows = 0;
static int g_term_cols = 0;
static int g_saved_stderr = -1; /* Saved stderr fd for restoration */
static int g_dev_null_fd = -1;  /* /dev/null fd */

/* Use ASCII box drawing for maximum compatibility */
#define USE_ASCII_BOX 1

/**
 * @brief Redirect stderr to /dev/null to suppress library output
 */
static void redirect_stderr_to_null(void) {
   if (g_saved_stderr == -1) {
      g_saved_stderr = dup(STDERR_FILENO);
      g_dev_null_fd = open("/dev/null", O_WRONLY);
      if (g_dev_null_fd != -1) {
         dup2(g_dev_null_fd, STDERR_FILENO);
      }
   }
}

/**
 * @brief Restore stderr from saved fd
 */
static void restore_stderr(void) {
   if (g_saved_stderr != -1) {
      dup2(g_saved_stderr, STDERR_FILENO);
      close(g_saved_stderr);
      g_saved_stderr = -1;
   }
   if (g_dev_null_fd != -1) {
      close(g_dev_null_fd);
      g_dev_null_fd = -1;
   }
}

/* ============================================================================
 * Theme Configuration
 * ============================================================================ */

static void setup_theme_green(void) {
   /* Apple ][ Classic Green theme */
   if (can_change_color()) {
      init_color(COLOR_GREEN, 0, 1000, 0);    /* Bright green */
      init_color(COLOR_YELLOW, 0, 800, 0);    /* Medium green for borders */
      init_color(COLOR_CYAN, 200, 1000, 200); /* Light green for highlights */
      init_color(COLOR_BLUE, 0, 533, 0);      /* Dark green for dim text */
   }

   init_pair(COLOR_PAIR_BORDER, COLOR_GREEN, COLOR_BLACK);
   init_pair(COLOR_PAIR_TITLE, COLOR_GREEN, COLOR_BLACK);
   init_pair(COLOR_PAIR_TEXT, COLOR_GREEN, COLOR_BLACK);
   init_pair(COLOR_PAIR_HIGHLIGHT, COLOR_GREEN, COLOR_BLACK);
   init_pair(COLOR_PAIR_DIM, COLOR_GREEN, COLOR_BLACK);
   init_pair(COLOR_PAIR_VALUE, COLOR_GREEN, COLOR_BLACK);
   init_pair(COLOR_PAIR_BAR, COLOR_GREEN, COLOR_BLACK);
   init_pair(COLOR_PAIR_WARNING, COLOR_BLACK, COLOR_GREEN);
}

static void setup_theme_blue(void) {
   /* JARVIS-inspired Blue theme */
   if (can_change_color()) {
      init_color(COLOR_CYAN, 0, 850, 1000);   /* Cyan-blue primary */
      init_color(COLOR_BLUE, 0, 600, 1000);   /* Electric blue borders */
      init_color(COLOR_WHITE, 0, 1000, 1000); /* Bright cyan highlights */
      init_color(COLOR_MAGENTA, 0, 800, 640); /* Teal accent */
   }

   init_pair(COLOR_PAIR_BORDER, COLOR_BLUE, COLOR_BLACK);
   init_pair(COLOR_PAIR_TITLE, COLOR_CYAN, COLOR_BLACK);
   init_pair(COLOR_PAIR_TEXT, COLOR_CYAN, COLOR_BLACK);
   init_pair(COLOR_PAIR_HIGHLIGHT, COLOR_WHITE, COLOR_BLACK);
   init_pair(COLOR_PAIR_DIM, COLOR_BLUE, COLOR_BLACK);
   init_pair(COLOR_PAIR_VALUE, COLOR_CYAN, COLOR_BLACK);
   init_pair(COLOR_PAIR_BAR, COLOR_CYAN, COLOR_BLACK);
   init_pair(COLOR_PAIR_WARNING, COLOR_BLACK, COLOR_CYAN);
}

static void setup_theme_bw(void) {
   /* High Contrast Black/White theme */
   init_pair(COLOR_PAIR_BORDER, COLOR_WHITE, COLOR_BLACK);
   init_pair(COLOR_PAIR_TITLE, COLOR_WHITE, COLOR_BLACK);
   init_pair(COLOR_PAIR_TEXT, COLOR_WHITE, COLOR_BLACK);
   init_pair(COLOR_PAIR_HIGHLIGHT, COLOR_WHITE, COLOR_BLACK);
   init_pair(COLOR_PAIR_DIM, COLOR_WHITE, COLOR_BLACK);
   init_pair(COLOR_PAIR_VALUE, COLOR_WHITE, COLOR_BLACK);
   init_pair(COLOR_PAIR_BAR, COLOR_WHITE, COLOR_BLACK);
   init_pair(COLOR_PAIR_WARNING, COLOR_BLACK, COLOR_WHITE);
}

static void apply_theme(tui_theme_t theme) {
   switch (theme) {
      case TUI_THEME_GREEN:
         setup_theme_green();
         break;
      case TUI_THEME_BLUE:
         setup_theme_blue();
         break;
      case TUI_THEME_BW:
         setup_theme_bw();
         break;
      default:
         setup_theme_blue();
         break;
   }
}

/* ============================================================================
 * Drawing Helpers
 * ============================================================================ */

/**
 * @brief Draw a box with title using simple ASCII characters
 *
 * Uses +, -, | for maximum terminal compatibility
 */
static void draw_box(int y, int x, int height, int width, const char *title) {
   attron(COLOR_PAIR(COLOR_PAIR_BORDER));

   /* Top border with title */
   mvaddch(y, x, '+');
   if (title && strlen(title) > 0) {
      addch('-');
      addch('[');
      attroff(COLOR_PAIR(COLOR_PAIR_BORDER));
      attron(COLOR_PAIR(COLOR_PAIR_TITLE) | A_BOLD);
      printw("%s", title);
      attroff(COLOR_PAIR(COLOR_PAIR_TITLE) | A_BOLD);
      attron(COLOR_PAIR(COLOR_PAIR_BORDER));
      addch(']');
      int title_len = strlen(title) + 4;
      for (int i = title_len; i < width - 2; i++) {
         addch('-');
      }
   } else {
      for (int i = 0; i < width - 2; i++) {
         addch('-');
      }
   }
   addch('+');

   /* Side borders */
   for (int i = 1; i < height - 1; i++) {
      mvaddch(y + i, x, '|');
      mvaddch(y + i, x + width - 1, '|');
   }

   /* Bottom border */
   mvaddch(y + height - 1, x, '+');
   for (int i = 0; i < width - 2; i++) {
      addch('-');
   }
   addch('+');

   attroff(COLOR_PAIR(COLOR_PAIR_BORDER));
}

/**
 * @brief Draw a horizontal progress bar using ASCII
 */
static void draw_bar(int y, int x, int width, float percentage) {
   int filled = (int)(percentage * width);
   if (filled > width)
      filled = width;
   if (filled < 0)
      filled = 0;

   attron(COLOR_PAIR(COLOR_PAIR_BAR));
   move(y, x);
   addch('[');
   for (int i = 0; i < width - 2; i++) {
      if (i < filled) {
         addch('#'); /* Hash for filled */
      } else {
         attron(A_DIM);
         addch('.'); /* Dot for empty */
         attroff(A_DIM);
      }
   }
   addch(']');
   attroff(COLOR_PAIR(COLOR_PAIR_BAR));
}

/**
 * @brief Format uptime as HH:MM:SS
 */
static void format_uptime(time_t seconds, char *buf, size_t buf_size) {
   int hours = seconds / 3600;
   int mins = (seconds % 3600) / 60;
   int secs = seconds % 60;
   snprintf(buf, buf_size, "%02d:%02d:%02d", hours, mins, secs);
}

/**
 * @brief Format large number with commas
 */
static void format_number(uint64_t num, char *buf, size_t buf_size) {
   if (num >= 1000000) {
      snprintf(buf, buf_size, "%lu,%03lu,%03lu", (unsigned long)(num / 1000000),
               (unsigned long)((num / 1000) % 1000), (unsigned long)(num % 1000));
   } else if (num >= 1000) {
      snprintf(buf, buf_size, "%lu,%03lu", (unsigned long)(num / 1000),
               (unsigned long)(num % 1000));
   } else {
      snprintf(buf, buf_size, "%lu", (unsigned long)num);
   }
}

/* ============================================================================
 * Panel Drawing Functions
 * ============================================================================ */

static void draw_status_panel(int y, int x, int width, dawn_metrics_t *metrics) {
   draw_box(y, x, 4, width, "DAWN Status");

   char uptime_str[16];
   format_uptime(metrics_get_uptime(), uptime_str, sizeof(uptime_str));

   /* Line 1: State, Uptime, Mode */
   attron(COLOR_PAIR(COLOR_PAIR_TEXT));
   mvprintw(y + 1, x + 2, "State: ");
   attroff(COLOR_PAIR(COLOR_PAIR_TEXT));
   attron(COLOR_PAIR(COLOR_PAIR_HIGHLIGHT) | A_BOLD);
   printw("%-18s", dawn_state_name(metrics->current_state));
   attroff(COLOR_PAIR(COLOR_PAIR_HIGHLIGHT) | A_BOLD);

   attron(COLOR_PAIR(COLOR_PAIR_TEXT));
   printw("  Uptime: ");
   attroff(COLOR_PAIR(COLOR_PAIR_TEXT));
   attron(COLOR_PAIR(COLOR_PAIR_VALUE));
   printw("%s", uptime_str);
   attroff(COLOR_PAIR(COLOR_PAIR_VALUE));

   /* Line 2: LLM info */
   const char *llm_type = (metrics->current_llm_type == LLM_LOCAL)
                              ? "Local"
                              : ((metrics->current_llm_type == LLM_CLOUD) ? "Cloud" : "N/A");
   const char *provider = (metrics->current_cloud_provider == CLOUD_PROVIDER_OPENAI)
                              ? "OpenAI"
                              : ((metrics->current_cloud_provider == CLOUD_PROVIDER_CLAUDE)
                                     ? "Claude"
                                     : "");

   attron(COLOR_PAIR(COLOR_PAIR_TEXT));
   mvprintw(y + 2, x + 2, "LLM: ");
   attroff(COLOR_PAIR(COLOR_PAIR_TEXT));
   attron(COLOR_PAIR(COLOR_PAIR_VALUE));
   if (strlen(provider) > 0) {
      printw("%s (%s)", llm_type, provider);
   } else {
      printw("%s", llm_type);
   }
   attroff(COLOR_PAIR(COLOR_PAIR_VALUE));

   attron(COLOR_PAIR(COLOR_PAIR_TEXT));
   printw("    ASR: ");
   attroff(COLOR_PAIR(COLOR_PAIR_TEXT));
   attron(COLOR_PAIR(COLOR_PAIR_VALUE));
   printw("Whisper");
   attroff(COLOR_PAIR(COLOR_PAIR_VALUE));

   attron(COLOR_PAIR(COLOR_PAIR_TEXT));
   printw("    VAD: ");
   attroff(COLOR_PAIR(COLOR_PAIR_TEXT));
   attron(COLOR_PAIR(COLOR_PAIR_VALUE));
   printw("Silero");
   attroff(COLOR_PAIR(COLOR_PAIR_VALUE));
}

static void draw_session_stats_panel(int y, int x, int width, dawn_metrics_t *metrics) {
   draw_box(y, x, 8, width, "Session Stats");

   char num_buf[32];

   /* Line 1: Query counts */
   attron(COLOR_PAIR(COLOR_PAIR_TEXT));
   mvprintw(y + 1, x + 2, "Queries: ");
   attroff(COLOR_PAIR(COLOR_PAIR_TEXT));
   attron(COLOR_PAIR(COLOR_PAIR_HIGHLIGHT) | A_BOLD);
   printw("%u total", metrics->queries_total);
   attroff(COLOR_PAIR(COLOR_PAIR_HIGHLIGHT) | A_BOLD);
   attron(COLOR_PAIR(COLOR_PAIR_DIM));
   printw("  (%u cloud, %u local)", metrics->queries_cloud, metrics->queries_local);
   attroff(COLOR_PAIR(COLOR_PAIR_DIM));

   attron(COLOR_PAIR(COLOR_PAIR_TEXT));
   printw("    Errors: ");
   attroff(COLOR_PAIR(COLOR_PAIR_TEXT));
   if (metrics->errors_count > 0) {
      attron(COLOR_PAIR(COLOR_PAIR_WARNING));
      printw("%u", metrics->errors_count);
      attroff(COLOR_PAIR(COLOR_PAIR_WARNING));
   } else {
      attron(COLOR_PAIR(COLOR_PAIR_VALUE));
      printw("0");
      attroff(COLOR_PAIR(COLOR_PAIR_VALUE));
   }

   attron(COLOR_PAIR(COLOR_PAIR_TEXT));
   printw("    Fallbacks: ");
   attroff(COLOR_PAIR(COLOR_PAIR_TEXT));
   attron(COLOR_PAIR(COLOR_PAIR_VALUE));
   printw("%u", metrics->fallbacks_count);
   attroff(COLOR_PAIR(COLOR_PAIR_VALUE));

   /* Line 3: Token header */
   attron(COLOR_PAIR(COLOR_PAIR_TEXT));
   mvprintw(y + 3, x + 2, "Token Usage (This Session):");
   attroff(COLOR_PAIR(COLOR_PAIR_TEXT));

   /* Line 4: Cloud tokens */
   uint64_t cloud_total = metrics->tokens_cloud_input + metrics->tokens_cloud_output;
   attron(COLOR_PAIR(COLOR_PAIR_DIM));
   mvprintw(y + 4, x + 4, "Cloud:  ");
   attroff(COLOR_PAIR(COLOR_PAIR_DIM));
   format_number(metrics->tokens_cloud_input, num_buf, sizeof(num_buf));
   attron(COLOR_PAIR(COLOR_PAIR_VALUE));
   printw("%s input", num_buf);
   attroff(COLOR_PAIR(COLOR_PAIR_VALUE));
   attron(COLOR_PAIR(COLOR_PAIR_DIM));
   printw("  +  ");
   attroff(COLOR_PAIR(COLOR_PAIR_DIM));
   format_number(metrics->tokens_cloud_output, num_buf, sizeof(num_buf));
   attron(COLOR_PAIR(COLOR_PAIR_VALUE));
   printw("%s output", num_buf);
   attroff(COLOR_PAIR(COLOR_PAIR_VALUE));
   attron(COLOR_PAIR(COLOR_PAIR_DIM));
   printw("  =  ");
   attroff(COLOR_PAIR(COLOR_PAIR_DIM));
   format_number(cloud_total, num_buf, sizeof(num_buf));
   attron(COLOR_PAIR(COLOR_PAIR_HIGHLIGHT));
   printw("%s total", num_buf);
   attroff(COLOR_PAIR(COLOR_PAIR_HIGHLIGHT));

   /* Line 5: Local tokens */
   uint64_t local_total = metrics->tokens_local_input + metrics->tokens_local_output;
   attron(COLOR_PAIR(COLOR_PAIR_DIM));
   mvprintw(y + 5, x + 4, "Local:  ");
   attroff(COLOR_PAIR(COLOR_PAIR_DIM));
   format_number(metrics->tokens_local_input, num_buf, sizeof(num_buf));
   attron(COLOR_PAIR(COLOR_PAIR_VALUE));
   printw("%s input", num_buf);
   attroff(COLOR_PAIR(COLOR_PAIR_VALUE));
   attron(COLOR_PAIR(COLOR_PAIR_DIM));
   printw("  +  ");
   attroff(COLOR_PAIR(COLOR_PAIR_DIM));
   format_number(metrics->tokens_local_output, num_buf, sizeof(num_buf));
   attron(COLOR_PAIR(COLOR_PAIR_VALUE));
   printw("%s output", num_buf);
   attroff(COLOR_PAIR(COLOR_PAIR_VALUE));
   attron(COLOR_PAIR(COLOR_PAIR_DIM));
   printw("  =  ");
   attroff(COLOR_PAIR(COLOR_PAIR_DIM));
   format_number(local_total, num_buf, sizeof(num_buf));
   attron(COLOR_PAIR(COLOR_PAIR_HIGHLIGHT));
   printw("%s total", num_buf);
   attroff(COLOR_PAIR(COLOR_PAIR_HIGHLIGHT));

   /* Line 6: Cached tokens */
   attron(COLOR_PAIR(COLOR_PAIR_DIM));
   mvprintw(y + 6, x + 4, "Cached: ");
   attroff(COLOR_PAIR(COLOR_PAIR_DIM));
   format_number(metrics->tokens_cached, num_buf, sizeof(num_buf));
   attron(COLOR_PAIR(COLOR_PAIR_VALUE));
   printw("%s tokens", num_buf);
   attroff(COLOR_PAIR(COLOR_PAIR_VALUE));
}

static void draw_performance_panel(int y, int x, int width, dawn_metrics_t *metrics) {
   draw_box(y, x, 10, width, "Last Query Performance");

   /* Show last command (truncated) */
   attron(COLOR_PAIR(COLOR_PAIR_TEXT));
   mvprintw(y + 1, x + 2, "Command: ");
   attroff(COLOR_PAIR(COLOR_PAIR_TEXT));
   attron(COLOR_PAIR(COLOR_PAIR_DIM));
   if (strlen(metrics->last_user_command) > 0) {
      /* Truncate if too long */
      int max_cmd_len = width - 14;
      if ((int)strlen(metrics->last_user_command) > max_cmd_len) {
         char truncated[256];
         strncpy(truncated, metrics->last_user_command, max_cmd_len - 3);
         truncated[max_cmd_len - 3] = '\0';
         printw("\"%s...\"", truncated);
      } else {
         printw("\"%s\"", metrics->last_user_command);
      }
   } else {
      printw("(none)");
   }
   attroff(COLOR_PAIR(COLOR_PAIR_DIM));

   /* Latency breakdown header */
   attron(COLOR_PAIR(COLOR_PAIR_TEXT));
   mvprintw(y + 3, x + 2, "Latency Breakdown:");
   attroff(COLOR_PAIR(COLOR_PAIR_TEXT));

   /* Calculate max for bar scaling (use LLM total as reference) */
   double max_ms = metrics->last_llm_total_ms;
   if (max_ms < 100)
      max_ms = 1000; /* Default scale */

   int bar_width = 20;
   int label_col = x + 4;
   int value_col = x + 26;
   int bar_col = x + 38;

   /* ASR */
   attron(COLOR_PAIR(COLOR_PAIR_DIM));
   mvprintw(y + 4, label_col, "ASR (RTF: %.3f):", metrics->last_asr_rtf);
   attroff(COLOR_PAIR(COLOR_PAIR_DIM));
   attron(COLOR_PAIR(COLOR_PAIR_VALUE));
   mvprintw(y + 4, value_col, "%6.0f ms", metrics->last_asr_time_ms);
   attroff(COLOR_PAIR(COLOR_PAIR_VALUE));
   draw_bar(y + 4, bar_col, bar_width, (float)(metrics->last_asr_time_ms / max_ms));

   /* LLM TTFT */
   attron(COLOR_PAIR(COLOR_PAIR_DIM));
   mvprintw(y + 5, label_col, "LLM TTFT:");
   attroff(COLOR_PAIR(COLOR_PAIR_DIM));
   attron(COLOR_PAIR(COLOR_PAIR_VALUE));
   mvprintw(y + 5, value_col, "%6.0f ms", metrics->last_llm_ttft_ms);
   attroff(COLOR_PAIR(COLOR_PAIR_VALUE));
   draw_bar(y + 5, bar_col, bar_width, (float)(metrics->last_llm_ttft_ms / max_ms));

   /* LLM Total */
   attron(COLOR_PAIR(COLOR_PAIR_DIM));
   mvprintw(y + 6, label_col, "LLM Total:");
   attroff(COLOR_PAIR(COLOR_PAIR_DIM));
   attron(COLOR_PAIR(COLOR_PAIR_VALUE));
   mvprintw(y + 6, value_col, "%6.0f ms", metrics->last_llm_total_ms);
   attroff(COLOR_PAIR(COLOR_PAIR_VALUE));
   draw_bar(y + 6, bar_col, bar_width, (float)(metrics->last_llm_total_ms / max_ms));

   /* TTS */
   attron(COLOR_PAIR(COLOR_PAIR_DIM));
   mvprintw(y + 7, label_col, "TTS Generation:");
   attroff(COLOR_PAIR(COLOR_PAIR_DIM));
   attron(COLOR_PAIR(COLOR_PAIR_VALUE));
   mvprintw(y + 7, value_col, "%6.0f ms", metrics->last_tts_time_ms);
   attroff(COLOR_PAIR(COLOR_PAIR_VALUE));
   draw_bar(y + 7, bar_col, bar_width, (float)(metrics->last_tts_time_ms / max_ms));

   /* Total Pipeline */
   attron(COLOR_PAIR(COLOR_PAIR_TEXT));
   mvprintw(y + 8, label_col, "Total Pipeline:");
   attroff(COLOR_PAIR(COLOR_PAIR_TEXT));
   attron(COLOR_PAIR(COLOR_PAIR_HIGHLIGHT) | A_BOLD);
   mvprintw(y + 8, value_col, "%6.0f ms", metrics->last_total_pipeline_ms);
   attroff(COLOR_PAIR(COLOR_PAIR_HIGHLIGHT) | A_BOLD);
}

static void draw_realtime_panel(int y, int x, int width, dawn_metrics_t *metrics) {
   draw_box(y, x, 4, width, "Real-Time Audio");

   /* VAD probability bar */
   attron(COLOR_PAIR(COLOR_PAIR_TEXT));
   mvprintw(y + 1, x + 2, "VAD Speech Probability: ");
   attroff(COLOR_PAIR(COLOR_PAIR_TEXT));

   int bar_width = 20;
   draw_bar(y + 1, x + 26, bar_width, metrics->current_vad_probability);

   attron(COLOR_PAIR(COLOR_PAIR_VALUE));
   printw(" %3.0f%%", metrics->current_vad_probability * 100);
   attroff(COLOR_PAIR(COLOR_PAIR_VALUE));

   /* State indicator */
   attron(COLOR_PAIR(COLOR_PAIR_DIM));
   mvprintw(y + 2, x + 2, "State: ");
   attroff(COLOR_PAIR(COLOR_PAIR_DIM));

   /* Color state based on activity */
   if (metrics->current_state == DAWN_STATE_COMMAND_RECORDING ||
       metrics->current_state == DAWN_STATE_PROCESS_COMMAND) {
      attron(COLOR_PAIR(COLOR_PAIR_HIGHLIGHT) | A_BOLD | A_BLINK);
   } else if (metrics->current_state == DAWN_STATE_WAKEWORD_LISTEN) {
      attron(COLOR_PAIR(COLOR_PAIR_VALUE));
   } else {
      attron(COLOR_PAIR(COLOR_PAIR_DIM));
   }
   printw("%s", dawn_state_name(metrics->current_state));
   attroff(COLOR_PAIR(COLOR_PAIR_HIGHLIGHT) | A_BOLD | A_BLINK);
   attroff(COLOR_PAIR(COLOR_PAIR_VALUE));
   attroff(COLOR_PAIR(COLOR_PAIR_DIM));
}

static void draw_activity_panel(int y, int x, int width, int height, dawn_metrics_t *metrics) {
   draw_box(y, x, height, width, "Recent Activity");

   int max_lines = height - 2;
   int start_idx;

   if (metrics->log_count <= max_lines) {
      start_idx = 0;
   } else {
      /* Show most recent entries */
      start_idx = (metrics->log_head - max_lines + METRICS_MAX_LOG_ENTRIES) %
                  METRICS_MAX_LOG_ENTRIES;
   }

   int lines_to_show = (metrics->log_count < max_lines) ? metrics->log_count : max_lines;

   for (int i = 0; i < lines_to_show; i++) {
      int idx = (start_idx + i) % METRICS_MAX_LOG_ENTRIES;
      attron(COLOR_PAIR(COLOR_PAIR_DIM));
      mvprintw(y + 1 + i, x + 2, "%-*.*s", width - 4, width - 4, metrics->activity_log[idx]);
      attroff(COLOR_PAIR(COLOR_PAIR_DIM));
   }
}

static void draw_footer(int y, int width) {
   attron(COLOR_PAIR(COLOR_PAIR_DIM));
   mvprintw(y, 1, " [D]ebug Logs  [R]eset Stats  [1/2/3] Theme  [Q]uit");
   attroff(COLOR_PAIR(COLOR_PAIR_DIM));

   /* Right-aligned help hint */
   const char *help_text = "Press ? for help";
   mvprintw(y, width - strlen(help_text) - 1, "%s", help_text);
}

static void draw_help_overlay(void) {
   int height = 14;
   int width = 50;
   int start_y = (g_term_rows - height) / 2;
   int start_x = (g_term_cols - width) / 2;

   /* Draw help box */
   attron(COLOR_PAIR(COLOR_PAIR_WARNING));
   for (int i = 0; i < height; i++) {
      mvhline(start_y + i, start_x, ' ', width);
   }

   mvprintw(start_y + 1, start_x + 2, "DAWN TUI Help");
   mvprintw(start_y + 2, start_x + 2, "---------------------------------------------");

   mvprintw(start_y + 4, start_x + 4, "Q        Quit DAWN");
   mvprintw(start_y + 5, start_x + 4, "D        Toggle Debug Log mode");
   mvprintw(start_y + 6, start_x + 4, "R        Reset session statistics");
   mvprintw(start_y + 7, start_x + 4, "1        Green (Apple ][) theme");
   mvprintw(start_y + 8, start_x + 4, "2        Blue (JARVIS) theme");
   mvprintw(start_y + 9, start_x + 4, "3        B/W (High Contrast) theme");
   mvprintw(start_y + 10, start_x + 4, "?        Toggle this help");

   mvprintw(start_y + 12, start_x + 2, "Press any key to close...");
   attroff(COLOR_PAIR(COLOR_PAIR_WARNING));
}

/* ============================================================================
 * Public API Implementation
 * ============================================================================ */

int tui_init(tui_theme_t theme) {
   /* Check if stdout is a TTY */
   if (!isatty(STDOUT_FILENO)) {
      LOG_WARNING("TUI: stdout is not a TTY, TUI disabled");
      return 1;
   }

   /* Redirect stderr FIRST to suppress library output during initialization */
   redirect_stderr_to_null();

   /* Set locale for Unicode box drawing */
   setlocale(LC_ALL, "");

   /* Initialize ncurses */
   if (initscr() == NULL) {
      restore_stderr(); /* Restore on failure */
      LOG_ERROR("TUI: Failed to initialize ncurses");
      return 1;
   }

   /* Get terminal size */
   getmaxyx(stdscr, g_term_rows, g_term_cols);

   /* Check minimum size */
   if (g_term_cols < TUI_MIN_COLS || g_term_rows < TUI_MIN_ROWS) {
      endwin();
      restore_stderr(); /* Restore on failure */
      LOG_WARNING("TUI: Terminal too small (%dx%d), need at least %dx%d", g_term_cols, g_term_rows,
                  TUI_MIN_COLS, TUI_MIN_ROWS);
      return 1;
   }

   /* Configure ncurses */
   cbreak();              /* Disable line buffering */
   noecho();              /* Don't echo input */
   keypad(stdscr, TRUE);  /* Enable special keys */
   nodelay(stdscr, TRUE); /* Non-blocking input */
   curs_set(0);           /* Hide cursor */

   /* Initialize colors */
   if (has_colors()) {
      start_color();
      use_default_colors();
      apply_theme(theme);
      g_current_theme = theme;
   }

   g_tui_initialized = 1;
   g_tui_mode = TUI_MODE_DISPLAY;

   /* Note: stderr already redirected at start of function */

   LOG_INFO("TUI initialized (%dx%d)", g_term_cols, g_term_rows);
   return 0;
}

void tui_cleanup(void) {
   if (!g_tui_initialized) {
      return;
   }

   /* Restore stderr before cleanup messages */
   restore_stderr();

   /* Restore terminal */
   curs_set(1);
   echo();
   nocbreak();
   endwin();

   g_tui_initialized = 0;
   g_tui_mode = TUI_MODE_OFF;
   LOG_INFO("TUI cleaned up");
}

int tui_is_active(void) {
   return g_tui_initialized && (g_tui_mode != TUI_MODE_OFF);
}

tui_mode_t tui_get_mode(void) {
   return g_tui_mode;
}

void tui_set_mode(tui_mode_t mode) {
   g_tui_mode = mode;
}

void tui_toggle_debug_mode(void) {
   if (g_tui_mode == TUI_MODE_DISPLAY) {
      g_tui_mode = TUI_MODE_DEBUG_LOG;
      /* Temporarily disable TUI display */
      endwin();
      /* Restore stderr for debug output */
      restore_stderr();
      /* Re-enable console logging for debug mode */
      logging_suppress_console(0);
      LOG_INFO("Switched to debug log mode (press D to return to TUI)");
   } else if (g_tui_mode == TUI_MODE_DEBUG_LOG) {
      /* Suppress console logging again */
      logging_suppress_console(1);
      /* Redirect stderr again */
      redirect_stderr_to_null();
      /* Re-enable TUI */
      refresh();
      g_tui_mode = TUI_MODE_DISPLAY;
   }
}

void tui_set_theme(tui_theme_t theme) {
   if (theme >= TUI_THEME_GREEN && theme <= TUI_THEME_BW) {
      g_current_theme = theme;
      apply_theme(theme);
   }
}

tui_theme_t tui_get_theme(void) {
   return g_current_theme;
}

void tui_update(void) {
   if (!g_tui_initialized || g_tui_mode != TUI_MODE_DISPLAY) {
      return;
   }

   /* Get current metrics snapshot */
   dawn_metrics_t metrics;
   metrics_get_snapshot(&metrics);

   /* Clear screen */
   erase();

   /* Calculate layout */
   int panel_width = g_term_cols - 2;
   int y = 0;

   /* Draw panels */
   draw_status_panel(y, 1, panel_width, &metrics);
   y += 4;

   draw_session_stats_panel(y, 1, panel_width, &metrics);
   y += 8;

   draw_performance_panel(y, 1, panel_width, &metrics);
   y += 10;

   draw_realtime_panel(y, 1, panel_width, &metrics);
   y += 4;

   /* Activity panel fills remaining space */
   int activity_height = g_term_rows - y - 2;
   if (activity_height < 4)
      activity_height = 4;
   draw_activity_panel(y, 1, panel_width, activity_height, &metrics);

   /* Footer */
   draw_footer(g_term_rows - 1, g_term_cols);

   /* Help overlay if active */
   if (g_show_help) {
      draw_help_overlay();
   }

   /* Refresh display */
   refresh();
}

int tui_handle_input(void) {
   if (!g_tui_initialized) {
      return 0;
   }

   int ch = getch();
   if (ch == ERR) {
      return 0; /* No input */
   }

   /* If help is showing, any key closes it */
   if (g_show_help) {
      g_show_help = 0;
      return 0;
   }

   switch (ch) {
      case 'q':
      case 'Q':
         return 1; /* Request quit */

      case 'd':
      case 'D':
         tui_toggle_debug_mode();
         break;

      case 'r':
      case 'R':
         metrics_reset();
         break;

      case '1':
         tui_set_theme(TUI_THEME_GREEN);
         break;

      case '2':
         tui_set_theme(TUI_THEME_BLUE);
         break;

      case '3':
         tui_set_theme(TUI_THEME_BW);
         break;

      case '?':
         g_show_help = !g_show_help;
         break;

      case KEY_RESIZE:
         tui_handle_resize();
         break;

      default:
         break;
   }

   return 0;
}

void tui_show_help(void) {
   g_show_help = 1;
}

void tui_hide_help(void) {
   g_show_help = 0;
}

int tui_check_terminal_size(void) {
   getmaxyx(stdscr, g_term_rows, g_term_cols);
   return (g_term_cols >= TUI_MIN_COLS && g_term_rows >= TUI_MIN_ROWS);
}

void tui_handle_resize(void) {
   getmaxyx(stdscr, g_term_rows, g_term_cols);

   if (!tui_check_terminal_size()) {
      /* Terminal too small - switch to debug mode */
      LOG_WARNING("TUI: Terminal resized to %dx%d (too small), switching to debug mode",
                  g_term_cols, g_term_rows);
      tui_toggle_debug_mode();
   } else {
      /* Redraw with new size */
      clear();
      tui_update();
   }
}
