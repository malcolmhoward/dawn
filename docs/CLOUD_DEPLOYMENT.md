# Cloud Deployment Guide

DAWN can run on a standard x86_64 Linux cloud server. All subsystems — WebUI, cloud LLM,
calendar, email, memory, contacts, documents, scheduler, MQTT — are network-first and work
without modification. Local audio capture (microphone/wake word) is the only feature that
requires physical hardware.

---

## Platform Independence

### CUDA — Optional

CUDA is only enabled when CMake detects a Jetson platform (`/etc/nv_tegra_release`). All other
platforms build with CPU-only inference automatically. No code changes needed.

### ARM NEON — Optional

`src/core/embedding_engine.c` uses NEON intrinsics for vectorized cosine similarity when
`__ARM_NEON` is defined. On x86_64, the scalar fallback path compiles automatically. No
performance-critical hot loop depends on NEON — embedding queries are infrequent.

### Platform Detection

`CMakeLists.txt` auto-detects platform via filesystem probes:
1. `/etc/nv_tegra_release` → Jetson
2. `/sys/firmware/devicetree/base/model` → Raspberry Pi
3. Otherwise → generic (no platform-specific flags set)

On x86_64 cloud servers, detection falls through cleanly. Optionally pass `-DPLATFORM=AUTO`.

### Argon2id Tuning

Password hashing uses 16MB memory cost on non-RPi platforms (8MB on RPi). Cloud servers have
plenty of RAM — no adjustment needed.

---

## ML Inference on CPU

All three ML components support CPU-only inference:

| Component | Library | GPU Path | CPU Path |
|-----------|---------|----------|----------|
| ASR (Whisper) | whisper.cpp / GGML | CUDA (Jetson) | CPU (automatic when `GGML_CUDA=OFF`) |
| TTS (Piper) | ONNX Runtime | CUDA EP | CPU EP (default on x86_64) |
| Embeddings | ONNX Runtime | CUDA EP | CPU EP (default on x86_64) |

CPU inference is slower but functional. For cloud deployments using `llm.type = "cloud"`, ASR
and TTS are only relevant if satellites connect to the server (Tier 2 audio streaming). If the
server is WebUI-only, ASR/TTS are unused at runtime.

---

## Required Dependencies

All packages are available in standard Ubuntu/Debian x86_64 repositories or as prebuilt
binaries.

### Core (always required)

```bash
# Build tools
sudo apt install cmake build-essential pkg-config

# Core libraries
sudo apt install libcurl4-openssl-dev libjson-c-dev libssl-dev libsqlite3-dev
sudo apt install libsodium-dev libwebsockets-dev libopus-dev libspdlog-dev

# MQTT
sudo apt install libmosquitto-dev

# Audio (libraries needed at link time even if no hardware present)
sudo apt install libasound2-dev libpulse-dev libflac-dev libsamplerate0-dev
```

### ML / Speech

```bash
# ONNX Runtime (CPU) — for Piper TTS and embedding engine
# Install from https://github.com/microsoft/onnxruntime/releases (x86_64 Linux CPU)
# Or: sudo apt install libonnxruntime-dev  (if packaged)

# Piper TTS dependencies
sudo apt install libespeak-ng-dev
# piper-phonemize: build from source or install prebuilt
# https://github.com/rhasspy/piper-phonemize

# Whisper.cpp — built as submodule, no separate install needed
```

### Optional

```bash
# Music playback codecs
sudo apt install libmpg123-dev libvorbis-dev

# Document processing (RAG)
sudo apt install libmupdf-dev libzip-dev libxml2-dev

# Calendar (CalDAV)
sudo apt install libical-dev
```

---

## MQTT Broker

MQTT is always required — there is no compile-time flag to disable it. The daemon initializes
MQTT at startup and uses it for device command routing.

For cloud deployments:

```bash
# Install and start Mosquitto
sudo apt install mosquitto mosquitto-clients
sudo systemctl enable mosquitto
sudo systemctl start mosquitto
```

Configure in `dawn.toml`:
```toml
[mqtt]
broker = "localhost"
port = 1883
```

If no home automation devices will connect, MQTT runs idle with negligible resource usage.

---

## Audio Subsystem

ALSA and PulseAudio are linked unconditionally at build time. The dev packages must be
installed even on headless servers.

