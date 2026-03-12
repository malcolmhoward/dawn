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
 * Stub symbols for test_embedding_engine. Provides globals and provider
 * symbols so embedding_engine.c links without the full daemon.
 */

#include "config/dawn_config.h"
#include "core/embedding_engine.h"

/* Global config stubs */
dawn_config_t g_config;
secrets_config_t g_secrets;

/* Provider stubs — test never calls init/embed */
const embedding_provider_t embedding_provider_onnx = { .name = "onnx" };
const embedding_provider_t embedding_provider_ollama = { .name = "ollama" };
const embedding_provider_t embedding_provider_openai = { .name = "openai" };
