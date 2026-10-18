#!/bin/bash
# build_wasm.sh — build both browser modules into web/ (thin wrapper over the
# CMake presets in CMakePresets.json; wasm/wasm.cmake holds the flags):
#
#   web/evo_boids.{js,wasm}      single-threaded, deterministic (parity-tested)
#   web/evo_boids_mt.{js,wasm}   pthreads, needs a cross-origin-isolated page
#
# Prereq: source ~/emsdk/emsdk_env.sh (sets EMSDK, which the presets need).
set -e
cd "$(dirname "$0")/.."
# The presets find the toolchain file through $EMSDK. Derive it from emcc on
# the PATH when it is not exported (emcc lives at $EMSDK/upstream/emscripten).
if [ -z "$EMSDK" ] && command -v emcc > /dev/null; then
    EMSDK=$(cd "$(dirname "$(command -v emcc)")/../.." && pwd)
    export EMSDK
fi
[ -n "$EMSDK" ] || { echo "EMSDK not set: source ~/emsdk/emsdk_env.sh first" >&2; exit 1; }
for p in wasm-st wasm-mt; do
    cmake --preset "$p" > /dev/null
    cmake --build --preset "$p"
done
ls -la web/evo_boids.js web/evo_boids.wasm web/evo_boids_mt.js web/evo_boids_mt.wasm
