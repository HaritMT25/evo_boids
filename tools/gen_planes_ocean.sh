#!/bin/bash
# gen_planes_ocean.sh — src/ocean/plane_data_ocean.hpp, the Ocean build's frozen
# colour / SimHash planes.
#
# Same generator and seeds as the Classic planes (tools/gen_planes.cpp: colour
# 0x5EEDC010, SimHash 0xC0FFEE, native gcc/libstdc++ normal_distribution), only
# compiled with -DEVO_WORLD_OCEAN, so STATS_DIM is the Ocean one
# ((46 + 12) x (13 + 12) = 1450 instead of Classic's 1272). The output's include
# is re-pointed at src/core, since the file lives in src/ocean.
#
#   bash tools/gen_planes_ocean.sh        (from anywhere; native g++ required)
set -e
cd "$(dirname "$0")/.."
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
g++ -O2 -std=c++20 -DEVO_WORLD_OCEAN -I src tools/gen_planes.cpp -o "$tmp/gen_planes_ocean"
{
    echo "// Ocean build (-DEVO_WORLD_OCEAN): regenerate with tools/gen_planes_ocean.sh."
    "$tmp/gen_planes_ocean" \
        | sed -e 's#"connectivity.hpp"#"../core/connectivity.hpp"#' \
              -e 's#regenerate plane_data.hpp #regenerate src/ocean/plane_data_ocean.hpp #'
} > src/ocean/plane_data_ocean.hpp
grep -m1 -o 'STATS_DIM == [0-9]*' src/ocean/plane_data_ocean.hpp
