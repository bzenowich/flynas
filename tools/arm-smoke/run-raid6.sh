#!/bin/sh
# Phase 5 exit test: the hammer2 RAID6 suite (hammer2-raid6/tests/v3,
# run_all.sh) on 4 virtio-blk-pci disks.  The kernel and the
# newfs_hammer2/hammer2 tools are built from a worktree of the fork with
# hammer2-raid6's overlay applied (bin/apply-overlay); the rest of the
# world is the stock installed one.
#   run-raid6.sh [-n] WORK [-- arm-vm args...]
#   -n  reuse WORK/root from an earlier run (only redo the images)
# Needs: the worktree in R6SRC (default WORK/src) with the kernel built as
# ARM64_R6 (tools/kobj/ARM64_R6) and sbin/{newfs_hammer2,hammer2} built
# with DFLY_SRC=R6SRC bin/arm-world.  H2R6= points at hammer2-raid6.
# R6GROUPS= picks run_all.sh groups (default all); EXP= replaces raid6.exp
# (e.g. to run one group under sh -x); KERNEL= boots another kernel.bin;
# WAIT= sets the expect
# timeout per step (default 6 h, since run_all.sh prints each group only
# when it ends).
D=$(cd "$(dirname "$0")/../.." && pwd)
T="$D/tools/arm-smoke"
reuse=0
[ "${1:-}" = -n ] && { reuse=1; shift; }
W=${1:?usage: run-raid6.sh [-n] WORK [-- arm-vm args...]}
shift
[ "${1:-}" = -- ] && shift
set -e
mkdir -p "$W"
W=$(cd "$W" && pwd)
R="$W/root"
SRC=$(cd "${R6SRC:-$W/src}" && pwd)
H2R6=$(cd "${H2R6:-$D/../hammer2-raid6}" && pwd)
U="$D/tools/uobj$SRC/sbin"
[ $reuse = 1 ] && [ -d "$R/etc" ] || "$D/bin/arm-installworld" "$R"

install -m 555 "$U/newfs_hammer2/newfs_hammer2" "$U/hammer2/hammer2" "$R/sbin/"
# group F's parity check (standalone C, no DragonFly headers)
mkdir -p "$R/usr/local/bin"
"$D/tools/host/ubin/cc" -O2 -o "$R/usr/local/bin/h2stripe_check" \
    "$H2R6/src/diag/h2stripe_check.c"
rm -rf "$R/root/hammer2-tests"
mkdir -p "$R/root/hammer2-tests"
cp -R "$H2R6/tests/v3" "$R/root/hammer2-tests/"
printf '/dev/vbd0\t/\tufs\trw\t1\t1\n' > "$R/etc/fstab"
cat > "$R/etc/rc.conf" <<EOI
hostname="arm64-virt"
EOI
sed "s/@GROUPS@/${R6GROUPS:-}/" "${EXP:-$T/raid6.exp}" > "$W/raid6.exp"
"$D/bin/arm-mkimg" -s 600m "$R" "$W/root.img"
args="-drive if=none,file=$W/root.img,format=raw,id=d0
    -device virtio-blk-pci,drive=d0"
for i in 1 2 3 4; do
    rm -f "$W/r$i.img"
    truncate -s 4g "$W/r$i.img"
    args="$args -drive if=none,file=$W/r$i.img,format=raw,id=d$i
    -device virtio-blk-pci,drive=d$i"
done

rc=0
ARM_VM_ARGS="-nic none $args" \
    "$T/vmexpect.py" -w "${WAIT:-21600}" "$W/raid6.exp" -- \
    -m 2G -t 0 -a "vfs.root.mountfrom=ufs:vbd0" "$@" \
    "${KERNEL:-$D/tools/kobj/ARM64_R6/kernel.bin}" || rc=$?
grep -a 'Total:\|FAIL\|SUITE rc' "$D/logs/arm-vm.log" | grep -av 'echo' || true
exit $rc
