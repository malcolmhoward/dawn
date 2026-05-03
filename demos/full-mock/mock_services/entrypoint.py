"""
Mock services entrypoint — starts HomeAssistantMock and LLMHTTPServer
for the D.A.W.N. simulation demo.

Runs both services in the same container on different ports:
  - HomeAssistantMock on port 8123 (HA REST API)
  - LLMHTTPServer on port 8080 (OpenAI-compatible /v1/chat/completions)

Environment variables:
  HA_PORT (default 8123)
  LLM_PORT (default 8080)
"""

import os
import signal
import sys
import time

from simulation.layer2.ha_mock import HomeAssistantMock
from simulation.layer2.llm_mock import LLMMock
from simulation.layer2.llm_http_server import LLMHTTPServer


def main():
    ha_port = int(os.environ.get("HA_PORT", "8123"))
    llm_port = int(os.environ.get("LLM_PORT", "8080"))

    # --- Home Assistant Mock ---
    ha = HomeAssistantMock(host="0.0.0.0", port=ha_port)

    # --- LLM Mock with smart home skills ---
    llm = LLMMock(default_response="I'm not sure how to help with that. Try asking about the lights or thermostat.")

    # Smart home tool calls — like Alexa skills, these map recognized
    # voice commands to specific Home Assistant service calls.
    llm.add_tool_rule("turn on the kitchen lights",
        tool="homeassistant", args={"action": "turn_on", "entity_id": "light.kitchen_lights"})
    llm.add_tool_rule("turn off the kitchen lights",
        tool="homeassistant", args={"action": "turn_off", "entity_id": "light.kitchen_lights"})
    llm.add_tool_rule("turn on the bedroom lights",
        tool="homeassistant", args={"action": "turn_on", "entity_id": "light.bedroom_lights"})
    llm.add_tool_rule("turn off the bedroom lights",
        tool="homeassistant", args={"action": "turn_off", "entity_id": "light.bedroom_lights"})
    llm.add_tool_rule("set thermostat",
        tool="homeassistant", args={"action": "set_temperature", "entity_id": "climate.living_room_thermostat", "temperature": 22})

    # Conversational responses for queries that don't trigger tool calls
    llm.add_rule("hello", "Hey there! I'm DAWN running in simulation mode. Try asking me to turn on the lights.")
    llm.add_rule("what can you do", "I can control your smart home devices. Try saying 'turn on the kitchen lights' or 'set the thermostat'.")
    llm.add_rule("lights", "I can control the kitchen and bedroom lights. Say 'turn on the kitchen lights' or 'turn off the bedroom lights'.")
    llm.add_rule("status", "All systems running in simulation mode. Home Assistant mock is active with kitchen lights, bedroom lights, and living room thermostat.")

    llm_server = LLMHTTPServer(llm, host="0.0.0.0", port=llm_port)

    # --- Start services ---
    print(f"Starting HomeAssistantMock on port {ha_port}...")
    ha.start()
    print(f"Starting LLMHTTPServer on port {llm_port}...")
    llm_server.start()

    print(f"\nD.A.W.N. simulation services ready:")
    print(f"  Home Assistant API: http://0.0.0.0:{ha_port}/api/states")
    print(f"  LLM API:           http://0.0.0.0:{llm_port}/v1/chat/completions")
    print(f"  LLM models:        http://0.0.0.0:{llm_port}/v1/models")
    print(f"\nDefault entities: kitchen lights, bedroom lights, living room thermostat")
    print(f"Try: 'turn on the kitchen lights', 'set the thermostat', 'hello'")

    # --- Wait for shutdown signal ---
    def shutdown(signum, frame):
        print("\nShutting down mock services...")
        llm_server.stop()
        ha.stop()
        sys.exit(0)

    signal.signal(signal.SIGTERM, shutdown)
    signal.signal(signal.SIGINT, shutdown)

    while True:
        time.sleep(1)


if __name__ == "__main__":
    main()
