#!/bin/sh
# Add fdtres.exp's test nodes (clocks, resets, regulators, a syscon and
# their consumer) to a QEMU virt device tree:
#
#   ARM_VM_MACHINE=virt,gic-version=2,dumpdtb=virt.dtb bin/arm-vm -s 2 -m 2G K
#   tools/arm-smoke/mk-res-dtb.sh virt.dtb res.dtb
#
# Uses QEMU's apb-pclk (phandle 0x8000, 24 MHz) and pl061 GPIO block
# (0x8005, at 0x9030000); the new phandles are 0x9001-0x9005.
set -e
[ $# -eq 2 ] || { echo "usage: $0 IN.dtb OUT.dtb" >&2; exit 2; }
add="python3 $(dirname "$0")/fdt-addprop.py"
t=$2.tmp
cp "$1" "$t"
p() { $add "$t" "$t" "$@"; }
for ph in 0x9001 0x9002 0x9003 0x9004 0x9005; do
	if python3 -c "import sys; b=open(sys.argv[1],'rb').read(); \
	    sys.exit(int(bytes.fromhex(sys.argv[2][2:].rjust(8,'0')) in b))" \
	    "$1" "$ph"; then :; else
		echo "phandle $ph may already be in $1" >&2
	fi
done

p /ffclk compatible s:fixed-factor-clock
p /ffclk '#clock-cells' 0
p /ffclk clocks 0x8000
p /ffclk clock-div 3
p /ffclk clock-mult 2
p /ffclk phandle 0x9001

p /reg3v3 compatible s:regulator-fixed
p /reg3v3 regulator-name s:vcc3v3
p /reg3v3 regulator-min-microvolt 3300000
p /reg3v3 regulator-max-microvolt 3300000
p /reg3v3 regulator-always-on
p /reg3v3 phandle 0x9002

p /reggpio compatible s:regulator-fixed
p /reggpio regulator-name s:vqmmc
p /reggpio gpio 0x8005 4 0
p /reggpio regulator-min-microvolt 1800000
p /reggpio regulator-max-microvolt 1800000
p /reggpio phandle 0x9003

p /rst compatible s:dfly,test-reset
p /rst '#reset-cells' 1
p /rst phandle 0x9004

p /syscon@9030000 compatible s:syscon
p /syscon@9030000 reg 0 0x9030000 0 0x1000
p /syscon@9030000 phandle 0x9005

p /fdtres-test clocks 0x8000 0x9001
p /fdtres-test clock-names s:apb s:scaled
p /fdtres-test resets 0x9004 7
p /fdtres-test reset-names s:core
p /fdtres-test vmmc-supply 0x9002
p /fdtres-test vqmmc-supply 0x9003
p /fdtres-test syscon 0x9005
mv "$t" "$2"