At runtime, if no audio hardware exists:
- Audio capture thread fails to open the device and logs an error
- The daemon continues running — all other subsystems are unaffected
- WebUI, LLM, calendar, email, memory, scheduler all work normally

For WebUI-only deployments, local audio is irrelevant. Satellite devices (Tier 1 RPi, Tier 2
ESP32) connect over WebSocket and handle their own audio I/O.

---

## Build

```bash
# Clone with submodules (whisper.cpp)
git clone --recursive https://github.com/The-OASIS-Project/dawn.git
cd dawn

# Configure (CPU-only, no CUDA)
cmake --preset debug

# Build
make -C build-debug -j$(nproc)
```

The build should complete without errors on x86_64 Ubuntu 22.04+.

---

## Configuration

Minimal `dawn.toml` for a cloud WebUI server:

```toml
[general]
ai_name = "friday"
timezone = "America/New_York"

[llm]
type = "cloud"

[llm.cloud]
provider = "claude"           # or "openai"
model = "claude-sonnet-4-20250514"

[asr]
engine = "whisper"
model_path = "models/whisper.cpp/ggml-base.en.bin"

[tts]
voice_model = "models/en_US-lessac-medium.onnx"

[audio]
backend = "pulse"             # or "alsa" — required at compile time

[webui]
enabled = true
bind_address = "0.0.0.0"     # Listen on all interfaces
port = 3000
# For HTTPS (recommended for cloud):
# https_enabled = true
# ssl_cert_path = "/etc/letsencrypt/live/yourdomain/fullchain.pem"
# ssl_key_path = "/etc/letsencrypt/live/yourdomain/privkey.pem"

[mqtt]
broker = "localhost"
port = 1883

[network]
session_timeout_minutes = 30
```

API keys in `secrets.toml`:
```toml
claude_api_key = "sk-ant-..."
# openai_api_key = "sk-..."
```

---

## Run

```bash
LD_LIBRARY_PATH=/usr/local/lib ./build-debug/dawn --config dawn.toml
```

Access WebUI at `http://your-server:3000` (or `https://` if SSL configured).

---

## What Works on Cloud

| Feature | Status | Notes |
|---------|--------|-------|
| WebUI | Works | Full functionality |
| Cloud LLM (OpenAI, Claude, Gemini) | Works | Primary use case |
| Local LLM (Ollama) | Works | Install Ollama on same server |
| Calendar (CalDAV/Google) | Works | Network-only |
| Email (IMAP/SMTP/Gmail) | Works | Network-only |
| Memory / Contacts | Works | SQLite, local storage |
| Document Search (RAG) | Works | CPU embedding inference |
| Scheduler | Works | Timers, alarms, reminders |
| MQTT commands | Works | Install Mosquitto locally |
| OAuth 2.0 flows | Works | Needs valid redirect URL |
| Satellite connections (Tier 1) | Works | RPi satellites connect via WebSocket |
| Satellite connections (Tier 2) | Works | ESP32 satellites connect via WebSocket |
| ASR (Whisper) | Works | CPU inference, slower than GPU |
| TTS (Piper) | Works | CPU inference, slower than GPU |
| Local microphone / wake word | No | Requires audio hardware |
| CUDA acceleration | No | x86_64 uses CPU inference |

---

## x86_64 Compatibility Audit (2026-03-19)

Full codebase review for x86_64 cloud deployment. Findings are grouped by severity. All changes
should be made and verified on an x86_64 system.

### BLOCKER — Must fix before x86_64 will compile and run

