# D.A.W.N. Full Simulation Demo

Run D.A.W.N.'s complete intent-processing pipeline without any external hardware, GPU, or API keys. All services are replaced by [E.C.H.O.](https://github.com/malcolmhoward/the-oasis-project-simulation-repo) simulation framework mocks.

## What This Demonstrates

The full "voice command to device control" loop:

```
User types in WebUI → D.A.W.N. sends to LLM mock → LLM returns tool call
  → D.A.W.N. executes tool against HA mock → Entity state changes → Response
```

If you have used a voice assistant like Amazon Alexa or Google Assistant, this
demo works on the same principle: recognized commands map to specific smart home
actions, without any AI inference. The LLM mock uses keyword matching instead
of a real model.

## Requirements

- Docker and Docker Compose
- ~2 GB disk (D.A.W.N. build image)
- No GPU, no API keys, no external services

## Quick Start

```bash
# From the dawn repo root
docker compose -f demos/full-mock/docker-compose.demo.yaml up --build
```

Then open **http://localhost:3000** to access D.A.W.N.'s WebUI.

## Try These Commands

Type in the WebUI chat:

| Command | What Happens |
|---------|-------------|
| `hello` | Greeting with simulation mode info |
| `turn on the kitchen lights` | LLM mock returns HA tool call → kitchen lights turn on |
| `turn off the bedroom lights` | LLM mock returns HA tool call → bedroom lights turn off |
| `set the thermostat` | LLM mock returns HA tool call → thermostat set to 22°C |
| `what can you do` | Lists available simulation commands |
| `what's the weather` | Default response (no rule matched) |

## Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                     Docker Compose                           │
│                                                              │
│  ┌──────────────┐  ┌──────────────────┐  ┌──────────────┐  │
│  │ mqtt-broker   │  │ mock-services    │  │    dawn      │  │
│  │ (Mosquitto)   │  │                  │  │              │  │
│  │ :1883         │  │ HA mock :8123    │  │ WebUI :3000  │  │
│  │               │  │ LLM mock :8080   │  │              │  │
│  └───────┬───────┘  └────────┬─────────┘  └──────┬───────┘  │
│          │                   │                    │          │
│          └───────────────────┼────────────────────┘          │
│                    MQTT + HTTP (Docker network)              │
└─────────────────────────────────────────────────────────────┘
```

### Mock Services

| Service | Port | What It Mocks | E.C.H.O. Class |
|---------|------|---------------|----------------|
| Home Assistant API | 8123 | Smart home entity control | `HomeAssistantMock` |
| LLM API | 8080 | OpenAI-compatible `/v1/chat/completions` | `LLMMock` + `LLMHTTPServer` |
| MQTT Broker | 1883 | Inter-component messaging | Eclipse Mosquitto (real) |

### Default Entities (HA Mock)

| Entity ID | Type | Initial State |
|-----------|------|---------------|
| `light.kitchen_lights` | Light | off |
| `light.bedroom_lights` | Light | off |
| `climate.living_room_thermostat` | Climate | heat (21°C) |

## Adding Custom Skills

Edit `mock_services/entrypoint.py` to add new voice command → tool call mappings:

```python
# Map a new voice command to an HA service call
llm.add_tool_rule("lock the front door",
    tool="homeassistant",
    args={"action": "lock", "entity_id": "lock.front_door"})

# Add a new entity to the HA mock
ha.entities["lock.front_door"] = {
    "entity_id": "lock.front_door",
    "state": "unlocked",
    "attributes": {"friendly_name": "Front Door Lock"},
    "last_changed": "",
    "last_updated": "",
}
```

## Related

- [E.C.H.O. Simulation Framework](https://github.com/malcolmhoward/the-oasis-project-simulation-repo) — Mock implementations
- [M.I.R.A.G.E. HUD Demo](https://github.com/malcolmhoward/mirage/tree/feat/mirage/5-simulation-demo/demos/hud-mock) — HUD display with simulated sensors
- [ADR-0003 Amendment 4](https://github.com/malcolmhoward/the-oasis-project-meta-repo) — Runtime injection and graceful degradation design
