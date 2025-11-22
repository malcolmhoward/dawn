#!/bin/bash

# Comprehensive Model Benchmark for DAWN
# Tests both speed and quality for each model

MODELS_DIR="/var/lib/llama-cpp/models"
TEMPLATE="/var/lib/llama-cpp/templates/qwen3_nonthinking.jinja"
RESULTS_DIR="./llm_benchmark_results_$(date +%Y%m%d_%H%M%S)"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

echo "============================================================================="
echo "DAWN LLM Comprehensive Benchmark"
echo "Testing Speed + Quality for all models"
echo "============================================================================="
echo ""

# Create results directory
mkdir -p "$RESULTS_DIR"

# Find all GGUF models
MODELS=$(find "$MODELS_DIR" -name "*.gguf" -type f 2>/dev/null)

if [ -z "$MODELS" ]; then
    echo -e "${RED}❌ No models found in $MODELS_DIR${NC}"
    exit 1
fi

MODEL_COUNT=$(echo "$MODELS" | wc -l)
echo "Found $MODEL_COUNT models to test"
echo ""

# Get current llama-server PID if running
CURRENT_PID=$(pgrep llama-server)

# Function to stop llama-server
stop_server() {
    echo "Stopping llama-server..."
    killall llama-server 2>/dev/null
    sleep 2
}

