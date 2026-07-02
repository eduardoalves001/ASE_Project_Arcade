#!/usr/bin/env bash
set -Eeuo pipefail

# setup_mosquitto_tls.sh
# Regenerates a Mosquitto TLS broker certificate for the PC's current LAN/AP IP,
# installs it into /etc/mosquitto/certs, writes a Mosquitto listener config,
# restarts the Mosquitto service, copies the CA certificate into the ESP-IDF
# project, and updates the ESP/dashboard MQTT settings for the detected IP.
#
# Usage from the repository root:
#   chmod +x scripts/setup_mosquitto_tls.sh
#   ./scripts/setup_mosquitto_tls.sh
#
# Optional:
#   ./scripts/setup_mosquitto_tls.sh --project-ca main/mqtt_broker_ca.pem
#   ./scripts/setup_mosquitto_tls.sh --ip 192.168.1.50
#   ./scripts/setup_mosquitto_tls.sh --interface wlan0
#   ./scripts/setup_mosquitto_tls.sh --with-plain

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

CERT_WORKDIR="${CERT_WORKDIR:-${PROJECT_ROOT}/.local/mqtt-certs}"
MOSQ_CERT_DIR="${MOSQ_CERT_DIR:-/etc/mosquitto/certs}"
MOSQ_MAIN_CONF_FILE="${MOSQ_MAIN_CONF_FILE:-/etc/mosquitto/mosquitto.conf}"
MOSQ_CONF_FILE="${MOSQ_CONF_FILE:-/etc/mosquitto/conf.d/arcade-local.conf}"
MOSQ_PASSWORD_FILE="${MOSQ_PASSWORD_FILE:-/etc/mosquitto/passwd/arcade.pw}"
PROJECT_CA_FILE="${PROJECT_CA_FILE:-${PROJECT_ROOT}/main/mqtt_broker_ca.pem}"
PROJECT_CONFIG_HEADER="${PROJECT_CONFIG_HEADER:-${PROJECT_ROOT}/main/generated_mqtt_config.h}"
DASHBOARD_CONFIG_FILE="${DASHBOARD_CONFIG_FILE:-${PROJECT_ROOT}/arcade-dashboard/mqtt-config.json}"
SERVICE_NAME="${SERVICE_NAME:-mosquitto}"
MQTT_USERNAME="${MQTT_USERNAME:-arcade}"
MQTT_PASSWORD="${MQTT_PASSWORD:-arcade-local-2026}"
BROKER_IP=""
IFACE=""
ENABLE_PLAIN="0"
DAYS_CA="3650"
DAYS_SERVER="825"

usage() {
  cat <<EOF
Usage: $0 [options]

Options:
  --ip <ip>              Use this broker IP instead of auto-detecting it.
  --interface <iface>    Auto-detect the IP from this network interface.
  --project-ca <path>    Where to copy the CA cert for the ESP-IDF firmware.
                         Default: ${PROJECT_CA_FILE}
  --project-config <path>
                         ESP-IDF generated MQTT config header.
                         Default: ${PROJECT_CONFIG_HEADER}
  --dashboard-config <path>
                         Node dashboard generated MQTT config JSON.
                         Default: ${DASHBOARD_CONFIG_FILE}
  --cert-workdir <path>  Temporary/private cert generation directory.
                         Default: ${CERT_WORKDIR}
  --with-plain          Also add a plain MQTT listener on port 1883.
  --no-plain            Do not add a plain listener; existing Mosquitto listeners
                         in the main config are left unchanged.
  -h, --help            Show this help.

Environment overrides:
  CERT_WORKDIR, MOSQ_CERT_DIR, MOSQ_MAIN_CONF_FILE, MOSQ_CONF_FILE,
  MOSQ_PASSWORD_FILE, PROJECT_CA_FILE, PROJECT_CONFIG_HEADER,
  DASHBOARD_CONFIG_FILE, SERVICE_NAME, MQTT_USERNAME, MQTT_PASSWORD
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --ip)
      BROKER_IP="${2:-}"
      shift 2
      ;;
    --interface)
      IFACE="${2:-}"
      shift 2
      ;;
    --project-ca)
      PROJECT_CA_FILE="${2:-}"
      shift 2
      ;;
    --project-config)
      PROJECT_CONFIG_HEADER="${2:-}"
      shift 2
      ;;
    --dashboard-config)
      DASHBOARD_CONFIG_FILE="${2:-}"
      shift 2
      ;;
    --cert-workdir)
      CERT_WORKDIR="${2:-}"
      shift 2
      ;;
    --with-plain)
      ENABLE_PLAIN="1"
      shift
      ;;
    --no-plain)
      ENABLE_PLAIN="0"
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown option: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
done

