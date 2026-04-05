#!/bin/bash
#
# Llama.cpp Server Installation Script for DAWN
# Installs llama-server as a systemd service with optimal settings.
# Supports interactive preset selection with hardware auto-detection.
#

set -e

# =============================================================================
# Configuration
# =============================================================================

PORT="8080"
HOST="127.0.0.1"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SERVICE_USER="llama"
DATA_DIR="/var/lib/llama-cpp"
CONFIG_DIR="/usr/local/etc/llama-cpp"

# CLI overrides (legacy mode)
CLI_MODEL_PATH=""
CLI_TEMPLATE_PATH=""
CLI_MMPROJ_PATH=""
CLI_PRESET=""

# Colors
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
RED='\033[0;31m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
BOLD='\033[1m'
DIM='\033[2m'
NC='\033[0m'

# =============================================================================
# Helper functions
# =============================================================================

log()   { echo -e "${GREEN}[INFO]${NC} $1"; }
warn()  { echo -e "${YELLOW}[WARNING]${NC} $1"; }
error() { echo -e "${RED}[ERROR]${NC} $1"; exit 1; }

# =============================================================================
# Preset definitions
# =============================================================================

# Each preset defines: label, model filename, model HF repo, model HF file,
# mmproj filename (empty=none), mmproj HF repo, mmproj HF file,
# template filename (empty=built-in), reasoning_format, context_size,
# batch_size, temperature, top_p, top_k, min_p, repeat_penalty,
# min_memory_gb, speed, quality, vision (0/1), notes

