#!/bin/sh
# Pi boot-path checks under QEMU: the console with a stdout-path that is
# not a PL011 (the kernel falls back to the first enabled PL011), and
# long bootargs like the Pi firmware's (Linux words in front of
# cmdline.txt).  Boots WORK/root.img from run-netmgmt.sh on vbd0.
#   run-console.sh WORK [-- arm-vm args...]
D=$(cd "$(dirname "$0")/../.." && pwd)
T="$D/tools/arm-smoke"
W=${1:?usage: run-console.sh WORK [-- arm-vm args...]}
shift
[ "${1:-}" = -- ] && shift
set -e
W=$(cd "$W" && pwd)
[ -f "$W/root.img" ] || { echo "run-console.sh: no $W/root.img (run run-netmgmt.sh first)" >&2; exit 1; }
mkdir -p "$W/dtb"
ARM_VM_MACHINE=virt,gic-version=2,dumpdtb=$W/dtb/virt.dtb \
    timeout 60 "$D/bin/arm-vm" -m 2G > /dev/null 2>&1 || true
python3 "$T/fdt-addprop.py" "$W/dtb/virt.dtb" "$W/dtb/nostdout.dtb" \
    /chosen stdout-path s:/pl061@9030000
pad="coherent_pool=1M 8250.nr_uarts=1 console=ttyS0,115200 console=tty1"
i=0
while [ $i -lt 50 ]; do
    pad="$pad pad$i=x$i.firmware-style-filler-word"
    i=$((i + 1))
done
ARM_VM_ARGS="-dtb $W/dtb/nostdout.dtb
    -drive if=none,file=$W/root.img,format=raw,id=d0
    -device virtio-blk-device,drive=d0" \
    "$T/vmexpect.py" -w "${WAIT:-900}" "$T/console.exp" -- \
    -m 2G -t 0 -a "$pad vfs.root.mountfrom=ufs:vbd0" "$@"
