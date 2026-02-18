# DAP2 Satellite System

This document describes the DAP2 (Dawn Audio Protocol 2.0) satellite architecture for distributed voice assistants - the "JARVIS in every room" vision.

## Quick Start (Raspberry Pi)

```bash
# 1. Install dependencies
sudo apt update
sudo apt install -y build-essential cmake pkg-config \
  libasound2-dev libwebsockets-dev libjson-c-dev git

# 2. Clone and build
git clone https://github.com/YOUR_REPO/dawn.git
cd dawn/dawn_satellite
mkdir build && cd build
cmake .. -DENABLE_DAP2=ON -DENABLE_NEOPIXEL=OFF -DCMAKE_BUILD_TYPE=Release
make -j2  # Use -j2 on Pi Zero 2 W (512MB RAM), -j4 on Pi 3/4/5

# 3. Install
sudo make install

# 4. Configure (set your daemon IP)
sudo nano /etc/dawn/satellite.toml
# Edit: host = "192.168.1.100"  (your DAWN daemon IP)
# Edit: name = "Kitchen"        (this satellite's name)
# Edit: location = "kitchen"    (room location)

# 5. Test connection
dawn_satellite --keyboard --verbose

# 6. Set up auto-start
sudo tee /etc/systemd/system/dawn-satellite.service << 'EOF'
[Unit]
Description=DAWN Voice Satellite
After=network-online.target sound.target
Wants=network-online.target

[Service]
Type=simple
User=pi
ExecStart=/usr/local/bin/dawn_satellite
Restart=always
RestartSec=5

[Install]
WantedBy=multi-user.target
EOF

sudo systemctl enable dawn-satellite
sudo systemctl start dawn-satellite
```

## Overview

DAP2 enables satellite devices to extend DAWN's voice assistant capabilities to multiple rooms. All satellites use **WebSocket** on the same port as the WebUI (default 3000). This document covers **Tier 1** (Raspberry Pi) satellites, which handle ASR/TTS locally and send only text to the daemon. For the complete protocol spec (both tiers) and Tier 2 (ESP32 audio path), see [DAP2_DESIGN.md](DAP2_DESIGN.md).

Tier 1 satellites are **fully hands-free** — wake word detection triggers listening, VAD detects end-of-speech, and responses are spoken via local TTS. No buttons or LEDs required.

```
┌─────────────────────────────────────────────────────────────────┐
│                        DAWN Daemon                               │
│  ┌─────────┐  ┌─────────┐  ┌─────────┐  ┌─────────────────────┐ │
│  │ WebUI   │  │   LLM   │  │  Tools  │  │ Satellite Protocol  │ │
│  │ Server  │  │ Pipeline│  │ Registry│  │    (WebSocket)      │ │
│  └────┬────┘  └────┬────┘  └────┬────┘  └──────────┬──────────┘ │
│       │            │            │                   │            │
│       └────────────┴────────────┴───────────────────┘            │
│                              │                                    │
└──────────────────────────────┼────────────────────────────────────┘
                               │ WebSocket (ws://host:3000)
           ┌───────────────────┼───────────────────┐
           │                   │                   │
           ▼                   ▼                   ▼
    ┌─────────────┐     ┌─────────────┐     ┌─────────────┐
    │  Kitchen    │     │  Bedroom    │     │   Office    │
    │  Satellite  │     │  Satellite  │     │  Satellite  │
    │             │     │             │     │             │
    │ ┌─────────┐ │     │ ┌─────────┐ │     │ ┌─────────┐ │
    │ │Wake Word│ │     │ │Wake Word│ │     │ │Wake Word│ │
    │ │  VAD    │ │     │ │  VAD    │ │     │ │  VAD    │ │
    │ │Local ASR│ │     │ │Local ASR│ │     │ │Local ASR│ │
    │ │Local TTS│ │     │ │Local TTS│ │     │ │Local TTS│ │
    │ └─────────┘ │     │ └─────────┘ │     │ └─────────┘ │
    └─────────────┘     └─────────────┘     └─────────────┘
```

## Architecture

### Tier 1 Voice Flow

```
┌─────────┐    wake word    ┌───────────┐    VAD silence    ┌────────────┐
│  IDLE   │ ──────────────► │ LISTENING │ ────────────────► │ PROCESSING │
│(monitor)│                 │ (record)  │                   │   (ASR)    │
└─────────┘                 └───────────┘                   └─────┬──────┘
     ▲                                                            │
     │                      ┌───────────┐    TTS complete         │
     └───────────────────── │ SPEAKING  │ ◄───────────────────────┘
                            │  (TTS)    │      daemon response
                            └───────────┘
```