define_presets() {
    # Preset A: Qwen3 4B Instruct (no vision)
    PRESET_A_LABEL="Qwen3 4B Instruct"
    PRESET_A_MODEL="Qwen_Qwen3-4B-Instruct-2507-Q4_K_M.gguf"
    PRESET_A_HF_REPO="bartowski/Qwen_Qwen3-4B-Instruct-2507-GGUF"
    PRESET_A_HF_FILE="Qwen_Qwen3-4B-Instruct-2507-Q4_K_M.gguf"
    PRESET_A_MMPROJ=""
    PRESET_A_MMPROJ_REPO=""
    PRESET_A_MMPROJ_FILE=""
    PRESET_A_TEMPLATE="qwen3_chatml.jinja"
    PRESET_A_REASONING="none"
    PRESET_A_CTX=16384
    PRESET_A_BATCH=768
    PRESET_A_TEMP="0.7"
    PRESET_A_TOP_P="0.8"
    PRESET_A_TOP_K=20
    PRESET_A_MIN_P=0
    PRESET_A_REPEAT="1.1"
    PRESET_A_MIN_MEM=6
    PRESET_A_SPEED="35.1"
    PRESET_A_QUALITY="91.8% (A)"
    PRESET_A_VISION=0
    PRESET_A_SIZE="2.5 GB"
    PRESET_A_NOTES="Voice, no vision"

    # Preset A2: Qwen3.5 4B (vision, hybrid SSM)
    PRESET_A2_LABEL="Qwen3.5 4B Vision"
    PRESET_A2_MODEL="Qwen_Qwen3.5-4B-Q4_K_M.gguf"
    PRESET_A2_HF_REPO="bartowski/Qwen_Qwen3.5-4B-GGUF"
    PRESET_A2_HF_FILE="Qwen_Qwen3.5-4B-Q4_K_M.gguf"
    PRESET_A2_MMPROJ="mmproj-Qwen_Qwen3.5-4B-f16.gguf"
    PRESET_A2_MMPROJ_REPO="bartowski/Qwen_Qwen3.5-4B-GGUF"
    PRESET_A2_MMPROJ_FILE="mmproj-Qwen_Qwen3.5-4B-f16.gguf"
    PRESET_A2_TEMPLATE=""
    PRESET_A2_REASONING="deepseek"
    PRESET_A2_CTX=16384
    PRESET_A2_BATCH=768
    PRESET_A2_TEMP="0.7"
    PRESET_A2_TOP_P="0.8"
    PRESET_A2_TOP_K=20
    PRESET_A2_MIN_P=0
    PRESET_A2_REPEAT="1.1"
    PRESET_A2_MIN_MEM=6
    PRESET_A2_SPEED="28.4"
    PRESET_A2_QUALITY="88.1% (B)"
    PRESET_A2_VISION=1
    PRESET_A2_SIZE="2.9 GB"
    PRESET_A2_NOTES="Vision, small hardware"

    # Preset A3: Gemma 3 4B Vision (fastest 4B with vision)
    PRESET_A3_LABEL="Gemma 3 4B Vision"
    PRESET_A3_MODEL="google_gemma-3-4b-it-Q4_K_M.gguf"
    PRESET_A3_HF_REPO="bartowski/google_gemma-3-4b-it-GGUF"
    PRESET_A3_HF_FILE="google_gemma-3-4b-it-Q4_K_M.gguf"
    PRESET_A3_MMPROJ="mmproj-google_gemma-3-4b-it-f16.gguf"
    PRESET_A3_MMPROJ_REPO="bartowski/google_gemma-3-4b-it-GGUF"
    PRESET_A3_MMPROJ_FILE="mmproj-google_gemma-3-4b-it-f16.gguf"
    PRESET_A3_TEMPLATE=""
    PRESET_A3_REASONING="deepseek"
    PRESET_A3_CTX=16384
    PRESET_A3_BATCH=768
    PRESET_A3_TEMP="1.0"
    PRESET_A3_TOP_P="0.95"
    PRESET_A3_TOP_K=64
    PRESET_A3_MIN_P=0
    PRESET_A3_REPEAT="1.0"
    PRESET_A3_MIN_MEM=6
    PRESET_A3_SPEED="36.3"
    PRESET_A3_QUALITY="87.3% (B)"
    PRESET_A3_VISION=1
    PRESET_A3_SIZE="2.5 GB"
    PRESET_A3_NOTES="Best for 16GB + vision"

    # Preset B: Qwen3 4B Thinking
    PRESET_B_LABEL="Qwen3 4B Thinking"
    PRESET_B_MODEL="Qwen3-4B-Thinking-2507-Q4_K_M.gguf"
    PRESET_B_HF_REPO="unsloth/Qwen3-4B-Thinking-2507-GGUF"
    PRESET_B_HF_FILE="Qwen3-4B-Thinking-2507-Q4_K_M.gguf"
    PRESET_B_MMPROJ=""
    PRESET_B_MMPROJ_REPO=""
    PRESET_B_MMPROJ_FILE=""
    PRESET_B_TEMPLATE="qwen3_official.jinja"
    PRESET_B_REASONING="deepseek"

    PRESET_B_CTX=16384
    PRESET_B_BATCH=768
    PRESET_B_TEMP="0.7"
    PRESET_B_TOP_P="0.8"
    PRESET_B_TOP_K=20
    PRESET_B_MIN_P=0
    PRESET_B_REPEAT="1.1"
    PRESET_B_MIN_MEM=6
    PRESET_B_SPEED="TBD"
    PRESET_B_QUALITY="TBD"
    PRESET_B_VISION=0
    PRESET_B_SIZE="2.3 GB"
    PRESET_B_NOTES="Reasoning mode"

    # Preset C: Gemma 3 12B Vision
    PRESET_C_LABEL="Gemma 3 12B Vision"
    PRESET_C_MODEL="google_gemma-3-12b-it-Q4_K_M.gguf"
    PRESET_C_HF_REPO="bartowski/google_gemma-3-12b-it-GGUF"
    PRESET_C_HF_FILE="google_gemma-3-12b-it-Q4_K_M.gguf"
    PRESET_C_MMPROJ="mmproj-google_gemma-3-12b-it-f16.gguf"
    PRESET_C_MMPROJ_REPO="bartowski/google_gemma-3-12b-it-GGUF"
    PRESET_C_MMPROJ_FILE="mmproj-google_gemma-3-12b-it-f16.gguf"
    PRESET_C_TEMPLATE=""
    PRESET_C_REASONING="deepseek"
    PRESET_C_CTX=16384
    PRESET_C_BATCH=768
    PRESET_C_TEMP="1.0"
    PRESET_C_TOP_P="0.95"
    PRESET_C_TOP_K=64
    PRESET_C_MIN_P=0
    PRESET_C_REPEAT="1.0"
    PRESET_C_MIN_MEM=16
    PRESET_C_SPEED="16.1"
    PRESET_C_QUALITY="91.8% (A)"
    PRESET_C_VISION=1
    PRESET_C_SIZE="7.3 GB"
    PRESET_C_NOTES="High quality + vision"

    # Preset D: Gemma 4 31B dense
    PRESET_D_LABEL="Gemma 4 31B Vision (dense)"
    PRESET_D_MODEL="google_gemma-4-31B-it-Q4_K_M.gguf"
    PRESET_D_HF_REPO="bartowski/google_gemma-4-31B-it-GGUF"
    PRESET_D_HF_FILE="google_gemma-4-31B-it-Q4_K_M.gguf"
    PRESET_D_MMPROJ="mmproj-google_gemma-4-31B-it-f16.gguf"
    PRESET_D_MMPROJ_REPO="bartowski/google_gemma-4-31B-it-GGUF"
    PRESET_D_MMPROJ_FILE="mmproj-google_gemma-4-31B-it-f16.gguf"
    PRESET_D_TEMPLATE=""
    PRESET_D_REASONING="deepseek"

    PRESET_D_CTX=32768
    PRESET_D_BATCH=768
    PRESET_D_TEMP="1.0"
    PRESET_D_TOP_P="0.95"
    PRESET_D_TOP_K=64
    PRESET_D_MIN_P=0
    PRESET_D_REPEAT="1.0"
    PRESET_D_MIN_MEM=48
    PRESET_D_SPEED="6.8"
    PRESET_D_QUALITY="TBD"
    PRESET_D_VISION=1
    PRESET_D_SIZE="18.2 GB"
    PRESET_D_NOTES="WebUI only, thinking leaks"

    # Preset E: Qwen 3.5 27B dense
    PRESET_E_LABEL="Qwen3.5 27B Vision (dense)"
    PRESET_E_MODEL="Qwen_Qwen3.5-27B-Q4_K_M.gguf"
    PRESET_E_HF_REPO="bartowski/Qwen_Qwen3.5-27B-GGUF"
    PRESET_E_HF_FILE="Qwen_Qwen3.5-27B-Q4_K_M.gguf"
    PRESET_E_MMPROJ="mmproj-Qwen_Qwen3.5-27B-f16.gguf"
    PRESET_E_MMPROJ_REPO="bartowski/Qwen_Qwen3.5-27B-GGUF"
    PRESET_E_MMPROJ_FILE="mmproj-Qwen_Qwen3.5-27B-f16.gguf"
    PRESET_E_TEMPLATE=""
    PRESET_E_REASONING="deepseek"

    PRESET_E_CTX=32768
    PRESET_E_BATCH=768
    PRESET_E_TEMP="0.7"
    PRESET_E_TOP_P="0.8"
    PRESET_E_TOP_K=20
    PRESET_E_MIN_P=0
    PRESET_E_REPEAT="1.1"
    PRESET_E_MIN_MEM=48
    PRESET_E_SPEED="7.2"
    PRESET_E_QUALITY="TBD"
    PRESET_E_VISION=1
    PRESET_E_SIZE="15.9 GB"
    PRESET_E_NOTES="WebUI only"

    # Preset F: Qwen 3.5 35B-A3B MoE (RECOMMENDED)
    PRESET_F_LABEL="Qwen3.5 35B-A3B Vision (MoE)"
    PRESET_F_MODEL="Qwen_Qwen3.5-35B-A3B-Q4_K_M.gguf"
    PRESET_F_HF_REPO="bartowski/Qwen_Qwen3.5-35B-A3B-GGUF"
    PRESET_F_HF_FILE="Qwen_Qwen3.5-35B-A3B-Q4_K_M.gguf"
    PRESET_F_MMPROJ="mmproj-Qwen_Qwen3.5-35B-A3B-f16.gguf"
    PRESET_F_MMPROJ_REPO="bartowski/Qwen_Qwen3.5-35B-A3B-GGUF"
    PRESET_F_MMPROJ_FILE="mmproj-Qwen_Qwen3.5-35B-A3B-f16.gguf"
    PRESET_F_TEMPLATE=""
    PRESET_F_REASONING="deepseek"

    PRESET_F_CTX=131072
    PRESET_F_BATCH=768
    PRESET_F_TEMP="0.7"
    PRESET_F_TOP_P="0.8"
    PRESET_F_TOP_K=20
    PRESET_F_MIN_P=0
    PRESET_F_REPEAT="1.1"
    PRESET_F_MIN_MEM=48
    PRESET_F_SPEED="29.6"
    PRESET_F_QUALITY="93.3% (A)"
    PRESET_F_VISION=1
    PRESET_F_SIZE="19.9 GB"
    PRESET_F_NOTES="RECOMMENDED for 64GB"

    # Preset G: Gemma 4 26B-A4B MoE (PENDING FIX)
    PRESET_G_LABEL="Gemma 4 26B-A4B Vision (MoE)"
    PRESET_G_MODEL="google_gemma-4-26B-A4B-it-Q4_K_M.gguf"
    PRESET_G_HF_REPO="bartowski/google_gemma-4-26B-A4B-it-GGUF"
    PRESET_G_HF_FILE="google_gemma-4-26B-A4B-it-Q4_K_M.gguf"
    PRESET_G_MMPROJ="mmproj-google_gemma-4-26B-A4B-it-f16.gguf"
    PRESET_G_MMPROJ_REPO="bartowski/google_gemma-4-26B-A4B-it-GGUF"
    PRESET_G_MMPROJ_FILE="mmproj-google_gemma-4-26B-A4B-it-f16.gguf"
    PRESET_G_TEMPLATE=""
    PRESET_G_REASONING="deepseek"

    PRESET_G_CTX=32768
    PRESET_G_BATCH=768
    PRESET_G_TEMP="1.0"
    PRESET_G_TOP_P="0.95"
    PRESET_G_TOP_K=64
    PRESET_G_MIN_P=0
    PRESET_G_REPEAT="1.0"
    PRESET_G_MIN_MEM=48
    PRESET_G_SPEED="32.2"
    PRESET_G_QUALITY="95.5% (A)"
    PRESET_G_VISION=1
    PRESET_G_SIZE="15.9 GB"
    PRESET_G_NOTES="PENDING: thinking leak"

    ALL_PRESETS="A A2 A3 B C D E F G"
}

