# DAWN Docker Guide

Docker configurations for building and running DAWN on multiple platforms.

## Overview

DAWN provides three platform-specific Dockerfiles:

| Dockerfile | Base Image | Target Platform | Whisper |
|-----------|-----------|-----------------|---------|
| `Dockerfile.dev` | `ubuntu:22.04` | x86/x64 | CPU (base.en) |
| `Dockerfile.jetson` | `nvcr.io/nvidia/l4t-base:r35.4.1` | NVIDIA Jetson (ARM64 Tegra) | CUDA-accelerated (base.en) |
| `Dockerfile.rpi` | `arm64v8/debian:bookworm-slim` | Raspberry Pi (ARM64) | CPU (tiny.en) |

Each Dockerfile is fully self-contained — see [ADR-0005](https://github.com/malcolmhoward/the-oasis-project-meta-repo/blob/main/coordination/decisions/adr/0005-dockerfile-independence.md) for rationale.

The `entrypoint.sh` handles Whisper model downloads and config setup at container startup. See [ADR-0006](https://github.com/malcolmhoward/the-oasis-project-meta-repo/blob/main/coordination/decisions/adr/0006-container-model-availability-strategy.md) for the model availability strategy.

---

## Prerequisites

### All Platforms

- **Submodules initialized** (required for build):
  ```bash
  git submodule update --init --recursive
  ```
- **Docker installed and running** — see [Docker Engine install guide](https://docs.docker.com/engine/install/) if needed
- An API key for at least one LLM provider (OpenAI, Anthropic, Gemini) or a local LLM server

### NVIDIA Jetson

1. **NVIDIA Docker runtime**:
   ```bash
   sudo apt-get update
   sudo apt-get install -y docker.io nvidia-docker2
   sudo systemctl restart docker
   ```
2. **Audio configured** (for local voice interface)

### Raspberry Pi

1. **64-bit OS** required (Raspberry Pi OS 64-bit or Ubuntu for Pi)
2. **Docker installed**:
   ```bash
   curl -fsSL https://get.docker.com | sh
   sudo usermod -aG docker $USER  # re-login after
   ```
   For alternative install methods or if the script fails, see the
   [Docker Engine install guide](https://docs.docker.com/engine/install/).
3. **Audio configured** (for local voice interface)

---

## Building

```bash
# Development (x86/x64)
docker build -f Dockerfile.dev -t dawn:dev .

# NVIDIA Jetson (build on Jetson hardware)
docker build -f Dockerfile.jetson -t dawn:jetson .

# Raspberry Pi (build on Pi hardware)
docker build -f Dockerfile.rpi -t dawn:rpi .
```

> **Build time**: DAWN has many dependencies (espeak-ng, ONNX Runtime, piper-phonemize, WebRTC AEC, DAWN itself). Expect 20–40 minutes on first build. Subsequent builds use Docker layer cache.

---

## Running

### Quick Start (no Whisper download)

By default, `SKIP_MODEL_DOWNLOAD=true` — the container starts immediately without downloading models. DAWN's ASR will fail gracefully until a Whisper model is present.

```bash
# Dev
docker run --rm -it -p 3000:3000 dawn:dev

# Jetson
docker run --rm -it --runtime=nvidia -p 3000:3000 --device /dev/snd dawn:jetson

# Raspberry Pi
docker run --rm -it -p 3000:3000 --device /dev/snd dawn:rpi
```

Access the Web UI at `http://localhost:3000`.

### Download Whisper Model on First Run

Set `SKIP_MODEL_DOWNLOAD=false` to download the missing Whisper model on startup.

```bash
# Dev: download base.en (~142MB)
docker run --rm -it -p 3000:3000 \
  -e SKIP_MODEL_DOWNLOAD=false \
  dawn:dev

# RPi: download tiny.en (~75MB, faster on Pi hardware)
docker run --rm -it -p 3000:3000 \
  --device /dev/snd \
  -e SKIP_MODEL_DOWNLOAD=false \
  dawn:rpi

# Use a different Whisper model (base is default; small for better accuracy)
docker run --rm -it -p 3000:3000 \
  -e SKIP_MODEL_DOWNLOAD=false \
  -e WHISPER_MODEL=small.en \
  dawn:dev
```

Models already present are never re-downloaded — safe to restart with `SKIP_MODEL_DOWNLOAD=false`.

### Using a Persistent Whisper Volume

```bash
# Create a named volume for Whisper models
docker volume create dawn-whisper

# First run: download model into volume
docker run --rm -it -p 3000:3000 \
  -v dawn-whisper:/opt/dawn/whisper.cpp/models \
  -e SKIP_MODEL_DOWNLOAD=false \
  dawn:dev

# Subsequent runs: model already in volume, skip download
docker run --rm -it -p 3000:3000 \
  -v dawn-whisper:/opt/dawn/whisper.cpp/models \
  dawn:dev
```

### Setting an API Key

```bash
# Pass API key as environment variable
docker run --rm -it -p 3000:3000 \
  -e OPENAI_API_KEY="sk-your-key" \
  dawn:dev

# Or mount a secrets.toml file
cp secrets.toml.example secrets.toml
# Edit secrets.toml with your keys
docker run --rm -it -p 3000:3000 \
  -v "$(pwd)/secrets.toml:/opt/dawn/secrets.toml:ro" \
  dawn:dev
```

### Mounting Custom Config

```bash
cp dawn.toml.example my-dawn.toml
# Edit my-dawn.toml as needed
docker run --rm -it -p 3000:3000 \
  -v "$(pwd)/my-dawn.toml:/opt/dawn/dawn.toml:ro" \
  dawn:dev
```

Without a mounted `dawn.toml`, the entrypoint copies the example config on first run.

---

## Model Download Reference

| Environment Variable | Default | Description |
|---------------------|---------|-------------|
| `SKIP_MODEL_DOWNLOAD` | `true` | Global: skip all model downloads |
| `SKIP_WHISPER` | (inherits) | Skip Whisper download; inherits `SKIP_MODEL_DOWNLOAD` unless overridden |
| `WHISPER_MODEL` | `base.en` (dev/jetson), `tiny.en` (rpi) | Whisper model variant to use/download |

**Whisper model sizes** (English-only variants):

| Model | Size | Speed | Use Case |
|-------|------|-------|----------|
| `tiny.en` | ~75MB | Fastest | Raspberry Pi |
| `base.en` | ~142MB | Fast | Jetson (GPU), dev/CI |
| `small.en` | ~466MB | Moderate | Higher accuracy |
| `medium.en` | ~1.5GB | Slow | Best accuracy |

**TTS and VAD models** (Piper voice + Silero VAD) are committed to git and always present in the image — no download required.

---

## Platform Differences

| Feature | Dockerfile.dev | Dockerfile.jetson | Dockerfile.rpi |
|---------|---------------|-------------------|----------------|
| Architecture | x86/x64 | ARM64 (Tegra) | ARM64 (RPi) |
| Base image | ubuntu:22.04 | nvcr.io/nvidia/l4t-base | arm64v8/debian:bookworm-slim |
| Whisper inference | CPU | CUDA (GPU-accelerated) | CPU |
| Default Whisper model | base.en | base.en | tiny.en |
| NVIDIA runtime required | No | Yes (`--runtime=nvidia`) | No |
| Audio passthrough | Optional | Recommended (`--device /dev/snd`) | Recommended (`--device /dev/snd`) |
| Local LLM | Possible | Recommended (Jetson GPU) | Not recommended (RAM) |

---

## Audio Device Passthrough

DAWN requires audio for the local voice interface. Without audio passthrough, the Web UI still works (text mode) but the wake word detection and voice commands won't function.

```bash
# Pass through audio devices
docker run --rm -it -p 3000:3000 \
  --device /dev/snd \
  dawn:dev

# With PulseAudio socket (if host uses PulseAudio)
docker run --rm -it -p 3000:3000 \
  --device /dev/snd \
  -e PULSE_SERVER=unix:${XDG_RUNTIME_DIR}/pulse/native \
  -v ${XDG_RUNTIME_DIR}/pulse/native:${XDG_RUNTIME_DIR}/pulse/native \
  dawn:dev
```

To list available audio devices in the container:
```bash
docker exec <container> arecord -L  # microphone
docker exec <container> aplay -L    # speakers
```

Then set the device in `dawn.toml`:
```toml
[audio]
capture_device = "hw:0,0"   # or "default"
playback_device = "hw:0,0"
```

---

## LLM Configuration

DAWN supports multiple LLM providers. Configure in `secrets.toml` or environment variables:

| Provider | Variable | Notes |
|----------|----------|-------|
| OpenAI | `OPENAI_API_KEY` | Recommended for cloud |
| Anthropic (Claude) | `ANTHROPIC_API_KEY` | |
| Google Gemini | `GEMINI_API_KEY` | |
| Local llama.cpp | Set `dawn.toml` `[llm.local]` endpoint | Jetson-recommended |
| Ollama | Set `dawn.toml` `[llm.local]` endpoint | Easier model management |

For local LLM on Jetson:
```bash
docker run --rm -it --runtime=nvidia -p 3000:3000 \
  --network host \
  --device /dev/snd \
  -e SKIP_MODEL_DOWNLOAD=false \
  dawn:jetson
```

Use `--network host` so DAWN can reach an Ollama or llama.cpp server on the host.

---

## Multi-Component Development

For running DAWN alongside other O.A.S.I.S. components (MIRAGE, S.T.A.T.), use the `docker-compose.yml` in the [S.C.O.P.E. meta-repo](https://github.com/malcolmhoward/the-oasis-project-meta-repo).

---

## Troubleshooting

### Build Fails: Submodules Empty

```
CMake Error: Cannot find whisper.cpp
```

Initialize submodules before building:
```bash
git submodule update --init --recursive
```

### Whisper ASR Not Working

DAWN's ASR fails gracefully when no Whisper model is present. To download:
```bash
docker run --rm -it -p 3000:3000 -e SKIP_MODEL_DOWNLOAD=false dawn:dev
```

Or check if the model is present:
```bash
docker exec <container> ls /opt/dawn/whisper.cpp/models/
```

### WebUI Login Required

On first run, DAWN prints a setup token to console. Use it to create an admin account:
```bash
docker exec -it <container> /opt/dawn/build/dawn-admin user create admin --admin
```

### No Audio in Container

Use `--device /dev/snd` to pass through audio hardware. Check host devices:
```bash
ls /dev/snd/
arecord -L  # list microphone devices on host
```

### MQTT Connection Refused

DAWN starts Mosquitto locally by default. For external brokers, configure in `dawn.toml`:
```toml
[mqtt]
host = "your-broker-host"
port = 1883
```

### Build Cache Invalidated

Each `git clone` during build (espeak-ng, piper-phonemize) re-clones on every build if that layer is invalidated. Pre-install steps are ordered to maximize cache reuse — only the COPY + build layers are invalidated on source changes.

---

## Security Considerations

- Never bake `secrets.toml` into the image — mount it at runtime or use environment variables
- SSL certificates in `ssl/` are excluded by `.dockerignore` — generate them per-deployment with `./generate_ssl_cert.sh`
- WebUI requires authentication (admin account setup at first run)
- The container runs as root by default — consider adding a non-root user for production

---

*Part of the [O.A.S.I.S. Project](https://github.com/The-OASIS-Project). For ecosystem orchestration, see [S.C.O.P.E.](https://github.com/malcolmhoward/the-oasis-project-meta-repo).*