1. **IDLE**: Always listening for wake word ("Hey Friday")
2. **LISTENING**: Recording speech, VAD monitors for silence
3. **PROCESSING**: Local ASR transcribes, sends text to daemon
4. **SPEAKING**: Local TTS renders daemon's text response

### Why Text-First?

| Aspect | Audio Path (Tier 2, ESP32) | Text Path (Tier 1, RPi) |
|--------|------------------------------|------------------------------|
| Transport | WebSocket binary PCM | WebSocket JSON |
| Bandwidth | ~960KB per interaction | ~100 bytes |
| Latency | Network + server ASR | Local ASR only |
| Offline | No response possible | "I can't reach the server" |
| Privacy | Audio leaves room | Text only over network |
| Wake Word | Push-to-talk (button) | Always listening locally |
| Interaction | Button press required | Fully hands-free |
| Music | Not supported (v1) | Opus streaming via dedicated WS |

## Protocol Specification

DAP2 uses JSON messages over WebSocket, connecting to the same port as the WebUI (default 3000).

### Message Types

#### Satellite → Daemon

**Registration** (required before queries):
```json
{
  "type": "satellite_register",
  "payload": {
    "uuid": "550e8400-e29b-41d4-a716-446655440000",
    "name": "Kitchen Assistant",
    "location": "kitchen",
    "tier": 1,
    "capabilities": {
      "local_asr": true,
      "local_tts": true,
      "wake_word": true
    }
  }
}
```

**Query** (transcribed speech):
```json
{
  "type": "satellite_query",
  "payload": {
    "text": "turn on the kitchen lights"
  }
}
```

**Ping** (keepalive):
```json
{
  "type": "satellite_ping"
}
```

#### Daemon → Satellite

**Registration Acknowledgment**:
```json
{
  "type": "satellite_register_ack",
  "payload": {
    "success": true,
    "session_id": 12345,
    "message": "Registered as Kitchen Assistant"
  }
}
```

**State Updates**:
```json
{
  "type": "state",
  "payload": {
    "state": "thinking",
    "detail": "Processing query..."
  }
}
```

**Streaming Response** (sentence-level TTS happens as deltas arrive):
```json
{"type": "stream_start", "payload": {"stream_id": 1}}
{"type": "stream_delta", "payload": {"stream_id": 1, "text": "I'll turn on the lights. "}}
{"type": "stream_delta", "payload": {"stream_id": 1, "text": "They should be on now."}}
{"type": "stream_end", "payload": {"stream_id": 1, "reason": "complete"}}
```

**Pong**:
```json
{
  "type": "satellite_pong"
}
```

**Error**:
```json
{
  "type": "error",
  "payload": {
    "code": "NOT_REGISTERED",
    "message": "Must register before sending queries"
  }
}
```

## Directory Structure

```
dawn_satellite/
├── assets/fonts/       # TTF fonts (Source Sans 3, IBM Plex Mono)
├── config/             # Default satellite.toml configuration
├── include/            # Public headers
│   └── ui/             # SDL2 UI headers (music_types, orb, transcript, etc.)
├── src/                # Implementation
│   └── ui/             # SDL2 touchscreen UI (optional, ENABLE_SDL_UI)
└── CMakeLists.txt

common/                 # Shared library (daemon + satellite)
├── include/
│   ├── asr/            # ASR engine abstraction (Whisper, Vosk, VAD)
│   ├── audio/          # Thread-safe ring buffer
│   ├── tts/            # Piper TTS + text preprocessing
│   └── utils/          # Sentence buffer, string utilities
├── src/                # Implementations matching include/ layout
└── CMakeLists.txt
```

## Configuration

### Config File Locations

The satellite searches for configuration in this order:
1. `./satellite.toml` (current directory)
2. `/etc/dawn/satellite.toml` (system-wide)
3. `~/.config/dawn/satellite.toml` (user-specific)

Use `--config /path/to/file.toml` to specify explicitly.

### Configuration Reference

