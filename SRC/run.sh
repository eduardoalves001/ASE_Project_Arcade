#!/usr/bin/env bash
#
# run.sh - one-command launcher for the ESP-Arcade dashboard (Windows / Git Bash).
#
# What it does, in order:
#   1. Locates openssl, mosquitto and node with Windows fallbacks, so it never
#      dies with "openssl: command not found" the way the .ps1 did.
#   2. (Re)generates the local TLS certificate bound to this PC's IP, reusing the
#      existing CA so the firmware never has to be re-flashed just for that.
#   3. Writes the Mosquitto config + password file and the dashboard/firmware
#      MQTT settings.
#   4. Installs the dashboard's Node dependencies if they are missing.
#   5. Starts the Mosquitto TLS broker in the background and the dashboard in the
#      foreground, then opens http://localhost:3000 in the browser.
#
# Usage (from Git Bash - right-click the repo folder > "Git Bash Here"):
#   ./run.sh                 # auto-detect Wi-Fi IP, full launch
#   ./run.sh --ip 10.0.0.5   # force a specific broker IP (what the ESP32 uses)
#   ./run.sh --no-broker     # just the web dashboard, do not start Mosquitto
#   ./run.sh --setup-only    # regenerate certs/config only, launch nothing
#
set -Eeuo pipefail

# Stop Git's MSYS layer from rewriting the openssl -subj argument and paths.
export MSYS_NO_PATHCONV=1
export MSYS2_ARG_CONV_EXCL='*'

# ---------- locations ----------
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CERT_DIR="$ROOT/.local/mqtt-certs"
DASH_DIR="$ROOT/arcade-dashboard"
mkdir -p "$CERT_DIR"

USERNAME="arcade"
PASSWORD="arcade-local-2026"
DAYS_CA=3650
DAYS_SRV=825

# ---------- options ----------
FORCE_IP=""
START_BROKER=1
LAUNCH=1
while [ $# -gt 0 ]; do
  case "$1" in
    --ip)         FORCE_IP="${2:-}"; shift 2 ;;
    --no-broker)  START_BROKER=0; shift ;;
    --setup-only) LAUNCH=0; shift ;;
    -h|--help)    sed -n '2,20p' "$0"; exit 0 ;;
    *) echo "Unknown option: $1" >&2; exit 2 ;;
  esac
done

log()  { printf '\033[36m%s\033[0m\n' "$*"; }
warn() { printf '\033[33m%s\033[0m\n' "$*" >&2; }

# ---------- resolve tools (with Windows fallbacks) ----------
# Prefer the msys2 openssl (handles POSIX /c/... paths). The mingw openssl on
# PATH cannot open absolute Git-Bash paths, which is the kind of breakage the
# old .ps1 hit from a different angle.
OPENSSL=""
for _cand in "/usr/bin/openssl.exe" "/c/Program Files/Git/usr/bin/openssl.exe" "$(command -v openssl || true)"; do
  if [ -n "$_cand" ] && [ -x "$_cand" ]; then OPENSSL="$_cand"; break; fi
done

MOSQ_DIR="/c/Program Files/mosquitto"
MOSQ="$MOSQ_DIR/mosquitto.exe"
MOSQ_PASSWD="$MOSQ_DIR/mosquitto_passwd.exe"

NODE="$(command -v node || true)"
NPM="$(command -v npm || true)"
if [ -z "$NPM" ] && [ -x "/c/Program Files/nodejs/npm" ]; then
  NPM="/c/Program Files/nodejs/npm"
fi
if [ -z "$NODE" ]; then
  echo "Node.js not found. Install it from https://nodejs.org and re-run." >&2
  exit 1
fi

# ---------- detect broker IP ----------
detect_wifi_ip() {
  powershell.exe -NoProfile -Command \
    "(Get-NetIPAddress -AddressFamily IPv4 | Where-Object { \$_.InterfaceAlias -like 'Wi-Fi*' -and \$_.IPAddress -notlike '169.254.*' } | Select-Object -First 1).IPAddress" \
    2>/dev/null | tr -d '\r\n '
}
all_ipv4() {
  powershell.exe -NoProfile -Command \
    "Get-NetIPAddress -AddressFamily IPv4 | Where-Object { \$_.IPAddress -notlike '169.254.*' -and \$_.IPAddress -ne '192.168.56.1' } | ForEach-Object { \$_.IPAddress }" \
    2>/dev/null | tr -d '\r'
}

