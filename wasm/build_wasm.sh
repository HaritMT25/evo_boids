#!/bin/bash
# build_wasm.sh — build the browser module web/evo_boids.{js,wasm}.
#
# Prereq: source ~/emsdk/emsdk_env.sh (or have em++ on PATH).
#
# Flags that are part of the determinism contract (do not relax):
#   -ffp-contract=off   no FMA contraction — same requirement as the native
#                       builds; without it feedForward's flattening equivalence
#                       and native<->wasm parity both break
#   no -ffast-math      ever
#   single-threaded     no USE_OPENMP, no pthreads: matches the serial
#                       reference build AND avoids the COOP/COEP headers that
#                       SharedArrayBuffer would force on the embedding page
set -e
cd "$(dirname "$0")/.."

VERSION=$(sed -n 's/^project(evo_boids VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt)

em++ -O3 -std=c++20 -ffp-contract=off \
    -I src \
    -DEVO_VERSION="\"$VERSION\"" \
    wasm/evo_wasm.cpp \
    -o web/evo_boids.js \
    --no-entry \
    -sMODULARIZE=1 \
    -sEXPORT_NAME=createEvoBoids \
    -sENVIRONMENT=web,worker,node \
    -sALLOW_MEMORY_GROWTH=1 \
    -sEXPORTED_FUNCTIONS=_malloc,_free \
    -sEXPORTED_RUNTIME_METHODS=ccall,cwrap,UTF8ToString,stringToUTF8,lengthBytesUTF8,HEAP8,HEAPU8,HEAP32,HEAPF32,HEAPF64

ls -la web/evo_boids.js web/evo_boids.wasm
echo "built evo_boids $VERSION"
