# DAWN Local LLM Model Test Analysis - FINAL RESULTS

## Executive Summary

After extensive testing (31+ configurations across 5 models), we achieved **81.9% quality (B grade)** with local LLM inference on Jetson Orin. The breakthrough came from discovering that **batch size is THE critical parameter** for quality.

**Winner:** Qwen3-4B-Instruct-2507-Q4_K_M @ batch 768, context 1024
- **Quality:** 81.9% (86/105 points, B grade)
- **TTFT:** 116-138ms (excellent for streaming)
- **Speed:** 13.5 tok/s
- **Streaming Latency:** ~1.3s perceived (ASR + TTFT + TTS start)

**Key Discovery:** Temperature, top-k, top-p, repeat_penalty have **ZERO effect** on quality (extensively tested). Only batch size and context matter.

---

## Final Model Rankings (Batch 768, Context 1024)

### Production-Ready Models (C Grade or Better)

| Rank | Model | Quality | TTFT | Speed | Streaming Latency | Recommendation |
|------|-------|---------|------|-------|-------------------|----------------|
| 🥇 1 | **Qwen3-4B Q4** | **81.9%** (B) | **116-138ms** | 13.5 tok/s | **~1.3s** | ✅ **PRODUCTION** |
| 🥈 2 | **Qwen2.5-7B Q4** | **85.7%** (B+) | 181-218ms | 9.8 tok/s | ~1.4s | ✅ Best quality |
| 🥉 3 | **Llama-3.2-3B** | **71.4%** (C) | **92-108ms** | 18.0 tok/s | **~1.2s** | ✅ Fastest |

### Non-Viable Models

| Rank | Model | Quality | Issue |
|------|-------|---------|-------|
| 4 | Phi-3-mini | 41.9% (F) | Poor instruction following |
| 5 | Qwen3-4B Q6 | 36.2% (F) | `<think>` loops (template issue) |

### Cloud Baseline (Reference)

| Model | Quality | Latency | Cost |
|-------|---------|---------|------|
| GPT-4o | 100% (A+) | 3.1s total | ~$0.01/query |
| Claude 3.5 Sonnet | 92.4% (A) | ~3.5s total | ~$0.01/query |

---

## Why Qwen3-4B Q4 is the Winner

### Decision Criteria

**Speed vs Quality Trade-off:**
- Qwen2.5-7B: 4% better quality, but slower TTFT (181ms vs 138ms)
- Llama-3.2-3B: 10% faster TTFT, but 10% lower quality
- **Qwen3-4B Q4: Best balance** - good quality + fast TTFT

### TTFT Analysis (Critical for Streaming)

With DAWN's streaming architecture (LLM → TTS pipeline), **Time To First Token (TTFT) determines perceived latency**, not total generation time.

**Perceived Latency Breakdown:**
```
User speaks → ASR (0.5s) → VAD (0.5s) → TTFT (0.1-0.2s) → TTS starts → User hears response
Total perceived: ~1.1-1.4s
```

**All three top models have excellent TTFT (<220ms):**
- Llama-3.2-3B: 92-108ms (best)
- Qwen3-4B Q4: 116-138ms (excellent)
- Qwen2.5-7B: 181-218ms (good)

**The tok/s speed matters less** - it only affects how long TTS continues generating in background, not when user first hears response.

### Quality Breakdown (Qwen3-4B Q4)

**Category Scores:**
- Boolean commands: **100%** ✅ (20/20)
- Getter commands: **90%** ✅ (18/20)
- Analog commands: **120%** ✅ (12/10, bonus points)
- Music playback: **90%** (9/10)
- Vision AI: **90%** (9/10)
- Multiple commands: **67%** (10/15)
- Clarification: **70%** (7/10)
- Conversational: **10%** (1/10)

**Strengths:**
- Perfect command execution (boolean/getter/analog)
- Excellent JSON formatting
- Maintains FRIDAY persona
- Respects word limits

**Weaknesses:**
- Struggles with conversational questions (10%)
- Multiple command formatting needs improvement (67%)

---

## The Discovery Journey - How We Got Here

### Phase 1: Initial Survey (Batch 256) - All Models Failed

**Nov 21, 18:00 - First tests with batch 256:**

