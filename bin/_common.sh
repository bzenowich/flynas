# Shared config for the FlyNAS dev harness (sourced by bin/* scripts).
#
# The dev VM (h2dev) is a DragonFly guest launched by the sibling
# hammer2-raid6 harness. Its login shell is tcsh, so every remote command
# MUST be run through sh — use rsh(), never a bare `vssh '<sh script>'`
# (tcsh will choke on it).
#
# Host, port and SSH identity come from the harness's vmenv.sh rather than
# from a `h2dev` entry in ~/.ssh/config: the claude-box sandbox has no ~/.ssh
# at all, so anything that said `ssh h2dev` was dead in there. If the harness
# is not present we fall back to the alias, which is what a plain host shell
# has always used.

VM="${FLYNAS_VM:-h2dev}"                 # ssh alias, used only as a fallback
FLYNAS_DIR="/usr/local/flynas"

# Repo root and the deployable overlay tree. Scripts outside bin/ (tools/uitest)
# set REPO themselves before sourcing this, since $0 would point at them.
if [ -z "${REPO:-}" ]; then
    REPO="$(cd "$(dirname "$0")/.." && pwd)"
fi
BIN_DIR="$REPO/bin"
OVERLAY="$REPO/overlay"
# The hammer2-raid6 harness that actually launches/babysits h2dev.
H2="${FLYNAS_H2:-$REPO/../hammer2-raid6}"
# Canonicalize: the path ends up inside ssh options, and `.../dfly/../hammer2-raid6`
# in every command line is noise.
if [ -d "$H2" ]; then H2="$(cd "$H2" && pwd)"; fi

if [ -f "$H2/vmenv.sh" ]; then
    VM_ROOT="$H2"
    . "$H2/vmenv.sh"
else
    VM_TARGET="$VM"
    VM_SSH_OPTS=""
    VM_RSYNC_RSH="ssh"
fi

# Raw ssh to the VM: interactive shells, tunnels, commands reading stdin.
# shellcheck disable=SC2086  # VM_SSH_OPTS must word-split
vssh() { ssh $VM_SSH_OPTS "$VM_TARGET" "$@"; }

# Run a sh script (given as $1) on the VM, forcing sh over the tcsh login.
rsh() { printf '%s\n' "$1" | vssh sh; }

# For a background SSH tunnel, background `ssh $VM_SSH_OPTS ... "$VM_TARGET"`
# directly rather than vssh: backgrounding a *function* gives you the pid of a
# subshell, and killing that leaves the real ssh running.
#
# FLYNAS_SSH is the same connection as a single string, for the node tests that
# shell out to sqlite3 on the VM.
FLYNAS_SSH="ssh $VM_SSH_OPTS $VM_TARGET"
export FLYNAS_SSH
