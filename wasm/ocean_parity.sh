#!/bin/bash
# ocean_parity.sh — the Ocean world's determinism suite (PLAN_ecosystem.md §12,
# E0 exit test: "Ocean: native == wasm fingerprints"), with a historical oracle.
#
# Builds tools/ocean_parity_driver.cpp natively (g++, the flags of wasm/parity.sh)
# and with Emscripten (the flags of wasm/build_ocean.sh: -msimd128, single-
# threaded; run under node), runs both over the case matrix below and holds
# every case to three oracles:
#   1. native == wasm, byte for byte. The driver fingerprints the WHOLE state
#      (tools/ocean_fingerprint.hpp) after EVERY step, folds it into a rolling
#      `chain` hash, and prints the sections and the chain every 100 steps
#      (250 in the long cases): a divergence at any step fails, even one that
#      heals before the next print.
#   2. native == the committed baseline, tests/ocean_baseline/<case>.txt.
#      Native and wasm are built from the SAME source, so a change to shared
#      behaviour (a cost, the rng stream, a feature-OFF path Classic shares)
#      passes oracle 1; the baseline, recorded from a known engine commit
#      (tests/ocean_baseline/MANIFEST), is what catches it.
#   3. the case's --expect conditions on the driver's coverage counters: it
#      really reached the branch it is in the matrix for (a predator died of
#      old age, the population sat at maxPopulation, the logs decimated, ...).
# A case also FAILS if its state ever holds a non-finite float (scanned every
# step), even when both builds agree on it: wasm leaves a NaN's bits to the host.
# Then builds tests/ocean_tests.cpp both ways and runs it (unit checks; their
# output must also match between the builds).
#
# E1 physics: the cases before the e1_* block run with every E1 knob at its
# default (OFF); the e1_* cases turn the features on (so do steer_life_e1_s5
# and eye_e1_*). The ocean-vision keys (steerFrame, foodLife, eye, eyeOnly,
# eyeCode) are at their defaults (0) in every case before the steer_* block. The r_* list holds
# configs outside the Ocean knobs' ranges (oceanConfigError) and the Ocean's
# ranges for SimConfig keys (oceanSimConfigError): each must be REFUSED by
# oc_init / oc_set_param. The harness's copy of the E1 preset is checked against
# kE1PresetJson (src/ocean/ocean_config.hpp).
#
# Direct compiler calls only: CMakeLists.txt / CMakePresets.json (Classic's
# build) are untouched. Prereq: em++ on PATH, or ~/emsdk/emsdk_env.sh.
#
#   wasm/ocean_parity.sh              # everything (~8 min on 8 cores, both builds)
#   JOBS=8 OCEAN_PARITY_DIR=/tmp/x wasm/ocean_parity.sh
#   ONLY=def_s1 wasm/ocean_parity.sh  # one case (a grep -E pattern on the name)
#   NO_TESTS=1 wasm/ocean_parity.sh   # parity cases only
#   NO_EYE=1 wasm/ocean_parity.sh     # the plain Ocean build only
#
# Two builds: the plain Ocean build (-DEVO_WORLD_OCEAN; every case not named
# eye_*) and the compound-eye build (+ -DEVO_OCEAN_EYE: 64 sensors, its own
# planes and genetics; the eye_* cases, eye on unless the case says otherwise;
# eye_only_* / eye_blind_* = OceanConfig::eyeOnly with the eye on / off;
# eye_code* = OceanConfig::eyeCode, the eye's encodings; eye_cloak_* = its
# cloaks, eyeCode 8 / 16: the eye ignores fish / food; eye_e1_* = with E1
# physics on). Each build's native and wasm binaries are compared with each
# other and with the baselines, each build's unit checks run natively and
# under node, and the r_eye_* refusals are asked of the eye build's shim.
#
# Bisecting a FAIL: the table names the first differing line (step and
# sections; `chain` alone = the states differed at some step since the last
# print and agree again). Re-run the case with --trace (every step) to find the
# first bad step S, then --dump-step S --dump FILE on both builds (for a
# baseline FAIL: on the old and the new engine) and diff the dumps.
#
# ---- Rebaselining -------------------------------------------------------------
#   wasm/ocean_parity.sh --rebaseline
#       runs everything and REPORTS which baselines differ and where; writes nothing
#   wasm/ocean_parity.sh --rebaseline --overwrite --reason "what changed, and why"
#       writes them, only if every selected case passes oracles 1 and 3, the
#       unit checks pass and no config guard failed (so not with NO_TESTS;
#       with ONLY the refused-config guard still runs); records the commit,
#       date, compilers, reason and cases in tests/ocean_baseline/MANIFEST.
#       Every write is checked (staged in tests/ocean_baseline/.rebaseline.*,
#       then moved into place): any failure exits 1 and says what was written
# Rebaselining is legitimate ONLY when behaviour is MEANT to change: a
# deliberate engine change (a new default path, a fix that moves a trajectory,
# a draw added by design), a new or edited case, or a change to what the
# harness hashes (bump ofp::kFormatVersion). It is NEVER legitimate to make a
# failure go away: a baseline FAIL nobody intended is a regression to find
# (--trace, --dump), not to re-record. If you cannot say which change moved
# which case and why, do not rebaseline. Commit the new baselines WITH the
# change that moved them, after reading `git diff tests/ocean_baseline`.
set -u
REBASE=0; OVERWRITE=0; REASON=""
while [ $# -gt 0 ]; do
    case "$1" in
        --rebaseline) REBASE=1 ;;
        --overwrite)  OVERWRITE=1 ;;
        --reason)     shift; REASON=${1:-} ;;
        -h|--help)    sed -n '2,/^set -u$/p' "$0" | sed '$d' | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown argument: $1 (see --help)" >&2; exit 2 ;;
    esac
    shift
done
if [ "$OVERWRITE" = 1 ] && [ "$REBASE" = 0 ]; then echo "--overwrite goes only with --rebaseline" >&2; exit 2; fi
if [ "$OVERWRITE" = 1 ] && [ -z "$REASON" ]; then
    echo "--overwrite needs --reason \"which behaviour changed, on purpose, and why\"" >&2; exit 2
fi
# A baseline is written only after every check: NO_TESTS skips the unit checks
# and the shim validation (and, with them, the refused-config guard), so it
# cannot go with --overwrite. (ONLY may: the guards below still run then.)
if [ "$OVERWRITE" = 1 ] && [ -n "${NO_TESTS:-}" ]; then
    echo "--overwrite cannot go with NO_TESTS: the unit checks and the shim / config guards must run before a baseline is written" >&2
    exit 2
fi
cd "$(dirname "$0")/.."
T0=$(date +%s)
BASE=tests/ocean_baseline

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

GPP_VER=$(g++ -dumpversion); EM_VER=$(em++ --version | head -1 | sed 's/.*) //')
echo "== tree: $(git rev-parse --abbrev-ref HEAD 2>/dev/null) $(git rev-parse --short HEAD 2>/dev/null)," \
     "$(git status --porcelain 2>/dev/null | wc -l) changed/untracked path(s)"
echo "== building (g++ $GPP_VER, $EM_VER)"
build () {  # build LABEL CMD...
    local label=$1; shift
    if "$@" > "$OUT/build_$label.log" 2>&1; then echo "   built $label"; else echo "   BUILD FAILED: $label"; cat "$OUT/build_$label.log"; return 1; fi
}
VARIANTS="plain"; [ -z "${NO_EYE:-}" ] && VARIANTS="plain eye"
PIDS=()
for v in $VARIANTS; do
    # binaries: plain = opd_native / opd_wasm.js / ocean_tests_*; eye = *_eye
    if [ $v = eye ]; then F="$CXXFLAGS -DEVO_OCEAN_EYE"; S=_eye; else F="$CXXFLAGS"; S=; fi
    build driver_native$S g++ $F tools/ocean_parity_driver.cpp -o "$OUT/opd_native$S" & PIDS+=($!)
    build driver_wasm$S   em++ $F $EMFLAGS tools/ocean_parity_driver.cpp -o "$OUT/opd_wasm$S.js" & PIDS+=($!)
    if [ -z "${NO_TESTS:-}" ]; then
        build tests_native$S g++ $F tests/ocean_tests.cpp -o "$OUT/ocean_tests_native$S" & PIDS+=($!)
        build tests_wasm$S   em++ $F $EMFLAGS tests/ocean_tests.cpp -o "$OUT/ocean_tests_wasm$S.js" & PIDS+=($!)
    fi
