# WebRTC AEC3 Integration Guide for DAWN

This document describes how to integrate WebRTC's Acoustic Echo Cancellation (AEC3) into DAWN to enable seamless barge-in (interrupt) capability without requiring special hardware like ReSpeaker arrays.

## Overview

### The Problem
When DAWN speaks through speakers, the microphone picks up its own voice. Without echo cancellation:
- VAD triggers on DAWN's own speech
- Wake word detection may false-trigger
- User speech is mixed with DAWN's output

### The Solution
AEC uses a reference signal (what DAWN is playing) to subtract the echo from the microphone input, leaving only the user's voice.

### Architecture Diagram

```
┌─────────────────────────────────────────────────────────────────┐
│                    Current Architecture                          │
├─────────────────────────────────────────────────────────────────┤
│                                                                  │
│  [Mic] → capture_thread → ring_buffer → [main loop reads]       │
│                                                                  │
│  [TTS queue] → tts_thread → audioBuffer → [speaker]             │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────┐
│                    With AEC Integration                          │
├─────────────────────────────────────────────────────────────────┤
│                                                                  │
│  [Mic] → capture_thread → mic_ring_buffer ──┐                   │
│                                              │                   │
│                                              ▼                   │
│                                         ┌─────────┐              │
│                                         │  AEC3   │              │
│                                         │ Process │              │
│                                         └────┬────┘              │
│                                              │                   │
│  [TTS] → tts_thread ──┬─→ [speaker]          │                   │
│                       │                      │                   │
│                       ▼                      ▼                   │
│              ref_ring_buffer ───────→ clean_ring_buffer          │
│              (resampled 16kHz)        (echo-cancelled)           │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

### Sample Rate Architecture

DAWN operates with multiple sample rates that must be properly managed:

```
┌────────────────────────────────────────────────────────────────┐
│                    Sample Rate Flow                             │
├────────────────────────────────────────────────────────────────┤
│                                                                 │
│  Microphone Capture: 16kHz ──────────────────→ ASR (Whisper)   │
│                         │                                       │
│                         ▼                                       │
│                    AEC Process (16kHz) ──────→ VAD/Wake Word   │
│                         ▲                                       │
│                         │                                       │
│  TTS Playback: 22050Hz ─┴─→ Resample to 16kHz (reference)      │
│                                                                 │
└────────────────────────────────────────────────────────────────┘
```

### Memory Footprint

Expected additional memory usage when AEC is enabled:

| Component | Size | Notes |
|-----------|------|-------|
| WebRTC AudioProcessing | ~100KB | Internal AEC3 state |
| Reference ring buffer | ~16KB | 500ms at 16kHz (configurable) |
| Resampler buffers | ~64KB | Pre-allocated float conversion |
| Capture AEC buffer | ~16KB | Matches capture buffer size |
| **Total** | **~200KB** | Acceptable for Jetson (4GB+ RAM) |

**Note for RPi users**: On systems with limited RAM, consider reducing `ref_buffer_ms` to 300ms.

## Prerequisites

### Dependencies
- WebRTC audio processing module >= 1.0.0 (freedesktop.org variant)
- libsamplerate (for high-quality resampling)
- Existing DAWN audio infrastructure

### Hardware Requirements
- Jetson: ~5-15% additional CPU overhead
- RPi 4: ~10-20% additional CPU overhead (consider mobile mode)

### WebRTC Version Compatibility

This guide targets the **freedesktop.org webrtc-audio-processing** package (version 1.x), which provides a standalone build of WebRTC's audio processing module. This is NOT the full WebRTC library from Google.

```bash
# Check installed version
pkg-config --modversion webrtc-audio-processing-1
```

## Implementation Steps

### Phase 1: Install Dependencies

```bash
# Resampler library
sudo apt-get install libsamplerate0-dev

# WebRTC audio processing (check if available)
sudo apt-get install libwebrtc-audio-processing-dev

# Or build from source (if not in repos):
git clone https://gitlab.freedesktop.org/pulseaudio/webrtc-audio-processing.git
cd webrtc-audio-processing
meson build
ninja -C build
sudo ninja -C build install
```

### Phase 2: Create Header Files

#### File: `include/audio/aec_processor.h`
```c
/*
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
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
#define AEC_SAMPLE_RATE   16000

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
#define AEC_FRAME_BYTES   (AEC_FRAME_SAMPLES * sizeof(int16_t))

/**
 * @brief Maximum samples that can be processed in one call
 *
 * Limits memory allocation and prevents excessive lock hold times.
 * 8192 samples = 512ms at 16kHz, more than enough for any capture chunk.
 */
#define AEC_MAX_SAMPLES   8192

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
    AEC_NS_LEVEL_LOW = 0,       /**< Minimal noise suppression */
    AEC_NS_LEVEL_MODERATE = 1,  /**< Balanced (default) */
    AEC_NS_LEVEL_HIGH = 2       /**< Aggressive noise suppression */
} aec_ns_level_t;

/**
 * @brief AEC runtime statistics for monitoring and debugging
 */
typedef struct {
    int estimated_delay_ms;       /**< Estimated acoustic delay */
    size_t ref_buffer_samples;    /**< Samples in reference buffer */
    int consecutive_errors;       /**< Error count (resets on success) */
    bool is_active;               /**< True if AEC is processing */
    float avg_processing_time_us; /**< Average processing time per frame */
    uint64_t frames_processed;    /**< Total frames successfully processed */
    uint64_t frames_passed_through; /**< Frames passed without AEC (no ref data) */
} aec_stats_t;

/**
 * @brief AEC configuration options
 */
typedef struct {
    bool enable_noise_suppression;  /**< Enable NS (adds CPU load) */
    aec_ns_level_t noise_suppression_level; /**< NS aggressiveness */
    bool enable_high_pass_filter;   /**< Remove DC offset */
    bool mobile_mode;               /**< Use AECM instead of AEC3 (lower CPU) */
    size_t ref_buffer_ms;           /**< Reference buffer size in ms (default: 500) */
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

#endif // AEC_PROCESSOR_H
```

#### File: `include/audio/resampler.h`
```c
/*
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Audio Resampler for DAWN
 *
 * Provides high-quality sample rate conversion using libsamplerate.
 * Used primarily to convert TTS output (22050Hz) to AEC reference (16kHz).
 *
 * Design Constraints:
 * - Pre-allocated buffers (no malloc in processing path)
 * - Fixed maximum chunk size to bound memory usage
 * - Thread-safe per-instance (each thread should have its own resampler)
 */

#ifndef RESAMPLER_H
#define RESAMPLER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Maximum samples that can be processed in one call
 *
 * Pre-allocated buffer size. TTS typically sends 1024-sample chunks,
 * so 8192 provides generous headroom.
 */
#define RESAMPLER_MAX_SAMPLES 8192

/**
 * @brief Opaque resampler handle
 */
typedef struct resampler_t resampler_t;

/**
 * @brief Create a resampler instance with pre-allocated buffers
 *
 * Allocates all memory upfront. Processing calls will not allocate.
 *
 * @param src_rate Source sample rate (e.g., 22050)
 * @param dst_rate Destination sample rate (e.g., 16000)
 * @param channels Number of channels (1 for mono)
 * @return Resampler handle, or NULL on error
 */
resampler_t *resampler_create(int src_rate, int dst_rate, int channels);

/**
 * @brief Destroy resampler instance and free all resources
 *
 * @param rs Resampler handle (NULL safe)
 */
void resampler_destroy(resampler_t *rs);

/**
 * @brief Resample audio data (no allocation)
 *
 * Input must not exceed RESAMPLER_MAX_SAMPLES.
 * Output buffer must be large enough for resampled data
 * (use resampler_get_output_size() to calculate).
 *
 * @param rs Resampler handle
 * @param in Input samples (16-bit signed)
 * @param in_samples Number of input samples (must be <= RESAMPLER_MAX_SAMPLES)
 * @param out Output buffer
 * @param out_samples_max Maximum output samples (buffer size)
 * @return Number of output samples produced, 0 on error
 */
size_t resampler_process(resampler_t *rs,
                         const int16_t *in, size_t in_samples,
                         int16_t *out, size_t out_samples_max);

/**
 * @brief Calculate required output buffer size for given input
 *
 * @param rs Resampler handle
 * @param in_samples Number of input samples
 * @return Required output buffer size in samples
 */
size_t resampler_get_output_size(resampler_t *rs, size_t in_samples);

/**
 * @brief Reset resampler state (clear internal buffers)
 *
 * Call this when audio stream is discontinuous.
 *
 * @param rs Resampler handle
 */
void resampler_reset(resampler_t *rs);

#ifdef __cplusplus
}
#endif

