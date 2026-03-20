# Getting Started — Server Mode (x86_64)

Step-by-step guide to building and running DAWN on an x86_64 Linux system in server mode.
Server mode provides WebUI + satellite connections without local audio hardware.

Tested on Ubuntu 24.04 LTS (x86_64) with NVIDIA GPU (optional).

---

## 1. System Dependencies

### Build tools

```bash
sudo apt install cmake build-essential pkg-config
```

### Core libraries

```bash
sudo apt install libcurl4-openssl-dev libjson-c-dev libssl-dev libsqlite3-dev \
   libsodium-dev libspdlog-dev libwebsockets-dev libopus-dev \
   libmosquitto-dev libflac-dev libsamplerate0-dev uuid-dev libncurses-dev
```

### Audio (required at link time even without local hardware)

```bash
sudo apt install libasound2-dev libpulse-dev
```

### Optional libraries

```bash
# Music playback codecs
sudo apt install libmpg123-dev libvorbis-dev

# Document processing (RAG — PDF and DOCX support)
sudo apt install libmupdf-dev libzip-dev libxml2-dev

# MuPDF transitive dependencies (static linking)
sudo apt install libmujs-dev libgumbo-dev libopenjp2-7-dev libjbig2dec0-dev

# Calendar (CalDAV)
sudo apt install libical-dev
```

### CUDA (optional — for GPU-accelerated Whisper ASR)

```bash
sudo apt install nvidia-cuda-toolkit
```

CUDA is auto-detected at configure time. If the CUDA toolkit is installed and `nvidia-smi`
shows a working GPU, Whisper will use GPU inference automatically. Without CUDA, Whisper
falls back to CPU (slower but functional).

---

## 2. ONNX Runtime

ONNX Runtime is required for TTS (Piper), VAD (Silero), and the embedding engine.
Install the prebuilt x86_64 CPU package:

```bash
wget https://github.com/microsoft/onnxruntime/releases/download/v1.22.0/onnxruntime-linux-x64-1.22.0.tgz
tar xzf onnxruntime-linux-x64-1.22.0.tgz

# Install headers (note: -r is required for the core/ subdirectory)
sudo cp -r onnxruntime-linux-x64-1.22.0/include/* /usr/local/include/
sudo cp -a onnxruntime-linux-x64-1.22.0/lib/libonnxruntime.so* /usr/local/lib/
sudo ldconfig
```

---

## 3. espeak-ng (rhasspy fork)

Piper TTS requires the rhasspy fork of espeak-ng, which adds the
`espeak_TextToPhonemesWithTerminator` symbol not present in the system package.

**Important:** Purge the system espeak-ng first to avoid library conflicts:

```bash
sudo apt purge libespeak-ng1 libespeak-ng-libespeak1 espeak-ng-data
```

Install build dependencies and build from source:

```bash
sudo apt install autotools-dev automake autoconf libtool

git clone https://github.com/rhasspy/espeak-ng.git
cd espeak-ng
./autogen.sh && ./configure --prefix=/usr
make -j$(nproc)
sudo make LIBDIR=/usr/lib/$(dpkg-architecture -qDEB_HOST_MULTIARCH) install
sudo ldconfig
cd ..
```

Verify the rhasspy symbol is present:

```bash
nm -D /usr/lib/$(dpkg-architecture -qDEB_HOST_MULTIARCH)/libespeak-ng.so | grep TextToPhonemesWithTerminator
```

---

## 4. piper-phonemize

```bash
git clone https://github.com/rhasspy/piper-phonemize.git
cd piper-phonemize
mkdir build && cd build
cmake .. -DONNXRUNTIME_DIR=/usr/local -DESPEAK_NG_DIR=/usr
make -j$(nproc)
```

**Note:** The `libpiper_phonemize.so` library builds successfully. The test executable and
CLI may fail to link — this is expected and does not affect the library.

Install the library and headers:

```bash
sudo cp -a libpiper_phonemize.so* /usr/local/lib/
sudo mkdir -p /usr/local/include/piper-phonemize
sudo cp ../src/*.hpp /usr/local/include/piper-phonemize/
sudo cp ../src/uni_algo.h /usr/local/include/piper-phonemize/
sudo ldconfig
cd ../..
```

---

## 5. MQTT Broker

MQTT is required — the daemon initializes it at startup for device command routing.

```bash
sudo apt install mosquitto mosquitto-clients
sudo systemctl enable mosquitto
sudo systemctl start mosquitto
```

If no home automation devices will connect, MQTT runs idle with negligible resource usage.

---

## 6. Clone and Build

```bash
git clone --recursive https://github.com/The-OASIS-Project/dawn.git
cd dawn
```

### Code formatting (required for commits)

```bash
# Formatter versions must match project standard
sudo apt install clang-format-14
npm install  # For Prettier (JS/CSS/HTML formatting)
```

