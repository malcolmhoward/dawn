#!/bin/bash
#
# Generate SSL certificates for DAWN using a private Certificate Authority.
#
# This creates a two-tier PKI:
#   1. CA certificate (ssl/ca.crt + ssl/ca.key) — trust anchor for all clients
#   2. Server certificate (ssl/dawn.crt + ssl/dawn.key) — signed by the CA
#
# The CA is only generated once. Server certs can be renewed as needed.
# A chain file (ssl/dawn-chain.crt) is also created for the daemon.
#
# Usage:
#   ./generate_ssl_cert.sh            # Generate CA (if needed) + server cert
#   ./generate_ssl_cert.sh --renew    # Regenerate server cert only (reuse CA)
#   ./generate_ssl_cert.sh --check    # Show cert expiry and SAN info
#   ./generate_ssl_cert.sh --force-ca # Regenerate CA (invalidates all clients!)
#   ./generate_ssl_cert.sh --san IP:203.0.113.50 --san DNS:dawn.example.com
#                                     # Add extra SANs (external IP, domain, etc.)
#
# After running, configure dawn.toml:
#   [webui]
#   https = true
#   ssl_cert_path = "ssl/dawn-chain.crt"
#   ssl_key_path = "ssl/dawn.key"
#

set -e

OUTPUT_DIR="ssl"
ARDUINO_DIR="dawn_satellite_arduino"

CA_KEY="$OUTPUT_DIR/ca.key"
CA_CERT="$OUTPUT_DIR/ca.crt"
SERVER_KEY="$OUTPUT_DIR/dawn.key"
SERVER_CERT="$OUTPUT_DIR/dawn.crt"
SERVER_CHAIN="$OUTPUT_DIR/dawn-chain.crt"
CA_CERT_HEADER="$ARDUINO_DIR/ca_cert.h"

# Parse arguments
FORCE_CA=0
RENEW=0
CHECK=0
EXTRA_SANS=""

while [ $# -gt 0 ]; do
   case "$1" in
      --force-ca)
         FORCE_CA=1
         ;;
      --renew)
         RENEW=1
         ;;
      --check)
         CHECK=1
         ;;
      --san)
         if [ -z "$2" ]; then
            echo "Error: --san requires a value (e.g., --san IP:203.0.113.50 or --san DNS:dawn.example.com)"
            exit 1
         fi
         EXTRA_SANS="$EXTRA_SANS,$2"
         shift
         ;;
      --help|-h)
         echo "Usage: $0 [--renew] [--check] [--force-ca] [--san SAN ...]"
         echo ""
         echo "  (no flags)   Generate CA (if needed) + server certificate"
         echo "  --renew      Regenerate server cert only (reuses existing CA)"
         echo "  --check      Show certificate expiry dates and SANs"
         echo "  --force-ca   Regenerate the CA (WARNING: invalidates all clients!)"
         echo "  --san SAN    Add extra Subject Alternative Name (repeatable)"
         echo "               Examples: --san IP:203.0.113.50  --san DNS:dawn.example.com"
         echo ""
         exit 0
         ;;
      *)
         echo "Unknown option: $1"
         echo "Run '$0 --help' for usage."
         exit 1
         ;;
   esac
   shift
done