#endif // RESAMPLER_H
```

### Phase 3: Create Implementation Files

#### File: `src/audio/resampler.c`
```c
/*
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Audio Resampler Implementation
 *
 * Uses libsamplerate (Secret Rabbit Code) for high-quality resampling.
 * All buffers are pre-allocated at creation time.
 */

#include "audio/resampler.h"

#include <stdlib.h>
#include <string.h>
#include <samplerate.h>

#include "logging.h"

struct resampler_t {
    SRC_STATE *src_state;
    double ratio;
    int channels;

    // Pre-allocated buffers (sized for RESAMPLER_MAX_SAMPLES)
    float *in_float;
    float *out_float;
    size_t buffer_capacity;
};

resampler_t *resampler_create(int src_rate, int dst_rate, int channels) {
    if (src_rate <= 0 || dst_rate <= 0 || channels <= 0) {
        LOG_ERROR("Invalid resampler parameters: src=%d dst=%d ch=%d",
                  src_rate, dst_rate, channels);
        return NULL;
    }

    resampler_t *rs = calloc(1, sizeof(resampler_t));
    if (!rs) {
        LOG_ERROR("Failed to allocate resampler");
        return NULL;
    }

    rs->ratio = (double)dst_rate / (double)src_rate;
    rs->channels = channels;

    int error;
    // SRC_SINC_FASTEST provides good quality with low CPU
    // Use SRC_SINC_MEDIUM_QUALITY if artifacts are audible
    rs->src_state = src_new(SRC_SINC_FASTEST, channels, &error);
    if (!rs->src_state) {
        LOG_ERROR("Failed to create SRC state: %s", src_strerror(error));
        free(rs);
        return NULL;
    }

    // Pre-allocate for maximum expected size
    // Output can be larger than input if upsampling, so size for worst case
    size_t max_out = (size_t)(RESAMPLER_MAX_SAMPLES * rs->ratio) + 64;
    rs->buffer_capacity = (max_out > RESAMPLER_MAX_SAMPLES) ? max_out : RESAMPLER_MAX_SAMPLES;

    rs->in_float = malloc(rs->buffer_capacity * sizeof(float));
    rs->out_float = malloc(rs->buffer_capacity * sizeof(float));

    if (!rs->in_float || !rs->out_float) {
        LOG_ERROR("Failed to allocate resampler buffers (%zu samples)", rs->buffer_capacity);
        src_delete(rs->src_state);
        free(rs->in_float);
        free(rs->out_float);
        free(rs);
        return NULL;
    }

    LOG_INFO("Resampler created: %d -> %d Hz (ratio %.4f, buffer %zu samples)",
             src_rate, dst_rate, rs->ratio, rs->buffer_capacity);
    return rs;
}

void resampler_destroy(resampler_t *rs) {
    if (!rs) return;

    if (rs->src_state) {
        src_delete(rs->src_state);
    }
    free(rs->in_float);
    free(rs->out_float);
    free(rs);
}

size_t resampler_process(resampler_t *rs,
                         const int16_t *in, size_t in_samples,
                         int16_t *out, size_t out_samples_max) {
    if (!rs || !in || !out || in_samples == 0) {
        return 0;
    }

    // Enforce maximum to prevent buffer overflow
    if (in_samples > RESAMPLER_MAX_SAMPLES) {
        LOG_ERROR("Resampler input too large: %zu > %d", in_samples, RESAMPLER_MAX_SAMPLES);
        return 0;
    }

    // Verify output buffer is sufficient
    size_t required_out = resampler_get_output_size(rs, in_samples);
    if (out_samples_max < required_out) {
        LOG_ERROR("Resampler output buffer too small: %zu < %zu", out_samples_max, required_out);
        return 0;
    }

    // Convert int16 to float (libsamplerate works with float)
    src_short_to_float_array(in, rs->in_float, (int)in_samples);

    // Perform resampling
    SRC_DATA src_data = {
        .data_in = rs->in_float,
        .data_out = rs->out_float,
        .input_frames = (long)(in_samples / rs->channels),
        .output_frames = (long)(out_samples_max / rs->channels),
        .src_ratio = rs->ratio,
        .end_of_input = 0
    };

    int error = src_process(rs->src_state, &src_data);
    if (error) {
        LOG_ERROR("Resampler error: %s", src_strerror(error));
        return 0;
    }

    size_t out_samples = (size_t)src_data.output_frames_gen * rs->channels;

    // Convert float back to int16
    src_float_to_short_array(rs->out_float, out, (int)out_samples);

    return out_samples;
}

size_t resampler_get_output_size(resampler_t *rs, size_t in_samples) {
    if (!rs) return 0;
    // Add margin for rounding and filter delay
    return (size_t)(in_samples * rs->ratio) + 32;
}

void resampler_reset(resampler_t *rs) {
    if (!rs || !rs->src_state) return;
    src_reset(rs->src_state);
}
```

#### File: `src/audio/aec_processor.cpp`
```cpp
/*
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * WebRTC AEC3 Processor Implementation
 *
 * Key Design Decisions:
 *
 * 1. Per-Frame Locking: Instead of locking for the entire aec_process() call,
 *    we lock only during WebRTC API calls (~10ms frames). This prevents
 *    blocking the real-time audio capture thread for extended periods.
 *
 * 2. Lock-Free Reference Path: aec_add_reference() uses ring_buffer_write()
 *    which is internally mutex-protected but non-blocking. The TTS thread
 *    can always write without waiting.
 *
 * 3. Graceful Degradation: On errors, AEC passes through unprocessed audio
 *    and tracks consecutive errors. After AEC_MAX_CONSECUTIVE_ERRORS,
 *    AEC disables itself to prevent log spam and wasted CPU.
 *
 * 4. Pre-allocated Buffers: All frame buffers are allocated at init time.
 *    No malloc/realloc in the processing path.
 *
 * 5. Reference Buffer Sizing: Default 500ms buffer accommodates typical
 *    acoustic delays (speaker to mic) plus system buffering delays.
 */

#include "audio/aec_processor.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <memory>

// WebRTC audio processing includes
#include <webrtc/modules/audio_processing/include/audio_processing.h>

extern "C" {
#include "audio/ring_buffer.h"
#include "logging.h"
}

namespace {

// AEC state
std::unique_ptr<webrtc::AudioProcessing> g_apm;
std::mutex g_aec_mutex;  // Protects WebRTC API calls only
std::atomic<bool> g_initialized{false};
std::atomic<bool> g_active{true};  // Can be disabled on repeated errors

// Reference signal buffer (TTS output, resampled to 16kHz)
ring_buffer_t *g_ref_buffer = nullptr;

// Pre-allocated frame buffers (sized for single 10ms frame)
int16_t g_ref_frame[AEC_FRAME_SAMPLES];
int16_t g_mic_frame[AEC_FRAME_SAMPLES];

// Error tracking
std::atomic<int> g_consecutive_errors{0};

// Performance tracking
std::atomic<float> g_avg_processing_time_us{0.0f};
std::atomic<int> g_frame_count{0};
std::atomic<uint64_t> g_frames_processed{0};
std::atomic<uint64_t> g_frames_passed_through{0};

// Configuration (set at init)
aec_config_t g_config;

} // anonymous namespace