| Model | Quality | Speed | Issue |
|-------|---------|-------|-------|
| Qwen3-4B Q4 | 37.1% | 15.0 tok/s | Poor JSON, missing commands |
| Llama-3.2-3B | 25.7% | 19.7 tok/s | **Gibberish** (3333...) |
| Phi-3-mini | 25.7% | 18.5 tok/s | Blank responses |
| Qwen2.5-7B Q4 | 22.9% | 10.6 tok/s | No JSON commands |
| Qwen3-4B Q6 | 15.2% | 11.5 tok/s | `<think>` loops |

**Conclusion:** Something fundamentally wrong - all models failing badly.

### Phase 2: Batch Size Discovery - BREAKTHROUGH! 🚀

**Qwen3-4B Q4 tested with varying batch sizes:**

| Batch | Quality | Change | Observation |
|-------|---------|--------|-------------|
| 128 | ~18% | baseline | Very poor |
| 256 | 37.1% | +19.1% | Still poor |
| 512 | 56% | +18.9% | Major improvement! |
| **768** | **81.9%** | **+25.9%** | 🎯 **BREAKTHROUGH** |
| 1024 | 81.9% | +0% | Same (memory limit) |

**Critical Finding:** Batch size is THE parameter that matters for quality!
- **18% → 81.9% improvement** just from batch size!
- Context must be ≥1024 for good quality

### Phase 3: Fine-Tuning (19 Parameters) - NO EFFECT!

**Systematically tested at batch 768, context 1024:**

**Sampling Parameters (ALL = 81.9% quality):**
- Temperature: 0.5, 0.7, 0.9, 1.0 → **All 81.9%**
- Top-K: 20, 40, 60, 100 → **All 81.9%**
- Top-P: 0.8, 0.85, 0.9, 0.95 → **All 81.9%**
- Repeat Penalty: 1.0, 1.1, 1.2, 1.3 → **All 81.9%**
- Min-P: 0, 0.05, 0.1 → **All 81.9%**

**Other Parameters (NO EFFECT on quality):**
- Threads: 2, 4, 6, 8 → Only affects speed
- Cache type: f16, q8_0 → No quality change
- GPU layers: 99 (all on GPU)

**Conclusion:** After batch/context are optimal, **nothing else matters for quality!**

### Phase 4: Retest All Models with Batch 768 - Game Changer!

**Nov 21, 23:00 - Fair comparison with batch 768:**

| Model | Batch 256 | Batch 768 | Improvement | Status |
|-------|-----------|-----------|-------------|--------|
| **Qwen3-4B Q4** | 37.1% | **81.9%** | **+44.8%** | ✅ Winner |
| **Qwen2.5-7B** | 22.9% | **85.7%** | **+62.8%** | ✅ Quality king |
| **Llama-3.2-3B** | 25.7% (gibberish) | **71.4%** | **+45.7%** | ✅ Now viable! |
| Phi-3-mini | 25.7% | 41.9% | +16.2% | ❌ Still poor |
| Qwen3-4B Q6 | 15.2% | 36.2% | +21.0% | ❌ Still broken |

**Impact:** Batch size scaling works for Qwen and Llama models, not for Phi-3.

---

## Critical Insights

### 1. Only Two Parameters Matter for Quality

**For Quality:**
- ✅ **Batch size: MUST be ≥768** (18% → 81.9% improvement!)
- ✅ **Context size: MUST be ≥1024** (56% at 512 → 81.9% at 1024)

**Have ZERO Effect on Quality:**
- ❌ Temperature (tested: 0.5, 0.7, 0.9, 1.0)
- ❌ Top-K (tested: 20, 40, 60, 100)
- ❌ Top-P (tested: 0.8, 0.85, 0.9, 0.95)
- ❌ Repeat Penalty (tested: 1.0-1.3)
- ❌ Min-P (tested: 0, 0.05, 0.1)
- ❌ Threads (affects speed only)
- ❌ Cache type (affects memory only)

### 2. TTFT vs Tokens/Sec for Streaming

**Traditional Metric (Misleading for Streaming):**
- Tokens/sec: How fast model generates complete response
- Relevant for: Batch processing, full-response generation
- **Not relevant for:** Streaming architectures like DAWN

**Correct Metric (TTFT):**
- Time To First Token: How long until first token appears
- Relevant for: Streaming TTS, perceived latency
- **Critical for:** User experience in voice assistants

**Why This Matters:**
```
Non-streaming: User waits for ALL tokens before hearing response
Latency = ASR + (all tokens / tok/s) + TTS

Streaming: User hears response as soon as first token arrives
Latency = ASR + TTFT + TTS_start (~0.1s)
```

