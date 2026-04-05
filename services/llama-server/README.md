# Llama.cpp Server as a Systemd Service

This package provides a clean, production-ready setup for running llama-server as a systemd service with **optimal DAWN settings**.

## Model Options

All speeds measured on **Jetson AGX Orin 64GB at MAXN (60W)** power mode.
Run `sudo nvpmodel -m 0 && sudo jetson_clocks` before benchmarking.
At 30W mode, speeds are ~3x slower.

### MoE Vision Models (AGX Orin 64GB) — Voice-viable with vision

| Model | Active/Total | Size | Quality | Speed | Vision | Notes |
|-------|-------------|------|---------|-------|--------|-------|
| **Qwen3.5 35B-A3B** | 3B/35B MoE | 19.9 GB | 93.3% (A) | 29.6 tok/s | Yes | **Recommended** for production |
| Gemma 4 26B-A4B | 4B/25B MoE | 15.9 GB | 95.5% (A) | 32.2 tok/s | Yes | Faster + higher quality, but thinking leaks (see below) |

### Dense Models

| Model | Params | Size | Quality | Speed | Vision | Use Case |
|-------|--------|------|---------|-------|--------|----------|
| **Qwen3-4B-Instruct** | 4.0B | 2.3 GB | 84.8% (B) | 35.5 tok/s | No | Voice (no vision needed) |
| **Qwen3-4B-Thinking** | 4.0B | 2.3 GB | TBD | TBD | No | Shows reasoning process |
| Qwen3-8B | 8.2B | ~4.5 GB | TBD | TBD | No | Quality over speed |
| Qwen3.5-27B Vision | 26.9B | 15.9 GB | TBD | 7.2 tok/s | Yes | WebUI only (too slow for voice) |
| Gemma 4 31B Vision | 30.7B | 18.2 GB | TBD | 6.8 tok/s | Yes | WebUI only (too slow for voice) |

### Gemma 4 Thinking Leak Issue

As of April 2025, all Gemma 4 models (dense and MoE) leak thinking content into
responses via llama.cpp. The `<|channel>thought` content appears in
`reasoning_content` with `--reasoning-format deepseek`, but the model spends its
entire token budget on thinking and produces empty `content`. Neither
`--reasoning off` nor `--chat-template-kwargs '{"enable_thinking":false}'`
reliably suppresses this. Monitor `ggml-org/llama.cpp` issues for fixes. The
Qwen 3.5 models do not have this issue.

**Current recommendation:** Use **Qwen 3.5 35B-A3B** (Preset F) for production.
The Gemma 4 26B-A4B scores higher on quality (95.5% vs 93.3%) and is faster
(32.2 vs 29.6 tok/s), so it becomes the top recommendation once the thinking
leak is resolved upstream.

### Hardware: Jetson AGX Orin 64GB Developer Kit

| Spec | Value |
|------|-------|
| GPU | Ampere (SM 8.7), 2048 CUDA cores, 1.3 GHz (MAXN) |
| Memory | 64 GB unified LPDDR5, ~204 GB/s bandwidth |
| CPU | 12-core Arm Cortex-A78AE, 2.2 GHz (MAXN) |
| Power modes | MAXN (60W), 30W, 15W |
| CUDA compute | 8.7 |

---

## Quick Start

The installer auto-detects your hardware and recommends a preset. Just run:

```bash
sudo ./install.sh
```

Or install a specific preset non-interactively:

```bash
# AGX Orin 64GB: Qwen 3.5 35B-A3B MoE (recommended — 93.3% quality, 29.6 tok/s, vision)
sudo ./install.sh -P F

# Small hardware: Qwen3 4B Instruct (84.8% quality, 35.5 tok/s, no vision)
sudo ./install.sh -P A
```

The installer will download model files automatically if they're missing (requires `hf` CLI).

### Manual Download (if needed)

