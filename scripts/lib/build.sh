#!/bin/bash
#
# DAWN Installation - Phase 3: Clone and Build
# Git submodules, WebRTC AEC, CMake configure, compile
#
# Sourced by install.sh. Do not execute directly.
#

# ─────────────────────────────────────────────────────────────────────────────
# Git submodules
# ─────────────────────────────────────────────────────────────────────────────

ensure_submodules() {
   if [ "$HAS_SUBMODULES" = true ]; then
      log "Git submodules: already initialized"
      return 0
   fi

   log "Initializing git submodules..."
   cd "$PROJECT_ROOT" || return
   git submodule update --init --recursive || error "Failed to initialize submodules"
   HAS_SUBMODULES=true
   log "Git submodules: initialized"
}

# ─────────────────────────────────────────────────────────────────────────────
# WebRTC AEC (echo cancellation)
# ─────────────────────────────────────────────────────────────────────────────

build_webrtc_aec() {
   # Skip for server presets (AEC disabled)
   case "${BUILD_PRESET:-default}" in
      server | server-debug)
         log "WebRTC AEC: skipped (server mode)"
         return 0
         ;;
   esac

   local webrtc_dir="$PROJECT_ROOT/webrtc-audio-processing"
   local aec_lib="$webrtc_dir/build/webrtc/modules/audio_processing/libwebrtc-audio-processing-1.a"

   if [ -f "$aec_lib" ]; then
      log "WebRTC AEC: already built"
      return 0
   fi

   if [ ! -d "$webrtc_dir" ]; then
      warn "WebRTC submodule directory not found. AEC can be disabled with -DENABLE_AEC=OFF"
      return 0
   fi

   log "Building WebRTC audio processing (static library)..."
   cd "$webrtc_dir" || return

   # Find meson — prefer pip-installed version if it exists
   local meson_cmd="meson"
   if [ -f "$HOME/.local/bin/meson" ]; then
      meson_cmd="$HOME/.local/bin/meson"
   fi

   # Setup or reconfigure
   if [ -d build ]; then
      "$meson_cmd" setup build --default-library=static --wipe 2>/dev/null ||
         "$meson_cmd" setup build --default-library=static
   else
      "$meson_cmd" setup build --default-library=static ||
         error "WebRTC meson setup failed"
   fi

   ninja -C build || error "WebRTC ninja build failed"

   cd "$PROJECT_ROOT" || return
   log "WebRTC AEC: built successfully"
}

# ─────────────────────────────────────────────────────────────────────────────
# Build DAWN
# ─────────────────────────────────────────────────────────────────────────────

build_dawn() {
   local preset="${BUILD_PRESET:-default}"

   log "Building DAWN with preset '$preset'..."
   cd "$PROJECT_ROOT" || return

   # Determine build directory from preset
   case "$preset" in
      default) BUILD_DIR="$PROJECT_ROOT/build" ;;
      local) BUILD_DIR="$PROJECT_ROOT/build-local" ;;
      full) BUILD_DIR="$PROJECT_ROOT/build-full" ;;
      debug) BUILD_DIR="$PROJECT_ROOT/build-debug" ;;
      server) BUILD_DIR="$PROJECT_ROOT/build-server" ;;
      server-debug) BUILD_DIR="$PROJECT_ROOT/build-server-debug" ;;
      *) error "Unknown build preset: $preset" ;;
   esac

   # Configure
   cmake --preset "$preset" || error "CMake configure failed for preset '$preset'"

   # Build
   cmake --build --preset "$preset" -- -j"$(nproc)" || error "Build failed for preset '$preset'"

   # Verify binary
   if [ ! -f "$BUILD_DIR/dawn" ]; then
      error "Build completed but binary not found at $BUILD_DIR/dawn"
   fi

   local binary_size
   binary_size=$(du -sh "$BUILD_DIR/dawn" | cut -f1)
   log "Binary built: $BUILD_DIR/dawn ($binary_size)"

   # Check dawn-admin
   if [ -f "$BUILD_DIR/dawn-admin/dawn-admin" ]; then
      log "dawn-admin built: $BUILD_DIR/dawn-admin/dawn-admin"
   else
      warn "dawn-admin not found in build output"
   fi

   # Quick sanity check — try --dump-config (may fail without config file, that's ok)
   if run_dawn "$BUILD_DIR/dawn" --dump-config >/dev/null 2>&1; then
      log "Binary verified: --dump-config succeeded"
   else
      info "Binary built (--dump-config skipped, may need config file)"
   fi

   log "Phase 3 complete — build successful"
}

# ─────────────────────────────────────────────────────────────────────────────
# Phase 3 entry point
# ─────────────────────────────────────────────────────────────────────────────

run_build() {
   header "Phase 3: Clone and Build"
   CURRENT_PHASE="build"
   ensure_submodules
   build_webrtc_aec
   build_dawn
}