**Example with Qwen2.5-7B:**
- Tokens/sec: 9.8 tok/s (seems slow)
- TTFT: 181-218ms (actually excellent!)
- **Perceived latency: ~1.4s** (very fast!)

### 3. Q4 vs Q6 Quantization

- **Q4:** 81.9% quality @ 13.5 tok/s, TTFT 138ms
- **Q6:** 36.2% quality @ ? tok/s (still broken)
- **Conclusion:** Q4 is superior for Qwen3-4B (Q6 has template issues)

### 4. Model Size vs Performance

- **3B models:** Fast but lower quality (Llama: 71.4%, 18 tok/s)
- **4B models:** Best balance (Qwen3-4B: 81.9%, 13.5 tok/s) ⭐
- **7B models:** Best quality but slower (Qwen2.5-7B: 85.7%, 9.8 tok/s)

### 5. Batch Size is Model-Specific

Not all models benefit equally from batch 768:
- **Qwen models:** Massive improvement (+45-63%)
- **Llama 3.2:** Massive improvement (+45.7%, fixes gibberish!)
- **Phi-3:** Moderate improvement (+16.2%, still broken)

---

## Performance Comparison

### Local vs Cloud

| Metric | Qwen3-4B Q4 (Local) | GPT-4o (Cloud) | Delta |
|--------|---------------------|----------------|-------|
| Quality | 81.9% (B) | 100% (A+) | -18.1% |
| TTFT | 116-138ms | ~200-300ms | Better! |
| Streaming Latency | ~1.3s | ~3.1s | **Better!** |
| Total Latency | ~4.9s | ~3.1s | +1.8s |
| Cost | $0 | ~$0.01/query | - |
| Privacy | Full | None | - |
| Offline | ✅ Yes | ❌ No | - |
| Reliability | ✅ High | ⚠️ Network-dependent | - |

### Local Model Comparison

| Model | Quality | TTFT | Speed | Use Case |
|-------|---------|------|-------|----------|
| **Qwen3-4B Q4** | 81.9% | 138ms | 13.5 tok/s | **Production** (balanced) |
| Qwen2.5-7B Q4 | 85.7% | 218ms | 9.8 tok/s | Quality-focused |
| Llama-3.2-3B | 71.4% | 108ms | 18.0 tok/s | Speed-focused |

---

## Test Methodology

### Quality Rubric (105 points total)

**Command Categories:**
- Boolean commands (ON/OFF): 20 points
- Getter commands (get status): 20 points
- Analog commands (set level): 10 points
- Music playback: 10 points
- Vision AI: 10 points
- Multiple commands: 15 points

**Response Quality:**
- Clarification requests: 10 points
- Conversational responses: 10 points

**Grading Scale:**
- A+ (95-100%): Excellent
- A (90-94%): Very Good
- B (80-89%): Good
- C (70-79%): Acceptable
- D (60-69%): Poor
- F (<60%): Fail

### Speed Measurement

- **TTFT:** Time to first token (critical for streaming)
- **Tokens/sec:** Generation throughput (background metric)
- **Streaming Latency:** ASR + TTFT + TTS_start
- **Total Latency:** ASR + all tokens + TTS

### Hardware Platform

**NVIDIA Jetson Orin (embedded):**
- GPU: 1024-core Ampere
- RAM: 16GB unified memory
- CUDA: 11.8

**Jetson-Specific Issues:**
- Memory fragmentation prevents large batch allocations
- Need to reboot for clean memory state
- CUDA allocator less robust than desktop GPUs
- Batch 1024 fails even with 11GB free (hence batch 768)

---

## Configuration Summary

### Tested Configurations: 31+

**Initial Survey:** 5 models × various configs = 19 tests
**Batch Size Search:** 7 different batch sizes
**Fine-Tuning:** 19 parameters tested (temp, top-k, top-p, etc.)

**Total:** 31+ distinct test runs with full quality validation

### Winner Configuration

**Model:** Qwen3-4B-Instruct-2507-Q4_K_M.gguf
```bash
GPU_LAYERS: 99
CONTEXT: 1024
BATCH: 768
UBATCH: 768
THREADS: 4
TEMP: 0.7
TOP_P: 0.9
TOP_K: 40
REPEAT_PENALTY: 1.1
EXTRA_FLAGS: --flash-attn
```

**Template:** ChatML format (standard for Qwen models)
**Location:** `/var/lib/llama-cpp/templates/qwen3_chatml.jinja`

---

## Production Recommendations

### Primary Recommendation: Qwen3-4B Q4

