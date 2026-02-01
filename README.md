# DAWN

**Digital Assistant with Natural-language Workflow**

> AI voice assistant for the [O.A.S.I.S. Project](https://github.com/The-OASIS-Project)

## Overview

DAWN is the AI voice assistant component of the O.A.S.I.S. ecosystem. Built in C/C++ for NVIDIA Jetson platforms, it provides speech recognition, natural language understanding, and text-to-speech capabilities for hands-free interaction.

## Features

- Speech recognition via Vosk
- Text-to-speech via Piper
- LLM integration (OpenAI API compatible)
- MQTT communication with O.A.S.I.S. components
- Music playback with FLAC support
- Command parsing and execution

## Credits

Initial adaptation from the piper project: https://github.com/rhasspy/piper

- Piper and language models: MIT License
- Vosk: Apache License 2.0

## Prerequisites

- NVIDIA Jetson platform (Nano, Orin Nano, or NX)
- JetPack SDK
- CMake 3.27+

## Installation

### I. CMake 3.27.1

```bash
tar xvf cmake-3.27.1.tar.gz
cd cmake-3.27.1
./configure --system-curl
make -j8
sudo make install
```

### II. spdlog

```bash
git clone https://github.com/gabime/spdlog.git
cd spdlog
mkdir build && cd build
cmake .. && make -j8
sudo make install
```

### III. espeak-ng (from source)

Before beginning:
```bash
sudo apt purge espeak-ng-data libespeak-ng1 speech-dispatcher-espeak-ng
```

```bash
git clone https://github.com/rhasspy/espeak-ng.git
cd espeak-ng
./autogen.sh
./configure --prefix=/usr
make -j8 src/espeak-ng src/speak-ng
make
sudo make LIBDIR=/usr/lib/aarch64-linux-gnu install
```

### IV. ONNX Runtime

```bash
git clone --recursive https://github.com/microsoft/onnxruntime
cd onnxruntime
./build.sh --use_cuda --cudnn_home /usr/local/cuda-11.4 --cuda_home /usr/local/cuda-11.4 --config MinSizeRel --update --build --parallel --build_shared_lib
sudo cp -a build/Linux/MinSizeRel/libonnxruntime.so* /usr/local/lib/
sudo mkdir -p /usr/local/include/onnxruntime
sudo cp include/onnxruntime/core/session/*.h /usr/local/include/onnxruntime
```

### V. piper-phonemize

```bash
git clone https://github.com/rhasspy/piper-phonemize.git
cd piper-phonemize
cd src && cp ../../onnxruntime/include/onnxruntime/core/session/*.h .
cd ..
mkdir build && cd build
cmake .. && make
sudo make install
```

### VI. Piper

```bash
git clone https://github.com/rhasspy/piper.git
cd piper
# Edit src/cpp/CMakeLists.txt - add /usr/local/include/onnxruntime
# and /usr/local/include/piper-phonemize to target_include_directories
make
```

### VII. Kaldi (Long build process)

```bash
sudo apt-get install sox subversion
sudo git clone -b vosk --single-branch --depth=1 https://github.com/alphacep/kaldi /opt/kaldi
sudo chown -R $USER /opt/kaldi
cd /opt/kaldi/tools
# Edit Makefile - remove -msse -msse2 from openfst_add_CXXFLAGS
make openfst cub
./extras/install_openblas_clapack.sh
cd ../src
./configure --mathlib=OPENBLAS_CLAPACK --shared
make -j 10 online2 lm rnnlm
```

### VIII. Vosk API

```bash
sudo git clone https://github.com/alphacep/vosk-api --depth=1
sudo chown -R $USER vosk-api
cd vosk-api/src
KALDI_ROOT=/opt/kaldi make -j8
cd ../c
# Edit Makefile - add to LDFLAGS: $(shell pkg-config --libs cuda-11.4 cudart-11.4) -lcusparse -lcublas -lcusolver -lcurand
make
wget https://alphacephei.com/vosk/models/vosk-model-en-us-0.22.zip
unzip vosk-model-en-us-0.22.zip
ln -s vosk-model-en-us-0.22 model
```

### IX. Build DAWN

```bash
mkdir build && cd build
cmake ..
make -j$(nproc)
```

## Key Files

| File | Purpose |
|------|---------|
| `dawn.c` | Main application entry point |
| `mosquitto_comms.c` | MQTT communication |
| `openai.c` | LLM API integration |
| `text_to_speech.cpp` | Piper TTS integration |
| `text_to_command_nuevo.c` | Command parsing |
| `flac_playback.c` | Audio playback |

## Related Projects

- [S.C.O.P.E.](https://github.com/malcolmhoward/the-oasis-project-meta-repo) - O.A.S.I.S. coordination repository
- [MIRAGE](https://github.com/The-OASIS-Project/mirage) - Heads-up display
- [AURA](https://github.com/The-OASIS-Project/aura) - Helmet sensor firmware
- [SPARK](https://github.com/The-OASIS-Project/spark) - Hand/gauntlet firmware

## Contributing

Please see [CONTRIBUTING.md](CONTRIBUTING.md) for guidelines.

## License

This program is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or any later version.

See [LICENSE](LICENSE) for full details.

---

*Part of the [O.A.S.I.S. Project](https://github.com/The-OASIS-Project) - Open-source Assistive System for Integrated Services*
