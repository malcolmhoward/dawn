# Plex Music Integration Design

**Goal:** Allow DAWN to stream music from a local Plex Media Server as an alternative to the local filesystem music library, selectable via a settings toggle.

**Last Updated:** February 2026

---

## Overview

DAWN currently plays music from a local directory (`paths.music_dir`), scanned into a SQLite database by `music_scanner.c` / `music_db.c`. The audio pipeline is:

```
local file → audio_decoder (FLAC/MP3/OGG) → resampler → Opus encoder → WebSocket broadcast
```

Plex integration adds an alternative **source** that replaces the first step:

```
HTTP download from Plex → temp file → audio_decoder (FLAC/MP3/OGG) → resampler → Opus encoder → WebSocket broadcast
```

Everything downstream of `audio_decoder_open()` stays the same. The key changes are:

1. A **`[music]` source selector** in config (`source = "local"` or `"plex"`)
2. A **Plex API client** (`plex_client.c`) for auth, browsing, search, and stream URL construction
3. An **HTTP download step** in `webui_music_start_playback()` that fetches Plex tracks to temp files before opening the decoder (the decoder layer stays unaware of HTTP)
4. **Mapping** Plex library responses into the existing `music_library_response` / `music_search_response` JSON formats

The WebUI, satellite music panels, LLM music tool, and Opus streaming pipeline all remain unchanged — they operate on the same `webui_music.c` playback engine regardless of whether the audio comes from a local file or Plex HTTP stream.

---

## Settings Integration

### Config Schema (`dawn.toml`)

```toml
[music]
source = "local"              # "local" or "plex"
scan_interval_minutes = 60    # Only used when source = "local"

[music.plex]
host = "192.168.1.100"        # Plex server IP or hostname
port = 32400                  # Default Plex port
music_section_id = 0          # 0 = auto-discover from /library/sections
ssl = false                   # Use HTTPS for Plex API calls
ssl_verify = true             # Verify TLS certificates (set false for self-signed)
client_identifier = ""        # Auto-generated UUID on first run (persistent)
```

**Token storage:** The Plex token is stored in `secrets.toml` (NOT `dawn.toml`), following the same pattern as OpenAI/Claude/Gemini API keys:

```toml
# In secrets.toml
[secrets]
plex_token = "your-X-Plex-Token-here"
```

This ensures the token is never serialized to `dawn.toml`, never sent in `get_config_response` WebSocket messages, and is protected by `chmod 0600` via the existing `secrets_write_toml()` function.

### WebUI Settings Panel

Add a "Music Source" dropdown in the existing Music & Media settings category:

| Setting | Type | Visibility | Notes |
|---------|------|-----------|-------|
| Music Source | dropdown | Always | Local, Plex |
| Library Rescan Interval | number | `source = "local"` | Existing field |
| Plex Server | text | `source = "plex"` | IP or hostname |
| Plex Port | number | `source = "plex"` | Default 32400 |
| Plex Music Library | number | `source = "plex"` | 0 = auto-detect |
| Use SSL | toggle | `source = "plex"` | HTTPS for API calls |
| Plex Token | password | Secrets panel | Not in schema — add to existing Secrets section |

**Conditional visibility:** The schema system needs a `showWhen` mechanism to hide Plex fields when `source = "local"` (and hide `scan_interval_minutes` when `source = "plex"`). This is a new general-purpose schema feature:

```javascript
host: {
   type: 'text',
   label: 'Plex Server',
   hint: 'Plex server IP or hostname',
   configPath: 'music.plex.host',
   showWhen: { key: 'music.source', value: 'plex' },
},
```

Implementation: ~15-20 lines in `createSettingField()` — add a CSS class and register a listener on the controlling field that toggles visibility. This mechanism will be reusable for future conditional settings (e.g., MQTT fields when MQTT enabled, FlareSolverr endpoint when enabled).

**Token in Secrets panel:** Add a "Plex Token" row to the existing Secrets panel in `index.html` alongside OpenAI/Claude/Gemini keys. Route through `handle_set_secrets()` and `secrets_to_json_status()` (is_set flag only, never the actual value). The hint text should guide users: *"Find your token: open Plex Web, go to any media, click ... > Get Info > View XML, and copy the X-Plex-Token from the URL."*

**Source switch behavior:** When the user switches from Local → Plex (or vice versa), the daemon:
1. **If music is playing or queue is non-empty**: Send a confirmation prompt via WebSocket before switching (prevents accidental data loss)
2. Stops any current playback
3. Clears the queue
4. Switches the library/search/browse backend
5. Does NOT rescan or rebuild anything — both backends are stateless queries

### Source Indicator in Music Player

Add a small source badge in the music panel header showing "LOCAL" or "PLEX" as part of the `music_state` WebSocket message. This helps users understand which library they're browsing and aids debugging ("why is my library empty?" → because Plex is selected but not configured).

### LLM Tool Integration

The existing `music_tool.c` handles actions like `play`, `search`, `queue`, `stop`. These route through `webui_music.c` which calls `music_db_search()` for library queries and `audio_decoder_open()` for playback.

