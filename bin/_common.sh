# Shared config for the FlyNAS dev harness (sourced by bin/* scripts).
#
# The dev VM (h2dev) is a DragonFly guest launched by the sibling
# hammer2-raid6 harness. Its login shell is tcsh, so every remote command
# MUST be run through sh — use rsh() / the `ssh "$VM" sh` heredoc pattern,
# never a bare `ssh "$VM" '<sh script>'` (tcsh will choke on it).

VM="${FLYNAS_VM:-h2dev}"                 # ssh alias (see ~/.ssh/config)
FLYNAS_DIR="/usr/local/flynas"

# Repo root (parent of bin/) and the deployable overlay tree.
BIN_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$BIN_DIR/.." && pwd)"
OVERLAY="$REPO/overlay"
# The hammer2-raid6 harness that actually launches/babysits h2dev.
H2="${FLYNAS_H2:-$REPO/../hammer2-raid6}"

# Run a sh script (given as $1) on the VM, forcing sh over the tcsh login.
rsh() { printf '%s\n' "$1" | ssh "$VM" sh; }
