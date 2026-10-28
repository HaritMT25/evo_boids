#!/bin/bash
# ocean_parity.sh — the Ocean world's native-vs-wasm determinism suite
# (PLAN_ecosystem.md §12, E0 exit test: "Ocean: native == wasm fingerprints").
#
# Builds tools/ocean_parity_driver.cpp natively (g++, the flags of wasm/parity.sh)
# and with Emscripten (the flags of wasm/build_ocean.sh: -msimd128, single-
# threaded; run under node), runs both over the case matrix below and byte-
# compares their output: a fingerprint of the WHOLE state every 100 steps
# (tools/ocean_fingerprint.hpp). Any difference = the wasm build does not
# reproduce the native one. Then builds tests/ocean_tests.cpp both ways and
# runs it (unit checks; their output must also match between the builds).
# A case also FAILS if its state ever holds a non-finite float, even when both
# builds agree on it: wasm leaves a NaN's bits to the host CPU.
#
# Direct compiler calls only: CMakeLists.txt / CMakePresets.json (Classic's
# build) are untouched. Prereq: em++ on PATH, or ~/emsdk/emsdk_env.sh.
#
#   wasm/ocean_parity.sh              # everything (~2-3 min on 8 cores)
#   JOBS=8 OCEAN_PARITY_DIR=/tmp/x wasm/ocean_parity.sh
#   ONLY=def_s1 wasm/ocean_parity.sh  # one case (a grep -E pattern on the name)
#   NO_TESTS=1 wasm/ocean_parity.sh   # parity cases only
#
# Bisecting a FAIL: the table names the first differing fingerprint (step and
# sections). Re-run that case with --trace (every step) to find the first bad
# step S, then --dump-step S --dump FILE on both builds and diff the dumps.
set -u
cd "$(dirname "$0")/.."
T0=$(date +%s)