extern "C" {

aec_config_t aec_get_default_config(void) {
    aec_config_t config = {
        .enable_noise_suppression = true,
        .noise_suppression_level = AEC_NS_LEVEL_MODERATE,
        .enable_high_pass_filter = true,
        .mobile_mode = false,
        .ref_buffer_ms = 500
    };
    return config;
}

int aec_init(const aec_config_t *config) {
    // Use defaults if no config provided
    if (config) {
        g_config = *config;
    } else {
        g_config = aec_get_default_config();
    }

    // Validate configuration
    if (g_config.ref_buffer_ms < AEC_MIN_REF_BUFFER_MS) {
        LOG_WARNING("AEC ref_buffer_ms (%zu) below minimum (%d), using minimum",
                    g_config.ref_buffer_ms, AEC_MIN_REF_BUFFER_MS);
        g_config.ref_buffer_ms = AEC_MIN_REF_BUFFER_MS;
    }

    std::lock_guard<std::mutex> lock(g_aec_mutex);

    if (g_initialized.load()) {
        LOG_WARNING("AEC already initialized");
        return 0;
    }

    // Create AudioProcessing instance
    webrtc::AudioProcessingBuilder builder;
    g_apm = builder.Create();

    if (!g_apm) {
        LOG_ERROR("Failed to create AudioProcessing instance");
        return 1;
    }

    // Configure AEC3
    webrtc::AudioProcessing::Config apm_config;
    apm_config.echo_canceller.enabled = true;
    apm_config.echo_canceller.mobile_mode = g_config.mobile_mode;

    // Noise suppression (optional, adds CPU load)
    apm_config.noise_suppression.enabled = g_config.enable_noise_suppression;
    if (g_config.enable_noise_suppression) {
        switch (g_config.noise_suppression_level) {
            case AEC_NS_LEVEL_LOW:
                apm_config.noise_suppression.level =
                    webrtc::AudioProcessing::Config::NoiseSuppression::kLow;
                break;
            case AEC_NS_LEVEL_HIGH:
                apm_config.noise_suppression.level =
                    webrtc::AudioProcessing::Config::NoiseSuppression::kHigh;
                break;
            case AEC_NS_LEVEL_MODERATE:
            default:
                apm_config.noise_suppression.level =
                    webrtc::AudioProcessing::Config::NoiseSuppression::kModerate;
                break;
        }
    }

    // Disable AGC (DAWN handles gain elsewhere)
    apm_config.gain_controller1.enabled = false;
    apm_config.gain_controller2.enabled = false;

    // High-pass filter removes DC offset
    apm_config.high_pass_filter.enabled = g_config.enable_high_pass_filter;

    g_apm->ApplyConfig(apm_config);

    // Calculate reference buffer size
    size_t ref_buffer_samples = (AEC_SAMPLE_RATE * g_config.ref_buffer_ms) / 1000;
    size_t ref_buffer_bytes = ref_buffer_samples * sizeof(int16_t);

    g_ref_buffer = ring_buffer_create(ref_buffer_bytes);
    if (!g_ref_buffer) {
        LOG_ERROR("Failed to create AEC reference buffer (%zu bytes)", ref_buffer_bytes);
        g_apm.reset();
        return 1;
    }

    // Reset state
    g_consecutive_errors.store(0);
    g_avg_processing_time_us.store(0.0f);
    g_frame_count.store(0);
    g_frames_processed.store(0);
    g_frames_passed_through.store(0);
    g_active.store(true);
    g_initialized.store(true);

    LOG_INFO("AEC3 initialized: %d Hz, %d samples/frame, %zu ms ref buffer, mobile=%d, NS=%d",
             AEC_SAMPLE_RATE, AEC_FRAME_SAMPLES, g_config.ref_buffer_ms,
             g_config.mobile_mode, g_config.enable_noise_suppression);

    return 0;
}

void aec_cleanup(void) {
    // Mark as not initialized first to stop processing
    g_initialized.store(false);

    // Wait for any in-progress processing to complete
    std::lock_guard<std::mutex> lock(g_aec_mutex);

    g_apm.reset();

    if (g_ref_buffer) {
        ring_buffer_free(g_ref_buffer);
        g_ref_buffer = nullptr;
    }

    LOG_INFO("AEC cleaned up (processed: %llu frames, passed through: %llu frames)",
             (unsigned long long)g_frames_processed.load(),
             (unsigned long long)g_frames_passed_through.load());
}

bool aec_is_enabled(void) {
    return g_initialized.load() && g_active.load();
}

void aec_add_reference(const int16_t *samples, size_t num_samples) {
    // Quick checks without locking
    if (!g_initialized.load() || !g_active.load()) {
        return;
    }
    if (!samples || num_samples == 0) {
        return;
    }

    // ring_buffer_write is internally thread-safe, no additional locking needed
    // Note: If buffer overflows, oldest data is dropped (expected behavior)
    ring_buffer_write(g_ref_buffer, (const char *)samples, num_samples * sizeof(int16_t));
}

void aec_process(const int16_t *mic_in, int16_t *clean_out, size_t num_samples) {
    // Handle NULL output buffer - zero it to prevent undefined behavior
    if (!clean_out) {
        return;
    }

    // Handle NULL input or invalid size - output silence
    if (!mic_in || num_samples == 0) {
        if (num_samples > 0 && num_samples <= AEC_MAX_SAMPLES) {
            memset(clean_out, 0, num_samples * sizeof(int16_t));
        }
        return;
    }

    // Validate sample count
    if (num_samples > AEC_MAX_SAMPLES) {
        LOG_ERROR("AEC input too large: %zu > %d", num_samples, AEC_MAX_SAMPLES);
        // Output silence for safety
        memset(clean_out, 0, AEC_MAX_SAMPLES * sizeof(int16_t));
        return;
    }

    // Check if AEC is available
    if (!g_initialized.load() || !g_active.load()) {
        // Pass through if AEC not available
        memcpy(clean_out, mic_in, num_samples * sizeof(int16_t));
        return;
    }

    // Process in AEC_FRAME_SAMPLES chunks (10ms frames)
    // Lock is acquired per-frame to minimize blocking
    size_t processed = 0;
    webrtc::StreamConfig stream_config(AEC_SAMPLE_RATE, 1);  // 16kHz, 1 channel

    while (processed < num_samples) {
        size_t chunk = num_samples - processed;
        if (chunk > AEC_FRAME_SAMPLES) {
            chunk = AEC_FRAME_SAMPLES;
        }

        auto frame_start = std::chrono::high_resolution_clock::now();

        // Copy mic input to frame buffer
        memcpy(g_mic_frame, mic_in + processed, chunk * sizeof(int16_t));

        // Pad with zeros if partial frame (last chunk may be smaller)
        if (chunk < AEC_FRAME_SAMPLES) {
            memset(g_mic_frame + chunk, 0, (AEC_FRAME_SAMPLES - chunk) * sizeof(int16_t));
        }

        // Get reference audio (lock-free read from ring buffer)
        size_t ref_available = ring_buffer_bytes_available(g_ref_buffer);
        bool has_reference = (ref_available >= AEC_FRAME_BYTES);

        if (has_reference) {
            ring_buffer_read(g_ref_buffer, (char *)g_ref_frame, AEC_FRAME_BYTES);
        } else {
            // No reference available - assume silence (no TTS playing)
            memset(g_ref_frame, 0, AEC_FRAME_BYTES);
            g_frames_passed_through.fetch_add(1);
        }

        // Lock only for WebRTC API calls (brief, ~1ms typically)
        bool frame_success = false;
        {
            std::lock_guard<std::mutex> lock(g_aec_mutex);

            if (!g_apm) {
                // AEC was cleaned up while we were processing
                memcpy(clean_out + processed, mic_in + processed, chunk * sizeof(int16_t));
                processed += chunk;
                continue;
            }

            // Feed reference signal (render/playback/far-end)
            int16_t *ref_ptr = g_ref_frame;
            int reverse_result = g_apm->ProcessReverseStream(
                &ref_ptr, stream_config, stream_config, &ref_ptr);

            // Process capture stream (microphone/near-end)
            int16_t *mic_ptr = g_mic_frame;
            int stream_result = g_apm->ProcessStream(
                &mic_ptr, stream_config, stream_config, &mic_ptr);

            frame_success = (reverse_result == 0 && stream_result == 0);
        }

        if (frame_success) {
            // Copy processed audio to output
            memcpy(clean_out + processed, g_mic_frame, chunk * sizeof(int16_t));

            // Reset error counter on success
            g_consecutive_errors.store(0);
            g_frames_processed.fetch_add(1);
        } else {
            // On error, pass through unprocessed audio
            memcpy(clean_out + processed, mic_in + processed, chunk * sizeof(int16_t));

            int errors = g_consecutive_errors.fetch_add(1) + 1;
            if (errors == 1 || errors % 100 == 0) {
                LOG_WARNING("AEC ProcessStream failed (consecutive errors: %d)", errors);
            }

            // Disable AEC after too many errors
            if (errors >= AEC_MAX_CONSECUTIVE_ERRORS) {
                LOG_ERROR("AEC disabled after %d consecutive errors - call aec_reset() to re-enable",
                          errors);
                g_active.store(false);
            }
        }

        // Update performance tracking
        auto frame_end = std::chrono::high_resolution_clock::now();
        float frame_us = std::chrono::duration<float, std::micro>(frame_end - frame_start).count();

        g_frame_count.fetch_add(1);
        float avg = g_avg_processing_time_us.load();
        // Exponential moving average
        g_avg_processing_time_us.store(avg * 0.99f + frame_us * 0.01f);

        processed += chunk;
    }
}

int aec_get_stats(aec_stats_t *stats) {
    if (!stats) {
        return 1;
    }

    if (!g_initialized.load()) {
        memset(stats, 0, sizeof(aec_stats_t));
        return 1;
    }

    // Calculate delay estimate from reference buffer level
    size_t ref_bytes = g_ref_buffer ? ring_buffer_bytes_available(g_ref_buffer) : 0;
    size_t ref_samples = ref_bytes / sizeof(int16_t);

    stats->estimated_delay_ms = (int)((ref_samples * 1000) / AEC_SAMPLE_RATE);
    stats->ref_buffer_samples = ref_samples;
    stats->consecutive_errors = g_consecutive_errors.load();
    stats->is_active = g_active.load();
    stats->avg_processing_time_us = g_avg_processing_time_us.load();
    stats->frames_processed = g_frames_processed.load();
    stats->frames_passed_through = g_frames_passed_through.load();

    return 0;
}

void aec_reset(void) {
    if (!g_initialized.load()) {
        return;
    }

    std::lock_guard<std::mutex> lock(g_aec_mutex);

    // Clear reference buffer
    if (g_ref_buffer) {
        ring_buffer_clear(g_ref_buffer);
    }

    // Reset error tracking and re-enable
    g_consecutive_errors.store(0);
    g_active.store(true);

    // Reset statistics
    g_frames_processed.store(0);
    g_frames_passed_through.store(0);
    g_avg_processing_time_us.store(0.0f);
    g_frame_count.store(0);

    // Note: WebRTC AEC3 state reset support varies by version
    // Some versions have Initialize() method, others don't expose reset

    LOG_INFO("AEC state reset - echo cancellation re-enabled");
}

} // extern "C"
```

### Phase 4: Update Audio Capture Thread Header

Modify `include/audio/audio_capture_thread.h` to add AEC buffer fields.

**Add these fields at the END of the `audio_capture_context_t` struct**, just before the closing brace:

```c
typedef struct {
   pthread_t thread;           /**< Capture thread handle */
   ring_buffer_t *ring_buffer; /**< Ring buffer for audio data */
   atomic_bool running;        /**< Thread running flag */
   int use_realtime_priority;  /**< Enable realtime scheduling */

#ifdef ALSA_DEVICE
   snd_pcm_t *handle;        /**< ALSA PCM handle */
   snd_pcm_uframes_t frames; /**< ALSA period size in frames */
#else
   pa_simple *pa_handle; /**< PulseAudio handle */
   size_t pa_framesize;  /**< PulseAudio frame size */
#endif

   char *pcm_device;   /**< Device name */
   size_t buffer_size; /**< Size of capture buffer */

#ifdef ENABLE_AEC
   int16_t *aec_buffer;      /**< Pre-allocated AEC output buffer */
   size_t aec_buffer_size;   /**< AEC buffer size in samples */
   bool aec_rate_mismatch;   /**< True if device rate != AEC_SAMPLE_RATE */
#endif
} audio_capture_context_t;
```

**Also add a compile-time sample rate check** near the top of the file:

```c
/* Verify capture rate matches AEC requirements at compile time */
#ifdef ENABLE_AEC
#include "audio/aec_processor.h"
#if DEFAULT_RATE != AEC_SAMPLE_RATE
#error "AEC requires capture rate to match AEC_SAMPLE_RATE (16000 Hz)"
#endif
#endif
```

### Phase 5: Integrate with TTS Thread

Modify `src/tts/text_to_speech.cpp` to feed reference audio to AEC.

**CRITICAL THREAD SAFETY NOTE**: DAWN has two TTS paths that may run concurrently:
1. **Local TTS Thread**: `tts_thread_function()` - plays audio to speaker
2. **Network TTS**: `text_to_speech_to_wav()` - generates WAV for network clients

The resampler is NOT thread-safe, so each path needs its own resampler instance.

#### Add includes at top (after existing includes):
```cpp
#ifdef ENABLE_AEC
#include "audio/aec_processor.h"
#include "audio/resampler.h"
#include <time.h>
#include <atomic>

