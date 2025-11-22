# Llama.cpp Server as a Systemd Service

This package provides a clean, production-ready setup for running llama-server as a systemd service with **optimal DAWN settings**.

## Optimal Configuration (82.9% Quality, B Grade)

After testing **31+ configurations**, these settings achieve best performance for DAWN:

**Critical Parameters** (affect quality):
- **Model:** `Qwen3-4B-Instruct-2507-Q4_K_M.gguf` (NOT Q6!)
- **Batch Size:** 768 (82.9% quality vs 18% at 256)
- **Context:** 1024 (drops to 56% quality at 512)
- **GPU Layers:** 99 (offload everything)

**Standard Parameters** (no effect on quality - tested):
- Temperature: 0.7 (tested 0.6-0.75, no difference)
- Top-K: 40 (tested 30-45, no difference)
- Top-P: 0.9 (tested 0.85-0.92, no difference)
- Repeat Penalty: 1.1 (tested 1.0-1.15, no difference)

**Key Finding:** For Qwen3-4B Q4, only batch size and context size matter for quality.

---

## Prerequisites

- llama.cpp must be compiled and installed on the system
- The server must be run on a Linux system with systemd
- CUDA drivers must be installed if GPU acceleration is desired
- Root privileges are required for installation
- **Model file:** Download Qwen3-4B-Instruct-2507-Q4_K_M.gguf from HuggingFace

## Quick Installation

1. Download the optimal model:
   ```bash
   cd /var/lib/llama-cpp/models
   wget https://huggingface.co/Qwen/Qwen3-4B-Instruct-GGUF/resolve/main/Qwen3-4B-Instruct-2507-Q4_K_M.gguf
   ```

2. Make the installation script executable:
   ```bash
   chmod +x install.sh
   ```

3. Run the installation script:
   ```bash
   sudo ./install.sh
   ```

The script will install with optimal DAWN settings from `llama-server.conf`.

## Manual Installation

If you prefer to set up the service manually:

1. Create the service user:
   ```bash
   sudo useradd --system --no-create-home --shell /usr/sbin/nologin llama
   sudo usermod -a -G video,render llama
   ```

2. Create the directory structure:
   ```bash
   sudo mkdir -p /var/lib/llama-cpp/models
   sudo mkdir -p /var/lib/llama-cpp/templates
   sudo mkdir -p /var/lib/llama-cpp/run
   sudo mkdir -p /usr/local/etc/llama-cpp
   sudo mkdir -p /var/log/llama-cpp
   ```

3. Copy your model and template files:
   ```bash
   sudo cp /path/to/Qwen3-4B-Instruct-2507-Q4_K_M.gguf /var/lib/llama-cpp/models/
   sudo cp /path/to/qwen3_nonthinking.jinja /var/lib/llama-cpp/templates/
   ```

4. Install and configure the config file:
   ```bash
   sudo cp llama-server.conf /usr/local/etc/llama-cpp/
   # Config already has optimal DAWN settings
   ```

5. Set proper permissions:
   ```bash
   sudo chown -R llama:llama /var/lib/llama-cpp
   sudo chown -R llama:llama /var/log/llama-cpp
   sudo chmod -R 755 /var/lib/llama-cpp
   sudo chmod -R 755 /var/log/llama-cpp
   ```

6. Add the library path configuration:
   ```bash
   sudo sh -c 'echo "/usr/local/lib" > /etc/ld.so.conf.d/llama-cpp.conf'
   sudo ldconfig
   ```

7. Install the systemd service file:
   ```bash
   sudo cp llama-server.service /etc/systemd/system/
   ```

8. Install log rotation:
   ```bash
   sudo cp llama-server /etc/logrotate.d/
   ```

9. Enable and start the service:
   ```bash
   sudo systemctl daemon-reload
   sudo systemctl enable llama-server.service
   sudo systemctl start llama-server.service
   ```

## Usage

Once installed, the llama-server will:

