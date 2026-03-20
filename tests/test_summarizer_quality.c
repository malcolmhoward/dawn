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
 * TF-IDF Summarizer Quality Test Harness
 *
 * Fetches articles from diverse outlets and analyzes summarizer quality.
 * Generates a report with common noise patterns and improvement suggestions.
 *
 * Usage: ./test_summarizer_quality [options] [urls_file] [output_dir]
 *   Options:
 *     --flaresolverr  Enable FlareSolverr for Cloudflare bypass (requires running service)
 *     --help          Show this help message
 *   Default: test_urls.txt, results/
 */

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "config/dawn_config.h"
#include "tools/tfidf_summarizer.h"
#include "tools/url_fetcher.h"

/* =============================================================================
 * Configuration
 * ============================================================================= */

#define MAX_URL_LEN 512
#define MAX_URLS 100
#define MAX_CONTENT_SIZE (512 * 1024) /* 512KB max per article */

/* Quality issue types */
typedef enum {
   ISSUE_LOWERCASE_START, /* Sentence starts with lowercase */
   ISSUE_VERY_SHORT,      /* Sentence < 20 chars */
   ISSUE_HAS_URL,         /* Contains http:// or https:// */
   ISSUE_HAS_BRACKETS,    /* Contains [ ] or { } */
   ISSUE_SUBSCRIPTION,    /* Contains subscribe/newsletter patterns */
   ISSUE_TIMESTAMP,       /* Contains timestamp patterns */
   ISSUE_AUTHOR_BIO,      /* Contains author bio patterns */
   ISSUE_UI_ELEMENT,      /* Contains click here, read more, etc. */
   ISSUE_COUNT
} issue_type_t;

static const char *ISSUE_NAMES[] = { "Lowercase start",   "Very short (<20 chars)",
                                     "Contains URL",      "Contains brackets",
                                     "Subscription text", "Timestamp",
                                     "Author bio",        "UI element" };

/* Stats tracking */
typedef struct {
   int total_urls;
   int successful_fetches;
   int failed_fetches;
   int total_sentences;
   int issue_counts[ISSUE_COUNT];
   char common_patterns[50][128]; /* Store up to 50 suspicious patterns */
   int pattern_count;
} quality_stats_t;

/* =============================================================================
 * Helper Functions
 * ============================================================================= */

static int ensure_directory(const char *path) {
   struct stat st;
   if (stat(path, &st) == 0) {
      return S_ISDIR(st.st_mode) ? 0 : -1;
   }
   return mkdir(path, 0755);
}

static char *extract_domain(const char *url) {
   static char domain[128];
   const char *start = strstr(url, "://");
   if (!start) {
      strncpy(domain, "unknown", sizeof(domain));
      return domain;
   }
   start += 3;
   const char *end = strchr(start, '/');
   size_t len = end ? (size_t)(end - start) : strlen(start);
   if (len >= sizeof(domain))
      len = sizeof(domain) - 1;
   strncpy(domain, start, len);
   domain[len] = '\0';
   return domain;
}

static char *sanitize_filename(const char *url) {
   static char filename[256];
   const char *start = strstr(url, "://");
   if (start)
      start += 3;
   else
      start = url;

   size_t j = 0;
   for (size_t i = 0; start[i] && j < sizeof(filename) - 5; i++) {
      char c = start[i];
      if (isalnum(c) || c == '-' || c == '_') {
         filename[j++] = c;
      } else if (c == '/' || c == '.') {
         filename[j++] = '_';
      }
   }
   filename[j] = '\0';
   strcat(filename, ".txt");
   return filename;
}

/* =============================================================================
 * Quality Analysis Functions
 * ============================================================================= */

static bool check_lowercase_start(const char *sentence) {
   while (*sentence && isspace((unsigned char)*sentence))
      sentence++;
   return *sentence && islower((unsigned char)*sentence);
}

static bool check_contains_url(const char *sentence) {
   return strstr(sentence, "http://") != NULL || strstr(sentence, "https://") != NULL;
}

static bool check_contains_brackets(const char *sentence) {
   return strchr(sentence, '[') != NULL || strchr(sentence, '{') != NULL;
}

