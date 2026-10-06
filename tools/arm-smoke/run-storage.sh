#!/bin/sh
# Phase 5d test: the installed world with root on virtio-blk-pci, plus a
# disk on QEMU's AHCI controller and a usb-storage disk on qemu-xhci, all
# behind the ECAM host bridge, booted with hw.busdma.debug=3.
# storage.exp writes and reads back both CAM disks and checks the busdma
# sync audit.
#   run-storage.sh [-n] WORK [-- arm-vm args...]
#   -n  reuse WORK/root from an earlier run (only redo the images)
# WAIT= sets the expect timeout per step (default 900 s).  EXP= replaces
# storage.exp (flush.exp: cache flushes on the usb-storage disk), USBSIZE=
# sizes the usb-storage image (default 64m, 0 for none), KENV= adds kernel
# environment variables (e.g. KENV=hw.busdma.lowaddr=0x5fffffff), DTB=
# replaces QEMU's device tree (see fdt-addprop.py), RCCONF= adds lines to
# the guest's rc.conf.  DUMPSIZE= adds a second virtio disk of that size
# (vbd1) for crash dumps and installs the kernel as /boot/kernel/kernel
# (dump.exp); set ARM_VM_REBOOT=1 with it so the guest can reboot.
# KMODS=1 installs the modules built for KERNCONF (bin/arm-kbuild ...
# modules) into /boot/kernel (kmod.exp).
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
${RCCONF:-}
EOI
if [ -n "${DUMPSIZE:-}" ]; then
    install -m 555 "$D/tools/kobj/${KERNCONF:-ARM64_VIRT}/kernel.stripped" \
        "$R/boot/kernel/kernel"
    rm -f "$W/dump.img"
    truncate -s "$DUMPSIZE" "$W/dump.img"
    DUMPARGS="-drive if=none,file=$W/dump.img,format=raw,id=d3
    -device virtio-blk-pci,drive=d3"
fi
if [ -n "${KMODS:-}" ]; then
    mkdir -p "$R/boot/kernel"
    rm -f "$R"/boot/kernel/*.ko
    find "$D/tools/kobj/${KERNCONF:-ARM64_VIRT}" -name '*.ko' \
        -exec install -m 555 {} "$R/boot/kernel/" \;
fi
"$D/bin/arm-mkimg" -s ${ROOTSIZE:-400m} "$R" "$W/root.img"
for d in sata usb; do
    rm -f "$W/$d.img"
done
truncate -s 64m "$W/sata.img"
# USBSIZE=0: no usb-storage disk.
if [ "${USBSIZE:-64m}" = 0 ]; then
    USBARGS=
else
    truncate -s "${USBSIZE:-64m}" "$W/usb.img"
    USBARGS="-drive if=none,file=$W/usb.img,format=raw,id=d2
    -device usb-storage,drive=d2,bus=xhci.0"
fi

ARM_VM_ARGS="-nic none
    -drive if=none,file=$W/root.img,format=raw,id=d0
    -device virtio-blk-pci,drive=d0
    -drive if=none,file=$W/sata.img,format=raw,id=d1
    -device ahci,id=ahci -device ide-hd,drive=d1,bus=ahci.0
    -device qemu-xhci,id=xhci
    $USBARGS
    ${DUMPARGS:-}
    ${DTB:+-dtb $DTB}" \
    "$T/vmexpect.py" -w "${WAIT:-900}" "${EXP:-$T/storage.exp}" -- \
    -m 2G -t 0 -a "vfs.root.mountfrom=ufs:vbd0 hw.busdma.debug=3${KENV:+ $KENV}" "$@"
