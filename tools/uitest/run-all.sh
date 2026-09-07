#!/bin/sh
# Run the FlyNAS UI test suite under ONE tunnel with ONE TOTP extraction,
# seeding/tearing-down each test's preconditions, and print a pass/fail
# summary. Replaces the per-test tunnel dance (and the pkill footgun) in the
# individual run-*.sh scripts.
#
#   ./run-all.sh              self-contained + OIDC-seeded tests (safe, default)
#
# NOT included (run their dedicated scripts manually — each is stateful in a
# way that can wedge the VM if it half-runs):
#   - storage / backup : need scratch disks vbd1-4 and MUST clean up volumes,
#                        or leftover fstab entries break the next boot.
#   - app-sso          : needs a fully provisioned+running Forgejo guest;
#                        use ./run-app-sso.sh (it also builds the guest tunnel).
set -e
cd "$(dirname "$0")"

# Host, port and SSH key come from the harness via ../../bin/_common.sh — not
# from an ssh-config alias, which does not exist inside the claude-box
# sandbox. It also exports FLYNAS_SSH for the node tests.
REPO="$(cd ../.. && pwd)"
. "$REPO/bin/_common.sh"

DB="/usr/local/flynas/flynas.db"
PORT=8443

# All SQL/sh is piped to `vssh sh` via heredoc — the VM login shell is
# tcsh and re-parses any inline command, choking on SQL parens (see README).

T=""
cleanup() {
    accounts_teardown 2>/dev/null || true
    oidc_teardown 2>/dev/null || true
    [ -n "$T" ] && kill "$T" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

# ---- TOTP + tunnel (once) -------------------------------------------------

TOTP_SECRET=$(vssh sh <<EOF
sqlite3 $DB "SELECT totp_secret FROM users WHERE is_admin=1 LIMIT 1"
EOF
)
[ -n "$TOTP_SECRET" ] || { echo "no admin TOTP secret found"; exit 1; }
export TOTP_SECRET

# Backgrounded directly, not through vssh(): backgrounding a function gives
# the pid of a subshell, and killing that would leave the tunnel behind.
# shellcheck disable=SC2086  # VM_SSH_OPTS must word-split
ssh $VM_SSH_OPTS -N -L "$PORT:localhost:443" "$VM_TARGET" & T=$!
sleep 3
curl -ks "https://localhost:$PORT/api/health" >/dev/null \
    || { echo "tunnel/health check failed"; exit 1; }
echo "tunnel up (health OK), TOTP loaded"
mkdir -p /tmp/flynas-uitest

# ---- per-test seeding -----------------------------------------------------

OIDC_CLIENT_ID=uitest-oidc
SSOUI_CLIENT=sso-ui-client
SSOUI_USER=ssouitestuser

oidc_seed() {
    vssh sh <<EOF
sqlite3 $DB "DELETE FROM oidc_clients WHERE client_id IN ('$OIDC_CLIENT_ID','$SSOUI_CLIENT'); DELETE FROM users WHERE username='$SSOUI_USER'; INSERT INTO oidc_clients (vm_id,name,client_id,client_secret_hash,redirect_uris) VALUES (NULL,'uitest-oidc','$OIDC_CLIENT_ID','x','https://localhost:$PORT/oidc-cb-probe'),(NULL,'SSO UI Test','$SSOUI_CLIENT','x','https://localhost:$PORT/cb'); INSERT INTO users (username,is_admin) VALUES ('$SSOUI_USER',0);"
EOF
}
oidc_teardown() {
    vssh sh <<EOF
sqlite3 $DB "DELETE FROM app_grants WHERE client_id IN (SELECT id FROM oidc_clients WHERE client_id IN ('$OIDC_CLIENT_ID','$SSOUI_CLIENT')); DELETE FROM oidc_codes WHERE client_id IN ('$OIDC_CLIENT_ID','$SSOUI_CLIENT'); DELETE FROM oidc_clients WHERE client_id IN ('$OIDC_CLIENT_ID','$SSOUI_CLIENT'); DELETE FROM users WHERE username='$SSOUI_USER';"
EOF
}

accounts_seed() {
    vssh sh <<EOF
pw groupadd testgrp 2>/dev/null || true
pw userdel uitestuser 2>/dev/null || true
sqlite3 $DB "INSERT OR IGNORE INTO groups(name) VALUES('testgrp'); INSERT OR IGNORE INTO users(username) VALUES('testuser1'); DELETE FROM users WHERE username='uitestuser';"
EOF
}
accounts_teardown() {
    vssh sh <<EOF
pw userdel uitestuser 2>/dev/null || true
pw groupdel testgrp 2>/dev/null || true
sqlite3 $DB "DELETE FROM user_groups WHERE user_id IN (SELECT id FROM users WHERE username IN ('testuser1','uitestuser')); DELETE FROM users WHERE username IN ('testuser1','uitestuser'); DELETE FROM groups WHERE name='testgrp';"
EOF
}

# ---- runner ---------------------------------------------------------------

PASS=0
FAIL=0
FAILED=""

# run <label> <test.mjs> [extra env assignments...]
run() {
    label="$1"; script="$2"; shift 2
    printf '\n=== %s (%s) ===\n' "$label" "$script"
    if env "$@" TOTP_SECRET="$TOTP_SECRET" node "$script"; then
        PASS=$((PASS + 1))
    else
        FAIL=$((FAIL + 1))
        FAILED="$FAILED $label"
    fi
}

# Self-contained tests (create + clean their own state).
run network    test-network.mjs
run monitoring test-monitoring.mjs
run vms        test-vms.mjs
run apps       test-apps.mjs

# OIDC tests (shared seed; torn down at exit).
oidc_seed
run oidc   test-oidc.mjs   OIDC_CLIENT_ID="$OIDC_CLIENT_ID"
run sso-ui test-sso-ui.mjs

# Accounts (best-effort seed; a seed miss reports as a fail, doesn't abort).
accounts_seed
run accounts test-accounts.mjs
accounts_teardown

# ---- summary --------------------------------------------------------------

printf '\n========================================\n'
printf 'PASSED: %d   FAILED: %d\n' "$PASS" "$FAIL"
if [ "$FAIL" -gt 0 ]; then
    printf 'failed:%s\n' "$FAILED"
    exit 1
fi
echo "all green"