static bool check_subscription_pattern(const char *sentence) {
   const char *patterns[] = { "subscribe",     "newsletter", "sign up",     "signup",
                              "email address", "enter your", "receive our", NULL };
   char lower[512];
   size_t len = strlen(sentence);
   if (len >= sizeof(lower))
      len = sizeof(lower) - 1;
   for (size_t i = 0; i < len; i++)
      lower[i] = tolower((unsigned char)sentence[i]);
   lower[len] = '\0';

   for (int i = 0; patterns[i]; i++) {
      if (strstr(lower, patterns[i]))
         return true;
   }
   return false;
}

static bool check_timestamp_pattern(const char *sentence) {
   const char *patterns[] = { "ago", "minutes", "hours", "days", "just now", "updated", NULL };
   char lower[512];
   size_t len = strlen(sentence);
   if (len >= sizeof(lower))
      len = sizeof(lower) - 1;
   for (size_t i = 0; i < len; i++)
      lower[i] = tolower((unsigned char)sentence[i]);
   lower[len] = '\0';

   for (int i = 0; patterns[i]; i++) {
      if (strstr(lower, patterns[i]))
         return true;
   }
   return false;
}

static bool check_author_bio_pattern(const char *sentence) {
   const char *patterns[] = { "is a journalist",
                              "is a writer",
                              "is a reporter",
                              "based in",
                              "covers",
                              "writes about",
                              "freelance",
                              "award-winning",
                              "years of experience",
                              NULL };
   char lower[512];
   size_t len = strlen(sentence);
   if (len >= sizeof(lower))
      len = sizeof(lower) - 1;
   for (size_t i = 0; i < len; i++)
      lower[i] = tolower((unsigned char)sentence[i]);
   lower[len] = '\0';

   for (int i = 0; patterns[i]; i++) {
      if (strstr(lower, patterns[i]))
         return true;
   }
   return false;
}

static bool check_ui_element_pattern(const char *sentence) {
   const char *patterns[] = { "click here", "read more",     "see more",   "learn more",
                              "watch now",  "download",      "share this", "follow us",
                              "skip ad",    "advertisement", "cookie",     NULL };
   char lower[512];
   size_t len = strlen(sentence);
   if (len >= sizeof(lower))
      len = sizeof(lower) - 1;
   for (size_t i = 0; i < len; i++)
      lower[i] = tolower((unsigned char)sentence[i]);
   lower[len] = '\0';

   for (int i = 0; patterns[i]; i++) {
      if (strstr(lower, patterns[i]))
         return true;
   }
   return false;
}

static void analyze_summary(const char *summary, quality_stats_t *stats, FILE *report) {
   if (!summary || !*summary)
      return;

   /* Split summary into sentences - require whitespace after terminator */
   char *copy = strdup(summary);
   if (!copy)
      return;

   char *sentence = copy;
   char *p = copy;

   while (*p) {
      if (*p == '.' || *p == '!' || *p == '?') {
         /* Look ahead - skip closing quotes/parens */
         char *next = p + 1;
         while (*next && (*next == '"' || *next == '\'' || *next == ')')) {
            next++;
         }

         /* Only treat as sentence boundary if followed by whitespace or end */
         if (*next && !isspace((unsigned char)*next)) {
            p++;
            continue; /* Not a real sentence boundary */
         }

         char terminator = *p;
         *p = '\0';

         /* Trim leading whitespace */
         while (*sentence && isspace((unsigned char)*sentence))
            sentence++;

         size_t len = strlen(sentence);
         if (len > 0) {
            stats->total_sentences++;

            /* Check for issues */
            if (check_lowercase_start(sentence)) {
               stats->issue_counts[ISSUE_LOWERCASE_START]++;
               if (report)
                  fprintf(report, "  [LOWERCASE] %s\n", sentence);
            }
            if (len < 20) {
               stats->issue_counts[ISSUE_VERY_SHORT]++;
               if (report)
                  fprintf(report, "  [SHORT] %s\n", sentence);
            }
            if (check_contains_url(sentence)) {
               stats->issue_counts[ISSUE_HAS_URL]++;
               if (report)
                  fprintf(report, "  [URL] %s\n", sentence);
            }
            if (check_contains_brackets(sentence)) {
               stats->issue_counts[ISSUE_HAS_BRACKETS]++;
               if (report)
                  fprintf(report, "  [BRACKETS] %s\n", sentence);
            }
            if (check_subscription_pattern(sentence)) {
               stats->issue_counts[ISSUE_SUBSCRIPTION]++;
               if (report)
                  fprintf(report, "  [SUBSCRIPTION] %s\n", sentence);
            }
            if (check_timestamp_pattern(sentence)) {
               stats->issue_counts[ISSUE_TIMESTAMP]++;
               if (report)
                  fprintf(report, "  [TIMESTAMP] %s\n", sentence);
            }
            if (check_author_bio_pattern(sentence)) {
               stats->issue_counts[ISSUE_AUTHOR_BIO]++;
               if (report)
                  fprintf(report, "  [AUTHOR_BIO] %s\n", sentence);
            }
            if (check_ui_element_pattern(sentence)) {
               stats->issue_counts[ISSUE_UI_ELEMENT]++;
               if (report)
                  fprintf(report, "  [UI_ELEMENT] %s\n", sentence);
            }
         }

         *p = terminator;
         sentence = p + 1;
      }
      p++;
   }

   free(copy);
}

