#!/bin/sh
# Drive the app-VM SSO round-trip against the live provisioned Forgejo guest.
# Two tunnels in this shell: 8443->FlyNAS:443, 20001->guest:3000 (via h2dev).
# The guest IP + admin TOTP secret come from the DB over `ssh h2dev sh`.
set -e
cd "$(dirname "$0")"

GUEST_IP=$(ssh h2dev sh <<'EOF'
sqlite3 /usr/local/flynas/flynas.db "SELECT ip_address FROM vms WHERE name='forge1'"
EOF
)
[ -n "$GUEST_IP" ] || { echo "forge1 VM/IP not found"; exit 1; }

TOTP_SECRET=$(ssh h2dev sh <<'EOF'
sqlite3 /usr/local/flynas/flynas.db "SELECT totp_secret FROM users WHERE is_admin=1 LIMIT 1"
EOF
)
[ -n "$TOTP_SECRET" ] || { echo "no admin TOTP secret"; exit 1; }
echo "guest=$GUEST_IP"

cleanup() { [ -n "$T" ] && kill "$T" 2>/dev/null || true; }
trap cleanup EXIT

ssh -N -L 8443:localhost:443 -L "20001:${GUEST_IP}:3000" h2dev & T=$!
sleep 3
curl -ks https://localhost:8443/api/health >/dev/null || { echo "flynas tunnel/health failed"; exit 1; }
code=$(curl -s -o /dev/null -w '%{http_code}' --max-time 8 http://localhost:20001/ 2>/dev/null)
[ "$code" = "200" ] || { echo "forgejo tunnel failed (http=$code)"; exit 1; }
echo "tunnels up (flynas health OK, forgejo=$code)"

mkdir -p /tmp/flynas-uitest
TOTP_SECRET="$TOTP_SECRET" node test-app-sso.mjs