# Get a preset variable by name: get_preset_var F LABEL -> value
get_preset_var() {
    local var="PRESET_${1}_${2}"
    echo "${!var}"
}

# =============================================================================
# Hardware detection
# =============================================================================

detect_hardware() {
    # Total memory
    TOTAL_MEM_KB=$(awk '/^MemTotal:/ {print $2}' /proc/meminfo)
    TOTAL_MEM_GB=$(( TOTAL_MEM_KB / 1024 / 1024 ))

    # Device model (Jetson identification)
    DEVICE_MODEL=""
    if [ -f /proc/device-tree/model ]; then
        DEVICE_MODEL=$(tr -d '\0' < /proc/device-tree/model)
    fi

    # GPU name
    GPU_NAME=""
    if command -v nvidia-smi &>/dev/null; then
        GPU_NAME=$(nvidia-smi --query-gpu=name --format=csv,noheader 2>/dev/null | head -1)
    fi

    # Determine hardware description
    if [ -n "$DEVICE_MODEL" ]; then
        HW_DESC="$DEVICE_MODEL"
    elif [ -n "$GPU_NAME" ]; then
        HW_DESC="GPU: $GPU_NAME"
    else
        HW_DESC="Unknown platform"
    fi

    # Determine recommendation based on memory
    if [ "$TOTAL_MEM_GB" -ge 48 ]; then
        RECOMMENDED_PRESET="F"
    elif [ "$TOTAL_MEM_GB" -ge 10 ]; then
        RECOMMENDED_PRESET="A"
    else
        RECOMMENDED_PRESET="A"
    fi
}