/* =============================================================================
 * Main Test Functions
 * ============================================================================= */

static int process_url(const char *url,
                       const char *output_dir,
                       quality_stats_t *stats,
                       FILE *report) {
   printf("Processing: %s\n", url);
   fprintf(report, "\n========================================\n");
   fprintf(report, "URL: %s\n", url);
   fprintf(report, "Domain: %s\n", extract_domain(url));
   fprintf(report, "========================================\n");

   /* Fetch the URL */
   char *content = NULL;
   size_t content_len = 0;
   int result = url_fetch_content(url, &content, &content_len);

   if (result != 0 || !content || content_len == 0) {
      printf("  FAILED: Could not fetch URL\n");
      fprintf(report, "FETCH FAILED (error %d)\n", result);
      stats->failed_fetches++;
      if (content)
         free(content);
      return -1;
   }

   stats->successful_fetches++;
   printf("  Fetched %zu bytes\n", content_len);
   fprintf(report, "Fetched: %zu bytes\n", content_len);

   /* Save markdown content */
   char markdown_path[512];
   snprintf(markdown_path, sizeof(markdown_path), "%s/markdown/%s", output_dir,
            sanitize_filename(url));
   FILE *md_file = fopen(markdown_path, "w");
   if (md_file) {
      fwrite(content, 1, content_len, md_file);
      fclose(md_file);
   }

   /* Run through summarizer */
   char *summary = NULL;
   int sum_result = tfidf_summarize(content, &summary, 0.25f, 3);

   if (sum_result != TFIDF_SUCCESS || !summary) {
      printf("  FAILED: Summarization failed\n");
      fprintf(report, "SUMMARIZATION FAILED (error %d)\n", sum_result);
      free(content);
      return -1;
   }

   size_t summary_len = strlen(summary);
   printf("  Summary: %zu bytes (%.1f%% reduction)\n", summary_len,
          (1.0 - (double)summary_len / content_len) * 100);
   fprintf(report, "Summary: %zu bytes (%.1f%% reduction)\n", summary_len,
           (1.0 - (double)summary_len / content_len) * 100);

   /* Save summary */
   char summary_path[512];
   snprintf(summary_path, sizeof(summary_path), "%s/summaries/%s", output_dir,
            sanitize_filename(url));
   FILE *sum_file = fopen(summary_path, "w");
   if (sum_file) {
      fwrite(summary, 1, summary_len, sum_file);
      fclose(sum_file);
   }

   /* Analyze quality */
   fprintf(report, "\n--- Quality Analysis ---\n");
   analyze_summary(summary, stats, report);

   /* Show summary preview in report */
   fprintf(report, "\n--- Summary Preview (first 500 chars) ---\n");
   size_t preview_len = summary_len > 500 ? 500 : summary_len;
   fprintf(report, "%.*s%s\n", (int)preview_len, summary, summary_len > 500 ? "..." : "");

   free(content);
   free(summary);
   return 0;
}

