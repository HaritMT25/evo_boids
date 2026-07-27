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
run_case "toro_s42"    42    2000 --toroidal
run_case "toro_rich_s7" 7    2000 --rich --toroidal
# Genetic reproduction gate (isolation.hpp). Both thresholds are chosen to
# actually BITE — in-run neighbour pairs sit ~18-20 apart, so 18 blocks roughly
# half of attempted births and 14 blocks nearly all of them. A threshold above
# ~23 would leave the gate inert and prove nothing about the new code path.
run_case "iso_s42"     42    2000 --repro-max-distance 18
run_case "iso_rich_s999" 999 2000 --rich --repro-max-distance 14
# Tier-1 body model (body_model.hpp). This is a DYNAMICS knob that also ADDS TO
# THE RANDOM STREAM (two rng.range draws per insertion), so the on-path needs
# native==wasm proven independently of the off-path: a draw-count or draw-order
# difference between the two builds would show up here and nowhere else.
# body_s42 is the case with live morphology at the end (192 insertions, bodySize
# spread 0.86-1.14 at step 2000). body_rich_s7 is the harsher one: --rich at 2000
# steps collapses to the --min-pop floor under predation in EVERY seed (that is
# pre-existing, not a body-model effect — plain --rich s7/s42 do it too), so its
# survivors are reseeded founders back at 1.0. It still earns its place: 23
# insertions happen along the way (peak bodySize spread 0.92-1.09 around step
# 1000), and it exercises the body factors against predators/currents/patches
# and through the reseed path, where founders must draw NOTHING.
run_case "body_s42"      42   2000 --body-model
run_case "body_rich_s7"  7    2000 --rich --body-model
# body_tri_s42 exists because the other two body cases cannot see turnFactor at
# all. turnFactor is the ONLY dynamical consumer of bodyElong, it multiplies
# cfg.maxTurnRate, and cfg.maxTurnRate is read on the triangular-motion branch
# only — so with --tri off, bodyElong is decorative and a turn factor wired to
# the wrong parameter (or deleted outright) ships green through every other
# case here. Measured at step 2000: 193 insertions, 471 alive, 186 of them off
# the default body, bodyElong spread 0.856-1.138 — so the elongation term is
# live and varying across the population when the compare happens.
run_case "body_tri_s42"  42   2000 --tri --body-model
run_case "long_s42"    42    10000

if [ "$FAILED" = "1" ]; then
    echo "== PARITY FAILED — bisect with --trace 10 on the failing case"
    exit 1
fi
echo "== ALL PARITY CASES PASS: native serial == wasm, bit for bit"
