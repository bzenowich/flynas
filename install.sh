#!/bin/sh
# FlyNAS installer — run on a fresh DragonFlyBSD system as root
# Usage: sh install.sh
set -e

FLYNAS_DIR="/usr/local/flynas"
FLYNAS_USER="flynas"
FLYNAS_GROUP="flynas"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
OVERLAY_DIR="${SCRIPT_DIR}/overlay"

# --- Helpers ---

step() {
    echo "==> $1"
}

# --- Checks ---

if [ "$(id -u)" -ne 0 ]; then
    echo "Error: must run as root" >&2
    exit 1
fi

if [ "$(uname -s)" != "DragonFly" ]; then
    echo "Error: this script is for DragonFlyBSD" >&2
    exit 1
fi

# --- Packages ---

step "Installing packages"
pkg install -y openresty git sqlite3 lua51-cjson

# --- User/Group ---

step "Creating flynas user and group"
if ! pw groupshow "$FLYNAS_GROUP" >/dev/null 2>&1; then
    pw groupadd "$FLYNAS_GROUP"
fi
if ! pw usershow "$FLYNAS_USER" >/dev/null 2>&1; then
    pw useradd "$FLYNAS_USER" -g "$FLYNAS_GROUP" -d /nonexistent -s /usr/sbin/nologin -c "FlyNAS service"
fi

# --- Deploy overlay ---

step "Deploying files"
cp -R "${OVERLAY_DIR}/" /

# --- Directory permissions ---

step "Setting permissions"
mkdir -p "${FLYNAS_DIR}/ssl"
mkdir -p "${FLYNAS_DIR}/logs"
mkdir -p /var/run/flynas
mkdir -p /var/log/flynas
chown -R "${FLYNAS_USER}:${FLYNAS_GROUP}" "$FLYNAS_DIR"
chown -R "${FLYNAS_USER}:${FLYNAS_GROUP}" /var/run/flynas
chown -R "${FLYNAS_USER}:${FLYNAS_GROUP}" /var/log/flynas

# --- SQLite database ---

step "Creating database"
DB_FILE="${FLYNAS_DIR}/flynas.db"
if [ ! -f "$DB_FILE" ]; then
    touch "$DB_FILE"
    chown "${FLYNAS_USER}:${FLYNAS_GROUP}" "$DB_FILE"
fi

# --- Self-signed SSL cert (if none exists) ---

step "Checking SSL certificate"
if [ ! -f "${FLYNAS_DIR}/ssl/cert.pem" ]; then
    step "Generating self-signed SSL certificate"
    openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 \
        -keyout "${FLYNAS_DIR}/ssl/key.pem" \
        -out "${FLYNAS_DIR}/ssl/cert.pem" \
        -days 3650 -nodes \
        -subj "/CN=flynas"
    chown "${FLYNAS_USER}:${FLYNAS_GROUP}" "${FLYNAS_DIR}/ssl/"*.pem
    chmod 600 "${FLYNAS_DIR}/ssl/key.pem"
fi

# --- Enable service ---

step "Enabling FlyNAS service"
if ! grep -q 'flynas_enable' /etc/rc.conf; then
    echo 'flynas_enable="YES"' >> /etc/rc.conf
fi

# --- Done ---

echo ""
echo "FlyNAS installed."
echo "Start with: service flynas start"
echo ""