done
BUILD_OK=1
for p in "${PIDS[@]}"; do wait "$p" || BUILD_OK=0; done
[ "$BUILD_OK" = 1 ] || { echo "== BUILD FAILED"; exit 1; }
sfx () { [[ $1 == eye_* ]] && echo _eye; }   # sfx CASE_NAME -> the build's binary suffix

# ---- the case matrix ---------------------------------------------------------
# NAME | ARGS (driver arguments; cfg JSON written without spaces; --expect
# NAMEopV conditions on the driver's `cover` counters) | what it covers
NAMES=(); ARGS=(); WHY=()
add () { NAMES+=("$1"); ARGS+=("$2"); WHY+=("$3"); }
BANDS='--expect bandTop>0 --expect bandFloor>0'
RICH='"currentStrength":80,"currentMode":0,"currentDrift":1,"foodPatches":5,"foodDrift":true,"predatorCount":4,"predatorEconomy":true,"moveCost":7e-5,"maxNeighbors":7,"geneCost":0.005'
add def_s1        "1 3000 $BANDS"             "preview defaults (applyOceanDefaults + OceanConfig{}); both wall bands hit"
add def_s42       '42 3000'                   "defaults, another seed"
add def_s999      '999 3000'                  "defaults, another seed"
add def_s31415    '31415 3000'                "defaults, another seed"
add pred_s7       '7 3000 --cfg {"predatorCount":5} --expect dPred>0'  "Stage A predators: 3-D hunt, wander pitch draws"
add econ_s42      '42 3000 --cfg {"predatorCount":6,"predatorEconomy":true,"predatorMinPopulation":3,"predatorMaxPopulation":24,"predatorInheritance":true} --expect pBirths>0 --expect predTraitVaried>0' \
                  "Stage B economy + floor + cap + inheritance (bud depth draws)"
add rich_s5       "5 3000 --cfg {$RICH}"      "Classic's --rich set: eddies, patches+drift, economy, move/gene cost, topological 7"
add eddies_s11    '11 3000 --cfg {"currentStrength":120,"currentEddies":8,"foodDrift":true,"foodPatches":6,"foodDepthMode":1,"foodDepthSpread":20}' \
                  "strong eddies, food at per-patch depths"
add gyre_toro_s13 '13 3000 --cfg {"currentStrength":60,"currentMode":1,"currentGyreEps":0.4,"toroidal":true,"foodDrift":true}' \
                  "double gyre, toroidal senses"
add rich_toro_s7  "7 3000 --cfg {$RICH,\"toroidal\":true,\"foodDepthMode\":1}" "rich + toroidal + patch depths"
add x_steep_s3    "3 3000 --cfg {\"pitchMax\":4,\"kBuoy\":25,\"wallBand\":100,\"vDrag\":0,\"foodDepthMode\":1,\"foodPatches\":4,\"foodDepthSpread\":500} $BANDS" \
                  "extreme: pitchMax 4, kBuoy 25, wallBand 100, no drag, spread 500"
add x_deep_s8     '8 3000 --depth 2000 --cfg {"pitchMax":0.01,"kBuoy":-5,"vDrag":200,"wallBand":0,"buoyMutation":0.5,"toroidal":true,"cHold":0.5,"cPress":0.3}' \
                  "extreme: D 2000, pitchMax 0.01, kBuoy -5, vDrag 200, no walls, depth costs"
add x_shallow_s4  "4 3000 --depth 10 --cfg {\"pitchMax\":1.5,\"wallBand\":5,\"kBuoy\":0,\"foodDepthMode\":1,\"foodPatches\":3,\"foodDepthSpread\":0} $BANDS" \
                  "extreme: D 10 (the minimum), bands cover it all, spread 0"
add x_osc_s6      '6 3000 --cfg {"vDrag":300,"pitchMax":2,"kBuoy":-20,"wallBand":119,"buoyMutation":1}' \
                  "extreme: vDrag 300 (vz flips sign each step), wallBand ~D/2, buoyMutation 1"
add x_tri_body_s9 '9 3000 --cfg {"triangularMotion":true,"bodyModel":true,"reproMaxDistance":16,"reproSoftWidth":5,"pitchMax":1.2}' \
                  "triangular motion + body model + soft repro gate (kernel draws interleaved with bodyBuoy)"
add live_s21      '21 3000 --cfg {"predatorCount":3} --set pitchMax=0.2@800 --set kBuoy=3@1200 --set vDrag=0.5@1200 --set wallBand=40@2000 --set buoyMutation=0.3@2000' \
                  "live oc_set_param-style changes mid-run"
add capture_s17   '17 3000 --capture --cfg {"predatorCount":2,"currentStrength":50}' \
                  "every fish's 46 sensor + 13 action bits hashed every step"
add big_s42       '42 2000 --pop 1500'        "1500 founders, defaults"
add big_toro_s9   '9 2000 --pop 1500 --w 2560 --h 1440 --cfg {"toroidal":true,"predatorCount":8,"predatorEconomy":true,"maxNeighbors":7,"currentStrength":60,"foodPatches":6,"foodDrift":true,"foodDepthMode":1}' \
                  "1500 founders, 2560x1440, toroidal, economy, topological, currents"
add long_s42      '42 10000 --every 250'      "10k steps, defaults"
# ---- branches the cases above never reach, each asserted with --expect ----
add pred_age_s23  '23 6000 --cfg {"predatorCount":6,"predatorEconomy":true,"predatorMaxAge":25,"predatorMeal":15,"predatorMoveCost":0.00003,"predatorMinPopulation":3,"predatorReproEnergy":50} --expect pAge>0 --expect dPred>0 --expect pBirths>0' \
                  "predators die of old age; a fixed predatorMeal; an explicit predatorMoveCost (96 s)"
add gate_s29      '29 6000 --cfg {"reproMaxDistance":16,"reproSoftWidth":0,"reproGracePeriod":30} --expect grace>0 --expect gateBirths>0 --expect soft=0' \
                  "the HARD pairing gate (soft width 0) and the grace bypass for young founders (96 s)"
add cap_s31       '31 4000 --cfg {"maxPopulation":330} --expect capSteps>0 --expect popMax=330' \
                  "a positive maxPopulation cap, binding for most of the run"
add fast_logs_s41 '41 8000 --capture --logs 16,16 --cfg {"fastTanh":true,"predatorCount":2} --expect fastTanhSteps>0 --expect clusterStride>1 --expect reproStride>1 --expect deathLog>0 --expect birthLog>0' \
                  "fastTanh nets (actions hashed); death-cluster map + birth log populated AND decimated (128 s)"
add toro_bud_s43  '43 8000 --cfg {"toroidal":true,"predatorCount":6,"predatorEconomy":true,"predatorInheritance":true,"predatorTraitMutation":0.2,"predatorMinPopulation":2,"predatorMaxPopulation":30} --expect pBirths>0 --expect predGenMax>1 --expect predTraitVaried>0' \
                  "toroidal predator budding + trait inheritance (128 s)"
# ---- E1 physics, features ON (the cases above run every E1 knob at its default) ----
# E1_PRESET must equal kE1PresetJson in src/ocean/ocean_config.hpp (checked below).
E1_PRESET='{"waveSpeed":6,"deepCurrent":true,"currentStrength":5,"foodDrift":true,"lightEnabled":true,"depthCostScaled":true,"cHold":0.05,"cPress":0.03}'
add e1_waves_s3   '3 3000 --cfg {"waveSpeed":8,"foodDrift":true,"predatorCount":3}' \
                  "E1 waves alone: three trains move food, fish, predators in 3-D"
add e1_deep_s5    '5 3000 --cfg {"deepCurrent":true,"currentStrength":60,"foodDrift":true,"foodPatches":4,"foodDepthMode":1,"predatorCount":4,"predatorEconomy":true,"predatorMinPopulation":2}' \
                  "E1 deep current (eddies x f(d)), patch depths, economy"
add e1_gyre_s13   '13 3000 --cfg {"deepCurrent":true,"waveSpeed":5,"currentStrength":40,"currentMode":1,"toroidal":true,"foodDrift":true,"maxNeighbors":7}' \
                  "E1 deep double gyre + waves, toroidal, topological"
add e1_costs_s8   '8 3000 --cfg {"depthCostScaled":true,"cHold":0.3,"cPress":0.2,"bodyModel":true,"waveSpeed":4}' \
                  "E1 depth costs x metabolicRate·massFactor, body model"
add e1_size_s9    '9 3000 --cfg {"bodyModel":true,"sizeDrift":true,"sizeFight":true,"mouthGate":true,"kMouth":1.2,"waveSpeed":6,"deepCurrent":true,"currentStrength":30,"foodDrift":true}' \
                  "E1 size laws: drift min(1,1/L), fights x L², mouth gate (L < .83 starve-gated)"
