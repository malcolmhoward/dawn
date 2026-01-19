#!/bin/bash
#
# Llama.cpp Server Installation Script
# This script installs the llama-server as a systemd service with optimal DAWN settings
#

set -e

# Default configuration variables
MODEL_PATH=""
TEMPLATE_PATH=""
PORT="8080"
HOST="127.0.0.1"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SERVICE_USER="llama"
DATA_DIR="/var/lib/llama-cpp"
CONFIG_DIR="/usr/local/etc/llama-cpp"

# Optimal DAWN defaults (if files not provided)
DEFAULT_MODEL="$DATA_DIR/models/Qwen3-4B-Instruct-2507-Q4_K_M.gguf"
# Use chatml template for Instruct models, official template for Thinking models
DEFAULT_TEMPLATE="$DATA_DIR/templates/qwen3_chatml.jinja"

# Colors for output
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
RED='\033[0;31m'
NC='\033[0m' # No Color

# Helper functions
log() {
    echo -e "${GREEN}[INFO]${NC} $1"
}

warn() {
    echo -e "${YELLOW}[WARNING]${NC} $1"
}

error() {
    echo -e "${RED}[ERROR]${NC} $1"
    exit 1
}

# Parse command line arguments
parse_args() {
    while [[ $# -gt 0 ]]; do
        case $1 in
            -m|--model)
                MODEL_PATH="$2"
                shift 2
                ;;
            -t|--template)
                TEMPLATE_PATH="$2"
                shift 2
                ;;
            -p|--port)
                PORT="$2"
                shift 2
                ;;
            --host)
                HOST="$2"
                shift 2
                ;;
            -h|--help)
                echo "Usage: $0 [options]"
                echo ""
                echo "Options:"
                echo "  -m, --model PATH       Path to model file (optional, uses default if not provided)"
                echo "  -t, --template PATH    Path to template file (optional, uses default if not provided)"
                echo "  -p, --port PORT        Server port (default: 8080)"
                echo "  --host HOST            Server host address (default: 127.0.0.1)"
                echo "  -h, --help             Show this help message"
                echo ""
                echo "Default files (if not provided):"
                echo "  Model:    $DEFAULT_MODEL"
                echo "  Template: $DEFAULT_TEMPLATE (chatml for Instruct models)"
                echo ""
                echo "Bundled templates:"
                echo "  qwen3_chatml.jinja    - For Instruct models (recommended)"
                echo "  qwen3_official.jinja  - For Thinking/reasoning models"
                echo ""
                echo "Note: If model/template not provided, script assumes they are already in place."
                echo "      Download model: wget https://huggingface.co/Qwen/Qwen3-4B-Instruct-GGUF/resolve/main/Qwen3-4B-Instruct-2507-Q4_K_M.gguf"
                exit 0
                ;;
            *)
                error "Unknown option: $1"
                ;;
        esac
    done
}

# Check if running as root
if [ "$(id -u)" -ne 0 ]; then
    error "This script must be run as root"
fi

# Parse command line arguments
parse_args "$@"

# Check if llama-server is installed
if ! command -v llama-server &> /dev/null; then
    error "llama-server not found. Please install llama.cpp first."
fi

# Create directory structure first
log "Creating directory structure"
mkdir -p "$DATA_DIR/models"
mkdir -p "$DATA_DIR/templates"
mkdir -p "$DATA_DIR/run"
mkdir -p "$CONFIG_DIR"
mkdir -p "/var/log/llama-cpp"

# Handle model file
if [ -z "$MODEL_PATH" ]; then
    warn "No model path provided. Checking for default model..."
    if [ -f "$DEFAULT_MODEL" ]; then
        log "Found existing model: $DEFAULT_MODEL"
        MODEL_FILENAME=$(basename "$DEFAULT_MODEL")
    else
        error "Default model not found: $DEFAULT_MODEL

Please either:
  1. Download the optimal model:
     wget -P $DATA_DIR/models https://huggingface.co/Qwen/Qwen3-4B-Instruct-GGUF/resolve/main/Qwen3-4B-Instruct-2507-Q4_K_M.gguf

  2. Or provide model path with: $0 -m /path/to/model.gguf"
    fi
else
    # Verify model file exists
    if [ ! -f "$MODEL_PATH" ]; then
        error "Model file not found: $MODEL_PATH"
    fi

    log "Copying model file"
    MODEL_FILENAME=$(basename "$MODEL_PATH")
    cp "$MODEL_PATH" "$DATA_DIR/models/$MODEL_FILENAME"
fi

# Handle template file
if [ -z "$TEMPLATE_PATH" ]; then
    warn "No template path provided. Checking for default template..."
    if [ -f "$DEFAULT_TEMPLATE" ]; then
        log "Found existing template: $DEFAULT_TEMPLATE"
        TEMPLATE_FILENAME=$(basename "$DEFAULT_TEMPLATE")
    else
        warn "Default template not found. Installing bundled template..."
        if [ -f "$SCRIPT_DIR/qwen3_chatml.jinja" ]; then
            cp "$SCRIPT_DIR/qwen3_chatml.jinja" "$DATA_DIR/templates/"
            TEMPLATE_FILENAME="qwen3_chatml.jinja"
            log "Installed qwen3_chatml.jinja template (for Instruct models)"
        else
            error "Template file not found: $SCRIPT_DIR/qwen3_chatml.jinja