// Separate resamplers for thread safety:
// - g_tts_thread_resampler: Used by TTS playback thread to feed AEC reference
// - g_network_resampler: Reserved for future use (network WAV path)
//
// NOTE: Network WAV generation (text_to_speech_to_wav) does NOT need to feed AEC reference
// because network clients (ESP32) play audio on THEIR speaker, not DAWN's local speaker.
// DAWN's microphone won't hear ESP32's output, so there's no echo to cancel.
//
// IMPORTANT: These must NOT be shared between threads - resampler_t is not thread-safe!
static resampler_t *g_tts_thread_resampler = nullptr;
static resampler_t *g_network_resampler = nullptr;

// Pre-allocated resample buffers (one per resampler)
static int16_t g_tts_resample_buffer[RESAMPLER_MAX_SAMPLES];
static int16_t g_network_resample_buffer[RESAMPLER_MAX_SAMPLES];

// Rate-limited warning for resampler overflow (warn once per 60 seconds)
static time_t g_last_resample_warning = 0;

// Atomic sequence counter for detecting discard during unlocked audio write.
// Incremented each time TTS is discarded. Used to detect TOCTOU race condition
// between releasing mutex and completing audio write.
static std::atomic<uint32_t> g_tts_discard_sequence{0};
#endif
```

#### Modify `initialize_text_to_speech()` (add near the end, before final success log):
```cpp
#ifdef ENABLE_AEC
    // Create resamplers for AEC reference (22050 -> 16000)
    // Two separate resamplers for thread safety - one for TTS thread, one for network
    g_tts_thread_resampler = resampler_create(DEFAULT_RATE, AEC_SAMPLE_RATE, 1);
    g_network_resampler = resampler_create(DEFAULT_RATE, AEC_SAMPLE_RATE, 1);

    if (!g_tts_thread_resampler || !g_network_resampler) {
        LOG_WARNING("Failed to create TTS resampler(s) for AEC - echo cancellation may be limited");
        // Clean up partial allocation
        if (g_tts_thread_resampler) {
            resampler_destroy(g_tts_thread_resampler);
            g_tts_thread_resampler = nullptr;
        }
        if (g_network_resampler) {
            resampler_destroy(g_network_resampler);
            g_network_resampler = nullptr;
        }
    } else {
        LOG_INFO("TTS resamplers initialized for AEC (%d -> %d Hz)", DEFAULT_RATE, AEC_SAMPLE_RATE);
    }
#endif
```

#### Modify TTS playback loop:

**CRITICAL**: The AEC reference feed and audio playback must be coordinated to prevent two race conditions:

1. **Race Condition A**: Discard happens between state check and AEC feed
   - **Solution**: Feed AEC while holding mutex

2. **Race Condition B (TOCTOU)**: Discard happens between releasing mutex and completing audio write
   - **Problem**: We can't hold mutex during `snd_pcm_writei()` (blocks 10-50ms, would prevent barge-in)
   - **Solution**: Use atomic sequence counter to detect if discard happened during unlocked I/O

**Atomic Sequence Counter Pattern**:
- `g_tts_discard_sequence` is incremented each time TTS is discarded
- Before releasing mutex, capture the current sequence value
- After audio write, check if sequence changed (indicates discard happened)
- If changed, the audio device will already be flushed by the discard handler

In `tts_thread_function()`, locate the playback loop. The integration goes in the ALSA and PulseAudio paths. Here's the PulseAudio version (similar changes for ALSA):

```cpp
// Inside the playback loop, find where playback state is checked:

