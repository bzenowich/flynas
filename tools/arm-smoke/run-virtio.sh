#!/bin/sh
# Phase 5a test: the installed world booted from a virtio-mmio disk
# (vtblk, root on vbd0) with a virtio-mmio NIC (vtnet, DHCP from QEMU's
# user network).  virtio.exp logs in on the console, checks the root
# mount, the lease, a 4 MB fetch over TCP from a web server on the host,
# and a write/read-back of a second virtio disk.
#   run-virtio.sh [-n] [-p] WORK [-- arm-vm args...]
#   -n  reuse WORK/root from an earlier run (only redo the images)
#   -p  the same devices as virtio-pci behind the ECAM host bridge (5b)
# WAIT= sets the expect timeout per step (default 900 s).
D=$(cd "$(dirname "$0")/../.." && pwd)
T="$D/tools/arm-smoke"
reuse=0 pci=0
while :; do
    case "${1:-}" in
    -n) reuse=1; shift ;;
    -p) pci=1; shift ;;
    *) break ;;
    esac
done
W=${1:?usage: run-virtio.sh [-n] [-p] WORK [-- arm-vm args...]}
shift
[ "${1:-}" = -- ] && shift
set -e
mkdir -p "$W"
W=$(cd "$W" && pwd)
R="$W/root"
[ $reuse = 1 ] && [ -d "$R/etc" ] || "$D/bin/arm-installworld" "$R"

printf '/dev/vbd0\t/\tufs\trw\t1\t1\n' > "$R/etc/fstab"
cat > "$R/etc/rc.conf" <<EOI
hostname="arm64-virt"
ifconfig_vtnet0="DHCP"
sshd_enable="YES"
EOI
"$D/bin/arm-mkimg" -s 400m "$R" "$W/root.img"
rm -f "$W/d1.img"
truncate -s 64m "$W/d1.img"

# The host side of the network test: 4 MB of random data over HTTP,
# reached from the guest as 10.0.2.2.
mkdir -p "$W/www"
head -c 4194304 /dev/urandom > "$W/www/blob"
sum=$(sha256sum "$W/www/blob" | cut -d' ' -f1)
port=$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1])')
python3 -m http.server -b 127.0.0.1 -d "$W/www" "$port" > "$W/http.log" 2>&1 &
http=$!
trap 'kill $http 2>/dev/null' EXIT INT TERM
sed -e "s/@PORT@/$port/g" -e "s/@SUM@/$sum/g" "$T/virtio.exp" > "$W/virtio.exp"

# QEMU fills the virtio-mmio slots from the top and they attach from the
# bottom, so the last -device is unit 0: the root disk goes last.  PCI
# devices take slots in order and attach in slot order: root goes first.
if [ $pci = 1 ]; then
    devs="-drive if=none,file=$W/root.img,format=raw,id=d0
    -device virtio-blk-pci,drive=d0
    -drive if=none,file=$W/d1.img,format=raw,id=d1
    -device virtio-blk-pci,drive=d1
    -netdev user,id=n0 -device virtio-net-pci,netdev=n0"
else
    devs="-netdev user,id=n0 -device virtio-net-device,netdev=n0
    -drive if=none,file=$W/d1.img,format=raw,id=d1
    -device virtio-blk-device,drive=d1
    -drive if=none,file=$W/root.img,format=raw,id=d0
    -device virtio-blk-device,drive=d0"
fi
ARM_VM_ARGS="$devs" \
    "$T/vmexpect.py" -w "${WAIT:-900}" "$W/virtio.exp" -- \
    -m 2G -t 0 -a "vfs.root.mountfrom=ufs:vbd0" "$@"
