#!/bin/sh
# Build libctest.c against the cross-built libc in tools/sysroot
# (bin/arm-world lib/libc and lib/csu/aarch64 installed):
#   build-libctest.sh OUT
D=$(cd "$(dirname "$0")/../.." && pwd)
OUT=${1:?usage: build-libctest.sh OUT}
exec "$D/tools/host/ubin/cc" -static -O1 -Wall -o "$OUT" "$D/tools/arm-smoke/libctest.c"
