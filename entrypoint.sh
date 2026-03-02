#!/bin/bash
# entrypoint.sh - DAWN container startup script
#
# Handles Whisper model download (lazy, on first run) and config setup.
#
# Key environment variables:
#   SKIP_MODEL_DOWNLOAD=true    Global default: skip all downloads (default: true)
#   SKIP_WHISPER=<inherits>     Skip Whisper download (inherits SKIP_MODEL_DOWNLOAD)
#   WHISPER_MODEL=base.en       Whisper model variant (default: base.en)
#
# Set SKIP_MODEL_DOWNLOAD=false to download missing models on first run.
# Models already present in the volume are never overwritten regardless of flags.
#
# See docs/DOCKER.md for full usage and ADR-0006 for design rationale.

set -e

DAWN_ROOT="${DAWN_ROOT:-/opt/dawn}"
WHISPER_MODELS_DIR="$DAWN_ROOT/whisper.cpp/models"
MODELS_SYMLINK="$DAWN_ROOT/models/whisper.cpp"

# Global default: skip all downloads. Set SKIP_MODEL_DOWNLOAD=false to enable.
SKIP_MODEL_DOWNLOAD="${SKIP_MODEL_DOWNLOAD:-true}"
# Per-model flags inherit from global unless explicitly overridden.
SKIP_WHISPER="${SKIP_WHISPER:-$SKIP_MODEL_DOWNLOAD}"

echo "[DAWN] Startup: SKIP_WHISPER=$SKIP_WHISPER"
echo "[DAWN] Models present in volume are used regardless of skip flags."

# Ensure Whisper models directory exists (submodule present, models/ may be empty)
mkdir -p "$WHISPER_MODELS_DIR"

# Ensure models/whisper.cpp symlink exists (DAWN resolves Whisper models through it)
if [ ! -e "$MODELS_SYMLINK" ]; then
    ln -s "../whisper.cpp/models" "$MODELS_SYMLINK"
    echo "[DAWN] Created symlink: models/whisper.cpp -> ../whisper.cpp/models"
fi

# Whisper model: download only if not present and download is enabled
WHISPER_MODEL="${WHISPER_MODEL:-base.en}"
WHISPER_MODEL_FILE="$WHISPER_MODELS_DIR/ggml-${WHISPER_MODEL}.bin"

if [ -f "$WHISPER_MODEL_FILE" ]; then
    echo "[DAWN] Whisper model present: ggml-${WHISPER_MODEL}.bin"
elif [ "$SKIP_WHISPER" = "false" ]; then
    echo "[DAWN] Downloading Whisper ${WHISPER_MODEL} model..."
    WHISPER_URL="https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-${WHISPER_MODEL}.bin"
    curl -L --progress-bar -o "$WHISPER_MODEL_FILE" "$WHISPER_URL"
    echo "[DAWN] Whisper model downloaded."
else
    echo "[DAWN] Skipping Whisper download (SKIP_WHISPER=$SKIP_WHISPER)."
    echo "       ASR will fail until a model is present at:"
    echo "       $WHISPER_MODEL_FILE"
    echo "       Set SKIP_MODEL_DOWNLOAD=false to download on next start, or"
    echo "       mount a model volume at $WHISPER_MODELS_DIR"
fi

# Copy default config if not present (allows container to start without a mounted config)
if [ ! -f "$DAWN_ROOT/dawn.toml" ]; then
    echo "[DAWN] No dawn.toml found. Copying from example (edit to customize)."
    cp "$DAWN_ROOT/dawn.toml.example" "$DAWN_ROOT/dawn.toml"
fi

exec "$@"
