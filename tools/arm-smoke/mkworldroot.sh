#!/bin/sh
# Build a world md root in WORK/worldroot and WORK/world.iso from what the
# world build installed into tools/sysroot: /bin, /sbin, /lib, /libexec,
# /usr/{bin,sbin,libexec}, the shared libraries of /usr/lib (and priv/),
# plus dltest and the load test (load.mk, job.sh in /root).
#   mkworldroot.sh WORK
# arm-mkiso writes plain ISO 9660: symlinks are copied as files, and names
# outside [a-z0-9_.-] (upper case, '+', '[') are left out.
D=$(cd "$(dirname "$0")/../.." && pwd)
W=${1:?usage: mkworldroot.sh WORK}
SYS="$D/tools/sysroot"
T="$D/tools/arm-smoke"
R="$W/worldroot"
set -e
rm -rf "$R"
mkdir -p "$R/usr/lib/priv" "$R/dev" "$R/etc" "$R/tmp" "$R/root" "$R/var/run" \
    "$R/var/tmp"
for d in bin sbin lib libexec usr/bin usr/sbin usr/libexec; do
	mkdir -p "$R/$d"
	cp -rL "$SYS/$d/." "$R/$d/"
done
cp -L "$SYS"/usr/lib/*.so* "$R/usr/lib/"
cp -L "$SYS"/usr/lib/priv/*.so* "$R/usr/lib/priv/"
chmod -R u+w "$R"
find "$R" -name '*.a' -delete
find "$R" -mindepth 1 | awk -F/ '$NF !~ /^[a-z0-9_.-]+$/' |
    while read -r f; do rm -rf "$f"; done
# Strip what is ELF; leave scripts alone.
find "$R" -type f | while read -r f; do
	[ "$(head -c 4 "$f" | od -An -c | tr -d ' ')" = "177ELF" ] &&
	    llvm-strip-18 "$f" 2>/dev/null || true
done
sh "$T/build-dltest.sh" "$R"
cp "$T/load.mk" "$T/job.sh" "$R/root/"
"$D/bin/arm-mkiso" "$W/world.iso" "$R" >/dev/null
echo "world root: $(du -sh "$R" | cut -f1), $(find "$R" -type f | wc -l) files, $(du -h "$W/world.iso" | cut -f1) iso"
