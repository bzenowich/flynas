#!/bin/sh
# Phase 4 exit test: installworld + etc distribution into WORK/root
# (bin/arm-installworld), a UFS image of it (bin/arm-mkimg), booted as the
# md root to multi-user; multiuser.exp logs in on the console and checks rc,
# getty, the databases and an ssh login over loopback (no NIC until Phase 5).
#   run-multiuser.sh [-n] WORK [-- arm-vm args...]
#   -n  reuse WORK/root from an earlier run (only redo the image)
# The image gets an fstab, an rc.conf with sshd, an ssh key for root, and
# cxxtest.cc built dynamic and static in /root.
# WAIT= sets the expect timeout per step (default 900 s).
D=$(cd "$(dirname "$0")/../.." && pwd)
T="$D/tools/arm-smoke"
reuse=0
[ "${1:-}" = -n ] && { reuse=1; shift; }
W=${1:?usage: run-multiuser.sh [-n] WORK [-- arm-vm args...]}
shift
[ "${1:-}" = -- ] && shift
set -e
mkdir -p "$W"
W=$(cd "$W" && pwd)
R="$W/root"
[ $reuse = 1 ] && [ -d "$R/etc" ] || "$D/bin/arm-installworld" "$R"

printf '/dev/md0\t/\tufs\trw\t1\t1\n' > "$R/etc/fstab"
cat > "$R/etc/rc.conf" <<EOI
hostname="arm64-virt"
sshd_enable="YES"
EOI
mkdir -p "$R/root/.ssh"
rm -f "$R/root/.ssh/id_ed25519"*
ssh-keygen -q -t ed25519 -N '' -C root@arm64-virt -f "$R/root/.ssh/id_ed25519"
cp "$R/root/.ssh/id_ed25519.pub" "$R/root/.ssh/authorized_keys"
{
    echo "$R/root/.ssh root wheel 700"
    echo "$R/root/.ssh/id_ed25519 root wheel 600"
    echo "$R/root/.ssh/authorized_keys root wheel 600"
} >> "$R.metalog"
for l in dyn static; do
    f=; [ $l = static ] && f=-static
    "$D/tools/host/ubin/c++" -O2 -std=c++20 -pthread $f "$T/cxxtest.cc" -o "$R/root/cxxtest-$l"
done
"$D/bin/arm-mkimg" -s 400m "$R" "$W/root.img"
exec "$T/vmexpect.py" -w "${WAIT:-900}" "$T/multiuser.exp" -- \
    -m 2G -t 0 -r "$W/root.img" -a "vfs.root.mountfrom=ufs:md0" "$@"
