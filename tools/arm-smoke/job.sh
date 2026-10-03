#!/bin/sh
# job.sh N R: one load.mk job.  Generate a pseudo-random data set with awk,
# sort it, compress and decompress it, and check every step; print JOBFAIL
# on any mismatch.  Runs in /tmp (tmpfs).
d=/tmp/j$1
mkdir -p $d || exit 1
fail() { echo "JOB""FAIL $1 round $2: $3"; exit 1; }
awk -v s="$1$2" 'BEGIN { srand(s); for (i = 0; i < 20000; i++)
    printf "%08d %s\n", int(rand() * 100000000),
        substr("abcdefghijklmnopqrstuvwxyz", 1 + int(rand() * 26)) }' > $d/in
sort $d/in > $d/s
sort -c $d/s || fail $1 $2 "sort -c"
gzip -c $d/s > $d/s.gz
gunzip -c $d/s.gz | cmp -s - $d/s || fail $1 $2 "gzip round trip"
n=$(wc -l < $d/s)
[ $n -eq 20000 ] || fail $1 $2 "wc $n"
[ "$(sort -u $d/in | sort -m - $d/s | uniq | wc -l)" -eq "$(sort -u $d/in | wc -l)" ] ||
    fail $1 $2 "sort -m/uniq"
rm -rf $d
