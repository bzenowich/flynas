#!/bin/sh
# Test bin/arm-pisd's SD image under QEMU (as far as QEMU can: no Pi
# firmware, so the ARM64_VIRT kernel boots instead of kernel8.img):
#  1. the whole image as a virtio disk, root on slice 2 (pisd-sd.exp):
#     MBR slices, fstab, the FAT boot slice (fsck_msdosfs, long and
#     lower-case names, checksums of kernel8.img and root-md.img),
#     ssh host keys and rc.conf;
#  2. root-md.img as an md root (pisd-md.exp), as the firmware's
#     initramfs line would give it.
#   run-pisd.sh [-n] WORK [-- arm-vm args...]
#   -n  reuse WORK/root (arm-pisd -n)
D=$(cd "$(dirname "$0")/../.." && pwd)
T="$D/tools/arm-smoke"
n=
[ "${1:-}" = -n ] && { n=-n; shift; }
W=${1:?usage: run-pisd.sh [-n] WORK [-- arm-vm args...]}
shift
[ "${1:-}" = -- ] && shift
set -e
mkdir -p "$W"
W=$(cd "$W" && pwd)
"$D/bin/arm-pisd" $n -r sd -D vbd0 -s 600m "$W"
ksum=$(sha256sum "$D/tools/kobj/ARM64_RPI4/kernel.bin" | cut -d' ' -f1)
msum=$(sha256sum "$W/root-md.img" | cut -d' ' -f1)
hfp=$(ssh-keygen -lf "$W/hostkeys/ssh_host_ed25519_key.pub" | cut -d' ' -f2)
for x in sd md; do
    sed -e "s|@KSUM@|$ksum|; s|@MSUM@|$msum|; s|@HFP@|$hfp|" \
        "$T/pisd-$x.exp" > "$W/pisd-$x.exp"
done
ARM_VM_ARGS="-drive if=none,file=$W/flynas-pi4.img,format=raw,id=d0
    -device virtio-blk-device,drive=d0" \
    "$T/vmexpect.py" -w "${WAIT:-900}" "$W/pisd-sd.exp" -- \
    -m 2G -t 0 -a "vfs.root.mountfrom=ufs:vbd0s2" "$@"
"$T/vmexpect.py" -w "${WAIT:-900}" "$W/pisd-md.exp" -- \
    -m 2G -t 0 -r "$W/root-md.img" -a "vfs.root.mountfrom=ufs:md0" "$@"