With the source switch:
- When `source = "local"`: library queries go to `music_db_search()` (SQLite), playback opens local files
- When `source = "plex"`: library queries go to `plex_client_search()` (HTTP API), playback opens Plex stream URLs via temp-file download

The LLM tool itself doesn't change — it still says "play Dark Side of the Moon" and the backend resolves it through whichever source is active. The tool's parameter schema (`action`, `query`, `artist`, `album`) maps naturally to both backends.

---

## Plex API Reference

All endpoints live at `http://<host>:<port>/`. Plex returns XML by default; request JSON via `Accept: application/json` header.

### Authentication

Every request requires an `X-Plex-Token`, passed as either:
- Query parameter: `?X-Plex-Token=<token>`
- HTTP header: `X-Plex-Token: <token>`

**Required client identification headers** (all requests):

```
X-Plex-Client-Identifier: <persistent-uuid>   (REQUIRED — generate once, store in config)
X-Plex-Product: DAWN
X-Plex-Version: <dawn-version>
X-Plex-Platform: Linux
X-Plex-Device: <hostname>
```

The `X-Plex-Client-Identifier` must be generated with `/dev/urandom` or `uuid_generate()` and stored persistently in `[music.plex] client_identifier`. It is not a secret, just needs to be stable and unique. Auto-generate on first use if empty.

**Obtaining a token:**

| Method | How |
|--------|-----|
| **Manual** (recommended for MVP) | User copies token from Plex Web → Settings → Account → XML link URL contains `X-Plex-Token` |
| **MyPlex sign-in** | `POST https://plex.tv/api/v2/users/signin` with `login` + `password` form fields. Returns `{ "authToken": "..." }` |
| **Local auth bypass** | Plex Server Settings → Network → "List of IP addresses allowed without auth". No token needed if DAWN's IP is listed |

**Token notes:**
- Plex is transitioning to JWT-based tokens (2025+). Transient tokens may expire after 48 hours.
- For a persistent appliance, storing a long-lived token in `secrets.toml` (mode 0600) is safest.
- If token expires, API calls return HTTP 401. The daemon should send a `music_state` update with an `error` field ("Plex auth failed — update your token in Settings > Secrets"). The music player panel displays this as a persistent error banner (not a transient toast) with a "Go to Settings" link.

