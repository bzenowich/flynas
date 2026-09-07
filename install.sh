#!/bin/sh
# FlyNAS installer — run on a fresh DragonFlyBSD system as root
#
#   sh install.sh
#
# Optional:
#   FLYNAS_ALPINE_IMAGE=/path/to/alpine.qcow2  install the app-VM base image
#
# What this leaves behind: OpenResty serving the UI/API over TLS, the setuid
# helper, the guest-VM plumbing (NVMM, dnsmasq, pf, seed/image dirs), and the
# snapshot cron. See docs/FlyNAS-plan.md §2.11 for the guest-provisioning
# design this sets up.
set -e

FLYNAS_DIR="/usr/local/flynas"
FLYNAS_GROUP="flynas"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
OVERLAY_DIR="${SCRIPT_DIR}/overlay"

step() {
    echo "==> $1"
}

warn() {
    echo "WARNING: $1" >&2
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

if [ ! -d "$OVERLAY_DIR" ]; then
    echo "Error: no overlay/ next to install.sh (run from a checkout)" >&2
    exit 1
fi

# --- Packages ---
#
# Beyond the web stack: dnsmasq serves DHCP reservations on the guest bridge,
# qemu runs the app VMs under NVMM, and cdrtools supplies the `mkisofs` that
# builds each guest's NoCloud `cidata` seed.
step "Installing packages"
ASSUME_ALWAYS_YES=yes pkg install -y \
    openresty git sqlite3 lua51-cjson libargon2 \
    dnsmasq qemu cdrtools

for cmd in /usr/local/bin/mkisofs /usr/local/sbin/dnsmasq \
           /usr/local/bin/qemu-system-x86_64; do
    [ -x "$cmd" ] || warn "$cmd missing after package install"
done

# --- Group ---
#
# There is no flynas USER: nginx runs as `www`, which joins the flynas group.
# Everything group-owned by flynas is what www is allowed to write.
step "Creating flynas group"
if ! pw groupshow "$FLYNAS_GROUP" >/dev/null 2>&1; then
    pw groupadd "$FLYNAS_GROUP"
fi
pw groupmod "$FLYNAS_GROUP" -m www

# --- Deploy overlay ---
#
# `/.` not `/` — with a trailing slash BSD cp would create /overlay.
step "Deploying files"
cp -R "${OVERLAY_DIR}/." /
chmod +x /usr/local/etc/rc.d/flynas
chmod +x "${FLYNAS_DIR}"/cron/*.sh 2>/dev/null || true

# --- Compile and install setuid helper ---

step "Building flynas-helper"
mkdir -p "${FLYNAS_DIR}/bin"
cc -o "${FLYNAS_DIR}/bin/flynas-helper" "${FLYNAS_DIR}/scripts/flynas-helper.c"
chown root:"$FLYNAS_GROUP" "${FLYNAS_DIR}/bin/flynas-helper"
chmod 4750 "${FLYNAS_DIR}/bin/flynas-helper"

# --- Directories and permissions ---
#
# 2775 on the tree: setgid so files created by www keep group flynas, and
# group-writable so SQLite can create its WAL/shm files next to the DB.
# seeds/ is root-only — it holds each guest's one-time OIDC client secret.
step "Setting permissions"
mkdir -p "${FLYNAS_DIR}/ssl" "${FLYNAS_DIR}/logs" "${FLYNAS_DIR}/run" \
         "${FLYNAS_DIR}/images" "${FLYNAS_DIR}/seeds" \
         /var/run/flynas /var/log/flynas

chgrp -R "$FLYNAS_GROUP" "$FLYNAS_DIR"
chmod 2775 "$FLYNAS_DIR"
chmod 0700 "${FLYNAS_DIR}/seeds"
chown root:"$FLYNAS_GROUP" "${FLYNAS_DIR}/seeds" "${FLYNAS_DIR}/images"

chgrp "$FLYNAS_GROUP" /var/run/flynas /var/log/flynas
chmod 2775 /var/run/flynas /var/log/flynas

# --- SQLite database ---

step "Creating database"
DB_FILE="${FLYNAS_DIR}/flynas.db"
if [ ! -f "$DB_FILE" ]; then
    touch "$DB_FILE"
fi
chgrp "$FLYNAS_GROUP" "$DB_FILE"
chmod 664 "$DB_FILE"

# --- Self-signed SSL cert (if none exists) ---

step "Checking SSL certificate"
if [ ! -f "${FLYNAS_DIR}/ssl/cert.pem" ]; then
    step "Generating self-signed SSL certificate"
    openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 \
        -keyout "${FLYNAS_DIR}/ssl/key.pem" \
        -out "${FLYNAS_DIR}/ssl/cert.pem" \
        -days 3650 -nodes \
        -subj "/CN=flynas"
    chgrp "$FLYNAS_GROUP" "${FLYNAS_DIR}/ssl/"*.pem
    chmod 600 "${FLYNAS_DIR}/ssl/key.pem"
fi

# --- OIDC signing keypair ---
#
# util/oidc_keys.lua generates this lazily on first use, but then it is owned
# by the nginx worker that happened to get there first. Minting it here keeps
# ownership and modes deterministic, and keeps the first SSO request fast.
step "Generating OIDC signing keypair"
if [ ! -f "${FLYNAS_DIR}/oidc_key.pem" ]; then
    openssl genrsa -out "${FLYNAS_DIR}/oidc_key.pem" 2048
    openssl rsa -in "${FLYNAS_DIR}/oidc_key.pem" \
        -pubout -out "${FLYNAS_DIR}/oidc_pub.pem"
fi
chgrp "$FLYNAS_GROUP" "${FLYNAS_DIR}/oidc_key.pem" "${FLYNAS_DIR}/oidc_pub.pem"
chmod 640 "${FLYNAS_DIR}/oidc_key.pem"
chmod 644 "${FLYNAS_DIR}/oidc_pub.pem"

# --- NVMM (hardware virtualisation for app VMs) ---
#
# Without this every app VM silently falls back to emulation and a guest that
# took two minutes to boot takes twenty.
step "Enabling NVMM"
if ! grep -q '^nvmm_load' /boot/loader.conf 2>/dev/null; then
    echo 'nvmm_load="YES"' >> /boot/loader.conf
fi
kldstat -q -m nvmm 2>/dev/null || kldload nvmm 2>/dev/null \
    || warn "could not load nvmm now; it will load at the next boot"

# --- Guest network prerequisites ---
#
# The bridge, pf NAT anchor and dnsmasq are brought up by the helper
# (`flynas-helper netbridge up`) when the first VM needs them, so nothing is
# enabled in rc.conf here — but the modules must be loadable, and dnsmasq
# reads its config from the overlay, not /usr/local/etc.
step "Checking guest network prerequisites"
for mod in if_bridge pf; do
    kldstat -q -m "$mod" 2>/dev/null || kldload "$mod" 2>/dev/null \
        || warn "could not load $mod; guest networking will fail"
done
[ -f "${FLYNAS_DIR}/conf/dnsmasq.conf" ] \
    || warn "${FLYNAS_DIR}/conf/dnsmasq.conf missing — guest DHCP will fail"

# --- App-VM base image ---
#
# Alpine's stock cloud image flakily kernel-panics under NVMM ("IO-APIC +
# timer doesn't work"), so the copy used here must have `no_timer_check` in
# its extlinux cmdline. Patching it means writing ext4, which DragonFly
# cannot do — it is prepared on a Linux host with `debugfs -w` (see
# docs/FlyNAS-plan.md §2.11 step 1) and supplied here.
step "Checking app-VM base image"
ALPINE_DEST="${FLYNAS_DIR}/images/alpine.qcow2"
if [ -n "${FLYNAS_ALPINE_IMAGE:-}" ] && [ -f "$FLYNAS_ALPINE_IMAGE" ]; then
    cp "$FLYNAS_ALPINE_IMAGE" "$ALPINE_DEST"
    chgrp "$FLYNAS_GROUP" "$ALPINE_DEST"
    chmod 644 "$ALPINE_DEST"
    echo "    installed from $FLYNAS_ALPINE_IMAGE"
fi
if [ ! -f "$ALPINE_DEST" ]; then
    warn "no ${ALPINE_DEST} — apps cannot be provisioned until one is supplied"
    warn "  (Alpine nocloud qcow2, patched with no_timer_check; then re-run"
    warn "   with FLYNAS_ALPINE_IMAGE=/path/to/alpine.qcow2)"
fi

# --- Cron ---

step "Installing cron jobs"
if [ -f "${FLYNAS_DIR}/cron/hourly-snapshots.sh" ] \
   && ! grep -q 'hourly-snapshots.sh' /etc/crontab; then
    echo "0 * * * *  root  ${FLYNAS_DIR}/cron/hourly-snapshots.sh" >> /etc/crontab
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
