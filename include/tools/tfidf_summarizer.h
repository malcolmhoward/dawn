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
 * TF-IDF Extractive Summarization - Local text summarization without LLM
 *
 * This module implements extractive summarization using Term Frequency-Inverse
 * Document Frequency (TF-IDF) scoring. It selects the most important sentences
 * from the input text based on their word significance.
 *
 * Algorithm:
 * 1. Split document into sentences
 * 2. Tokenize words, remove stopwords
 * 3. Calculate TF-IDF scores per sentence
 * 4. Apply position weighting (first sentence 1.2x, last 1.1x)
 * 5. Select top N sentences by score
 * 6. Return in original order
 */

#ifndef TFIDF_SUMMARIZER_H
#define TFIDF_SUMMARIZER_H

#include <stddef.h>

/* Maximum number of sentences that can be processed */
#define TFIDF_MAX_SENTENCES 500

/* Default ratio of sentences to keep (20%) */
#define TFIDF_DEFAULT_RATIO 0.2f

/* Minimum number of sentences to always keep in summary */
#define TFIDF_MIN_SENTENCES 3

/* Return codes */
#define TFIDF_SUCCESS 0
#define TFIDF_ERROR_NULL_INPUT 1
#define TFIDF_ERROR_ALLOC 2
#define TFIDF_ERROR_NO_SENTENCES 3
#define TFIDF_ERROR_TOO_MANY_SENTENCES 4

/**
 * @brief Summarize text using TF-IDF extractive summarization
 *
 * Extracts the most important sentences from the input text based on
 * TF-IDF scoring. The summary maintains the original sentence order.
 *
 * @param input_text The text to summarize (null-terminated)
 * @param output_summary Pointer to receive allocated summary string (caller frees)
 * @param keep_ratio Ratio of sentences to keep (0.0-1.0, e.g., 0.2 = 20%)
 * @param min_sentences Minimum number of sentences to keep regardless of ratio
 * @return TFIDF_SUCCESS on success, error code otherwise
 */
int tfidf_summarize(const char *input_text,
                    char **output_summary,
                    float keep_ratio,
                    size_t min_sentences);

/**
 * @brief Get error message for TF-IDF error code
 *
 * @param error_code Error code from tfidf_summarize()
 * @return Human-readable error message
 */
const char *tfidf_error_string(int error_code);

#endif /* TFIDF_SUMMARIZER_H */