need_cmd() {
  if ! command -v "$1" >/dev/null 2>&1; then
    echo "Missing required command: $1" >&2
    exit 1
  fi
}

need_cmd openssl
need_cmd ip
need_cmd mosquitto_passwd
need_cmd sudo
need_cmd systemctl

is_ipv4() {
  [[ "$1" =~ ^([0-9]{1,3}\.){3}[0-9]{1,3}$ ]]
}

json_escape() {
  local value="$1"
  value="${value//\\/\\\\}"
  value="${value//\"/\\\"}"
  printf '%s' "$value"
}

detect_ip_from_iface() {
  local iface="$1"
  ip -4 addr show dev "$iface" scope global 2>/dev/null \
    | awk '/inet / {print $2}' \
    | cut -d/ -f1 \
    | head -n1
}

detect_default_ip() {
  local route_iface route_ip

  route_iface="$(ip route get 1.1.1.1 2>/dev/null | awk '{for (i=1; i<=NF; i++) if ($i == "dev") {print $(i+1); exit}}')"
  route_ip="$(ip route get 1.1.1.1 2>/dev/null | awk '{for (i=1; i<=NF; i++) if ($i == "src") {print $(i+1); exit}}')"

  if [[ -n "$route_ip" ]]; then
    echo "$route_ip"
    return 0
  fi

  if [[ -n "$route_iface" ]]; then
    detect_ip_from_iface "$route_iface"
    return 0
  fi

  ip -4 addr show scope global \
    | awk '/inet / {print $2}' \
    | cut -d/ -f1 \
    | grep -Ev '^(127\.|169\.254\.)' \
    | head -n1
}

if [[ -n "$IFACE" && -z "$BROKER_IP" ]]; then
  BROKER_IP="$(detect_ip_from_iface "$IFACE")"
fi

if [[ -z "$BROKER_IP" ]]; then
  BROKER_IP="$(detect_default_ip)"
fi

if [[ -z "$BROKER_IP" || ! "$BROKER_IP" =~ ^([0-9]{1,3}\.){3}[0-9]{1,3}$ ]]; then
  echo "Could not detect a valid IPv4 address." >&2
  echo "Run: $0 --ip <your-PC-IP-on-the-AP>" >&2
  exit 1
fi

# Validate octets are <= 255.
IFS='.' read -r o1 o2 o3 o4 <<< "$BROKER_IP"
for octet in "$o1" "$o2" "$o3" "$o4"; do
  if (( octet < 0 || octet > 255 )); then
    echo "Invalid IPv4 address: ${BROKER_IP}" >&2
    exit 1
  fi
done

mkdir -p "$CERT_WORKDIR"
chmod 700 "$CERT_WORKDIR"

CA_KEY="${CERT_WORKDIR}/virtualpet-ca.key"
CA_CRT="${CERT_WORKDIR}/virtualpet-ca.crt"
SERVER_KEY="${CERT_WORKDIR}/virtualpet-server.key"
SERVER_CSR="${CERT_WORKDIR}/virtualpet-server.csr"
SERVER_CRT="${CERT_WORKDIR}/virtualpet-server.crt"
SERVER_CNF="${CERT_WORKDIR}/virtualpet-server.cnf"

cat > "$SERVER_CNF" <<EOF
[req]
default_bits = 2048
prompt = no
default_md = sha256
distinguished_name = dn
req_extensions = v3_req

[dn]
CN = ${BROKER_IP}

[v3_req]
basicConstraints = CA:FALSE
keyUsage = digitalSignature, keyEncipherment
extendedKeyUsage = serverAuth
subjectAltName = @alt_names

[alt_names]
IP.1 = ${BROKER_IP}
EOF

# Reuse the same local CA if it already exists, so the ESP32 only needs the copied CA file
# to be rebuilt when you run this for the first time or intentionally delete the CA.
if [[ ! -f "$CA_KEY" || ! -f "$CA_CRT" ]]; then
  echo "Creating local CA in ${CERT_WORKDIR}"
  openssl genrsa -out "$CA_KEY" 4096 >/dev/null 2>&1
  chmod 600 "$CA_KEY"
  openssl req -x509 -new -nodes \
    -key "$CA_KEY" \
    -sha256 \
    -days "$DAYS_CA" \
    -out "$CA_CRT" \
    -subj "/CN=VirtualPet-MQTT-Local-CA" >/dev/null 2>&1