OUT=${OCEAN_PARITY_DIR:-/tmp/ocean_parity}
JOBS=${JOBS:-4}
mkdir -p "$OUT"
rm -f "$OUT"/*.txt "$OUT"/*.log "$OUT"/*.rc
if ! command -v em++ > /dev/null; then
    # shellcheck disable=SC1090
    source ~/emsdk/emsdk_env.sh > /dev/null 2>&1
fi
command -v em++ > /dev/null || { echo "em++ not found: source ~/emsdk/emsdk_env.sh" >&2; exit 2; }
NODE="node --max-old-space-size=6144"

CXXFLAGS="-O3 -std=c++20 -ffp-contract=off -DEVO_WORLD_OCEAN -I src"
# build_ocean.sh's codegen flags; node instead of web, real files (NODERAWFS)
# for --dump, EXIT_RUNTIME so stdout is flushed and main's status is the exit code.
EMFLAGS="-msimd128 -sALLOW_MEMORY_GROWTH=1 -sENVIRONMENT=node -sNODERAWFS=1 -sEXIT_RUNTIME=1"

echo "== tree: $(git rev-parse --abbrev-ref HEAD 2>/dev/null) $(git rev-parse --short HEAD 2>/dev/null)," \
     "$(git status --porcelain 2>/dev/null | wc -l) changed/untracked path(s)"
echo "== building (g++ $(g++ -dumpversion), $(em++ --version | head -1 | sed 's/.*) //'))"
build () {  # build LABEL CMD...
    local label=$1; shift
    if "$@" > "$OUT/build_$label.log" 2>&1; then echo "   built $label"; else echo "   BUILD FAILED: $label"; cat "$OUT/build_$label.log"; return 1; fi
}
build driver_native g++ $CXXFLAGS tools/ocean_parity_driver.cpp -o "$OUT/opd_native" & P1=$!
build driver_wasm   em++ $CXXFLAGS $EMFLAGS tools/ocean_parity_driver.cpp -o "$OUT/opd_wasm.js" & P2=$!
if [ -z "${NO_TESTS:-}" ]; then
    build tests_native g++ $CXXFLAGS tests/ocean_tests.cpp -o "$OUT/ocean_tests_native" & P3=$!
    build tests_wasm   em++ $CXXFLAGS $EMFLAGS tests/ocean_tests.cpp -o "$OUT/ocean_tests_wasm.js" & P4=$!
fi
BUILD_OK=1
for p in $P1 $P2 ${P3:-} ${P4:-}; do wait "$p" || BUILD_OK=0; done
[ "$BUILD_OK" = 1 ] || { echo "== BUILD FAILED"; exit 1; }

# ---- the case matrix ---------------------------------------------------------
# NAME | ARGS (driver arguments; cfg JSON written without spaces) | what it covers
NAMES=(); ARGS=(); WHY=()
add () { NAMES+=("$1"); ARGS+=("$2"); WHY+=("$3"); }
RICH='"currentStrength":80,"currentMode":0,"currentDrift":1,"foodPatches":5,"foodDrift":true,"predatorCount":4,"predatorEconomy":true,"moveCost":7e-5,"maxNeighbors":7,"geneCost":0.005'
add def_s1        '1 3000'                    "preview defaults (applyOceanDefaults + OceanConfig{})"
add def_s42       '42 3000'                   "defaults, another seed"
add def_s999      '999 3000'                  "defaults, another seed"
add def_s31415    '31415 3000'                "defaults, another seed"
add pred_s7       '7 3000 --cfg {"predatorCount":5}'  "Stage A predators: 3-D hunt, wander pitch draws"
add econ_s42      '42 3000 --cfg {"predatorCount":6,"predatorEconomy":true,"predatorMinPopulation":3,"predatorMaxPopulation":24,"predatorInheritance":true}' \
                  "Stage B economy + floor + cap + inheritance (bud depth draws)"
add rich_s5       "5 3000 --cfg {$RICH}"      "Classic's --rich set: eddies, patches+drift, economy, move/gene cost, topological 7"
add eddies_s11    '11 3000 --cfg {"currentStrength":120,"currentEddies":8,"foodDrift":true,"foodPatches":6,"foodDepthMode":1,"foodDepthSpread":20}' \
                  "strong eddies, food at per-patch depths"
add gyre_toro_s13 '13 3000 --cfg {"currentStrength":60,"currentMode":1,"currentGyreEps":0.4,"toroidal":true,"foodDrift":true}' \
                  "double gyre, toroidal senses"
add rich_toro_s7  "7 3000 --cfg {$RICH,\"toroidal\":true,\"foodDepthMode\":1}" "rich + toroidal + patch depths"
add x_steep_s3    '3 3000 --cfg {"pitchMax":4,"kBuoy":25,"wallBand":100,"vDrag":0,"foodDepthMode":1,"foodPatches":4,"foodDepthSpread":500}' \
                  "extreme: pitchMax 4, kBuoy 25, wallBand 100, no drag, spread 500"
add x_deep_s8     '8 3000 --depth 2000 --cfg {"pitchMax":0.01,"kBuoy":-5,"vDrag":200,"wallBand":0,"buoyMutation":0.5,"toroidal":true,"cHold":0.5,"cPress":0.3}' \
                  "extreme: D 2000, pitchMax 0.01, kBuoy -5, vDrag 200, no walls, depth costs"
add x_shallow_s4  '4 3000 --depth 10 --cfg {"pitchMax":1.5,"wallBand":5,"kBuoy":0,"foodDepthMode":1,"foodPatches":3,"foodDepthSpread":0}' \
                  "extreme: D 10 (the minimum), bands cover it all, spread 0"
add x_osc_s6      '6 3000 --cfg {"vDrag":300,"pitchMax":2,"kBuoy":-20,"wallBand":119,"buoyMutation":1}' \
                  "extreme: vDrag 300 (vz flips sign each step), wallBand ~D/2, buoyMutation 1"
add x_tri_body_s9 '9 3000 --cfg {"triangularMotion":true,"bodyModel":true,"reproMaxDistance":16,"reproSoftWidth":5,"pitchMax":1.2}' \
                  "triangular motion + body model + soft repro gate (kernel draws interleaved with bodyBuoy)"
add live_s21      '21 3000 --cfg {"predatorCount":3} --set pitchMax=0.2@800 --set kBuoy=3@1200 --set vDrag=0.5@1200 --set wallBand=40@2000 --set buoyMutation=0.3@2000' \
                  "live oc_set_param-style changes mid-run"
add capture_s17   '17 3000 --capture --cfg {"predatorCount":2,"currentStrength":50}' \
                  "every fish's 46 sensor + 13 action bits hashed every 100 steps"
add big_s42       '42 2000 --pop 1500'        "1500 founders, defaults"
add big_toro_s9   '9 2000 --pop 1500 --w 2560 --h 1440 --cfg {"toroidal":true,"predatorCount":8,"predatorEconomy":true,"maxNeighbors":7,"currentStrength":60,"foodPatches":6,"foodDrift":true,"foodDepthMode":1}' \
                  "1500 founders, 2560x1440, toroidal, economy, topological, currents"
add long_s42      '42 10000 --every 250'      "10k steps, defaults"

run_one () {  # run_one INDEX: native and wasm side by side
    local i=$1 name=${NAMES[$1]}
    local -a A
    read -r -a A <<< "${ARGS[$i]}"
    ( "$OUT/opd_native" "${A[@]}" > "$OUT/$name.native.txt" 2> "$OUT/$name.native.log"; echo $? > "$OUT/$name.native.rc" ) &
    ( $NODE "$OUT/opd_wasm.js" "${A[@]}" > "$OUT/$name.wasm.txt" 2> "$OUT/$name.wasm.log"; echo $? > "$OUT/$name.wasm.rc" ) &
    wait
}

SEL=()
for i in "${!NAMES[@]}"; do
    if [ -n "${ONLY:-}" ] && ! [[ ${NAMES[$i]} =~ ${ONLY} ]]; then continue; fi
    SEL+=("$i")
done
# Every case must be a world the page accepts: the real oc_init / oc_set_param
# validation (wasm/ocean_wasm.cpp, through ocean_tests --accepts) is asked first.
declare -A REFUSED=()
if [ -z "${NO_TESTS:-}" ]; then
    for i in "${SEL[@]}"; do
        read -r -a A <<< "${ARGS[$i]}"
        msg=$("$OUT/ocean_tests_native" --accepts "${A[@]}" 2>&1) || REFUSED[$i]=${msg:-refused}
    done
    echo "== shim validation (oc_init + oc_set_param): $(( ${#SEL[@]} - ${#REFUSED[@]} )) of ${#SEL[@]} case configs accepted"
fi
echo "== running ${#SEL[@]} case(s), $JOBS at a time (native and wasm side by side) in $OUT"
for i in "${SEL[@]}"; do
    while [ "$(jobs -rp | wc -l)" -ge "$JOBS" ]; do wait -n; done
    run_one "$i" &
done
wait

# Which sections differ on the first differing line (fingerprint fields are k=v).
first_diff () {
    local ln la lb
    ln=$(cmp "$1" "$2" 2>&1 | sed -n 's/.*line \([0-9]*\).*/\1/p')
    [ -n "$ln" ] || { echo "(no line info)"; return; }
    la=$(sed -n "${ln}p" "$1"); lb=$(sed -n "${ln}p" "$2")
    [ -n "$lb" ] || { echo "wasm output ends at line $ln"; return; }
    echo "line $ln, step $(echo "$la" | awk '{print $2}'): $(paste -d'\t' <(echo "$la" | tr ' ' '\n') <(echo "$lb" | tr ' ' '\n') |
        awk -F'\t' '$1 != $2 && $1 ~ /=/ {split($1, k, "="); printf "%s ", k[1]}')differ"
}
secs_of () { sed -n 's/^# [0-9]* steps, \([0-9.]*\) s.*/\1/p' "$1" 2>/dev/null; }

