#!/bin/bash

# Display all configured models and their settings

echo "============================================================================="
echo "Configured Models"
echo "============================================================================="
echo ""

CONFIG_FILE="$(dirname "$0")/model_configs.conf"

if [ ! -f "$CONFIG_FILE" ]; then
    echo "ERROR: Config file not found: $CONFIG_FILE"
    exit 1
fi

# Parse and display configurations
echo "Model                               | GPU | Ctx  | Batch | Temp | Top-P | Top-K | Repeat"
echo "------------------------------------+-----+------+-------+------+-------+-------+-------"

while IFS='|' read -r model gpu ctx batch ubatch threads temp topp topk repeat flags; do
    # Skip comments and empty lines
    [[ "$model" =~ ^#.*$ ]] && continue
    [[ -z "$model" ]] && continue

    # Format model name (truncate if too long)
    model_display=$(printf "%-35s" "$model" | cut -c1-35)

    printf "%s | %3s | %4s | %5s | %4s | %5s | %5s | %6s\n" \
        "$model_display" "$gpu" "$ctx" "$batch" "$temp" "$topp" "$topk" "$repeat"
done < <(grep -v "^#" "$CONFIG_FILE" | grep -v "^$")

echo ""
echo "============================================================================="
echo "Key Differences:"
echo "============================================================================="
echo ""
echo "Llama/Phi Models:"
echo "  - Lower temperature (0.5 vs 0.7) to prevent gibberish"
echo "  - Restricted top-k (20 vs 40) for tighter control"
echo "  - Higher repeat penalty (1.3 vs 1.1) to avoid repetition"
echo ""
echo "Qwen 7B:"
echo "  - Smaller batch (128 vs 512) to fit all layers on GPU"
echo "  - Same sampling parameters as 4B models"
echo ""
echo "To test a model with its specific configuration:"
echo "  ./test_single_model.sh <model_filename.gguf>"
echo ""