```bash
# Preset F: Qwen 3.5 35B-A3B MoE
hf download bartowski/Qwen_Qwen3.5-35B-A3B-GGUF \
  Qwen_Qwen3.5-35B-A3B-Q4_K_M.gguf \
  --local-dir /var/lib/llama-cpp/models/

hf download bartowski/Qwen_Qwen3.5-35B-A3B-GGUF \
  mmproj-Qwen_Qwen3.5-35B-A3B-f16.gguf \
  --local-dir /var/lib/llama-cpp/models/

# Preset A: Qwen3 4B Instruct
hf download unsloth/Qwen3-4B-Instruct-2507-GGUF \
  Qwen3-4B-Instruct-2507-Q4_K_M.gguf \
  --local-dir /var/lib/llama-cpp/models/
```

### Qwen 3.5 27B Vision (AGX Orin 64GB)

```bash
# Download model and vision projector
hf download bartowski/Qwen_Qwen3.5-27B-GGUF \
  Qwen_Qwen3.5-27B-Q4_K_M.gguf \
  --local-dir /var/lib/llama-cpp/models/

hf download bartowski/Qwen_Qwen3.5-27B-GGUF \
  mmproj-Qwen_Qwen3.5-27B-f16.gguf \
  --local-dir /var/lib/llama-cpp/models/

# Install service
sudo ./install.sh -m /var/lib/llama-cpp/models/Qwen_Qwen3.5-27B-Q4_K_M.gguf \
  --mmproj /var/lib/llama-cpp/models/mmproj-Qwen_Qwen3.5-27B-f16.gguf
```

### Extended Thinking Mode

```bash
# Download thinking model
huggingface-cli download unsloth/Qwen3-4B-Thinking-2507-GGUF \
  Qwen3-4B-Thinking-2507-Q4_K_M.gguf \
  --local-dir /var/lib/llama-cpp/models/

# Copy thinking template
sudo cp qwen3_thinking.jinja /var/lib/llama-cpp/templates/

# Edit config before install (or after, see Switching Modes)
nano llama-server.conf
# Uncomment Thinking MODEL line, comment Instruct MODEL line
# Set REASONING_FORMAT=deepseek

# Install service
sudo ./install.sh
```

---

## Switching Between Instruct and Thinking Modes

Edit `/usr/local/etc/llama-cpp/llama-server.conf`:

### For Instruct Mode (default):
```bash
MODEL="/var/lib/llama-cpp/models/Qwen3-4B-Instruct-2507-Q4_K_M.gguf"
TEMPLATE="/var/lib/llama-cpp/templates/qwen3_chatml.jinja"
REASONING_FORMAT=none
```

### For Thinking Mode:
```bash
MODEL="/var/lib/llama-cpp/models/Qwen3-4B-Thinking-2507-Q4_K_M.gguf"
TEMPLATE="/var/lib/llama-cpp/templates/qwen3_thinking.jinja"
REASONING_FORMAT=deepseek
MMPROJ=
```

### For Qwen 3.5 35B-A3B Vision MoE (AGX Orin 64GB, recommended):
```bash
MODEL="/var/lib/llama-cpp/models/Qwen_Qwen3.5-35B-A3B-Q4_K_M.gguf"
TEMPLATE=
REASONING_FORMAT=deepseek
MMPROJ="/var/lib/llama-cpp/models/mmproj-Qwen_Qwen3.5-35B-A3B-f16.gguf"
CONTEXT_SIZE=32768
```

### For Gemma 4 26B-A4B Vision MoE (AGX Orin 64GB):
```bash
MODEL="/var/lib/llama-cpp/models/google_gemma-4-26B-A4B-it-Q4_K_M.gguf"
TEMPLATE=
REASONING_FORMAT=deepseek
MMPROJ="/var/lib/llama-cpp/models/mmproj-google_gemma-4-26B-A4B-it-f16.gguf"
CONTEXT_SIZE=32768
TEMPERATURE=1.0
TOP_P=0.95
TOP_K=64
REPEAT_PENALTY=1.0
```

### For Qwen 3.5 27B Vision dense (AGX Orin 64GB):
```bash
MODEL="/var/lib/llama-cpp/models/Qwen_Qwen3.5-27B-Q4_K_M.gguf"
TEMPLATE=
REASONING_FORMAT=deepseek
MMPROJ="/var/lib/llama-cpp/models/mmproj-Qwen_Qwen3.5-27B-f16.gguf"
CONTEXT_SIZE=32768
```