```toml
# =============================================================================
# Identity - UNIQUE PER SATELLITE
# =============================================================================
[identity]
# UUID auto-generated if empty. Set explicitly for persistent identity:
#   uuidgen | tr '[:upper:]' '[:lower:]'
uuid = ""
name = "Satellite"
location = ""  # e.g., "kitchen", "bedroom", "office"

# =============================================================================
# Server Connection
# =============================================================================
[server]
host = "192.168.1.100"  # DAWN daemon IP
port = 3000             # WebUI port
ssl = true              # Use wss:// instead of ws://
ssl_verify = true       # Verify SSL certificates (default: true)
ca_cert_path = "/etc/dawn/ca.crt"  # Path to DAWN private CA certificate
reconnect_delay_ms = 5000
max_reconnect_attempts = 0  # 0 = infinite

# =============================================================================
# Audio
# =============================================================================
[audio]
# Use 'arecord -l' and 'aplay -l' to list devices
capture_device = "plughw:1,0"   # ReSpeaker or USB mic
playback_device = "plughw:1,0"
sample_rate = 16000
max_record_seconds = 30

# =============================================================================
# Voice Activity Detection (VAD)
# =============================================================================
[vad]
enabled = true
silence_duration_ms = 800   # Silence to trigger end-of-speech
min_speech_ms = 250         # Minimum speech before accepting
threshold = 0.5             # 0.0-1.0, higher = more strict

# =============================================================================
# Wake Word Detection
# =============================================================================
[wake_word]
enabled = true
word = "friday"             # Must match daemon's AI_NAME
sensitivity = 0.5           # 0.0-1.0, higher = more false positives

# =============================================================================
# GPIO (optional - for Tier 2 push-to-talk or development)
# =============================================================================
[gpio]
# Tier 1 uses VAD + wake word, not push-to-talk
enabled = false
chip = "gpiochip0"
button_pin = 17
button_active_low = true

# =============================================================================
# NeoPixel LEDs (optional - for custom builds)
# =============================================================================
[neopixel]
# Tier 1 RPi satellites typically don't use indicator LEDs
enabled = false
spi_device = "/dev/spidev0.0"
num_leds = 8
brightness = 64  # 0-255

# =============================================================================
# Display (optional)
# =============================================================================
[display]
enabled = false
device = "/dev/fb1"

# =============================================================================
# Screensaver / Ambient Mode
# =============================================================================
[screensaver]
enabled = true   # Activate after idle timeout
timeout = 120    # Seconds of inactivity before activation (30-600)

# =============================================================================
# SDL2 Touchscreen UI
# =============================================================================
[sdl_ui]
enabled = false       # Requires libsdl2-dev libsdl2-ttf-dev, build with -DENABLE_SDL_UI=ON
width = 1024          # Display resolution
height = 600
brightness = 100      # Saved brightness (10-100)
volume_pct = 80       # Saved volume (0-100)
time_24h = false      # 12-hour (false) or 24-hour (true) clock format
theme = "cyan"        # UI color theme: cyan, purple, green, blue, terminal

# =============================================================================
# Logging
# =============================================================================
[logging]
level = "info"  # error, warning, info, debug
use_syslog = false
```

### Command-Line Overrides

Command-line arguments override config file values:

```bash
./dawn_satellite \
  --config /etc/dawn/satellite.toml \
  --server 192.168.1.100 \
  --port 3000 \
  --name "Kitchen" \
  --location "kitchen" \
  --capture "plughw:1,0" \
  --playback "plughw:1,0" \
  --num-leds 8 \
  --verbose
```

| Option | Description |
|--------|-------------|
| `-C, --config FILE` | Config file path |
| `-s, --server IP` | Daemon hostname/IP |
| `-p, --port PORT` | WebUI port |
| `-S, --ssl` | Use secure WebSocket |
| `-N, --name NAME` | Satellite name |
| `-L, --location LOC` | Room location |
| `-c, --capture DEV` | ALSA capture device |
| `-o, --playback DEV` | ALSA playback device |
| `-k, --keyboard` | Use keyboard for testing (disables VAD) |
| `--ca-cert FILE` | Path to CA certificate for SSL verification |
| `--registration-key KEY` | Pre-shared key for satellite registration |
| `-I, --no-ssl-verify` | Disable SSL cert verification (dev only) |
| `-v, --verbose` | Enable debug output |

## TLS Setup

DAWN uses a private Certificate Authority (CA) to secure WebSocket connections. The daemon's `generate_ssl_cert.sh` creates a CA and signs the server certificate with it. Each client type needs the CA certificate to validate the server.

### RPi Satellite

1. On the daemon machine, generate certificates (if not already done):
   ```bash
   ./generate_ssl_cert.sh
   ```

2. Copy the CA certificate to the satellite:
   ```bash
   scp ssl/ca.crt pi@satellite-ip:/etc/dawn/ca.crt
   ```

3. Configure the satellite (`/etc/dawn/satellite.toml`):
   ```toml
   [server]
   ssl = true
   ssl_verify = true
   ca_cert_path = "/etc/dawn/ca.crt"
   ```

4. Or use CLI flags:
   ```bash
   dawn_satellite --ssl --ca-cert /etc/dawn/ca.crt
   ```

### ESP32 Satellite