FAILED=0
printf '\n%-14s %6s %6s %5s %6s %5s %4s %7s %7s  %s\n' CASE SEED STEPS POP BIRTHS FOOD PRED NATIVE WASM RESULT
for i in "${SEL[@]}"; do
    name=${NAMES[$i]}
    read -r -a A <<< "${ARGS[$i]}"
    n="$OUT/$name.native.txt"; w="$OUT/$name.wasm.txt"
    fin=$(grep '^final ' "$n" 2>/dev/null)
    get () { echo "$fin" | tr ' ' '\n' | sed -n "s/^$1=//p"; }
    nfp=$(grep -c '^fp ' "$n" 2>/dev/null)
    rcn=$(cat "$OUT/$name.native.rc" 2>/dev/null); rcw=$(cat "$OUT/$name.wasm.rc" 2>/dev/null)
    if [ -n "${REFUSED[$i]:-}" ]; then
        res="FAIL  the page refuses this config: ${REFUSED[$i]}"
        FAILED=1
    elif [ "$rcn" != 0 ] || [ "$rcw" != 0 ]; then
        res="FAIL  exit native=$rcn wasm=$rcw: $(tail -1 "$OUT/$name.wasm.log" 2>/dev/null) $(tail -1 "$OUT/$name.native.log" 2>/dev/null)"
        FAILED=1
    elif [ -z "$fin" ]; then
        res="FAIL  no final line"; FAILED=1
    elif [ "$(get nonfinite_max)" != 0 ]; then
        # Both builds may agree here and still not be portable: wasm leaves a
        # NaN's sign/payload to the host (x86 0xffc00000, ARM 0x7fc00000).
        res="FAIL  non-finite state from step $(get nonfinite_from) (up to $(get nonfinite_max) values)"
        cmp -s "$n" "$w" && res="$res; native == wasm on this host only"
        FAILED=1
    elif cmp -s "$n" "$w"; then
        res="PASS  $nfp fingerprints identical"
    else
        res="FAIL  $(first_diff "$n" "$w")"; FAILED=1
    fi
    printf '%-14s %6s %6s %5s %6s %5s %4s %6ss %6ss  %s\n' "$name" "${A[0]}" "${A[1]}" "$(get pop)" "$(get births)" \
        "$(get food)" "$(get pred)" "$(secs_of "$OUT/$name.native.log")" "$(secs_of "$OUT/$name.wasm.log")" "$res"
