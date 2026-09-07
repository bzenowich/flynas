#!/bin/sh
# Seed an oidc_client + a target user, run the SSO grant-matrix UI test,
# clean up. SQL piped to `vssh sh` (remote tcsh chokes on inline parens).
set -e
cd "$(dirname "$0")"

# Host, port and SSH key come from the harness via ../../bin/_common.sh — not
# from an ssh-config alias, which does not exist inside the claude-box
# sandbox. It also exports FLYNAS_SSH for the node tests.
REPO="$(cd ../.. && pwd)"
. "$REPO/bin/_common.sh"

CLIENT_ID=sso-ui-client
TARGET=ssouitestuser
REDIRECT='https://localhost:8443/cb'

cleanup() {
    [ -n "$T" ] && kill "$T" 2>/dev/null || true
    vssh sh <<EOF 2>/dev/null || true
sqlite3 /usr/local/flynas/flynas.db "DELETE FROM app_grants WHERE client_id IN (SELECT id FROM oidc_clients WHERE client_id='$CLIENT_ID'); DELETE FROM oidc_clients WHERE client_id='$CLIENT_ID'; DELETE FROM users WHERE username='$TARGET';"
EOF
}
trap cleanup EXIT

vssh sh <<EOF
sqlite3 /usr/local/flynas/flynas.db "DELETE FROM oidc_clients WHERE client_id='$CLIENT_ID'; DELETE FROM users WHERE username='$TARGET'; INSERT INTO oidc_clients (vm_id,name,client_id,client_secret_hash,redirect_uris) VALUES (NULL,'SSO UI Test','$CLIENT_ID','x','$REDIRECT'); INSERT INTO users (username,is_admin) VALUES ('$TARGET',0);"
EOF

TOTP_SECRET=$(vssh sh <<'EOF'
sqlite3 /usr/local/flynas/flynas.db "SELECT totp_secret FROM users WHERE is_admin=1 LIMIT 1"
EOF
)
[ -n "$TOTP_SECRET" ] || { echo "no admin TOTP secret"; exit 1; }

# Backgrounded directly, not through vssh(): backgrounding a function gives
# the pid of a subshell, and killing that would leave the tunnel behind.
# shellcheck disable=SC2086  # VM_SSH_OPTS must word-split
ssh $VM_SSH_OPTS -N -L 8443:localhost:443 "$VM_TARGET" & T=$!
sleep 3
curl -ks https://localhost:8443/api/health >/dev/null || { echo "tunnel/health failed"; exit 1; }

mkdir -p /tmp/flynas-uitest
TOTP_SECRET="$TOTP_SECRET" node test-sso-ui.mjs