The CA certificate is embedded at compile time:

1. Run `./generate_ssl_cert.sh` on the daemon — this auto-generates `dawn_satellite_arduino/ca_cert.h`
2. Flash the ESP32 with the updated firmware
3. The ESP32 validates the server certificate against the embedded CA

> **Security note**: WiFi credentials and the session reconnect secret are stored in plaintext on the ESP32 (compiled into the binary and NVS respectively). An attacker with physical access to the device can extract them. For production deployments, consider WiFi provisioning (BLE/SoftAP) to remove credentials from the binary, and ESP-IDF flash encryption to protect NVS contents. These are beyond the current scope of this project.

### Browser

Install the CA certificate in your OS trust store:

```bash
# Linux
sudo cp ssl/ca.crt /usr/local/share/ca-certificates/dawn-ca.crt
sudo update-ca-certificates

# macOS
sudo security add-trusted-cert -d -r trustRoot \
  -k /Library/Keychains/System.keychain ssl/ca.crt

# Windows
certutil -addstore -f "ROOT" ssl\ca.crt
```

After installing, restart your browser and access `https://<dawn-ip>:3000` with no security warnings.

### Certificate Renewal

When the daemon's IP changes or the server certificate expires (1 year):

```bash
./generate_ssl_cert.sh --renew   # Regenerates server cert, reuses CA
./generate_ssl_cert.sh --check   # Show expiry dates
```

To include an external IP or domain name (e.g., for port forwarding):

```bash
./generate_ssl_cert.sh --renew --san IP:203.0.113.50 --san DNS:dawn.example.com
```

Only the server certificate needs renewal. The CA (10-year validity) remains unchanged, so clients don't need updating.

## Registration Key

A pre-shared key prevents unauthorized devices from registering as satellites. Without a key, any device on the LAN can register.

### Generate a Key

On the daemon machine:

```bash
./generate_ssl_cert.sh --gen-key
```

This generates a 32-byte hex key and appends it to `secrets.toml`. Running it again shows the existing key without regenerating.

### Configure Satellites

**RPi satellite** (`satellite.toml`):
```toml
[server]
registration_key = "your-64-char-hex-key-here"
```

Or via CLI: `--registration-key <key>`

**ESP32 satellite** (`arduino_secrets.h`):
```c
#define SECRET_REGISTRATION_KEY "your-64-char-hex-key-here"
```

**Environment variable** (alternative for all client types):
```bash
export DAWN_SATELLITE_KEY="your-64-char-hex-key-here"
```

### Behavior

- **Key set on daemon**: Satellites must provide a matching `registration_key` in their `satellite_register` message. Mismatched or missing keys are rejected with a clear error.
- **No key on daemon**: Registration is open (backward-compatible). The daemon logs a warning at startup.

## Building for Raspberry Pi

### Supported Hardware

| Model | RAM | CPU | Status | Notes |
|-------|-----|-----|--------|-------|
| Pi Zero 2 W | 512MB | Cortex-A53 (quad) | Not recommended | Too slow for local ASR/TTS |
| Pi 3B/3B+ | 1GB | Cortex-A53 (quad) | Supported | Good balance of cost/performance |
| **Pi 4B** | 2-8GB | Cortex-A72 (quad) | **Primary target** | Recommended for satellite |
| Pi 5 | 4-8GB | Cortex-A76 (quad) | Supported | Fastest, best for local Whisper ASR |

### Step 1: Raspberry Pi OS Setup

**Flash Raspberry Pi OS Lite (64-bit)** - or Desktop if using a touchscreen display.

```bash
# Use Raspberry Pi Imager or:
# Download: https://www.raspberrypi.com/software/operating-systems/

# Before first boot, enable SSH and configure WiFi:
# In /boot (or /bootfs on newer images):
touch ssh
cat > wpa_supplicant.conf << 'EOF'
country=US
ctrl_interface=DIR=/var/run/wpa_supplicant GROUP=netdev
update_config=1

network={
    ssid="YOUR_WIFI_SSID"
    psk="YOUR_WIFI_PASSWORD"
}
EOF
```

**First boot setup:**
```bash
# SSH into the Pi (default: pi@raspberrypi.local)
ssh pi@raspberrypi.local

# Update system
sudo apt update && sudo apt upgrade -y

# Set hostname for easy identification
sudo hostnamectl set-hostname kitchen-satellite  # or bedroom, office, etc.

# Expand filesystem if needed
sudo raspi-config --expand-rootfs

# Reboot
sudo reboot
```

### Step 2: Install Dependencies