Please provide template path with: $0 -t /path/to/template.jinja"
        fi
    fi
else
    # Verify template file exists
    if [ ! -f "$TEMPLATE_PATH" ]; then
        error "Template file not found: $TEMPLATE_PATH"
    fi

    log "Copying template file"
    TEMPLATE_FILENAME=$(basename "$TEMPLATE_PATH")
    cp "$TEMPLATE_PATH" "$DATA_DIR/templates/$TEMPLATE_FILENAME"
fi

# Always install both templates so users can switch between models
if [ -f "$SCRIPT_DIR/qwen3_chatml.jinja" ]; then
    cp "$SCRIPT_DIR/qwen3_chatml.jinja" "$DATA_DIR/templates/"
    log "Installed qwen3_chatml.jinja template (for Instruct models)"
fi
if [ -f "$SCRIPT_DIR/qwen3_official.jinja" ]; then
    cp "$SCRIPT_DIR/qwen3_official.jinja" "$DATA_DIR/templates/"
    log "Installed qwen3_official.jinja template (for Thinking models)"
fi

# Create service user if it doesn't exist
if ! id -u "$SERVICE_USER" &>/dev/null; then
    log "Creating service user: $SERVICE_USER"
    useradd --system --no-create-home --shell /usr/sbin/nologin "$SERVICE_USER"

    # Add to required GPU access groups
    log "Adding $SERVICE_USER to GPU access groups"
    for group in video render; do
        if getent group $group >/dev/null; then
            usermod -a -G $group "$SERVICE_USER"
            log "Added $SERVICE_USER to $group group"
        fi
    done
else
    log "Service user $SERVICE_USER already exists"
    # Ensure GPU access groups
    for group in video render; do
        if getent group $group >/dev/null && ! groups "$SERVICE_USER" | grep -q "\b$group\b"; then
            usermod -a -G $group "$SERVICE_USER"
            log "Added $SERVICE_USER to $group group"
        fi
    done
fi

# Set permissions
log "Setting permissions"
chown -R "$SERVICE_USER:$SERVICE_USER" "$DATA_DIR"
chmod -R 755 "$DATA_DIR"
chown "$SERVICE_USER:$SERVICE_USER" "/var/log/llama-cpp"
chmod 755 "/var/log/llama-cpp"

# Ensure library path is configured
log "Configuring library path"
if [ ! -f /etc/ld.so.conf.d/llama-cpp.conf ]; then
    echo "/usr/local/lib" > /etc/ld.so.conf.d/llama-cpp.conf
    ldconfig
    log "Added /usr/local/lib to library path"
fi

# Copy and configure config file
log "Installing configuration file"
cp "$SCRIPT_DIR/llama-server.conf" "$CONFIG_DIR/"

# Update the config file with the actual paths and settings
sed -i "s|MODEL=\".*\"|MODEL=\"$DATA_DIR/models/$MODEL_FILENAME\"|g" "$CONFIG_DIR/llama-server.conf"
sed -i "s|TEMPLATE=\".*\"|TEMPLATE=\"$DATA_DIR/templates/$TEMPLATE_FILENAME\"|g" "$CONFIG_DIR/llama-server.conf"
sed -i "s|PORT=.*|PORT=$PORT|g" "$CONFIG_DIR/llama-server.conf"
sed -i "s|HOST=.*|HOST=$HOST|g" "$CONFIG_DIR/llama-server.conf"
chmod 644 "$CONFIG_DIR/llama-server.conf"

log "Configuration:"
log "  Model: $DATA_DIR/models/$MODEL_FILENAME"
log "  Template: $DATA_DIR/templates/$TEMPLATE_FILENAME"
log "  Host: $HOST"
log "  Port: $PORT"

# Copy systemd service file
log "Installing systemd service file"
cp "$SCRIPT_DIR/llama-server.service" /etc/systemd/system/

# Install logrotate configuration
log "Installing logrotate configuration"
cp "$SCRIPT_DIR/llama-server" /etc/logrotate.d/
chmod 644 /etc/logrotate.d/llama-server

# Enable and start service
log "Enabling and starting service"
systemctl daemon-reload
systemctl enable llama-server.service
systemctl restart llama-server.service

# Check service status
sleep 2
if systemctl is-active --quiet llama-server.service; then
    log "✅ Service successfully started"
    log "Server is running at: http://$HOST:$PORT"
    log ""
    log "Management commands:"
    log "  Status:  systemctl status llama-server.service"
    log "  Logs:    journalctl -u llama-server.service -f"
    log "  Restart: systemctl restart llama-server.service"
    log "  Stop:    systemctl stop llama-server.service"
    log ""
    log "Test the server:"
    log "  curl http://$HOST:$PORT/health"
else
    warn "Service failed to start. Check logs with: journalctl -u llama-server.service -f"
    exit 1
fi
