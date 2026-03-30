#!/bin/bash
#
# DAWN Installation - Phase 9: Optional Features
# SearXNG (Docker), FlareSolverr (Docker), MQTT (Mosquitto)
#
# Sourced by install.sh. Do not execute directly.
#

# ─────────────────────────────────────────────────────────────────────────────
# SearXNG (privacy-focused web search)
# ─────────────────────────────────────────────────────────────────────────────

install_searxng() {
   if ! echo "${FEATURES:-}" | grep -q "searxng"; then
      return 0
   fi
   if [ "$HAS_DOCKER_COMPOSE" = false ]; then
      warn "SearXNG: Docker Compose not available, skipping"
      return 0
   fi

   local searx_dir="$HOME/docker/searxng"

   # Check if already running
   if docker compose -f "$searx_dir/docker-compose.yml" ps 2>/dev/null | grep -q "searxng"; then
      log "SearXNG: already running"
      return 0
   fi

   log "Setting up SearXNG..."
   mkdir -p "$searx_dir/searxng"

   local secret_key
   secret_key=$(openssl rand -hex 32)

   cat >"$searx_dir/docker-compose.yml" <<'COMPOSE_EOF'
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
COMPOSE_EOF

   cat >"$searx_dir/searxng/settings.yml" <<SETTINGS_EOF
use_default_settings: true

general:
  instance_name: "DAWN Search"
  debug: false

server:
  secret_key: "$secret_key"
  bind_address: "0.0.0.0"
  port: 8080
  method: "GET"
  image_proxy: false
  limiter: false
  public_instance: false

search:
  safe_search: 1
  default_lang: "en"
  autocomplete: ""
  formats:
    - json
  max_page: 3

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
  - name: bing news
    disabled: false
    weight: 2
  - name: google news
    disabled: false
    weight: 2
SETTINGS_EOF

   cd "$searx_dir" || return
   docker compose up -d || error "Failed to start SearXNG"

   # Wait and verify
   local tries=0
   while [ $tries -lt 10 ]; do
      if curl -sf "http://localhost:8384/search?q=test&format=json" >/dev/null 2>&1; then
         log "SearXNG: running and verified (http://localhost:8384)"
         cd "$PROJECT_ROOT" || return
         return 0
      fi
      sleep 2
      ((tries++))
   done

   warn "SearXNG: started but health check failed (may need a moment to initialize)"
   cd "$PROJECT_ROOT" || return
}

# ─────────────────────────────────────────────────────────────────────────────
# FlareSolverr (JavaScript-heavy site proxy)
# ─────────────────────────────────────────────────────────────────────────────

install_flaresolverr() {
   if ! echo "${FEATURES:-}" | grep -q "flaresolverr"; then
      return 0
   fi
   if [ "$HAS_DOCKER" = false ]; then
      warn "FlareSolverr: Docker not available, skipping"
      return 0
   fi

   # Check if already running
   if docker ps --filter name=flaresolverr --format '{{.Names}}' 2>/dev/null | grep -q flaresolverr; then
      log "FlareSolverr: already running"
      return 0
   fi

   log "Starting FlareSolverr..."
   docker run -d --name flaresolverr \
      --restart unless-stopped \
      -p 8191:8191 \
      ghcr.io/flaresolverr/flaresolverr:latest || error "Failed to start FlareSolverr"

   # Wait and verify
   sleep 5
   if curl -sf http://localhost:8191/health >/dev/null 2>&1; then
      log "FlareSolverr: running and verified (http://localhost:8191)"
   else
      warn "FlareSolverr: started but health check failed"
   fi

   # Enable in dawn.toml
   sed_safe_set "$PROJECT_ROOT/dawn.toml" "url_fetcher.flaresolverr" "enabled" "true"
}

# ─────────────────────────────────────────────────────────────────────────────
# MQTT (Mosquitto broker)
# ─────────────────────────────────────────────────────────────────────────────

start_mqtt() {
   if ! echo "${FEATURES:-}" | grep -q "mqtt"; then
      return 0
   fi

   if ! is_pkg_installed mosquitto; then
      warn "MQTT: mosquitto not installed, skipping"
      return 0
   fi

   if systemctl is-active --quiet mosquitto 2>/dev/null; then
      log "MQTT: mosquitto already running"
      return 0
   fi

   log "Starting Mosquitto MQTT broker..."
   sudo_begin_phase "mqtt"
   run_sudo systemctl enable mosquitto || warn "Failed to enable mosquitto"
   run_sudo systemctl start mosquitto || warn "Failed to start mosquitto"

   if systemctl is-active --quiet mosquitto 2>/dev/null; then
      log "MQTT: mosquitto started and enabled"
   else
      warn "MQTT: mosquitto failed to start"
   fi
}

# ─────────────────────────────────────────────────────────────────────────────
# Phase 9 entry point
# ─────────────────────────────────────────────────────────────────────────────

run_services() {
   header "Phase 9: Optional Features"
   CURRENT_PHASE="services"
   install_searxng
   install_flaresolverr
   start_mqtt
   log "Phase 9 complete"
}