```bash
# Build tools
sudo apt install -y \
  build-essential \
  cmake \
  pkg-config \
  git

# Required libraries
sudo apt install -y \
  libasound2-dev \
  libwebsockets-dev \
  libjson-c-dev

# Optional: GPIO support (for Tier 2 push-to-talk or development)
sudo apt install -y libgpiod-dev

# Optional: SDL2 touchscreen UI (7" display with themes, music, screensaver)
sudo apt install -y libsdl2-dev libsdl2-ttf-dev libsdl2-gfx-dev
```

**Dependency versions (Raspberry Pi OS Bookworm):**
| Package | Version | Notes |
|---------|---------|-------|
| cmake | 3.25+ | Sufficient for project |
| libwebsockets | 4.3+ | WebSocket client |
| libjson-c | 0.16+ | JSON parsing |
| libasound2 | 1.2+ | ALSA audio |
| libsdl2 | 2.0.20+ | Optional: touchscreen UI |
| libsdl2-ttf | 2.0.18+ | Optional: UI text rendering |
| libsdl2-gfx | 1.0.4+ | Optional: smooth circle primitives (fallback: scanline fill) |

### Step 3: Clone and Build

```bash
# Clone the repository
git clone https://github.com/YOUR_REPO/dawn.git
cd dawn/dawn_satellite

# Create build directory
mkdir build && cd build

# Configure (Tier 1, no NeoPixels)
cmake .. \
  -DENABLE_DAP2=ON \
  -DENABLE_NEOPIXEL=OFF \
  -DCMAKE_BUILD_TYPE=Release

# Build
# Pi Zero 2 W: use -j2 to avoid OOM (512MB RAM)
# Pi 3/4/5: use -j4
make -j2
```

**Build times (approximate):**
| Model | Build Time |
|-------|------------|
| Pi Zero 2 W | ~8-10 minutes |
| Pi 3B+ | ~3-4 minutes |
| Pi 4B | ~1-2 minutes |

### Step 4: Install

```bash
sudo make install
```

This installs:
- `/usr/local/bin/dawn_satellite` - Main binary
- `/etc/dawn/satellite.toml` - Configuration file

### Build Options

| Option | Default | Description |
|--------|---------|-------------|
| `ENABLE_VAD` | ON | Silero VAD (requires ONNX Runtime) |
| `ENABLE_VOSK_ASR` | ON | Vosk streaming ASR (default, lightweight) |
| `ENABLE_WHISPER_ASR` | OFF | Whisper batch ASR (heavier, more accurate) |
| `ENABLE_TTS` | ON | Piper TTS (requires ONNX Runtime) |
| `ENABLE_DAP2` | ON | WebSocket text protocol (Tier 1) |
| `ENABLE_NEOPIXEL` | OFF | NeoPixel LED support (optional) |
| `ENABLE_SDL_UI` | OFF | SDL2 touchscreen UI with 5-theme system (requires libsdl2-dev, libsdl2-ttf-dev; libsdl2-gfx-dev optional for smooth circles) |
| `ENABLE_DISPLAY` | OFF | Framebuffer display support (optional) |
| `CMAKE_BUILD_TYPE` | Release | Use `Debug` for development |

Default `cmake ..` gives: VAD + Vosk + TTS (no Whisper compile).

### Cross-Compilation (Optional)

For faster builds, cross-compile on a desktop Linux machine:

```bash
# Install cross-compiler (Ubuntu/Debian)
sudo apt install gcc-aarch64-linux-gnu g++-aarch64-linux-gnu

# Configure for cross-compilation
cmake .. \
  -DCMAKE_SYSTEM_NAME=Linux \
  -DCMAKE_SYSTEM_PROCESSOR=aarch64 \
  -DCMAKE_C_COMPILER=aarch64-linux-gnu-gcc \
  -DCMAKE_CXX_COMPILER=aarch64-linux-gnu-g++ \
  -DENABLE_DAP2=ON \
  -DENABLE_NEOPIXEL=OFF

make -j$(nproc)

# Copy to Pi
scp dawn_satellite pi@kitchen-satellite.local:/usr/local/bin/
```

**Note:** Cross-compilation requires matching library versions. Native compilation on the Pi is simpler.

## Audio Hardware Setup

### Recommended: USB Audio Adapter

The Pi Zero 2 W has no built-in audio output. Use a USB audio adapter:

```bash
# List audio devices
arecord -l  # Capture devices
aplay -l    # Playback devices

# Example output:
# card 1: Device [USB Audio Device], device 0: USB Audio [USB Audio]
#   Subdevices: 1/1
#   Subdevice #0: subdevice #0
```

