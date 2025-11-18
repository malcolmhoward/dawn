#!/bin/bash
################################################################################
# run_complete_benchmark.sh - Comprehensive ASR Benchmark Suite
#
# Benchmarks ALL engines and models:
#   - Vosk
#   - Whisper tiny
#   - Whisper base
#   - Whisper small
#
# Generates combined results and comparison statistics
################################################################################

set -e

# Colors for output
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
RED='\033[0;31m'
CYAN='\033[0;36m'
NC='\033[0m' # No Color

# Configuration
BENCHMARK_BIN="../build/tests/asr_benchmark"
RESULTS_DIR="results"
MASTER_CSV="${RESULTS_DIR}/complete_benchmark.csv"
MASTER_LOG="${RESULTS_DIR}/complete_benchmark.log"

VOSK_MODEL="../model"
WHISPER_TINY="../whisper.cpp/models/ggml-tiny.bin"
WHISPER_BASE="../whisper.cpp/models/ggml-base.bin"
WHISPER_SMALL="../whisper.cpp/models/ggml-small.bin"

# Check if benchmark binary exists
if [ ! -f "$BENCHMARK_BIN" ]; then
   echo -e "${RED}Error: Benchmark binary not found at $BENCHMARK_BIN${NC}"
   echo "Please build the project first:"
   echo "  cd ../build && cmake .. && make -j8"
   exit 1
fi

# Create results directory
mkdir -p "$RESULTS_DIR"

