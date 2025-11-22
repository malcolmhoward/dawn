# DAWN LLM Model Comparison Report

## Test Configuration

- **Hardware:** NVIDIA Jetson Orin
- **GPU Layers:** 99 (full GPU acceleration)
- **Context Size:** 1024 tokens
- **Batch Size:** 512
- **Temperature:** 0.7
- **System Prompt:** DAWN FRIDAY persona with JSON command format

---

## Results Summary

| Model | Tokens/sec | Quality Score | Grade | Speed Grade | Recommendation |
|-------|------------|---------------|-------|-------------|----------------|
| Llama-3.2-3B-Instruct-Q4_K_M |  | 75/105 | C | ❌ | ❌ Too Slow/Poor |
| Qwen3-4B-Instruct-2507-Q4_K_M |  | 86/105 | B | ❌ | ❌ Too Slow/Poor |
| Phi-3-mini-4k-instruct-q4 |  | 44/105 | F | ❌ | ❌ Too Slow/Poor |
| Qwen2.5-7B-Instruct-Q4_K_M |  | 90/105 | B | ❌ | ❌ Too Slow/Poor |
| Qwen_Qwen3-4B-Q6_K_L |  | 38/105 | F | ❌ | ❌ Too Slow/Poor |

---

## Speed Grades

- ⚡⚡⚡ Excellent (35+ tokens/sec)
- ⚡⚡ Good (25-35 tokens/sec)
- ⚡ Acceptable (15-25 tokens/sec)
- ❌ Too Slow (<15 tokens/sec)

## Quality Grades

- **A:** 90-100% (Excellent instruction following)
- **B:** 80-89% (Good instruction following)
- **C:** 70-79% (Acceptable instruction following)
- **D:** 60-69% (Marginal instruction following)
- **F:** <60% (Poor instruction following)

## Recommendations

- ✅ **Recommended:** Fast + High Quality (25+ tokens/sec, A/B grade)
- ⚠️ **Acceptable:** Moderate speed + Decent Quality (20+ tokens/sec, A/B/C grade)
- ❌ **Not Recommended:** Too slow or poor quality

---

## Target Performance for DAWN

**Speed Requirements:**
- Minimum: 20 tokens/sec (2.5s for 50-token response)
- Target: 30 tokens/sec (1.7s for 50-token response)
- Excellent: 40+ tokens/sec (1.25s for 50-token response)

**Quality Requirements:**
- Must follow JSON command format correctly
- Must maintain FRIDAY persona
- Must respect word limits
- Must distinguish between boolean/analog/getter actions

**Expected DAWN Latency:**
```
Silence (1.2s) + ASR (0.5s) + LLM (1.5-2.5s) + TTS (0.2s) = 3.4-4.4s
Target: Under 4.5 seconds end-to-end
```

---

## Detailed Results

See individual files in this directory:
- `<model>_speed.txt` - Speed test results
- `<model>_quality.txt` - Quality test results
- `<model>_server.log` - Server logs