Configure in `/etc/dawn/satellite.toml`:
```toml
[audio]
capture_device = "plughw:1,0"   # USB mic
playback_device = "plughw:1,0" # USB speaker
```

### Test Audio

```bash
# Test recording (5 seconds)
arecord -D plughw:1,0 -f S16_LE -r 16000 -c 1 -d 5 test.wav

# Test playback
aplay -D plughw:1,0 test.wav

# Adjust volume
alsamixer
```

### ReSpeaker Support

For ReSpeaker 2-Mic HAT or similar I2S microphone arrays:

```bash
# Install drivers (ReSpeaker 2-Mic)
git clone https://github.com/respeaker/seeed-voicecard
cd seeed-voicecard
sudo ./install.sh
sudo reboot

# Device will appear as:
# card 0: seeed2micvoicec [seeed-2mic-voicecard], device 0: ...
```

Configure:
```toml
[audio]
capture_device = "plughw:0,0"
playback_device = "plughw:0,0"
```

## Deployment

### Systemd Service

Create `/etc/systemd/system/dawn-satellite.service`:

```ini
[Unit]
Description=DAWN Voice Satellite
After=network-online.target sound.target
Wants=network-online.target

[Service]
Type=simple
User=pi
ExecStart=/usr/local/bin/dawn_satellite --config /etc/dawn/satellite.toml
Restart=always
RestartSec=5

[Install]
WantedBy=multi-user.target
```

Enable and start:
```bash
sudo systemctl enable dawn-satellite
sudo systemctl start dawn-satellite
```

### Multi-Satellite Deployment

**Strategy 1: Per-device config files**
```bash
# On each satellite, customize identity:
sudo nano /etc/dawn/satellite.toml
# Set: name = "Kitchen", location = "kitchen"
```

**Strategy 2: Base config + service overrides**
```bash
# Same config everywhere
# Customize in systemd:
sudo systemctl edit dawn-satellite
```
```ini
[Service]
ExecStart=
ExecStart=/usr/local/bin/dawn_satellite \
  --name "Kitchen" --location "kitchen"
```

**Strategy 3: Hostname-based identity**
```bash
# Set hostname to match location
sudo hostnamectl set-hostname kitchen-satellite

# In config or startup script:
name = "$(hostname)"
```

## Testing

### Python Test Client

For protocol testing without audio hardware:

```bash
pip3 install websocket-client
python3 tests/test_satellite_protocol.py --host localhost --port 3000
```

Commands:
- `/register [name] [location]` - Register satellite
- `/query <text>` - Send query
- `/ping` - Send keepalive
- `<text>` - Direct query (shortcut)

### Manual Testing with wscat

```bash
npm install -g wscat
wscat -c ws://localhost:3000

# Register
{"type":"satellite_register","payload":{"uuid":"test-123","name":"Test","location":"test","tier":1,"capabilities":{"local_asr":true,"local_tts":true}}}

# Query
{"type":"satellite_query","payload":{"text":"what time is it"}}
```

### Satellite Binary Testing

```bash
# Development mode: keyboard input instead of VAD
./dawn_satellite --keyboard --verbose

# Type text at the prompt to simulate transcribed speech
# This bypasses wake word and VAD for testing

# Production mode: VAD + wake word (requires ASR/TTS integration)
./dawn_satellite --verbose
# Say "Hey Friday" to activate, speak your query
```

## Current Status

### Implemented — Core Voice Pipeline

1. **Local VAD** - Silero ONNX model for real-time speech detection
2. **Wake Word Detection** - VAD + ASR transcription checked for wake word ("Hey Friday")
3. **Local ASR** - Vosk streaming (default, near-instant) or Whisper batch
4. **Local TTS** - Piper with sentence-level streaming during LLM response
5. **Session Reconnection** - Cryptographic reconnect secrets, conversation history preserved
6. **Offline Fallback** - TTS "I can't reach the server right now" on connection loss
7. **Time-of-Day Greeting** - Spoken on startup/reconnection (morning/day/evening)
8. **Unified Logging** - Same format as daemon (timestamps, colors, file:line)
9. **Clean Shutdown** - Ctrl+C responsive in all states (model loading, TTS playback, idle)
10. **Music Library Pagination** - Browse full library via voice (50 items per page)

### Implemented — SDL2 Touchscreen UI

