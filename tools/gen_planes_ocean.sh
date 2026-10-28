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
#   EYE=1 bash tools/gen_planes_ocean.sh  the eye variant (-DEVO_OCEAN_EYE: + 18 eye
#                                         sensors, (64 + 12) x 25 = 1900) ->
#                                         src/ocean/plane_data_ocean_eye.hpp
set -e
cd "$(dirname "$0")/.."
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
if [ "${EYE:-0}" = 1 ]; then
    DEFS="-DEVO_WORLD_OCEAN -DEVO_OCEAN_EYE"; OUTF=src/ocean/plane_data_ocean_eye.hpp
    WHAT="Ocean eye build (-DEVO_WORLD_OCEAN -DEVO_OCEAN_EYE): regenerate with EYE=1 tools/gen_planes_ocean.sh."
else
    DEFS="-DEVO_WORLD_OCEAN"; OUTF=src/ocean/plane_data_ocean.hpp
    WHAT="Ocean build (-DEVO_WORLD_OCEAN): regenerate with tools/gen_planes_ocean.sh."
fi
g++ -O2 -std=c++20 $DEFS -I src tools/gen_planes.cpp -o "$tmp/gen_planes_ocean"
{
    echo "// $WHAT"
    "$tmp/gen_planes_ocean" \
        | sed -e 's#"connectivity.hpp"#"../core/connectivity.hpp"#' \
              -e "s#regenerate plane_data.hpp #regenerate $OUTF #"
} > $OUTF
grep -m1 -o 'STATS_DIM == [0-9]*' $OUTF