# --check: display certificate info and exit
if [ "$CHECK" -eq 1 ]; then
   echo "============================================"
   echo "  DAWN Certificate Status"
   echo "============================================"
   echo ""

   if [ -f "$CA_CERT" ]; then
      echo "CA Certificate ($CA_CERT):"
      expiry=$(openssl x509 -in "$CA_CERT" -noout -enddate 2>/dev/null | cut -d= -f2)
      subject=$(openssl x509 -in "$CA_CERT" -noout -subject 2>/dev/null | sed 's/subject=/  /')
      echo "  Subject: $subject"
      echo "  Expires: $expiry"

      # Warn if expiring within 30 days
      if openssl x509 -in "$CA_CERT" -noout -checkend 2592000 >/dev/null 2>&1; then
         echo "  Status:  OK"
      else
         echo "  Status:  WARNING — expires within 30 days!"
      fi
   else
      echo "CA Certificate: NOT FOUND"
   fi

   echo ""

   if [ -f "$SERVER_CERT" ]; then
      echo "Server Certificate ($SERVER_CERT):"
      expiry=$(openssl x509 -in "$SERVER_CERT" -noout -enddate 2>/dev/null | cut -d= -f2)
      subject=$(openssl x509 -in "$SERVER_CERT" -noout -subject 2>/dev/null | sed 's/subject=/  /')
      echo "  Subject: $subject"
      echo "  Expires: $expiry"

      # Show SANs
      sans=$(openssl x509 -in "$SERVER_CERT" -noout -ext subjectAltName 2>/dev/null | tail -1 | sed 's/^ */  /')
      if [ -n "$sans" ]; then
         echo "  SANs:    $sans"
      fi

      # Warn if expiring within 30 days
      if openssl x509 -in "$SERVER_CERT" -noout -checkend 2592000 >/dev/null 2>&1; then
         echo "  Status:  OK"
      else
         echo "  Status:  WARNING — expires within 30 days! Run: $0 --renew"
      fi
   else
      echo "Server Certificate: NOT FOUND"
   fi

   echo ""
   exit 0
fi

# Create output directory
mkdir -p "$OUTPUT_DIR"

# Get hostname and IP addresses for SANs
HOSTNAME=$(hostname)
IP_ADDRS=$(hostname -I 2>/dev/null | tr ' ' '\n' | grep -v '^$' || echo "")

# Build Subject Alternative Names
SAN="DNS:localhost,DNS:$HOSTNAME"
for ip in $IP_ADDRS; do
   SAN="$SAN,IP:$ip"
done
SAN="$SAN,IP:127.0.0.1"

# Append any extra SANs from --san flags
if [ -n "$EXTRA_SANS" ]; then
   SAN="$SAN$EXTRA_SANS"
fi

echo "============================================"
echo "  DAWN SSL Certificate Generator"
echo "============================================"
echo ""
echo "Hostname: $HOSTNAME"
echo "IP addresses: $IP_ADDRS"
if [ -n "$EXTRA_SANS" ]; then
   echo "Extra SANs: ${EXTRA_SANS#,}"
fi
echo ""

# =============================================================================
# Step 1: CA Certificate
# =============================================================================

NEED_CA=0
if [ "$FORCE_CA" -eq 1 ]; then
   echo "WARNING: --force-ca will regenerate the CA certificate."
   echo "All existing client trust (ESP32 firmware, RPi satellites, browsers)"
   echo "will need to be updated with the new CA certificate."
   echo ""
   read -r -p "Are you sure? (yes/no): " confirm
   if [ "$confirm" != "yes" ]; then
      echo "Aborted."
      exit 1
   fi
   NEED_CA=1
elif [ ! -f "$CA_KEY" ] || [ ! -f "$CA_CERT" ]; then
   NEED_CA=1
fi

if [ "$NEED_CA" -eq 1 ] && [ "$RENEW" -eq 0 ]; then
   echo "--- Generating CA Certificate (RSA-4096, 10 years) ---"
   echo ""
   echo "You will be prompted for a passphrase to protect the CA private key."
   echo "This passphrase is needed when signing new server certificates."
   echo ""

   openssl genrsa -aes256 -out "$CA_KEY" 4096

   openssl req -new -x509 \
      -key "$CA_KEY" \
      -out "$CA_CERT" \
      -days 3650 \
      -subj "/CN=DAWN Private CA/O=DAWN"

   chmod 0400 "$CA_KEY"
   chmod 0644 "$CA_CERT"

   echo ""
   echo "CA certificate created."
   echo ""
   echo "  IMPORTANT: Protect $CA_KEY — anyone with this key can"
   echo "  issue certificates trusted by your DAWN network."
   echo ""
elif [ "$RENEW" -eq 1 ] && { [ ! -f "$CA_KEY" ] || [ ! -f "$CA_CERT" ]; }; then
   echo "ERROR: Cannot renew — no CA certificate found."
   echo "Run without --renew first to generate the CA."
   exit 1
else
   echo "CA certificate already exists — reusing."
   echo ""
fi

# =============================================================================
# Step 2: Server Certificate (EC P-256, signed by CA)
# =============================================================================

echo "--- Generating Server Certificate (EC P-256, 1 year) ---"
echo ""
echo "SANs: $SAN"
echo ""