| # | Domain | File(s) | Finding | Recommended Fix |
|---|--------|---------|---------|-----------------|
| 1 | Build | `CMakeLists.txt:274-279` | Hardcoded `aarch64-linux` in CUDA library paths — breaks `find_library()` on x86_64 | Use `CMAKE_SYSTEM_PROCESSOR` to select arch suffix, or use `find_library()` without hardcoded paths |
| 2 | Build | `CMakeLists.txt:710,714` | Hardcoded aarch64 harfbuzz include/lib paths (`/usr/lib/aarch64-linux-gnu/`) | Use `pkg_check_modules(HARFBUZZ harfbuzz)` or `find_package(harfbuzz)` |
| 3 | Build | `dawn_satellite/CMakeLists.txt:194-201` | Same hardcoded aarch64 harfbuzz paths in satellite build | Same fix as #2 |
| 4 | Audio | `dawn.c:1945-1951` | `init_audio_capture()` failure returns 1 (exit) — no audio device = daemon won't start | Add `--headless` flag or `[general] mode = "cloud"` that skips local audio init |
| 5 | Audio | `dawn.c` (speaker init) | Local speaker required at startup — same fatal exit pattern | Same headless mode flag |
| 6 | Audio | Audio backend | `AUDIO_ERR_NO_DEVICE` with no null/dummy fallback — no way to run without audio hardware | Add null audio backend that accepts but discards all audio operations |
| 7 | Config | `services/dawn-server/dawn.service` | Hardcoded aarch64 Tegra paths (`/usr/lib/aarch64-linux-gnu/tegra/`) in systemd unit | Create separate `dawn-server-x86_64.service` or template with arch variable |

### PROBLEM — Will cause issues but have workarounds

| # | Domain | File(s) | Finding | Recommended Fix |
|---|--------|---------|---------|-----------------|
| 8 | Build | `CMakeLists.txt` | CUDA linked unconditionally when detected — will error on x86 without CUDA toolkit installed | Guard CUDA block with `if(CMAKE_CUDA_COMPILER)` and add `-DENABLE_CUDA=OFF` option |
| 9 | Build | `CMakeLists.txt` | Unconditional ALSA/PulseAudio linking — dev packages always required even if audio unused | Make optional when headless mode is active (`-DENABLE_LOCAL_AUDIO=OFF`) |
| 10 | Audio | ASR init | ASR subsystem requires an audio capture device to initialize | Allow ASR to init without local capture — only needed for satellite/WebUI audio paths |
| 11 | Audio | Music player | Music playback requires local audio output device | Skip music player init in headless/cloud mode |
| 12 | Audio | `dawn.c` (boot greeting) | Boot greeting TTS blocks startup for ~10 seconds | Skip in headless/cloud mode |
| 13 | Config | `src/tools/llm_tools.c:361` | Hardcoded `/home/jetson/` path in vision tool image storage | Use runtime config path or `$HOME` expansion |
| 14 | Config | Various | Memory tuning (512KB thread stacks, buffer sizes) calibrated for Jetson, not cloud | Acceptable defaults — document cloud-recommended tuning values |

### MINOR — No action required

| # | Domain | File(s) | Finding | Status |
|---|--------|---------|---------|--------|
| 15 | Platform | `src/core/embedding_engine.c` | NEON intrinsics behind `#ifdef __ARM_NEON` with scalar fallback | Already portable — no fix needed |
| 16 | Platform | CUDA detection | Treats x86_64 as non-CUDA platform | Correct behavior for CPU-only cloud |
| 17 | Config | `dawn.toml` defaults | Worker pool (4), max_clients (4) conservative for cloud | Document cloud-recommended values |
| 18 | Config | Thread config | 512KB thread stacks, conservative timeouts | Fine for cloud — document if tuning needed |

### Recommended Fix Approach

Fixes fall into two logical work packages, both best done on an x86_64 system:

**Package A — Build system portability (Blockers 1-3, Problems 8-9):**
- Replace hardcoded `aarch64-linux-gnu` paths with `find_library()` / `pkg-config`
- Make CUDA fully optional via `-DENABLE_CUDA=OFF`
- Add `-DENABLE_LOCAL_AUDIO=OFF` to skip audio library requirements
- Estimated scope: CMakeLists.txt changes only

**Package B — Headless/cloud runtime mode (Blockers 4-7, Problems 10-13):**
- Add `[general] mode = "cloud"` config option (or `--headless` CLI flag)
- When active: skip local audio init (mic + speaker), skip boot greeting, skip music player
- Add null audio backend that gracefully handles calls without hardware
- WebUI and satellite audio paths (WebSocket-based) continue to work normally
- Fix hardcoded `/home/jetson/` path in llm_tools.c
- Create x86_64 systemd unit file
- Estimated scope: `dawn.c` startup logic, audio backend, config parsing, one llm_tools.c path

**Platform code (NEON, atomics, ONNX) is already portable — no work needed.**
