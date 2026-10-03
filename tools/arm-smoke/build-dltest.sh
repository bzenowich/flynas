#!/bin/sh
# Build the rtld smoke test (dltest.c, dltestlib.c) against the cross-built
# libc.so in tools/sysroot (ARM_SHARED=1 bin/arm-world lib/libc,
# libexec/rtld-elf): ROOT/bin/dltest, ROOT/lib/libdla.so, ROOT/lib/libdlb.so.
#   build-dltest.sh ROOT
D=$(cd "$(dirname "$0")/../.." && pwd)
R=${1:?usage: build-dltest.sh ROOT}
CC="$D/tools/host/ubin/cc"
T="$D/tools/arm-smoke"
set -e
mkdir -p "$R/bin" "$R/lib"
for n in a b; do
	"$CC" -O1 -Wall -fpic -shared -DNAME=$n -Wl,-soname,libdl$n.so \
	    -o "$R/lib/libdl$n.so" "$T/dltestlib.c"
done
"$CC" -O1 -Wall -o "$R/bin/dltest" "$T/dltest.c" -L"$R/lib" -ldla