# Count WAV files
WAV_FILES=(test_*.wav)
NUM_FILES=${#WAV_FILES[@]}

if [ $NUM_FILES -eq 0 ]; then
   echo -e "${RED}Error: No test_*.wav files found in current directory${NC}"
   exit 1
fi

echo "================================================================================"
echo "DAWN Complete ASR Benchmark Suite"
echo "================================================================================"
echo -e "${GREEN}Found $NUM_FILES WAV file(s) to process${NC}"
echo ""
echo "Testing engines/models:"
echo "  1. Vosk (${VOSK_MODEL})"
echo "  2. Whisper tiny (${WHISPER_TINY})"
echo "  3. Whisper base (${WHISPER_BASE})"
echo "  4. Whisper small (${WHISPER_SMALL})"
echo ""
echo "Output: ${MASTER_CSV}"
echo ""

# Initialize master CSV with header
echo "wav_file,duration_sec,samples,sample_rate,engine,model,success,rtf,load_time_ms,transcription_time_ms,confidence,transcription" > "$MASTER_CSV"

# Initialize master log
{
   echo "================================================================================"
   echo "DAWN Complete ASR Benchmark"
   echo "Date: $(date)"
   echo "Files: $NUM_FILES"
   echo "================================================================================"
   echo ""
} > "$MASTER_LOG"

# Track overall progress
TOTAL_TESTS=$((NUM_FILES * 4))
CURRENT_TEST=0
TOTAL_FAILED=0

################################################################################
# Benchmark 1: Vosk
################################################################################
echo "================================================================================"
echo -e "${CYAN}[1/4] Benchmarking Vosk${NC}"
echo "================================================================================"

FAILED=0
for wav_file in "${WAV_FILES[@]}"; do
   [ -f "$wav_file" ] || continue

   CURRENT_TEST=$((CURRENT_TEST + 1))
   echo -e "${YELLOW}[${CURRENT_TEST}/${TOTAL_TESTS}]${NC} Vosk: $wav_file"

   if $BENCHMARK_BIN "$wav_file" \
      --engines vosk \
      --vosk-model "$VOSK_MODEL" \
      --csv >> "$MASTER_CSV" 2>> "$MASTER_LOG"; then
      echo -e "  ${GREEN}✓${NC}"
   else
      echo -e "  ${RED}✗ Failed${NC}"
      FAILED=$((FAILED + 1))
      TOTAL_FAILED=$((TOTAL_FAILED + 1))
   fi
done

echo "Vosk: $((NUM_FILES - FAILED))/$NUM_FILES successful"
echo ""

################################################################################
# Benchmark 2: Whisper tiny
################################################################################
echo "================================================================================"
echo -e "${CYAN}[2/4] Benchmarking Whisper tiny${NC}"
echo "================================================================================"

FAILED=0
for wav_file in "${WAV_FILES[@]}"; do
   [ -f "$wav_file" ] || continue

   CURRENT_TEST=$((CURRENT_TEST + 1))
   echo -e "${YELLOW}[${CURRENT_TEST}/${TOTAL_TESTS}]${NC} Whisper tiny: $wav_file"

   if $BENCHMARK_BIN "$wav_file" \
      --engines whisper \
      --whisper-model "$WHISPER_TINY" \
      --csv >> "$MASTER_CSV" 2>> "$MASTER_LOG"; then
      echo -e "  ${GREEN}✓${NC}"
   else
      echo -e "  ${RED}✗ Failed${NC}"
      FAILED=$((FAILED + 1))
      TOTAL_FAILED=$((TOTAL_FAILED + 1))
   fi
done

echo "Whisper tiny: $((NUM_FILES - FAILED))/$NUM_FILES successful"
echo ""

################################################################################
# Benchmark 3: Whisper base
################################################################################
echo "================================================================================"
echo -e "${CYAN}[3/4] Benchmarking Whisper base${NC}"
echo "================================================================================"

FAILED=0
for wav_file in "${WAV_FILES[@]}"; do
   [ -f "$wav_file" ] || continue

   CURRENT_TEST=$((CURRENT_TEST + 1))
   echo -e "${YELLOW}[${CURRENT_TEST}/${TOTAL_TESTS}]${NC} Whisper base: $wav_file"

   if $BENCHMARK_BIN "$wav_file" \
      --engines whisper \
      --whisper-model "$WHISPER_BASE" \
      --csv >> "$MASTER_CSV" 2>> "$MASTER_LOG"; then
      echo -e "  ${GREEN}✓${NC}"
   else
      echo -e "  ${RED}✗ Failed${NC}"
      FAILED=$((FAILED + 1))
      TOTAL_FAILED=$((TOTAL_FAILED + 1))
   fi
done

echo "Whisper base: $((NUM_FILES - FAILED))/$NUM_FILES successful"
echo ""

################################################################################
# Benchmark 4: Whisper small
################################################################################
echo "================================================================================"
echo -e "${CYAN}[4/4] Benchmarking Whisper small${NC}"
echo "================================================================================"

FAILED=0
for wav_file in "${WAV_FILES[@]}"; do
   [ -f "$wav_file" ] || continue

   CURRENT_TEST=$((CURRENT_TEST + 1))
   echo -e "${YELLOW}[${CURRENT_TEST}/${TOTAL_TESTS}]${NC} Whisper small: $wav_file"

   if $BENCHMARK_BIN "$wav_file" \
      --engines whisper \
      --whisper-model "$WHISPER_SMALL" \
      --csv >> "$MASTER_CSV" 2>> "$MASTER_LOG"; then
      echo -e "  ${GREEN}✓${NC}"
   else
      echo -e "  ${RED}✗ Failed${NC}"
      FAILED=$((FAILED + 1))
      TOTAL_FAILED=$((TOTAL_FAILED + 1))
   fi
done

echo "Whisper small: $((NUM_FILES - FAILED))/$NUM_FILES successful"
echo ""

################################################################################
# Generate Summary Statistics
################################################################################
echo "================================================================================"
echo "Generating Summary Statistics"
echo "================================================================================"

if command -v python3 &> /dev/null; then
   python3 - << 'PYTHON_SCRIPT'
import csv
import sys
from collections import defaultdict

master_csv = 'results/complete_benchmark.csv'

print("\n" + "="*80)
print("COMPLETE ASR BENCHMARK SUMMARY")
print("="*80)

try:
    with open(master_csv, 'r') as f:
        reader = csv.DictReader(f)
        data = list(reader)

    if not data:
        print("No data in CSV file")
        sys.exit(0)

    # Group by engine (Vosk vs Whisper) and model
    engines = defaultdict(list)

    for row in data:
        if row['success'] == '1':
            engine_key = row['engine']
            if engine_key == 'Whisper':
                # Extract model name from path
                model_path = row['model']
                if 'tiny' in model_path:
                    engine_key = 'Whisper tiny'
                elif 'base' in model_path:
                    engine_key = 'Whisper base'
                elif 'small' in model_path:
                    engine_key = 'Whisper small'

            engines[engine_key].append({
                'rtf': float(row['rtf']),
                'load_time_ms': float(row['load_time_ms']),
                'transcription_time_ms': float(row['transcription_time_ms']),
                'confidence': float(row['confidence']) if row['confidence'] != '-1.0' else None
            })

    # Sort by average RTF for display
    engine_order = sorted(engines.items(), key=lambda x: sum(r['rtf'] for r in x[1])/len(x[1]))

    for engine_name, results in engine_order:
        if not results:
            continue

        rtfs = [r['rtf'] for r in results]
        load_times = [r['load_time_ms'] for r in results]
        trans_times = [r['transcription_time_ms'] for r in results]
        confs = [r['confidence'] for r in results if r['confidence'] is not None]

        avg_rtf = sum(rtfs)/len(rtfs)

        # Determine if realtime capable
        realtime_status = "✓ REALTIME" if avg_rtf < 1.0 else "✗ NOT REALTIME"

        print(f"\n{engine_name.upper()}:")
        print("-" * 80)
        print(f"  Samples:              {len(results)}")
        print(f"  Avg Load Time:        {sum(load_times)/len(load_times):.1f} ms")
        print(f"  Avg Trans Time:       {sum(trans_times)/len(trans_times):.1f} ms")
        print(f"  Avg RTF:              {avg_rtf:.3f} {realtime_status}")
        print(f"  Min RTF:              {min(rtfs):.3f}")
        print(f"  Max RTF:              {max(rtfs):.3f}")
        if confs:
            print(f"  Avg Confidence:       {sum(confs)/len(confs):.2f}")
        else:
            print(f"  Avg Confidence:       N/A")

    print("\n" + "="*80)
    print("\nRealtime Performance Ranking (lower RTF = better):")
    print("-" * 80)
    for i, (engine_name, results) in enumerate(engine_order, 1):
        rtfs = [r['rtf'] for r in results]
        avg_rtf = sum(rtfs)/len(rtfs)
        speedup = 1.0 / avg_rtf if avg_rtf > 0 else 0
        print(f"  {i}. {engine_name:20s} RTF: {avg_rtf:.3f} ({speedup:.1f}x faster than realtime)")

    print("\n" + "="*80)
    print(f"\nDetailed results: {master_csv}")
    print("="*80 + "\n")

except FileNotFoundError:
    print(f"Error: {master_csv} not found")
except Exception as e:
    print(f"Error generating statistics: {e}")
PYTHON_SCRIPT
else
   echo "Python3 not available - skipping summary statistics"
   echo "Install python3 for automatic stats: sudo apt-get install python3"
fi

echo ""
echo "================================================================================"
echo "Benchmark Complete!"
echo "================================================================================"
echo "Total tests run:    ${TOTAL_TESTS}"
echo "Total successful:   $((TOTAL_TESTS - TOTAL_FAILED))"
echo "Total failed:       ${TOTAL_FAILED}"
echo ""
echo "Results saved to:"
echo "  CSV:  ${MASTER_CSV}"
echo "  Log:  ${MASTER_LOG}"
echo ""
echo "Next steps:"
echo "  1. Review results/complete_benchmark.csv"
echo "  2. Analyze transcription accuracy across all engines"
echo "  3. Compare RTF (Real-Time Factor) values"
echo "  4. Make production engine selection decision"
echo ""
