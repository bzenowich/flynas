#!/bin/sh
# Symbolize the backtrace in an aarch64 kernel panic log (print_backtrace()
# lines "#N <addr>" and "--- exception ... at <addr>").
#   ksym.sh LOG [KERNCONF]
D=$(cd "$(dirname "$0")/../.." && pwd)
LOG=${1:?usage: ksym.sh LOG [KERNCONF]}
K="$D/tools/kobj/${2:-ARM64_VIRT}/kernel.debug"
tr -d '\r' < "$LOG" | grep -E '^  (#[0-9]+ +[0-9a-f]{16}|--- exception .* at [0-9a-f]{16})' |
while read -r l; do
    a=$(echo "$l" | grep -oE '[0-9a-f]{16}$')
    printf '%s\n' "$l"
    llvm-symbolizer-18 -e "$K" -f -i -s "0x$a" | paste - - | sed '/^[[:space:]]*$/d; s/^/        /'
done