add e1_preset_s42 "42 3000 --cfg $E1_PRESET"  "E1 preset (kE1PresetJson, the page's ?e1=1)"
add e1_preset_long "7 10000 --every 250 --cfg $E1_PRESET" "E1 preset, 10k steps"
add e1_live_s21   '21 3000 --cfg {"currentStrength":40,"foodDrift":true,"predatorCount":2} --set waveSpeed=6@500 --set deepCurrent=1@800 --set sizeDrift=1@1200 --set depthCostScaled=1@1500 --set cHold=0.05@1500 --set waveGravity=40@2000 --set waveSpeed=0@2500 --set mouthGate=1@2600 --set kMouth=0.6@2600' \
                  "E1 knobs switched live (oc_set_param), waves stopped again (clock kept)"
add e1_capture_s17 '17 3000 --capture --cfg {"waveSpeed":6,"deepCurrent":true,"currentStrength":40,"foodDrift":true}' \
                  "E1: sensors (CURRENT_FWD/LR read the 3-D flow) hashed every step"
add e1_x_shallow_s4 '4 3000 --depth 10 --cfg {"waveSpeed":100,"waveBand":1,"waveGravity":10000,"deepCurrent":true,"currentStrength":80,"foodDrift":true,"predatorCount":3}' \
                  "E1 extreme: D 10, waves at every range end (speed 100, band 1, g 10^4)"
add e1_x_big_s9   '9 2000 --pop 1500 --w 2560 --h 1440 --cfg {"waveSpeed":12,"waveBand":8,"deepCurrent":true,"currentStrength":60,"toroidal":true,"foodDrift":true,"bodyModel":true,"sizeDrift":true,"sizeFight":true,"mouthGate":true,"predatorCount":6,"predatorEconomy":true,"depthCostScaled":true,"cHold":0.1,"cPress":0.1}' \
                  "E1 everything on, 1500 founders, 2560x1440, toroidal"

# ---- own-frame steering and the food lifetime (plain build; ocean-vision) ----
# Each asserts with --expect that the branch ran (cover counters printed only
# when non-zero: steerOwnSteps, foodExpired, agesKept, eye*; see the driver).
add steer_own_s1  '1 3000 --cfg {"steerFrame":1} --expect steerOwnSteps>0' "own-frame steering (STEER_X right, STEER_Y ahead), defaults"
add steer_own_rich_toro_s7 "7 3000 --cfg {$RICH,\"toroidal\":true,\"foodDepthMode\":1,\"steerFrame\":1} --expect steerOwnSteps>0" \
                  "own-frame steering + rich + toroidal + patch depths"
add steer_own_capture_s17 '17 3000 --capture --cfg {"steerFrame":1,"predatorCount":2,"currentStrength":50} --expect steerOwnSteps>0' \
                  "own-frame steering, every fish's sensor + action bits hashed"
add steer_own_live_s21 '21 3000 --cfg {"predatorCount":3} --set steerFrame=1@700 --set steerFrame=0@1500 --set steerFrame=1@2200 --expect steerOwnSteps>0' \
                  "steerFrame switched on / off / on mid-run (oc_set_param)"
add steer_own_tri_s9 '9 3000 --cfg {"triangularMotion":true,"bodyModel":true,"reproMaxDistance":16,"reproSoftWidth":5,"pitchMax":1.2,"steerFrame":1} --expect steerOwnSteps=0' \
                  "triangular motion ignores steerFrame (fish as x_tri_body_s9's)"
SV='"foodSpawnBase":1,"foodSpawnAmp":0,"foodEnergy":15,"targetFood":800'
add life_s1       "1 3000 --cfg {\"foodLife\":8,$SV} --expect foodExpired>0" "foodLife 8, sparse valuable food (1 item/step x 15)"
add life_rich_toro_s7 "7 3000 --cfg {$RICH,\"toroidal\":true,\"foodDepthMode\":1,\"foodLife\":5} --expect foodExpired>0" \
                  "foodLife 5 + rich (patches + drift) + toroidal"
add life_live_s21 '21 3000 --cfg {"predatorCount":3} --set foodLife=6@600 --set foodLife=0@1800 --set foodLife=2.5@2400 --expect foodExpired>0 --expect agesKept>0' \
                  "foodLife switched on / off (ages kept and hashed) / on mid-run (oc_set_param)"
add steer_life_e1_s5 "5 3000 --cfg {\"waveSpeed\":6,\"deepCurrent\":true,\"currentStrength\":20,\"foodDrift\":true,\"depthCostScaled\":true,\"cHold\":0.05,\"cPress\":0.03,\"steerFrame\":1,\"foodLife\":8,$SV} --expect steerOwnSteps>0 --expect foodExpired>0" \
                  "E1 waves + deep current + scaled costs with own-frame steering and foodLife 8"
# The spawn cap (targetFood·2) counts live items: food expiring in a step frees
# its place in that step's spawn (foodCapFreed: steps that refilled such places).
add life_cap_fixture_s1 '1 3000 --pop 0 --cfg {"minPopulation":0,"targetFood":1,"foodSpawnBase":2,"foodSpawnAmp":0,"foodLife":0.016} --expect foodExpired=5999 --expect foodCapFreed=3000' \
                  "no fish, cap 2, 2 items/step that last one step: 2 live items every step"
add life_cap_s3   '3 3000 --cfg {"targetFood":300,"foodSpawnBase":6,"foodSpawnAmp":0,"foodLife":10} --expect foodExpired>0 --expect foodCapFreed>0' \
                  "foodLife 10 with the stock at its cap (targetFood 300, 6 items/step)"
# ---- the compound-eye build (-DEVO_OCEAN_EYE: 64 sensors, its own planes) ----
# Every case named eye_* runs in the eye build (the eye on unless it says
# otherwise; eye_only_* / eye_blind_* = eyeOnly with the eye on / off; eye_code*
# = eyeCode, the eye's encodings; eye_e1_* = with E1 physics on).
add eye_off_s1    '1 3000 --expect eyeSteps=0' "eye build, eye off: 64 sensors, the 18 eye inputs read 0"
add eye_def_s1    '1 3000 --cfg {"eye":1} --expect eyeSteps>0' "eye on, defaults: fish, food in all three loops"
add eye_def_s999  '999 3000 --cfg {"eye":1} --expect eyeSteps>0' "eye on, another seed"
add eye_capture_s17 '17 3000 --capture --cfg {"eye":1,"predatorCount":2,"currentStrength":50} --expect eyeLit>0' \
                  "eye on, every fish's 64 sensor (18 eye) + 13 action bits hashed"
add eye_rich_s5   "5 3000 --cfg {$RICH,\"eye\":1} --expect eyeSteps>0" "eye on + rich: predators (economy) in the predator scan, topological 7"
add eye_rich_toro_s7 "7 3000 --cfg {$RICH,\"toroidal\":true,\"foodDepthMode\":1,\"eye\":1} --expect eyeSteps>0" \
                  "eye on + rich + toroidal (wrapped offsets) + patch depths"
add eye_x_deep_s8 '8 3000 --depth 2000 --cfg {"pitchMax":0.01,"kBuoy":-5,"vDrag":200,"wallBand":0,"buoyMutation":0.5,"toroidal":true,"cHold":0.5,"cPress":0.3,"eye":1} --expect eyeSteps>0' \
                  "eye on, D 2000: ambient light underflows toward 0 at depth"
add eye_x_shallow_s4 '4 3000 --depth 10 --cfg {"pitchMax":1.5,"wallBand":5,"kBuoy":0,"foodDepthMode":1,"foodPatches":3,"foodDepthSpread":0,"eye":1} --expect eyeSteps>0' \
                  "eye on, D 10: nearly flat, so up/down sectors are rare"
add eye_tri_body_s9 '9 3000 --cfg {"triangularMotion":true,"bodyModel":true,"reproMaxDistance":16,"reproSoftWidth":5,"pitchMax":1.2,"eye":1} --expect eyeSteps>0' \
                  "eye on + body model: fish apparent size from heritable bodySize"
add eye_live_s21  '21 3000 --cfg {"predatorCount":3} --set eye=1@500 --set eye=0@1500 --set eye=1@2200 --expect eyeSteps>0' \
                  "eye switched on / off / on mid-run (oc_set_param)"
add eye_big_toro_s9 '9 2000 --pop 1500 --w 2560 --h 1440 --cfg {"toroidal":true,"predatorCount":8,"predatorEconomy":true,"maxNeighbors":7,"currentStrength":60,"foodPatches":6,"foodDrift":true,"foodDepthMode":1,"eye":1} --expect eyeSteps>0' \
                  "eye on, 1500 founders, 2560x1440, toroidal, economy, topological, currents"