11. **Orb visualization** - Animated orb with state-driven colors (idle/listening/thinking/speaking)
12. **FFT spectrum bars** - Real-time frequency visualization around orb during speech playback
13. **Streaming transcript** - Live-updating conversation history (40 entries, 4KB per entry)
14. **Inline markdown rendering** - Bold, italic, code spans, and bullet points in AI responses
15. **Touch-drag scrolling** - Finger and mouse scrolling with auto-scroll on new messages
16. **User transcription display** - Shows "You: ..." after ASR completes
17. **Touch gestures** - Tap orb to cancel/listen, drag to scroll, swipe for panels
18. **Status bar** - WiFi signal quality, connection status, tool call info, date/time
19. **KMSDRM backend** - Direct rendering without X11, suitable for headless Pi setups
20. **UI persists across reconnections** - SDL lifecycle independent of WebSocket connection
21. **Settings panel** - Pull-down panel with server address, connection status, device name, IP, uptime, session duration
22. **TTS playback queue** - Producer-consumer pipeline: synthesize sentence N+1 while playing sentence N

### Implemented — Music Player

23. **Music player panel** - Three-tab design: Playing (transport + visualizer), Queue, Library
24. **Transport controls** - Play/pause, prev/next, shuffle/repeat toggles, progress bar with drag-to-seek
25. **Opus audio streaming** - Daemon streams music via Opus over WebSocket, satellite decodes to ALSA
26. **Live FFT visualizer** - Goertzel analysis on audio stream (not simulated), 16 radial bars
27. **Library browsing** - Artists, albums, and tracks with paginated navigation (50 items/page)
28. **Artist/album drill-down** - Tap artist to see tracks, tap album to see tracks
29. **SDL primitive icons** - All UI icons drawn with SDL (no image assets needed)
30. **Daemon satellite auth** - Per-handler whitelist allows satellite access to music endpoints

### Implemented — Settings & Display

31. **Brightness slider** - sysfs backlight control for DSI displays, software dimming fallback for HDMI
32. **Volume slider** - ALSA mixer control from settings panel
33. **Persistent settings** - Brightness, volume, time format, and theme saved to config file across restarts
34. **12/24-hour time toggle** - Animated toggle in settings panel; applies to both main clock and screensaver
35. **Buffer-compensated position** - Music progress bar subtracts ring buffer + ALSA delay for accurate display
36. **5-theme system** - Cyan, Purple, Green, Blue, Terminal with dot picker, 200ms crossfade, TOML persistence
37. **SDL2_gfx circle primitives** - Smooth anti-aliased circles for orb rendering (fallback to scanline fill without library)
38. **Lock-free music playback** - SPSC ring buffer with LWS-thread drain eliminates relay thread and playback underruns

### Implemented — Screensaver / Ambient Mode

36. **Clock mode** - Time and date centered with Lissajous drift for burn-in prevention
37. **"D.A.W.N." watermarks** - Corner watermarks with sine-pulse fade animation
38. **Fullscreen rainbow visualizer** - 64-bin Goertzel FFT spectrum with HSV color cycling, peak hold, gradient reflections
39. **Track info pill** - Two-line display (large title, smaller album/artist) with fade-in/out on track change
40. **dB-scale spectrum** - Matches WebUI's getByteFrequencyData approach (60dB range, 0.7 gamma)
41. **Auto-activation** - Configurable idle timeout (default 120s), panels block timer
42. **Manual trigger** - Tap music panel visualizer to enter fullscreen mode
43. **Wake word dismissal** - Only dismissed by wake word detection, not simple VAD

### Implemented — Reliability & Bug Fixes

23. **App-level keep-alive** - `satellite_ping` every 10s (WS-level pings disabled for lws 4.3.5 compat)
24. **Ping-query race fix** - `ws_client_ping()` skips if tx_buffer already has pending data
25. **Response timeout** - Returns to idle after 30s (no data) or 120s (streaming stalled)
26. **Sentence breaking** - Breaks on `.!?`, bullets (`\n-`, `\n*`), numbered lists, paragraphs, `:\n`
27. **Emoji stripping** - Full SMP coverage (0x1F000-0x1FFFF) for both TTS and display rendering
28. **Atomic init guards** - Thread-safe `_Atomic bool` with `atomic_exchange()` for lookup tables

### Planned Features

1. [ ] **Barge-in support** - Interrupt TTS by speaking (stubbed, not connected)
2. [x] ~~**Quick actions panel**~~ - Replaced by status bar icon pattern (icons appear in transcript header as features ship)
3. [x] ~~**Screensaver / ambient mode**~~ - Clock with Lissajous drift + fullscreen rainbow FFT visualizer
4. [x] ~~**Theme support**~~ - 5-theme system (Cyan, Purple, Green, Blue, Terminal) with dot picker, crossfade, TOML persistence
5. [x] ~~**TTS ducking during music**~~ - Volume ducks to 30% during voice activity (wake word, recording, processing), hard-pauses during TTS
6. [x] ~~**Multi-satellite routing**~~ - Per-session room context in system prompt; daemon knows which room each query originates from
7. [ ] **Speaker identification** - Personalized responses per user
8. [x] ~~**Location-aware queries**~~ - Room context injected into system prompt: local session via `Room=X` in `get_localization_context()` (from `dawn.toml` `room`), DAP2 sessions via `session_append_room_context()` (from satellite `location` field)

