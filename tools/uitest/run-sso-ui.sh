#!/bin/sh
# Seed an oidc_client + a target user, run the SSO grant-matrix UI test,
# clean up. SQL piped to `ssh h2dev sh` (remote tcsh chokes on inline parens).
set -e
cd "$(dirname "$0")"

CLIENT_ID=sso-ui-client
TARGET=ssouitestuser
REDIRECT='https://localhost:8443/cb'

cleanup() {
    [ -n "$T" ] && kill "$T" 2>/dev/null || true
    ssh h2dev sh <<EOF 2>/dev/null || true
sqlite3 /usr/local/flynas/flynas.db "DELETE FROM app_grants WHERE client_id IN (SELECT id FROM oidc_clients WHERE client_id='$CLIENT_ID'); DELETE FROM oidc_clients WHERE client_id='$CLIENT_ID'; DELETE FROM users WHERE username='$TARGET';"
EOF
}
trap cleanup EXIT

ssh h2dev sh <<EOF
sqlite3 /usr/local/flynas/flynas.db "DELETE FROM oidc_clients WHERE client_id='$CLIENT_ID'; DELETE FROM users WHERE username='$TARGET'; INSERT INTO oidc_clients (vm_id,name,client_id,client_secret_hash,redirect_uris) VALUES (NULL,'SSO UI Test','$CLIENT_ID','x','$REDIRECT'); INSERT INTO users (username,is_admin) VALUES ('$TARGET',0);"
EOF

TOTP_SECRET=$(ssh h2dev sh <<'EOF'
sqlite3 /usr/local/flynas/flynas.db "SELECT totp_secret FROM users WHERE is_admin=1 LIMIT 1"
EOF
)
[ -n "$TOTP_SECRET" ] || { echo "no admin TOTP secret"; exit 1; }

ssh -N -L 8443:localhost:443 h2dev & T=$!
sleep 3
curl -ks https://localhost:8443/api/health >/dev/null || { echo "tunnel/health failed"; exit 1; }

mkdir -p /tmp/flynas-uitest
TOTP_SECRET="$TOTP_SECRET" node test-sso-ui.mjs
