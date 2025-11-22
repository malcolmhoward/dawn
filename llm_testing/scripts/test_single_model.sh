#!/bin/bash

# Test single model with model-specific optimized configuration
# Uses settings from model_configs.conf for each model

if [ -z "$1" ]; then
    echo "Usage: $0 <model_name.gguf>"
    echo ""
    echo "Available models:"
    ls -1 /var/lib/llama-cpp/models/*.gguf 2>/dev/null | xargs -n 1 basename
    echo ""
    exit 1
fi

MODEL_PATH="/var/lib/llama-cpp/models/$1"

if [ ! -f "$MODEL_PATH" ]; then
    echo "❌ Model not found: $MODEL_PATH"
    echo ""
    echo "Available models:"
    ls -1 /var/lib/llama-cpp/models/*.gguf 2>/dev/null | xargs -n 1 basename
    exit 1
fi

MODEL_NAME=$(basename "$MODEL_PATH" .gguf)
MODEL_FILE=$(basename "$MODEL_PATH")

echo "============================================================================="
echo "Testing Model: $MODEL_NAME"
echo "============================================================================="
echo ""

# Load model-specific configuration
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/get_model_config.sh" "$MODEL_FILE"

if [ $? -ne 0 ]; then
    echo "❌ Failed to load model configuration"
    exit 1
fi

# Display configuration being used
echo "Model-Specific Configuration:"
echo "  GPU Layers:      $GPU_LAYERS"
echo "  Context Size:    $CONTEXT"
echo "  Batch Size:      $BATCH / $UBATCH"
echo "  Threads:         $THREADS"
echo "  Temperature:     $TEMP"
echo "  Top-P:           $TOP_P"
echo "  Top-K:           $TOP_K"
echo "  Repeat Penalty:  $REPEAT_PENALTY"
echo "  Extra Flags:     $EXTRA_FLAGS"
echo ""

# Stop any running server
echo "Stopping any running llama-server..."
killall llama-server 2>/dev/null
sleep 2

# Start server with model-specific settings
echo "Starting llama-server with optimized settings..."
/usr/local/bin/llama-server \
    -m "$MODEL_PATH" \
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
    --metrics &

SERVER_PID=$!
echo "Server PID: $SERVER_PID"
echo ""

# Wait for server to start AND model to load
echo "Waiting for server to start and model to load..."
echo "(This can take 30-90 seconds for model loading...)"
MAX_WAIT=120
WAITED=0
while [ $WAITED -lt $MAX_WAIT ]; do
    # Check if health returns 200 (not 503)
    HTTP_CODE=$(curl -s -o /dev/null -w "%{http_code}" http://127.0.0.1:8080/health 2>/dev/null)
    if [ "$HTTP_CODE" = "200" ]; then
        # Try a test query to make sure model is actually loaded
        TEST_RESPONSE=$(curl -s -X POST http://127.0.0.1:8080/v1/chat/completions \
            -H "Content-Type: application/json" \
            -d '{"messages":[{"role":"user","content":"test"}],"max_tokens":5}' 2>/dev/null)

        if echo "$TEST_RESPONSE" | grep -q "choices"; then
            echo ""
            echo "✅ Server ready and model loaded!"
            echo ""
            break
        fi
    fi
    sleep 2
    WAITED=$((WAITED + 2))
    echo -n "."
done

if [ $WAITED -ge $MAX_WAIT ]; then
    echo ""
    echo "❌ Server failed to fully load within ${MAX_WAIT}s"
    kill $SERVER_PID 2>/dev/null
    exit 1
fi

# Create results directory
TIMESTAMP=$(date +%Y%m%d_%H%M%S)
RESULTS_DIR="./single_model_test_${MODEL_NAME}_${TIMESTAMP}"
mkdir -p "$RESULTS_DIR"

# Save configuration used
cat > "$RESULTS_DIR/config_used.txt" << EOF
Model: $MODEL_NAME
File: $MODEL_FILE
Timestamp: $TIMESTAMP

Configuration Applied:
  GPU Layers:      $GPU_LAYERS
  Context Size:    $CONTEXT
  Batch Size:      $BATCH
  Micro-Batch:     $UBATCH
  Threads:         $THREADS
  Temperature:     $TEMP
  Top-P:           $TOP_P
  Top-K:           $TOP_K
  Repeat Penalty:  $REPEAT_PENALTY
  Extra Flags:     $EXTRA_FLAGS

Command Line:
/usr/local/bin/llama-server \\
  -m "$MODEL_PATH" \\
  --gpu-layers $GPU_LAYERS \\
  -c $CONTEXT \\
  -b $BATCH \\
  -ub $UBATCH \\
  -t $THREADS \\
  --temp $TEMP \\
  --top-p $TOP_P \\
  --top-k $TOP_K \\
  --repeat-penalty $REPEAT_PENALTY \\
  $EXTRA_FLAGS \\
  --host 127.0.0.1 \\
  --port 8080 \\
  --parallel 1 \\
  --cont-batching
EOF

echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "SPEED TEST"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo ""

./test_llama_performance.sh | tee "$RESULTS_DIR/speed_results.txt"

echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "QUALITY TEST"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo ""

./test_llm_quality.py | tee "$RESULTS_DIR/quality_results.txt"

# Stop server
echo ""
echo "Stopping llama-server..."
kill $SERVER_PID 2>/dev/null
sleep 2

# Parse results
TOKENS_SEC=$(grep "Tokens/sec:" "$RESULTS_DIR/speed_results.txt" | awk '{print $2}' | head -2 | tail -1)
QUALITY_SCORE=$(grep "Total Score:" "$RESULTS_DIR/quality_results.txt" | tail -1 | grep -oP '\d+/\d+')
QUALITY_PCT=$(grep "Total Score:" "$RESULTS_DIR/quality_results.txt" | tail -1 | grep -oP '\(\K[0-9.]+')

echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "SUMMARY: $MODEL_NAME"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo ""
echo "Speed:   $TOKENS_SEC tokens/sec"
echo "Quality: $QUALITY_SCORE ($QUALITY_PCT%)"
echo ""

if [ -n "$TOKENS_SEC" ] && [ -n "$QUALITY_PCT" ]; then
    if (( $(echo "$TOKENS_SEC >= 25 && $QUALITY_PCT >= 80" | bc -l 2>/dev/null || echo 0) )); then
        echo "✅ RECOMMENDED FOR DAWN"
    elif (( $(echo "$TOKENS_SEC >= 20 && $QUALITY_PCT >= 70" | bc -l 2>/dev/null || echo 0) )); then
        echo "⚠️  ACCEPTABLE"
    else
        echo "❌ NOT RECOMMENDED"
    fi
fi

echo ""
echo "Results saved to: $RESULTS_DIR/"
echo "Configuration saved to: $RESULTS_DIR/config_used.txt"
echo ""