// Check playback state first
pthread_mutex_lock(&tts_mutex);
bool was_paused = false;
while (tts_playback_state == TTS_PLAYBACK_PAUSE) {
    if (!was_paused) {
        LOG_WARNING("TTS playback is PAUSED.");
        was_paused = true;
    }
    pthread_cond_wait(&tts_cond, &tts_mutex);
}

// Check for discard BEFORE feeding AEC
if (tts_playback_state == TTS_PLAYBACK_DISCARD) {
    LOG_WARNING("TTS unpaused to DISCARD.");
    tts_playback_state = TTS_PLAYBACK_IDLE;
    audioBuffer.clear();

#ifdef ENABLE_AEC
    // Increment sequence counter to signal any in-flight playback
    g_tts_discard_sequence.fetch_add(1, std::memory_order_release);
#endif

    // Flush audio device
#ifdef ALSA_DEVICE
    snd_pcm_drop(tts_handle.handle);
    snd_pcm_prepare(tts_handle.handle);
#else
    int pa_error;
    pa_simple_flush(tts_handle.pa_handle, &pa_error);
#endif

    pthread_mutex_unlock(&tts_mutex);
    tts_stop_processing.store(true);
    return;
}

// ============================================================
// AEC INTEGRATION: Feed reference signal WHILE HOLDING MUTEX
// Capture sequence counter to detect discard during audio write
// ============================================================
#ifdef ENABLE_AEC
uint32_t seq_before_write = g_tts_discard_sequence.load(std::memory_order_acquire);

if (tts_playback_state == TTS_PLAYBACK_PLAY &&
    g_tts_thread_resampler && aec_is_enabled()) {
    size_t in_samples = bytes_to_write / sizeof(int16_t);

    // Enforce maximum chunk size
    if (in_samples <= RESAMPLER_MAX_SAMPLES) {
        size_t out_max = resampler_get_output_size(g_tts_thread_resampler, in_samples);

        // Use pre-allocated buffer (g_tts_resample_buffer for TTS thread)
        if (out_max <= RESAMPLER_MAX_SAMPLES) {
            size_t resampled = resampler_process(
                g_tts_thread_resampler,
                (int16_t *)(((uint8_t *)audioBuffer.data()) + i),
                in_samples,
                g_tts_resample_buffer,
                RESAMPLER_MAX_SAMPLES
            );

            if (resampled > 0) {
                aec_add_reference(g_tts_resample_buffer, resampled);
            }
        } else {
            // Rate-limited warning (once per 60 seconds)
            time_t now = time(NULL);
            if (now - g_last_resample_warning >= 60) {
                LOG_WARNING("AEC resampler output too large (%zu > %d), skipping reference",
                            out_max, RESAMPLER_MAX_SAMPLES);
                g_last_resample_warning = now;
            }
        }
    }
}
#endif
// ============================================================

// Release mutex AFTER feeding AEC, BEFORE audio write
// (audio write can block 10-50ms and we can't hold mutex during I/O)
pthread_mutex_unlock(&tts_mutex);

// Perform blocking audio write (cannot hold mutex here)
#ifdef ALSA_DEVICE
rc = snd_pcm_writei(tts_handle.handle, &audioBuffer[i], ...);
#else
rc = pa_simple_write(tts_handle.pa_handle, ((uint8_t*)audioBuffer.data()) + i,
                     bytes_to_write, &error);
#endif

#ifdef ENABLE_AEC
// Check if discard happened during audio write (TOCTOU detection)
if (g_tts_discard_sequence.load(std::memory_order_acquire) != seq_before_write) {
    // Discard occurred while we were writing - audio device already flushed
    // by the discard handler. Exit playback loop.
    LOG_DEBUG("TTS discarded during audio write - exiting playback");
    break;  // Exit the playback loop
}
#endif
```

**Why This Works**:
1. **State check + AEC feed are atomic** (both under mutex) - prevents Race Condition A
2. **Sequence counter detects discard during I/O** - handles Race Condition B
3. **Mutex not held during blocking I/O** - allows main thread to request discard promptly
4. **Discard handler flushes audio device** - any partially-written audio is cleared

**Trade-off Analysis**:
- Mutex hold time: ~1ms (state check + resampling) - acceptable
- Small window where audio may start playing before discard detected - acceptable (human reaction time is 100-200ms, audio device flush is immediate)
- AEC reference fed for audio that may not fully play - acceptable (AEC handles partial reference gracefully)

#### Modify `cleanup_text_to_speech()`:
```cpp
void cleanup_text_to_speech() {
    // ... existing cleanup code ...

#ifdef ENABLE_AEC
    if (g_tts_thread_resampler) {
        resampler_destroy(g_tts_thread_resampler);
        g_tts_thread_resampler = nullptr;
    }
    if (g_network_resampler) {
        resampler_destroy(g_network_resampler);
        g_network_resampler = nullptr;
    }
    // Note: g_tts_resample_buffer and g_network_resample_buffer are static, no free needed
#endif
}
```

### Phase 6: Integrate with Audio Capture Thread

Modify `src/audio/audio_capture_thread.c`.

#### Add include at top:
```c
#ifdef ENABLE_AEC
#include "audio/aec_processor.h"
#endif
```

#### Modify `audio_capture_start()` to allocate AEC buffer:

Add after the context is allocated and before the thread is created:

```c
#ifdef ENABLE_AEC
    // Pre-allocate AEC buffer (same size as capture buffer)
    ctx->aec_buffer_size = ctx->buffer_size / sizeof(int16_t);
    if (ctx->aec_buffer_size > AEC_MAX_SAMPLES) {
        ctx->aec_buffer_size = AEC_MAX_SAMPLES;
    }
    ctx->aec_buffer = (int16_t *)malloc(ctx->aec_buffer_size * sizeof(int16_t));
    if (!ctx->aec_buffer) {
        LOG_WARNING("Failed to allocate AEC buffer - continuing without AEC");
    }
#endif
```

#### Modify `capture_thread_func()` to process through AEC:

Add runtime sample rate validation at the start of the thread (after device is opened):
```c
#ifdef ENABLE_AEC
    // Runtime validation: verify actual sample rate matches AEC requirements
    // This catches cases where device doesn't support requested rate
    unsigned int actual_rate = DEFAULT_RATE;
#ifdef ALSA_DEVICE
    snd_pcm_hw_params_get_rate(hw_params, &actual_rate, NULL);
#endif
    if (actual_rate != AEC_SAMPLE_RATE) {
        LOG_WARNING("AEC requires %d Hz but device is %u Hz - AEC disabled for this session",
                    AEC_SAMPLE_RATE, actual_rate);
        // Set flag to skip AEC processing (aec_buffer will still be allocated but unused)
        ctx->aec_rate_mismatch = true;
    }
#endif
```

Update the log message:
```c
    LOG_INFO("Audio capture thread started (buffer=%zu bytes, device=%s, AEC=%s)",
             ctx->buffer_size, ctx->pcm_device,
#ifdef ENABLE_AEC
             (aec_is_enabled() && ctx->aec_buffer && !ctx->aec_rate_mismatch) ? "enabled" : "disabled"
#else
             "not compiled"
#endif
    );
```

In the ALSA capture loop, replace the `ring_buffer_write` call:
```c
        if (rc > 0) {
            size_t bytes_read = rc * DEFAULT_CHANNELS * 2;
            size_t samples_read = bytes_read / sizeof(int16_t);

#ifdef ENABLE_AEC
            // Process through AEC if enabled, buffer available, and sample rate matches
            if (aec_is_enabled() && ctx->aec_buffer &&
                !ctx->aec_rate_mismatch && samples_read <= ctx->aec_buffer_size) {
                aec_process((int16_t *)buffer, ctx->aec_buffer, samples_read);
                ring_buffer_write(ctx->ring_buffer, (char *)ctx->aec_buffer, bytes_read);
            } else {
                ring_buffer_write(ctx->ring_buffer, buffer, bytes_read);
            }
#else
            ring_buffer_write(ctx->ring_buffer, buffer, bytes_read);
#endif
        }
