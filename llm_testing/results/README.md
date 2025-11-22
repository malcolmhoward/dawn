# LLM Testing Results

This directory contains results from extensive local LLM testing on NVIDIA Jetson Orin.

## Directory Structure

### `final_benchmark/`
**Nov 21, 2025 - Final production results (batch 768, context 1024)**

All 5 models tested with optimal fair settings:
- Qwen3-4B-Instruct-2507-Q4_K_M: 81.9% quality (B grade) ✅ **WINNER**
- Qwen2.5-7B-Instruct-Q4_K_M: 85.7% quality (B+ grade, but slower)
- Llama-3.2-3B-Instruct: 71.4% quality (C grade, fastest TTFT)
- Phi-3-mini-4k-instruct: 41.9% quality (F grade)
- Qwen_Qwen3-4B-Q6_K_L: 36.2% quality (F grade, template issues)

Contains:
- `BENCHMARK_REPORT.md` - Executive summary
- Individual quality/speed results for each model
- Server logs

### `cloud_baseline/`
**Cloud LLM baseline for comparison**

- GPT-4o: 100% quality (A+ grade)
- Claude 3.5 Sonnet: 92.4% quality (A grade)

Demonstrates local achieves 82% of cloud quality at zero cost.

### `archive/`
**Historical test data (not needed for reproduction)**

Archived intermediate results from the discovery process. Not required for understanding final results.

## Key Results Summary

| Model | Quality | TTFT | Speed | Recommendation |
|-------|---------|------|-------|----------------|
| **Qwen3-4B Q4** | **81.9%** | 116-138ms | 13.5 tok/s | ✅ **Production** |
| Qwen2.5-7B Q4 | 85.7% | 181-218ms | 9.8 tok/s | Quality-focused |
| Llama-3.2-3B | 71.4% | 92-108ms | 18.0 tok/s | Speed-focused |

## How to Reproduce

1. Install llama.cpp and download models
2. Use configs from `../scripts/model_configs.conf`
3. Run: `../scripts/benchmark_all_models.sh`
4. Quality test: `../scripts/test_llm_quality.py`

See `../docs/MODEL_TEST_ANALYSIS.md` for complete analysis.

## Critical Findings

1. **Batch size is THE parameter** - 18% → 81.9% improvement from batch 256 → 768
2. **Context must be ≥1024** - Quality drops significantly at 512
3. **Sampling parameters have ZERO effect** - Temperature, top-k, top-p don't matter
4. **TTFT matters most** - For streaming, first token time determines perceived latency
5. **Local LLM is viable** - 81.9% quality is production-ready for voice assistant

## Test Date

November 21-22, 2025

## Hardware

NVIDIA Jetson Orin
- 1024-core Ampere GPU
- 16GB unified memory
- CUDA 11.8