done

# Same build, same arguments, twice: the native driver itself is reproducible.
if [ ${#SEL[@]} -gt 0 ]; then
    i=${SEL[0]}; name=${NAMES[$i]}
    read -r -a A <<< "${ARGS[$i]}"
    "$OUT/opd_native" "${A[@]}" > "$OUT/$name.native2.txt" 2> /dev/null
    if cmp -s "$OUT/$name.native.txt" "$OUT/$name.native2.txt"; then
        echo "repeat: $name native run twice -> identical"
    else
        echo "repeat: $name native run twice -> DIFFERENT"; FAILED=1
    fi
fi

TESTS_OK=1
if [ -z "${NO_TESTS:-}" ]; then
    echo
    echo "== unit checks (tests/ocean_tests.cpp), native"
    "$OUT/ocean_tests_native" > "$OUT/tests.native.txt" 2>&1; rct=$?
    cat "$OUT/tests.native.txt"
    echo "== unit checks, wasm (node)"
    $NODE "$OUT/ocean_tests_wasm.js" > "$OUT/tests.wasm.txt" 2>&1; rcw=$?
    tail -1 "$OUT/tests.wasm.txt"
    if [ $rct != 0 ] || [ $rcw != 0 ]; then TESTS_OK=0; echo "   unit checks FAILED (native rc $rct, wasm rc $rcw)"; fi
    if cmp -s "$OUT/tests.native.txt" "$OUT/tests.wasm.txt"; then
        echo "   unit-check output native == wasm (every printed value identical)"
    else
        echo "   unit-check output differs native vs wasm:"; diff "$OUT/tests.native.txt" "$OUT/tests.wasm.txt" | head -20; TESTS_OK=0
    fi
fi

echo
echo "== $(( $(date +%s) - T0 )) s total"
if [ "$FAILED" = 1 ] || [ "$TESTS_OK" = 0 ]; then
    [ "$FAILED" = 1 ] && echo "== OCEAN PARITY FAILED: bisect with --trace, then --dump-step/--dump on both builds"
    [ "$TESTS_OK" = 0 ] && echo "== OCEAN UNIT CHECKS FAILED"
    exit 1
fi
echo "== ALL OCEAN PARITY CASES PASS: native == wasm, bit for bit; unit checks pass in both builds"
