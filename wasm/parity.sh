#!/bin/bash
# parity.sh — the native-vs-wasm bit-for-bit determinism suite.
#
# Builds wasm/parity_driver.cpp with the native toolchain and with Emscripten,
# runs both across a seed/config matrix, and byte-compares the final
# checkpoints. Any mismatch = the wasm build is NOT reproducing the native
# serial build and the port must not ship.
#
# Prereq: source ~/emsdk/emsdk_env.sh (or have em++ on PATH).
set -e
cd "$(dirname "$0")/.."

OUT=${PARITY_DIR:-/tmp/evo_parity}
mkdir -p "$OUT"

echo "== building parity drivers"
g++ -O3 -std=c++20 -ffp-contract=off -I src wasm/parity_driver.cpp -o "$OUT/parity_native"
em++ -O3 -std=c++20 -ffp-contract=off -I src wasm/parity_driver.cpp -o "$OUT/parity_wasm.js"

run_case () {  # run_case NAME SEED STEPS [extra args...]
    local name=$1 seed=$2 steps=$3; shift 3
    "$OUT/parity_native" "$seed" "$steps" "$@" > "$OUT/$name.native.json" 2> "$OUT/$name.native.log"
    node "$OUT/parity_wasm.js" "$seed" "$steps" "$@" > "$OUT/$name.wasm.json" 2> "$OUT/$name.wasm.log"
    if cmp -s "$OUT/$name.native.json" "$OUT/$name.wasm.json"; then
        echo "PASS  $name (seed $seed, $steps steps $*) $(grep -o '[0-9]* steps/s' "$OUT/$name.native.log" | head -1)"
    else
        echo "FAIL  $name (seed $seed, $steps steps $*)"
        echo "      first differing byte: $(cmp "$OUT/$name.native.json" "$OUT/$name.wasm.json" | head -1)"
        FAILED=1
    fi
}

FAILED=0
for s in 1 42 999 2718 31415; do
    run_case "default_s$s" "$s" 2000
done
run_case "rich_s42"    42    2000 --rich
run_case "rich_s31415" 31415 2000 --rich
run_case "tri_s42"     42    2000 --tri
run_case "long_s42"    42    10000

if [ "$FAILED" = "1" ]; then
    echo "== PARITY FAILED — bisect with --trace 10 on the failing case"
    exit 1
fi
echo "== ALL PARITY CASES PASS: native serial == wasm, bit for bit"