# =============================================================================
# Interactive preset menu
# =============================================================================

show_preset_menu() {
    echo ""
    echo -e "${BOLD}Hardware Detected${NC}"
    echo -e "  Platform: ${CYAN}${HW_DESC}${NC}"
    echo -e "  Memory:   ${CYAN}${TOTAL_MEM_GB} GB${NC}"
    echo ""
    echo -e "${BOLD}Available Model Presets${NC}"
    echo ""

    printf "  ${DIM}%-3s %-35s %9s %10s %8s  %-30s${NC}\n" \
        "" "Model" "Size" "Speed" "Vision" "Notes"
    echo -e "  ${DIM}$(printf '%.0s-' {1..100})${NC}"

    for p in $ALL_PRESETS; do
        local label=$(get_preset_var "$p" LABEL)
        local size=$(get_preset_var "$p" SIZE)
        local speed=$(get_preset_var "$p" SPEED)
        local vision=$(get_preset_var "$p" VISION)
        local notes=$(get_preset_var "$p" NOTES)
        local quality=$(get_preset_var "$p" QUALITY)
        local min_mem=$(get_preset_var "$p" MIN_MEM)

        local vision_str="no"
        [ "$vision" = "1" ] && vision_str="yes"

        local prefix="  "
        local suffix=""
        local color=""

        # Mark recommended
        if [ "$p" = "$RECOMMENDED_PRESET" ]; then
            prefix="* "
            color="${GREEN}"
        fi

        # Grey out if not enough memory
        if [ "$TOTAL_MEM_GB" -lt "$min_mem" ]; then
            color="${DIM}"
            suffix=" (needs ${min_mem}GB+)"
        fi

        printf "  ${color}${prefix}%s) %-33s %9s %8s tok/s %8s  %-30s${NC}\n" \
            "$p" "$label" "$size" "$speed" "$vision_str" "${notes}${suffix}"
    done

    echo ""
    echo -e "  ${GREEN}*${NC} = recommended for your hardware"
    echo ""

    # Read selection
    local default="$RECOMMENDED_PRESET"
    while true; do
        read -r -p "  Select preset [A-G] (Enter for $default): " choice
        choice="${choice:-$default}"
        choice="${choice^^}"  # uppercase

        # Validate
        if [[ " $ALL_PRESETS " == *" $choice "* ]]; then
            local min_mem=$(get_preset_var "$choice" MIN_MEM)
            if [ "$TOTAL_MEM_GB" -lt "$min_mem" ]; then
                echo ""
                warn "Preset $choice requires ${min_mem}GB+ memory (you have ${TOTAL_MEM_GB}GB)"
                read -r -p "  Continue anyway? [y/N]: " confirm
                if [[ ! "$confirm" =~ ^[Yy] ]]; then
                    continue
                fi
            fi
            SELECTED_PRESET="$choice"
            break
        else
            echo -e "  ${RED}Invalid selection. Choose A-G.${NC}"
        fi
    done

    echo ""
    log "Selected preset $SELECTED_PRESET: $(get_preset_var "$SELECTED_PRESET" LABEL)"
}

# =============================================================================
# Apply preset settings to globals
# =============================================================================

apply_preset() {
    local p="$1"

    SEL_MODEL=$(get_preset_var "$p" MODEL)
    SEL_HF_REPO=$(get_preset_var "$p" HF_REPO)
    SEL_HF_FILE=$(get_preset_var "$p" HF_FILE)
    SEL_MMPROJ=$(get_preset_var "$p" MMPROJ)
    SEL_MMPROJ_REPO=$(get_preset_var "$p" MMPROJ_REPO)
    SEL_MMPROJ_FILE=$(get_preset_var "$p" MMPROJ_FILE)
    SEL_TEMPLATE=$(get_preset_var "$p" TEMPLATE)
    SEL_REASONING=$(get_preset_var "$p" REASONING)

    SEL_CTX=$(get_preset_var "$p" CTX)
    SEL_BATCH=$(get_preset_var "$p" BATCH)
    SEL_TEMP=$(get_preset_var "$p" TEMP)
    SEL_TOP_P=$(get_preset_var "$p" TOP_P)
    SEL_TOP_K=$(get_preset_var "$p" TOP_K)
    SEL_MIN_P=$(get_preset_var "$p" MIN_P)
    SEL_REPEAT=$(get_preset_var "$p" REPEAT)
}

