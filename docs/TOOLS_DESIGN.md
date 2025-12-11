# DAWN Tools Subsystem Design

This document describes the external tool integrations available to DAWN's LLM, enabling
real-time data retrieval via voice commands. All tools are located in `src/tools/` and
`include/tools/`.

## Table of Contents

1. [Architecture Overview](#architecture-overview)
2. [Calculator (tinyexpr)](#calculator-tinyexpr)
3. [Web Search (SearXNG)](#web-search-searxng)
4. [Weather Service (Open-Meteo)](#weather-service-open-meteo)
5. [Adding New Tools](#adding-new-tools)

---

## Architecture Overview

All tools follow a common pattern:

```
User Voice Command
        │
        ▼
    ASR (Whisper)
        │
        ▼
      LLM ────────────────────────────────────┐
        │                                     │
        ▼                                     ▼
  <command> tag detected                Regular response
  {"device": "...",                           │
   "action": "...",                           ▼
   "value": "..."}                           TTS
        │
        ▼
  Tool Callback (mosquitto_comms.c)
        │
        ▼
  External API (SearXNG, Open-Meteo, etc.)
        │
        ▼
  JSON results formatted for LLM
        │
        ▼
      LLM (summarize/interpret)
        │
        ▼
       TTS
```

### File Structure

```
src/tools/
├── calculator.c        # Math expression evaluator
├── tinyexpr.c          # Expression parser library
├── weather_service.c   # Open-Meteo client
└── web_search.c        # SearXNG client

include/tools/
├── calculator.h        # Calculator API
├── curl_buffer.h       # Shared CURL utilities
├── tinyexpr.h          # Expression parser
├── weather_service.h   # Weather API
└── web_search.h        # Search API
```

---

# Calculator (tinyexpr)

## Overview

The calculator tool provides mathematical expression evaluation using the
[tinyexpr](https://github.com/codeplea/tinyexpr) library. This enables voice commands like
"What's 15% of 847?" or "Calculate the square root of 144". No external dependencies or
API keys required.

## Files

```
src/tools/
    calculator.c          # Calculator module
    tinyexpr.c            # Expression parser (third-party, zlib license)
include/tools/
    calculator.h          # Public API
    tinyexpr.h            # Parser header
```

## Supported Operations

| Category | Operations |
|----------|------------|
| Basic | `+`, `-`, `*`, `/`, `^` (power), `%` (modulo) |
| Functions | `sqrt`, `sin`, `cos`, `tan`, `asin`, `acos`, `atan`, `log`, `log10`, `exp`, `floor`, `ceil`, `abs` |
| Constants | `pi`, `e` |
| Special | `random(min, max)` - random integer in range |

## Command Format

```json
{"device": "calculator", "action": "calculate", "value": "<expression>"}
```

### Examples

| Voice Command | Expression Sent | Result |
|--------------|-----------------|--------|
| "What's 15% of 847?" | `847 * 0.15` | 127.05 |
| "Calculate the square root of 144" | `sqrt(144)` | 12 |
| "What's 2 to the power of 10?" | `2^10` | 1024 |
| "Give me a random number between 1 and 100" | `random(1, 100)` | (random) |

## API

```c
/**
 * @brief Evaluate a mathematical expression
 * @param expression Expression string (e.g., "2 + 2", "sqrt(16)")
 * @return Result as heap-allocated string (caller must free), or error message
 */
char *calculator_evaluate(const char *expression);

/**
 * @brief Generate random integer in range [min, max]
 * @param min Minimum value
 * @param max Maximum value
 * @return Random integer, or min if range invalid
 */
int calculator_random(int min, int max);
```

## Result Formatting

Results are formatted for natural speech:
- Integers displayed without decimal places: `144` not `144.000000`
- Decimals limited to 6 significant digits: `3.141593`
- Large numbers use scientific notation when appropriate

## Error Handling

| Scenario | Response |
|----------|----------|
| Empty expression | "Error: Empty expression" |
| Invalid syntax | "Error evaluating expression: \<expr\>" |
| Division by zero | Returns `inf` or `-inf` |
| Invalid function | Parse error |

## Thread Safety

The calculator module is fully thread-safe:
- `tinyexpr` uses no global state
- Random number generation uses mutex-protected seeding
- Each evaluation is independent

## LLM Prompt Configuration

From `dawn.h` AI_DESCRIPTION:

```
For CALCULATOR: use action 'calculate' with a mathematical expression.
Example: <command>{"device":"calculator","action":"calculate","value":"sqrt(144)"}</command>
Supports: +, -, *, /, ^, sqrt, sin, cos, tan, log, exp, pi, e, random(min,max).
Convert natural language to expressions (e.g., "15% of 200" → "200 * 0.15").
```

---

# Web Search (SearXNG)

## Overview

This section outlines the integration of [SearXNG](https://docs.searxng.org/), a self-hosted
metasearch engine, with DAWN to provide web search capabilities via voice commands.

## Architecture

```
User Voice Command
        │
        ▼
    ASR (Whisper)
        │
        ▼
      LLM ────────────────────────────────────┐
        │                                     │
        ▼                                     ▼
  <command> tag detected              Regular response
  {"action": "search",                        │
   "query": "..."}                            ▼
        │                                    TTS
        ▼
  web_search.c module
        │
        ▼
  SearXNG (localhost:8384)
        │
        ▼
  JSON results (top 5)
        │
        ▼
  Format as system message
        │
        ▼
      LLM (summarize)
        │
        ▼
       TTS
```

## SearXNG Deployment

### Docker Compose Configuration

Location: `/home/jetson/docker/searxng/docker-compose.yml`

```yaml
version: '3.8'

services:
  searxng:
    image: searxng/searxng:latest
    container_name: searxng
    restart: unless-stopped
    ports:
      - "8384:8080"
    volumes:
      - ./searxng:/etc/searxng:rw
    environment:
      - SEARXNG_BASE_URL=http://localhost:8384/
    cap_drop:
      - ALL
    cap_add:
      - CHOWN
      - SETGID
      - SETUID
    logging:
      driver: "json-file"
      options:
        max-size: "1m"
        max-file: "1"
```

### SearXNG Settings

Location: `/home/jetson/docker/searxng/searxng/settings.yml`

```yaml
use_default_settings: true

general:
  instance_name: "DAWN Search"
  debug: false

server:
  secret_key: "<GENERATE_RANDOM_KEY>"
  bind_address: "0.0.0.0"
  port: 8080
  method: "GET"
  image_proxy: false
  limiter: false              # Not needed for local-only access
  public_instance: false      # Private instance

search:
  safe_search: 1              # Moderate - filter explicit content
  default_lang: "en"
  autocomplete: ""            # Disabled - not needed for API use
  formats:
    - json                    # Only JSON needed for DAWN integration
  max_page: 1                 # Single page of results is sufficient

ui:
  static_use_hash: true

# Engine Configuration
# Using keep_only to whitelist specific engines for faster, focused results
engines:
  - name: google
    disabled: false
  - name: duckduckgo
    disabled: false
  - name: brave
    disabled: false
  - name: wikipedia
    disabled: false
  - name: bing
    disabled: false
```

### Alternative: Full Engine List

If broader search coverage is preferred over speed:

```yaml
# Remove keep_only and let defaults apply, then disable unwanted engines:
engines:
  - name: google
    disabled: false
  - name: bing
    disabled: false
  - name: duckduckgo
    disabled: false
  - name: brave
    disabled: false
  - name: wikipedia
    disabled: false
  - name: wikidata
    disabled: false
  # Disable slow/unreliable engines:
  - name: yahoo
    disabled: true
  - name: ask
    disabled: true
```

## DAWN Integration

### Files

```
src/tools/
    web_search.c          # SearXNG client module
include/tools/
    web_search.h          # Public API
```

### Search Types

| Type | Action | SearXNG Parameter | Use Case |
|------|--------|-------------------|----------|
| `SEARCH_TYPE_WEB` | `web` | None (default) | General web search |
| `SEARCH_TYPE_NEWS` | `news` | `categories=news` | Current events, news articles |
| `SEARCH_TYPE_FACTS` | `facts` | `engines=wikipedia` | Factual data, Wikipedia infoboxes |

### Command Format

```json
{"device": "search", "action": "<type>", "value": "query"}
```

Where `<type>` is one of: `web`, `news`, or `facts`.

### Header: web_search.h

```c
#ifndef WEB_SEARCH_H
#define WEB_SEARCH_H

#define SEARXNG_DEFAULT_URL "http://localhost:8384"
#define SEARXNG_MAX_RESULTS 5
#define SEARXNG_TIMEOUT_SEC 10

/**
 * @brief Search type enum for different search categories
 */
typedef enum {
   SEARCH_TYPE_WEB,   // General web search (default)
   SEARCH_TYPE_NEWS,  // News articles only (categories=news)
   SEARCH_TYPE_FACTS  // Wikipedia/factual data (engines=wikipedia, uses infoboxes)
} search_type_t;

typedef struct {
   char *title;
   char *url;
   char *snippet;
   char *engine;
} search_result_t;

typedef struct {
   search_result_t *results;
   int count;
   float query_time_sec;
   char *error;
} search_response_t;

int web_search_init(const char *searxng_url);
search_response_t *web_search_query(const char *query, int max_results);
search_response_t *web_search_query_typed(const char *query, int max_results, search_type_t type);
int web_search_format_for_llm(const search_response_t *response, char *buffer, size_t buffer_size);
void web_search_free_response(search_response_t *response);
void web_search_cleanup(void);
int web_search_is_initialized(void);

#endif /* WEB_SEARCH_H */
```

### Implementation Sketch: web_search.c

```c
// Uses libcurl for HTTP requests
// Uses json-c for JSON parsing (already a project dependency)

static char *searxng_base_url = NULL;

int web_search_init(const char *url) {
   searxng_base_url = strdup(url ? url : SEARXNG_DEFAULT_URL);
   curl_global_init(CURL_GLOBAL_DEFAULT);
   return 0;
}

search_response_t *web_search_query(const char *query, int max_results) {
   // Build URL: {base}/search?q={query}&format=json
   // Perform HTTP GET with timeout
   // Parse JSON response
   // Extract: results[].title, results[].url, results[].content, results[].engine
   // Return structured response
}

int web_search_format_for_llm(const search_response_t *response,
                               char *buffer, size_t buffer_size) {
   // Format as concise text for LLM:
   // "Web search results for '{query}':\n"
   // "1. {title} ({engine})\n   {snippet}\n   URL: {url}\n"
   // ...
   // Keep total under ~1000 chars to minimize token usage
}
```

### Command Integration

#### Update: commands_config_nuevo.json

```json
{
  "types": {
    "search": {
      "actions": {
        "web_search": {
          "action_command": "{\"device\": \"search\", \"action\": \"web\", \"value\": \"$QUERY\"}"
        }
      }
    }
  },
  "devices": {
    "search": {
      "type": "search",
      "topic": "dawn/search"
    }
  }
}
```

#### Update: dawn.h (AI_DESCRIPTION)

Add to the system prompt:

```
You can search the web for current information. To search, use:
<command>{"device": "search", "action": "web", "value": "your search query"}</command>

Use web search when:
- Asked about current events, news, or recent information
- Asked about topics you're uncertain about
- Asked to look something up or find information
- The user explicitly asks you to search

The search results will be provided in the next message for you to summarize.
```

#### New Callback: mosquitto_comms.c

```c
char *searchCallback(const char *actionName, char *value, int *should_respond) {
   *should_respond = 1;  // Always return results to LLM

   if (strcmp(actionName, "web") == 0) {
      search_response_t *response = web_search_query(value, SEARXNG_MAX_RESULTS);
      if (response && response->count > 0) {
         char *result = malloc(4096);
         web_search_format_for_llm(response, result, 4096);
         web_search_free_response(response);
         return result;
      }
      return strdup("Web search returned no results.");
   }

   return strdup("Unknown search action.");
}
```

### CMakeLists.txt Updates

```cmake
# Add search module
set(DAWN_SOURCES
   ...
   src/search/web_search.c
)

# Ensure libcurl is linked (should already be present for OpenAI)
find_package(CURL REQUIRED)
target_link_libraries(dawn ${CURL_LIBRARIES})
```

## Configuration Options

### Environment Variables (dawn.h or runtime)

```c
#define SEARXNG_URL       "http://localhost:8384"
#define SEARXNG_TIMEOUT   10    // seconds
#define SEARXNG_MAX_RESULTS 5   // results to fetch
#define SEARXNG_SNIPPET_LEN 200 // max chars per snippet for LLM
```

### Potential secrets.h Addition

```c
// If using authenticated SearXNG or external instance:
// #define SEARXNG_API_KEY "..."
```

## Deployment Steps

### Phase 1: SearXNG Setup

1. Create directory structure:
   ```bash
   mkdir -p ~/docker/searxng/searxng
   ```

2. Create docker-compose.yml (see above)

3. Create settings.yml with generated secret key:
   ```bash
   openssl rand -hex 32  # Generate secret_key
   ```

4. Start container:
   ```bash
   cd ~/docker/searxng
   docker-compose up -d
   ```

5. Verify:
   ```bash
   curl "http://localhost:8384/search?q=test&format=json" | jq '.results[:2]'
   ```

### Phase 2: DAWN Integration

1. Create `src/tools/web_search.c` and `include/tools/web_search.h`
2. Add to CMakeLists.txt
3. Add callback registration in mosquitto_comms.c
4. Update AI_DESCRIPTION in dawn.h
5. Update commands_config_nuevo.json
6. Build and test

### Phase 3: Testing

1. Voice: "Friday, search for weather in Austin"
2. Verify SearXNG receives query
3. Verify results formatted and sent to LLM
4. Verify LLM summarizes and TTS speaks response

## Confirmed Configuration

| Setting | Value | Rationale |
|---------|-------|-----------|
| Port | 8384 | Avoids common defaults (8080, 8000, 8888) |
| Safe Search | 1 (Moderate) | Filter explicit content |
| Engines | Fast (5) | Google, DuckDuckGo, Brave, Wikipedia, Bing - ~500ms |
| Result Count | 5 | Balanced context vs token cost |
| Redis Cache | Skip | Not needed for low-volume personal use |

## Error Handling

| Scenario | Handling |
|----------|----------|
| SearXNG unreachable | Return "Search service unavailable" to LLM |
| Query timeout | Return "Search timed out" to LLM |
| No results | Return "No results found for {query}" to LLM |
| Invalid JSON | Log error, return generic failure message |

## Security Considerations

1. **Localhost only:** Bind to 127.0.0.1 or use Docker network isolation
2. **No public exposure:** Don't expose port externally
3. **Rate limiting:** Not needed for single-user local instance
4. **Secret key:** Generate strong random key, store in settings.yml (not in code)

## Future Enhancements

1. **Category support:** Add ability to search specific categories (news, images, maps)
2. **Search history:** Log searches for debugging/analytics
3. **Caching:** Add Redis for frequently repeated queries
4. **Timeout tuning:** Adjust per-engine timeouts based on observed latency
5. **Result scoring:** Weight results by engine reliability

## References

- [SearXNG Documentation](https://docs.searxng.org/)
- [SearXNG GitHub](https://github.com/searxng/searxng)
- [SearXNG Docker Setup](https://github.com/searxng/searxng-docker)
- [Settings Reference](https://docs.searxng.org/admin/settings/settings.html)

---

# Weather Service (Open-Meteo)

## Overview

Weather integration uses [Open-Meteo](https://open-meteo.com/), a free weather API that requires
no API key. The service provides current conditions and forecasts up to 7 days.

## API Endpoints

| Endpoint | Purpose |
|----------|---------|
| `geocoding-api.open-meteo.com/v1/search` | Convert location names to coordinates |
| `api.open-meteo.com/v1/forecast` | Fetch weather data |

## Files

```
src/tools/
    weather_service.c     # Open-Meteo client
include/tools/
    weather_service.h     # Weather API
```

## Command Format

```json
{"device": "weather", "action": "<type>", "value": "City, State"}
```

### Forecast Types

| Action | Days | Use Case |
|--------|------|----------|
| `today` | 1 | Current conditions ("What's the weather right now?") |
| `tomorrow` | 2 | Short-term ("What's the weather tomorrow?") |
| `week` | 7 | Extended ("What's the forecast for this weekend?") |

The LLM selects the appropriate action based on the user's question.

## Response Format

The weather callback returns JSON formatted for LLM consumption:

```json
{
  "location": "Atlanta, Georgia, United States",
  "current": {
    "temperature_f": 52.3,
    "feels_like_f": 48.1,
    "humidity": 65,
    "condition": "Partly cloudy",
    "wind_mph": 8.5,
    "wind_direction": 180
  },
  "forecast": [
    {
      "date": "2025-01-15",
      "high_f": 58.0,
      "low_f": 42.0,
      "condition": "Clear sky",
      "precipitation_chance": 10
    }
  ]
}
```

## Key Data Structures

### Forecast Type Enum

```c
typedef enum {
   FORECAST_TODAY,     // Current conditions only (1 day)
   FORECAST_TOMORROW,  // Today and tomorrow (2 days)
   FORECAST_WEEK       // Full 7-day forecast
} forecast_type_t;
```

### Weather Response

```c
typedef struct {
   char *location_name;
   double latitude;
   double longitude;
   current_weather_t current;
   daily_forecast_t daily[MAX_FORECAST_DAYS];  // Up to 7 days
   int num_days;
   char *error;
} weather_response_t;
```

## Geocoding

Location strings are automatically geocoded:
- Parses "City, State" or "City, Country" format
- Queries Open-Meteo geocoding API with city name
- Filters results by state/region if provided
- Falls back to first result if no exact match

Example: "Sugar Hill, Georgia" → lat=34.1065, lon=-84.0335

## Unit Conversions

The API returns metric units; the service converts to imperial:
- Temperature: Celsius → Fahrenheit
- Wind speed: km/h → mph
- Precipitation: mm → inches

## Error Handling

| Scenario | Handling |
|----------|----------|
| Location not found | Return "Failed to find location" |
| API timeout | Return curl error message |
| Invalid JSON | Return "Failed to parse weather response" |
| Buffer overflow | Return "Weather data too large to format" |

## LLM Prompt Configuration

From `dawn.h` AI_DESCRIPTION:

```
9. For WEATHER: use action 'today' (current), 'tomorrow' (2-day), or 'week' (7-day forecast).
   Example: <command>{"device":"weather","action":"week","value":"City, State"}</command>.
   If user provides location, use it directly. Only ask for location if not specified.
   Choose action based on user's question (e.g., 'this weekend' -> week, 'right now' -> today).
```

## References

- [Open-Meteo API Documentation](https://open-meteo.com/en/docs)
- [Geocoding API](https://open-meteo.com/en/docs/geocoding-api)
- [WMO Weather Codes](https://open-meteo.com/en/docs#weathervariables)

---

# Adding New Tools

## Checklist

1. **Create source files:**
   - `src/tools/new_tool.c`
   - `include/tools/new_tool.h`

2. **Add to CMakeLists.txt:**
   ```cmake
   # Tools subsystem
   src/tools/calculator.c
   src/tools/tinyexpr.c
   src/tools/weather_service.c
   src/tools/web_search.c
   src/tools/new_tool.c  # Add here
   ```

3. **Register device type in `mosquitto_comms.h`:**
   ```c
   typedef enum {
      // ... existing types ...
      NEW_TOOL,
      MAX_DEVICE_TYPES
   } deviceType;
   ```

4. **Add callback in `mosquitto_comms.c`:**
   ```c
   static deviceCallback deviceCallbackArray[] = {
      // ... existing callbacks ...
      { NEW_TOOL, newToolCallback }
   };
   ```

5. **Update AI_DESCRIPTION in `dawn.h`:**
   Add rules and examples for when/how to use the new tool.

## Callback Contract

All tool callbacks must follow this pattern:

```c
char *newToolCallback(const char *actionName, char *value, int *should_respond) {
   *should_respond = 1;  // Set to 1 to return data to LLM

   // Perform action...

   // Return heap-allocated string (caller frees) or NULL
   return strdup("Result data for LLM");
}
```

See `include/mosquitto_comms.h` for the full callback contract documentation.
