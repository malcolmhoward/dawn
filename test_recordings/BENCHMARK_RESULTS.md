# DAWN ASR Benchmark Results - GPU Acceleration

**Date**: November 21, 2025
**Hardware**: NVIDIA Jetson Orin (Compute Capability 8.7)
**CUDA**: 12.6
**Test Dataset**: 50 WAV files (voice commands, various durations)

## Executive Summary

GPU acceleration via CUDA provides **2.3x to 5.5x speedup** for Whisper ASR models on Jetson Orin, enabling real-time speech recognition with RTF (Real-Time Factor) as low as **0.079** (12.7x faster than realtime).

## Performance Results

### GPU-Accelerated Performance (Current)

| Model          | Avg RTF | Realtime Factor | Samples | Status        |
|----------------|---------|-----------------|---------|---------------|
| Whisper tiny   | 0.079   | 12.7x realtime  | 50      | ✅ Excellent   |
| Whisper base   | 0.109   | 9.2x realtime   | 50      | ✅ Excellent   |
| Whisper small  | 0.225   | 4.4x realtime   | 50      | ✅ Very Good   |

### CPU Baseline (Pre-GPU)

| Model          | Avg RTF | Realtime Factor | Samples | Status        |
|----------------|---------|-----------------|---------|---------------|
| Vosk           | 0.370   | 2.7x realtime   | 50      | ✅ Good        |
| Whisper tiny   | 0.179   | 5.6x realtime   | 50      | ✅ Very Good   |
| Whisper base   | 0.365   | 2.7x realtime   | 50      | ✅ Good        |
| Whisper small  | 1.245   | 0.8x realtime   | 50      | ❌ Unusable    |

## GPU Speedup Analysis

| Model          | CPU RTF | GPU RTF | Speedup | Improvement |
|----------------|---------|---------|---------|-------------|
| Whisper tiny   | 0.179   | 0.079   | 2.27x   | +55.9%      |
| Whisper base   | 0.365   | 0.109   | 3.36x   | +70.2%      |
| Whisper small  | 1.245   | 0.225   | 5.53x   | +81.9%      |

## Key Findings

### 1. GPU Acceleration Impact
- **All Whisper models** benefit significantly from GPU acceleration
- **Larger models** (small) see the most dramatic improvement (5.53x)
- **Whisper small** transformed from unusable to highly practical
- Flash attention disabled to avoid KV cache alignment issues on Jetson

### 2. Model Recommendations

**For Production Use (DAWN Voice Assistant):**
- **Whisper base**: Best balance of speed (9.2x realtime) and accuracy
- RTF 0.109 means 1 second of audio processes in ~109ms
- Leaves plenty of CPU/GPU headroom for other tasks

**For Maximum Speed:**
- **Whisper tiny**: Fastest (12.7x realtime) with acceptable accuracy
- Ideal for low-latency applications

**For Maximum Accuracy:**
- **Whisper small**: Still very fast (4.4x realtime) with GPU
- Now viable for production after GPU acceleration

### 3. Comparison to Vosk
- GPU-accelerated Whisper base is **3.4x faster** than Vosk streaming (0.109 vs 0.370 RTF)
- Whisper provides better accuracy with batch processing
- Vosk still useful for true streaming scenarios

## Technical Configuration

### GPU Settings
```c
// asr_whisper.c configuration
cparams.use_gpu = true;
cparams.flash_attn = false;  // Disabled for Jetson compatibility
```

### Model Sizes
- **Whisper tiny**: ~75 MB
- **Whisper base**: ~147 MB (CUDA0: 147.37 MB)
- **Whisper small**: ~487 MB (CUDA0: 487.01 MB)

### Latency Optimizations
- VAD polling: 50ms (reduced from 100ms)
- Speech end detection: 1.2s silence (reduced from 1.5s)
- Pause detection: 0.3s (reduced from 0.5s)
- Chunking: 10s max per chunk for long utterances

## Real-World Performance

### Live Testing Results (DAWN Application)
During actual voice assistant operation with GPU acceleration:

**Single utterances (1-5 seconds):**
- RTF range: 0.11 - 0.40
- Typical: ~0.2 (5x realtime)

**Multi-chunk long utterances (10+ seconds):**
- Chunk processing: 0.11 - 0.27 RTF
- Example: 10s audio → 1175ms (RTF 0.111)

**End-to-end latency (user speaks → response starts):**
- Before optimizations: ~4.9s
- After GPU + optimizations: ~2.3-3.3s
- **Improvement**: 40-60% reduction

## Methodology

### Test Setup
- **Benchmark tool**: `asr_benchmark` (custom C implementation)
- **Sample rate**: 16 kHz mono
- **Input format**: WAV PCM
- **Engines tested**: Vosk, Whisper tiny/base/small
- **Test files**: 50 real voice command recordings

### Metrics Collected
- **RTF (Real-Time Factor)**: Processing time / audio duration
  - RTF < 1.0 = faster than realtime (good)
  - RTF = 1.0 = exactly realtime
  - RTF > 1.0 = slower than realtime (bad)
- **Model load time**: One-time initialization cost
- **Transcription time**: Per-utterance processing time
- **Transcription accuracy**: Qualitative assessment

### Hardware Specifications
```
Device: NVIDIA Jetson Orin
Compute Capability: 8.7
CUDA Version: 12.6
CUDA Devices: 1
Memory: Unified (shared CPU/GPU)
VMM: Yes (Virtual Memory Management)
```

## Files and Data

### Benchmark Results
- **GPU results**: `results/complete_benchmark.csv`
- **CPU baseline**: `results_cpu_baseline_20251121/complete_benchmark.csv`
- **Detailed logs**: `results/complete_benchmark.log`
- **Analysis**: `BENCHMARK_RESULTS.md` (this file)

### Running the Benchmark
```bash
cd test_recordings
./run_complete_benchmark.sh
```

## Conclusions

1. **GPU acceleration is essential** for Whisper on Jetson platforms
2. **Whisper base with GPU** provides optimal speed/accuracy tradeoff
3. **Real-world performance** matches benchmark results (RTF 0.11-0.27)
4. **Production recommendation**: Use Whisper base with GPU for DAWN voice assistant
5. **Flash attention** should remain disabled on Jetson to avoid KV cache issues

## Future Work

- [ ] Test with Whisper medium/large models
- [ ] Benchmark quantized models (INT8/INT4)
- [ ] Measure power consumption (GPU vs CPU)
- [ ] Test concurrent voice session handling
- [ ] Evaluate Whisper turbo models
- [ ] Compare with other Jetson Orin variants

---

**Generated by**: DAWN Development Team
**Repository**: https://github.com/The-OASIS-Project/dawn
**License**: GPLv3