# =============================================================================
# Check model files and offer download
# =============================================================================

check_model_files() {
    local model_path="$DATA_DIR/models/$SEL_MODEL"
    local need_download=0

    echo ""

    # Check main model
    if [ -f "$model_path" ]; then
        log "Model found: $model_path"
    else
        warn "Model not found: $SEL_MODEL"
        need_download=1
    fi

    # Check mmproj if needed
    if [ -n "$SEL_MMPROJ" ]; then
        local mmproj_path="$DATA_DIR/models/$SEL_MMPROJ"
        if [ -f "$mmproj_path" ]; then
            log "Vision projector found: $mmproj_path"
        else
            warn "Vision projector not found: $SEL_MMPROJ"
            need_download=1
        fi
    fi

    if [ "$need_download" -eq 0 ]; then
        return 0
    fi

    echo ""

    # Check for hf CLI
    local hf_cmd=""
    if command -v hf &>/dev/null; then
        hf_cmd="hf"
    elif command -v huggingface-cli &>/dev/null; then
        hf_cmd="huggingface-cli"
    fi

    if [ -n "$hf_cmd" ]; then
        echo -e "  Download missing files now? This may take a while."
        local size=$(get_preset_var "$SELECTED_PRESET" SIZE)
        echo -e "  Model size: ${CYAN}${size}${NC}"
        echo ""
        read -r -p "  Download? [Y/n]: " confirm
        if [[ ! "$confirm" =~ ^[Nn] ]]; then
            echo ""

            # Download model if missing
            if [ ! -f "$DATA_DIR/models/$SEL_MODEL" ]; then
                log "Downloading model: $SEL_MODEL"
                $hf_cmd download "$SEL_HF_REPO" "$SEL_HF_FILE" \
                    --local-dir "$DATA_DIR/models/" || \
                    error "Model download failed. Check network and try again."
            fi

            # Download mmproj if missing
            if [ -n "$SEL_MMPROJ" ] && [ ! -f "$DATA_DIR/models/$SEL_MMPROJ" ]; then
                log "Downloading vision projector: $SEL_MMPROJ"
                $hf_cmd download "$SEL_MMPROJ_REPO" "$SEL_MMPROJ_FILE" \
                    --local-dir "$DATA_DIR/models/" || \
                    error "Vision projector download failed."
            fi

            log "Downloads complete"
            return 0
        fi
    fi

    # No hf CLI or user declined — print manual commands
    echo ""
    echo -e "  ${BOLD}Download the model files manually, then re-run this script:${NC}"
    echo ""
    if [ ! -f "$DATA_DIR/models/$SEL_MODEL" ]; then
        echo "    hf download $SEL_HF_REPO $SEL_HF_FILE \\"
        echo "      --local-dir $DATA_DIR/models/"
        echo ""
    fi
    if [ -n "$SEL_MMPROJ" ] && [ ! -f "$DATA_DIR/models/$SEL_MMPROJ" ]; then
        echo "    hf download $SEL_MMPROJ_REPO $SEL_MMPROJ_FILE \\"
        echo "      --local-dir $DATA_DIR/models/"
        echo ""
    fi
    if [ -z "$hf_cmd" ]; then
        echo -e "  ${DIM}(Install hf CLI: pip install huggingface_hub[cli])${NC}"
        echo ""
    fi
    exit 0
}

# =============================================================================
# Preserve existing settings on re-install
# =============================================================================

# Flags to track whether HOST/PORT were explicitly set on CLI
CLI_HOST_SET=false
CLI_PORT_SET=false

read_existing_config() {
    local conf_file="$CONFIG_DIR/llama-server.conf"
    if [ ! -f "$conf_file" ]; then
        return
    fi

    log "Detected existing configuration"

    # Read existing values
    local existing_host=$(grep -E '^HOST=' "$conf_file" | head -1 | cut -d= -f2)
    local existing_port=$(grep -E '^PORT=' "$conf_file" | head -1 | cut -d= -f2)

    # Preserve HOST if not explicitly overridden on CLI
    if [ "$CLI_HOST_SET" = false ] && [ -n "$existing_host" ] && [ "$existing_host" != "$HOST" ]; then
        if [ -t 0 ]; then
            warn "Existing HOST=$existing_host (default: $HOST)"
            read -r -p "  Keep existing HOST? [Y/n]: " confirm
            if [[ ! "$confirm" =~ ^[Nn] ]]; then
                HOST="$existing_host"
                log "Preserving HOST=$HOST"
            fi
        else
            HOST="$existing_host"
            log "Preserving existing HOST=$HOST"
        fi
    fi

    # Preserve PORT if not explicitly overridden on CLI
    if [ "$CLI_PORT_SET" = false ] && [ -n "$existing_port" ] && [ "$existing_port" != "$PORT" ]; then
        if [ -t 0 ]; then
            warn "Existing PORT=$existing_port (default: $PORT)"
            read -r -p "  Keep existing PORT? [Y/n]: " confirm
            if [[ ! "$confirm" =~ ^[Nn] ]]; then
                PORT="$existing_port"
                log "Preserving PORT=$PORT"
            fi
        else
            PORT="$existing_port"
            log "Preserving existing PORT=$PORT"
        fi
    fi
}

