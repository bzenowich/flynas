#!/bin/sh
# Build a test program from tools/arm-smoke/PROG.c against the
# cross-built libraries in tools/sysroot (bin/arm-world lib/libc,
# lib/csu/aarch64, lib/libcompiler_rt, and for -lm lib/libm installed):
#   build-utest.sh PROG OUT [cc args...]
#   build-utest.sh libctest OUT
#   build-utest.sh libmtest OUT -lm
# Static by default; LINK= (empty) links it dynamically against libc.so
# (needs ARM_SHARED=1 bin/arm-world lib/libc and libexec/rtld-elf).
D=$(cd "$(dirname "$0")/../.." && pwd)
PROG=${1:?usage: build-utest.sh PROG OUT [cc args...]}
OUT=${2:?usage: build-utest.sh PROG OUT [cc args...]}
shift 2
exec "$D/tools/host/ubin/cc" ${LINK--static} -O1 -Wall -o "$OUT" "$D/tools/arm-smoke/$PROG.c" "$@"