IP="$FORCE_IP"
[ -z "$IP" ] && IP="$(detect_wifi_ip || true)"
[ -z "$IP" ] && IP="$(all_ipv4 | head -n1 || true)"
if [ -z "$IP" ]; then
  IP="127.0.0.1"
  warn "No network IP found - using 127.0.0.1 (dashboard only, the ESP32 will not reach it)."
fi
log "Broker IP for the ESP32: $IP"

# ---------- certificate paths ----------
CA_KEY="$CERT_DIR/arcade-ca.key";      CA_CRT="$CERT_DIR/arcade-ca.crt"
SRV_KEY="$CERT_DIR/arcade-server.key"; SRV_CSR="$CERT_DIR/arcade-server.csr"
SRV_CRT="$CERT_DIR/arcade-server.crt"; SRV_CNF="$CERT_DIR/arcade-server.cnf"
PASSWD="$CERT_DIR/arcade.passwd"
CONF="$CERT_DIR/mosquitto-arcade.conf"

if [ -n "$OPENSSL" ]; then
  # Run every openssl command with bare filenames from inside the cert dir, so
  # it never matters whether the resolved openssl understands POSIX or Windows
  # paths (and spaces in the path stop being an issue).
  (
    cd "$CERT_DIR"

    if [ ! -f arcade-ca.crt ] || [ ! -f arcade-ca.key ]; then
      log "Creating local CA..."
      "$OPENSSL" req -x509 -newkey rsa:2048 -nodes -keyout arcade-ca.key -out arcade-ca.crt \
        -days "$DAYS_CA" -subj "/CN=ESP-Arcade Local CA" >/dev/null 2>&1
    else
      log "Reusing existing CA."
    fi

    # SAN = chosen IP + 127.0.0.1 + every local IPv4, de-duplicated. Including
    # 127.0.0.1 lets the dashboard validate the broker over localhost regardless
    # of which Wi-Fi address the PC currently has.
    {
      echo "[req]"
      echo "distinguished_name = dn"
      echo "req_extensions = v3_req"
      echo "prompt = no"
      echo "[dn]"
      echo "CN = $IP"
      echo "[v3_req]"
      echo "subjectAltName = @alt_names"
      echo "[alt_names]"
      echo "DNS.1 = localhost"
      n=1; seen=" "
      for ipa in "$IP" "127.0.0.1" $(all_ipv4); do
        case "$seen" in *" $ipa "*) continue ;; esac
        echo "IP.$n = $ipa"; seen="$seen$ipa "; n=$((n + 1))
      done
    } > arcade-server.cnf

    log "Generating server certificate (SAN bound to $IP)..."
    "$OPENSSL" req -new -newkey rsa:2048 -nodes -keyout arcade-server.key -out arcade-server.csr \
      -config arcade-server.cnf >/dev/null 2>&1
    "$OPENSSL" x509 -req -in arcade-server.csr -CA arcade-ca.crt -CAkey arcade-ca.key -CAcreateserial \
      -out arcade-server.crt -days "$DAYS_SRV" -extensions v3_req -extfile arcade-server.cnf >/dev/null 2>&1
  )
elif [ -f "$CA_CRT" ] && [ -f "$SRV_CRT" ]; then
  warn "openssl not found - reusing the certificates already in $CERT_DIR."
else
  warn "openssl not found and no certificates present - skipping the TLS broker."
  START_BROKER=0
fi

# ---------- password file ----------
if [ -x "$MOSQ_PASSWD" ]; then
  "$MOSQ_PASSWD" -c -b "$(cygpath -w "$PASSWD")" "$USERNAME" "$PASSWORD" >/dev/null 2>&1 \
    || warn "mosquitto_passwd failed (continuing)."
fi

# ---------- mosquitto config (needs native Windows paths) ----------
if [ -f "$SRV_CRT" ]; then
  cat > "$CONF" <<EOF
