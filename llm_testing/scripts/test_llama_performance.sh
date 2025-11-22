#!/bin/bash

# LLM Performance Test Script for DAWN
# Tests tokens/sec and latency with different prompts

SERVER="http://127.0.0.1:8080"

echo "==================================================================="
echo "DAWN Local LLM Performance Test"
echo "==================================================================="
echo ""

# Test 1: Simple greeting (short response)
echo "Test 1: Simple greeting"
echo "-------------------------------------------------------------------"
RESPONSE=$(curl -s -X POST "$SERVER/v1/chat/completions" \
  -H "Content-Type: application/json" \
  -d '{
    "messages": [
      {"role": "system", "content": "You are FRIDAY, Tony Stark'\''s AI assistant."},
      {"role": "user", "content": "Hello FRIDAY"}
    ],
    "temperature": 0.7,
    "max_tokens": 50,
    "stream": false
  }')

echo "$RESPONSE" | python3 -c "
import sys, json
data = json.load(sys.stdin)
print('Response:', data['choices'][0]['message']['content'])
print('Tokens:', data['usage']['completion_tokens'])
print('Time:', round(data['timings']['predicted_ms']/1000, 2), 'seconds')
print('Tokens/sec:', round(data['timings']['predicted_per_second'], 2))
print('TTFT:', round(data['timings']['prompt_ms'], 2), 'ms')
"
echo ""

# Test 2: Medium complexity (typical DAWN query)
echo "Test 2: Conversational query"
echo "-------------------------------------------------------------------"
RESPONSE=$(curl -s -X POST "$SERVER/v1/chat/completions" \
  -H "Content-Type: application/json" \
  -d '{
    "messages": [
      {"role": "system", "content": "You are FRIDAY, Tony Stark'\''s AI assistant in the OASIS Project."},
      {"role": "user", "content": "What can you tell me about the workshop systems today?"}
    ],
    "temperature": 0.7,
    "max_tokens": 100,
    "stream": false
  }')

echo "$RESPONSE" | python3 -c "
import sys, json
data = json.load(sys.stdin)
print('Response:', data['choices'][0]['message']['content'][:150], '...')
print('Tokens:', data['usage']['completion_tokens'])
print('Time:', round(data['timings']['predicted_ms']/1000, 2), 'seconds')
print('Tokens/sec:', round(data['timings']['predicted_per_second'], 2))
print('TTFT:', round(data['timings']['prompt_ms'], 2), 'ms')
"
echo ""

# Test 3: With conversation history (context test)
echo "Test 3: Multi-turn conversation"
echo "-------------------------------------------------------------------"
RESPONSE=$(curl -s -X POST "$SERVER/v1/chat/completions" \
  -H "Content-Type: application/json" \
  -d '{
    "messages": [
      {"role": "system", "content": "You are FRIDAY, Tony Stark'\''s AI assistant."},
      {"role": "user", "content": "How are the workshop systems?"},
      {"role": "assistant", "content": "All systems operational, sir. OASIS is running smoothly."},
      {"role": "user", "content": "Good. What about the suit status?"}
    ],
    "temperature": 0.7,
    "max_tokens": 80,
    "stream": false
  }')

echo "$RESPONSE" | python3 -c "
import sys, json
data = json.load(sys.stdin)
print('Response:', data['choices'][0]['message']['content'])
print('Tokens:', data['usage']['completion_tokens'])
print('Time:', round(data['timings']['predicted_ms']/1000, 2), 'seconds')
print('Tokens/sec:', round(data['timings']['predicted_per_second'], 2))
print('TTFT:', round(data['timings']['prompt_ms'], 2), 'ms')
print('Prompt tokens:', data['usage']['prompt_tokens'])
"
echo ""

echo "==================================================================="
echo "Performance Summary"
echo "==================================================================="
echo ""
echo "Target Performance:"
echo "  - Tokens/sec: 25+ (acceptable), 40+ (excellent)"
echo "  - TTFT: <200ms"
echo "  - 50-token response: <2 seconds"
echo ""
echo "Current Model: $(basename /var/lib/llama-cpp/models/*.gguf 2>/dev/null || echo 'Unknown')"
echo ""
echo "If tokens/sec < 20, consider:"
echo "  1. Download Q4_K_M quantization instead of Q6_K_L"
echo "  2. Reduce context size: -c 1024"
echo "  3. Increase batch size: -b 512 -ub 512"
echo "  4. Try smaller model (Llama 3.2 3B)"
echo ""
