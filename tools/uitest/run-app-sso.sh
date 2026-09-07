#!/bin/sh
# Drive the app-VM SSO round-trip against the live provisioned Forgejo guest.
# Two tunnels in this shell: 8443->FlyNAS:443, 20001->guest:3000 (via the VM).
# The guest IP + admin TOTP secret come from the DB over `vssh sh`.
set -e
cd "$(dirname "$0")"

# Host, port and SSH key come from the harness via ../../bin/_common.sh — not
# from an ssh-config alias, which does not exist inside the claude-box
# sandbox. It also exports FLYNAS_SSH for the node tests.
REPO="$(cd ../.. && pwd)"
. "$REPO/bin/_common.sh"

GUEST_IP=$(vssh sh <<'EOF'
sqlite3 /usr/local/flynas/flynas.db "SELECT ip_address FROM vms WHERE name='forge1'"
EOF
)
[ -n "$GUEST_IP" ] || { echo "forge1 VM/IP not found"; exit 1; }

TOTP_SECRET=$(vssh sh <<'EOF'
sqlite3 /usr/local/flynas/flynas.db "SELECT totp_secret FROM users WHERE is_admin=1 LIMIT 1"
EOF
)
[ -n "$TOTP_SECRET" ] || { echo "no admin TOTP secret"; exit 1; }
PORT="${FLYNAS_PORT:-19443}"
echo "guest=$GUEST_IP port=$PORT"

cleanup() { [ -n "$T" ] && kill "$T" 2>/dev/null || true; }
trap cleanup EXIT

# Backgrounded directly, not through vssh(): backgrounding a function gives
# the pid of a subshell, and killing that would leave the tunnel behind.
#
# ExitOnForwardFailure=yes is not optional here. ssh defaults it to "no", so a
# port already in use leaves ssh running with no forward and the checks below
# then talk to whatever else holds that port. Inside the claude-box sandbox
# 8443 is exactly that: the box's own HTTPS proxy (socat -> proxy.sock, see
# sandbox.conf), which is why FLYNAS_PORT defaults off it.
# shellcheck disable=SC2086  # VM_SSH_OPTS must word-split
ssh $VM_SSH_OPTS -o ExitOnForwardFailure=yes -N -L "$PORT:localhost:443" \
    -L "20001:${GUEST_IP}:3000" "$VM_TARGET" & T=$!
sleep 3
curl -ks "https://localhost:$PORT/api/health" >/dev/null || { echo "flynas tunnel/health failed"; exit 1; }
code=$(curl -s -o /dev/null -w '%{http_code}' --max-time 8 http://localhost:20001/ 2>/dev/null)
[ "$code" = "200" ] || { echo "forgejo tunnel failed (http=$code)"; exit 1; }
echo "tunnels up (flynas health OK, forgejo=$code)"

mkdir -p /tmp/flynas-uitest
TOTP_SECRET="$TOTP_SECRET" node test-app-sso.mjs