Reference: [Finding an authentication token](https://support.plex.tv/articles/204059436-finding-an-authentication-token-x-plex-token/)

### Server Discovery (GDM Protocol)

Plex servers advertise via the "G'Day Mate" (GDM) UDP protocol. This enables auto-discovery without manual IP configuration.

**Discovery request:**
- Send `M-SEARCH * HTTP/1.1\r\n\r\n` via UDP to port **32414**
- Target: multicast `239.0.0.250` (preferred) or subnet broadcast derived from the host's network interface (do NOT hardcode `192.168.1.255`)

**Discovery response** (from each Plex server on the network):

```
HTTP/1.1 200 OK\r
Content-Type: plex/media-server\r
Name: My Plex Server\r
Port: 32400\r
Resource-Identifier: a1b2c3d4-...\r
Version: 1.40.0.1234-abcdef\r
```

**Security:** GDM responses are unauthenticated UDP — any device on the LAN can spoof a response. Discovery results MUST be presented to the user for manual confirmation, never auto-applied. After user selects a discovered server, validate by calling `GET /identity` with the token to confirm the server's `machineIdentifier`.

**Implementation requirements:**
- Run in a detached thread or with `select()`/`poll()` (not blocking the main/LWS thread)
- 3-second timeout, reset `fromlen` before each `recvfrom()`, cap at 10 responses
- Parse header values with `snprintf`/`strncpy` and explicit size limits
- Validate `Port:` is integer in range 1-65535

Reference: [GDM protocol — python-plexapi](https://python-plexapi.readthedocs.io/en/latest/modules/gdm.html)

### Library Browsing Endpoints

**Step 1: Find the music library section**

```
GET /library/sections
Accept: application/json
X-Plex-Token: <token>
```

Response:
```json
{
  "MediaContainer": {
    "Directory": [
      {
        "key": "3",
        "title": "Music",
        "type": "artist"
      }
    ]
  }
}
```

Filter for `type == "artist"` to find music sections. Cache the `key` (e.g., `"3"`).

**Step 2: List artists**

```
GET /library/sections/{section_id}/all?type=8
X-Plex-Container-Start=0
X-Plex-Container-Size=50
Accept: application/json
```

Type codes: `8` = artist, `9` = album, `10` = track.

Response:
```json
{
  "MediaContainer": {
    "size": 150,
    "totalSize": 150,
    "Directory": [
      {
        "ratingKey": "12345",
        "key": "/library/metadata/12345/children",
        "type": "artist",
        "title": "Pink Floyd",
        "thumb": "/library/metadata/12345/thumb/1234567890"
      }
    ]
  }
}
```

Supports pagination (`X-Plex-Container-Start`, `X-Plex-Container-Size`) and filtering (`?title=Pink`).

**Always paginate** with a reasonable page size (50-100 items). A Plex library with 10,000+ artists produces multi-megabyte JSON responses that spike memory if parsed all at once. Cap the API response buffer at 2MB (`CURL_BUFFER_MAX_PLEX_API`), analogous to the existing `CURL_BUFFER_MAX_WEB_SEARCH` pattern.

**Step 3: List albums for an artist**

```
GET /library/metadata/{artist_ratingKey}/children
Accept: application/json
```

Response:
```json
{
  "MediaContainer": {
    "Directory": [
      {
        "ratingKey": "12346",
        "key": "/library/metadata/12346/children",
        "type": "album",
        "title": "The Dark Side of the Moon",
        "year": 1973,
        "parentTitle": "Pink Floyd"
      }
    ]
  }
}
```

**Step 4: List tracks for an album**

```
GET /library/metadata/{album_ratingKey}/children
Accept: application/json
```

Response (critical — contains stream URL data):
```json
{
  "MediaContainer": {
    "Metadata": [
      {
        "ratingKey": "12347",
        "type": "track",
        "title": "Time",
        "parentTitle": "The Dark Side of the Moon",
        "grandparentTitle": "Pink Floyd",
        "index": 4,
        "duration": 413000,
        "Media": [
          {
            "container": "flac",
            "audioCodec": "flac",
            "audioChannels": 2,
            "bitrate": 1024,
            "Part": [
              {
                "key": "/library/parts/9877/1234567890/file.flac",
                "container": "flac",
                "duration": 413000,
                "size": 52428800
              }
            ]
          }
        ]
      }
    ]
  }
}
```

**Step 5: All tracks for an artist (flat list)**

```
GET /library/metadata/{artist_ratingKey}/allLeaves
X-Plex-Container-Size=200
Accept: application/json
```

Returns all tracks under an artist without drilling through albums. Useful for "play all by artist." Always paginate with `X-Plex-Container-Size` to avoid unbounded responses.

Reference: [Plex Media Server URL Commands](https://support.plex.tv/articles/201638786-plex-media-server-url-commands/) | [plexapi.dev Library API](https://plexapi.dev/api-reference/library/get-all-libraries)

### Playlist Browsing

```
GET /playlists
Accept: application/json
X-Plex-Token: <token>
```

Get playlist items:
```
GET /playlists/{playlist_id}/items
Accept: application/json
X-Plex-Token: <token>
```

Returns `Track` elements with full `Media`/`Part` data for music playlists. Maps cleanly into existing `music_library_response` format.

### Search

```
GET /hubs/search?query=dark+side&sectionId={section_id}&limit=10
Accept: application/json
```

Response groups results into "Hubs" by type:
```json
{
  "MediaContainer": {
    "Hub": [
      {
        "type": "artist",
        "title": "Artists",
        "Metadata": [
          { "ratingKey": "12345", "title": "Pink Floyd", "type": "artist" }
        ]
      },
      {
        "type": "album",
        "title": "Albums",
        "Metadata": [
          { "ratingKey": "12346", "title": "The Dark Side of the Moon", "type": "album" }
        ]
      },
      {
        "type": "track",
        "title": "Tracks",
        "Metadata": [
          {
            "ratingKey": "12347",
            "title": "Time",
            "type": "track",
            "Media": [{ "Part": [{ "key": "/library/parts/..." }] }]
          }
        ]
      }
    ]
  }
}
```

Track results include `Media`/`Part` data for direct stream URL construction.

Library-specific search alternative:
```
GET /library/sections/{section_id}/search?type=10&query=time
```

Genre filtering:
```
GET /library/sections/{section_id}/all?type=10&genre=rock
```

Reference: [plexapi.dev Search API](https://plexapi.dev/api-reference/search/perform-a-search)

### Audio Streaming (Direct Play)

The `Part.key` from track metadata gives a direct HTTP URL to the raw audio file:

```
http://{host}:{port}{Part.key}?X-Plex-Token={token}
```

Example:
```
http://192.168.1.100:32400/library/parts/9877/1234567890/file.flac?X-Plex-Token=abc123
```

This returns the **original file bytes** with no transcoding. The response is a standard HTTP download with support for:
- `Content-Length` header (total file size)
- `Accept-Ranges: bytes` (HTTP range requests for seeking)
- `Content-Type` reflecting the actual file format

On a local LAN, even FLAC files stream instantly. **Direct Play is the only mode we need** — the daemon already has FLAC, MP3, and OGG decoders.

There is no need for Plex's transcode/HLS endpoints. Those exist for remote streaming over the internet where bandwidth is limited. On LAN, direct play is simpler, faster, and higher quality.

**IMPORTANT:** The full URL (with token) is constructed server-side at download time only — see "Token Separation" in Security Considerations.

Reference: [Streaming: Direct Play and Direct Stream](https://support.plex.tv/articles/200250387-streaming-media-direct-play-and-direct-stream/)

### Playback Reporting (Optional)

Plex tracks "On Deck", play counts, and last-played timestamps via timeline updates from clients.

**Timeline update** (call every ~10s during playback and on state changes):

```
POST /:/timeline?ratingKey={id}&state={playing|paused|stopped}&time={ms}&duration={ms}
X-Plex-Client-Identifier: <uuid>
X-Plex-Token: <token>
```

**Scrobble** (mark as played, call when track finishes or > 90% played):

```
GET /:/scrobble?key={ratingKey}&identifier=com.plexapp.plugins.library
X-Plex-Token: <token>
```

For MVP, start with scrobble only. Add timeline updates later for proper "Now Playing" on the Plex dashboard.

**Threading:** Scrobble and timeline updates must NOT run on the streaming thread (would introduce jitter in the decode/encode loop). Signal "position changed" via an atomic variable; a separate timer or the main thread fires the HTTP POST.

Reference: [plexapi.dev Timeline API](https://plexapi.dev/api-reference/timeline/report-media-timeline)

---

## Architecture

### Token Separation (Critical Design Decision)

**The Plex token MUST NOT appear in the `path` field of queue entries or search results.**

The `path` field in `music_queue_entry_t` and `music_search_result_t` is broadcast to every WebSocket client via `webui_music_send_state()` (line 355 of `webui_music.c`), sent in search/library JSON responses, sent to satellite devices, and logged at `LOG_INFO` level (line 838). If the token were embedded in the URL, it would leak to:
- Every authenticated WebUI user (including non-admins)
- All DAP2 satellite devices
- Log files (typically 0644)
- Any future log aggregation

**Solution:** Store only the `Part.key` (e.g., `/library/parts/9877/1234567890/file.flac`) in the `path` field. The full URL with token is constructed server-side inside `plex_client_get_stream_url()` at the moment of HTTP download only. Clients never see the token.

For the `path` field to be distinguishable from a local file path, use a `plex:` prefix:
```
plex:/library/parts/9877/1234567890/file.flac
```

This prefix is checked in `webui_music_start_playback()` to route to the Plex download path, and in `webui_music_is_path_valid()` for URL-specific validation.

### Source Abstraction

```
┌──────────────────────────────────────────────────────────┐
│                   webui_music.c                           │
│          (playback engine, Opus encode, broadcast)        │
│                                                           │
│   webui_music_start_playback(state, path)                │
│          │                                                │
│    ┌─────┴─────┐                                         │
│    │ local     │  plex:// prefix?                        │
│    │ file path │  → plex_client_download_track()         │
│    │           │    (constructs full URL with token,     │
│    │           │     downloads to temp file)              │
│    ▼           ▼                                         │
│   audio_decoder_open(local_path_or_temp_path)            │
│          │                                                │
│          ▼                                                │
│  flac_decoder / mp3_decoder / ogg_decoder                │
│          │                                                │
│          ▼                                                │
│  resampler → opus_encoder → WebSocket broadcast          │
└──────────────────────────────────────────────────────────┘
```

**Key difference from original design:** The decoder layer (`audio_decoder.c`) is never modified. It always receives a local file path. HTTP download is handled in `webui_music_start_playback()` before the decoder is invoked. This avoids the extension-detection bug where `get_extension()` on a URL like `file.flac?X-Plex-Token=abc` returns the wrong extension (the `?` query string confuses `strrchr(path, '.')`).

### Library / Search / Browse Routing

```
┌─────────────────────────────────────────────────────────┐
│             handle_music_library()                        │
│             handle_music_search()                         │
│             handle_music_control("play", query)           │
│                      │                                    │
│              ┌───────┴───────┐                           │
│              │ source check  │                           │
│              └───┬───────┬───┘                           │
│          local   │       │  plex                         │
│                  ▼       ▼                                │
│           music_db_*()  plex_client_*()                  │
│           (SQLite)      (HTTP→JSON)                      │
│                  │       │                                │
│                  ▼       ▼                                │
│         Same JSON response format to WebUI/satellite     │
└─────────────────────────────────────────────────────────┘
```

The routing happens inside `webui_music.c` handlers. Both backends produce the same `music_library_response` / `music_search_response` JSON payloads, so the WebUI and satellite UI code require zero changes.

Keep the routing logic in `webui_music.c` thin — delegate immediately to `plex_client_*()` functions that return pre-formatted JSON objects. This avoids bloating `webui_music.c` (already 2,507 lines, at the CLAUDE.md warning threshold).

### Path Validation

The existing `webui_music_is_path_valid()` (line 193 of `webui_music.c`) calls `realpath()` and verifies the resolved path is within `g_config.paths.music_dir`. This will reject all Plex paths.

**Updated validation logic:**

```c
bool webui_music_is_path_valid(const char *path) {
   if (!path || path[0] == '\0') return false;

   /* Plex paths: validate prefix and host match */
   if (strncmp(path, "plex:", 5) == 0) {
      if (strcmp(g_config.music.source, "plex") != 0) return false;
      const char *part_key = path + 5;
      /* Validate Part.key starts with expected Plex API path */
      if (strncmp(part_key, "/library/parts/", 15) != 0 &&
          strncmp(part_key, "/library/metadata/", 18) != 0) return false;
      /* No path traversal in the Part.key */
      if (contains_path_traversal(part_key)) return false;
      return true;
   }

   /* Local paths: existing realpath() validation (unchanged) */
   // ... existing code ...
}
```

This prevents:
- SSRF: only `plex:` prefix paths with known Plex API prefixes are accepted
- Path traversal: `contains_path_traversal()` check on the Part.key
- Source mismatch: Plex paths rejected when `source != "plex"`

### Metadata Handling

The existing `audio_decoder_get_metadata()` opens a file independently to extract ID3/Vorbis tags. For Plex tracks, metadata comes from the API response, not embedded tags. Code paths that call `audio_decoder_get_metadata()` on a Plex path will fail.

**Solution:** For Plex tracks, metadata (title, artist, album, duration) is populated from the Plex API response at search/browse time and stored in the `music_queue_entry_t` struct. The `audio_decoder_get_metadata()` call is skipped when the path has a `plex:` prefix. The fallback path in `handle_music_control()` (line 1144: `music_db_get_by_path()`) is also skipped for Plex paths since the metadata is already in the queue entry.

### HTTP Download for Playback (Temp File Approach)

The download step lives in `webui_music_start_playback()`, NOT in `audio_decoder_open()`:

```c
int webui_music_start_playback(session_music_state_t *state, const char *path) {
   char local_path[PATH_MAX];

   if (strncmp(path, "plex:", 5) == 0) {
      /* Download Plex track to temp file */
      const char *part_key = path + 5;
      if (plex_client_download_track(part_key, local_path, sizeof(local_path)) != 0) {
         return 1; /* Download failed */
      }
      /* Store temp path for cleanup */
      safe_strncpy(state->temp_file, local_path, sizeof(state->temp_file));
   } else {
      safe_strncpy(local_path, path, sizeof(local_path));
   }

   /* Existing decoder open logic — always receives a local file path */
   audio_decoder_t *dec = audio_decoder_open(local_path);
   // ...
}
```

**Temp file management:**
- Use `mkstemps()` for atomic creation with the correct extension (`.flac`, `.mp3`, `.ogg`)
- Set permissions via `fchmod(fd, 0600)` immediately on the open file descriptor
- `unlink()` the temp file immediately after the decoder opens its fd (Unix trick — file stays accessible via fd until closed, but is automatically cleaned up if daemon crashes)
- Add startup cleanup sweep: `glob("/tmp/dawn_plex_*", ...)` and remove any orphaned files

**Download size limit:** Set `CURLOPT_MAXFILESIZE_LARGE` to 500MB (covers even 24-bit/96kHz FLAC). Abort early on `Content-Length` before downloading. Log a warning if a file exceeds 100MB.

**Disk considerations (Jetson):** Verify `/tmp` is tmpfs (RAM-backed) via `statfs()` at startup and warn if it is not. A 50MB temp file on tmpfs is 0.6% of 8GB RAM — acceptable.

---

## Implementation Plan

### Buffer Size Reconciliation

Before any implementation, reconcile the path buffer sizes:

| Struct | Field | Current Size | Issue |
|--------|-------|-------------|-------|
| `music_search_result_t` | `path` | 1024 (`MUSIC_DB_PATH_MAX`) | OK for Plex `plex:` + Part.key |
| `music_queue_entry_t` | `path` | 512 (`WEBUI_MUSIC_PATH_MAX`) | Truncation risk on copy from search result |

A `plex:` path is typically ~60-80 chars (e.g., `plex:/library/parts/9877/1234567890/file.flac`), well within 512. But to eliminate the existing size mismatch (which could truncate long local paths too), increase `WEBUI_MUSIC_PATH_MAX` to 1024 to match `MUSIC_DB_PATH_MAX`.

**Memory impact:** `music_queue_entry_t` increases from ~900 to ~1400 bytes. With 100-entry queues, per-session memory goes from ~88KB to ~137KB. Acceptable on 8GB Jetson.

### New Files

| File | Purpose | Est. Lines |
|------|---------|------------|
| `src/audio/plex_client.c` | Plex REST API client (auth, browse, search, stream URL, scrobble) | ~600-800 |
| `include/audio/plex_client.h` | Public API for Plex client | ~80 |
| `src/audio/http_download.c` | libcurl download-to-temp-file utility | ~150 |
| `include/audio/http_download.h` | Public API for HTTP download | ~30 |

### Modified Files

| File | Change |
|------|--------|
| `src/webui/webui_music.c` | Route library/search/browse to Plex or local based on `source` config; add Plex download step to `start_playback()`; update `is_path_valid()` for `plex:` prefix; skip `audio_decoder_get_metadata()` for Plex paths |
| `include/webui/webui_music.h` | Increase `WEBUI_MUSIC_PATH_MAX` to 1024; add `temp_file` field to `session_music_state_t` |
| `include/config/dawn_config.h` | Add `plex_config_t` to `music_config_t`; add `plex_token` to `secrets_config_t` |
| `src/config/config_parser.c` | Parse `[music]` source + `[music.plex]` section |
| `src/config/config_defaults.c` | Default values for Plex config |
| `src/config/config_env.c` | JSON serialization + TOML write for Plex config; secrets JSON status for plex_token |
| `src/webui/webui_config.c` | Add plex_token to `handle_set_secrets()`; add `[music.plex]` to `apply_config_from_json()` |
| `www/js/ui/settings/schema.js` | Add Music Source dropdown + Plex fields with `showWhen` conditional visibility |
| `www/js/ui/settings.js` | Add Plex Token row to Secrets panel HTML |
| `www/js/ui/settings/config.js` | Add plex_token to secrets save/load flow |
| `www/js/ui/music.js` | Add source indicator badge; add persistent error banner for Plex failures |

### Phase 1: Plex Client + Config (MVP)

1. Increase `WEBUI_MUSIC_PATH_MAX` to 1024
2. Add `plex_config_t` struct and `plex_token` to `secrets_config_t`
3. Add config parsing, defaults, TOML write, JSON serialization
4. Implement `plex_client.c`:
   - `plex_client_init()` — create two persistent CURL handles (one for API queries, one for downloads) with `CURLOPT_TCP_KEEPALIVE` and HTTP keep-alive. Reuse the existing `curl_buffer_t` from `curl_buffer.h` for API responses with `curl_buffer_init_with_max(buf, CURL_BUFFER_MAX_PLEX_API)`.
   - `plex_client_discover_section()` — `GET /library/sections`, find music section
   - `plex_client_list_artists(page, limit)` — paginated artist list
   - `plex_client_list_albums(artist_id, page, limit)` — albums for artist
   - `plex_client_list_tracks(album_id)` — tracks for album
   - `plex_client_search(query, limit)` — hub search
   - `plex_client_get_stream_url(part_key)` — construct full URL with token (used only for download, never stored)
   - `plex_client_download_track(part_key, out_path, out_path_size)` — download via HTTP to temp file
   - `plex_client_scrobble(rating_key)` — mark track as played
   - `plex_client_cleanup()` — free CURL handles
5. Implement `http_download.c`:
   - `http_download_to_temp(url, headers, out_path, out_path_size, max_size)` — download via libcurl to `/tmp/dawn_plex_XXXXXX.ext` using `mkstemps()`, `fchmod(0600)`, `unlink()` after decoder open
   - Set `CURLOPT_MAXFILESIZE_LARGE` to cap download size
   - Set `CURLOPT_CONNECTTIMEOUT = 10`, `CURLOPT_LOW_SPEED_LIMIT = 1024`, `CURLOPT_LOW_SPEED_TIME = 30`
   - Startup cleanup: `glob("/tmp/dawn_plex_*", ...)` to remove orphans
6. Add `plex_token` to Secrets panel (HTML + JS)

### Phase 2: Wire Into Music Pipeline

7. Update `webui_music_is_path_valid()` for `plex:` prefix validation
8. Update `webui_music_start_playback()` to detect `plex:` prefix and download to temp file
9. Route `handle_music_library()` / `handle_music_search()` in `webui_music.c` through source switch
10. Map Plex JSON responses to existing `music_library_response` / `music_search_response` formats:
    - Artists: `{ name, track_count }` — use `Directory[].leafCount` and `Directory[].childCount`
    - Albums: `{ name, artist, year, track_count }` — all available from Plex
    - Tracks: `{ title, artist, album, duration_sec, path }` — `path` is `plex:{Part.key}` (no token)
11. Add `showWhen` conditional visibility to schema system
12. Add Plex connection fields to WebUI settings panel
13. Add source indicator badge to music player panel
14. Add persistent error banner for Plex auth/connection failures
15. Add confirmation dialog on source switch during active playback

### Phase 3: Polish

16. GDM server discovery — "Discover" button next to Plex Server field; results presented for user confirmation; run in detached thread with 3-second timeout
17. Scrobble reporting on track completion (from main thread, not streaming thread)
18. Timeline updates during playback (for Plex "Now Playing" dashboard)
19. Album art thumbnails — add `thumbnail` field to track JSON response (Plex: `/photo/:/transcode` URL; local: `null` for now). Daemon proxies art to WebUI (browser should not need direct Plex access).
20. "Test Connection" button in settings — sends `test_plex_connection` WebSocket message, daemon calls `GET /library/sections`, returns server name or error
21. Token refresh / expiry handling with user notification
22. Pre-fetch next track during current playback (last 10% triggers download of `queue[index+1]` — eliminates inter-track silence on slower networks)
23. Plex playlist browsing (list playlists, get playlist tracks)

---

## Response Format Mapping

The WebUI and satellites expect specific JSON formats. Here's how Plex data maps to them.

### Artists (`browse_type: "artists"`)

Current format:
```json
{
  "type": "music_library_response",
  "payload": {
    "browse_type": "artists",
    "artists": [
      { "name": "Pink Floyd", "album_count": 15, "track_count": 165 }
    ],
    "total": 150,
    "offset": 0,
    "limit": 50
  }
}
```

Plex source:
- `name` ← `Directory[].title`
- `album_count` / `track_count` — not directly available in artist listing. Options:
  - Use `Directory[].leafCount` (total tracks) and `Directory[].childCount` (albums) if available
  - Or omit counts (set to 0) and populate on drill-down

### Albums (`browse_type: "albums"`)

Current format:
```json
{
  "payload": {
    "browse_type": "albums",
    "albums": [
      { "name": "The Dark Side of the Moon", "artist": "Pink Floyd", "year": 1973, "track_count": 10 }
    ]
  }
}
```

Plex source:
- `name` ← `Directory[].title`
- `artist` ← `Directory[].parentTitle`
- `year` ← `Directory[].year`
- `track_count` ← `Directory[].leafCount`

### Tracks

Current format:
```json
{
  "payload": {
    "results": [
      {
        "title": "Time",
        "artist": "Pink Floyd",
        "album": "The Dark Side of the Moon",
        "duration_sec": 413,
        "path": "/home/user/Music/Pink Floyd/DSOTM/04 - Time.flac"
      }
    ]
  }
}
```

Plex source:
- `title` ← `Metadata[].title`
- `artist` ← `Metadata[].grandparentTitle`
- `album` ← `Metadata[].parentTitle`
- `duration_sec` ← `Metadata[].duration / 1000` (Plex uses milliseconds)
- `path` ← `plex:{Part.key}` (e.g., `plex:/library/parts/9877/1234567890/file.flac`)

**The `path` field does NOT contain the token.** The full stream URL is constructed server-side only at download time in `plex_client_download_track()`.

### Search

Current `music_search_response` already returns tracks in the same format as above. Plex's `/hubs/search` returns tracks with full `Media`/`Part` data, so mapping is straightforward.

---

## Dependencies

| Library | Purpose | Status |
|---------|---------|--------|
| **libcurl** | HTTP client for all Plex API calls + file download | Already used by LLM providers (`llm_openai.c`, `llm_claude.c`) |
| **json-c** | JSON parsing for Plex API responses | Already used throughout daemon |
| **POSIX sockets** | GDM UDP discovery | Standard, no additional library |

**No new dependencies required.** The Plex API is pure REST/JSON over HTTP.

---

## Security Considerations

### 1. Token Separation (HIGH)

The Plex token MUST NOT appear in `path` fields, JSON messages to clients, or log output. Store only `plex:{Part.key}` in path fields. Construct the full URL with token server-side at download time only. See "Token Separation" in Architecture section.

### 2. Token Storage (HIGH)

Store in `secrets_config_t.plex_token` (loaded from `secrets.toml`, mode 0600). Add to `secrets_to_json_status()` as is_set flag only. Add to `handle_set_secrets()` for admin-only updates. Never include in `config_to_json()` or `get_config_response`.

### 3. Path Validation (HIGH)

`webui_music_is_path_valid()` must be updated to handle `plex:` prefix paths. Validate:
- Scheme is exactly `plex:` (not `http://`, not arbitrary URLs)
- Part.key starts with `/library/parts/` or `/library/metadata/`
- No `@` character (prevents authority injection)
- No path traversal sequences (`../`)
- Source config is actually `"plex"` (don't accept plex paths when source is local)

### 4. URL Construction Safety

When constructing the full download URL in `plex_client_get_stream_url()`:
- Validate the Part.key starts with `/library/` before appending to host
- Use `CURLOPT_FOLLOWLOCATION = 0` (do not follow redirects — prevents redirect to attacker-controlled host)
- Validate total URL length fits in buffer before construction (use `snprintf` return value check)

### 5. SSL/TLS

When `ssl = true`:
- Default to full certificate verification (`CURLOPT_SSL_VERIFYPEER = 1`)
- Add `ssl_verify = false` as a separate config option with a log warning when disabled
- For self-signed Plex certs: document how to export and add to DAWN's existing private CA trust store (same infrastructure used for satellite TLS)
- Never silently disable verification

### 6. GDM Discovery

- Never auto-apply discovered servers — always present to user for confirmation
- Validate discovered `Port:` is integer 1-65535
- Parse response headers with `snprintf`/`strncpy` and explicit size limits
- After user selects, validate via `GET /identity` with the token

### 7. Temp File Security

- Create with `mkstemps()` (atomic, suffix-aware)
- Set `fchmod(fd, 0600)` immediately
- `unlink()` after decoder opens its fd (file persists until fd closed)
- Startup orphan cleanup sweep
- `CURLOPT_MAXFILESIZE_LARGE` prevents disk exhaustion from malicious server

### 8. Log Sanitization

The existing `LOG_INFO("WebUI music: Playing %s", path)` at line 838 of `webui_music.c` would previously only log local file paths. With the `plex:` prefix approach, the logged path is just `plex:/library/parts/...` — no token. No code changes needed.

---

## Agent Review Findings (Incorporated)

This design was reviewed by all four DAWN review agents (architecture, security, efficiency, UI). All findings have been incorporated into the sections above. Key changes from the original design:

| Finding | Source Agents | Resolution |
|---------|---------------|------------|
| Token in path leaks to all WebSocket clients | Security, Architecture, Efficiency | `plex:` prefix with server-side URL construction |
| `webui_music_is_path_valid()` rejects URLs | Architecture, Security | Updated validation for `plex:` prefix with whitelist |
| `WEBUI_MUSIC_PATH_MAX` (512) < `MUSIC_DB_PATH_MAX` (1024) | Architecture, Security | Increase to 1024 |
| Token in `dawn.toml` config struct | Architecture, Security, UI | Move to `secrets_config_t` |
| `audio_decoder_open()` extension detection fails on URLs | Architecture | Download in `start_playback()`, not in decoder |
| Schema has no conditional field visibility | UI | Implement `showWhen` mechanism |
| Schema has no `password` field type for token | UI | Route through existing Secrets panel |
| No error state UX for Plex failures | UI, Architecture | Persistent error banner + `music_state.error` field |
| Temp file cleanup on crash | Efficiency, Security | `unlink()` after open + startup sweep |
| No download size limit | Efficiency, Security | `CURLOPT_MAXFILESIZE_LARGE` (500MB) |
| No CURL handle reuse | Efficiency | Two persistent handles (API + download) with keep-alive |
| GDM discovery spoofable | Security | User confirmation required, never auto-apply |
| `CURLOPT_SSL_VERIFYPEER = 0` defeats TLS | Security | Separate `ssl_verify` toggle, reuse private CA |
| No source switch confirmation | UI | Confirm dialog when playback active |
| No source indicator in music player | UI | Source badge in `music_state` |
| `webui_music.c` already 2,507 lines | Architecture | Keep routing thin, delegate to `plex_client_*()` |
| `audio_decoder_get_metadata()` fails on URLs | Architecture | Skip for Plex paths, use API metadata |
| Large library JSON spikes memory | Efficiency | Mandate pagination, cap API buffer at 2MB |
| Blocking download on track transition | Efficiency | Phase 3: pre-fetch next track |
| No connection/low-speed timeouts | Efficiency | CURL timeout settings specified |
| Scrobble/timeline on streaming thread adds jitter | Efficiency | Fire from main thread via atomic signal |
| Album art needs `thumbnail` field | UI | Add to track JSON (Phase 3) |
| GDM implementation guards | Efficiency | Thread, timeout, iteration cap, `fromlen` reset |

---

## Reference Links

- [Plex API Documentation (plexapi.dev)](https://plexapi.dev/)
- [Finding an Authentication Token](https://support.plex.tv/articles/204059436-finding-an-authentication-token-x-plex-token/)
- [Plex Media Server URL Commands](https://support.plex.tv/articles/201638786-plex-media-server-url-commands/)
- [Streaming: Direct Play and Direct Stream](https://support.plex.tv/articles/200250387-streaming-media-direct-play-and-direct-stream/)
- [Authentication for Local Network Access](https://support.plex.tv/articles/200890058-authentication-for-local-network-access/)
- [Network Ports / Firewall (Port 32400 default, 32414 GDM)](https://support.plex.tv/articles/201543147-what-network-ports-do-i-need-to-allow-through-my-firewall/)
- [GDM Discovery — python-plexapi](https://python-plexapi.readthedocs.io/en/latest/modules/gdm.html)
- [plexapi.dev Search API](https://plexapi.dev/api-reference/search/perform-a-search)
- [plexapi.dev Timeline API](https://plexapi.dev/api-reference/timeline/report-media-timeline)
- [python-plexapi audio.py (GitHub)](https://github.com/pkkid/python-plexapi/blob/master/plexapi/audio.py)
- [Plex Pro Week '25: API Unlocked](https://www.plex.tv/blog/plex-pro-week-25-api-unlocked/)
- [Plex Playlist API (Plexopedia)](https://www.plexopedia.com/plex-media-server/api/playlists/view-items/)

---

## Source Upgrade Path

This implementation uses **Option A: Either/Or** — the user selects either "local" or "plex" as their music source via the settings panel. All library/search/browse operations go to the selected source exclusively.

**Planned upgrade path:**

1. **Option A → Option C (Switchable with Mixed Queue):** Allow the queue to contain tracks from both sources simultaneously. A local track and a Plex track can coexist in the same playlist. The `plex:` prefix on paths already distinguishes the two at playback time, so the queue and decoder routing require minimal changes. The source selector becomes a "default for search/browse" rather than an exclusive gate.

2. **Option C → Option B (Joined Library):** Merge local and Plex libraries into a unified view. Search queries fan out to both backends in parallel, results are interleaved by relevance. Browse shows combined artist/album listings with a source badge. This requires deduplication logic (same album from both sources) and is the most complex upgrade.

Each step builds incrementally on the previous one with no architectural rewrites.
