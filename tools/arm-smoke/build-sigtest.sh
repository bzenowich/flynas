#!/bin/sh
# Build sigtest.c into a static aarch64 ELF: build-sigtest.sh OUT
# Uses the fork's headers through the kernel objdir's machine/ and cpu/
# links (bin/arm-kbuild must have configured tools/kobj/ARM64_VIRT).
D=$(cd "$(dirname "$0")/../.." && pwd)
SRC=$D/../dragonfly/sys
OBJ=$D/tools/kobj/ARM64_VIRT
set -e
clang-18 --target=aarch64-unknown-dragonfly -O1 -ffreestanding -nostdinc \
    -fno-stack-protector -fno-builtin -I"$OBJ/include" -I"$SRC" -I"$OBJ" -idirafter "$SRC/sys" \
    -c "$D/tools/arm-smoke/sigtest.c" -o "${1:-sigtest}.o"
"$D/tools/lld/usr/bin/ld.lld-18" -static -e _start "${1:-sigtest}.o" -o "${1:-sigtest}"
rm -f "${1:-sigtest}.o"