static void print_final_report(quality_stats_t *stats, FILE *report) {
   fprintf(report, "\n");
   fprintf(report, "============================================================\n");
   fprintf(report, "                    FINAL QUALITY REPORT\n");
   fprintf(report, "============================================================\n\n");

   fprintf(report, "FETCH STATISTICS:\n");
   fprintf(report, "  Total URLs:        %d\n", stats->total_urls);
   fprintf(report, "  Successful:        %d (%.1f%%)\n", stats->successful_fetches,
           stats->total_urls > 0 ? (100.0 * stats->successful_fetches / stats->total_urls) : 0);
   fprintf(report, "  Failed:            %d\n", stats->failed_fetches);
   fprintf(report, "\n");

   fprintf(report, "SUMMARY STATISTICS:\n");
   fprintf(report, "  Total sentences:   %d\n", stats->total_sentences);
   fprintf(report, "\n");

   fprintf(report, "QUALITY ISSUES DETECTED:\n");
   int total_issues = 0;
   for (int i = 0; i < ISSUE_COUNT; i++) {
      total_issues += stats->issue_counts[i];
      fprintf(report, "  %-20s %4d", ISSUE_NAMES[i], stats->issue_counts[i]);
      if (stats->total_sentences > 0) {
         fprintf(report, " (%.1f%%)", 100.0 * stats->issue_counts[i] / stats->total_sentences);
      }
      fprintf(report, "\n");
   }
   fprintf(report, "  %-20s %4d\n", "TOTAL ISSUES", total_issues);

   if (stats->total_sentences > 0) {
      double quality_score = 100.0 * (1.0 - (double)total_issues / stats->total_sentences);
      fprintf(report, "\n");
      fprintf(report, "QUALITY SCORE: %.1f%% (lower issues = higher score)\n", quality_score);
   }

   fprintf(report, "\n");
   fprintf(report, "RECOMMENDATIONS:\n");
   if (stats->issue_counts[ISSUE_SUBSCRIPTION] > 2) {
      fprintf(report, "  - Add more subscription-related patterns to NOISE_KEYWORDS\n");
   }
   if (stats->issue_counts[ISSUE_TIMESTAMP] > 2) {
      fprintf(report, "  - Add more timestamp patterns to NOISE_KEYWORDS\n");
   }
   if (stats->issue_counts[ISSUE_AUTHOR_BIO] > 2) {
      fprintf(report, "  - Add more author bio patterns to NOISE_KEYWORDS\n");
   }
   if (stats->issue_counts[ISSUE_UI_ELEMENT] > 2) {
      fprintf(report, "  - Add more UI element patterns to NOISE_KEYWORDS\n");
   }
   if (stats->issue_counts[ISSUE_HAS_BRACKETS] > 2) {
      fprintf(report, "  - Consider filtering sentences with unmatched brackets\n");
   }
   if (stats->issue_counts[ISSUE_LOWERCASE_START] > 5) {
      fprintf(report, "  - Review sentence merging logic for fragments\n");
   }

   fprintf(report, "\n============================================================\n");
}

/* =============================================================================
 * Usage
 * ============================================================================= */

static void print_usage(const char *progname) {
   printf("Usage: %s [options] [urls_file] [output_dir]\n", progname);
   printf("\nOptions:\n");
   printf("  --flaresolverr    Enable FlareSolverr for Cloudflare bypass\n");
   printf("                    (requires: docker run -d -p 8191:8191 flaresolverr/flaresolverr)\n");
   printf("  --help            Show this help message\n");
   printf("\nDefaults:\n");
   printf("  urls_file:  test_urls.txt\n");
   printf("  output_dir: results/\n");
}

/* =============================================================================
 * Main
 * ============================================================================= */

