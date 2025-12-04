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
 * @file input_queue.h
 * @brief Unified input queue for multiple command sources.
 *
 * Provides a thread-safe queue that aggregates text commands from multiple
 * input sources (TUI, voice, MQTT, REST, WebSocket, etc.) into a single
 * consumption point for the main processing loop.
 *
 * @section rationale Design Rationale
 * AI conversations are inherently linear - users send input, wait for a
 * response, then send the next input. In practice, the queue will almost
 * always contain 0 or 1 items. The primary value of this abstraction is:
 *
 * 1. **Single polling point**: The main loop checks one place for all input
 *    sources rather than polling each source individually.
 *
 * 2. **Race condition safety**: If two inputs arrive simultaneously (e.g.,
 *    voice detection completes at the exact moment user presses Enter in TUI),
 *    both are captured rather than one being lost.
 *
 * The queue mechanism is defensive programming for a rare edge case, not a
 * throughput optimization. High queue depth (3+) would indicate the main loop
 * is stuck, not normal operation.
 *
 * @section overflow Overflow Policy
 * When the queue reaches capacity (INPUT_QUEUE_MAX_ITEMS), the oldest item
 * is dropped to make room for the new item (FIFO eviction). A warning is
 * logged when this occurs. Text exceeding INPUT_QUEUE_MAX_TEXT is truncated.
 *
 * @section threading Thread Safety
 * All public functions are thread-safe and use an internal mutex. Multiple
 * producer threads can safely push items concurrently. The queue is designed
 * for polling (non-blocking) consumption from a single consumer thread.
 */

#ifndef INPUT_QUEUE_H
#define INPUT_QUEUE_H

/** Maximum length of input text (matches TUI_INPUT_MAX_LEN) */
#define INPUT_QUEUE_MAX_TEXT 512

/** Maximum number of queued items before oldest are dropped */
#define INPUT_QUEUE_MAX_ITEMS 8

/**
 * @brief Input source identifiers.
 *
 * Used to track where each queued command originated from,
 * enabling source-specific logging and potential prioritization.
 */
typedef enum {
   INPUT_SOURCE_VOICE,     /**< Voice command via microphone */
   INPUT_SOURCE_TUI,       /**< Text typed in TUI input mode */
   INPUT_SOURCE_MQTT,      /**< Command received via MQTT */
   INPUT_SOURCE_NETWORK,   /**< Command from network client (DAP) */
   INPUT_SOURCE_REST,      /**< Future: REST API endpoint */
   INPUT_SOURCE_WEBSOCKET, /**< Future: WebSocket connection */
   INPUT_SOURCE_COUNT      /**< Number of input sources */
} input_source_t;

/**
 * @brief Queued input item.
 *
 * Contains the command text and its source for processing.
 */
typedef struct {
   input_source_t source;               /**< Where the input came from */
   char text[INPUT_QUEUE_MAX_TEXT + 1]; /**< Null-terminated command text */
} queued_input_t;

/**
 * @brief Check if the input queue has items.
 *
 * @return 1 if there are items waiting, 0 if empty.
 * @note Thread-safe: Uses internal mutex.
 */
int input_queue_has_item(void);

/**
 * @brief Get current number of items in queue.
 *
 * Useful for monitoring queue depth and detecting potential overflow conditions.
 *
 * @return Number of queued items (0 to INPUT_QUEUE_MAX_ITEMS).
 * @note Thread-safe: Uses internal mutex.
 */
int input_queue_get_count(void);

/**
 * @brief Pop the next item from the queue.
 *
 * Retrieves and removes the oldest queued item (FIFO order).
 *
 * @param out Pointer to structure to receive the item.
 * @return 1 if an item was retrieved, 0 if queue was empty.
 * @note Thread-safe: Uses internal mutex.
 */
int input_queue_pop(queued_input_t *out);

/**
 * @brief Push a new item onto the queue.
 *
 * Adds a command from the specified source to the queue.
 * If the queue is full, the oldest item is dropped (see @ref overflow).
 *
 * @param source The input source identifier (must be valid).
 * @param text The command text (will be truncated if too long).
 * @return 1 on success, 0 if text was NULL or source was invalid.
 * @note Thread-safe: Uses internal mutex.
 */
int input_queue_push(input_source_t source, const char *text);

/**
 * @brief Get human-readable name for an input source.
 *
 * @param source The input source identifier.
 * @return Static string with source name (e.g., "TUI", "voice").
 */
const char *input_source_name(input_source_t source);

/**
 * @brief Clear all items from the queue.
 *
 * @note Thread-safe: Uses internal mutex.
 */
void input_queue_clear(void);

#endif /* INPUT_QUEUE_H */