```

In the PulseAudio capture loop, similar change:
```c
        size_t samples_read = ctx->buffer_size / sizeof(int16_t);

#ifdef ENABLE_AEC
        if (aec_is_enabled() && ctx->aec_buffer &&
            !ctx->aec_rate_mismatch && samples_read <= ctx->aec_buffer_size) {
            aec_process((int16_t *)buffer, ctx->aec_buffer, samples_read);
            ring_buffer_write(ctx->ring_buffer, (char *)ctx->aec_buffer, ctx->buffer_size);
        } else {
            ring_buffer_write(ctx->ring_buffer, buffer, ctx->buffer_size);
        }
#else
        ring_buffer_write(ctx->ring_buffer, buffer, ctx->buffer_size);
#endif
```

#### Modify `audio_capture_stop()` to free AEC buffer:

Add before existing cleanup:
```c
#ifdef ENABLE_AEC
    if (ctx->aec_buffer) {
        free(ctx->aec_buffer);
        ctx->aec_buffer = NULL;
    }
#endif
```

### Phase 7: Initialize/Cleanup in Main

Modify `src/dawn.c`.

#### Add include near top:
```c
#ifdef ENABLE_AEC
#include "audio/aec_processor.h"
#endif
```

#### Initialization sequence:

**IMPORTANT**: AEC must be initialized AFTER TTS (because TTS creates the resampler).

In the initialization section, add AFTER TTS initialization:
```c
    // Initialize TTS
    initialize_text_to_speech(pcm_playback_device);

#ifdef ENABLE_AEC
    // Initialize AEC (must be after TTS which creates the resampler)
    aec_config_t aec_config = aec_get_default_config();

    // Auto-detect platform for mobile mode
#ifdef PLATFORM_RPI
    aec_config.mobile_mode = true;
    LOG_INFO("AEC: Using mobile mode for Raspberry Pi");
#endif

    if (aec_init(&aec_config) != 0) {
        LOG_WARNING("AEC initialization failed - continuing without echo cancellation");
    }
#endif
```

#### Cleanup sequence:

**IMPORTANT**: AEC must be cleaned up BEFORE audio capture stops.

In the cleanup section:
```c
#ifdef ENABLE_AEC
    // Cleanup AEC before stopping audio capture
    aec_cleanup();
#endif

    // Stop audio capture
    audio_capture_stop(capture_ctx);
```

### Phase 8: Update CMakeLists.txt

Add the following section. Place it AFTER the existing audio backend configuration and BEFORE `add_executable`:

```cmake
# =============================================================================
# AEC Support (Acoustic Echo Cancellation)
# =============================================================================
option(ENABLE_AEC "Enable Acoustic Echo Cancellation (WebRTC AEC3)" OFF)

