#!/bin/bash
#
# Smoke test script for Dawn build modes
# Builds all presets and verifies each binary starts successfully
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
cd "$PROJECT_ROOT"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Build presets to test
# Format: "preset_name[:extra_cmake_args]"
#   preset_name    - CMake preset (local, full, debug)
#   extra_args     - Additional -D flags (optional, colon-separated from preset)
PRESETS=(
    "local"
    "full"
    "debug"
    "debug:-DDAWN_ENABLE_EMAIL_TOOL=ON"
)

# Track results
declare -A BUILD_RESULTS
declare -A RUN_RESULTS
FAILED=0

log_info() {
    echo -e "${GREEN}[INFO]${NC} $1"
}

log_warn() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

log_header() {
    echo ""
    echo "=============================================="
    echo " $1"
    echo "=============================================="
}

# Parse a preset entry into its components
# "debug:-DFOO=ON" -> preset=debug, extra_args="-DFOO=ON", label="debug+FOO"
parse_preset_entry() {
    local entry=$1
    PRESET_NAME="${entry%%:*}"
    if [[ "$entry" == *:* ]]; then
        PRESET_EXTRA="${entry#*:}"
    else
        PRESET_EXTRA=""
    fi
}

# Get a human-readable label for a preset entry
get_label() {
    local entry=$1
    parse_preset_entry "$entry"
    if [[ -n "$PRESET_EXTRA" ]]; then
        # Extract option names: "-DDAWN_ENABLE_EMAIL_TOOL=ON" -> "email"
        local suffix=$(echo "$PRESET_EXTRA" | sed 's/-DDAWN_ENABLE_//g; s/_TOOL=ON//g' | tr '[:upper:]' '[:lower:]' | tr ' ' '+')
        echo "${PRESET_NAME}+${suffix}"
    else
        echo "$PRESET_NAME"
    fi
}

# Map preset name to build directory
get_build_dir() {
    local entry=$1
    local label=$(get_label "$entry")
    parse_preset_entry "$entry"
    case "$PRESET_NAME" in
        local) echo "build-local" ;;
        full)  echo "build-full" ;;
        debug)
            if [[ -n "$PRESET_EXTRA" ]]; then
                echo "build-debug-${label#debug+}"
            else
                echo "build-debug"
            fi
            ;;
        *)     echo "build-${label}" ;;
    esac
}

# Build a preset
build_preset() {
    local entry=$1
    local label=$(get_label "$entry")
    parse_preset_entry "$entry"
    local build_dir=$(get_build_dir "$entry")

    log_info "Configuring $label..."
    if [[ -n "$PRESET_EXTRA" ]]; then
        # Variant build: use -B to place in a separate build dir so we don't
        # clobber the base preset's build directory
        if ! cmake --preset "$PRESET_NAME" -B "$build_dir" $PRESET_EXTRA > /tmp/cmake_${label}.log 2>&1; then
            log_error "CMake configure failed for $label"
            cat /tmp/cmake_${label}.log
            return 1
        fi
    else
        if ! cmake --preset "$PRESET_NAME" > /tmp/cmake_${label}.log 2>&1; then
            log_error "CMake configure failed for $label"
            cat /tmp/cmake_${label}.log
            return 1
        fi
    fi

    log_info "Building $label..."
    if ! make -C "$build_dir" -j$(nproc) > /tmp/make_${label}.log 2>&1; then
        log_error "Build failed for $label"
        tail -50 /tmp/make_${label}.log
        return 1
    fi

    log_info "Build successful: $build_dir/dawn"
    return 0
}

# Test if binary starts and reaches "Listening..."
test_run() {
    local entry=$1
    local label=$(get_label "$entry")
    local build_dir=$(get_build_dir "$entry")
    local binary="$build_dir/dawn"
    local log_file="/tmp/dawn_${label}_run.log"

    if [[ ! -x "$binary" ]]; then
        log_error "Binary not found: $binary"
        return 1
    fi

    log_info "Testing $label startup..."

    # Run with timeout, capture output
    export LD_LIBRARY_PATH=/usr/local/lib
    timeout 8 "$binary" > "$log_file" 2>&1 || true

    # Check for successful startup indicators
    if grep -q "Listening\.\.\." "$log_file"; then
        log_info "$label: Reached 'Listening...' state"
        return 0
    elif grep -q "ERR.*Failed" "$log_file"; then
        log_error "$label: Startup failed"
        grep "ERR" "$log_file" | head -5
        return 1
    else
        log_warn "$label: Did not reach 'Listening...' state"
        tail -10 "$log_file"
        return 1
    fi
}

# Main
log_header "Dawn Smoke Test"
echo "Testing ${#PRESETS[@]} build presets..."
echo "Project root: $PROJECT_ROOT"

# Option to skip builds
SKIP_BUILD=0
if [[ "$1" == "--skip-build" ]]; then
    SKIP_BUILD=1
    log_info "Skipping builds (--skip-build)"
fi

# Collect labels for results tracking
LABELS=()
for entry in "${PRESETS[@]}"; do
    LABELS+=("$(get_label "$entry")")
done

# Build phase
if [[ $SKIP_BUILD -eq 0 ]]; then
    log_header "Build Phase"
    for i in "${!PRESETS[@]}"; do
        echo ""
        local_label="${LABELS[$i]}"
        if build_preset "${PRESETS[$i]}"; then
            BUILD_RESULTS[$local_label]="PASS"
        else
            BUILD_RESULTS[$local_label]="FAIL"
            FAILED=1
        fi
    done
fi

# Run phase
log_header "Run Phase"
for i in "${!PRESETS[@]}"; do
    echo ""
    local_label="${LABELS[$i]}"
    if test_run "${PRESETS[$i]}"; then
        RUN_RESULTS[$local_label]="PASS"
    else
        RUN_RESULTS[$local_label]="FAIL"
        FAILED=1
    fi
done

# Summary
log_header "Results Summary"
printf "%-20s %-10s %-10s\n" "Preset" "Build" "Run"
printf "%-20s %-10s %-10s\n" "-------" "-----" "---"
for label in "${LABELS[@]}"; do
    build_status="${BUILD_RESULTS[$label]:-SKIP}"
    run_status="${RUN_RESULTS[$label]:-SKIP}"

    # Color the results
    if [[ "$build_status" == "PASS" ]]; then
        build_colored="${GREEN}PASS${NC}"
    elif [[ "$build_status" == "FAIL" ]]; then
        build_colored="${RED}FAIL${NC}"
    else
        build_colored="${YELLOW}SKIP${NC}"
    fi

    if [[ "$run_status" == "PASS" ]]; then
        run_colored="${GREEN}PASS${NC}"
    elif [[ "$run_status" == "FAIL" ]]; then
        run_colored="${RED}FAIL${NC}"
    else
        run_colored="${YELLOW}SKIP${NC}"
    fi

    printf "%-20s " "$label"
    echo -e "$build_colored       $run_colored"
done

echo ""
if [[ $FAILED -eq 0 ]]; then
    log_info "All tests passed!"
    exit 0
else
    log_error "Some tests failed!"
    exit 1
fi
