# D.A.W.N. - Digital Assistant for Wearable Neutronics (AI Assistant)

**These instructions are currently in the "do this" phase. This "works for me" and I welcome your feedback.**

<img src="https://www.oasisproject.net/assets/dawn_ai_concept.png" alt="DAWN AI Assistant" width="350" align="right">

## Overview

D.A.W.N. (Digital Assistant for Wearable Neutronics) is the AI assistant component of the O.A.S.I.S. ecosystem. It provides voice-controlled interaction for the wearable system, handling speech recognition, natural language processing, and text-to-speech output.

DAWN listens for a wake word, records voice commands, processes them through either a local LLM or cloud AI (OpenAI), and executes actions by publishing MQTT commands to other O.A.S.I.S. components. It also provides text-to-speech feedback using Piper TTS with CUDA acceleration.

Key capabilities:
- Voice command recognition via Vosk speech-to-text
- Local and cloud LLM support for natural language understanding
- Text-to-speech output via Piper TTS with ONNX Runtime (CUDA-accelerated)
- MQTT-based command dispatch to MIRAGE HUD, helmet systems, and audio devices
- Configurable device commands via JSON configuration

## Hardware

| Component | Description | Link |
|-----------|-------------|------|
| NVIDIA Jetson Orin Nano/NX | Primary compute platform with CUDA GPU | [NVIDIA Jetson](https://www.nvidia.com/en-us/autonomous-machines/embedded-systems/) |
| Creative Sound Blaster Play! 3 | USB audio capture device (microphone input) | [Creative Labs](https://us.creative.com/p/sound-blaster/sound-blaster-play-3) |
| Audio playback device | Headphones or speakers via PulseAudio | — |

## Application Notes

* OpenAI API - An OpenAI API key is required for the current implementation of the cloud AI using OpenAI. Getting an API key is currently beyond the scope of this document. Please see OpenAI's documentation for details.
* If you do not wish to use cloud AI or you only want local command support, there is a flag to disable it.

## Installation Notes (Required Software)

### System Packages
1. `sudo apt install libssl-dev`

### Cmake 3.27.1
1. `tar xvf cmake-3.27.1.tar.gz`
2. `cd cmake-3.27.1`
3. `./configure --system-curl`
4. `make -j8`
5. `sudo make install`

### spdlog
1. `git clone https://github.com/gabime/spdlog.git`
2. `cd spdlog`
3. `mkdir build && cd build`
4. `cmake .. && make -j8`
5. `sudo make install`

### espeak-ng (git)
Before we begin:
`sudo apt purge espeak-ng-data libespeak-ng1 speech-dispatcher-espeak-ng`

1. `git clone https://github.com/rhasspy/espeak-ng.git`
2. `cd espeak-ng`
3. `./autogen.sh`
4. `./configure --prefix=/usr`
5. `make -j8 src/espeak-ng src/speak-ng`
6. `make`
7. `sudo make LIBDIR=/usr/lib/aarch64-linux-gnu install`

### Onnxruntime (git)
1. `git clone --recursive https://github.com/microsoft/onnxruntime`
2. `cd onnxruntime`
3. `./build.sh --config Release --use_cuda --cuda_home /usr/local/cuda-12.2 --cudnn_home /usr/lib/aarch64-linux-gnu --build_shared_lib --skip_tests --parallel $(nproc) --arm`
4. `cd build/Linux/Release/`
5. `sudo make install`

### piper-phonemize (git)
1. `git clone https://github.com/rhasspy/piper-phonemize.git`
2. `cd piper-phonemize`
    1. `cd src && cp ../../onnxruntime/include/onnxruntime/core/session/*.h .`
    2. `cd ..`
3. `mkdir build && cd build`
4. `cmake ..`
5. `make`
6. `sudo make install`

### piper (git)
1. `git clone https://github.com/rhasspy/piper.git`
2. `cd piper`
3. `make` - You'll get some errors on copies at the end but it builds.

### kaldi (git) (This is a REALLY long build process!)
1. `sudo apt-get install sox subversion`
2. `sudo git clone -b vosk --single-branch --depth=1 https://github.com/alphacep/kaldi /opt/kaldi`
3. `sudo chown -R $USER /opt/kaldi`
4. `cd /opt/kaldi/tools`
5. Edit Makefile. Remove `-msse -msse2` from `openfst_add_CXXFLAGS`
6. `make openfst cub` (Note: -j# doesn't seem to work here.) *LONG BUILD*
7. `./extras/install_openblas_clapack.sh`
8. `cd ../src`
9. `./configure --mathlib=OPENBLAS_CLAPACK --shared`
10. `make -j 10 online2 lm rnnlm`
11. `cd ../..`
12. `sudo git clone https://github.com/alphacep/vosk-api --depth=1`
13. `sudo chown -R $USER vosk-api`
14. `cd vosk-api/src`
15. `KALDI_ROOT=/opt/kaldi make -j8`
16. `cd ../c`
17. Edit Makefile. Add the following to LDFLAGS: `$(shell pkg-config --libs cuda-12.2 cudart-12.2) -lcusparse -lcublas -lcusolver -lcurand`
    1. `make`
18. Choose a model:
    1. `wget https://alphacephei.com/vosk/models/vosk-model-small-en-us-0.15.zip`
    2. `wget https://alphacephei.com/vosk/models/vosk-model-en-us-0.22.zip`
19. `unzip vosk-model-en-us-0.22.zip`
20. `ln -s vosk-model-en-us-0.22 model`
21. `cp ../python/example/test.wav .`
22. `./test_vosk`

### Copy some files over for compiling
1. `cp -r vosk-model-en-us-0.22 SOURCE_DIR`
2. `cp ../src/vosk_api.h ../src/libvosk.so SOURCE_DIR`

## Build DAWN

1. `mkdir build`
2. `cd build`
3. `cmake ..`
4. `make`

## DAWN Application Configuration Documentation (`commands_config_nuevo.json`)

The DAWN application utilizes a sophisticated configuration file designed to enhance interactivity through local voice commands and actions. This documentation outlines the structure and purpose of each section within the file, focusing on how actions are defined and linked to specific devices, including audio settings customization.

### Types and Actions

```json
{
  "types": {
     "boolean": {
        "actions": {
           "enable": {
              "action_words": ["enable %device_name%", "turn on %device_name%", "switch on %device_name%", "show %device_name%", "display %device_name%", "open %device_name%", "start %device_name%"],
              "action_command": "{\"device\": \"%device_name%\", \"action\": \"enable\"}"
           },
           "disable": {
              "action_words": ["disable %device_name%", "turn off %device_name%", "switch off %device_name%", "hide %device_name%", "close %device_name%", "stop %device_name%"],
              "action_command": "{\"device\": \"%device_name%\", \"action\": \"disable\"}"
           }
        }
     }
  }
}
```

* **`types`:** Represent the different categories of settings that can be adjusted or monitored within the DAWN system. These include `boolean` for toggle settings, `analog` for value-based adjustments, `getter` for retrieving information, and `music` for controlling audio playback.
* **`actions`:** Defined within each type, actions describe what operations can be performed. Each action has associated `action_words`, which are the voice commands recognized by DAWN to trigger the action, and an `action_command`, the MQTT JSON string sent to the target device to execute the action.

### Devices

This section lists the various devices controlled by DAWN, detailing how voice commands translate into specific actions for each device:

* **`type`:** Links the device to one of the defined types (e.g., boolean, analog), dictating the nature of its control.
* **`aliases`:** Alternative names or phrases that can also refer to the device, enhancing the system's ability to recognize voice commands intended for it.
* **`topic`:** The MQTT topic the device publishes to, ensuring that commands are accurately directed in the network.

### Audio Devices

Specific to the configuration of audio input and output devices, this section allows DAWN to correctly setup and utilize audio hardware. This is independent of the rest of the configuration.

Each audio device is categorized by its function (e.g., `microphone`, `headphones`, `speakers`), with detailed configurations for effective operation.

* **`type`:** Identifies the role of the audio device within the system (e.g., audio capture device for microphones).
* **`aliases`:** Provides additional identification terms for each device, facilitating user interaction.
* **`device`:** The system identifier for the hardware, used by DAWN to apply the correct settings.

``` json
  "audio devices": {
     "microphone": {
        "type": "audio capture device",
        "aliases": ["mic", "helmet mic", "audio input device"],
        "device": "alsa_input.usb-Creative_Technology_Ltd_Sound_Blaster_Play__3_00128226-00.analog-stereo"
     },
     "headphones": {
        "type": "audio playback device",
        "aliases": ["helmet"],
        "device": "combined"
     },
     "speakers": {
        "type": "audio playback device",
        "aliases": ["speaker", "loud speakers", "loud speaker", "chest speaker"],
        "device": "combined"
     }
  }
```

Hints:

* `pactl list short sinks`
* `pactl list short sources`
* Set your audio devices: `commands_config_nuevo.json`

## Run DAWN
1. `./dawn`

## Communication

DAWN communicates with other O.A.S.I.S. components via MQTT. The broker runs locally on `127.0.0.1:1883` (configurable in `dawn.h`).

### Subscribed Topics

| Topic | Purpose |
|-------|---------|
| `dawn` | Receives device commands (audio control, TTS, music, LLM switching, shutdown) |

Incoming messages are JSON with the format: `{"device": "<type>", "action": "<action>", "value": "<optional>"}`.

Supported device types: `audio playback device`, `audio capture device`, `text to speech`, `date`, `time`, `music`, `voice amplifier`, `shutdown alpha bravo charlie`, `viewing`, `volume`, `local llm`, `cloud llm`.

### Published Topics

| Topic | Purpose | Payload Example |
|-------|---------|-----------------|
| `hud` | AI state updates for MIRAGE HUD display | `{"device": "ai", "name":"friday", "state":"WAKEWORD_LISTEN"}` |
| Dynamic (from config) | Device commands routed to other components | `{"device": "<name>", "action": "<action>"}` |

AI states published to `hud`: `SILENCE`, `WAKEWORD_LISTEN`, `COMMAND_RECORDING`, `PROCESS_COMMAND`, `VISION_AI_READY`.

Dynamic publish topics are defined per-device in `commands_config_nuevo.json` and include `hud` (for MIRAGE HUD elements), `helmet` (for mask control), and `dawn` (for self-directed commands).

## Troubleshooting

| Issue | Possible Cause | Solution |
|-------|---------------|----------|
| No audio capture | Wrong PulseAudio device | Run `pactl list short sources` and update `commands_config_nuevo.json` |
| No audio playback | Wrong PulseAudio sink | Run `pactl list short sinks` and update `commands_config_nuevo.json` |
| TTS not working | Piper model not found | Verify Piper model files are in the expected location |
| TTS slow / no CUDA | ONNX Runtime built without CUDA | Rebuild ONNX Runtime with `--use_cuda` flag |
| Vosk recognition poor | Wrong model size | Try the larger `vosk-model-en-us-0.22` model |
| MQTT connection failed | Mosquitto broker not running | Start the broker: `sudo systemctl start mosquitto` |
| MQTT commands not received | Wrong topic subscription | Verify the broker is on `127.0.0.1:1883` and topic is `dawn` |
| Build fails at CMake | Missing dependencies | Verify all prerequisites are installed (spdlog, espeak-ng, onnxruntime, piper, kaldi/vosk) |
| espeak-ng conflicts | System espeak-ng still installed | Run `sudo apt purge espeak-ng-data libespeak-ng1 speech-dispatcher-espeak-ng` first |
| kaldi build fails with SSE errors | ARM platform incompatibility | Remove `-msse -msse2` from Makefile `openfst_add_CXXFLAGS` |

## Related Components

- [M.I.R.A.G.E.](https://www.oasisproject.net/components/mirage/) - DAWN publishes AI state to the MIRAGE HUD and sends device commands for HUD elements (armor display, detection, map, visual offset)
- [A.U.R.A.](https://www.oasisproject.net/components/aura/) - DAWN sends helmet control commands (mask enable/disable) via the `helmet` MQTT topic
- [S.P.A.R.K.](https://www.oasisproject.net/components/spark/) - DAWN can send commands to hand-mounted components
- [B.E.A.C.O.N.](https://www.oasisproject.net/components/beacon/) - Physical enclosure that houses DAWN's audio hardware and compute platform

## Credits

Initial adaptation from the piper project: [https://github.com/rhasspy/piper](https://github.com/rhasspy/piper)

Piper and the language models are covered under the MIT license.
Vosk Licensed under the Apache License, Version 2.0.