if(ENABLE_AEC)
    message(STATUS "AEC: Checking dependencies...")

    # Include CheckCXXSourceCompiles for API compatibility check
    include(CheckCXXSourceCompiles)

    # -------------------------------------------------------------------------
    # Resampler library (required)
    # Note: We don't use REQUIRED keyword so we can provide user-friendly errors
    # -------------------------------------------------------------------------
    pkg_check_modules(SAMPLERATE samplerate)
    if(NOT SAMPLERATE_FOUND)
        message(FATAL_ERROR
            "libsamplerate not found.\n"
            "Install with: sudo apt-get install libsamplerate0-dev\n"
            "Or disable AEC with: cmake -DENABLE_AEC=OFF ..")
    endif()
    include_directories(${SAMPLERATE_INCLUDE_DIRS})
    message(STATUS "  libsamplerate: Found (${SAMPLERATE_VERSION})")

    # -------------------------------------------------------------------------
    # WebRTC audio processing (required)
    # Target the freedesktop.org variant (webrtc-audio-processing-1)
    # -------------------------------------------------------------------------
    pkg_check_modules(WEBRTC_AP webrtc-audio-processing-1)
    if(NOT WEBRTC_AP_FOUND)
        message(FATAL_ERROR
            "WebRTC audio processing not found.\n"
            "Install with: sudo apt-get install libwebrtc-audio-processing-dev\n"
            "Or build from source: https://gitlab.freedesktop.org/pulseaudio/webrtc-audio-processing\n"
            "Or disable AEC with: cmake -DENABLE_AEC=OFF ..")
    endif()

    # Version check (require 1.0+)
    if(WEBRTC_AP_VERSION VERSION_LESS "1.0")
        message(FATAL_ERROR
            "WebRTC Audio Processing >= 1.0 required, found ${WEBRTC_AP_VERSION}\n"
            "Build from source: https://gitlab.freedesktop.org/pulseaudio/webrtc-audio-processing")
    endif()

    include_directories(${WEBRTC_AP_INCLUDE_DIRS})
    message(STATUS "  webrtc-audio-processing: Found (${WEBRTC_AP_VERSION})")

    # -------------------------------------------------------------------------
    # Verify headers exist at expected location
    # -------------------------------------------------------------------------
    find_path(WEBRTC_HEADER_CHECK
        NAMES modules/audio_processing/include/audio_processing.h
        PATHS ${WEBRTC_AP_INCLUDE_DIRS}
        PATH_SUFFIXES webrtc)
    if(NOT WEBRTC_HEADER_CHECK)
        message(WARNING
            "WebRTC headers not found at expected location - build may fail.\n"
            "Expected: modules/audio_processing/include/audio_processing.h")
    endif()

    # -------------------------------------------------------------------------
    # API Compatibility Check
    # WebRTC audio-processing API changed between versions 1.0-1.3
    # This ensures the installed version has the expected API
    # -------------------------------------------------------------------------
    set(CMAKE_REQUIRED_INCLUDES ${WEBRTC_AP_INCLUDE_DIRS})
    set(CMAKE_REQUIRED_LIBRARIES ${WEBRTC_AP_LIBRARIES})
    check_cxx_source_compiles("
        #include <webrtc/modules/audio_processing/include/audio_processing.h>
        int main() {
            webrtc::AudioProcessing::Config cfg;
            cfg.echo_canceller.enabled = true;
            cfg.echo_canceller.mobile_mode = false;
            cfg.noise_suppression.enabled = true;
            return 0;
        }
    " WEBRTC_API_COMPATIBLE)

    if(NOT WEBRTC_API_COMPATIBLE)
        message(FATAL_ERROR
            "WebRTC Audio Processing API incompatible.\n"
            "This guide targets the 1.0-1.2 API with echo_canceller.enabled.\n"
            "Your version may use a different API structure.\n"
            "Try: sudo apt-get install libwebrtc-audio-processing1-dev\n"
            "Or check the WebRTC API documentation for your version.")
    endif()
    message(STATUS "  WebRTC API: Compatible")

    add_definitions(-DENABLE_AEC)
    message(STATUS "AEC: ENABLED (WebRTC AEC3)")

    # Add AEC source files to DAWN_SOURCES
    list(APPEND DAWN_SOURCES
        src/audio/aec_processor.cpp
        src/audio/resampler.c
    )
else()
    message(STATUS "AEC: DISABLED (use -DENABLE_AEC=ON to enable)")
endif()
```

Then, AFTER the `add_executable(dawn ${DAWN_SOURCES})` and existing `target_link_libraries` calls, add:

```cmake
# Link AEC libraries (must come after add_executable)
# Note: Order matters for static linking - put higher-level libs first
if(ENABLE_AEC)
    target_link_libraries(dawn
        ${WEBRTC_AP_LIBRARIES}    # WebRTC depends on...
        ${SAMPLERATE_LIBRARIES}   # ...nothing AEC-specific
    )
endif()
```

### Phase 9: Build and Test

```bash
# Clean build directory
cd build
rm -rf *

# Configure with AEC enabled
cmake -DENABLE_AEC=ON -DPLATFORM=JETSON ..

# Build
make -j4

# Verify AEC was enabled
grep "AEC:" ../CMakeCache.txt

# Test without AEC first (verify no regression)
cmake -DENABLE_AEC=OFF ..
make -j4
./dawn

# Test with AEC
cmake -DENABLE_AEC=ON ..
make -j4
./dawn
```

## Runtime Tuning Parameters

### Noise Gate Tuning

The noise gate zeros low-amplitude samples after AEC processing to eliminate residual echo that AEC couldn't fully cancel.

**Location**: `src/audio/aec_processor.cpp` - `aec_get_default_config()`

**Default threshold**: 200 (int16 range: -32768 to 32767, ~0.6% of full scale)

**Calibration reference** (tested with ReSpeaker 4-mic array on Jetson Orin Nano):
- Observed residual echo levels post-AEC: 50-180 samples
- Observed speech onset levels: 400+ samples (normal conversation at 0.5m)
- Threshold 200 provides ~20 sample margin above residual, ~200 samples below speech

**Tuning guide**:
| Symptom | Current Value | Adjustment |
|---------|---------------|------------|
| False VAD triggers during TTS | Too low | Increase to 250-300 |
| Speech not detected / clipped | Too high | Decrease to 150-175 |
| Works but marginal | 200 | Consider microphone gain adjustment |

**Hardware considerations**:
- USB microphones may have different noise floors - adjust threshold accordingly
- Higher speaker volume = more residual echo = may need higher threshold
- Microphone placement affects echo level (farther from speaker = less echo)

### VAD Threshold Tuning (Barge-In)

VAD thresholds control when speech is detected during TTS playback.

**Location**: `src/dawn.c` - VAD configuration defines

| Parameter | Default | Purpose |
|-----------|---------|---------|
| `VAD_SPEECH_THRESHOLD` | 0.5 | Normal speech detection (no TTS) |
| `VAD_SPEECH_THRESHOLD_TTS` | 0.85 | Higher threshold during TTS playback |
| `VAD_TTS_DEBOUNCE_COUNT` | 2 | Consecutive detections required during TTS |
| `VAD_TTS_COOLDOWN_MS` | 1000 | Keep TTS threshold active after TTS stops |

**Why 0.85 for TTS threshold**:
- False triggers from residual echo typically produce VAD: 0.65-0.82
- Real barge-in speech produces VAD: 0.90-1.00
- Threshold 0.85 separates these populations with margin

**Tuning guide**:
| Symptom | Adjustment |
|---------|------------|
| Still getting false triggers during TTS | Increase `VAD_SPEECH_THRESHOLD_TTS` to 0.90 |
| Barge-in requires shouting | Decrease `VAD_SPEECH_THRESHOLD_TTS` to 0.80 |
| False triggers between TTS chunks | Increase `VAD_TTS_COOLDOWN_MS` to 1500 |
| Slow response after TTS ends | Decrease `VAD_TTS_COOLDOWN_MS` to 500 |

## Testing and Tuning

### Basic Functionality Test
1. Start DAWN with AEC enabled
2. Trigger a voice response (e.g., ask a question)
3. While DAWN is speaking, say "Friday" clearly
4. Verify wake word is detected without false triggers from DAWN's voice

### Monitoring AEC Performance

Add to TUI metrics display or logging:
```c
#ifdef ENABLE_AEC
aec_stats_t stats;
if (aec_get_stats(&stats) == 0) {
    LOG_DEBUG("AEC: delay=%dms ref_buf=%zu active=%d errors=%d "
              "proc=%llu pass=%llu avg_time=%.1fus",
              stats.estimated_delay_ms,
              stats.ref_buffer_samples,
              stats.is_active,
              stats.consecutive_errors,
              (unsigned long long)stats.frames_processed,
              (unsigned long long)stats.frames_passed_through,
              stats.avg_processing_time_us);
}
#endif
```

### Interpreting Statistics

| Statistic | Normal Range | Issue Indication |
|-----------|--------------|------------------|
| `estimated_delay_ms` | 20-100ms | >200ms suggests buffer sizing issue |
| `ref_buffer_samples` | 0-8000 | Constantly full = TTS faster than capture |
| `consecutive_errors` | 0 | >0 indicates WebRTC API issues |
| `frames_processed` | Increasing | Should grow during TTS playback |
| `frames_passed_through` | Majority | Normal during silence periods |
| `avg_processing_time_us` | <1000 | >2000 may cause audio glitches |

### Tuning Guide

#### Echo Not Fully Cancelled
- **Symptom**: DAWN's voice still triggers VAD/wake word
- **Possible causes**:
  1. Reference buffer too small (audio delay > buffer size)
  2. Acoustic path delay not aligned
- **Fixes**:
  - Increase `ref_buffer_ms` in config (try 750ms or 1000ms)
  - Check `estimated_delay_ms` in stats - should be 20-100ms typically
  - Verify microphone and speaker are not too close together

#### Audio Artifacts / Distortion
- **Symptom**: Cleaned audio sounds robotic or has artifacts
- **Possible causes**:
  1. Resampler quality too low
  2. AEC being too aggressive
- **Fixes**:
  - Change resampler to `SRC_SINC_MEDIUM_QUALITY` in resampler.c
  - Disable noise suppression: `config.enable_noise_suppression = false`

#### High CPU Usage
- **Symptom**: System becomes sluggish when AEC is enabled
- **Possible causes**: Full AEC3 too heavy for platform
- **Fixes**:
  - Enable mobile mode: `config.mobile_mode = true`
  - Disable noise suppression
  - Reduce noise suppression level: `config.noise_suppression_level = AEC_NS_LEVEL_LOW`

#### AEC Disabling Itself
- **Symptom**: Log shows "AEC disabled after N consecutive errors"
- **Possible causes**:
  1. WebRTC library version mismatch
  2. Sample rate misconfiguration
  3. Memory corruption
- **Fixes**:
  - Check WebRTC version matches header API
  - Verify all audio is 16kHz mono
  - Run with address sanitizer: `cmake -DCMAKE_CXX_FLAGS="-fsanitize=address" ..`
  - Call `aec_reset()` to re-enable after fixing the issue

### Performance Benchmarks

Expected performance on target platforms:

| Platform | Mobile Mode | CPU Overhead | Latency Added |
|----------|-------------|--------------|---------------|
| Jetson Orin Nano | OFF | 5-10% | ~10ms (processing) |
| Jetson Orin Nano | ON | 3-5% | ~10ms (processing) |
| RPi 4 | OFF | 15-25% | ~10ms (processing) |
| RPi 4 | ON | 8-12% | ~10ms (processing) |

**Note**: The "latency added" refers to processing overhead only. Total acoustic delay includes speaker-to-mic path (10-50ms) and system buffering (20-50ms).

Echo reduction: 25-40dB typical (depends on acoustic environment)

## Multi-Client Architecture Considerations

**Important**: This AEC implementation is designed for **local microphone echo cancellation only**.

### Current Scope

The AEC processes audio from the local microphone attached to the DAWN device (Jetson/RPi). It removes echo caused by DAWN's TTS output playing through local speakers.

### Network Clients (ESP32)

Network clients currently **do not need AEC** because:
1. ESP32 captures audio **before** transmitting to DAWN (no local TTS playback during capture)
2. DAWN's response is sent back as WAV data (no echo at ESP32's microphone)
3. The echo path is: ESP32 mic → DAWN → DAWN processes → DAWN responds → ESP32 speaker
4. By the time ESP32 plays audio, it's no longer capturing

### Future Multi-Client Considerations

If future designs require client-side echo cancellation (e.g., full-duplex audio streaming):
- Each network client session would need its own `webrtc::AudioProcessing` instance
- The current global `g_apm` cannot be shared between clients
- Consider adding a per-session AEC context structure

```c
// Hypothetical future design (NOT part of current implementation)
typedef struct {
    webrtc::AudioProcessing *apm;
    ring_buffer_t *ref_buffer;
    // ... per-client state
} client_aec_context_t;
```

### Resource Limits for Future Multi-Client AEC

If implementing per-client AEC, consider these constraints:

| Platform | RAM | Max AEC Instances | Total AEC Memory |
|----------|-----|-------------------|------------------|
| Jetson Orin Nano | 8GB | 8 | ~1.6MB |
| Raspberry Pi 4 | 4GB | 4 | ~800KB |

Each AEC instance uses ~200KB (100KB WebRTC state + 16KB ref buffer + 64KB resamplers).

**Recommended approach**: Pre-allocate a fixed pool of AEC instances at startup. When pool is exhausted, new clients operate without AEC (degraded but functional).

### Why This Matters

AEC state is specific to an acoustic environment. Sharing AEC state between:
- Multiple microphones → incorrect echo estimation
- Multiple speakers → reference signal mismatch
- Different rooms → wrong acoustic model

The current design correctly scopes AEC to the single local audio path.

## TUI Metrics Integration

DAWN has an existing TUI (Terminal User Interface) with metrics display. Here's how to integrate AEC status:

### Add to `src/ui/metrics.c` (or equivalent):

```c
#ifdef ENABLE_AEC
#include "audio/aec_processor.h"

/**
 * @brief Update AEC metrics display in TUI
 *
 * Call this from the TUI update loop to show real-time AEC status.
 * Provides visibility into AEC health without spamming logs.
 */
void metrics_update_aec(int row) {
    aec_stats_t stats;
    if (aec_get_stats(&stats) == 0) {
        // Format: "AEC: ACTIVE | Delay: 45ms | Buf: 1234 | Err: 0 | Proc: 12345"
        mvprintw(row, 0, "AEC: %-6s | Delay: %3dms | Buf: %4zu | Err: %2d | Proc: %llu",
                 stats.is_active ? "ACTIVE" : "IDLE",
                 stats.estimated_delay_ms,
                 stats.ref_buffer_samples,
                 stats.consecutive_errors,
                 (unsigned long long)stats.frames_processed);

        // Color coding for status
        if (!stats.is_active) {
            // Red if AEC disabled due to errors
            attron(COLOR_PAIR(COLOR_RED));
            mvprintw(row, 5, "IDLE");
            attroff(COLOR_PAIR(COLOR_RED));
        } else if (stats.consecutive_errors > 0) {
            // Yellow if recovering from errors
            attron(COLOR_PAIR(COLOR_YELLOW));
            mvprintw(row, 5, "ACTIVE");
            attroff(COLOR_PAIR(COLOR_YELLOW));
        }
    } else {
        mvprintw(row, 0, "AEC: NOT INITIALIZED");
    }
}
#endif
```

### Minimal Logging Alternative

If TUI integration is deferred, add periodic logging in the main loop:

```c
#ifdef ENABLE_AEC
// Log AEC stats every 30 seconds during active use
static time_t last_aec_log = 0;
time_t now = time(NULL);
if (now - last_aec_log >= 30) {
    aec_stats_t stats;
    if (aec_get_stats(&stats) == 0 && stats.frames_processed > 0) {
        LOG_INFO("AEC stats: active=%d delay=%dms buf=%zu err=%d proc=%llu pass=%llu",
                 stats.is_active, stats.estimated_delay_ms, stats.ref_buffer_samples,
                 stats.consecutive_errors,
                 (unsigned long long)stats.frames_processed,
                 (unsigned long long)stats.frames_passed_through);
    }
    last_aec_log = now;
}
#endif
```

### Health Indicators

| Indicator | Healthy | Warning | Critical |
|-----------|---------|---------|----------|
| `is_active` | `true` | - | `false` (AEC disabled) |
| `estimated_delay_ms` | 20-100 | 100-200 | >200 (buffer issue) |
| `consecutive_errors` | 0 | 1-5 | >5 (approaching disable) |
| `ref_buffer_samples` | 0-4000 | 4000-7000 | >7000 (overflow risk) |

## Alternative: SpeexDSP (Simpler, Lower Quality)

If WebRTC proves too complex or resource-intensive, SpeexDSP is a lighter alternative:

```bash
sudo apt-get install libspeexdsp-dev
```

```c
#include <speex/speex_echo.h>
#include <speex/speex_preprocess.h>

// Initialize (10ms frame, 100ms filter tail)
SpeexEchoState *echo_state = speex_echo_state_init(160, 1600);
SpeexPreprocessState *preprocess = speex_preprocess_state_init(160, 16000);
speex_preprocess_ctl(preprocess, SPEEX_PREPROCESS_SET_ECHO_STATE, echo_state);

// Process each frame
speex_echo_cancellation(echo_state, mic_frame, ref_frame, out_frame);
speex_preprocess_run(preprocess, out_frame);

// Cleanup
speex_echo_state_destroy(echo_state);
speex_preprocess_state_destroy(preprocess);
```

Trade-offs:
- **Pros**: Simpler API, lower CPU, smaller binary
- **Cons**: Lower quality echo cancellation (10-20dB vs 25-40dB)

## File Summary

### New Files to Create
| File | Description |
|------|-------------|
| `include/audio/aec_processor.h` | AEC public interface with config/stats types |
| `include/audio/resampler.h` | Resampler public interface |
| `src/audio/aec_processor.cpp` | WebRTC AEC3 wrapper implementation |
| `src/audio/resampler.c` | libsamplerate wrapper implementation |

### Files to Modify
| File | Changes |
|------|---------|
| `CMakeLists.txt` | Add ENABLE_AEC option, dependencies, source files, linking |
| `include/audio/audio_capture_thread.h` | Add AEC buffer fields to context struct |
| `src/audio/audio_capture_thread.c` | Add sample rate check, process capture through AEC |
| `src/tts/text_to_speech.cpp` | Add resampler, feed TTS audio to AEC reference |
| `src/dawn.c` | Initialize AEC after TTS, cleanup before capture stops |

## Estimated Effort

| Task | Time |
|------|------|
| Install dependencies | 1 hour |
| Create resampler module | 2-3 hours |
| Create AEC processor module | 5-6 hours |
| Update audio_capture_thread.h | 0.5 hours |
| Integrate with TTS (dual resampler, mutex-protected AEC feed) | 4-5 hours |
| Integrate with capture thread | 2-3 hours |
| Integrate with dawn.c | 1 hour |
| CMake updates (API compatibility check) | 2-3 hours |
| TUI metrics integration (optional) | 2-3 hours |
| Testing and tuning | 10-14 hours |
| **Total** | **30-40 hours** |

**Note**: Time increased from previous estimate due to:
- Dual resampler architecture for thread safety
- Mutex-protected AEC reference feed
- CMake API compatibility check
- Optional TUI integration

## Appendix A: Signal Flow for Barge-In

When user speaks during TTS playback:

```
Time →
──────────────────────────────────────────────────────────────────────

TTS Thread:
  [Generate audio] → [Check state] → [Resample 22050→16000] → [aec_add_reference()] → [pa_write()]
                                                │
                                                ▼
                                        Reference Ring Buffer
                                                │
                                                ▼
Capture Thread:
  [pa_read()] → [aec_process(mic, ref)] → [Ring Buffer] → [Main Loop]
                        │
                        ▼
               Echo-Cancelled Audio
                        │
                        ▼
Main Loop:
  [VAD] → [Wake Word Detection] → "Friday" detected! → [Interrupt TTS]
                                                              │
                                                              ▼
                                                    tts_playback_state = DISCARD
```

Key timing considerations:
1. Reference audio must reach AEC **before** echo arrives at microphone
2. Typical acoustic delay: 10-50ms (speaker to mic through air)
3. System buffering delay: 20-50ms (PulseAudio/ALSA buffers)
4. Reference buffer should accommodate: acoustic delay + system delay + margin ≈ 100-200ms minimum

## Appendix B: Troubleshooting Checklist

Before reporting issues, verify:

- [ ] WebRTC version >= 1.0 (`pkg-config --modversion webrtc-audio-processing-1`)
- [ ] libsamplerate installed (`pkg-config --modversion samplerate`)
- [ ] Build log shows "AEC: ENABLED"
- [ ] Runtime log shows "AEC3 initialized"
- [ ] Capture rate is 16kHz (check audio_capture_thread.c)
- [ ] TTS rate is 22050Hz (check text_to_speech.cpp)
- [ ] `aec_get_stats()` returns `is_active = true`
- [ ] `frames_processed` counter increases during TTS playback
- [ ] No "consecutive errors" in stats