## Troubleshooting

### Connection Issues

```bash
# Check daemon is running and WebUI is accessible
curl http://192.168.1.100:3000/

# Test WebSocket with verbose output
./dawn_satellite --server 192.168.1.100 --verbose
```

### Audio Issues

```bash
# List capture devices
arecord -l

# List playback devices
aplay -l

# Test capture
arecord -D plughw:1,0 -f S16_LE -r 16000 -c 1 test.wav

# Test playback
aplay -D plughw:1,0 test.wav
```

### Service Issues

```bash
# Check service status
sudo systemctl status dawn-satellite

# View logs
journalctl -u dawn-satellite -f

# Check if binary exists
which dawn_satellite
ls -la /usr/local/bin/dawn_satellite

# Check config file
cat /etc/dawn/satellite.toml
```

### Memory Issues (Pi Zero 2 W)

The Pi Zero 2 W has only 512MB RAM. If builds fail:

```bash
# Create swap file
sudo fallocate -l 1G /swapfile
sudo chmod 600 /swapfile
sudo mkswap /swapfile
sudo swapon /swapfile

# Make permanent
echo '/swapfile none swap sw 0 0' | sudo tee -a /etc/fstab

# Build with fewer parallel jobs
make -j1
```

### Network Issues

```bash
# Check WiFi connection
iwconfig wlan0
ping -c 3 google.com

# Check if daemon is reachable
ping -c 3 192.168.1.100  # Your daemon IP
nc -zv 192.168.1.100 3000  # Test port

# Check firewall on daemon
sudo ufw status  # On daemon machine
```

## Tier 2 Satellite (ESP32-S3)

Tier 2 satellites are push-to-talk devices with server-side ASR and TTS. The implementation lives in `dawn_satellite_arduino/` as an Arduino sketch (not ESP-IDF).

### Hardware

| Component | Part | Notes |
|-----------|------|-------|
| MCU | Adafruit ESP32-S3 TFT Feather | Built-in 240x135 TFT + NeoPixels, 2MB OPI PSRAM |
| Speaker amp | MAX98357 I2S breakout | 3.3V logic, mono, 48kHz stereo output |
| Microphone | Analog electret mic module | Connected to ADC pin (GPIO 1) |
| Button | Momentary push button | Push-to-talk trigger (GPIO 18) |

### Quick Start

1. Install the Arduino IDE with ESP32 board support (see `dawn_satellite_arduino/README.md`)
2. Copy `arduino_secrets.h.example` to `arduino_secrets.h` and fill in WiFi/server credentials
3. Select board "Adafruit Feather ESP32-S3 TFT" with OPI PSRAM enabled
4. Upload and open Serial Monitor at 115200 baud

### How It Works

```
User presses button → ADC samples at 16kHz → PCM chunks sent via WS binary 0x01
User releases button → End marker 0x02 sent
Daemon: Whisper ASR → LLM → Piper TTS → 22050Hz PCM via WS binary 0x11/0x12
ESP32: Ring buffer → 22050→48kHz resample → I2S stereo → Speaker
```

### Key Implementation Details

- **Ring buffer**: 524,288-sample power-of-two buffer in PSRAM (~1MB), spinlock-protected
- **Resampling**: 22050→48000Hz linear interpolation with cross-boundary carry sample
- **TCP backpressure**: Skips `webSocket.loop()` when ring buffer >50% full
- **NVS persistence**: Random UUID v4 and reconnect_secret survive power cycles
- **Credentials**: `arduino_secrets.h` is gitignored; template in `.h.example`
- **I2S channel**: Created once in `setup()`, enable/disable per playback (avoids DMA descriptor fragmentation)
- **NeoPixels**: Mode-change detection — static modes (recording/playing/waiting) only call `strip.show()` once on transition

For full details, see `dawn_satellite_arduino/README.md` and `docs/DAP2_DESIGN.md` Phase 4.

## Related Documentation

- `docs/DAP2_DESIGN.md` - Protocol design specification (Tier 1 + Tier 2)
- `dawn_satellite_arduino/README.md` - Tier 2 Arduino sketch setup and usage
- `ARCHITECTURE.md` - System architecture with DAP2 protocol overview
- `CODING_STYLE_GUIDE.md` - Code style requirements