# =============================================================================
# Generate configuration file
# =============================================================================

generate_config() {
    local conf_file="$CONFIG_DIR/llama-server.conf"
    local preset_label=$(get_preset_var "$SELECTED_PRESET" LABEL)

    log "Generating configuration for Preset $SELECTED_PRESET: $preset_label"

    # Build MODEL path
    local model_val="$DATA_DIR/models/$SEL_MODEL"

    # Build TEMPLATE line (quoted path or empty)
    local template_line="TEMPLATE="
    if [ -n "$SEL_TEMPLATE" ]; then
        template_line="TEMPLATE=\"$DATA_DIR/templates/$SEL_TEMPLATE\""
    fi

    # Build MMPROJ line (quoted path or empty)
    local mmproj_line="MMPROJ="
    if [ -n "$SEL_MMPROJ" ]; then
        mmproj_line="MMPROJ=\"$DATA_DIR/models/$SEL_MMPROJ\""
    fi

    cat > "$conf_file" << CONF
# Llama.cpp Server Configuration for DAWN
# Generated by install.sh on $(date '+%Y-%m-%d %H:%M:%S')
# Preset $SELECTED_PRESET: $preset_label
#
# To change presets, re-run install.sh or edit this file manually.
# See the source llama-server.conf for full preset documentation.
#
# IMPORTANT: Set power mode to MAXN for best performance on AGX Orin 64GB:
#   sudo nvpmodel -m 0 && sudo jetson_clocks

# Model
MODEL="$model_val"
$template_line
REASONING_FORMAT=$SEL_REASONING
$mmproj_line

# Environment variables (Jetson-specific CUDA settings)
GGML_CUDA_NO_VMM=1
GGML_CUDA_FORCE_MMQ=1

# Server options
PORT=$PORT
HOST=$HOST
GPU_LAYERS=99

# Context and batch (critical for quality — do not reduce batch below 768)
CONTEXT_SIZE=$SEL_CTX
BATCH_SIZE=$SEL_BATCH
UNBATCH_SIZE=$SEL_BATCH

# Sampling parameters
THREADS=4
CACHE_TYPE_K=q8_0
CACHE_TYPE_V=q8_0
TEMPERATURE=$SEL_TEMP
TOP_P=$SEL_TOP_P
TOP_K=$SEL_TOP_K
MIN_P=$SEL_MIN_P
REPEAT_PENALTY=$SEL_REPEAT
CONF

    chmod 644 "$conf_file"
}

# =============================================================================
# Parse command line arguments
# =============================================================================

parse_args() {
    while [[ $# -gt 0 ]]; do
        case $1 in
            -m|--model)
                CLI_MODEL_PATH="$2"
                shift 2
                ;;
            -t|--template)
                CLI_TEMPLATE_PATH="$2"
                shift 2
                ;;
            --mmproj)
                CLI_MMPROJ_PATH="$2"
                shift 2
                ;;
            -P|--preset)
                CLI_PRESET="${2^^}"
                shift 2
                ;;
            -p|--port)
                PORT="$2"
                CLI_PORT_SET=true
                shift 2
                ;;
            --host)
                HOST="$2"
                CLI_HOST_SET=true
                shift 2
                ;;
            -h|--help)
                echo "Usage: $0 [options]"
                echo ""
                echo "Preset selection (recommended):"
                echo "  -P, --preset LETTER    Install a specific preset (A-G) non-interactively"
                echo "                         Without this flag, an interactive menu is shown"
                echo ""
                echo "Legacy mode (manual file paths):"
                echo "  -m, --model PATH       Path to model GGUF file"
                echo "  -t, --template PATH    Path to Jinja template file"
                echo "  --mmproj PATH          Path to multimodal projector GGUF (vision models)"
                echo ""
                echo "Server options:"
                echo "  -p, --port PORT        Server port (default: 8080)"
                echo "  --host HOST            Server bind address (default: 127.0.0.1)"
                echo "  -h, --help             Show this help message"
                echo ""
                echo "Presets:"
                echo "  A   Qwen3 4B Instruct        2.5 GB   Voice, no vision"
                echo "  A2  Qwen3.5 4B Vision        2.9 GB   Vision, small hardware"
                echo "  A3  Gemma 3 4B Vision         2.5 GB   Fastest 4B + vision"
                echo "  B   Qwen3 4B Thinking         2.5 GB   Reasoning mode"
                echo "  C   Gemma 3 12B Vision        7.3 GB   High quality + vision"
                echo "  D   Gemma 4 31B Vision       18.2 GB   WebUI only (64GB)"
                echo "  E   Qwen3.5 27B Vision       15.9 GB   WebUI only (64GB)"
                echo "  F   Qwen3.5 35B-A3B MoE      19.9 GB   Recommended for 64GB"
                echo "  G   Gemma 4 26B-A4B MoE      15.9 GB   Pending thinking fix"
                echo ""
                echo "Examples:"
                echo "  $0                     # Interactive preset selection"
                echo "  $0 -P F               # Install Preset F non-interactively"
                echo "  $0 -m /path/to/model.gguf  # Legacy manual mode"
                exit 0
                ;;
            *)
                error "Unknown option: $1 (use -h for help)"
                ;;
        esac
    done
}

