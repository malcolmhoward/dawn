# ASR Baseline Testing - Quick Start

## Official Test Results Summary (50 Samples - November 2025)

**Winner: Whisper Base** - Selected as DAWN's default ASR engine

### Performance Rankings

| Rank | Engine | RTF | Speed | Load Time | Realtime? | Accuracy |
|------|--------|-----|-------|-----------|-----------|----------|
| 🥇 1 | **Whisper base** | 0.365 | 2.7x faster | 187ms | ✅ Yes | ⭐⭐⭐⭐⭐ Excellent |
| 🥈 2 | Whisper tiny | 0.179 | 5.6x faster | 146ms | ✅ Yes | ⭐⭐⭐ Good (wake word failure) |
| 🥉 3 | Vosk | 0.370 | 2.7x faster | 14,979ms | ✅ Yes | ⭐⭐⭐⭐ Very Good |
| ❌ 4 | Whisper small | 1.245 | 0.8x | 334ms | ❌ No | ⭐⭐⭐⭐⭐ Excellent |

### Key Findings

**Whisper Base Selected Because:**
- Best balance of speed (2.7x realtime) and accuracy
- Fast startup (187ms vs Vosk's 15 seconds)
- Proper punctuation/capitalization improves LLM processing
- Only 1 error found across 50 diverse samples
- No critical wake word failures

**Why Not Others:**
- **Whisper Tiny:** Wake word failure in test_049 ("For today" vs "Friday") - disqualifying
- **Vosk:** 15-second startup creates poor UX, lowercase-only output
- **Whisper Small:** Not realtime (RTF 1.245) without GPU acceleration

### Detailed Statistics

**Whisper Base (PRODUCTION CHOICE):**
- Samples: 50
- Avg Load Time: 187.0ms
- Avg Transcription Time: 2,572.2ms (per 5-second utterance)
- Avg RTF: 0.365 (processes 5 seconds of audio in ~1.8 seconds)
- RTF Range: 0.175 - 0.545
- Known Issues: 1 error in test_004 ("or you" instead of "are you")

For complete benchmark data, see `results/complete_benchmark.csv`

---

# ASR Baseline Testing - Quick Start

## Setup (One-time)

```bash
cd /home/jetson/code/The-OASIS-Project/dawn/test_recordings
```

## 1. Recording Phase

### Quick Recording Commands

```bash
# Short utterances (5 seconds)
arecord -f S16_LE -c 1 -r 16000 -d 5 test_001.wav

# Medium utterances (10 seconds)
arecord -f S16_LE -c 1 -r 16000 -d 10 test_026.wav

# Long utterances (15 seconds)
arecord -f S16_LE -c 1 -r 16000 -d 15 test_041.wav
```

### Recording Tips

- **Press Ctrl+C** to stop early if you finish before the timer
- **Preview:** `aplay test_XXX.wav` to verify recording
- **Re-record:** Just run the same command again to overwrite
- **Refer to:** `recording_guide.txt` for the full test script

### Recommended Recording Workflow

```bash
# Record a batch (e.g., tests 1-10)
for i in {001..010}; do
    echo "=== Recording test_${i}.wav ==="
    cat recording_guide.txt | grep "test_${i}.wav"
    echo "Press Enter when ready..."
    read
    arecord -f S16_LE -c 1 -r 16000 -d 5 test_${i}.wav
done
```

## 2. Verification Phase

```bash
# List all recordings
ls -lh test_*.wav

# Play a specific recording
aplay test_001.wav

# Check file info
file test_001.wav
# Should show: RIFF (little-endian) data, WAVE audio, Microsoft PCM, 16 bit, mono 16000 Hz
```

## 3. Benchmark Phase

```bash
# Run complete benchmark suite (all 4 ASR engines: Vosk, Whisper tiny/base/small)
./run_complete_benchmark.sh

# The script will:
# - Process all test_*.wav files
# - Test all 4 ASR engines on each sample
# - Save results to results/complete_benchmark.csv
# - Generate comprehensive statistics
# - Takes approximately 25 minutes for 50 samples
```

## 4. Review Results

```bash
# View CSV in terminal (requires 'column' command)
column -t -s ',' results/complete_benchmark.csv | less -S

# View summary log
less results/complete_benchmark.log

# Open in spreadsheet (if you have GUI access)
libreoffice results/complete_benchmark.csv
# or copy to your desktop and open with Excel/Google Sheets
```

## Expected Output

### CSV Columns
- `wav_file` - Input filename
- `duration_sec` - Audio duration
- `samples` - Number of audio samples
- `sample_rate` - Sample rate (16000)
- `engine` - ASR engine (Vosk/Whisper)
- `model` - Model path
- `success` - 1 if successful, 0 if failed
- `rtf` - Real-Time Factor (lower is better, <1.0 is realtime)
- `time_ms` - Processing time in milliseconds
- `confidence` - Transcription confidence (0.0-1.0, or -1.0 if N/A)
- `transcription` - Actual transcribed text

### Key Metrics to Compare

1. **Accuracy:** How often does each engine transcribe correctly?
2. **RTF:** Is the engine fast enough for real-time use?
3. **Confidence:** Does the confidence score correlate with accuracy?
4. **Failure Rate:** How many samples failed to process?

## Troubleshooting

### No sound from microphone
```bash
# List audio devices
arecord -l

# Test microphone
arecord -f S16_LE -c 1 -r 16000 -d 5 test_mic.wav
aplay test_mic.wav
```

### Benchmark fails
```bash
# Verify benchmark binary exists
ls -lh ../build/asr_benchmark

# If not found, rebuild:
cd ../build && make asr_benchmark
```

### Wrong audio format
```bash
# Check file format
file test_001.wav

# Should be: 16-bit mono 16000 Hz
# If not, re-record with correct arecord parameters
```

## Progress Tracking

Record your progress:
- [ ] Tests 001-010: Wake word detection (10 files)
- [ ] Tests 011-025: Short commands (15 files)
- [ ] Tests 026-040: Medium commands (15 files)
- [ ] Tests 041-050: Long complex utterances (10 files)
- [ ] Tests 051-055: Challenging scenarios (5 files, optional)
- [ ] Run benchmarks
- [ ] Review results
- [ ] Document findings

Total: **50 required**, 55 optional
