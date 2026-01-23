# Dawn Audio Protocol 2.0 (DAP 2) Design Document

**Status**: Draft
**Version**: 0.1
**Date**: November 2025

## Executive Summary

DAP 2.0 represents a fundamental architectural shift from the "dumb satellite" model of DAP 1 to a "smart satellite" model where satellites run significant portions of the DAWN pipeline locally. This enables:

- **Seamless UX**: Local wake word detection, VAD, and TTS playback
- **Reduced latency**: Only LLM queries go to the central daemon
- **Offline resilience**: Basic functionality without network
- **Scalability**: Central daemon only handles LLM orchestration

---

## Table of Contents

1. [Problem Statement](#1-problem-statement)
2. [Architecture Comparison](#2-architecture-comparison)
3. [DAP 2.0 Architecture](#3-dap-20-architecture)
4. [Satellite Tiers](#4-satellite-tiers)
5. [Protocol Specification](#5-protocol-specification)
6. [Common Code Architecture](#6-common-code-architecture)
7. [Implementation Phases](#7-implementation-phases)
8. [Platform Considerations](#8-platform-considerations)
9. [Security Architecture](#9-security-architecture)
10. [Network Reliability](#10-network-reliability)
11. [Performance Validation](#11-performance-validation)
12. [Open Questions](#12-open-questions)

---

## 1. Problem Statement

### DAP 1.0 Limitations

| Issue | Impact |
|-------|--------|
| **Audio round-trip** | User speaks → audio sent to server → ASR → LLM → TTS → audio sent back. High latency (~5-15s total). |
| **No local wake word** | Cannot activate hands-free; requires button press on ESP32. |
| **No local VAD** | Can't detect end of speech locally; relies on button release or timeout. |
| **Large audio payloads** | 30 seconds of 16kHz audio = ~960KB per interaction. |
| **Server bottleneck** | Central daemon blocks on each client during processing. |
| **No offline mode** | Satellite is useless without network connection. |

### DAP 2.0 Goals

1. **Local wake word detection** - Satellite continuously listens, activates on "Friday"
2. **Local VAD** - Satellite detects speech end, sends only transcribed text OR minimal audio
3. **Local TTS** - Satellite renders speech from text, no audio download
4. **Lightweight protocol** - Text-based messages, not audio streams
5. **Conversation continuity** - Central daemon maintains conversation history per satellite
6. **Graceful degradation** - Offline mode with local-only capabilities

---

## 2. Architecture Comparison

### DAP 1.0 Architecture (Current)

```
┌─────────────────────────────────────────────────────────────────────┐
│                          ESP32 Satellite                             │
│                                                                      │
│  ┌──────────┐    ┌──────────┐    ┌──────────┐    ┌──────────┐      │
│  │  Button  │───>│  Record  │───>│  Encode  │───>│   Send   │      │
│  │  Press   │    │  Audio   │    │   WAV    │    │  to DAP  │      │
│  └──────────┘    └──────────┘    └──────────┘    └──────────┘      │
│                                                                      │
│  ┌──────────┐    ┌──────────┐                                       │
│  │  Play    │<───│  Receive │<─────────────────── (large WAV)       │
│  │  Audio   │    │   WAV    │                                       │
│  └──────────┘    └──────────┘                                       │
└─────────────────────────────────────────────────────────────────────┘
         │                                      ▲
         │ ~960KB audio upload                  │ ~1-5MB audio download
         ▼                                      │
┌─────────────────────────────────────────────────────────────────────┐
│                        DAWN Central Daemon                           │
│                                                                      │
│  ┌─────────┐  ┌─────────┐  ┌─────────┐  ┌─────────┐  ┌─────────┐   │
│  │  DAP    │─>│  ASR    │─>│  LLM    │─>│  TTS    │─>│  DAP    │   │
│  │ Receive │  │(Whisper)│  │(OpenAI) │  │ (Piper) │  │  Send   │   │
│  └─────────┘  └─────────┘  └─────────┘  └─────────┘  └─────────┘   │
│                                                                      │
│                 BLOCKS entire main loop for 10-15s                   │
└─────────────────────────────────────────────────────────────────────┘
```

### DAP 2.0 Architecture (Proposed)

```
┌─────────────────────────────────────────────────────────────────────┐
│                     DAWN Satellite (RPi/ESP32-S3)                    │
│                                                                      │
│  ┌──────────────────────────────────────────────────────────────┐   │
│  │                    Local Processing Pipeline                   │   │
│  │                                                                │   │
│  │  ┌────────┐  ┌────────┐  ┌────────┐  ┌────────┐              │   │
│  │  │ Audio  │─>│  VAD   │─>│ Wake   │─>│  ASR   │              │   │
│  │  │Capture │  │(Silero)│  │  Word  │  │(Whisper│              │   │
│  │  │        │  │        │  │ Detect │  │  tiny) │              │   │
│  │  └────────┘  └────────┘  └────────┘  └────────┘              │   │
│  │                                           │                    │   │
│  │                                           ▼ (text: "turn on   │   │
│  │                                              the lights")     │   │
│  │  ┌────────┐  ┌────────┐  ┌────────────────────────────┐      │   │
│  │  │ Audio  │<─│  TTS   │<─│      DAP 2.0 Client        │      │   │
│  │  │Playback│  │(Piper) │  │ (send text, receive text)  │      │   │
│  │  └────────┘  └────────┘  └────────────────────────────┘      │   │
│  │                                                                │   │
│  └──────────────────────────────────────────────────────────────┘   │
│                                                                      │
│  Local-only fallback: "I can't reach the server right now"          │
└─────────────────────────────────────────────────────────────────────┘
         │                                      ▲
         │ ~100-500 bytes (text)                │ ~100-2000 bytes (text)
         ▼                                      │
┌─────────────────────────────────────────────────────────────────────┐
│                        DAWN Central Daemon                           │
│                                                                      │
│  ┌─────────────────────────────────────────────────────────────┐    │
│  │                    LLM Orchestration Layer                    │    │
│  │                                                               │    │
│  │  ┌───────────┐  ┌───────────┐  ┌───────────┐                │    │
│  │  │  Session  │  │    LLM    │  │  Command  │                │    │
│  │  │  Manager  │─>│ Interface │─>│  Parser   │                │    │
│  │  │(per-sat)  │  │           │  │           │                │    │
│  │  └───────────┘  └───────────┘  └───────────┘                │    │
│  │        │                             │                        │    │
│  │        ▼                             ▼                        │    │
│  │  ┌───────────┐               ┌───────────┐                   │    │
│  │  │   Conv.   │               │   MQTT    │                   │    │
│  │  │  History  │               │  Publish  │                   │    │
│  │  └───────────┘               └───────────┘                   │    │
│  │                                                               │    │
│  └─────────────────────────────────────────────────────────────┘    │
│                                                                      │
│  Worker thread per satellite - non-blocking                          │
└─────────────────────────────────────────────────────────────────────┘
```

---

## 3. DAP 2.0 Architecture

### Core Principles

1. **Text-First Protocol**: Satellites send transcribed text, not raw audio
2. **Local Intelligence**: ASR, VAD, wake word, and TTS run on satellite
3. **Stateless Messages**: Each request is self-contained; server maintains state
4. **Streaming Support**: LLM responses streamed sentence-by-sentence
5. **Fallback Modes**: Graceful degradation when network unavailable

### Message Types

| Type | Direction | Purpose |
|------|-----------|---------|
| `QUERY` | Satellite → Daemon | User's transcribed command |
| `RESPONSE` | Daemon → Satellite | LLM response text (may be streamed) |
| `RESPONSE_END` | Daemon → Satellite | End of streamed response |
| `COMMAND` | Daemon → Satellite | Direct command for satellite (e.g., play sound) |
| `STATUS` | Bidirectional | Health check, capabilities exchange |
| `REGISTER` | Satellite → Daemon | Initial registration with capabilities |
| `ACK` | Bidirectional | Acknowledgment |

### Connection Model: TCP-First (UDP Optional)

DAP 2.0 uses a **phased transport model** - starting with TCP-only for simplicity, with UDP as a later optimization if measurements justify it.

#### Phase 1-3: TCP Only

All communication (control AND audio) uses TCP:

```
┌─────────────────────────────────────────────────────────────┐
│                        Satellite                             │
│                                                              │
│  ┌───────────────────────────────────────────────────────┐  │
│  │                    TCP Client                          │  │
│  │         (control messages + audio streaming)           │  │
│  └───────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────┘
                              │
                              │ TCP :5000
                              ▼
┌─────────────────────────────────────────────────────────────┐
│                      DAWN Daemon                             │
│                                                              │
│  ┌───────────────────────────────────────────────────────┐  │
│  │                    TCP Server                          │  │
│  │         (control messages + audio streaming)           │  │
│  └───────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────┘
```

**Why TCP-first?**
- Simpler implementation (single socket, built-in ordering/reliability)
- Home WiFi has <0.1% packet loss (TCP overhead is minimal)
- Validate protocol correctness before adding complexity
- Measure actual latency to justify UDP optimization

#### Phase 4+: Optional UDP for Audio (If Needed)

Add UDP channel only if latency measurements show >100ms p95 benefit:

```
┌─────────────────────────────────────────────────────────────┐
│                        Satellite                             │
│                                                              │
│  ┌─────────────┐                    ┌─────────────────────┐ │
│  │ TCP Client  │ ◄──── Control ────►│  UDP Client         │ │
│  │ (text msgs) │                    │  (audio streaming)  │ │
│  └─────────────┘                    └─────────────────────┘ │
└─────────────────────────────────────────────────────────────┘
         │                                      │
         │ TCP :5000                            │ UDP :5001
         ▼                                      ▼
┌─────────────────────────────────────────────────────────────┐
│                      DAWN Daemon                             │
│                                                              │
│  ┌─────────────┐                    ┌─────────────────────┐ │
│  │ TCP Server  │                    │  UDP Server         │ │
│  │ (control)   │                    │  (audio)            │ │
│  └─────────────┘                    └─────────────────────┘ │
└─────────────────────────────────────────────────────────────┘
```

**When to add UDP?**
- Latency measurements show TCP p95 > 150ms for audio
- Deployment on lossy networks (not typical home WiFi)
- Multiple concurrent audio streams causing TCP head-of-line blocking

---

## 4. Satellite Tiers

DAP 2.0 supports **two satellite tiers** based on hardware capabilities:

### Tier 1: Full Satellite (Raspberry Pi 4/5, Orange Pi 5)

Runs the complete local pipeline - only LLM queries go to the daemon. **Follows the same interaction model as the DAWN daemon** for a seamless user experience.

| Component | Implementation | Memory | Notes |
|-----------|---------------|--------|-------|
| Audio Capture | ALSA | ~1 MB | Continuous 16kHz capture |
| VAD | Silero ONNX | ~2 MB | Real-time speech detection |
| Wake Word | Whisper tiny | ~77 MB | **Same implementation as DAWN daemon** |
| ASR | Whisper tiny/base | ~77-140 MB | Command transcription |
| TTS | Piper | ~60 MB | Local speech synthesis |
| **Total** | | **~220-280 MB** | Fits in 2GB+ RAM |

**Wake Word Detection**: Identical to DAWN daemon implementation:
1. VAD (Silero) continuously monitors for speech
2. On speech detection, run Whisper on audio chunk
3. Check transcript for wake word ("Friday")
4. If found, transition to command recording state
5. Uses same state machine: `SILENCE → WAKEWORD_LISTEN → COMMAND_RECORDING → PROCESSING`

**Capabilities**:
- Hands-free wake word activation ("Friday") - same feel as talking to main daemon
- Local ASR - sends **text** to daemon
- Local TTS - receives **text** from daemon
- Offline fallback: TTS "I can't reach the server right now"

**Communication**: TCP only (text-based messages)

```
User: "Friday, turn on the lights"
  │
  ▼ (local processing)
Satellite: VAD → Wake Word → ASR → "turn on the lights"
  │
  ▼ (TCP: ~100 bytes)
Daemon: LLM → "I'll turn on the lights for you"
  │
  ▼ (TCP: ~200 bytes)
Satellite: TTS → Audio playback
```

### Tier 2: Audio Satellite (ESP32-S3)

Button-activated with audio streaming - follows the DAP 1.1 interaction model with improved protocol.

| Component | Implementation | Memory | Notes |
|-----------|---------------|--------|-------|
| Audio Capture | I2S ADC | ~64 KB | Button-activated |
| VAD | Energy-based | ~1 KB | Silence detection (end of speech) |
| Audio Codec | ADPCM | ~8 KB | 4:1 compression, low CPU |
| Audio Playback | I2S DAC | ~64 KB | Streaming from daemon |
| Network Buffers | TCP/UDP | ~100 KB | WiFi stack + protocol |
| **Total** | | **~250-300 KB** | Requires ESP32-S3 with PSRAM |

**Hardware Requirement**: ESP32-S3 with 8MB PSRAM (standard on most dev boards)

**Capabilities**:
- Button-to-talk activation (no wake word - follows DAP 1.1 model)
- Local silence detection (knows when you stopped speaking)
- ADPCM-compressed audio upload
- Streaming audio playback

**Communication**: TCP (Phase 1-3), UDP audio streaming (Phase 4+)

```
User: [presses button] "Turn on the lights" [releases or silence detected]
  │
  ▼ (UDP: ~30KB ADPCM audio)
Daemon: ASR → LLM → TTS → audio stream
  │
  ▼ (UDP: ~50KB ADPCM audio)
Satellite: Audio playback
```

### Audio Codec (Tier 2): ADPCM

Tier 2 satellites use **ADPCM exclusively** for audio compression. This simplifies implementation and minimizes resource usage on ESP32.

| Codec | Bitrate | CPU | Quality | ESP32 Support |
|-------|---------|-----|---------|---------------|
| Raw PCM | 256 kbps | None | Perfect | Trivial |
| **ADPCM** | 32 kbps | Very Low | Good | Built into ESP-IDF |

**Why ADPCM only (not Opus)?**
- **Simpler implementation**: No codec negotiation needed
- **Lower resource usage**: Opus library adds ~180KB flash + ~60KB RAM
- **Sufficient quality**: ADPCM is designed for speech, not music
- **ESP-IDF native**: Built-in support, no external library
- **Home WiFi reality**: <0.1% packet loss means Opus FEC provides no benefit

**ADPCM Specifications**:
- Algorithm: IMA ADPCM (standard)
- Compression ratio: 4:1
- Input: 16-bit PCM @ 16kHz mono
- Output: 4-bit ADPCM @ 32 kbps
- Block size: 256 samples (16ms per block)

### Satellite Registration and Identification

Satellites self-identify on connection with a structured identity that enables:
- Session tracking across reconnections
- Per-satellite conversation history
- Location-aware responses ("the kitchen lights")
- Logging and debugging

**Registration Message**:

```json
{
  "type": "REGISTER",
  "identity": {
    "uuid": "550e8400-e29b-41d4-a716-446655440000",
    "name": "Kitchen Assistant",
    "location": "kitchen",
    "hardware_id": "rpi5-sn-12345678"
  },
  "tier": 1,
  "capabilities": {
    "local_asr": true,
    "local_tts": true,
    "wake_word": true
  },
  "hardware": {
    "platform": "rpi5",
    "memory_mb": 4096
  },
  "protocol_version": "2.0"
}
```

**Identity Fields**:

| Field | Required | Description | Example |
|-------|----------|-------------|---------|
| `uuid` | Yes | Unique identifier (persists across reboots) | UUID v4 or MAC-based |
| `name` | Yes | Human-readable name | "Kitchen Assistant" |
| `location` | Yes | Room/area for context | "kitchen", "bedroom", "office" |
| `hardware_id` | No | Hardware serial number | For inventory tracking |

**UUID Generation**:
- Tier 1 (RPi): Generate UUID v4 on first boot, store in `/etc/dawn-satellite-id`
- Tier 2 (ESP32): Derive UUID v5 from MAC address using DAWN namespace

**Tier 2 UUID v5 Generation**:
```c
// DAWN namespace UUID (randomly generated, fixed)
static const uint8_t DAWN_NAMESPACE_UUID[16] = {
   0x6b, 0xa7, 0xb8, 0x10, 0x9d, 0xad, 0x11, 0xd1,
   0x80, 0xb4, 0x00, 0xc0, 0x4f, 0xd4, 0x30, 0xc8
};

// ESP32 implementation:
// 1. Get MAC: uint8_t mac[6]; esp_read_mac(mac, ESP_MAC_WIFI_STA);
// 2. Create name string: "AA:BB:CC:DD:EE:FF"
// 3. SHA-1 hash: SHA1(namespace_uuid + mac_string)
// 4. Format as UUID v5: set version (0x50) and variant (0x80) bits
//
// Result: deterministic UUID like "f47ac10b-58cc-5372-8567-0e02b2c3d479"
//         Same MAC always produces same UUID (survives reflash)
```

**Why UUID v5?**
- Deterministic: Same MAC → same UUID (no storage needed on ESP32)
- Standard: RFC 4122 compliant
- Collision-free: SHA-1 hash provides uniqueness
- Survives firmware updates (derived from hardware, not stored)

**Registration Response**:

```json
{
  "type": "REGISTER_ACK",
  "session_id": "sess-abc123",
  "daemon_name": "DAWN Central",
  "conversation_restored": true,
  "conversation_age_seconds": 120
}
```

**Daemon Behavior by Tier**:
- **Tier 1**: Expects text QUERY, sends text RESPONSE
- **Tier 2**: Expects audio QUERY_AUDIO, sends audio RESPONSE_AUDIO

---

## 5. Protocol Specification

### 5.1 Packet Format

```
┌────────────────┬────────────────┬────────────────┬────────────────┐
│  Magic (2B)    │  Version (1B)  │  Type (1B)     │  Flags (1B)    │
├────────────────┴────────────────┴────────────────┴────────────────┤
│                        Payload Length (4B)                         │
├────────────────────────────────────────────────────────────────────┤
│                        Sequence Number (4B)                        │
├────────────────────────────────────────────────────────────────────┤
│                        Checksum (2B)                               │
├────────────────────────────────────────────────────────────────────┤
│                        Payload (variable)                          │
└────────────────────────────────────────────────────────────────────┘
```

**Total Header Size**: 14 bytes (vs 8 bytes in DAP 1.0)

| Field | Size | Description |
|-------|------|-------------|
| Magic | 2B | `0xDA 0x02` (Dawn Audio v2) |
| Version | 1B | Protocol version (0x02) |
| Type | 1B | Message type (see below) |
| Flags | 1B | Bit flags (streaming, compressed, etc.) |
| Payload Length | 4B | Length of payload in bytes |
| Sequence Number | 4B | Message sequence (for ordering/ack) |
| Checksum | 2B | CRC-16 of header + payload |

### 5.2 Message Types

| Value | Name | Direction | Payload |
|-------|------|-----------|---------|
| 0x01 | REGISTER | S→D | JSON capabilities |
| 0x02 | REGISTER_ACK | D→S | JSON session config |
| 0x10 | QUERY | S→D | UTF-8 text (transcribed command) |
| 0x11 | QUERY_AUDIO | S→D | Compressed audio (Tier 2/3 only) |
| 0x20 | RESPONSE | D→S | UTF-8 text (LLM response) |
| 0x21 | RESPONSE_STREAM | D→S | UTF-8 text chunk (streaming) |
| 0x22 | RESPONSE_END | D→S | Empty (end of stream) |
| 0x23 | RESPONSE_AUDIO | D→S | Audio chunk (Tier 2/3 only) |
| 0x30 | COMMAND | D→S | JSON command for satellite |
| 0x40 | STATUS | Both | JSON status/health |
| 0x50 | ACK | Both | Sequence number being acked |
| 0x51 | NACK | Both | Sequence number + error code |
| 0xF0 | PING | Both | Empty |
| 0xF1 | PONG | Both | Empty |

### 5.3 Flags

| Bit | Name | Description |
|-----|------|-------------|
| 0 | STREAMING | This is part of a stream (more to come) |
| 1 | COMPRESSED | Payload is compressed (Opus for audio, gzip for text) |
| 2 | REQUIRES_ACK | Sender expects ACK for this message |
| 3 | PRIORITY | High-priority message (interrupt handling) |
| 4-7 | Reserved | Future use |

### 5.4 Example Flows

#### Tier 1 Satellite (Full Local Processing)

```
Satellite                                    Daemon
    │                                           │
    │──── REGISTER {local_asr:true} ──────────>│
    │                                           │
    │<─── REGISTER_ACK {session_id:xxx} ───────│
    │                                           │
    │  [User says "Friday, turn on the lights"] │
    │  [Local: VAD → Wake Word → ASR]           │
    │                                           │
    │──── QUERY "turn on the lights" ─────────>│
    │                                           │
    │              [Daemon: LLM processing]     │
    │                                           │
    │<─── RESPONSE_STREAM "I'll turn" ─────────│
    │                                           │
    │  [Satellite: TTS "I'll turn"]             │
    │                                           │
    │<─── RESPONSE_STREAM " on the lights" ────│
    │                                           │
    │  [Satellite: TTS " on the lights"]        │
    │                                           │
    │<─── RESPONSE_END ────────────────────────│
    │                                           │
    │──── ACK ─────────────────────────────────>│
    │                                           │
```

#### Tier 2 Satellite (Audio Upload)

```
Satellite                                    Daemon
    │                                           │
    │──── REGISTER {local_asr:false} ─────────>│
    │                                           │
    │<─── REGISTER_ACK {session_id:xxx} ───────│
    │                                           │
    │  [User presses button, speaks]            │
    │  [Local: VAD detects silence]             │
    │                                           │
    │──── QUERY_AUDIO <opus_data> ────────────>│
    │                                           │
    │              [Daemon: ASR → LLM]          │
    │                                           │
    │<─── RESPONSE_AUDIO <opus_chunk> ─────────│
    │                                           │
    │  [Satellite: decode and play]             │
    │                                           │
    │<─── RESPONSE_AUDIO <opus_chunk> ─────────│
    │                                           │
    │<─── RESPONSE_END ────────────────────────│
    │                                           │
```

---

## 6. Common Code Architecture

### Monorepo Structure

Both the DAWN daemon and satellites share common code via a `common/` directory. This ensures implementations stay in sync and reduces maintenance burden.

```
dawn/
├── common/                        # SHARED CODE (daemon + satellites)
│   ├── include/
│   │   ├── audio/
│   │   │   └── ring_buffer.h
│   │   ├── asr/
│   │   │   ├── vad_silero.h
│   │   │   ├── asr_whisper.h
│   │   │   └── chunking_manager.h
│   │   ├── tts/
│   │   │   ├── text_to_speech.h
│   │   │   └── sentence_buffer.h
│   │   ├── protocol/
│   │   │   └── dap2.h             # DAP 2.0 protocol definitions
│   │   └── logging.h
│   ├── src/
│   │   ├── audio/
│   │   │   └── ring_buffer.c
│   │   ├── asr/
│   │   │   ├── vad_silero.c
│   │   │   ├── asr_whisper.c
│   │   │   └── chunking_manager.c
│   │   ├── tts/
│   │   │   ├── text_to_speech.cpp
│   │   │   ├── piper.cpp
│   │   │   └── sentence_buffer.c
│   │   ├── protocol/
│   │   │   └── dap2_common.c      # Packet encoding/decoding
│   │   └── logging.c
│   └── CMakeLists.txt             # Builds libdawn_common.a
│
├── src/                           # DAEMON-SPECIFIC CODE
│   ├── dawn.c                     # Main daemon entry point
│   ├── llm/                       # LLM integration (daemon only)
│   ├── network/
│   │   ├── dawn_server.c          # DAP 1.0 server (legacy)
│   │   ├── dap2_server.c          # DAP 2.0 server (NEW)
│   │   └── dap2_session.c         # Per-satellite session management
│   └── ...
│
├── satellite/                     # SATELLITE-SPECIFIC CODE
│   ├── tier1/                     # Raspberry Pi satellite
│   │   ├── src/
│   │   │   ├── satellite_main.c   # Entry point + state machine
│   │   │   ├── dap2_client.c      # DAP 2.0 client
│   │   │   ├── wake_word.c        # Wake word detection
│   │   │   └── audio_capture.c    # ALSA capture (satellite-specific)
│   │   ├── include/
│   │   └── CMakeLists.txt         # Links libdawn_common.a
│   │
│   └── tier2/                     # ESP32-S3 satellite
│       ├── main/
│       │   ├── satellite_main.c
│       │   ├── dap2_client.c
│       │   ├── adpcm_codec.c      # ADPCM encode/decode
│       │   └── i2s_audio.c        # I2S capture/playback
│       ├── components/
│       │   └── dap2_common/       # Subset of common code (C only)
│       └── CMakeLists.txt         # ESP-IDF build
│
├── models/                        # Shared models
│   ├── silero_vad_16k_op15.onnx
│   ├── ggml-tiny.en.bin
│   └── en_GB-alba-medium.onnx
│
└── CMakeLists.txt                 # Top-level build
```

### What Goes in Common vs Specific

| Component | Location | Reason |
|-----------|----------|--------|
| **Ring Buffer** | `common/` | Identical implementation |
| **VAD (Silero)** | `common/` | Identical implementation |
| **ASR (Whisper)** | `common/` | Identical, just different model sizes |
| **TTS (Piper)** | `common/` | Identical implementation |
| **Sentence Buffer** | `common/` | Identical implementation |
| **Logging** | `common/` | Identical implementation |
| **DAP 2.0 Protocol** | `common/` | Packet format, encode/decode |
| **Audio Capture** | `specific/` | ALSA (daemon/RPi) vs I2S (ESP32) |
| **Audio Playback** | `specific/` | Platform-specific |
| **LLM Interface** | `daemon only` | Satellites don't call LLM |
| **DAP Server** | `daemon only` | Only daemon accepts connections |
| **DAP Client** | `satellite only` | Only satellites connect out |
| **Wake Word Logic** | `satellite only` | Daemon uses main state machine |

### Build Configuration

```cmake
# common/CMakeLists.txt
add_library(dawn_common STATIC
    src/audio/ring_buffer.c
    src/asr/vad_silero.c
    src/asr/asr_whisper.c
    src/asr/chunking_manager.c
    src/tts/text_to_speech.cpp
    src/tts/piper.cpp
    src/tts/sentence_buffer.c
    src/protocol/dap2_common.c
    src/logging.c
)

target_include_directories(dawn_common PUBLIC include)

# satellite/tier1/CMakeLists.txt
add_executable(dawn_satellite
    src/satellite_main.c
    src/dap2_client.c
    src/wake_word.c
    src/audio_capture.c
)

target_link_libraries(dawn_satellite
    dawn_common          # Shared code
    whisper              # Whisper.cpp
    onnxruntime          # ONNX for VAD + TTS
    asound               # ALSA
    pthread
)
```

### ESP32 Considerations

The ESP32-S3 (Tier 2) cannot use C++ or ONNX runtime. For Tier 2:

- **No Whisper/VAD/TTS** - all processing on daemon
- **Subset of common code**: Only `ring_buffer.c`, `dap2_common.c`, `logging.c`
- **ADPCM codec**: ESP-IDF native or simple C implementation
- **Energy-based VAD**: Simple threshold on audio amplitude (not Silero)

---

## 7. Implementation Phases

### Phase 0: Code Refactoring (Foundation)

**Goal**: Extract shared code to `common/` library without breaking daemon

This phase addresses the architecture reviewer's critical finding: existing code has tight coupling to daemon-specific infrastructure (logging, metrics, AEC). Must decouple before any DAP 2.0 work.

**Code Standards**: All new code in `common/` MUST follow existing `.clang-format` rules. Run `./format_code.sh` before committing. Install pre-commit hook via `./install-git-hooks.sh`.

1. **Create logging abstraction**:
   ```c
   // common/include/logging_common.h
   typedef void (*log_callback_t)(int level, const char *fmt, ...);
   void dawn_common_set_logger(log_callback_t callback);

   // Daemon provides its logger, satellite provides its own
   ```

2. **Create metrics abstraction**:
   ```c
   // common/include/metrics_common.h
   typedef void (*metrics_callback_t)(const char *name, double value);
   void dawn_common_set_metrics_callback(metrics_callback_t callback);

   // Optional: satellite can ignore metrics, daemon feeds to TUI
   ```

3. **Decouple TTS from daemon-specific features**:
   - Remove AEC reference feed (daemon-only)
   - Remove command callback integration (daemon-only)
   - Keep mutex-protected synthesis as core functionality

4. **TTS C/C++ Interface Boundary**:

   Since TTS uses C++ (Piper, ONNX) but satellites may be C-based, create clean C interface:

   ```c
   // common/include/tts/tts_interface.h
   #ifdef __cplusplus
   extern "C" {
   #endif

   typedef struct TTSContext TTSContext;

   // Core TTS functions (go to common/)
   TTSContext *tts_init(const char *model_path);
   int tts_synthesize(TTSContext *ctx, const char *text, int16_t **audio, size_t *samples);
   void tts_cleanup(TTSContext *ctx);

   #ifdef __cplusplus
   }
   #endif
   ```

   **What goes to `common/`**:
   - `tts_interface.c/h` - C-compatible wrapper (NEW)
   - `text_to_speech.cpp` - Core synthesis (refactored)
   - `piper.cpp` - Piper integration (unchanged)
   - `sentence_buffer.c` - Text buffering (unchanged)

   **What stays daemon-specific**:
   - AEC reference signal feeding
   - Command callback integration (`text_to_command_nuevo.c`)
   - TUI metrics reporting (uses callback abstraction)

   **Tier 1 Satellite**: Links C++ TTS via `extern "C"` interface (RPi supports C++)
   **Tier 2 Satellite**: No TTS (audio streaming from daemon)

5. **Create `common/` directory structure**:
   - Move: ring_buffer, vad_silero, asr_whisper, asr_interface, chunking_manager
   - Move: text_to_speech, piper, sentence_buffer
   - Move: logging (with abstraction)
   - Create: `common/CMakeLists.txt` for `libdawn_common.a`

5. **Update daemon to use common library**:
   - Update daemon CMakeLists.txt to link `libdawn_common.a`
   - Provide daemon-specific callbacks for logging/metrics
   - **Verify daemon still builds and passes all tests**

**Deliverables**:
- `common/` directory with abstracted shared code
- `libdawn_common.a` static library
- `tts_interface.h` with C-compatible TTS API
- Daemon fully functional (no behavior change)
- Unit tests for:
  - Logging abstraction (callback registration, log levels)
  - Metrics abstraction (callback registration, optional metrics)
  - TTS C interface:
    - `tts_init()`: valid model path, invalid path (returns NULL), missing file
    - `tts_synthesize()`: single sentence, multiple sentences, empty string, long text (>1000 chars)
    - `tts_cleanup()`: normal cleanup, double-free protection, NULL context
    - Thread safety: multiple contexts in parallel, concurrent synthesize calls
    - Memory: no leaks on repeated init/synthesize/cleanup cycles (valgrind)

**Exit Criteria**:
- Daemon builds, runs, and behaves identically to pre-refactor
- All unit tests pass
- TTS synthesis works through new C interface

---

### Phase 1: DAP 2.0 Protocol Foundation

**Goal**: Define and implement DAP 2.0 protocol (TCP-only)

1. **Finalize protocol specification** (this document)
2. **Implement protocol library** in `common/`:
   - `dap2_common.c` - packet encode/decode
   - `dap2_common.h` - message types, structures
   - Unit tests for encoding/decoding

3. **Implement DAP 2.0 server** in daemon:
   - TCP server on port 5000
   - REGISTER → session creation
   - QUERY (text) → LLM → RESPONSE (text)
   - Session management per satellite (by UUID)

4. **Python test client**:
   - Verify protocol works end-to-end
   - Test: REGISTER, QUERY, RESPONSE, PING/PONG
   - Simulate Tier 1 satellite behavior

**Deliverables**:
- `common/src/protocol/dap2_common.c`
- `src/network/dap2_server.c`
- `src/network/dap2_session.c`
- Python test client
- Protocol unit tests

---

### Phase 2: Tier 1 Satellite (Raspberry Pi)

**Goal**: Full satellite with local ASR/TTS that feels identical to talking to the daemon

1. **Create `satellite/tier1/` directory**
2. **Implement satellite state machine** (mirrors daemon):
   ```
   SILENCE → WAKEWORD_LISTEN → COMMAND_RECORDING → PROCESSING → SPEAKING
   ```
3. **Implement DAP 2.0 client** (TCP)
4. **Implement wake word detection** (same as daemon: VAD + Whisper)
5. **Implement offline fallback**:
   - On network error: TTS "I can't reach the server right now"
6. **Integration testing** on RPi 4/5

**Deliverables**:
- `satellite/tier1/` with working code
- `dawn_satellite` binary for RPi
- Config file support (`/etc/dawn-satellite.conf`)
- User documentation

**Exit Criteria**: User cannot distinguish satellite from daemon interaction

---

### Phase 3: Streaming Responses

**Goal**: Stream LLM responses sentence-by-sentence for lower perceived latency

1. **Modify daemon** to send RESPONSE_STREAM messages
2. **Modify satellite** to:
   - Buffer incoming text
   - TTS each complete sentence immediately
   - Continue receiving while speaking
3. **Implement metrics collection**:
   - Track wake_word_latency, asr_latency, tts_latency
   - Report via STATUS messages

**Deliverables**:
- Streaming protocol working end-to-end
- Latency measurements documented
- Metrics visible in daemon TUI

**Exit Criteria**: First audio plays <1.5s after query (p95)

---

### Phase 4: Tier 2 Satellite (ESP32-S3)

**Goal**: Button-activated satellite following DAP 1.1 interaction model

1. **Create `satellite/tier2/` directory** (ESP-IDF project)
2. **Port DAP 2.0 client** to ESP32-S3
3. **Implement ADPCM codec**:
   - Encode: recorded audio → ADPCM
   - Decode: ADPCM response → PCM playback
4. **Implement energy-based VAD** (silence detection)
5. **Daemon support** for QUERY_AUDIO / RESPONSE_AUDIO:
   - Receive ADPCM → decode → ASR → LLM → TTS → encode → send ADPCM

**Deliverables**:
- `satellite/tier2/` ESP-IDF project
- Working ESP32-S3 firmware
- ADPCM streaming working over TCP

**ADPCM Quality Validation**:
- Round-trip test: record → encode → transmit → decode → play
- Subjective quality: speech clearly intelligible, no artifacts
- Objective metrics (if tooling available):
  - PESQ score >3.0 (good quality) or POLQA >3.5
  - SNR degradation <6dB vs. raw PCM
- Performance targets:
  - ESP32 CPU usage <20% for encode+decode at 16kHz
  - Encode latency <10ms per 16ms audio block
  - Decode latency <10ms per 16ms audio block
  - No audible glitches at sustained 16kHz streaming

**Exit Criteria**: Button-to-response <4s (p95)

---

### Phase 5: Multi-Satellite + Polish

**Goal**: Production-ready multi-satellite support

1. **Worker thread pool** in daemon (see `dawn_multi_client_architecture.md`):
   - Handle multiple concurrent satellites
   - Per-satellite LLM context
   - Reference implementation: one worker thread per satellite
2. **Session persistence**:
   - Save/restore conversation history
   - Handle reconnections gracefully
   - Storage format: JSON files in `/var/lib/dawn/sessions/`
   ```json
   // /var/lib/dawn/sessions/{uuid}.json
   {
     "uuid": "f47ac10b-58cc-5372-8567-0e02b2c3d479",
     "name": "Kitchen Assistant",
     "location": "kitchen",
     "tier": 1,
     "conversation": [
       {"role": "user", "content": "turn on the lights", "timestamp": "2025-11-29T10:30:00Z"},
       {"role": "assistant", "content": "I'll turn on the lights for you.", "timestamp": "2025-11-29T10:30:02Z"}
     ],
     "last_active": "2025-11-29T10:30:02Z",
     "created": "2025-11-29T08:00:00Z"
   }
   ```
   - Load on REGISTER (if UUID matches and file exists)
   - Save after each completed interaction
   - Prune conversations older than 24 hours on daemon startup
3. **Load testing**:
   - 5+ simultaneous satellites
   - Measure resource usage
4. **Documentation**:
   - User guide for satellite setup
   - Troubleshooting guide
   - API documentation

**Deliverables**:
- Multi-client support verified
- Performance benchmarks
- Complete documentation

---

### Phase 6: Optional Enhancements

**Goal**: Optimizations based on real-world measurements

1. **UDP audio streaming** (if TCP latency >150ms p95):
   - Add UDP channel for Tier 2 audio
   - Implement jitter buffer
2. **mDNS discovery**:
   - Daemon advertises `_dawn._tcp.local`
   - Satellites auto-discover daemon
3. **TLS encryption** (Phase 2+ security):
   - Certificate-based authentication
   - Encrypted connections

**Deliverables**: Based on Phase 5 measurements and user feedback

---

## 8. Platform Considerations

### Tier 1 Platforms

#### Raspberry Pi 5 (Recommended)

| Requirement | Available | Sufficient? |
|-------------|-----------|-------------|
| RAM | 4-8 GB | ✅ Yes (need ~280 MB) |
| CPU | Cortex-A76 2.4 GHz | ✅ Yes (Whisper tiny: ~1-2s) |
| Storage | 32+ GB SD | ✅ Yes (need ~200 MB for models) |
| Audio | USB mic + 3.5mm out | ✅ Yes |

**Estimated Performance**:
- Wake word detection: Real-time
- ASR (Whisper tiny): ~1-2s for 5s audio
- TTS (Piper): ~200-500ms per sentence

#### Raspberry Pi 4 (Supported)

| Requirement | Available | Sufficient? |
|-------------|-----------|-------------|
| RAM | 2-8 GB | ⚠️ 2GB tight, 4GB+ preferred |
| CPU | Cortex-A72 1.5 GHz | ✅ Yes (slower than Pi 5) |
| Storage | 32+ GB SD | ✅ Yes |
| Audio | USB mic + 3.5mm out | ✅ Yes |

**Estimated Performance**:
- Wake word detection: Real-time
- ASR (Whisper tiny): ~3-4s for 5s audio
- TTS (Piper): ~300-700ms per sentence

#### Orange Pi 5 (Supported)

| Requirement | Available | Sufficient? |
|-------------|-----------|-------------|
| RAM | 4-16 GB | ✅ Yes |
| CPU | RK3588S (4x A76 + 4x A55) | ✅ Yes (faster than RPi 5) |
| Storage | eMMC or SD | ✅ Yes |
| Audio | USB mic + 3.5mm out | ✅ Yes |

**Note**: NPU not used (would require RKNN model conversion)

### Tier 2 Platforms

#### ESP32-S3 (Recommended)

| Requirement | Available | Sufficient? |
|-------------|-----------|-------------|
| RAM | 512 KB + 8 MB PSRAM | ✅ Yes for audio buffering |
| CPU | Xtensa LX7 240 MHz | ✅ Yes for ADPCM + networking |
| Storage | 16 MB flash | ✅ Yes for firmware |
| Audio | I2S mic + I2S DAC | ✅ Yes |

**Capabilities**:
- Button-activated recording
- Energy-based VAD (silence detection)
- ADPCM encode/decode
- Cannot run Whisper or Piper locally

#### ESP32 Classic (Limited Support)

| Requirement | Available | Notes |
|-------------|-----------|-------|
| RAM | 520 KB (no PSRAM on most) | ⚠️ Very tight |
| CPU | Xtensa LX6 240 MHz | ✅ OK for ADPCM |

**Limitations**:
- May need smaller audio buffers
- No PSRAM limits recording duration
- Consider ESP32-S3 instead

---

## 9. Security Architecture

### Threat Model

| Threat | Impact | Phase 1 Mitigation | Phase 2+ Mitigation |
|--------|--------|-------------------|---------------------|
| **Eavesdropping** | Voice commands exposed | Network isolation (VLAN) | TLS 1.3 encryption |
| **Impersonation** | Unauthorized satellite | Trusted network only | Certificate-based auth |
| **Man-in-the-middle** | Command injection | Trusted network only | Mutual TLS (mTLS) |
| **Replay attacks** | Duplicate commands | Sequence numbers | Timestamps + nonce |
| **DoS attacks** | Service disruption | None | Rate limiting |

### Phase 1: Network Isolation (MVP)

For initial deployment, rely on network-level security:

```
┌─────────────────────────────────────────────────────────────┐
│                    Home Network                              │
│                                                              │
│  ┌─────────────────────────────────────────────────────────┐│
│  │              DAWN VLAN (isolated)                        ││
│  │                                                          ││
│  │  ┌──────────┐  ┌──────────┐  ┌──────────┐              ││
│  │  │  DAWN    │  │ Satellite│  │ Satellite│              ││
│  │  │  Daemon  │  │  (RPi)   │  │  (ESP32) │              ││
│  │  └──────────┘  └──────────┘  └──────────┘              ││
│  │                                                          ││
│  └─────────────────────────────────────────────────────────┘│
│                                                              │
│  ┌─────────────────────────────────────────────────────────┐│
│  │              Regular VLAN (untrusted)                    ││
│  │  Phones, laptops, IoT devices, guests                   ││
│  └─────────────────────────────────────────────────────────┘│
└─────────────────────────────────────────────────────────────┘
```

**Phase 1 Requirements**:
- Document security limitations in user guide
- Recommend VLAN isolation for DAWN devices
- No authentication (trusted network assumption)
- No encryption (local network only)

**Example VLAN Setup** (UniFi/OpenWrt):
```
# OpenWrt: Create DAWN VLAN (ID 10)
uci set network.dawn_vlan=interface
uci set network.dawn_vlan.proto='static'
uci set network.dawn_vlan.ipaddr='192.168.10.1'
uci set network.dawn_vlan.netmask='255.255.255.0'
uci set network.dawn_vlan.device='br-lan.10'
uci commit network

# Firewall: Isolate DAWN VLAN from main network
uci add firewall zone
uci set firewall.@zone[-1].name='dawn'
uci set firewall.@zone[-1].input='ACCEPT'
uci set firewall.@zone[-1].output='ACCEPT'
uci set firewall.@zone[-1].forward='REJECT'
uci set firewall.@zone[-1].network='dawn_vlan'
uci commit firewall

# Result:
#   DAWN devices: 192.168.10.0/24 (can only talk to each other)
#   Main network: 192.168.1.0/24 (isolated from DAWN)
#   Daemon port 5000 only accessible from DAWN VLAN
```

**Alternative: WiFi SSID Isolation**:
- Create separate "DAWN-Satellites" SSID on VLAN 10
- Simpler for most home users
- ESP32 and RPi satellites connect to dedicated SSID

### Phase 2+: TLS and Authentication

**TLS 1.3 for All Connections**:
```c
// Server-side (daemon)
SSL_CTX *ctx = SSL_CTX_new(TLS_server_method());
SSL_CTX_use_certificate_file(ctx, "/etc/dawn/daemon.crt", SSL_FILETYPE_PEM);
SSL_CTX_use_PrivateKey_file(ctx, "/etc/dawn/daemon.key", SSL_FILETYPE_PEM);

// Client-side (satellite)
SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
SSL_CTX_load_verify_locations(ctx, "/etc/dawn/ca.crt", NULL);
```

**Certificate-Based Authentication**:
- Daemon has CA certificate, issues satellite certificates
- Satellites present certificate on REGISTER
- Daemon validates certificate before accepting connection
- Revocation via certificate revocation list (CRL)

**Best Practices Applied**:
- TLS 1.3 only (no fallback to older versions)
- Strong cipher suites (AES-256-GCM, ChaCha20-Poly1305)
- Certificate pinning on satellites
- Regular certificate rotation (yearly)

---

## 10. Network Reliability

### Connection Management

Following DAP 1.1 patterns and MQTT best practices:

**Keepalive Mechanism**:
```
┌───────────┐                              ┌───────────┐
│ Satellite │                              │  Daemon   │
└─────┬─────┘                              └─────┬─────┘
      │                                          │
      │──────────── PING ───────────────────────>│
      │                                          │
      │<─────────── PONG ────────────────────────│
      │                                          │
      │         (60 seconds later)               │
      │                                          │
      │──────────── PING ───────────────────────>│
      │                                          │
      │         (no PONG received)               │
      │                                          │
      │──────────── PING (retry 1) ─────────────>│
      │                                          │
      │         (still no PONG)                  │
      │                                          │
      │──────────── PING (retry 2) ─────────────>│
      │                                          │
      │         [3 missed → disconnect]          │
```

**Keepalive Parameters** (following MQTT conventions):
| Parameter | Value | Rationale |
|-----------|-------|-----------|
| Ping interval | 60 seconds | Balance between detection speed and overhead |
| Pong timeout | 10 seconds | Allow for network latency |
| Max missed pongs | 3 | Avoid false disconnects on transient issues |
| Disconnect threshold | 90 seconds (1.5x interval) | MQTT standard |

### Session Recovery

**On Satellite Disconnect**:
1. Daemon marks session as "disconnected" (not deleted)
2. Conversation history preserved for 10 minutes
3. Satellite UUID used to match reconnection

**On Satellite Reconnect**:
1. Satellite sends REGISTER with same UUID
2. Daemon matches UUID to existing session
3. Returns `conversation_restored: true` if history available
4. Satellite continues conversation seamlessly

**Session Cleanup**:
- Idle timeout: 10 minutes (no queries)
- Disconnect timeout: 5 minutes (no reconnection)
- Memory cleanup on session expiration

### Reconnection Strategy

**Satellite Responsibility**: The satellite (client) implements reconnection logic. The daemon (server) does NOT attempt to reconnect to satellites.

**On Satellite Disconnect** (network failure or timeout):
```
┌───────────────────────────────────────────────────────────────┐
│                    Satellite Reconnection                      │
├───────────────────────────────────────────────────────────────┤
│  1. Detect disconnect:                                         │
│     - TCP socket error (send/receive fails)                    │
│     - 3 consecutive missed PONGs (90s timeout)                 │
│                                                                │
│  2. Enter reconnection loop:                                   │
│     ┌─────────────────────────────────────────────────────┐   │
│     │  Attempt 1: wait 1s  → connect + REGISTER           │   │
│     │  Attempt 2: wait 2s  → connect + REGISTER           │   │
│     │  Attempt 3: wait 4s  → connect + REGISTER           │   │
│     │  Attempt 4: wait 8s  → connect + REGISTER           │   │
│     │  Attempt 5: wait 16s → connect + REGISTER           │   │
│     │  Attempt 6+: wait 30s (max) → connect + REGISTER    │   │
│     └─────────────────────────────────────────────────────┘   │
│                                                                │
│  3. On REGISTER_ACK received:                                  │
│     - Reset backoff timer to 1s                                │
│     - Resume normal operation                                  │
│     - If conversation_restored=true, continue context          │
│                                                                │
│  4. During reconnection:                                       │
│     - Tier 1: TTS "I've lost connection to the server"        │
│     - Tier 2: LED indicator (blinking = reconnecting)          │
└───────────────────────────────────────────────────────────────┘
```

**Daemon Behavior During Satellite Disconnect**:
- Marks session as "disconnected" (not deleted)
- Preserves conversation history for 10 minutes
- Accepts reconnection via new TCP connection + REGISTER
- Matches reconnecting satellite by UUID

**Why Exponential Backoff?**
- Prevents "thundering herd" if daemon restarts
- Reduces network congestion during outages
- 30s max cap ensures reasonable recovery time
- Industry standard pattern (TCP, MQTT, gRPC)

### Packet Reliability (TCP Mode)

Since Phase 1-3 uses TCP only, reliability is built-in:
- Automatic retransmission
- In-order delivery
- Congestion control

**Application-Level Reliability**:
- Sequence numbers in packets (for logging/debugging)
- ACK messages for request/response correlation
- Timeout handling (30 seconds for LLM responses)

### Future: UDP Reliability (Phase 4+)

If UDP is added for audio streaming:

**Jitter Buffer**:
- Size: 50-100ms (adaptive)
- Reorder window: 5 packets
- Late packet policy: Discard if >100ms late

**Packet Loss Handling**:
- Accept glitches (no retransmission for audio)
- Sequence numbers for gap detection
- Interpolation for single lost packets (PLC)

---

## 11. Performance Validation

### Metrics Collection

Extend the existing DAWN daemon metrics infrastructure to satellites and network clients:

**Satellite Metrics** (collected locally, reported to daemon):
| Metric | Description | Target |
|--------|-------------|--------|
| `wake_word_latency_ms` | Time from speech end to wake word detection | <500ms p95 |
| `asr_latency_ms` | Time to transcribe command | <2000ms p95 |
| `tts_latency_ms` | Time to synthesize first audio | <500ms p95 |
| `network_rtt_ms` | Round-trip time to daemon | <50ms p95 |

**Daemon Metrics** (per satellite):
| Metric | Description | Target |
|--------|-------------|--------|
| `llm_ttft_ms` | Time to first LLM token | <500ms p95 |
| `llm_total_ms` | Total LLM response time | <3000ms p95 |
| `e2e_latency_ms` | Query received → response sent | <4000ms p95 |
| `active_satellites` | Current connected satellites | N/A |
| `queries_per_minute` | Query rate per satellite | N/A |

**End-to-End Targets** (user-perceived):
| Interaction | Target | Measurement |
|-------------|--------|-------------|
| Wake word → acknowledgment | <1.5s | First TTS audio plays |
| Command → first response audio | <2.5s | Streaming response starts |
| Total interaction | <8s | Response audio completes |

### Metrics Reporting

**Satellite → Daemon** (periodic STATUS message):
```json
{
  "type": "STATUS",
  "metrics": {
    "wake_word_latency_p95_ms": 420,
    "asr_latency_p95_ms": 1800,
    "tts_latency_p95_ms": 380,
    "uptime_seconds": 86400,
    "queries_total": 142
  }
}
```

**Daemon Logging** (compatible with existing TUI):
```c
// Extend metrics.h for satellite tracking
typedef struct {
   char satellite_id[64];
   char satellite_name[64];
   char satellite_location[32];
   uint32_t queries_total;
   uint32_t avg_latency_ms;
   time_t last_query_time;
   bool connected;
} satellite_metrics_t;
```

### Validation Plan

**Pre-Implementation Benchmarks**:
1. Measure Whisper tiny on RPi 4, RPi 5, Orange Pi 5
2. Measure Piper TTS latency per sentence length
3. Measure TCP round-trip on typical home WiFi

**Phase Completion Criteria**:
| Phase | Metric | Pass Criteria |
|-------|--------|---------------|
| Phase 2 | Tier 1 wake-to-response | <3s p95 |
| Phase 3 | Streaming first audio | <1.5s p95 |
| Phase 4 | Tier 2 button-to-response | <4s p95 |

---

## 12. Open Questions

### Decided

| Question | Decision | Rationale |
|----------|----------|-----------|
| **Transport** | TCP-first, UDP optional (Phase 6) | Simpler implementation; home WiFi doesn't need UDP |
| **Audio Codec** | ADPCM only (Tier 2) | Simpler, lower resources; Opus adds 240KB to ESP32 |
| **Wake Word (Tier 1)** | Same as daemon (VAD + Whisper) | Seamless UX, code reuse |
| **Wake Word (Tier 2)** | None (button-activated, DAP 1.1 style) | ESP32 can't run Whisper |
| **Offline Mode** | TTS "I can't reach the server" | Simple, clear user feedback |
| **Code Sharing** | Monorepo with `common/` | Single source of truth, easy builds |
| **Tiers** | 2 tiers only | RPi (full local) and ESP32 (audio upload) |
| **Satellite Identity** | UUID + name + location | Session tracking, conversation history |
| **Session Timeout** | 10 min idle, 60s keepalive | MQTT best practices |
| **Security (Phase 1)** | Network isolation (VLAN) | Document limitations, recommend isolation |
| **Security (Phase 2+)** | TLS 1.3 + certificates | Best practices for production |
| **Refactoring Phase** | Phase 0 before protocol work | Decouple logging/metrics first |
| **Reconnection Backoff** | Exponential: 1s, 2s, 4s, 8s, max 30s | Industry standard, prevents thundering herd |

### Research-Based Decisions

#### 1. Streaming Granularity: **Sentence-level**

**Decision**: Stream at sentence boundaries (`.`, `?`, `!`, `;`, `\n`)

**Research**: Analysis of Alexa, Google Assistant, and Siri shows they "almost exclusively respond in full sentences" ([ACM CHI 2022](https://dl.acm.org/doi/fullHtml/10.1145/3491102.3517684)). Voice assistant best practices recommend 40-90 spoken words per response, with sub-1.5s end-to-end latency targets.

**Rationale**:
- Matches our existing `sentence_buffer.c` implementation
- Natural pauses between sentences sound better than mid-word cuts
- Simpler buffering logic than word/token level
- Token-level streaming sounds choppy with TTS

#### 2. Session Timeout: **10 minutes with keepalive**

**Decision**:
- Conversation history: 10 minute idle timeout
- TCP keepalive: 60 second PING/PONG interval
- Disconnect after: 1.5x keepalive (90 seconds) without response

**Research**: MQTT protocol uses 1.5x keepalive as disconnect threshold ([HiveMQ](https://www.hivemq.com/blog/mqtt-essentials-part-10-alive-client-take-over/)). AWS IoT recommends minimum 4 minute timeout for IoT devices. Smart home devices use keepalive to confirm connectivity during idle periods.

**Rationale**:
- 10 min matches `dawn_multi_client_architecture.md`
- 60s keepalive catches network drops quickly without excessive traffic
- 1.5x multiplier is industry standard (MQTT)

#### 3. Satellite Configuration: **Config file + future mDNS**

**Decision**: Config file initially, with mDNS discovery as Phase 2+ enhancement

**Config file** (`/etc/dawn-satellite.conf`):
```ini
[daemon]
host = 192.168.1.100
tcp_port = 5000
udp_port = 5001

[satellite]
id = kitchen-pi
tier = 1
```

**Future: mDNS/DNS-SD Discovery**:
- Daemon advertises `_dawn._tcp.local`
- Satellites auto-discover without config
- Similar to: Chromecast, AirPlay, Home Assistant
- Protocols: Avahi (Linux), Bonjour (Apple)

#### 4. UDP Packet Loss (Phase 6, if needed)

**Decision**: TCP-first approach; UDP only added if measurements show benefit

If UDP is added in Phase 6, follow WebRTC best practices ([GetStream](https://getstream.io/resources/projects/webrtc/advanced/media-resilience/)):

| Technique | How it works | When to use |
|-----------|--------------|-------------|
| **Sequence numbers** | Order packets, detect gaps | Always |
| **Jitter buffer** | 50-100ms buffer before playback | Always |
| **Accept glitches** | Don't retransmit lost audio | Home WiFi (<0.1% loss) |
| **PLC** | Decoder interpolates missing audio | Optional enhancement |

**Note**: Since we're using ADPCM only (no Opus), FEC is not available. This is acceptable for home WiFi deployments.

### Still Open (Minor)

1. **LLM context per satellite**: Should each satellite have independent conversation history?
   - Current plan: Yes, per-satellite context identified by UUID
   - Open: Maximum context length per satellite?

---

## 13. Memory System Integration

### User Mapping for Satellites

DAP2 satellites operate as "guest" sessions by default and do NOT store memories. This prevents visitors or other household members from polluting a user's memory profile.

**Configuration for memory-enabled satellites:**

```toml
[dap2.satellites]
# Map satellite UUIDs to authenticated users for memory storage
# Only mapped satellites will have memories stored
# Unmapped satellites operate in guest mode (no memory)

[dap2.satellites."550e8400-e29b-41d4-a716-446655440000"]
name = "Kitchen Assistant"
user = "krisk"              # Map to user for memory storage

[dap2.satellites."660f9511-f3ac-52e5-b827-557766551111"]
name = "Office Assistant"
user = "tomp"               # Different user

[dap2.satellites."770a0622-g4bd-63f6-c938-668877662222"]
name = "Guest Room"
# No user mapping = guest mode, no memory storage
```

**Behavior:**
- **Mapped satellite**: Queries associated with configured user, memories stored and retrieved
- **Unmapped satellite**: Guest mode, no memory storage, core facts not injected
- **Registration response** includes memory status:
  ```json
  {
    "type": "REGISTER_ACK",
    "session_id": "sess-abc123",
    "memory_enabled": true,
    "memory_user": "krisk"
  }
  ```

**Why not auto-create users for satellites?**
- Security: Prevents unauthorized memory accumulation
- Privacy: Explicit opt-in for memory features
- Simplicity: Reuses existing auth user database
- Future: Speaker identification will provide automatic user resolution

See `MEMORY_SYSTEM_DESIGN.md` for full memory system specification.

---

## Appendix A: Comparison with DAP 1.0

| Aspect | DAP 1.0 | DAP 2.0 |
|--------|---------|---------|
| **Primary payload** | Audio (WAV) | Text (UTF-8) |
| **Header size** | 8 bytes | 14 bytes |
| **Message types** | 6 | 12+ |
| **Streaming** | No | Yes |
| **Wake word** | No (button) | Yes (local) |
| **VAD** | No (button) | Yes (local) |
| **ASR location** | Server only | Server or local |
| **TTS location** | Server only | Server or local |
| **Offline mode** | No | Yes (Tier 1) |
| **Bandwidth** | ~1 MB/interaction | ~1 KB/interaction (Tier 1) |

## Appendix B: Related Documents

- `protocol_specification.md` - DAP 1.0 specification
- `dawn_multi_client_architecture.md` - Multi-client threading design
- `ARCHITECTURE.md` - DAWN system architecture
- `LLM_INTEGRATION_GUIDE.md` - LLM setup and configuration

---

**Document History**:
- v0.1 (Nov 2025): Initial draft