# =============================================================================
# Legacy install mode (--model flag)
# =============================================================================

legacy_install() {
    log "Legacy mode: using provided file paths"

    # Model
    if [ ! -f "$CLI_MODEL_PATH" ]; then
        error "Model file not found: $CLI_MODEL_PATH"
    fi
    SEL_MODEL=$(basename "$CLI_MODEL_PATH")
    if [ ! -f "$DATA_DIR/models/$SEL_MODEL" ] || \
       [ "$CLI_MODEL_PATH" -nt "$DATA_DIR/models/$SEL_MODEL" ]; then
        log "Copying model file ($SEL_MODEL)"
        cp "$CLI_MODEL_PATH" "$DATA_DIR/models/$SEL_MODEL"
    else
        log "Model already in place: $SEL_MODEL"
    fi

    # Template
    SEL_TEMPLATE=""
    if [ -n "$CLI_TEMPLATE_PATH" ]; then
        if [ ! -f "$CLI_TEMPLATE_PATH" ]; then
            error "Template file not found: $CLI_TEMPLATE_PATH"
        fi
        SEL_TEMPLATE=$(basename "$CLI_TEMPLATE_PATH")
        cp "$CLI_TEMPLATE_PATH" "$DATA_DIR/templates/$SEL_TEMPLATE"
    fi

    # mmproj
    SEL_MMPROJ=""
    if [ -n "$CLI_MMPROJ_PATH" ]; then
        if [ ! -f "$CLI_MMPROJ_PATH" ]; then
            error "Multimodal projector not found: $CLI_MMPROJ_PATH"
        fi
        SEL_MMPROJ=$(basename "$CLI_MMPROJ_PATH")
        if [ ! -f "$DATA_DIR/models/$SEL_MMPROJ" ] || \
           [ "$CLI_MMPROJ_PATH" -nt "$DATA_DIR/models/$SEL_MMPROJ" ]; then
            log "Copying vision projector ($SEL_MMPROJ)"
            cp "$CLI_MMPROJ_PATH" "$DATA_DIR/models/$SEL_MMPROJ"
        else
            log "Vision projector already in place: $SEL_MMPROJ"
        fi
    fi

    # Defaults for legacy mode
    SEL_REASONING="none"
    SEL_CTX=8192
    SEL_BATCH=768
    SEL_TEMP="0.7"
    SEL_TOP_P="0.8"
    SEL_TOP_K=20
    SEL_MIN_P=0
    SEL_REPEAT="1.1"
    SELECTED_PRESET="custom"
}

# =============================================================================
# Main
# =============================================================================

# Parse args first (so --help works without root)
parse_args "$@"

# Must be root for everything else
if [ "$(id -u)" -ne 0 ]; then
    error "This script must be run as root"
fi

# Validate inputs
if [[ ! "$PORT" =~ ^[0-9]+$ ]] || [ "$PORT" -lt 1 ] || [ "$PORT" -gt 65535 ]; then
    error "Invalid port number: $PORT"
fi
if [[ ! "$HOST" =~ ^[0-9a-zA-Z.:]+$ ]]; then
    error "Invalid host address: $HOST"
fi

# Check llama-server is installed
if ! command -v llama-server &> /dev/null; then
    error "llama-server not found. Please install llama.cpp first."
fi

# Initialize preset definitions
define_presets

echo ""
echo -e "${BOLD}DAWN Llama.cpp Server Installer${NC}"
echo ""

# Determine install mode
if [ -n "$CLI_MODEL_PATH" ]; then
    # Legacy mode: --model provided
    legacy_install
elif [ -n "$CLI_PRESET" ]; then
    # Non-interactive preset mode
    if [[ ! " $ALL_PRESETS " == *" $CLI_PRESET "* ]]; then
        error "Invalid preset: $CLI_PRESET (valid: A A2 A3 B C D E F G)"
    fi
    SELECTED_PRESET="$CLI_PRESET"
    log "Preset $SELECTED_PRESET: $(get_preset_var "$SELECTED_PRESET" LABEL)"
    apply_preset "$SELECTED_PRESET"
elif [ -t 0 ]; then
    # Interactive mode (terminal attached)
    detect_hardware
    show_preset_menu
    apply_preset "$SELECTED_PRESET"
else
    # Non-interactive without preset — error
    error "No terminal detected. Use -P/--preset or -m/--model for non-interactive install."
