# DAWN Dependencies

This document tracks all third-party dependencies used by the DAWN project.

## WebUI (JavaScript)

| Library | Version | License | Source | Description |
|---------|---------|---------|--------|-------------|
| marked.js | 15.0.12 | MIT | [GitHub](https://github.com/markedjs/marked) | Markdown parser |
| DOMPurify | 3.3.1 | Apache 2.0 / MPL 2.0 | [GitHub](https://github.com/cure53/DOMPurify) | XSS sanitizer |

**Local copies**: `www/js/marked.min.js`, `www/js/purify.min.js`

## WebUI (Fonts)

| Font | License | Source | Files |
|------|---------|--------|-------|
| IBM Plex Mono | SIL OFL 1.1 | [GitHub](https://github.com/IBM/plex) | `IBMPlexMono-Regular.woff2`, `IBMPlexMono-Medium.woff2` |
| Source Sans 3 | SIL OFL 1.1 | [GitHub](https://github.com/adobe-fonts/source-sans) | `SourceSans3-Regular.woff2`, `SourceSans3-Medium.woff2` |

**Local copies**: `www/fonts/`

## C/C++ Libraries

### Core Dependencies (Required)

| Library | License | Purpose |
|---------|---------|---------|
| json-c | MIT | JSON parsing |
| libcurl | MIT/X | HTTP client for API calls |
| OpenSSL | Apache 2.0 | Cryptography, TLS |
| libmosquitto | EPL/EDL | MQTT client |
| libwebsockets | MIT | WebSocket server for WebUI |
| pthread | LGPL | Threading |
| MuPDF | AGPL-3.0 (dual-licensed AGPL/commercial) | PDF text extraction for document upload |
| libzip | BSD-3-Clause | DOCX ZIP archive reading |
| libmujs-dev, libgumbo-dev, libopenjp2-7-dev, libjbig2dec0-dev | Various (MIT/LGPL/BSD) | MuPDF static link dependencies |

### Audio Processing

| Library | License | Purpose |
|---------|---------|---------|
| PulseAudio | LGPL 2.1+ | Audio capture/playback |
| FLAC | BSD-3-Clause | FLAC audio decoding |
| Opus | BSD-3-Clause | Audio codec for WebUI streaming |
| libsamplerate | BSD-2-Clause | Sample rate conversion |
| WebRTC Audio Processing | BSD-3-Clause | AEC3 echo cancellation (optional) |

### Speech Recognition (ASR)

| Library | Version | License | Purpose |
|---------|---------|---------|---------|
| Vosk | - | Apache 2.0 | Offline speech recognition |
| whisper.cpp | - | MIT | Alternative ASR engine |
| Kaldi | Apache 2.0 | Vosk dependency |

### Text-to-Speech (TTS)

| Library | License | Purpose |
|---------|---------|---------|
| Piper | MIT | Neural TTS engine |
| ONNX Runtime | MIT | ML inference for Piper |
| piper-phonemize | MIT | Text-to-phoneme conversion |
| espeak-ng | GPL 3.0 | Phonemizer backend |

### CUDA (Jetson Only)

| Library | License | Purpose |
|---------|---------|---------|
| cuBLAS | NVIDIA EULA | Matrix operations |
| cuSPARSE | NVIDIA EULA | Sparse matrix operations |
| cuSOLVER | NVIDIA EULA | Linear algebra |
| cuRAND | NVIDIA EULA | Random number generation |

## Build Tools

| Tool | Version | Purpose |
|------|---------|---------|
| CMake | 3.27.1+ | Build system |
| GCC/G++ | 11+ | C/C++ compiler |
| pkg-config | - | Library discovery |

## Model Files (Not Libraries)

These are model files required at runtime, not source dependencies:

- `vosk-model-en-us-0.22/` - Vosk English model
- `models/*.onnx` - Piper TTS voice models
- `models/whisper.cpp/` - Whisper ASR models (optional)

## License Compatibility

All dependencies are compatible with GPLv3:
- MIT, BSD, Apache 2.0 - Permissive, GPL-compatible
- LGPL - Compatible when dynamically linked
- EPL (Mosquitto) - Compatible via explicit dual-license (EDL)
- espeak-ng GPL 3.0 - Same license as project
- AGPL-3.0 (MuPDF) — Compatible, project is GPLv3

## Updating Dependencies

### WebUI Libraries
```bash
# marked.js
curl -sL "https://cdn.jsdelivr.net/npm/marked@latest/marked.min.js" -o www/js/marked.min.js

# DOMPurify
curl -sL "https://cdn.jsdelivr.net/npm/dompurify@latest/dist/purify.min.js" -o www/js/purify.min.js
```

### System Libraries
System libraries should be updated through the package manager (apt on Ubuntu/Debian).

See `README.md` for full installation instructions.
