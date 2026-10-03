#!/bin/sh
# Boot test of rtld and the shared libraries: builds a root in WORK with
# the static /sbin/init and /bin/sh (from bin/arm-world's objdirs), rtld,
# libc.so and libm.so from tools/sysroot, and dynamic dltest, libctest and
# libmtest, then runs dyn.exp against it on QEMU.
#   run-dyn.sh WORK [arm-vm args...]
# Needs: bin/arm-world bin/sh obj all BOOTSTRAPPING=1, sbin/init obj all,
# and with ARM_SHARED=1: lib/libc, lib/libm, libexec/rtld-elf.
D=$(cd "$(dirname "$0")/../.." && pwd)
W=${1:?usage: run-dyn.sh WORK [arm-vm args...]}
shift
SRC="$(cd "${DFLY_SRC:-$D/../dragonfly}" && pwd)"
OBJ="$D/tools/uobj$SRC"
SYS="$D/tools/sysroot"
T="$D/tools/arm-smoke"
set -e
R="$W/dynroot"
rm -rf "$R"
mkdir -p "$R/bin" "$R/sbin" "$R/lib" "$R/libexec" "$R/usr/libexec" \
    "$R/dev" "$R/etc" "$R/tmp"
llvm-strip-18 -o "$R/sbin/init" "$OBJ/sbin/init/init"
llvm-strip-18 -o "$R/bin/sh" "$OBJ/bin/sh/sh"
cp "$SYS/lib/libc.so.8" "$SYS/lib/libm.so.4" "$R/lib/"
cp "$SYS/libexec/ld-elf.so.2" "$R/libexec/"
cp "$SYS/libexec/ld-elf.so.2" "$R/usr/libexec/"
LINK= sh "$T/build-utest.sh" libctest "$R/bin/libctest"
LINK= sh "$T/build-utest.sh" libmtest "$R/bin/libmtest" -lm
sh "$T/build-dltest.sh" "$R"
"$D/bin/arm-mkiso" "$W/dyn.iso" "$R" >/dev/null
exec "$T/vmexpect.py" -w 300 "$T/dyn.exp" -- -s 2 -r "$W/dyn.iso" \
    -a "vfs.root.mountfrom=cd9660:md0" "$@"
