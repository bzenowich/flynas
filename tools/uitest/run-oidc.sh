#!/bin/sh
# Seed an oidc_client, run the OIDC SSO handoff UI test, clean up.
# Tunnel is kept in this same shell (README §"tunnel drops"). All SQL is
# piped to `vssh sh` stdin — the remote login shell is tcsh and
# re-parses any inline command, choking on the parens in SQL.
set -e
cd "$(dirname "$0")"

# Host, port and SSH key come from the harness via ../../bin/_common.sh — not
# from an ssh-config alias, which does not exist inside the claude-box
# sandbox. It also exports FLYNAS_SSH for the node tests.
REPO="$(cd ../.. && pwd)"
. "$REPO/bin/_common.sh"

CLIENT_ID=uitest-oidc
REDIRECT='https://localhost:8443/oidc-cb-probe'

cleanup() {
    [ -n "$T" ] && kill "$T" 2>/dev/null || true
    vssh sh <<EOF 2>/dev/null || true
sqlite3 /usr/local/flynas/flynas.db "DELETE FROM oidc_codes WHERE client_id='$CLIENT_ID'; DELETE FROM oidc_clients WHERE client_id='$CLIENT_ID';"
EOF
}
trap cleanup EXIT

# Seed the client (secret_hash unused by /authorize; token exchange not tested).
vssh sh <<EOF
sqlite3 /usr/local/flynas/flynas.db "DELETE FROM oidc_clients WHERE client_id='$CLIENT_ID'; INSERT INTO oidc_clients (vm_id,name,client_id,client_secret_hash,redirect_uris) VALUES (NULL,'uitest-oidc','$CLIENT_ID','x','$REDIRECT');"
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
# Gate on health, not on the tunnel pid (README §pkill warning).
curl -ks https://localhost:8443/api/health >/dev/null || { echo "tunnel/health failed"; exit 1; }

mkdir -p /tmp/flynas-uitest
OIDC_CLIENT_ID="$CLIENT_ID" TOTP_SECRET="$TOTP_SECRET" node test-oidc.mjs
