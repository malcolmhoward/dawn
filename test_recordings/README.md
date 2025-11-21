# DAWN ASR Test Recordings & Benchmarks

This directory contains voice command test recordings and comprehensive ASR benchmarking tools for the DAWN voice assistant project.

## Contents

### Test Audio Files
- `test_001.wav` through `test_050.wav` - 50 voice command recordings
- Real-world voice commands for wake word, queries, and system controls
- Format: 16 kHz, mono, 16-bit PCM WAV

### Benchmark Scripts
- **`run_complete_benchmark.sh`** - Main benchmark script
- **`analyze_results.py`** - Results analysis tool

### Results
- `results/complete_benchmark.csv` - Current GPU-accelerated results
- `results_cpu_baseline_20251121/` - CPU baseline for comparison
- **`BENCHMARK_RESULTS.md`** - Full performance report

## Quick Start

```bash
# Run full benchmark (15-20 minutes)
./run_complete_benchmark.sh

# Analyze results
./analyze_results.py

# Read full report
cat BENCHMARK_RESULTS.md
```

## Key Results (GPU-Accelerated)

| Model          | Avg RTF | Realtime Factor | GPU Speedup |
|----------------|---------|-----------------|-------------|
| Whisper tiny   | 0.079   | 12.7x realtime  | 2.27x       |
| Whisper base   | 0.109   | 9.2x realtime   | 3.36x       |
| Whisper small  | 0.225   | 4.4x realtime   | 5.53x       |

**RTF (Real-Time Factor)**: Processing time / audio duration
- RTF < 1.0 = faster than realtime ✅

See `BENCHMARK_RESULTS.md` for complete analysis.
