#!/bin/bash
# build_ocean.sh — the Ocean preview's browser module, web/evo_ocean.{js,wasm}
# (wasm/ocean_wasm.cpp, factory createEvoOcean, single-threaded).
#
# A direct em++ call, not a CMake preset, so Classic's build (wasm/wasm.cmake,
# CMakeLists.txt, CMakePresets.json) stays untouched. Flags are wasm.cmake's
# (-O3 -std=c++20 -ffp-contract=off -msimd128, the same module options) plus
# -DEVO_WORLD_OCEAN, which selects the Ocean sensor/action list and planes.
#
# Prereq: source ~/emsdk/emsdk_env.sh
set -e
cd "$(dirname "$0")/.."
command -v em++ > /dev/null || { echo "em++ not on PATH: source ~/emsdk/emsdk_env.sh first" >&2; exit 1; }
em++ -O3 -std=c++20 -ffp-contract=off -msimd128 -DEVO_WORLD_OCEAN \
    -DEVO_VERSION='"ocean-preview"' -I src wasm/ocean_wasm.cpp -o web/evo_ocean.js \
    --no-entry \
    -sMODULARIZE=1 \
    -sEXPORT_NAME=createEvoOcean \
    -sENVIRONMENT=web,worker,node \
    -sALLOW_MEMORY_GROWTH=1 \
    -sEXPORTED_FUNCTIONS=_malloc,_free \
    -sEXPORTED_RUNTIME_METHODS=ccall,cwrap,UTF8ToString,stringToUTF8,lengthBytesUTF8,HEAP8,HEAPU8,HEAP32,HEAPF32,HEAPF64,wasmMemory \
    -sINCOMING_MODULE_JS_API=locateFile,mainScriptUrlOrBlob,print,printErr,onAbort
ls -la web/evo_ocean.js web/evo_ocean.wasm