int main(int argc, char *argv[]) {
   bool use_flaresolverr = false;
   const char *urls_file = "test_urls.txt";
   const char *output_dir = "results";

   /* Parse command line options */
   static struct option long_options[] = { { "flaresolverr", no_argument, 0, 'f' },
                                           { "help", no_argument, 0, 'h' },
                                           { 0, 0, 0, 0 } };

   int opt;
   while ((opt = getopt_long(argc, argv, "fh", long_options, NULL)) != -1) {
      switch (opt) {
         case 'f':
            use_flaresolverr = true;
            break;
         case 'h':
            print_usage(argv[0]);
            return 0;
         default:
            print_usage(argv[0]);
            return 1;
      }
   }

   /* Positional arguments after options */
   if (optind < argc) {
      urls_file = argv[optind];
   }
   if (optind + 1 < argc) {
      output_dir = argv[optind + 1];
   }

   /* Initialize config with defaults */
   config_set_defaults(&g_config);

   /* Enable FlareSolverr if requested */
   if (use_flaresolverr) {
      g_config.url_fetcher.flaresolverr.enabled = true;
      printf("FlareSolverr: ENABLED (endpoint: %s)\n", g_config.url_fetcher.flaresolverr.endpoint);
   }

   printf("TF-IDF Summarizer Quality Test\n");
   printf("==============================\n");
   printf("URLs file:  %s\n", urls_file);
   printf("Output dir: %s\n", output_dir);
   printf("FlareSolverr: %s\n\n", use_flaresolverr ? "enabled" : "disabled");

   /* Create output directories */
   if (ensure_directory(output_dir) != 0) {
      fprintf(stderr, "Failed to create output directory: %s\n", output_dir);
      return 1;
   }

   char subdir[512];
   snprintf(subdir, sizeof(subdir), "%s/markdown", output_dir);
   ensure_directory(subdir);
   snprintf(subdir, sizeof(subdir), "%s/summaries", output_dir);
   ensure_directory(subdir);

   /* Open report file */
   char report_path[512];
   snprintf(report_path, sizeof(report_path), "%s/report.txt", output_dir);
   FILE *report = fopen(report_path, "w");
   if (!report) {
      fprintf(stderr, "Failed to create report file: %s\n", report_path);
      return 1;
   }

   /* Write report header */
   time_t now = time(NULL);
   fprintf(report, "TF-IDF Summarizer Quality Report\n");
   fprintf(report, "Generated: %s", ctime(&now));
   fprintf(report, "URLs file: %s\n", urls_file);

   /* Read URLs */
   FILE *urls = fopen(urls_file, "r");
   if (!urls) {
      fprintf(stderr, "Failed to open URLs file: %s\n", urls_file);
      fprintf(report, "\nERROR: Could not open URLs file\n");
      fclose(report);
      return 1;
   }

   /* Initialize stats */
   quality_stats_t stats = { 0 };

   /* Process each URL */
   char url[MAX_URL_LEN];
   while (fgets(url, sizeof(url), urls)) {
      /* Trim newline and skip comments/empty lines */
      size_t len = strlen(url);
      while (len > 0 && (url[len - 1] == '\n' || url[len - 1] == '\r'))
         url[--len] = '\0';

      if (len == 0 || url[0] == '#')
         continue;

      stats.total_urls++;
      process_url(url, output_dir, &stats, report);
   }

   fclose(urls);

   /* Print final report */
   print_final_report(&stats, report);
   fclose(report);

   /* Print summary to console */
   printf("\n==============================\n");
   printf("Test Complete!\n");
   printf("  Processed: %d URLs\n", stats.total_urls);
   printf("  Succeeded: %d\n", stats.successful_fetches);
   printf("  Failed:    %d\n", stats.failed_fetches);
   printf("  Sentences: %d\n", stats.total_sentences);

   int total_issues = 0;
   for (int i = 0; i < ISSUE_COUNT; i++)
      total_issues += stats.issue_counts[i];
   printf("  Issues:    %d\n", total_issues);

   if (stats.total_sentences > 0) {
      double quality_score = 100.0 * (1.0 - (double)total_issues / stats.total_sentences);
      printf("  Quality:   %.1f%%\n", quality_score);
   }

   printf("\nFull report: %s\n", report_path);

   return 0;
}