- Start automatically on system boot
- Listen on 127.0.0.1:8080 (configurable)
- Use optimal DAWN settings (82.9% quality)
- Restart automatically if it crashes
- Use GPU acceleration (full offload with 768 batch size)
- Log to files in `/var/log/llama-cpp/` with automatic log rotation

### Service Management

```bash
# Check service status
sudo systemctl status llama-server.service

# View logs
sudo tail -f /var/log/llama-cpp/server.log
sudo tail -f /var/log/llama-cpp/error.log

# Or use journalctl
sudo journalctl -u llama-server.service -f

# Restart the service
sudo systemctl restart llama-server.service

# Stop the service
sudo systemctl stop llama-server.service
```

### Configuration

The server configuration is stored in `/usr/local/etc/llama-cpp/llama-server.conf`.

**Important:** The config file is already optimized for DAWN. Only change:
- Model path (if different location)
- Template path (if using custom template)
- Port/Host (if different binding needed)

**Do NOT change** batch size or context size without testing - they are critical for quality.

After changing the configuration, restart the service:

```bash
sudo systemctl restart llama-server.service
```

## Performance Testing

To verify the service is running with optimal settings:

```bash
# Check server is responding
curl http://127.0.0.1:8080/health

# Run quality test
cd ../../llm_testing/scripts
python3 test_llm_quality.py
```

**Expected results:**
- Quality: 82-83% (87/105 points, B grade)
- Speed: ~15 tok/s
- All JSON commands properly formatted

## Troubleshooting

### Out of Memory Errors

```
cudaMalloc failed: out of memory
```

**Cause:** GPU memory fragmentation (Jetson specific)

**Solution:**
1. Reboot the system: `sudo reboot`
2. Or reduce batch size in config (quality will drop):
   ```bash
   sudo nano /usr/local/etc/llama-cpp/llama-server.conf
   # Change BATCH_SIZE=768 to BATCH_SIZE=512 (drops to ~28% quality)
   sudo systemctl restart llama-server.service
   ```

### Low Quality (<70%)

**Check batch size:**
```bash
grep BATCH_SIZE /usr/local/etc/llama-cpp/llama-server.conf
# Should be 768
```

**Check context size:**
```bash
grep CONTEXT_SIZE /usr/local/etc/llama-cpp/llama-server.conf
# Should be 1024
```

**Check model:**
```bash
grep MODEL /usr/local/etc/llama-cpp/llama-server.conf
# Should be Qwen3-4B-Instruct-2507-Q4_K_M.gguf (Q4, not Q6!)
```

### GPU Access Issues

If the server isn't using the GPU:

1. Check if the llama user has the right groups:
   ```bash
   sudo usermod -a -G video,render llama
   ```

2. Verify GPU accessibility:
   ```bash
   sudo -u llama bash -c 'ls -la /dev/nvidia*'
   ```

3. Check server logs for CUDA errors:
   ```bash
   sudo journalctl -u llama-server.service | grep -i cuda
   ```

### Library Path Issues

If you see library-related errors:

```bash
# Update the dynamic linker cache
sudo ldconfig

# Check if libraries are found
sudo -u llama bash -c 'LD_LIBRARY_PATH=/usr/local/lib ldd /usr/local/bin/llama-server'
```

### Server Won't Start

1. Check if port is already in use:
   ```bash
   netstat -tulpn | grep 8080
   ```

2. Check model file exists:
   ```bash
   ls -lh /var/lib/llama-cpp/models/*.gguf
   ```

3. Check detailed logs:
   ```bash
   sudo journalctl -u llama-server.service -n 100
   ```

---

## Performance Comparison

| Metric | Cloud (GPT-4o) | Local (Qwen3-4B) |
|--------|----------------|------------------|
| Quality | 100% | 82.9% |
| Speed | 1.2s LLM | 3.4s LLM |
| Total Latency | 3.1s | 5.2s |
| Offline? | No | Yes |
| Privacy | Data sent to API | Fully local |
| Cost | ~$0.01-0.02 each | Free |

See `../../LLM_INTEGRATION_GUIDE.md` for complete integration details.
