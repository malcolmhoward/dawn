#!/bin/bash

# Parse model-specific configuration from model_configs.conf
# Usage: source get_model_config.sh <model_filename>
# Sets variables: GPU_LAYERS, CONTEXT, BATCH, UBATCH, THREADS, TEMP, TOP_P, TOP_K, REPEAT_PENALTY, EXTRA_FLAGS

get_model_config() {
    local model_file="$1"
    local model_name=$(basename "$model_file")
    local script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
    local config_file="$script_dir/model_configs.conf"

    # Check if config file exists
    if [ ! -f "$config_file" ]; then
        echo "ERROR: Config file not found: $config_file" >&2
        return 1
    fi

    # Search for model-specific config
    local config_line=$(grep "^${model_name}|" "$config_file" 2>/dev/null)

    if [ -z "$config_line" ]; then
        echo "WARNING: No config found for $model_name, using defaults" >&2

        # Default configuration (safe for most models)
        export GPU_LAYERS=99
        export CONTEXT=1024
        export BATCH=512
        export UBATCH=512
        export THREADS=4
        export TEMP=0.7
        export TOP_P=0.9
        export TOP_K=40
        export REPEAT_PENALTY=1.1
        export EXTRA_FLAGS="--flash-attn"
        return 0
    fi

    # Parse configuration line
    # Format: MODEL_NAME|gpu_layers|context|batch|ubatch|threads|temp|top_p|top_k|repeat_penalty|extra_flags
    IFS='|' read -r _ GPU_LAYERS CONTEXT BATCH UBATCH THREADS TEMP TOP_P TOP_K REPEAT_PENALTY EXTRA_FLAGS <<< "$config_line"

    # Export for use in calling script
    export GPU_LAYERS
    export CONTEXT
    export BATCH
    export UBATCH
    export THREADS
    export TEMP
    export TOP_P
    export TOP_K
    export REPEAT_PENALTY
    export EXTRA_FLAGS

    return 0
}

# If script is sourced with an argument, load that model's config
if [ -n "$1" ]; then
    get_model_config "$1"
fi