### For Gemma 4 31B Vision (AGX Orin 64GB):
```bash
MODEL="/var/lib/llama-cpp/models/google_gemma-4-31B-it-Q4_K_M.gguf"
TEMPLATE=
REASONING_FORMAT=deepseek
MMPROJ="/var/lib/llama-cpp/models/mmproj-google_gemma-4-31B-it-f16.gguf"
CONTEXT_SIZE=32768
TEMPERATURE=1.0
TOP_P=0.95
TOP_K=64
REPEAT_PENALTY=1.0
```

Then restart:
```bash
sudo systemctl restart llama-server
```

### DAWN Configuration

When using Thinking mode, also update `dawn.toml`:
```toml
[llm.thinking]
mode = "enabled"
budget_tokens = 5000
```

---

## Configuration Reference

### llama-server.conf Options

| Variable | Default | Description |
|----------|---------|-------------|
| `MODEL` | Qwen3-4B-Instruct | Path to GGUF model file |
| `TEMPLATE` | qwen3_chatml.jinja | Chat template file |
| `REASONING_FORMAT` | none | `none` for Instruct, `deepseek` for Thinking models |
| `MMPROJ` | *(empty)* | Path to multimodal projector GGUF (vision models only) |
| `CONTEXT_SIZE` | 8192 | Context window size (32768 for vision models) |
| `BATCH_SIZE` | 768 | **Critical for quality** - do not reduce |
| `UNBATCH_SIZE` | 768 | Should match BATCH_SIZE |
| `GPU_LAYERS` | 99 | Layers to offload to GPU |
| `PORT` | 8080 | Server listen port |
| `HOST` | 127.0.0.1 | Server bind address |

### Critical Parameters (affect quality)

After testing **31+ configurations**, these settings achieve best performance:

- **Batch Size:** 768 (84.8% quality vs 18% at 256)
- **Context:** 8192 (headroom for conversations)
- **GPU Layers:** 99 (full offload)

**Do NOT reduce batch size** - quality drops dramatically.

---

## Prerequisites

- llama.cpp compiled and installed (`/usr/local/bin/llama-server`)
- Linux system with systemd
- CUDA drivers (for GPU acceleration)
- Root privileges for installation

---

## Installation

### Automatic Installation

```bash
chmod +x install.sh
sudo ./install.sh
```

The script will:
1. Create `llama` user and group
2. Set up directory structure
3. Install config, service, and logrotate files
4. Enable and start the service

### Manual Installation

1. Create the service user:
   ```bash
   sudo useradd --system --no-create-home --shell /usr/sbin/nologin llama
   sudo usermod -a -G video,render llama
   ```

2. Create directory structure:
   ```bash
   sudo mkdir -p /var/lib/llama-cpp/{models,templates,run}
   sudo mkdir -p /usr/local/etc/llama-cpp
   sudo mkdir -p /var/log/llama-cpp
   ```

3. Copy model and template files:
   ```bash
   sudo cp /path/to/model.gguf /var/lib/llama-cpp/models/
   sudo cp qwen3_chatml.jinja /var/lib/llama-cpp/templates/
   sudo cp qwen3_thinking.jinja /var/lib/llama-cpp/templates/  # Optional
   ```

4. Install config and service:
   ```bash
   sudo cp llama-server.conf /usr/local/etc/llama-cpp/
   sudo cp llama-server.service /etc/systemd/system/
   sudo cp llama-server /etc/logrotate.d/
   ```

5. Set permissions:
   ```bash
   sudo chown -R llama:llama /var/lib/llama-cpp /var/log/llama-cpp
   sudo chmod -R 755 /var/lib/llama-cpp /var/log/llama-cpp
   ```

6. Configure library path:
   ```bash
   sudo sh -c 'echo "/usr/local/lib" > /etc/ld.so.conf.d/llama-cpp.conf'
   sudo ldconfig
   ```

7. Enable and start:
   ```bash
   sudo systemctl daemon-reload
   sudo systemctl enable llama-server
   sudo systemctl start llama-server
   ```

---

## Service Management

