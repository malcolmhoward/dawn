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
 * Acoustic Echo Cancellation (AEC) Processor for DAWN
 *
 * This module wraps WebRTC's AEC3 algorithm to remove speaker echo
 * from microphone input, enabling barge-in during TTS playback.
 *
 * Thread Safety:
 * - aec_add_reference(): Safe to call from TTS thread (lock-free write to ring buffer)
 * - aec_process(): Safe to call from capture thread (per-frame locking)
 * - aec_init/cleanup(): Call from main thread only during startup/shutdown
 *
 * Real-Time Constraints:
 * - aec_process() uses per-frame locking (~160 samples = 10ms)
 * - No dynamic allocation in processing path
 * - Graceful degradation on errors (pass-through mode)
 *
 * Reference Buffer Behavior:
 * - Uses DAWN's ring_buffer_t which drops oldest data on overflow
 * - This is expected when TTS produces data faster than AEC consumes
 * - Does not affect echo cancellation quality (AEC only needs recent history)
 */

#ifndef AEC_PROCESSOR_H
#define AEC_PROCESSOR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Compile-time configuration validation
 */
#define AEC_SAMPLE_RATE 16000

/* Verify AEC sample rate matches capture rate at compile time */
#ifdef ENABLE_AEC
/* This will be checked in audio_capture_thread.c */
#endif

/**
 * @brief AEC processing constants
 *
 * WebRTC AEC3 processes in 10ms frames at 16kHz = 160 samples.
 * These values are fixed by the WebRTC API.
 */
#define AEC_FRAME_SAMPLES 160
#define AEC_FRAME_BYTES (AEC_FRAME_SAMPLES * sizeof(int16_t))

/**
 * @brief Maximum samples that can be processed in one call
 *
 * Limits memory allocation and prevents excessive lock hold times.
 * 8192 samples = 512ms at 16kHz, more than enough for any capture chunk.
 */
#define AEC_MAX_SAMPLES 8192

/**
 * @brief Consecutive error threshold before AEC disables itself
 */
#define AEC_MAX_CONSECUTIVE_ERRORS 10

/**
 * @brief Minimum reference buffer size in milliseconds
 *
 * Must accommodate: acoustic delay + system buffering + margin
 * Typical values: 100-200ms minimum, 500ms recommended
 */
#define AEC_MIN_REF_BUFFER_MS 100

/* Compile-time sanity checks */
#if AEC_MAX_SAMPLES < AEC_FRAME_SAMPLES
#error "AEC_MAX_SAMPLES must be >= AEC_FRAME_SAMPLES"
#endif

/**
 * @brief Noise suppression level enumeration
 */
typedef enum {
   AEC_NS_LEVEL_LOW = 0,      /**< Minimal noise suppression */
   AEC_NS_LEVEL_MODERATE = 1, /**< Balanced (default) */
   AEC_NS_LEVEL_HIGH = 2      /**< Aggressive noise suppression */
} aec_ns_level_t;

/**
 * @brief AEC runtime statistics for monitoring and debugging
 */
typedef struct {
   int estimated_delay_ms;         /**< Estimated acoustic delay */
   size_t ref_buffer_samples;      /**< Samples in reference buffer */
   int consecutive_errors;         /**< Error count (resets on success) */
   bool is_active;                 /**< True if AEC is processing */
   float avg_processing_time_us;   /**< Average processing time per frame */
   uint64_t frames_processed;      /**< Total frames successfully processed */
   uint64_t frames_passed_through; /**< Frames passed without AEC (no ref data) */
} aec_stats_t;

/**
 * @brief AEC configuration options
 */
typedef struct {
   bool enable_noise_suppression;          /**< Enable NS (adds CPU load) */
   aec_ns_level_t noise_suppression_level; /**< NS aggressiveness */
   bool enable_high_pass_filter;           /**< Remove DC offset */
   bool mobile_mode;                       /**< Use AECM instead of AEC3 (lower CPU) */
   size_t ref_buffer_ms;                   /**< Reference buffer size in ms (default: 500) */
   int16_t noise_gate_threshold; /**< Post-AEC noise gate threshold (0=disabled, default: 0) */
} aec_config_t;

/**
 * @brief Get default AEC configuration
 *
 * Returns configuration with sensible defaults:
 * - Noise suppression: enabled, moderate level
 * - High-pass filter: enabled
 * - Mobile mode: disabled (full AEC3)
 * - Reference buffer: 500ms
 *
 * @return Configuration with sensible defaults for Jetson/RPi
 */
aec_config_t aec_get_default_config(void);

/**
 * @brief Initialize AEC processor with configuration
 *
 * Creates WebRTC AudioProcessing instance with AEC3 enabled.
 * Pre-allocates all buffers to avoid runtime allocation.
 *
 * Call AFTER TTS initialization (TTS creates the resampler).
 *
 * @param config Configuration options (NULL for defaults)
 * @return 0 on success, non-zero on error
 */
int aec_init(const aec_config_t *config);

/**
 * @brief Cleanup AEC processor
 *
 * Releases all AEC resources. Safe to call multiple times.
 * Blocks until any in-progress processing completes.
 *
 * Call BEFORE audio capture stops.
 */
void aec_cleanup(void);

/**
 * @brief Check if AEC is initialized and active
 *
 * Returns false if AEC failed initialization, hit error threshold,
 * or was never initialized.
 *
 * @return true if AEC is processing audio, false otherwise
 */
bool aec_is_enabled(void);

/**
 * @brief Add reference (far-end) audio from TTS playback
 *
 * Call this with TTS audio BEFORE it goes to the speaker.
 * Audio must be 16kHz mono S16_LE format (resample if necessary).
 *
 * This function is lock-free and safe to call from TTS thread.
 * Internally uses the thread-safe ring_buffer_write().
 *
 * @param samples Audio samples (16-bit signed)
 * @param num_samples Number of samples
 */
void aec_add_reference(const int16_t *samples, size_t num_samples);

/**
 * @brief Process microphone audio to remove echo
 *
 * Takes raw microphone input and outputs echo-cancelled audio.
 * Audio must be 16kHz mono S16_LE format.
 *
 * Uses per-frame locking (10ms granularity) to minimize impact
 * on real-time audio thread. On error, passes through unprocessed
 * audio to maintain audio continuity.
 *
 * If mic_in or clean_out is NULL, the output buffer is zeroed
 * to prevent undefined behavior in the caller.
 *
 * @param mic_in Input microphone samples (NULL safe - outputs silence)
 * @param clean_out Output buffer for echo-cancelled samples (same size as input)
 * @param num_samples Number of samples (must be <= AEC_MAX_SAMPLES)
 */
void aec_process(const int16_t *mic_in, int16_t *clean_out, size_t num_samples);

/**
 * @brief Get AEC runtime statistics
 *
 * @param stats Output structure for statistics
 * @return 0 on success, non-zero if AEC not initialized
 */
int aec_get_stats(aec_stats_t *stats);

/**
 * @brief Reset AEC state (clear buffers and error counters)
 *
 * Call this if audio routing changes or after long silence periods.
 * Re-enables AEC if it was disabled due to consecutive errors.
 *
 * Note: WebRTC AEC3 state reset support varies by version.
 * This function always clears the reference buffer and error counters.
 */
void aec_reset(void);

#ifdef __cplusplus
}
#endif

#endif  // AEC_PROCESSOR_H