add eye_long_s42  '42 10000 --every 250 --cfg {"eye":1} --expect eyeSteps>0' "eye on, 10k steps"
# ---- eye-only mode (OceanConfig::eyeOnly: the 23 labeled senses 0 and skipped) ----
EXP='"predatorCount":4,"predatorEconomy":true,"predatorCooldown":3,"predatorMetabolism":2,"predatorMeal":10,"predatorReproEnergy":60,"predatorMaxPopulation":6,"predatorMinPopulation":2,"predatorInheritance":true'
add eye_only_s1   '1 3000 --cfg {"eye":1,"eyeOnly":1} --expect eyeOnlySteps>0 --expect eyeSteps>0' "eye-only: the eye alone, defaults"
add eye_blind_s1  '1 3000 --cfg {"eyeOnly":1} --expect eyeOnlySteps>0 --expect eyeSteps=0' "blind: eyeOnly with the eye off (own state, pheromones, current)"
add eye_only_capture_s17 '17 3000 --capture --cfg {"eye":1,"eyeOnly":1,"predatorCount":2,"currentStrength":50} --expect eyeLit>0 --expect labeledLit=0' \
                  "eye-only, every fish's 64 sensor bits hashed (23 labeled = 0)"
add eye_only_rich_s5 "5 3000 --cfg {$RICH,\"eye\":1,\"eyeOnly\":1} --expect eyeOnlySteps>0" \
                  "eye-only + rich: economy predators, topological 7 (no cut in eye-only)"
add eye_only_rich_toro_s7 "7 3000 --cfg {$RICH,\"toroidal\":true,\"foodDepthMode\":1,\"eye\":1,\"eyeOnly\":1} --expect eyeOnlySteps>0" \
                  "eye-only + rich + toroidal (wrapped offsets) + patch depths"
add eye_only_exp_s3 "3 5000 --cfg {$EXP,\"eye\":1,\"eyeOnly\":1} --expect eyeOnlySteps>0" \
                  "eye-only, the eye experiment's predator economy (meal 10, cap 6, floor 2, inheritance)"
add eye_blind_exp_toro_s4 "4 3000 --cfg {$EXP,\"toroidal\":true,\"eyeOnly\":1} --expect eyeOnlySteps>0 --expect eyeSteps=0" "blind + the experiment's economy, toroidal"
add eye_only_live_s21 '21 3000 --cfg {"predatorCount":3,"eye":1} --set eyeOnly=1@500 --set eye=0@1200 --set eyeOnly=0@1800 --set eyeOnly=1@2400 --set eye=1@2600 --expect eyeOnlySteps>0 --expect eyeSteps>0' \
                  "eyeOnly / eye switched mid-run: labeled+eye -> eye-only -> blind -> labeled -> blind -> eye-only"
add eye_steer_rich_s5 "5 3000 --cfg {$RICH,\"eye\":1,\"steerFrame\":1} --expect steerOwnSteps>0 --expect eyeSteps>0" "eye + labels + own-frame steering + rich"
add eye_steer_only_exp_s3 "3 5000 --cfg {$EXP,\"eye\":1,\"eyeOnly\":1,\"steerFrame\":1} --expect steerOwnSteps>0 --expect eyeOnlySteps>0" \
                  "eye-only + own-frame steering, the eye experiment's economy"
add eye_steer_blind_exp_toro_s4 "4 3000 --cfg {$EXP,\"toroidal\":true,\"eyeOnly\":1,\"steerFrame\":1} --expect steerOwnSteps>0 --expect eyeSteps=0" \
                  "blind + own-frame steering + the experiment's economy, toroidal"
add eye_life_steer_only_s3 "3 5000 --cfg {$EXP,$SV,\"eye\":1,\"eyeOnly\":1,\"steerFrame\":1,\"foodLife\":8} --expect foodExpired>0 --expect steerOwnSteps>0 --expect eyeOnlySteps>0" \
                  "eye-only + own frame + foodLife 8 + sparse valuable food + the experiment's economy"
add eye_life_steer_blind_s4 "4 3000 --cfg {$EXP,$SV,\"eyeOnly\":1,\"steerFrame\":1,\"foodLife\":8} --expect foodExpired>0 --expect eyeSteps=0" \
                  "blind + own frame + foodLife 8 + sparse valuable food"
# ---- eye encodings (OceanConfig::eyeCode: 1 faded contrast, 2 opponent pairs, 4 summed sectors) ----
add eye_code1_s1  '1 3000 --cfg {"eye":1,"eyeCode":1} --expect eyeCodeMask=1' "eyeCode 1 (faded contrast), defaults"
add eye_code2_capture_s17 '17 3000 --capture --cfg {"eye":1,"eyeCode":2,"predatorCount":2,"currentStrength":50} --expect eyeCodeMask=2 --expect eyeLit>0' \
                  "eyeCode 2 (opponent pairs), every fish's sensor + action bits hashed"
add eye_code4_capture_s17 '17 3000 --capture --cfg {"eye":1,"eyeCode":4,"predatorCount":2,"currentStrength":50} --expect eyeCodeMask=4 --expect eyeLit>0' \
                  "eyeCode 4 (summed sectors), every fish's sensor + action bits hashed"
add eye_code4_rich_toro_s7 "7 3000 --cfg {$RICH,\"toroidal\":true,\"foodDepthMode\":1,\"eye\":1,\"eyeCode\":4} --expect eyeCodeMask=4" \
                  "summed sectors + rich (economy predators; topological 7: sums before the cut) + toroidal"
add eye_code7_only_capture_s17 '17 3000 --capture --cfg {"eye":1,"eyeOnly":1,"eyeCode":7,"predatorCount":2,"currentStrength":50} --expect eyeCodeMask=7 --expect eyeLit>0 --expect labeledLit=0' \
                  "eyeCode 7 (all three) + eye-only, every fish's sensor + action bits hashed"
add eye_code7_steer_only_exp_s3 "3 5000 --cfg {$EXP,\"eye\":1,\"eyeOnly\":1,\"steerFrame\":1,\"eyeCode\":7} --expect eyeCodeMask=7 --expect steerOwnSteps>0" \
                  "eyeCode 7 + eye-only + own-frame steering, the eye experiment's economy"
add eye_code7_life_steer_only_s3 "3 5000 --cfg {$EXP,$SV,\"eye\":1,\"eyeOnly\":1,\"steerFrame\":1,\"foodLife\":8,\"eyeCode\":7} --expect eyeCodeMask=7 --expect foodExpired>0" \
                  "eyeCode 7 + eye-only + own frame + foodLife 8 + sparse valuable food + the experiment's economy"
add eye_code7_big_s42 '42 2000 --pop 1500 --cfg {"eye":1,"eyeCode":7} --expect eyeCodeMask=7' \
                  "eyeCode 7, 1500 founders: long per-sector float sums in visit order"
add eye_code_live_s21 '21 3000 --cfg {"predatorCount":3,"eye":1} --set eyeCode=4@500 --set eyeCode=7@1000 --set eyeOnly=1@1500 --set eyeCode=2@2000 --set eyeCode=1@2400 --set eyeCode=0@2700 --expect eyeCodeMask=7 --expect eyeOnlySteps>0' \
                  "eyeCode switched mid-run (oc_set_param): 0 -> 4 -> 7 -> 7 eye-only -> 2 -> 1 -> 0"
# ---- E1 physics with the eye (each feature as defined alone; ocean_world.hpp
# "E1 with the eye build's features": food eyeCon follows its depth under E1
# drift, the eye sees mouth-gated food) ----
add eye_e1_preset_s42 "42 3000 --cfg ${E1_PRESET%\}},\"eye\":1} --expect eyeE1Steps>0" "the E1 preset (kE1PresetJson) + the eye"
add eye_e1_capture_s17 '17 3000 --capture --cfg {"waveSpeed":6,"deepCurrent":true,"currentStrength":40,"foodDrift":true,"eye":1,"eyeCode":7} --expect eyeE1Steps>0 --expect eyeLit>0 --expect eyeCodeMask=7' \
                  "E1 waves + deep current + food drift in depth, eyeCode 7: every sensor bit hashed"
add eye_e1_size_s9 '9 3000 --cfg {"bodyModel":true,"sizeDrift":true,"sizeFight":true,"mouthGate":true,"kMouth":1.2,"waveSpeed":6,"deepCurrent":true,"currentStrength":30,"foodDrift":true,"eye":1} --expect eyeE1Steps>0' \
                  "E1 size laws (mouth gate: smell gated, sight not) + the eye"
