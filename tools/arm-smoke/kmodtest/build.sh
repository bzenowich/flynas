#!/bin/sh
# Build kmodtest.ko (an ET_REL kernel module, like bsd.kmod.mk makes) and
# its loader init: build.sh OUTDIR.  Needs a configured
# tools/kobj/ARM64_VIRT (bin/arm-kbuild).
D=$(cd "$(dirname "$0")/../../.." && pwd)
SRC=$D/../dragonfly/sys
OBJ=$D/tools/kobj/ARM64_VIRT
LD=$D/tools/lld/usr/bin/ld.lld-18
OUT=${1:-.}
H=$(dirname "$0")
set -e
CF="--target=aarch64-unknown-dragonfly -O2 -std=gnu11 -nostdinc -I$OBJ/include
    -I$SRC -I$OBJ -D_KERNEL -DKLD_MODULE -fno-common -ffreestanding
    -fno-stack-protector -fno-strict-aliasing -fno-asynchronous-unwind-tables
    -fno-omit-frame-pointer -mgeneral-regs-only -ffixed-x18
    -mno-outline-atomics -fno-pic -Wall"
clang-18 $CF -c "$H/kmt_small.c" -o "$OUT/kmt_small.o"
clang-18 $CF -mcmodel=large -c "$H/kmt_large.c" -o "$OUT/kmt_large.o"
"$LD" -r -d "$OUT/kmt_small.o" "$OUT/kmt_large.o" -o "$OUT/kmodtest.ko"
rm -f "$OUT/kmt_small.o" "$OUT/kmt_large.o"
clang-18 --target=aarch64-unknown-dragonfly -O1 -ffreestanding -nostdinc \
    -fno-stack-protector -fno-builtin -I"$OBJ/include" -I"$SRC" -I"$OBJ" \
    -idirafter "$SRC/sys" -c "$H/kldinit.c" -o "$OUT/kldinit.o"
"$LD" -static -e _start "$OUT/kldinit.o" -o "$OUT/kldinit"
rm -f "$OUT/kldinit.o"