### Configure and build

```bash
# Debug build (recommended for initial setup)
cmake --preset debug -DENABLE_AEC=OFF

# Build
make -C build-debug -j$(nproc)
```

For a server-only build that doesn't require audio libraries:

```bash
cmake --preset server-debug
make -C build-server-debug -j$(nproc)
```

### Available presets

| Preset | Description |
|--------|-------------|
| `debug` | Full build with debug symbols (all features) |
| `default` | Full release build |
| `server` | Server-only release (no local audio required) |
| `server-debug` | Server-only with debug symbols |
| `local` | Local-only (no WebUI) |
| `full` | Full release build |

---

## 7. Download Models

```bash
./setup_models.sh
```

This downloads:
- Whisper ASR model (`base.en`, ~142MB)
- Embedding model for semantic memory search (~23MB)
- Creates symlinks in all build directories

TTS (Piper) and VAD (Silero) models are committed to git — no download needed.

---

## 8. SSL Certificates

For HTTPS (recommended — required for browser microphone access):

```bash
./generate_ssl_cert.sh
```

This creates a self-signed CA and server certificate in `ssl/`. Configure in `dawn.toml`:

```toml
[webui]
https = true
ssl_cert_path = "ssl/dawn-chain.crt"
ssl_key_path = "ssl/dawn.key"
```

For browser access, you'll need to accept the self-signed certificate or install the CA
(`ssl/ca.crt`) in your browser's trust store.

---

## 9. Configuration

### secrets.toml

Create `secrets.toml` in the project root with your API keys:

```toml
claude_api_key = "sk-ant-..."
# openai_api_key = "sk-..."
# gemini_api_key = "..."
```

The cloud LLM provider is auto-detected from available keys (Claude > OpenAI > Gemini).
You can override with `[llm.cloud] provider = "claude"` in `dawn.toml`.

### dawn.toml (optional)

DAWN works with sensible defaults. A `dawn.toml` is only needed to customize settings.
Key settings for server mode:

```toml
[general]
ai_name = "friday"
mode = "server"       # Equivalent to --server CLI flag

[webui]
enabled = true        # Default: true
bind_address = "0.0.0.0"
port = 3000
https = true
ssl_cert_path = "ssl/dawn-chain.crt"
ssl_key_path = "ssl/dawn.key"

[llm]
type = "cloud"

[mqtt]
enabled = true
broker = "127.0.0.1"
port = 1883
```

---

## 10. Run

```bash
# Server mode via CLI flag (no dawn.toml mode setting needed)
LD_LIBRARY_PATH=/usr/local/lib ./build-debug/dawn --server

# Or if mode = "server" is set in dawn.toml
LD_LIBRARY_PATH=/usr/local/lib ./build-debug/dawn
```

Access WebUI at `https://your-server:3000`.

On first access, you'll be prompted to create an admin account.

### What server mode skips

- Local microphone capture and speaker playback
- AEC (acoustic echo cancellation)
- VAD (voice activity detection for local mic)
- Boot greeting TTS
- Music player and audio decoder initialization

### What server mode keeps

- TTS engine (generates audio for WebUI and satellite clients via WebSocket)
- ASR engine (transcribes audio from WebUI and satellite clients)
- WebUI (full functionality including voice chat)
- All network services: LLM, calendar, email, memory, scheduler, MQTT
- Satellite connections (Tier 1 RPi, Tier 2 ESP32)

---

## Troubleshooting

### WebUI not accessible

- Check `[webui] enabled = true` in dawn.toml (default is true)
- If using HTTPS, ensure `ssl_cert_path` and `ssl_key_path` are set
- For browser mic access, HTTPS is required (or use `localhost`)

### No TTS audio in browser

- Click the TTS toggle button (speaker icon) to enable
- TTS state persists in browser localStorage but must be synced to server on each connection

### Cloud LLM not working

- Check `secrets.toml` has the correct API key
- Provider is auto-detected; if `dawn.toml` specifies a provider without a key, it falls
  back to the first available provider
- Check server logs for `Cloud provider auto-detected: Claude` (or similar)

### Whisper slow (high RTF)

- Without CUDA: CPU inference is expected to be 2-10x realtime
- With CUDA: ensure `nvidia-smi` works and shows your GPU
- Check server logs for `gpu: yes` in the Whisper init line
- A clean rebuild is needed after installing CUDA drivers

### PulseAudio warnings at startup

If you see `libpulse` warnings on stderr, these are harmless — PulseAudio's library
constructor attempts to connect to a daemon that may not be running on a headless server.

### Ctrl+C slow to exit

- If Whisper is mid-transcription on CPU, the process waits for it to finish
- With CUDA, transcription is fast enough that shutdown is near-instant