else
  echo "Reusing existing local CA: ${CA_CRT}"
fi

# Always regenerate the server cert because the AP may assign a different IP.
echo "Generating Mosquitto server certificate for IP ${BROKER_IP}"
openssl genrsa -out "$SERVER_KEY" 2048 >/dev/null 2>&1
chmod 600 "$SERVER_KEY"
openssl req -new -key "$SERVER_KEY" -out "$SERVER_CSR" -config "$SERVER_CNF" >/dev/null 2>&1
openssl x509 -req \
  -in "$SERVER_CSR" \
  -CA "$CA_CRT" \
  -CAkey "$CA_KEY" \
  -CAcreateserial \
  -out "$SERVER_CRT" \
  -days "$DAYS_SERVER" \
  -sha256 \
  -extensions v3_req \
  -extfile "$SERVER_CNF" >/dev/null 2>&1

sudo mkdir -p "$MOSQ_CERT_DIR"
sudo install -o mosquitto -g mosquitto -m 0644 "$CA_CRT" "${MOSQ_CERT_DIR}/virtualpet-ca.crt"
sudo install -o mosquitto -g mosquitto -m 0644 "$SERVER_CRT" "${MOSQ_CERT_DIR}/virtualpet-server.crt"
sudo install -o mosquitto -g mosquitto -m 0600 "$SERVER_KEY" "${MOSQ_CERT_DIR}/virtualpet-server.key"
sudo mkdir -p "$(dirname "$MOSQ_CONF_FILE")"
sudo mkdir -p "$(dirname "$MOSQ_PASSWORD_FILE")"

TMP_PASSWORD_FILE="$(mktemp)"
mosquitto_passwd -b -c "$TMP_PASSWORD_FILE" "$MQTT_USERNAME" "$MQTT_PASSWORD" >/dev/null
sudo install -o mosquitto -g mosquitto -m 0640 "$TMP_PASSWORD_FILE" "$MOSQ_PASSWORD_FILE"
rm -f "$TMP_PASSWORD_FILE"

TMP_CONF="$(mktemp)"
if [[ "$ENABLE_PLAIN" == "1" ]]; then
  cat > "$TMP_CONF" <<EOF
# Generated by ASE_virtualpet/scripts/setup_mosquitto_tls.sh
# Plain MQTT for local debugging only. Authentication is configured globally.
listener 1883

# Secure MQTT over TLS. Authentication is configured globally.
listener 8883
cafile ${MOSQ_CERT_DIR}/virtualpet-ca.crt
certfile ${MOSQ_CERT_DIR}/virtualpet-server.crt
keyfile ${MOSQ_CERT_DIR}/virtualpet-server.key
tls_version tlsv1.2
EOF
else
  cat > "$TMP_CONF" <<EOF
# Generated by ASE_virtualpet/scripts/setup_mosquitto_tls.sh
# Secure MQTT over TLS. Authentication is configured globally.
listener 8883
cafile ${MOSQ_CERT_DIR}/virtualpet-ca.crt
certfile ${MOSQ_CERT_DIR}/virtualpet-server.crt
keyfile ${MOSQ_CERT_DIR}/virtualpet-server.key
tls_version tlsv1.2
EOF
fi

sudo install -o root -g root -m 0644 "$TMP_CONF" "$MOSQ_CONF_FILE"
rm -f "$TMP_CONF"

MOSQ_CONF_DIR="$(dirname "$MOSQ_CONF_FILE")"
if ! sudo grep -Eq "^[[:space:]]*include_dir[[:space:]]+${MOSQ_CONF_DIR}([[:space:]]|$)" "$MOSQ_MAIN_CONF_FILE"; then
  TMP_MAIN_CONF="$(mktemp)"
  sudo cp "$MOSQ_MAIN_CONF_FILE" "$TMP_MAIN_CONF"
  {
    printf '\n# Added by ASE scripts/setup_mosquitto_tls.sh for the Virtual Pet TLS listener.\n'
    printf 'include_dir %s\n' "$MOSQ_CONF_DIR"
  } >> "$TMP_MAIN_CONF"
  sudo install -o root -g root -m 0644 "$TMP_MAIN_CONF" "$MOSQ_MAIN_CONF_FILE"
  rm -f "$TMP_MAIN_CONF"