add eye_e1_only_steer_life_s3 "3 5000 --cfg {$EXP,$SV,\"waveSpeed\":6,\"deepCurrent\":true,\"currentStrength\":20,\"foodDrift\":true,\"eye\":1,\"eyeOnly\":1,\"steerFrame\":1,\"foodLife\":8} --expect eyeE1Steps>0 --expect eyeOnlySteps>0 --expect steerOwnSteps>0 --expect foodExpired>0" \
                  "eye-only + own frame + foodLife 8 + E1 waves / deep current + the experiment's economy"
add eye_e1_live_s21 '21 3000 --cfg {"currentStrength":40,"foodDrift":true,"predatorCount":2,"eye":1} --set waveSpeed=6@500 --set eyeOnly=1@900 --set deepCurrent=1@1200 --set steerFrame=1@1500 --set foodLife=6@1800 --set waveSpeed=0@2400 --set eyeCode=4@2600 --expect eyeE1Steps>0 --expect foodExpired>0 --expect steerOwnSteps>0' \
                  "E1 and the eye's keys switched live, interleaved (oc_set_param)"
# ---- the cloaked eye (eyeCode 8: the eye ignores fish, 16: it ignores food;
# predators always seen; the labeled senses unchanged; in eye-only mode the
# cloaked kind is not queried) ----
# L12 = the cloaked-eye ablation's world verbatim (experiment 2's cfg_L12.json:
# the stable predator economy, 1 food item/step x 15 energy, foodLife 12 s).
L12='"predatorCount":4,"predatorSpeed":110,"predatorPerception":150,"predatorCaptureRadius":14,"predatorEconomy":true,"predatorCooldown":3,"predatorMetabolism":2,"predatorMeal":10,"predatorReproEnergy":60,"predatorMaxPopulation":6,"predatorMinPopulation":2,"predatorInheritance":true,"foodSpawnBase":1,"foodSpawnAmp":0,"foodEnergy":15,"targetFood":800,"foodLife":12'
add eye_cloak_fish_s1 '1 3000 --cfg {"eye":1,"eyeCode":8} --expect eyeCodeMask=8 --expect eyeSteps>0' \
                  "the eye ignores fish, labels on (the neighbour loop runs, only the eye's bin skips fish)"
add eye_cloak_food_capture_s17 '17 3000 --capture --cfg {"eye":1,"eyeCode":16,"predatorCount":2,"currentStrength":50} --expect eyeCodeMask=16 --expect eyeLit>0' \
                  "the eye ignores food, every sensor bit hashed (the labeled food senses still read it)"
add eye_cloak_sum_rich_toro_s7 "7 3000 --cfg {$RICH,\"toroidal\":true,\"foodDepthMode\":1,\"eye\":1,\"eyeCode\":12} --expect eyeCodeMask=12" \
                  "summed sectors over food + predators (fish cloaked) + rich + toroidal + topological 7"
add eye_cloak_fish_only_steer_l12_s3 "3 5000 --cfg {$L12,\"eye\":1,\"eyeOnly\":1,\"steerFrame\":1,\"eyeCode\":8} --expect eyeCodeMask=8 --expect eyeOnlySteps>0 --expect steerOwnSteps>0 --expect foodExpired>0" \
                  "eye-only (no neighbour query) + own frame, the ablation's world (L12)"
add eye_cloak_food_only_steer_l12_s3 "3 5000 --cfg {$L12,\"eye\":1,\"eyeOnly\":1,\"steerFrame\":1,\"eyeCode\":16} --expect eyeCodeMask=16 --expect eyeOnlySteps>0 --expect steerOwnSteps>0 --expect foodExpired>0" \
                  "eye-only (no food query) + own frame, the ablation's world (L12)"
add eye_cloak_both_only_capture_s17 '17 3000 --capture --cfg {"eye":1,"eyeOnly":1,"eyeCode":24,"predatorCount":4,"currentStrength":50} --expect eyeCodeMask=24 --expect eyeLit>0 --expect labeledLit=0' \
                  "only predators seen (eyeCode 24), eye-only, every sensor bit hashed"
add eye_cloak_live_s21 '21 3000 --cfg {"predatorCount":3,"eye":1} --set eyeCode=8@500 --set eyeCode=16@1000 --set eyeOnly=1@1500 --set eyeCode=28@2000 --set eyeCode=0@2500 --expect eyeCodeMask=28 --expect eyeOnlySteps>0' \
                  "cloaks switched mid-run (oc_set_param): 0 -> 8 -> 16 -> 16 eye-only -> 28 -> 0"