fi

# --- Create directory structure ---
log "Creating directory structure"
mkdir -p "$DATA_DIR/models"
mkdir -p "$DATA_DIR/templates"
mkdir -p "$DATA_DIR/run"
mkdir -p "$CONFIG_DIR"
mkdir -p "/var/log/llama-cpp"

# --- Check/download model files (preset mode only) ---
if [ "$SELECTED_PRESET" != "custom" ]; then
    check_model_files
fi

# --- Install bundled templates ---
for tpl in "$SCRIPT_DIR"/*.jinja; do
    if [ -f "$tpl" ]; then
        cp "$tpl" "$DATA_DIR/templates/"
        log "Installed template: $(basename "$tpl")"
    fi
done

# --- Create service user ---
if ! id -u "$SERVICE_USER" &>/dev/null; then
    log "Creating service user: $SERVICE_USER"
    useradd --system --no-create-home --shell /usr/sbin/nologin "$SERVICE_USER"
    for group in video render; do
        if getent group $group >/dev/null; then
            usermod -a -G $group "$SERVICE_USER"
            log "Added $SERVICE_USER to $group group"
        fi
    done
else
    log "Service user $SERVICE_USER already exists"
    for group in video render; do
        if getent group $group >/dev/null && ! groups "$SERVICE_USER" | grep -q "\b$group\b"; then
            usermod -a -G $group "$SERVICE_USER"
            log "Added $SERVICE_USER to $group group"
        fi
    done
fi

# --- Set permissions (preserve custom permissions on re-install) ---
log "Setting permissions"
# Check for custom permissions on models dir before overwriting
MODELS_PERMS=$(stat -c '%a' "$DATA_DIR/models" 2>/dev/null)
chown -R "$SERVICE_USER:$SERVICE_USER" "$DATA_DIR"
chmod -R 755 "$DATA_DIR"
if [ -n "$MODELS_PERMS" ] && [ "$MODELS_PERMS" != "755" ]; then
    if [ -t 0 ]; then
        echo ""
        warn "Models directory had custom permissions: $MODELS_PERMS (reset to 755)"
        read -r -p "  Restore previous permissions ($MODELS_PERMS)? [Y/n]: " confirm
        if [[ ! "$confirm" =~ ^[Nn] ]]; then
            chmod "$MODELS_PERMS" "$DATA_DIR/models"
            log "Restored models directory permissions: $MODELS_PERMS"
        fi
    else
        chmod "$MODELS_PERMS" "$DATA_DIR/models"
        log "Preserved models directory permissions: $MODELS_PERMS"
    fi
fi
chown "$SERVICE_USER:$SERVICE_USER" "/var/log/llama-cpp"
chmod 755 "/var/log/llama-cpp"

# --- Library path ---
if [ ! -f /etc/ld.so.conf.d/llama-cpp.conf ]; then
    log "Configuring library path"
    echo "/usr/local/lib" > /etc/ld.so.conf.d/llama-cpp.conf
    ldconfig
fi

# --- Check existing config and preserve user settings ---
read_existing_config

# --- Generate configuration ---
generate_config

# --- Install systemd service and logrotate ---
log "Installing systemd service file"
cp "$SCRIPT_DIR/llama-server.service" /etc/systemd/system/
log "Installing logrotate configuration"
cp "$SCRIPT_DIR/llama-server" /etc/logrotate.d/
chmod 644 /etc/logrotate.d/llama-server

# --- Enable and start ---
log "Enabling and starting service"
systemctl daemon-reload
systemctl enable llama-server.service
systemctl restart llama-server.service

# --- Status report ---
sleep 2
echo ""
if systemctl is-active --quiet llama-server.service; then
    local_label="$SELECTED_PRESET"
    if [ "$SELECTED_PRESET" != "custom" ]; then
        local_label="Preset $SELECTED_PRESET: $(get_preset_var "$SELECTED_PRESET" LABEL)"
    fi

    echo -e "${GREEN}${BOLD}Service started successfully${NC}"
    echo ""
    log "Configuration: $local_label"
    log "Model: $DATA_DIR/models/$SEL_MODEL"
    if [ -n "$SEL_MMPROJ" ]; then
        log "Vision: $DATA_DIR/models/$SEL_MMPROJ"
    fi
    log "Server: http://$HOST:$PORT"
    echo ""
    log "Management commands:"
    log "  Status:   systemctl status llama-server"
    log "  Logs:     journalctl -u llama-server -f"
    log "  Restart:  systemctl restart llama-server"
    log "  Stop:     systemctl stop llama-server"
    echo ""
    log "Test:  curl http://$HOST:$PORT/health"
    echo ""

    # Power mode reminder for Jetson
    if [ -n "$DEVICE_MODEL" ] && echo "$DEVICE_MODEL" | grep -qi "jetson"; then
        warn "For best performance, ensure MAXN power mode:"
        warn "  sudo nvpmodel -m 0 && sudo jetson_clocks"
        echo ""
    fi
else
    warn "Service failed to start. Check logs:"
    warn "  journalctl -u llama-server -f"
    exit 1
fi
