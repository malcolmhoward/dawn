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
 * DAWN Configuration Validation - Config value validation interface
 */

#ifndef CONFIG_VALIDATE_H
#define CONFIG_VALIDATE_H

#include <stddef.h>

#include "config/dawn_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Configuration error information
 */
typedef struct {
   char field[64];    /* Field name that failed validation */
   char message[256]; /* Error description */
} config_error_t;

/**
 * @brief Validate configuration values
 *
 * Checks:
 * - Range validation (thresholds 0.0-1.0, ports 1-65535, etc.)
 * - Enum validation (processing_mode, llm.type)
 * - Dependency validation (cloud LLM requires API key)
 *
 * @param config Configuration to validate
 * @param secrets Secrets to validate dependencies (can be NULL)
 * @param errors Array to receive error details
 * @param max_errors Maximum errors to report
 * @return Number of errors found (0 = valid)
 */
int config_validate(const dawn_config_t *config,
                    const secrets_config_t *secrets,
                    config_error_t *errors,
                    size_t max_errors);

/**
 * @brief Print validation errors to stderr
 *
 * Convenience function to display validation errors.
 *
 * @param errors Array of errors
 * @param count Number of errors
 */
void config_print_errors(const config_error_t *errors, int count);

#ifdef __cplusplus
}
#endif

#endif /* CONFIG_VALIDATE_H */
