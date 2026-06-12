#!/bin/sh
# Build the FlyNAS Clay UI to WASM and stage it into the overlay.
# Cross-compiled on Linux (see challenges.md §2) — the NAS never
# needs a WASM toolchain.
#
# Requires: clang with wasm32 target, wasm-ld (extracted lld in
# ../tools/lld if not installed system-wide).

set -e

ROOT="$(cd "$(dirname "$0")" && pwd)"
STATIC="$ROOT/../overlay/usr/local/flynas/static"

# Prefer system wasm-ld, fall back to the locally extracted lld
if ! command -v wasm-ld >/dev/null 2>&1; then
    export PATH="$ROOT/../tools/lld/usr/bin:$ROOT/../tools/lld/usr/lib/llvm-18/bin:$PATH"
fi

mkdir -p "$STATIC"

clang \
    -Wall \
    -Os \
    -DCLAY_WASM \
    -mbulk-memory \
    --target=wasm32 \
    -nostdlib \
    -Wl,--strip-all \
    -Wl,--export-dynamic \
    -Wl,--no-entry \
    -Wl,--export=__heap_base \
    -Wl,--export=ACTIVE_RENDERER_INDEX \
    -Wl,--initial-memory=6553600 \
    -o "$STATIC/clay.wasm" \
    "$ROOT/main.c"

cp "$ROOT/index.html" "$STATIC/index.html"
cp "$ROOT/qrcode.js" "$STATIC/qrcode.js"

echo "built: $STATIC/clay.wasm ($(stat -c %s "$STATIC/clay.wasm") bytes)"