# ESP-Arcade local TLS broker (generated by run.sh)
per_listener_settings true

listener 8883
cafile $(cygpath -w "$CA_CRT")
certfile $(cygpath -w "$SRV_CRT")
keyfile $(cygpath -w "$SRV_KEY")
require_certificate false
allow_anonymous false
password_file $(cygpath -w "$PASSWD")
EOF
fi

# ---------- wire the CA + settings into firmware and dashboard ----------
[ -f "$CA_CRT" ] && cp "$CA_CRT" "$ROOT/main/mqtt_broker_ca.pem"

cat > "$ROOT/main/generated_mqtt_config.h" <<EOF
#ifndef GENERATED_MQTT_CONFIG_H
#define GENERATED_MQTT_CONFIG_H

/* Generated by run.sh. Re-run it (and rebuild+flash the firmware) when the PC IP changes. */
#define ARCADE_MQTT_BROKER_IP   "$IP"
#define ARCADE_MQTT_BROKER_PORT 8883
#define ARCADE_MQTT_BROKER_URI  "mqtts://$IP:8883"
#define ARCADE_MQTT_USERNAME    "$USERNAME"
#define ARCADE_MQTT_PASSWORD    "$PASSWORD"

#endif
EOF

# The dashboard talks to its own local broker, so it never depends on the Wi-Fi IP.
cat > "$DASH_DIR/mqtt-config.json" <<EOF
{
  "brokerUrl": "mqtts://127.0.0.1:8883",
  "caFile": "../main/mqtt_broker_ca.pem",
  "username": "$USERNAME",
  "password": "$PASSWORD"
}
EOF

# ---------- node dependencies ----------
if [ ! -d "$DASH_DIR/node_modules" ]; then
  log "Installing dashboard dependencies (npm install)..."
  ( cd "$DASH_DIR" && "$NPM" install )
fi

if [ "$LAUNCH" = "0" ]; then
  log "Setup complete (--setup-only). Certificates and config are ready."
  exit 0
fi

# ---------- start the broker in the background ----------
MOSQ_PID=""
NODE_PID=""
cleanup() {
  [ -n "$NODE_PID" ] && kill "$NODE_PID" >/dev/null 2>&1 || true
  [ -n "$MOSQ_PID" ] && kill "$MOSQ_PID" >/dev/null 2>&1 || true
}
trap cleanup EXIT INT TERM

if [ "$START_BROKER" = "1" ] && [ -x "$MOSQ" ] && [ -f "$CONF" ]; then
  log "Starting Mosquitto TLS broker on :8883..."
  "$MOSQ" -c "$(cygpath -w "$CONF")" >/dev/null 2>&1 &
  MOSQ_PID=$!
  sleep 1
  if ! kill -0 "$MOSQ_PID" >/dev/null 2>&1; then
    warn "Broker did not stay up (is port 8883 already taken by a Mosquitto service?). Continuing anyway - the dashboard keeps retrying."
    MOSQ_PID=""
  fi
elif [ "$START_BROKER" = "1" ]; then
  warn "Mosquitto not found - starting the dashboard without a broker (no live data)."
fi

# ---------- run the dashboard, then open the browser once it is actually up ----------
log "Starting the dashboard..."
cd "$DASH_DIR"
"$NODE" server.js &
NODE_PID=$!

# Wait until port 3000 accepts connections before opening the browser, otherwise
# the browser races the server and shows "connection refused" / "ligacao nao
# estabelecida". Use 127.0.0.1 (server binds IPv4; localhost may resolve to ::1).
for _i in $(seq 1 40); do
  if curl -s -o /dev/null --max-time 1 http://127.0.0.1:3000/ 2>/dev/null; then break; fi
  kill -0 "$NODE_PID" 2>/dev/null || break   # dashboard exited - stop waiting
  sleep 0.3
done

log "Opening http://127.0.0.1:3000 ..."
powershell.exe -NoProfile -Command "Start-Process 'http://127.0.0.1:3000'" >/dev/null 2>&1 || true

log "Dashboard running at http://127.0.0.1:3000  -  press Ctrl+C to stop."
wait "$NODE_PID"
