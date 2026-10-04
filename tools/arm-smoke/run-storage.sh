#!/bin/sh
# Phase 5d test: the installed world with root on virtio-blk-pci, plus a
# disk on QEMU's AHCI controller and a usb-storage disk on qemu-xhci, all
# behind the ECAM host bridge, booted with hw.busdma.debug=3.
# storage.exp writes and reads back both CAM disks and checks the busdma
# sync audit.
#   run-storage.sh [-n] WORK [-- arm-vm args...]
#   -n  reuse WORK/root from an earlier run (only redo the images)
# WAIT= sets the expect timeout per step (default 900 s).
D=$(cd "$(dirname "$0")/../.." && pwd)
T="$D/tools/arm-smoke"
reuse=0
[ "${1:-}" = -n ] && { reuse=1; shift; }
W=${1:?usage: run-storage.sh [-n] WORK [-- arm-vm args...]}
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
EOI
"$D/bin/arm-mkimg" -s 400m "$R" "$W/root.img"
for d in sata usb; do
    rm -f "$W/$d.img"
    truncate -s 64m "$W/$d.img"
done

ARM_VM_ARGS="-nic none
    -drive if=none,file=$W/root.img,format=raw,id=d0
    -device virtio-blk-pci,drive=d0
    -drive if=none,file=$W/sata.img,format=raw,id=d1
    -device ahci,id=ahci -device ide-hd,drive=d1,bus=ahci.0
    -drive if=none,file=$W/usb.img,format=raw,id=d2
    -device qemu-xhci,id=xhci -device usb-storage,drive=d2,bus=xhci.0" \
    "$T/vmexpect.py" -w "${WAIT:-900}" "$T/storage.exp" -- \
    -m 2G -t 0 -a "vfs.root.mountfrom=ufs:vbd0 hw.busdma.debug=3" "$@"