# Configs the page must REFUSE (oc_init / oc_set_param; ocean_tests --accepts
# must exit 1): every Ocean knob has a finite range, so none of these can run.
RNAMES=(); RARGS=()
radd () { RNAMES+=("$1"); RARGS+=("$2"); }
radd r_kbuoy_hole   '1 10 --cfg {"kBuoy":-1e37}'
radd r_kbuoy_big    '1 10 --cfg {"kBuoy":1001}'
radd r_vdrag        '1 10 --cfg {"vDrag":1e30}'
radd r_pitch_tiny   '1 10 --cfg {"pitchMax":1e-9}'
radd r_wallband     '1 10 --cfg {"wallBand":1e9}'
radd r_buoymut      '1 10 --cfg {"buoyMutation":1.5}'
radd r_chold_neg    '1 10 --cfg {"cHold":-0.5}'
radd r_spread       '1 10 --cfg {"foodDepthSpread":1e30}'
radd r_mode         '1 10 --cfg {"foodDepthMode":2}'
radd r_wave_speed   '1 10 --cfg {"waveSpeed":1e6}'
radd r_wave_band    '1 10 --cfg {"waveSpeed":6,"waveBand":0}'
radd r_wave_g       '1 10 --cfg {"waveGravity":0}'
radd r_kwater       '1 10 --cfg {"kWater":-0.1}'
radd r_l0           '1 10 --cfg {"L0":1e39}'
radd r_kmouth       '1 10 --cfg {"kMouth":0}'
radd r_bool         '1 10 --cfg {"deepCurrent":2}'
radd r_depth        '1 10 --depth 1e6'
radd r_live_kbuoy   '1 10 --set kBuoy=1e37@5'
radd r_live_wave    '1 10 --set waveSpeed=500@5'
radd r_live_bool    '1 10 --set mouthGate=0.5@5'
radd r_live_band    '1 10 --set waveBand=0.5@5'
# The Ocean's ranges for SimConfig keys (ocean_config.hpp oceanSimConfigError).
radd r_cur_hang     '1 10 --pop 0 --cfg {"currentMode":1,"currentStrength":1e20,"deepCurrent":true,"foodDrift":true,"targetFood":1}'
radd r_cur_strength '1 10 --cfg {"currentStrength":1001}'
radd r_cur_neg      '1 10 --cfg {"currentStrength":-1}'
radd r_cur_eddies   '1 10 --cfg {"currentStrength":50,"currentEddies":101}'
radd r_cur_core     '1 10 --cfg {"currentStrength":50,"currentCore":1e9}'
radd r_cur_drift    '1 10 --cfg {"currentStrength":50,"currentDrift":1e30}'
radd r_cur_eps      '1 10 --cfg {"currentStrength":50,"currentMode":1,"currentGyreEps":2}'
radd r_cur_mode     '1 10 --cfg {"currentStrength":50,"currentMode":2}'
radd r_patch_radius '1 10 --cfg {"toroidal":true,"foodPatches":3,"foodPatchRadius":1e30}'
# Int keys must be whole numbers in int's range (checked before the shared
# reader's (int) cast), and the values that reach an int conversion while stepping.
radd r_int_big      '1 10 --cfg {"maxPopulation":2147483648}'
radd r_int_neg_big  '1 10 --cfg {"foodPatches":-1e10}'
radd r_int_frac     '1 10 --cfg {"currentMode":0.5}'
radd r_int_dup      '1 10 --cfg {"predatorCount":2,"predatorCount":1e300}'
radd r_live_int     '1 10 --set maxPopulation=2147483648@5'
radd r_live_frac    '1 10 --set minPopulation=7.5@5'
radd r_food_base    '1 10 --cfg {"foodSpawnBase":1e20}'
radd r_food_amp     '1 10 --cfg {"foodSpawnAmp":-1e20}'
radd r_live_food    '1 10 --set foodSpawnBase=1e20@5'
radd r_target_food  '1 10 --cfg {"targetFood":2000000000}'
radd r_live_target  '1 10 --set targetFood=2147483647@5'
radd r_speed        '1 10 --cfg {"maxSpeed":1e30}'
radd r_live_speed   '1 10 --set maxSpeed=1e30@5'
radd r_pred_speed   '1 10 --cfg {"predatorCount":2,"predatorSpeed":1e30}'
radd r_radius_big   '1 10 --cfg {"predatorCount":2,"predatorPerception":1e30}'
# Every other float the step reads must be finite (1e39 is an infinite float),
# and the counts / world sizes that size allocations have generous ceilings.
radd r_turn_inf     '1 10 --cfg {"triangularMotion":true,"maxTurnRate":1e39}'
radd r_cooldown_inf '1 10 --cfg {"interactCooldown":1e39}'
radd r_repro_cd_inf '1 10 --cfg {"reproCooldown":-1e39}'
radd r_pred_metab   '1 10 --cfg {"predatorCount":2,"predatorEconomy":true,"predatorMetabolism":1e39}'
radd r_pred_cd_inf  '1 10 --cfg {"predatorCount":2,"predatorCooldown":1e39}'
radd r_pred_meal    '1 10 --cfg {"predatorCount":2,"predatorEconomy":true,"predatorMeal":1e39}'
radd r_pred_repro   '1 10 --cfg {"predatorCount":2,"predatorEconomy":true,"predatorReproEnergy":-1e39}'
radd r_pred_age_inf '1 10 --cfg {"predatorCount":2,"predatorEconomy":true,"predatorMaxAge":1e39}'
# The energies in [-1e6, 1e6] (kOceanMaxEnergy): finite values past it overflowed
# (one fish eating four items of 1e38: reserves +inf; moveCost 1e38: a step's cost inf).
radd r_food_energy  '1 1 --pop 1 --cfg {"minPopulation":0,"pickupRadius":1000000,"foodEnergy":1e38}'
radd r_move_cost    '1 10 --cfg {"moveCost":1e38}'
radd r_gene_cost    '1 10 --cfg {"geneCost":-1000001}'
radd r_metab_big    '1 10 --cfg {"metabolicRate":2e6}'
radd r_newborn_big  '1 10 --cfg {"newbornEnergy":1e7}'
radd r_repro_big    '1 10 --cfg {"reproEnergy":-2e6}'
radd r_pred_meal_big '1 10 --cfg {"predatorCount":4,"predatorEconomy":true,"predatorMeal":3e38}'
radd r_pred_mcost_big '1 10 --cfg {"predatorCount":2,"predatorEconomy":true,"predatorMoveCost":1e38}'
radd r_pred_newborn_big '1 10 --cfg {"predatorCount":2,"predatorEconomy":true,"predatorNewbornEnergy":-3e38}'
radd r_live_energy  '1 10 --set foodEnergy=1000001@5'
radd r_live_pred_metab '1 10 --cfg {"predatorCount":2,"predatorEconomy":true} --set predatorMetabolism=1e30@5'
# (Each just past its ceiling, and still small enough to allocate: were the
# check missing, oc_init would accept it, not crash into a false "refused".)
radd r_pop_big      '1 10 --pop 100001'
radd r_width_big    '1 10 --w 100001'
radd r_height_big   '1 10 --h 100001'
radd r_grid_cells   '1 10 --w 6000 --h 3000 --cfg {"perceptionRadius":1,"interactRadius":1,"pickupRadius":1}'
radd r_minpop_big   '1 10 --cfg {"minPopulation":100001}'
radd r_live_minpop  '1 10 --set minPopulation=100001@5'
radd r_predcount_big '1 10 --cfg {"predatorCount":100001}'
radd r_patches_big  '1 10 --cfg {"foodPatches":100001}'
# The ocean-vision keys: the int keys (eye, eyeOnly, eyeCode, steerFrame) are
# whole numbers in int's range, then in range; foodLife in [0, 3600] (beyond,
# float ages lose accuracy, and from 2^19 s they stop: kOceanMaxFoodLife). r_eye_*
# are asked of the eye build's shim, the rest of the plain build's (where eye,
# eyeOnly and eyeCode must stay 0).
radd r_steer_frac   '1 10 --cfg {"steerFrame":0.5}'
radd r_steer_two    '1 10 --cfg {"steerFrame":2}'
radd r_steer_huge   '1 10 --cfg {"steerFrame":1e300}'
radd r_steer_dup    '1 10 --cfg {"steerFrame":1,"steerFrame":0.5}'
radd r_life_neg     '1 10 --cfg {"foodLife":-1}'
radd r_life_big     '1 10 --cfg {"foodLife":1000001}'
radd r_life_inf     '1 10 --cfg {"foodLife":1e39}'
radd r_life_hour    '1 10 --cfg {"foodLife":3600.5}'
radd r_life_stall   '1 10 --cfg {"foodLife":1000000}'
radd r_live_steer   '1 10 --set steerFrame=0.5@5'
radd r_live_steer2  '1 10 --set steerFrame=2@5'
radd r_live_life    '1 10 --set foodLife=1e7@5'
radd r_live_life_hour '1 10 --set foodLife=3601@5'
radd r_plain_eye    '1 10 --cfg {"eye":1}'
radd r_plain_eyeonly '1 10 --cfg {"eyeOnly":1}'
radd r_plain_eyecode '1 10 --cfg {"eyeCode":1}'
radd r_plain_live_eye '1 10 --set eye=1@5'
radd r_eye_frac     '1 10 --cfg {"eye":0.5}'
radd r_eye_two      '1 10 --cfg {"eye":2}'
radd r_eye_huge     '1 10 --cfg {"eye":1e300}'
radd r_eye_dup      '1 10 --cfg {"eye":1,"eye":0.5}'
radd r_eye_only_frac '1 10 --cfg {"eye":1,"eyeOnly":0.5}'
radd r_eye_only_two '1 10 --cfg {"eyeOnly":2}'
radd r_eye_code_frac '1 10 --cfg {"eye":1,"eyeCode":0.5}'
radd r_eye_code_big '1 10 --cfg {"eye":1,"eyeCode":32}'
radd r_plain_cloak  '1 10 --cfg {"eyeCode":16}'
radd r_eye_code_huge '1 10 --cfg {"eye":1,"eyeCode":1e300}'
radd r_eye_code_neg '1 10 --cfg {"eye":1,"eyeCode":-1}'
radd r_eye_code_int '1 10 --cfg {"eye":1,"eyeCode":2147483648}'
radd r_eye_live_frac '1 10 --set eye=0.5@5'
radd r_eye_live_code '1 10 --set eyeCode=32@5'
radd r_eye_live_code_frac '1 10 --set eyeCode=2.5@5'
radd r_eye_radius_sq '1 10 --cfg {"eye":1,"perceptionRadius":1e20}'

run_one () {  # run_one INDEX: native and wasm side by side
    local i=$1 name=${NAMES[$1]} s
    s=$(sfx "$name")
    local -a A
    read -r -a A <<< "${ARGS[$i]}"
    ( "$OUT/opd_native$s" "${A[@]}" > "$OUT/$name.native.txt" 2> "$OUT/$name.native.log"; echo $? > "$OUT/$name.native.rc" ) &
    ( $NODE "$OUT/opd_wasm$s.js" "${A[@]}" > "$OUT/$name.wasm.txt" 2> "$OUT/$name.wasm.log"; echo $? > "$OUT/$name.wasm.rc" ) &
    wait
}

SEL=()
for i in "${!NAMES[@]}"; do
    if [ -n "${ONLY:-}" ] && ! [[ ${NAMES[$i]} =~ ${ONLY} ]]; then continue; fi
    if [ -n "${NO_EYE:-}" ] && [[ ${NAMES[$i]} == eye_* ]]; then continue; fi
    SEL+=("$i")
done
# Every case must be a world the page accepts: the real oc_init / oc_set_param
# validation (wasm/ocean_wasm.cpp, through ocean_tests --accepts) is asked first.
declare -A REFUSED=()
if [ -z "${NO_TESTS:-}" ]; then
    for i in "${SEL[@]}"; do
        read -r -a A <<< "${ARGS[$i]}"
        msg=$("$OUT/ocean_tests_native$(sfx "${NAMES[$i]}")" --accepts "${A[@]}" 2>&1) || REFUSED[$i]=${msg:-refused}
    done
    echo "== shim validation (oc_init + oc_set_param): $(( ${#SEL[@]} - ${#REFUSED[@]} )) of ${#SEL[@]} case configs accepted"