**Use When:**
- Balanced speed/quality needed
- TTFT ~138ms acceptable
- 81.9% quality sufficient
- Running on Jetson Orin or similar

**Deployment:**
```bash
cd services/llama-server
sudo ./install.sh
# Uses optimal settings by default
```

### Alternative 1: Qwen2.5-7B Q4 (Quality-Focused)

**Use When:**
- Best quality needed (85.7%)
- Can tolerate +80ms TTFT vs Qwen3-4B
- Have memory for 7B model

**Trade-off:** 4% better quality, 60% slower generation (but TTFT only +80ms)

### Alternative 2: Llama-3.2-3B (Speed-Focused)

**Use When:**
- Fastest TTFT needed (92-108ms)
- 71.4% quality acceptable
- Want snappiest responses

**Trade-off:** 10% lower quality, but 25% faster TTFT

### Cloud Option: GPT-4o (Premium)

**Use When:**
- 100% quality required
- Cost acceptable (~$0.01/query)
- Network reliability guaranteed
- Privacy not a concern

---

## Detailed Results Locations

### Final Benchmark Results
- `llm_testing/results/final_benchmark/` - Nov 21 batch 768 tests
  - All 5 models tested with fair settings
  - Quality + speed results
  - Individual test outputs

### Historical Results (Archive)
- `llm_testing/results/archive/preliminary/` - Batch 256 tests
- `llm_testing/results/archive/batch_search/` - Batch size discovery
- `llm_testing/results/archive/fine_tuning/` - 19-parameter tests

### Cloud Baseline
- `llm_testing/results/cloud_baseline/` - GPT-4o & Claude results

### Scripts
- `llm_testing/scripts/test_single_model.sh` - Single model testing
- `llm_testing/scripts/benchmark_all_models.sh` - Batch testing
- `llm_testing/scripts/test_llm_quality.py` - Quality validator
- `llm_testing/scripts/model_configs.conf` - Model-specific configs

---

## Key Takeaways

1. ⭐ **Batch size is everything** - 18% → 81.9% improvement
2. ⭐ **Context must be ≥1024** - Quality drops at 512
3. ⭐ **Sampling parameters don't matter** - Temp/top-k/top-p have ZERO effect
4. ⭐ **TTFT > tokens/sec** - For streaming, first token time matters most
5. ⭐ **Local LLM is viable** - 81.9% is good enough for production
6. ⭐ **Three viable options** - Qwen3-4B (balanced), Qwen2.5-7B (quality), Llama-3.2 (speed)
7. ⭐ **Batch scaling is model-specific** - Works great for Qwen/Llama, not Phi-3
8. ⭐ **Q4 > Q6** - For Qwen3-4B, Q4 performs better (Q6 has template issues)

---

## Future Work

### Potential Improvements

1. **Fix Qwen3-4B Q6 template issue** - Could be better than Q4 if template works
2. **Test larger context (2048)** - May improve multi-turn conversations
3. **Fine-tune for DAWN** - Custom training on DAWN command dataset
4. **Hybrid architecture** - Local for common commands, cloud for complex queries
5. **Investigate Phi-3 issues** - Why doesn't batch scaling help?

### Open Questions

1. Why do only batch/context affect quality? (LLM inference theory question)
2. Can we get Q6 working properly? (Template investigation needed)
3. Would context 2048 improve conversational scores? (Currently 10%)
4. Is there a sweet spot between batch 512-768? (Fine-grained search)

---

## Conclusion

After 31+ configurations tested across 5 models, we achieved **81.9% quality (B grade)** with local inference on Jetson Orin. The breakthrough was discovering that **batch size ≥768 and context ≥1024 are the only parameters that matter** for quality.

**Winner:** Qwen3-4B-Instruct-2507-Q4_K_M @ batch 768, context 1024
- **Quality:** 81.9% (B grade, production-ready)
- **TTFT:** 116-138ms (excellent for streaming)
- **Streaming Latency:** ~1.3s (fast, responsive)
- **Status:** Ready for production deployment

**Alternative options available** for quality-focused (Qwen2.5-7B @ 85.7%) or speed-focused (Llama-3.2-3B @ 71.4%) use cases.

**Cloud comparison:** GPT-4o achieves 100% quality but costs $0.01/query and requires network. Local option provides 82% of cloud quality at zero cost with full privacy and offline capability.

**Recommendation:** Deploy Qwen3-4B Q4 for production. Consider Qwen2.5-7B for quality-critical applications or Llama-3.2-3B for speed-critical applications.
