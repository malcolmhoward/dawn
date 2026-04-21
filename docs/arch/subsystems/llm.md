# LLM Subsystem

Source: `src/llm/`, `include/llm/`. The sentence buffer (`common/src/utils/sentence_buffer.c`) lives in the common library because it is also used by Tier 1 satellites.

Part of the [D.A.W.N. architecture](../../../ARCHITECTURE.md) — see the main doc for layer rules, threading model, and lock ordering.

---

**Purpose**: Large Language Model integration with streaming support.

## Architecture Pattern: Strategy + Observer

- **Strategy**: multiple LLM providers (OpenAI, Claude, Gemini, local) via a unified interface.
- **Observer**: streaming responses notify the sentence buffer for real-time TTS.

## Key Components

- **llm_interface.c/h**: LLM abstraction layer
   - `LLMContext` struct: provider-agnostic context
   - `llm_init()`: initialize selected provider
   - `llm_send_message()`: send message, get complete response (blocking)
   - `llm_send_message_streaming()`: send message, stream response chunks
   - Provider selection based on configuration (`OPENAI_MODEL`, `ANTHROPIC_MODEL`)

- **llm_openai.c/h**: OpenAI API implementation
   - Supports GPT-5 series, GPT-4o, GPT-4
   - Supports llama.cpp local server (OpenAI-compatible endpoint)
   - Supports Ollama with runtime model switching
   - Supports Google Gemini (via OpenAI-compatible endpoint)
   - Both blocking and streaming modes
   - Conversation history management
   - Extended thinking support (reasoning_effort for OpenAI/Gemini models)

- **llm_claude.c/h**: Claude API implementation
   - Supports Claude 4.6 Opus/Sonnet, Claude 4.5 Sonnet
   - Streaming support
   - Different API format than OpenAI (Messages API)
   - Extended thinking support with configurable token budget
   - Full thinking content visibility (unlike OpenAI/Gemini)

- **llm_streaming.c/h**: Streaming response handler
   - Manages Server-Sent Events (SSE) connections
   - Buffers and parses incoming chunks
   - Notifies sentence buffer for TTS integration

- **sse_parser.c/h**: Server-Sent Events parser
   - Parses SSE format: `data: {...}\n\n`
   - Extracts JSON content from events
   - Handles partial events across network chunks

- **`common/src/utils/sentence_buffer.c`, `common/include/utils/sentence_buffer.h`**: Sentence boundary detection (shared with satellite)
   - Buffers streaming text until complete sentence
   - Detects sentence boundaries (`.`, `!`, `?`), bullets, numbered lists, `:\n`, `\n\n`
   - Sends complete sentences to TTS for natural phrasing
   - Reduces perceived latency (speak while generating)

- **llm_command_parser.c/h**: JSON command extraction
   - Extracts `<command>` JSON tags from LLM responses
   - Validates JSON structure
   - Handles malformed JSON gracefully

- **llm_rate_limit.c/h**: Cloud API rate limiter
   - Process-wide sliding window throttle (default 40 RPM, configurable)
   - Gates all cloud LLM call paths; local providers bypass
   - Interrupt-aware blocking (wakes on shutdown signal)

## LLM Worker Thread

LLM processing is non-blocking — the main audio loop never waits on an API call.

```
┌───────────────────────────────────────────────────────────┐
│                      Main Thread                          │
│  - State machine (never blocks on LLM)                    │
│  - Audio capture + VAD (continuous, 50ms intervals)       │
│  - ASR processing (Whisper/Vosk)                          │
│  - TTS synthesis (mutex protected)                        │
│  - LLM completion detection (polling llm_processing flag) │
└────────────┬──────────────────────────────────────────────┘
             │ spawns on-demand, max 1 concurrent
             ▼
┌───────────────────────────────────────────────────────────┐
│                   LLM Worker Thread                       │
│  - Blocking CURL call to LLM API                          │
│  - CURL progress callback (checks interrupt flag)         │
│  - Returns response via shared buffer                     │
│  - Thread-safe via llm_mutex                              │
└───────────────────────────────────────────────────────────┘
```

Request and response buffers use **ownership transfer** to prevent data races. The mutex is held only during transfer, not during processing.

### Interrupt Mechanism

Users can interrupt an in-flight LLM call by saying the wake word. The CURL progress callback checks `llm_interrupt_requested` periodically; wake word detection sets the flag via `llm_request_interrupt()`, returning non-zero from the callback aborts the transfer, and the main thread discards the partial response and rolls back conversation history.

## Data Flow (Streaming Mode)

```
User Query → LLM Provider (OpenAI/Claude/Local)
                    ↓ (SSE stream)
            SSE Parser → Streaming Handler
                    ↓ (text chunks)
            Sentence Buffer → TTS (as sentences complete)
                    ↓ (complete response)
            Command Parser → MQTT Commands
```

## Performance Comparison

| Provider                | Quality | TTFT      | Latency | Cost          |
| ----------------------- | ------- | --------- | ------- | ------------- |
| OpenAI GPT-5            | 100%    | ~300ms    | ~3.1s   | ~$0.01/query  |
| Claude 4.6 Sonnet       | 92.4%   | ~400ms    | ~3.5s   | ~$0.015/query |
| Gemini 2.5 Flash        | ~90%    | ~250ms    | ~2.5s   | ~$0.002/query |
| llama.cpp (Qwen3-4B Q4) | 81.9%   | 116-138ms | ~1.5s   | FREE          |
| Ollama (Qwen3-4B Q4)    | 81.9%   | ~150ms    | ~1.6s   | FREE          |

**TTFT = Time To First Token** (lower = faster perceived response).
