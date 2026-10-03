#!/bin/sh
# Build one of the libc-free smoke tests (sigtest, ttytest) into a static
# aarch64 ELF, to be run as /sbin/init:  build-smoke.sh PROG OUT
# Uses the fork's headers through the kernel objdir's machine/ and cpu/
# links (bin/arm-kbuild must have configured tools/kobj/ARM64_VIRT).
D=$(cd "$(dirname "$0")/../.." && pwd)
SRC=$D/../dragonfly/sys
OBJ=$D/tools/kobj/ARM64_VIRT
PROG=${1:?usage: build-smoke.sh PROG OUT}
OUT=${2:-$PROG}
set -e
clang-18 --target=aarch64-unknown-dragonfly -O1 -ffreestanding -nostdinc \
    -fno-stack-protector -fno-builtin -I"$OBJ/include" -I"$SRC" -I"$OBJ" -idirafter "$SRC/sys" \
    -c "$D/tools/arm-smoke/$PROG.c" -o "$OUT.o"
"$D/tools/lld/usr/bin/ld.lld-18" -static -e _start "$OUT.o" -o "$OUT"
rm -f "$OUT.o"