fi
# The configs that must be refused, and the preset's copy above vs the source.
# (ONLY skips the refused-config list to save time, except under --overwrite.)
GUARD_FAILED=0
if [ -z "${NO_TESTS:-}" ] && { [ -z "${ONLY:-}" ] || [ "$OVERWRITE" = 1 ]; }; then
    nref=0; bad=""
    for i in "${!RNAMES[@]}"; do
        read -r -a A <<< "${RARGS[$i]}"
        rs=; [[ ${RNAMES[$i]} == r_eye_* ]] && rs=_eye
        if [ -n "$rs" ] && [ -n "${NO_EYE:-}" ]; then continue; fi
        if "$OUT/ocean_tests_native$rs" --accepts "${A[@]}" > "$OUT/${RNAMES[$i]}.refusal.txt" 2>&1; then
            bad="$bad ${RNAMES[$i]}"
        else
            nref=$((nref + 1))
        fi
    done
    if [ -z "$bad" ]; then
        echo "== out-of-range configs: $nref of ${#RNAMES[@]} refused by oc_init / oc_set_param (as they must be)${NO_EYE:+; r_eye_* skipped (NO_EYE)}"
    else
        echo "== out-of-range configs ACCEPTED (must be refused):$bad"; GUARD_FAILED=1
    fi
fi
SRC_PRESET=$(sed -n 's/^ *R"(\(.*\))";$/\1/p' src/ocean/ocean_config.hpp | head -1)
if [ "$SRC_PRESET" = "$E1_PRESET" ]; then
    echo "== E1 preset: the harness's copy == kE1PresetJson"
else
    echo "== E1 preset DRIFT: harness '$E1_PRESET' vs ocean_config.hpp '$SRC_PRESET'"; GUARD_FAILED=1
fi
# The Ocean's pre-cast check of int keys (ocean_config.hpp kSimConfigIntKeys)
# must cover exactly the keys checkpoint::readConfig reads with intOr.
RC_INT=$(sed -n '/^inline void readConfig/,/^}/p' src/core/checkpoint_io.hpp | grep -o 'intOr("[A-Za-z]*"' | sed 's/intOr("//;s/"//' | sort | tr '\n' ' ')
OC_INT=$(sed -n '/kSimConfigIntKeys\[\] = {/,/};/p' src/ocean/ocean_config.hpp | grep -o '"[A-Za-z]*"' | tr -d '"' | sort | tr '\n' ' ')
if [ -n "$RC_INT" ] && [ "$RC_INT" = "$OC_INT" ]; then
    echo "== int keys: kSimConfigIntKeys == checkpoint::readConfig's intOr keys ($(echo "$OC_INT" | wc -w))"
else
    echo "== int keys DRIFT: readConfig reads '$RC_INT', kSimConfigIntKeys lists '$OC_INT'"; GUARD_FAILED=1
fi
# Baseline files without a case (a renamed or dropped case): stale history.
ORPHANS=""
if [ -z "${ONLY:-}" ] && [ -d "$BASE" ]; then
    for f in "$BASE"/*.txt; do
        [ -e "$f" ] || continue
        b=$(basename "$f" .txt); found=0
        for nm in "${NAMES[@]}"; do [ "$nm" = "$b" ] && found=1 && break; done
        [ "$found" = 1 ] || ORPHANS="$ORPHANS $b"
    done
    if [ -n "$ORPHANS" ]; then
        if [ "$REBASE" = 1 ]; then echo "== baselines with no case (removed by --overwrite):$ORPHANS"
        else echo "== baselines with no case (git rm them, or --rebaseline):$ORPHANS"; GUARD_FAILED=1; fi
    fi
fi
echo "== running ${#SEL[@]} case(s), $JOBS at a time (native and wasm side by side) in $OUT"
for i in "${SEL[@]}"; do
    while [ "$(jobs -rp | wc -l)" -ge "$JOBS" ]; do wait -n; done
    run_one "$i" &
done
wait

# The first differing line of EXPECTED and GOT, and what differs on it: for a
# fingerprint line the step and the sections, else the line's k=v keys.
first_diff () {  # first_diff EXPECTED GOT LABEL_OF_GOT
    local c ln la lb
    c=$(cmp "$1" "$2" 2>&1)
    ln=$(echo "$c" | sed -n 's/.*line \([0-9]*\).*/\1/p')
    if echo "$c" | grep -q EOF; then echo "$3 output $(echo "$c" | grep -q "EOF on $2" && echo ends || echo runs on) after line $ln"; return; fi
    [ -n "$ln" ] || { echo "(no line info)"; return; }
    la=$(sed -n "${ln}p" "$1"); lb=$(sed -n "${ln}p" "$2")
    echo "line $ln ($(echo "$la" | awk '{print ($1 == "fp") ? "step " $2 : "the " $1 " line"}')): $(paste -d'\t' <(echo "$la" | tr ' ' '\n') <(echo "$lb" | tr ' ' '\n') |
        awk -F'\t' '$1 != $2 { if ($1 ~ /=/) { split($1, k, "="); printf "%s ", k[1] } else if (!shown) { printf "value "; shown = 1 } }')differ"
}
secs_of () { sed -n 's/^# [0-9]* steps, \([0-9.]*\) s.*/\1/p' "$1" 2>/dev/null; }

FAILED=0; NPASS=0
CHANGED=(); declare -A CHANGE_WHY=()
printf '\n%-30s %6s %6s %5s %6s %5s %4s %7s %7s  %s\n' CASE SEED STEPS POP BIRTHS FOOD PRED NATIVE WASM RESULT
for i in "${SEL[@]}"; do
    name=${NAMES[$i]}
    read -r -a A <<< "${ARGS[$i]}"
    n="$OUT/$name.native.txt"; w="$OUT/$name.wasm.txt"; b="$BASE/$name.txt"
    fin=$(grep '^final ' "$n" 2>/dev/null)
    get () { echo "$fin" | tr ' ' '\n' | sed -n "s/^$1=//p"; }
    nfp=$(grep -c '^fp ' "$n" 2>/dev/null)
    rcn=$(cat "$OUT/$name.native.rc" 2>/dev/null); rcw=$(cat "$OUT/$name.wasm.rc" 2>/dev/null)
    ok=0
    if [ -n "${REFUSED[$i]:-}" ]; then
        res="FAIL  the page refuses this config: ${REFUSED[$i]}"
    elif [ "$rcn" = 5 ] && [ "$rcw" = 5 ]; then
        res="FAIL  expectation not met (the case did not reach its branch):$(sed -n 's/^expect \(.*\) FAILED \(.*\)/ \1 \2/p' "$n" | tr '\n' ' ')"
    elif [ "$rcn" != 0 ] || [ "$rcw" != 0 ]; then
        res="FAIL  exit native=$rcn wasm=$rcw: $(tail -1 "$OUT/$name.wasm.log" 2>/dev/null) $(tail -1 "$OUT/$name.native.log" 2>/dev/null)"
    elif [ -z "$fin" ]; then
        res="FAIL  no final line"
    elif [ "$(get nonfinite_max)" != 0 ]; then
        # Both builds may agree here and still not be portable: wasm leaves a
        # NaN's sign/payload to the host (x86 0xffc00000, ARM 0x7fc00000).
        res="FAIL  non-finite state from step $(get nonfinite_from) (up to $(get nonfinite_max) values)"
        cmp -s "$n" "$w" && res="$res; native == wasm on this host only"
    elif ! cmp -s "$n" "$w"; then
        res="FAIL  native != wasm: $(first_diff "$n" "$w" wasm)"
    else
        ok=1
    fi
    if [ "$ok" = 1 ]; then
        if [ ! -f "$b" ]; then
            why="no baseline yet ($b)"
        elif cmp -s "$n" "$b"; then
            why=""
        else
            why="differs from the baseline at $(first_diff "$b" "$n" native); $(diff "$b" "$n" | grep -c '^>') line(s) differ"
        fi
        if [ -z "$why" ]; then
            res="PASS  native == wasm == baseline ($nfp prints, every step chained)"; NPASS=$((NPASS + 1))
        elif [ "$REBASE" = 1 ]; then
            res="CHANGED  native == wasm, but $why"; CHANGED+=("$i"); CHANGE_WHY[$i]=$why
        else
            res="FAIL  native == wasm, but $why (an intended change? see --rebaseline; else a regression)"; FAILED=1
        fi
    else
        FAILED=1
    fi
    printf '%-30s %6s %6s %5s %6s %5s %4s %6ss %6ss  %s\n' "$name" "${A[0]}" "${A[1]}" "$(get pop)" "$(get births)" \
        "$(get food)" "$(get pred)" "$(secs_of "$OUT/$name.native.log")" "$(secs_of "$OUT/$name.wasm.log")" "$res"
done