```bash
# Status
sudo systemctl status llama-server

# Logs
sudo tail -f /var/log/llama-cpp/server.log
sudo tail -f /var/log/llama-cpp/error.log
sudo journalctl -u llama-server -f

# Restart (after config changes)
sudo systemctl restart llama-server

# Stop
sudo systemctl stop llama-server
```

---

## Testing

### Health Check
```bash
curl http://127.0.0.1:8080/health
```

### Quality Test
```bash
cd ../../llm_testing/scripts
python3 test_llm_quality.py
```

**Expected (Instruct):** 84-85% quality, ~15 tok/s

---

## Troubleshooting

### Out of Memory

```
cudaMalloc failed: out of memory
```

**Solution:** Reboot to clear GPU memory fragmentation (Jetson-specific):
```bash
sudo reboot
```

### Low Quality (<70%)

Verify critical settings:
```bash
grep -E "BATCH_SIZE|CONTEXT_SIZE|MODEL" /usr/local/etc/llama-cpp/llama-server.conf
```

Should show:
- `BATCH_SIZE=768`
- `CONTEXT_SIZE=8192`
- Model ending in `Q4_K_M.gguf`

### Thinking Not Working

1. Check reasoning format:
   ```bash
   grep REASONING_FORMAT /usr/local/etc/llama-cpp/llama-server.conf
   # Should be: REASONING_FORMAT=deepseek
   ```

2. Check model is Thinking variant:
   ```bash
   grep MODEL /usr/local/etc/llama-cpp/llama-server.conf
   # Should contain "Thinking"
   ```

3. Check DAWN config (`dawn.toml`):
   ```toml
   [llm.thinking]
   mode = "enabled"
   ```

### GPU Access Issues

```bash
# Add llama user to GPU groups
sudo usermod -a -G video,render llama

# Verify GPU access
sudo -u llama ls -la /dev/nvidia*

# Check for CUDA errors
sudo journalctl -u llama-server | grep -i cuda
```

### Server Won't Start

```bash
# Port in use?
netstat -tulpn | grep 8080

# Model exists?
ls -lh /var/lib/llama-cpp/models/*.gguf

# Detailed logs
sudo journalctl -u llama-server -n 100
```

---

## Files

| File | Installed Location | Purpose |
|------|-------------------|---------|
| `llama-server.conf` | `/usr/local/etc/llama-cpp/` | Server configuration |
| `llama-server.service` | `/etc/systemd/system/` | Systemd service unit |
| `llama-server` | `/etc/logrotate.d/` | Log rotation config |
| `qwen3_chatml.jinja` | `/var/lib/llama-cpp/templates/` | Instruct chat template |
| `qwen3_thinking.jinja` | `/var/lib/llama-cpp/templates/` | Thinking chat template |

---

## Performance Comparison

### AGX Orin 64GB at MAXN (60W)

| Metric | Cloud (GPT-4o) | Qwen3-4B | Qwen3.5-35B-A3B | Gemma4-26B-A4B |
|--------|----------------|----------|-----------------|----------------|
| Quality | 100% | 84.8% (B) | 93.3% (A) | 95.5% (A) |
| Speed | ~50 tok/s | 35.5 tok/s | 29.6 tok/s | 32.2 tok/s |
| Prompt eval | N/A | 181 tok/s | 78 tok/s | 110 tok/s |
| Vision | Yes | No | Yes | Yes |
| Thinking | Yes | No | Clean disable | Leaks (blocking) |
| Offline | No | Yes | Yes | Yes |
| Privacy | Data sent to API | Fully local | Fully local | Fully local |
| Cost | ~$0.01/query | Free | Free | Free |

### Power Mode Impact (Qwen3-4B baseline)

| Power Mode | GPU Clock | EMC Clock | tok/s | Relative |
|------------|-----------|-----------|-------|----------|
| MAXN (60W) | 1.3 GHz | 3.2 GHz | 35.5 | 1.0x |
| 30W | 612 MHz | 2.1 GHz | 10.4 | 0.29x |

**Always use MAXN for inference workloads:**
```bash
sudo nvpmodel -m 0 && sudo jetson_clocks
```

See `../../docs/LLM_INTEGRATION_GUIDE.md` for complete integration details.
