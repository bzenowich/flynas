#!/bin/sh
# Phase 3 exit test under load: boot a world root (mkworldroot.sh) on
# -smp 4, QEMU pinned to two host cpus (taskset), and run load.exp:
# ROUNDS rounds of make -j8 over load.mk plus two dltest loops of NDL
# runs each, all in parallel.
#   run-load.sh WORK [ROUNDS [NDL]] [-- arm-vm args...]
# WAIT= sets the expect timeout per step (default 4 h).
D=$(cd "$(dirname "$0")/../.." && pwd)
T="$D/tools/arm-smoke"
W=${1:?usage: run-load.sh WORK [ROUNDS [NDL]] [-- arm-vm args...]}
shift
rounds=20 ndl=200
[ $# -gt 0 ] && [ "$1" != -- ] && { rounds=$1; shift; }
[ $# -gt 0 ] && [ "$1" != -- ] && { ndl=$1; shift; }
[ "${1:-}" = -- ] && shift
set -e
sh "$T/mkworldroot.sh" "$W"
sed -e "s/@ROUNDS@/$rounds/g" -e "s/@NDL@/$ndl/g" "$T/load.exp" > "$W/load.exp"
exec taskset -c ${CPUS:-0,1} "$T/vmexpect.py" -w "${WAIT:-14400}" "$W/load.exp" -- \
    -s 4 -m 2G -r "$W/world.iso" -a "vfs.root.mountfrom=cd9660:md0" "$@"