# Same build, same arguments, twice: the native driver itself is reproducible
# (the first selected case of each build).
for v in $VARIANTS; do
    for i in "${SEL[@]}"; do
        name=${NAMES[$i]}
        [ "$(sfx "$name")" = "$([ $v = eye ] && echo _eye)" ] || continue
        read -r -a A <<< "${ARGS[$i]}"
        "$OUT/opd_native$(sfx "$name")" "${A[@]}" > "$OUT/$name.native2.txt" 2> /dev/null
        if cmp -s "$OUT/$name.native.txt" "$OUT/$name.native2.txt"; then
            echo "repeat: $name native run twice -> identical"
        else
            echo "repeat: $name native run twice -> DIFFERENT"; FAILED=1
        fi
        break
    done
done

TESTS_OK=1
if [ -z "${NO_TESTS:-}" ]; then
    for v in $VARIANTS; do
        S=; [ $v = eye ] && S=_eye
        echo
        echo "== unit checks (tests/ocean_tests.cpp), $v build, native"
        "$OUT/ocean_tests_native$S" > "$OUT/tests$S.native.txt" 2>&1; rct=$?
        cat "$OUT/tests$S.native.txt"
        echo "== unit checks, $v build, wasm (node)"
        $NODE "$OUT/ocean_tests_wasm$S.js" > "$OUT/tests$S.wasm.txt" 2>&1; rcw=$?
        tail -1 "$OUT/tests$S.wasm.txt"
        if [ $rct != 0 ] || [ $rcw != 0 ]; then TESTS_OK=0; echo "   unit checks FAILED (native rc $rct, wasm rc $rcw)"; fi
        if cmp -s "$OUT/tests$S.native.txt" "$OUT/tests$S.wasm.txt"; then
            echo "   unit-check output native == wasm (every printed value identical)"
        else
            echo "   unit-check output differs native vs wasm:"; diff "$OUT/tests$S.native.txt" "$OUT/tests$S.wasm.txt" | head -20; TESTS_OK=0
        fi
    done
fi

echo
echo "== $(( $(date +%s) - T0 )) s total"
if [ "$REBASE" = 1 ]; then
    echo "== rebaseline: ${#CHANGED[@]} case(s) differ from their baseline or have none${ORPHANS:+; orphans:$ORPHANS}"
    for i in "${CHANGED[@]}"; do echo "   ${NAMES[$i]}: ${CHANGE_WHY[$i]}"; done
    if [ ${#CHANGED[@]} -eq 0 ] && [ -z "$ORPHANS" ]; then
        echo "   nothing to write"
    elif [ "$OVERWRITE" = 0 ]; then
        echo "   DRY RUN: nothing written. If (and only if) these changes are the behaviour you meant, record them:"
        echo "     $0 --rebaseline --overwrite --reason \"what changed and why\""
    elif [ "$FAILED" = 1 ] || [ "$TESTS_OK" = 0 ] || [ "$GUARD_FAILED" = 1 ]; then
        echo "   REFUSED: every case must pass native == wasm and its expectations, the unit checks must pass and"
        echo "   no guard may fail before a baseline is written (fix the failures above first)"
        exit 1
    else
        # Every write is checked. All new files (the baselines and the
        # MANIFEST with its new entry) are first staged in $BASE/.rebaseline.*
        # (same filesystem), so a full disk or a read-only directory fails
        # there with nothing replaced; then each is moved into place, checked.
        mkdir -p "$BASE" || { echo "   FAILED: cannot create $BASE; nothing written"; exit 1; }
        STAGE=$(mktemp -d "$BASE/.rebaseline.XXXXXX") || { echo "   FAILED: cannot create a staging directory in $BASE; nothing written"; exit 1; }
        stage_fail () { echo "   FAILED: $1; nothing written to $BASE"; rm -rf "$STAGE"; exit 1; }
        names=""
        for i in "${CHANGED[@]}"; do
            cp "$OUT/${NAMES[$i]}.native.txt" "$STAGE/${NAMES[$i]}.txt" || stage_fail "cannot stage ${NAMES[$i]}.txt"
            names="$names ${NAMES[$i]}"
        done
        fmt=$(sed -n '1s/.*format=\([0-9]*\).*/\1/p' "$OUT/${NAMES[${SEL[0]}]}.native.txt")
        dirty=$(git status --porcelain -- src wasm/ocean_wasm.cpp 2>/dev/null | wc -l)
        if [ -f "$BASE/MANIFEST" ]; then
            cp "$BASE/MANIFEST" "$STAGE/MANIFEST" || stage_fail "cannot stage MANIFEST"
        else
            cat > "$STAGE/MANIFEST" << 'EOF' || stage_fail "cannot write a new MANIFEST"
tests/ocean_baseline — the Ocean determinism harness's historical oracle.

<case>.txt is ocean_parity_driver's native output for that case of
wasm/ocean_parity.sh (whose wasm output must equal it too): the fingerprint of
the whole world every 100 / 250 steps, the rolling chain over every step, the
coverage counters, the expectations, the final line. wasm/ocean_parity.sh fails
any case whose output differs from its file here, so a change to shared
behaviour that native and wasm make identically (they are built from the same
source) still fails.

Rewrite these files ONLY with `wasm/ocean_parity.sh --rebaseline --overwrite
--reason "..."`, ONLY when behaviour is MEANT to change (a deliberate engine
change, a new or edited case, a change to what the harness hashes — then bump
ofp::kFormatVersion), and NEVER to make a failure go away. Commit them with
the change that moved them. Each rewrite is logged below (newest last).

EOF
        fi
        {
            echo "$(date -u +%Y-%m-%dT%H:%MZ)  engine $(git rev-parse --short HEAD 2>/dev/null)$( [ "$dirty" -gt 0 ] && echo " + $dirty uncommitted engine path(s)" )  format=$fmt  g++ $GPP_VER / $EM_VER"
            echo "  reason: $REASON"
            echo "  wrote:$names${ORPHANS:+; removed:$ORPHANS}"
        } >> "$STAGE/MANIFEST" || stage_fail "cannot append the entry to the staged MANIFEST"
        # Staged: now replace. A failure from here on can leave the set partly
        # rewritten; say so, and how to undo it.
        moved=0
        place_fail () {
            echo "   FAILED: $1, after $moved of the baseline file(s) were replaced: $BASE may be PARTLY rewritten" \
                 "and the MANIFEST may not record it. Restore it: git checkout -- $BASE (staging kept in $STAGE)"
            exit 1
        }
        for i in "${CHANGED[@]}"; do
            mv -f "$STAGE/${NAMES[$i]}.txt" "$BASE/${NAMES[$i]}.txt" || place_fail "cannot replace $BASE/${NAMES[$i]}.txt"
            moved=$((moved + 1))
        done
        for o in $ORPHANS; do
            rm -f "$BASE/$o.txt" && [ ! -e "$BASE/$o.txt" ] || place_fail "cannot remove the orphan $BASE/$o.txt"
        done
        mv -f "$STAGE/MANIFEST" "$BASE/MANIFEST" || place_fail "cannot replace $BASE/MANIFEST"
        rmdir "$STAGE" || echo "   (could not remove the empty staging directory $STAGE)"
        echo "   WROTE ${#CHANGED[@]} baseline(s) to $BASE and logged them in $BASE/MANIFEST."
        echo "   Read the diff before committing: git diff --stat $BASE; git diff $BASE/MANIFEST"
    fi
    [ "$FAILED" = 1 ] || [ "$TESTS_OK" = 0 ] || [ "$GUARD_FAILED" = 1 ] && { echo "== (parity / unit / guard failures above)"; exit 1; }
    exit 0
fi
if [ "$FAILED" = 1 ] || [ "$TESTS_OK" = 0 ] || [ "$GUARD_FAILED" = 1 ]; then
    [ "$FAILED" = 1 ] && echo "== OCEAN PARITY FAILED: bisect with --trace, then --dump-step/--dump (native vs wasm, or old vs new engine)"
    [ "$TESTS_OK" = 0 ] && echo "== OCEAN UNIT CHECKS FAILED"
    [ "$GUARD_FAILED" = 1 ] && echo "== OCEAN CONFIG GUARDS FAILED (a bad config accepted, the E1 preset copy or the int-key list drifted, or an orphan baseline)"
    exit 1
fi
if [ -n "${NO_TESTS:-}" ]; then
    echo "== ALL $NPASS OCEAN PARITY CASES PASS: native == wasm == baseline, bit for bit, every step; expectations met;" \
         "unit checks, shim validation and config guards NOT run (NO_TESTS)"
    exit 0
fi
echo "== ALL $NPASS OCEAN PARITY CASES PASS: native == wasm == baseline, bit for bit, every step; expectations met;" \
     "unit checks pass in both builds"