fi

TMP_MAIN_CONF="$(mktemp)"
sudo awk -v password_file="$MOSQ_PASSWORD_FILE" '
  BEGIN { saw_password_file = 0; saw_allow = 0 }
  /^[[:space:]]*password_file[[:space:]]+/ {
    if (!saw_password_file) {
      print "password_file " password_file
      saw_password_file = 1
    }
    next
  }
  /^[[:space:]]*allow_anonymous[[:space:]]+/ {
    if (!saw_allow) {
      print "allow_anonymous false"
      saw_allow = 1
    }
    next
  }
  { print }
  END {
    if (!saw_password_file) print "password_file " password_file
    if (!saw_allow) print "allow_anonymous false"
  }
' "$MOSQ_MAIN_CONF_FILE" > "$TMP_MAIN_CONF"
sudo install -o root -g root -m 0644 "$TMP_MAIN_CONF" "$MOSQ_MAIN_CONF_FILE"
rm -f "$TMP_MAIN_CONF"

mkdir -p "$(dirname "$PROJECT_CA_FILE")"
cp "$CA_CRT" "$PROJECT_CA_FILE"
chmod 644 "$PROJECT_CA_FILE"

mkdir -p "$(dirname "$PROJECT_CONFIG_HEADER")"
cat > "$PROJECT_CONFIG_HEADER" <<EOF
#ifndef GENERATED_MQTT_CONFIG_H
#define GENERATED_MQTT_CONFIG_H

/*
 * Generated by scripts/setup_mosquitto_tls.sh.
 * Re-run that script whenever the laptop gets a new AP IP address.
 */
#define ARCADE_MQTT_BROKER_IP "${BROKER_IP}"
#define ARCADE_MQTT_BROKER_PORT 8883
#define ARCADE_MQTT_BROKER_URI "mqtts://${BROKER_IP}:8883"
#define ARCADE_MQTT_USERNAME "${MQTT_USERNAME}"
#define ARCADE_MQTT_PASSWORD "${MQTT_PASSWORD}"

#endif
EOF
chmod 644 "$PROJECT_CONFIG_HEADER"

mkdir -p "$(dirname "$DASHBOARD_CONFIG_FILE")"
cat > "$DASHBOARD_CONFIG_FILE" <<EOF
{
  "brokerUrl": "mqtts://${BROKER_IP}:8883",
  "caFile": "$(json_escape "$PROJECT_CA_FILE")",
  "username": "$(json_escape "$MQTT_USERNAME")",
  "password": "$(json_escape "$MQTT_PASSWORD")"
}
EOF
chmod 644 "$DASHBOARD_CONFIG_FILE"

sudo systemctl reset-failed "$SERVICE_NAME" >/dev/null 2>&1 || true
sudo systemctl restart "$SERVICE_NAME"

if ! systemctl is-active --quiet "$SERVICE_NAME"; then
  echo "Mosquitto did not start successfully. Recent logs:" >&2
  sudo journalctl -u "$SERVICE_NAME" -n 80 --no-pager >&2 || true
  exit 1
fi

cat <<EOF

Done.

Broker IP:       ${BROKER_IP}
TLS URI:         mqtts://${BROKER_IP}:8883
Project CA file: ${PROJECT_CA_FILE}
ESP config:      ${PROJECT_CONFIG_HEADER}
Dashboard config:${DASHBOARD_CONFIG_FILE}
Mosquitto conf:  ${MOSQ_CONF_FILE}
Main config:      ${MOSQ_MAIN_CONF_FILE}
Password file:    ${MOSQ_PASSWORD_FILE}
MQTT username:    ${MQTT_USERNAME}
CA for MQTTX:    ${CA_CRT}

Next ESP-IDF steps:
  idf.py build flash monitor

Run the dashboard:
  cd ${PROJECT_ROOT}/arcade-dashboard
  npm start

Test from this PC:
  mosquitto_sub -h ${BROKER_IP} -p 8883 --cafile ${CA_CRT} -u ${MQTT_USERNAME} -P '${MQTT_PASSWORD}' -t arcade/status -v
  mosquitto_pub -h ${BROKER_IP} -p 8883 --cafile ${CA_CRT} -u ${MQTT_USERNAME} -P '${MQTT_PASSWORD}' -t arcade/command -m start_snake

EOF
