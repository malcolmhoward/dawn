# CLAUDE.md - LLM Integration Guide

## Project Overview

**DAWN** (Digital Assistant with Natural-language Workflow) is the AI voice assistant component of the O.A.S.I.S. ecosystem. It provides speech recognition, natural language processing, and text-to-speech on NVIDIA Jetson platforms.

## Build System

| Aspect | Details |
|--------|---------|
| Language | C (with C++ for TTS/Piper integration) |
| Build Tool | CMake |
| Target Platform | NVIDIA Jetson (Nano, Orin Nano, NX) |
| Key Dependencies | Vosk, Piper, ONNX Runtime, Mosquitto (MQTT), OpenAI API |

### Building

```bash
mkdir build && cd build
cmake ..
make -j$(nproc)
```

### Configuration

- `commands_config_nuevo.json`: Command configuration
- `secrets.h`: API keys and sensitive config - **do not commit real secrets**

## Key Files

| File | Purpose |
|------|---------
| `dawn.c` / `.h` | Main entry point, event loop, initialization |
| `mosquitto_comms.c` / `.h` | MQTT communication with MIRAGE, AURA, SPARK |
| `openai.c` / `.h` | LLM API integration (OpenAI compatible) |
| `text_to_speech.cpp` / `.h` | Piper TTS integration |
| `piper.cpp` / `.hpp` | Piper engine wrapper |
| `text_to_command_nuevo.c` / `.h` | Command parsing and execution |
| `llm_command_parser.c` / `.h` | LLM response parsing |
| `flac_playback.c` / `.h` | Music/audio playback |
| `logging.c` / `.h` | Logging utilities |

## Architecture

### Voice Pipeline

```
Microphone → Vosk (STT) → Command Parser → LLM (if needed) → Action
                                                    ↓
                                              Piper (TTS) → Speaker
```

### Communication (MQTT)

DAWN uses MQTT via Mosquitto for inter-component communication:

```
DAWN <--MQTT--> MIRAGE (display requests, image capture)
DAWN <--MQTT--> AURA (sensor queries)
DAWN <--MQTT--> SPARK (armor control)
```

Key topics defined in `mosquitto_comms.h`.

### LLM Integration

- OpenAI-compatible API (`openai.c`)
- Vision capabilities (image analysis via MIRAGE capture)
- Command extraction from natural language
- Configurable endpoint (local or cloud)

## Hardware Dependencies

| Hardware | Purpose | Configuration |
|----------|---------|---------------|
| Jetson GPU | ONNX inference, CUDA acceleration | JetPack SDK required |
| Microphone | Voice input | PulseAudio |
| Speaker | TTS output, music playback | PulseAudio |

## O.A.S.I.S. Ecosystem Context

DAWN is part of the O.A.S.I.S. ecosystem coordinated by [S.C.O.P.E.](https://github.com/malcolmhoward/the-oasis-project-meta-repo):

| Component | Interaction |
|-----------|-------------|
| **MIRAGE** | Sends TTS notifications, receives image capture for vision AI |
| **AURA** | Queries sensor data (motion, environment, GPS) |
| **SPARK** | Sends armor control commands |

For ecosystem-level coordination, see S.C.O.P.E. roadmaps and documentation.

## Common Tasks

### Adding a New Voice Command

1. Define command pattern in `commands_config_nuevo.json`
2. Add handler in `text_to_command_nuevo.c`
3. Implement action (may involve MQTT messages to other components)

### Adding a New MQTT Topic

1. Define topic constant in `mosquitto_comms.h`
2. Add subscription in `mosquitto_comms.c`
3. Add handler for incoming messages

### Debugging

- Logs via `logging.c` - check `logging.h` for levels
- MQTT: Use `mosquitto_sub` to monitor topics
- Audio: Check PulseAudio configuration

## Testing

Currently manual testing on hardware.

## Conventions

- C11/C++11 standards
- 4-space indentation
- `snake_case` for functions and variables
- `UPPER_CASE` for constants and macros

## Security Considerations

- `secrets.h`: Contains API keys - **do not commit real secrets**
- MQTT: Currently unencrypted on local network - production should use TLS
- LLM API: Keys should be environment variables in production

---

*For contribution guidelines, see [CONTRIBUTING.md](CONTRIBUTING.md).*
*For ecosystem coordination, see [S.C.O.P.E.](https://github.com/malcolmhoward/the-oasis-project-meta-repo).*

## Branch Naming Convention

**Critical**: Branch names must include the GitHub issue number being addressed.

### Format
```
feat/<component>/<issue#>-<short-description>
```

### Before Creating a Branch

1. **Identify the issue** you're working on (check GitHub Issues)
2. **Use that issue's number** in the branch name
3. **Verify** the issue number matches the work being done

### Examples
```bash
# Check available issues first
gh issue list --repo malcolmhoward/dawn

# Create branch with correct issue number
git checkout -b feat/dawn/<issue#>-description
```

### Common Mistake
❌ Using arbitrary numbers or the wrong issue number
✅ Always check `gh issue list` or GitHub Issues before creating a branch
