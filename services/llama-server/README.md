# Llama.cpp Server as a Systemd Service

This package provides a clean, production-ready setup for running llama-server as a systemd service with **optimal DAWN settings**.

## Model Options

| Model | Quality | Speed | Use Case |
|-------|---------|-------|----------|
| **Qwen3-4B-Instruct** | 84.8% | 15.2 tok/s | Standard assistant (recommended) |
| **Qwen3-4B-Thinking** | TBD | TBD | Shows reasoning process |
| Qwen3-8B | Higher | ~4 tok/s | Quality over speed |

---

## Quick Start

### Standard Mode (Instruct)

```bash
# Download model
huggingface-cli download unsloth/Qwen3-4B-Instruct-2507-GGUF \
  Qwen3-4B-Instruct-2507-Q4_K_M.gguf \
  --local-dir /var/lib/llama-cpp/models/

# Install service
sudo ./install.sh
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
| `CONTEXT_SIZE` | 8192 | Context window size |
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

| Metric | Cloud (GPT-4o) | Local (Qwen3-4B) |
|--------|----------------|------------------|
| Quality | 100% | 84.8% |
| Speed | ~50 tok/s | ~15 tok/s |
| Offline | No | Yes |
| Privacy | Data sent to API | Fully local |
| Cost | ~$0.01/query | Free |

See `../../docs/LLM_INTEGRATION_GUIDE.md` for complete integration details.