# Function to start llama-server with a model
start_server() {
    local model_path="$1"
    local model_name=$(basename "$model_path")

    echo ""
    echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
    echo -e "${BLUE}Testing Model: $model_name${NC}"
    echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
    echo ""

    # Load model-specific configuration
    SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
    source "$SCRIPT_DIR/get_model_config.sh" "$model_name"

    echo "Model-Specific Configuration:"
    echo "  GPU: $GPU_LAYERS, Ctx: $CONTEXT, Batch: $BATCH/$UBATCH"
    echo "  Temp: $TEMP, Top-P: $TOP_P, Top-K: $TOP_K, Repeat: $REPEAT_PENALTY"
    echo ""

    # Start server with model-specific optimized parameters
    /usr/local/bin/llama-server \
        -m "$model_path" \
        --gpu-layers $GPU_LAYERS \
        -c $CONTEXT \
        -b $BATCH \
        -ub $UBATCH \
        -t $THREADS \
        --temp $TEMP \
        --top-p $TOP_P \
        --top-k $TOP_K \
        --repeat-penalty $REPEAT_PENALTY \
        $EXTRA_FLAGS \
        --host 127.0.0.1 \
        --port 8080 \
        --parallel 1 \
        --cont-batching \
        --log-disable \
        > "$RESULTS_DIR/${model_name}_server.log" 2>&1 &

    local server_pid=$!

    # Wait for server to start AND model to load completely
    echo "Starting llama-server (PID: $server_pid)..."
    echo "Waiting for model to load (can take 30-60 seconds)..."
    local max_wait=90
    local waited=0
    while [ $waited -lt $max_wait ]; do
        # Check if health returns 200 (not 503)
        local http_code=$(curl -s -o /dev/null -w "%{http_code}" http://127.0.0.1:8080/health 2>/dev/null)
        if [ "$http_code" = "200" ]; then
            # Try a test query to ensure model is actually loaded
            local test_response=$(curl -s -X POST http://127.0.0.1:8080/v1/chat/completions \
                -H "Content-Type: application/json" \
                -d '{"messages":[{"role":"user","content":"test"}],"max_tokens":5}' 2>/dev/null)

            if echo "$test_response" | grep -q "choices"; then
                echo -e "${GREEN}✅ Server ready and model loaded${NC}"
                return 0
            fi
        fi
        sleep 2
        waited=$((waited + 2))
        echo -n "."
    done

    echo -e "${RED}❌ Server failed to fully load within ${max_wait}s${NC}"
    return 1
}

# Function to run benchmarks for a model
benchmark_model() {
    local model_path="$1"
    local model_name=$(basename "$model_path" .gguf)

    # Start server with this model
    start_server "$model_path"
    if [ $? -ne 0 ]; then
        echo -e "${RED}Skipping $model_name due to server start failure${NC}"
        return 1
    fi

    # Run speed test
    echo ""
    echo "Running Speed Test..."
    echo "─────────────────────────────────────────────────────────────────────────────"
    ./test_llama_performance.sh > "$RESULTS_DIR/${model_name}_speed.txt" 2>&1

    # Run quality test
    echo ""
    echo "Running Quality Test..."
    echo "─────────────────────────────────────────────────────────────────────────────"
    ./test_llm_quality.py > "$RESULTS_DIR/${model_name}_quality.txt" 2>&1

    # Stop server
    stop_server

    echo ""
    echo -e "${GREEN}✅ Completed: $model_name${NC}"
    echo ""
}

# Main benchmark loop
current=1
for model in $MODELS; do
    echo ""
    echo "[$current/$MODEL_COUNT] Benchmarking: $(basename $model)"
    benchmark_model "$model"
    current=$((current + 1))

    # Small delay between models
    sleep 3
done

# Generate comparison report
echo ""
echo "============================================================================="
echo "Generating Comparison Report..."
echo "============================================================================="
echo ""

cat > "$RESULTS_DIR/BENCHMARK_REPORT.md" << 'EOF'
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

EOF

# Parse results and create comparison table
echo "| Model | Tokens/sec | Quality Score | Grade | Speed Grade | Recommendation |" >> "$RESULTS_DIR/BENCHMARK_REPORT.md"
echo "|-------|------------|---------------|-------|-------------|----------------|" >> "$RESULTS_DIR/BENCHMARK_REPORT.md"

for model in $MODELS; do
    model_name=$(basename "$model" .gguf)
    speed_file="$RESULTS_DIR/${model_name}_speed.txt"
    quality_file="$RESULTS_DIR/${model_name}_quality.txt"

    # Extract tokens/sec (average from test 2 - conversational query)
    tokens_sec="N/A"
    if [ -f "$speed_file" ]; then
        tokens_sec=$(grep -A 1 "Test 2: Conversational query" "$speed_file" | grep "Tokens/sec:" | awk '{print $2}')
    fi

    # Extract quality score
    quality_score="N/A"
    quality_grade="N/A"
    if [ -f "$quality_file" ]; then
        quality_line=$(grep "Total Score:" "$quality_file" | tail -1)
        if [ -n "$quality_line" ]; then
            quality_score=$(echo "$quality_line" | grep -oP '\d+/\d+')
            quality_pct=$(echo "$quality_line" | grep -oP '\(\K[0-9.]+')

            # Assign grade
            if (( $(echo "$quality_pct >= 90" | bc -l) )); then
                quality_grade="A"
            elif (( $(echo "$quality_pct >= 80" | bc -l) )); then
                quality_grade="B"
            elif (( $(echo "$quality_pct >= 70" | bc -l) )); then
                quality_grade="C"
            elif (( $(echo "$quality_pct >= 60" | bc -l) )); then
                quality_grade="D"
            else
                quality_grade="F"
            fi
        fi
    fi

    # Speed grade
    speed_grade="N/A"
    if [ "$tokens_sec" != "N/A" ]; then
        if (( $(echo "$tokens_sec >= 35" | bc -l) )); then
            speed_grade="⚡⚡⚡"
        elif (( $(echo "$tokens_sec >= 25" | bc -l) )); then
            speed_grade="⚡⚡"
        elif (( $(echo "$tokens_sec >= 15" | bc -l) )); then
            speed_grade="⚡"
        else
            speed_grade="❌"
        fi
    fi

    # Recommendation
    recommend="⏸️"
    if [ "$tokens_sec" != "N/A" ] && [ "$quality_grade" != "N/A" ]; then
        if (( $(echo "$tokens_sec >= 25" | bc -l) )) && [[ "$quality_grade" =~ [AB] ]]; then
            recommend="✅ Recommended"
        elif (( $(echo "$tokens_sec >= 20" | bc -l) )) && [[ "$quality_grade" =~ [ABC] ]]; then
            recommend="⚠️ Acceptable"
        else
            recommend="❌ Too Slow/Poor"
        fi
    fi

    echo "| $model_name | $tokens_sec | $quality_score | $quality_grade | $speed_grade | $recommend |" >> "$RESULTS_DIR/BENCHMARK_REPORT.md"
done

cat >> "$RESULTS_DIR/BENCHMARK_REPORT.md" << 'EOF'

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

EOF

echo ""
echo -e "${GREEN}✅ Benchmark Complete!${NC}"
echo ""
echo "Results saved to: $RESULTS_DIR/"
echo ""
echo "View report: cat $RESULTS_DIR/BENCHMARK_REPORT.md"
echo ""

# Display quick summary
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "QUICK SUMMARY"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
cat "$RESULTS_DIR/BENCHMARK_REPORT.md" | grep -A 100 "| Model |"
echo ""

# Restart original server if it was running
if [ -n "$CURRENT_PID" ]; then
    echo "Note: Your original llama-server was stopped for testing."
    echo "Restart it manually with your preferred configuration."
fi