# Generate EC P-256 private key
openssl ecparam -genkey -name prime256v1 -noout -out "$SERVER_KEY" 2>/dev/null

# Create CSR
openssl req -new \
   -key "$SERVER_KEY" \
   -out "$OUTPUT_DIR/dawn.csr" \
   -subj "/CN=$HOSTNAME/O=DAWN" \
   2>/dev/null

# Create extensions file for SANs
cat > "$OUTPUT_DIR/dawn.ext" << EOF
authorityKeyIdentifier=keyid,issuer
basicConstraints=CA:FALSE
keyUsage=digitalSignature,keyEncipherment
extendedKeyUsage=serverAuth
subjectAltName=$SAN
EOF

# Sign with CA
echo "Signing server certificate with CA (enter CA passphrase)..."
openssl x509 -req \
   -in "$OUTPUT_DIR/dawn.csr" \
   -CA "$CA_CERT" \
   -CAkey "$CA_KEY" \
   -CAcreateserial \
   -out "$SERVER_CERT" \
   -days 365 \
   -extfile "$OUTPUT_DIR/dawn.ext"

# Create chain file (server cert + CA cert)
cat "$SERVER_CERT" "$CA_CERT" > "$SERVER_CHAIN"

# Clean up temporary files
rm -f "$OUTPUT_DIR/dawn.csr" "$OUTPUT_DIR/dawn.ext" "$OUTPUT_DIR/ca.srl"

chmod 0600 "$SERVER_KEY"
chmod 0644 "$SERVER_CERT" "$SERVER_CHAIN"

echo ""
echo "Server certificate created and signed by CA."
echo ""

# =============================================================================
# Step 3: Generate Arduino CA cert header
# =============================================================================

if [ -d "$ARDUINO_DIR" ]; then
   echo "--- Generating Arduino CA header ($CA_CERT_HEADER) ---"

   CA_PEM=$(cat "$CA_CERT")

   cat > "$CA_CERT_HEADER" << HEADER_EOF
/* Auto-generated by generate_ssl_cert.sh — do not edit manually */
#ifndef CA_CERT_H
#define CA_CERT_H

static const char CA_CERT_PEM[] PROGMEM = R"EOF(
${CA_PEM}
)EOF";

#endif /* CA_CERT_H */
HEADER_EOF

   echo "Arduino CA header written."
   echo ""
fi

# =============================================================================
# Summary
# =============================================================================

echo "============================================"
echo "  Files Created"
echo "============================================"
echo ""
echo "  CA certificate:     $CA_CERT  (distribute to clients)"
echo "  CA private key:     $CA_KEY   (keep secret!)"
echo "  Server certificate: $SERVER_CERT"
echo "  Server private key: $SERVER_KEY"
echo "  Server chain:       $SERVER_CHAIN  (use in dawn.toml)"
if [ -d "$ARDUINO_DIR" ]; then
echo "  Arduino CA header:  $CA_CERT_HEADER"
fi
echo ""
echo "============================================"
echo "  Daemon Configuration"
echo "============================================"
echo ""
echo "Add to your dawn.toml:"
echo ""
echo "[webui]"
echo "https = true"
echo "ssl_cert_path = \"$SERVER_CHAIN\""
echo "ssl_key_path = \"$SERVER_KEY\""
echo ""
echo "============================================"
echo "  Client Setup"
echo "============================================"
echo ""
echo "1. ESP32: Reflash with updated firmware (ca_cert.h was auto-generated)"
echo ""
echo "2. RPi Satellite: Copy $CA_CERT to the satellite and set in config:"
echo "   ca_cert_path = \"/path/to/ca.crt\""
echo "   ssl_verify = true"
echo ""
echo "3. Browser: Install $CA_CERT in your OS trust store:"
echo "   Linux:   sudo cp $CA_CERT /usr/local/share/ca-certificates/dawn-ca.crt && sudo update-ca-certificates"
echo "   macOS:   sudo security add-trusted-cert -d -r trustRoot -k /Library/Keychains/System.keychain $CA_CERT"
echo "   Windows: certutil -addstore -f \"ROOT\" $CA_CERT"
echo ""
echo "After installing the CA, access https://<dawn-ip>:3000 with no warnings."
echo ""
