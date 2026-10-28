// ocean_tests.cpp — unit checks for the Ocean world (src/ocean/), the
// companion of the native-vs-wasm harness (wasm/ocean_parity.sh, which builds
// and runs this file natively and under node).
//
//   g++ -O3 -std=c++20 -ffp-contract=off -DEVO_WORLD_OCEAN -I src tests/ocean_tests.cpp -o ocean_tests
//
// What is checked (each block prints one ok / FAIL line):
//   bounds        depth in [0, D] and |vz| <= pitchMax for every fish every
//                 step (food and predator depth too), default and extreme configs
//   buoyancy      an idle fish (no wiring) is pulled monotonically to bodyBuoy·D;
//                 with kBuoy = 0 it holds its depth exactly
//   steer_z       STEER_Z moves a fish down (+) / up (−); a LOC_Z -> STEER_Z wire
//                 drives it; the action's value is tanh of the wire
//   sensors       LOC_Z, OWN_VEL_Z, BOUNDARY_DIST_Z equal their defining
//                 expressions; NEAREST_FOOD_DZ / AVG_NEIGHBOR_DZ in range; the
//                 food direction is a unit vector; every sensor finite
//   sensor_signs  hand-placed fixtures: the SIGN and value of LOC_Z,
//                 OWN_VEL_Z, NEAREST_FOOD_DZ (+ = food deeper), AVG_NEIGHBOR_DZ
//                 (+ = neighbours deeper), BOUNDARY_DIST_Z; a NEAREST_FOOD_DZ ->
//                 STEER_Z (+) wire takes a fish toward its food in depth
//   current_sens  3-D flow on: CURRENT_FWD / LR are + along / to the right of
//                 the heading, 0 at the surface (deep current), f(d)-weighted,
//                 and read flowAt at the fish (waves too)
//   walls         the depth wall bands push away from surface / floor by
//                 exactly (1 − z/wb)·3·dt, capped at D/2 in a shallow sea
//   capture       captureActivations changes no dynamics
//   reproducible  same seed twice -> identical fingerprints; another seed and a
//                 1-ulp change in any section change the hash — incl. a
//                 vortex frequency, a food-patch depth, a grid's cell size, the
//                 signal grid's wrap flag, a wave phase (each its own section)
//   reset         oc_init / oc_reset (the shim, wasm/ocean_wasm.cpp) replay
//                 exactly, live oc_set_param changes are dropped, and the shim's
//                 world == the harness's ofp::initWorld world (the harness's
//                 prints, rng included, taken BEFORE the shim runs: an extra
//                 draw in oc_step fails); a fresh world re-inited replays, a
//                 reused one keeps exactly frame / ids / totals (asserted)
//   pred_econ     fixtures: burn = metabolism + predatorMoveCost·speed² (or
//                 moveCost when < 0), a fixed predatorMeal vs the prey's
//                 reserves, death at predatorMaxAge — bit for bit
//   pair_gates    fixtures: the maxPopulation cap, the hard distance gate
//                 (reproSoftWidth 0: refuses far pairs, no draw) and the grace
//                 bypass (young founders only), with their counters
//   fast_tanh     cfg.fastTanh reaches the Ocean net: actions == fastTanhApprox
//   child         child depth == the parents' midpoint, bodyBuoy within
//                 ±buoyMutation of their mean (clamped to [0, 1]), on and
//                 across the toroidal seam
//   draws_*       the rng draw count per step does not depend on values: fish
//                 motion (RANDOM_SENSOR + MOVE_RANDOM's three draws, across
//                 depth / vz / bodyBuoy / every vertical knob), a birth (across
//                 buoyMutation incl. 0, bodyBuoy, depth), predators (wander,
//                 bud, floor refill, across depth / pitch / D), food spawn
//                 (across foodDepthMode / spread)
// E1 physics (the E1 design §6):
//   divergence    central-difference div of each wave train, their sum, the
//                 depth-weighted current and the whole flow: normalised
//                 residual < 1e-3, shrinking with h (bulk and boundary bands);
//                 exactly zero normal flow at surface and floor; flowAt (one
//                 shared depth taper per call) == the current + each train's
//                 own waveTrainAt, bit for bit
//   e1_buoyancy   with the non-flow E1 knobs on and wallBand 0, an idle fish
//                 still settles monotonically at bodyBuoy·D
//   drift         sizeDrift: one step moves fish of size .5 / 1 / 2 by exactly
//                 1 / 1 / .5 of dt·flow at their spot; over 200 steps the big
//                 one has drifted about half as far; off: all alike
//   costs         depthCostScaled: neutral / surface / deep fixtures equal
//                 dt·M·(cHold·H + cPress·P) bit for bit, ∝ bodySize, timers and
//                 gene / move costs untouched; the preview formula unchanged
//   fight         the scaled overload: (1, 1) == the Classic fight bit for bit
//                 (rng too), known draws, ties go to c2, L² decides most fights
//   mouth         mouthGate: food below / equal to kMouth·L is eaten, above
//                 is not, and is not smelled either
//   e1_neutral    knobs that must not change a run (light, wave shape with no
//                 waves, scaled costs at 0, mouth / fight / drift at size 1)
//                 leave every dynamic hash identical, step by step
//   e1_bounds     E1 worlds keep depth in [0, D], every float finite
//   draws_e1      no E1 feature draws: per-step draws == N + 3·MOVE_RANDOM +
//                 2·predators with everything on; a birth draws as without
//   light         lightAt(0) == L0, falls as L0·exp(−kWater·d); the exports
//   ranges        every Ocean knob, and the SimConfig keys the Ocean bounds
//                 (oceanSimConfigError: the current, foodPatchRadius): its
//                 range's ends accepted, just outside, inf and the -1e37 hole
//                 refused (oc_init, oc_set_param); a run at each end stays
//                 finite; bool / mode keys strict
//   e1_hash       a wave-phase or E1-knob ulp changes only its own section
//   preset        kE1PresetJson is accepted and is what it says
//   wrap          OceanWorld::wrapCoord (constant time) == the loops it
//                 replaced, bit for bit, at every edge of [-span, 2·span) and
//                 [0, span) and on sweeps (multi-pass ones too, whole-number
//                 spans); far values land in [0, span) as the exact remainder;
//                 inf / NaN end; the config that hung (currentStrength 1e20,
//                 validation bypassed) steps with every position in the world
//
// `ocean_tests --accepts SEED STEPS [driver args]` instead asks the shim
// whether it would accept one ocean_parity_driver case (see acceptsMain).
//
// Build with -DOCEAN_TESTS_NO_SHIM to leave out the shim-based checks.
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <limits>
#include <set>
#include <utility>
#include <string>
#include <unordered_map>
#include <vector>
#include "../tools/ocean_fingerprint.hpp"
#ifndef OCEAN_TESTS_NO_SHIM
#include "../wasm/ocean_wasm.cpp"
#endif

namespace ot {

int g_passed = 0, g_failed = 0;

struct Test {
    const char* name;
    long checks = 0, fails = 0;
    std::string first;
    std::vector<std::string> notes;
    explicit Test(const char* n) : name(n) {}
    __attribute__((format(printf, 3, 4)))
    bool expect(bool ok, const char* fmt, ...) {
        ++checks;
        if (ok) return true;
        if (fails++ == 0) {
            char buf[600];
            va_list ap;
            va_start(ap, fmt);
            std::vsnprintf(buf, sizeof buf, fmt, ap);
            va_end(ap);
            first = buf;
        }
        return false;
    }
    __attribute__((format(printf, 2, 3)))
    void note(const char* fmt, ...) {
        char buf[600];
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(buf, sizeof buf, fmt, ap);
        va_end(ap);
        notes.push_back(buf);
    }
    ~Test() {
        if (fails) { ++g_failed; std::printf("FAIL %-14s %ld of %ld checks failed; first: %s\n", name, fails, checks, first.c_str()); }
        else       { ++g_passed; std::printf("ok   %-14s %ld checks\n", name, checks); }
        for (const std::string& n : notes) std::printf("     %-14s %s\n", "", n.c_str());
        std::fflush(stdout);
    }
};

bool finite(float v) { return std::isfinite(v); }

// ---- small worlds built by hand --------------------------------------------

// A gene into an ACTION: from a sensor (srcSensor) or from hidden neuron `src`.
// An undriven hidden neuron outputs a fixed 0.5, so neuron 0 -> action with
// weight w gives the action tanh(0.5 · w / 8192).
Gene mkGene(bool srcSensor, int src, int action, int weight) {
    Gene g;
    g.sourceType = srcSensor ? SOURCE_SENSOR : SOURCE_NEURON;
    g.sourceNum = (uint16_t)src;
    g.sinkType = SINK_ACTION;
    g.sinkNum = (uint16_t)action;
    g.weight = (int16_t)weight;
    return g;
}
constexpr int kStrong = 32767;   // neuron 0 (0.5) x 4.0 = 2.0 -> tanh 0.964

// No food, no spawn, no reseed floor, no predators, no current, no hunger, no
// old age: only what a test adds moves the rng or the state.
void initQuiet(OceanWorld& w, unsigned seed, float D, const OceanConfig& oc, bool capture = false,
               bool toroidal = false, float currentStrength = 0.0f) {
    SimConfig sc;
    applyOceanDefaults(sc);
    if (currentStrength > 0.0f) sc.currentStrength = currentStrength;   // eddies (draws at init only)
    sc.minPopulation = 0;
    sc.targetFood = 0;
    sc.foodSpawnBase = 0.0f;
    sc.foodSpawnAmp = 0.0f;
    sc.metabolicRate = 0.0f;
    sc.maxAge = 1e9f;
    sc.predatorCount = 0;
    sc.captureActivations = capture;
    sc.toroidal = toroidal;
    rng.seed(seed);
    w.cfg = sc;
    w.ocfg = oc;
    w.ocfg.depth = D;
    w.init(1920, 1080, D, 0);
}

// Appends a founder (Fish::init draws its heading and oscillator phase) and
// returns its index.
int addFish(OceanWorld& w, Vec2 pos, float z, std::vector<Gene> genome, float bodyBuoy) {
    Fish f;
    f.genome = std::move(genome);
    f.init(pos, z);
    f.bodyBuoy = bodyBuoy;
    w.creatures.push_back(std::move(f));
    w.markFounder(w.creatures.back());
    return (int)w.creatures.size() - 1;
}

// ---- counting engine draws -------------------------------------------------

bool sameEngine(const RandomGen& a, const RandomGen& b) {
    RandomGen x = a, y = b;
    for (int i = 0; i < 3; ++i)
        if (x.uint() != y.uint()) return false;
    return a.state() == b.state();
}

// How many engine draws take `before` to `after` (-1 if not within maxDraws).
long drawsBetween(const RandomGen& before, const RandomGen& after, long maxDraws = 100000) {
    RandomGen p = before;
    for (long k = 0; k <= maxDraws; ++k) {
        if (sameEngine(p, after)) return k;
        p.uint();
    }
    return -1;
}

// ---- tests -----------------------------------------------------------------

void testBounds() {
    Test t("bounds");
    struct V { const char* name; unsigned seed; float D; const char* cfg; };
    const V vs[] = {
        {"default", 1, 240.0f, nullptr},
        {"steep/strong buoy/wide walls/no drag", 2, 240.0f,
         R"({"pitchMax":4,"kBuoy":25,"wallBand":100,"vDrag":0})"},
        {"tiny pitch/anti-buoy/drag 200/deep/toroidal", 3, 2000.0f,
         R"({"pitchMax":0.01,"kBuoy":-5,"vDrag":200,"wallBand":0,"buoyMutation":0.5,"toroidal":true})"},
        {"D=10/costs/patch depths", 4, 10.0f,
         R"({"pitchMax":1.5,"wallBand":5,"cHold":0.5,"cPress":0.3,"foodDepthMode":1,"foodPatches":4,"foodDepthSpread":500})"},
        {"predators+economy+currents", 5, 240.0f,
         R"({"predatorCount":6,"predatorEconomy":true,"predatorMinPopulation":2,"predatorInheritance":true,"currentStrength":80,"foodDrift":true,"foodPatches":5,"foodDepthMode":1})"},
        {"vDrag 300 (overshooting)", 6, 240.0f, R"({"vDrag":300,"pitchMax":2,"kBuoy":-20,"wallBand":30})"},
    };
    long fishSeen = 0, atSurface = 0, atFloor = 0, atPitch = 0;
    for (const V& v : vs) {
        OceanWorld w;
        std::string err;
        if (!t.expect(ofp::initWorld(w, v.seed, 1920, 1080, v.D, 300, v.cfg, err), "%s: init refused: %s", v.name, err.c_str()))
            continue;
        const float pm = w.ocfg.pitchMax;
        for (int s = 1; s <= 1200; ++s) {
            w.update(0.016f);
            for (const Fish& c : w.creatures) {
                ++fishSeen;
                atSurface += (c.depth == 0.0f);
                atFloor += (c.depth == w.D);
                atPitch += (std::abs(c.vz) == pm);
                t.expect(c.depth >= 0.0f && c.depth <= w.D, "%s step %d: fish %llu depth %.9g outside [0, %g]",
                         v.name, s, (unsigned long long)c.id, c.depth, w.D);
                t.expect(std::abs(c.vz) <= pm, "%s step %d: fish %llu |vz| %.9g > pitchMax %g",
                         v.name, s, (unsigned long long)c.id, c.vz, pm);
                t.expect(c.bodyBuoy >= 0.0f && c.bodyBuoy <= 1.0f, "%s step %d: bodyBuoy %.9g", v.name, s, c.bodyBuoy);
                t.expect(finite(c.pos.x) && finite(c.pos.y) && finite(c.speed) && finite(c.foodReserves),
                         "%s step %d: non-finite fish state", v.name, s);
            }
            for (const OceanFood& f : w.food)
                t.expect(f.depth >= 0.0f && f.depth <= w.D, "%s step %d: food depth %.9g", v.name, s, f.depth);
            for (const OceanPredator& p : w.predators)
                t.expect(p.depth >= 0.0f && p.depth <= w.D && std::abs(p.pitch) <= 0.3f,
                         "%s step %d: predator depth %.9g pitch %.9g", v.name, s, p.depth, p.pitch);
        }
    }
    t.note("%zu configs x 1200 steps, %ld fish-steps; %ld at the surface, %ld on the floor, %ld at |vz| == pitchMax",
           sizeof vs / sizeof vs[0], fishSeen, atSurface, atFloor, atPitch);
    t.expect(atSurface > 0 && atFloor > 0 && atPitch > 0, "the limits were never reached: the bounds checks did not bite");
}

void testBuoyancy() {
    Test t("buoyancy");
    OceanConfig oc;   // kBuoy 1, vDrag 2, pitchMax 0.5, wallBand 12
    OceanWorld w;
    initQuiet(w, 11, 240.0f, oc);
    struct F { int idx; float target; };
    const F fs[] = {
        {addFish(w, {300, 300}, 24.0f, {}, 0.5f), 120.0f},    // above neutral: sinks
        {addFish(w, {900, 700}, 216.0f, {}, 0.5f), 120.0f},   // below neutral: rises
        {addFish(w, {1500, 500}, 192.0f, {}, 0.2f), 48.0f},   // a long climb to 0.2·D
    };
    const int still = addFish(w, {600, 900}, 120.0f, {}, 0.5f);   // already neutral
    float off[3];
    for (int k = 0; k < 3; ++k) off[k] = w.creatures[fs[k].idx].depth - fs[k].target;
    for (int s = 1; s <= 3000; ++s) {
        w.update(0.016f);
        if (!t.expect(w.creatures.size() == 4, "step %d: %zu fish (none should die or be born)", s, w.creatures.size()))
            return;
        for (int k = 0; k < 3; ++k) {
            const float o = w.creatures[fs[k].idx].depth - fs[k].target;
            t.expect(std::abs(o) <= std::abs(off[k]), "fish %d step %d: |offset| grew %.9g -> %.9g", k, s, off[k], o);
            t.expect(o == 0.0f || (o > 0.0f) == (off[k] > 0.0f) || off[k] == 0.0f,
                     "fish %d step %d: overshot neutral (%.9g -> %.9g)", k, s, off[k], o);
            off[k] = o;
        }
        t.expect(w.creatures[still].depth == 120.0f && w.creatures[still].vz == 0.0f,
                 "step %d: a neutral idle fish moved (depth %.9g vz %.9g)", s, w.creatures[still].depth,
                 w.creatures[still].vz);
    }
    for (int k = 0; k < 3; ++k)
        t.expect(std::abs(off[k]) < 0.5f, "fish %d still %.4f from bodyBuoy·D after 3000 steps", k, off[k]);
    t.note("offsets after 3000 steps: %.4f %.4f %.4f (from -96, +96, +144)", off[0], off[1], off[2]);

    // kBuoy = 0: an idle fish holds its depth exactly.
    OceanConfig oc0;
    oc0.kBuoy = 0.0f;
    OceanWorld w0;
    initQuiet(w0, 12, 240.0f, oc0);
    const int a = addFish(w0, {500, 500}, 60.0f, {}, 0.9f);
    for (int s = 0; s < 500; ++s) w0.update(0.016f);
    t.expect(w0.creatures[a].depth == 60.0f && w0.creatures[a].vz == 0.0f,
             "kBuoy 0: idle fish moved to %.9g (vz %.9g)", w0.creatures[a].depth, w0.creatures[a].vz);
}

void testSteerZ() {
    Test t("steer_z");
    OceanConfig oc;
    oc.kBuoy = 0.0f;   // isolate the action: no buoyancy, no walls
    oc.wallBand = 0.0f;
    OceanWorld w;
    initQuiet(w, 21, 240.0f, oc, /*capture*/ true);
    const int dn = addFish(w, {300, 300}, 120.0f, {mkGene(false, 0, STEER_Z, 16384)}, 0.5f);
    const int idle = addFish(w, {900, 300}, 120.0f, {}, 0.5f);
    const int up = addFish(w, {1500, 300}, 120.0f, {mkGene(false, 0, STEER_Z, -16384)}, 0.5f);
    // A sensor-driven wire: deeper -> more negative STEER_Z -> rises.
    const int loc = addFish(w, {300, 800}, 200.0f, {mkGene(true, LOC_Z, STEER_Z, -kStrong)}, 0.5f);
    for (int s = 0; s < 60; ++s) w.update(0.016f);
    const Fish &D = w.creatures[dn], &I = w.creatures[idle], &U = w.creatures[up], &L = w.creatures[loc];
    t.expect(D.depth > 130.0f, "STEER_Z > 0 did not dive: depth %.4f", D.depth);
    t.expect(U.depth < 110.0f, "STEER_Z < 0 did not rise: depth %.4f", U.depth);
    t.expect(I.depth == 120.0f, "unwired fish moved: depth %.9g", I.depth);
    t.expect(D.vz > 0.0f && U.vz < 0.0f && std::abs(D.vz) <= oc.pitchMax && std::abs(U.vz) <= oc.pitchMax,
             "vz signs/limits: dive %.6f rise %.6f", D.vz, U.vz);
    t.expect(std::abs((D.depth - 120.0f) - (120.0f - U.depth)) < 1e-3f,
             "dive and rise not symmetric: +%.6f vs -%.6f", D.depth - 120.0f, 120.0f - U.depth);
    t.expect(D.lastActions[STEER_Z] == evomath::tanhf(1.0f) && U.lastActions[STEER_Z] == -evomath::tanhf(1.0f),
             "STEER_Z action %.9g / %.9g, expected ±tanh(1) = %.9g", D.lastActions[STEER_Z],
             U.lastActions[STEER_Z], evomath::tanhf(1.0f));
    t.expect(L.depth < 190.0f && L.lastActions[STEER_Z] < 0.0f,
             "LOC_Z -> STEER_Z (-) did not raise the fish: depth %.4f action %.6f", L.depth, L.lastActions[STEER_Z]);
    t.note("after 60 steps: dive %.2f, idle %.2f, rise %.2f, LOC_Z-wired %.2f (from 200)", D.depth, I.depth, U.depth, L.depth);
}

void testSensors() {
    Test t("sensors");
    struct V { unsigned seed; float D; const char* cfg; };
    const V vs[] = {
        {31, 240.0f, R"({"captureActivations":true})"},
        {32, 600.0f, R"({"pitchMax":2.5,"kBuoy":-3,"wallBand":50,"maxNeighbors":7,"toroidal":true,"foodPatches":3,"foodDepthMode":1,"predatorCount":3})"},
        {33, 10.0f, R"({"pitchMax":0.05,"vDrag":0,"wallBand":0,"foodDepthSpread":0,"foodDepthMode":1,"foodPatches":2})"},
    };
    const float tanh1 = evomath::tanhf(1.0f), tanh2 = evomath::tanhf(2.0f);
    long fishChecked = 0, sawFood = 0, sawNeighbor = 0;
    for (const V& v : vs) {
        OceanWorld w;
        std::string err;
        if (!t.expect(ofp::initWorld(w, v.seed, 1920, 1080, v.D, 400, v.cfg, err), "init: %s", err.c_str())) continue;
        w.cfg.captureActivations = true;   // not a checkpoint key: set it directly
        const float D = w.D, pm = w.ocfg.pitchMax, R = w.cfg.perceptionRadius;
        std::unordered_map<std::uint64_t, std::pair<float, float>> before;
        for (int s = 1; s <= 600; ++s) {
            before.clear();
            for (const Fish& c : w.creatures) before[c.id] = {c.depth, c.vz};
            w.update(0.016f);
            for (const Fish& c : w.creatures) {
                auto it = before.find(c.id);
                if (it == before.end() || !c.alive) continue;   // born or reseeded this step: no sensing yet
                ++fishChecked;
                const float z = it->second.first, vz = it->second.second;
                const float* S = c.lastSensors;
                bool allFinite = true;
                for (int k = 0; k < NUM_SENSES; ++k) allFinite &= finite(S[k]);
                t.expect(allFinite, "seed %u step %d: a non-finite sensor", v.seed, s);
                t.expect(S[LOC_Z] == z / D && S[LOC_Z] >= 0.0f && S[LOC_Z] <= 1.0f,
                         "LOC_Z %.9g != depth/D %.9g", S[LOC_Z], z / D);
                t.expect(S[OWN_VEL_Z] == vz / pm && std::abs(S[OWN_VEL_Z]) <= 1.0f,
                         "OWN_VEL_Z %.9g != vz/pitchMax %.9g", S[OWN_VEL_Z], vz / pm);
                const float bz = evomath::tanhf(std::min(z, D - z) / (0.25f * D));
                t.expect(S[BOUNDARY_DIST_Z] == bz && S[BOUNDARY_DIST_Z] >= 0.0f && S[BOUNDARY_DIST_Z] <= tanh2,
                         "BOUNDARY_DIST_Z %.9g expected %.9g", S[BOUNDARY_DIST_Z], bz);
                t.expect(std::abs(S[NEAREST_FOOD_DZ]) <= 1.0f, "NEAREST_FOOD_DZ %.9g", S[NEAREST_FOOD_DZ]);
                t.expect(std::abs(S[AVG_NEIGHBOR_DZ]) <= tanh1, "AVG_NEIGHBOR_DZ %.9g > tanh(1)", S[AVG_NEIGHBOR_DZ]);
                if (S[NEAREST_FOOD_DIST] < 1.0f) {
                    ++sawFood;
                    const float n2 = S[NEAREST_FOOD_DX] * S[NEAREST_FOOD_DX] + S[NEAREST_FOOD_DY] * S[NEAREST_FOOD_DY] +
                                     S[NEAREST_FOOD_DZ] * S[NEAREST_FOOD_DZ];
                    t.expect(std::abs(n2 - 1.0f) < 1e-5f || S[NEAREST_FOOD_DIST] * R <= 0.001f,
                             "food direction not a unit vector: |d|^2 = %.9g", n2);
                }
                if (S[NEAREST_NEIGHBOR_DIST] < 1.0f) ++sawNeighbor;
            }
        }
    }
    t.expect(sawFood > 1000 && sawNeighbor > 1000, "too few fish saw food (%ld) / a neighbour (%ld) to test", sawFood, sawNeighbor);
    t.note("%ld fish-steps; %ld saw food, %ld a neighbour", fishChecked, sawFood, sawNeighbor);
}

bool sameDynamics(const ofp::Fingerprint& a, const ofp::Fingerprint& b) {
    return a.ctr == b.ctr && a.fish == b.fish && a.food == b.food && a.pred == b.pred && a.sig == b.sig &&
           a.cur == b.cur && a.geo == b.geo && a.rng == b.rng;
}

// One step of hand-placed fish; sensors are read before anything moves (a fish
// senses at the start of its own stepFish, and these fixtures do not move
// vertically: kBuoy 0, no walls, no wiring unless a test adds it).
void testSensorSigns() {
    Test t("sensor_signs");
    OceanConfig oc;
    oc.kBuoy = 0.0f;
    oc.wallBand = 0.0f;
    OceanWorld w;
    initQuiet(w, 1301, 240.0f, oc, /*capture*/ true);
    const float D = w.D, R = w.cfg.perceptionRadius, pm = w.ocfg.pitchMax;
    // Fixtures >= 400 apart (> R): each sees only its own food / neighbour.
    struct FoodFix { Vec2 at; float z; Vec2 off; float dz; };
    const FoodFix ff[] = {
        {{200, 200}, 60.0f, {0, 0}, 50.0f},      // food straight below: DZ = +1
        {{600, 200}, 180.0f, {0, 0}, -50.0f},    // straight above: DZ = -1
        {{1000, 200}, 100.0f, {30, 0}, 40.0f},   // (30, 0, 40): DX .6, DZ .8
        {{1400, 200}, 200.0f, {0, -30}, -40.0f}, // (0, -30, -40): DY -.6, DZ -.8
    };
    int foodIdx[4];
    for (int k = 0; k < 4; ++k) {
        foodIdx[k] = addFish(w, ff[k].at, ff[k].z, {}, ff[k].z / D);
        OceanFood f;
        f.pos = {ff[k].at.x + ff[k].off.x, ff[k].at.y + ff[k].off.y};
        f.depth = ff[k].z + ff[k].dz;
        w.food.push_back(f);
    }
    // A pair 40 apart in depth (5 apart horizontally): the upper one's neighbours are deeper.
    const int up = addFish(w, {200, 700}, 100.0f, {}, 100.0f / D);
    const int dn = addFish(w, {205, 700}, 140.0f, {}, 140.0f / D);
    // vz preset: diving (+) and rising (-).
    const int dive = addFish(w, {600, 700}, 120.0f, {}, 0.5f);
    const int rise = addFish(w, {1000, 700}, 120.0f, {}, 0.5f);
    w.creatures[dive].vz = 0.25f;
    w.creatures[rise].vz = -0.25f;
    // BOUNDARY_DIST_Z along the column: surface, quarter, middle, three quarters, floor.
    const float bzDepth[5] = {0.0f, 60.0f, 120.0f, 180.0f, 240.0f};
    int bz[5];
    for (int k = 0; k < 5; ++k) bz[k] = addFish(w, {1400.0f + 100.0f * (k % 2), 600.0f + 110.0f * k}, bzDepth[k], {}, bzDepth[k] / D);
    // Sensor -> action: a NEAREST_FOOD_DZ -> STEER_Z (+) fish with food below, one with food above.
    const int seekDn = addFish(w, {1800, 200}, 100.0f, {mkGene(true, NEAREST_FOOD_DZ, STEER_Z, kStrong)}, 100.0f / D);
    const int seekUp = addFish(w, {1800, 900}, 140.0f, {mkGene(true, NEAREST_FOOD_DZ, STEER_Z, kStrong)}, 140.0f / D);
    for (const auto& fz : {std::pair<Vec2, float>{{1800, 200}, 150.0f}, std::pair<Vec2, float>{{1800, 900}, 90.0f}}) {
        OceanFood f;
        f.pos = fz.first;
        f.depth = fz.second;
        w.food.push_back(f);
    }
    std::vector<Fish> before = w.creatures;
    w.update(0.016f);
    auto S = [&](int i) { return w.creatures[i].lastSensors; };

    for (int k = 0; k < 4; ++k) {
        const Fish& c = before[foodIdx[k]];
        const float dx = ff[k].off.x, dy = ff[k].off.y, dz = ff[k].dz;
        const float dist = std::sqrt(dx * dx + dy * dy + dz * dz), inv = 1.0f / dist;
        const float* s = S(foodIdx[k]);
        t.expect(s[NEAREST_FOOD_DX] == dx * inv && s[NEAREST_FOOD_DY] == dy * inv && s[NEAREST_FOOD_DZ] == dz * inv,
                 "food fixture %d: (%.9g, %.9g, %.9g), expected (%.9g, %.9g, %.9g)", k, s[NEAREST_FOOD_DX],
                 s[NEAREST_FOOD_DY], s[NEAREST_FOOD_DZ], dx * inv, dy * inv, dz * inv);
        t.expect((s[NEAREST_FOOD_DZ] > 0.0f) == (dz > 0.0f) && s[NEAREST_FOOD_DZ] != 0.0f,
                 "food fixture %d: NEAREST_FOOD_DZ %.9g has the wrong sign (food is %s)", k, s[NEAREST_FOOD_DZ],
                 dz > 0.0f ? "deeper" : "shallower");
        t.expect(s[NEAREST_FOOD_DIST] == dist / R, "food fixture %d: NEAREST_FOOD_DIST %.9g != %.9g", k,
                 s[NEAREST_FOOD_DIST], dist / R);
        t.expect(s[LOC_Z] == c.depth / D, "food fixture %d: LOC_Z %.9g != depth/D %.9g", k, s[LOC_Z], c.depth / D);
    }
    t.expect(S(foodIdx[0])[NEAREST_FOOD_DZ] == 1.0f && S(foodIdx[1])[NEAREST_FOOD_DZ] == -1.0f,
             "straight below / above: DZ %.9g / %.9g, expected +1 / -1", S(foodIdx[0])[NEAREST_FOOD_DZ],
             S(foodIdx[1])[NEAREST_FOOD_DZ]);
    t.expect(S(foodIdx[0])[LOC_Z] == 0.25f && S(foodIdx[1])[LOC_Z] == 0.75f, "LOC_Z at 60 / 180 of 240: %.9g / %.9g",
             S(foodIdx[0])[LOC_Z], S(foodIdx[1])[LOC_Z]);

    // AVG_NEIGHBOR_DZ: tanh(mean dz / R), + when the neighbours are deeper.
    t.expect(S(up)[AVG_NEIGHBOR_DZ] == evomath::tanhf(40.0f / R) && S(up)[AVG_NEIGHBOR_DZ] > 0.0f,
             "upper fish: AVG_NEIGHBOR_DZ %.9g, expected +tanh(0.4) = %.9g", S(up)[AVG_NEIGHBOR_DZ], evomath::tanhf(40.0f / R));
    t.expect(S(dn)[AVG_NEIGHBOR_DZ] == evomath::tanhf(-40.0f / R) && S(dn)[AVG_NEIGHBOR_DZ] < 0.0f,
             "lower fish: AVG_NEIGHBOR_DZ %.9g, expected -tanh(0.4)", S(dn)[AVG_NEIGHBOR_DZ]);
    t.expect(S(dive)[AVG_NEIGHBOR_DZ] == 0.0f && S(dive)[NEAREST_FOOD_DZ] == 0.0f,
             "a lone fish: AVG_NEIGHBOR_DZ %.9g NEAREST_FOOD_DZ %.9g (expected 0)", S(dive)[AVG_NEIGHBOR_DZ],
             S(dive)[NEAREST_FOOD_DZ]);

    // OWN_VEL_Z: vz / pitchMax, + = diving; a diving fish gets deeper (LOC_Z up).
    t.expect(S(dive)[OWN_VEL_Z] == 0.25f / pm && S(dive)[OWN_VEL_Z] > 0.0f, "diving: OWN_VEL_Z %.9g, expected +%.9g",
             S(dive)[OWN_VEL_Z], 0.25f / pm);
    t.expect(S(rise)[OWN_VEL_Z] == -0.25f / pm, "rising: OWN_VEL_Z %.9g, expected %.9g", S(rise)[OWN_VEL_Z], -0.25f / pm);
    t.expect(w.creatures[dive].depth > 120.0f && w.creatures[rise].depth < 120.0f,
             "+vz must dive (depth 120 -> %.6f) and -vz rise (-> %.6f)", w.creatures[dive].depth, w.creatures[rise].depth);

    // BOUNDARY_DIST_Z = tanh(min(z, D - z) / (D/4)): 0 at both walls, largest mid-column, symmetric.
    for (int k = 0; k < 5; ++k) {
        const float z = bzDepth[k];
        const float e = evomath::tanhf(std::min(z, D - z) / (0.25f * D));
        t.expect(S(bz[k])[BOUNDARY_DIST_Z] == e, "BOUNDARY_DIST_Z at %g: %.9g, expected %.9g", z, S(bz[k])[BOUNDARY_DIST_Z], e);
    }
    t.expect(S(bz[0])[BOUNDARY_DIST_Z] == 0.0f && S(bz[4])[BOUNDARY_DIST_Z] == 0.0f &&
                 S(bz[1])[BOUNDARY_DIST_Z] == S(bz[3])[BOUNDARY_DIST_Z] &&
                 S(bz[2])[BOUNDARY_DIST_Z] > S(bz[1])[BOUNDARY_DIST_Z] && S(bz[1])[BOUNDARY_DIST_Z] > 0.0f,
             "BOUNDARY_DIST_Z along the column: %.6f %.6f %.6f %.6f %.6f", S(bz[0])[BOUNDARY_DIST_Z],
             S(bz[1])[BOUNDARY_DIST_Z], S(bz[2])[BOUNDARY_DIST_Z], S(bz[3])[BOUNDARY_DIST_Z], S(bz[4])[BOUNDARY_DIST_Z]);

    // The sensor's meaning end to end: wired (+) to STEER_Z it steers toward the food.
    t.expect(S(seekDn)[NEAREST_FOOD_DZ] > 0.0f && S(seekUp)[NEAREST_FOOD_DZ] < 0.0f,
             "seekers: DZ %.6f (food below) / %.6f (food above)", S(seekDn)[NEAREST_FOOD_DZ], S(seekUp)[NEAREST_FOOD_DZ]);
    for (int s = 0; s < 9; ++s) w.update(0.016f);
    t.expect(w.creatures[seekDn].depth > 100.0f && w.creatures[seekUp].depth < 140.0f,
             "NEAREST_FOOD_DZ -> STEER_Z (+) did not move toward the food: %.4f (from 100, food at 150), %.4f (from 140, food at 90)",
             w.creatures[seekDn].depth, w.creatures[seekUp].depth);
    t.note("DZ +1 / -1 / .8 / -.8 as placed; AVG_NEIGHBOR_DZ ±tanh(.4); seekers after 10 steps at %.2f / %.2f",
           w.creatures[seekDn].depth, w.creatures[seekUp].depth);
}

// CURRENT_FWD / CURRENT_LR with the 3-D flow on.
void testCurrentSensors() {
    Test t("current_sens");
    for (int waves = 0; waves < 2; ++waves) {
        SimConfig sc;
        applyOceanDefaults(sc);
        sc.minPopulation = 0; sc.targetFood = 0; sc.foodSpawnBase = 0.0f; sc.foodSpawnAmp = 0.0f;
        sc.metabolicRate = 0.0f; sc.maxAge = 1e9f; sc.predatorCount = 0;
        sc.captureActivations = true;
        sc.currentStrength = 60.0f;
        sc.currentDrift = 0.0f;   // a static field: the current a fish senses is currentAt() before and after
        OceanConfig oc;
        oc.deepCurrent = true;    // flowE1(): the sensors read flowAt
        oc.waveSpeed = waves ? 6.0f : 0.0f;
        oc.kBuoy = 0.0f;
        oc.wallBand = 0.0f;
        OceanWorld w;
        rng.seed(1311);
        w.cfg = sc;
        w.ocfg = oc;
        w.init(1920, 1080, 240.0f, 0);
        w.update(0.016f);   // the first advance() places the eddy centres
        // The strongest current on a coarse lattice: a clear direction to align with.
        Vec2 at{0, 0}, cur{0, 0};
        float best = -1.0f;
        for (int j = 1; j < 10; ++j)
            for (int i = 1; i < 18; ++i) {
                const Vec2 p{(float)i * 100.0f, (float)j * 100.0f};
                const Vec2 c = w.currentAt(p);
                const float m = c.x * c.x + c.y * c.y;
                if (m > best) { best = m; at = p; cur = c; }
            }
        const float len = std::sqrt(best);
        const Vec2 u{cur.x / len, cur.y / len};
        // Fixtures at that spot, 60+ apart in depth (> interactRadius): along the
        // current, against it, and rotated so the current is on their right.
        struct Fx { const char* what; float z; Vec2 head; };
        const Fx fx[] = {
            {"along, mid-column", 120.0f, u}, {"against, mid-column", 180.0f, {-u.x, -u.y}},
            {"current on the right", 60.0f, {u.y, -u.x}}, {"along, at the surface", 0.0f, u},
            {"along, deep", 240.0f, u},
        };
        constexpr int NF = 5;
        int idx[NF];
        Vec2 fwd[NF];
        for (int k = 0; k < NF; ++k) {
            idx[k] = addFish(w, at, fx[k].z, {}, fx[k].z / 240.0f);
            w.creatures[idx[k]].vel = fx[k].head;
            fwd[k] = w.creatures[idx[k]].forward();
        }
        w.update(0.016f);
        for (int k = 0; k < NF; ++k) {
            const float* s = w.creatures[idx[k]].lastSensors;
            const float z = fx[k].z;
            OceanWorld::Flow U;
            if (waves) {
                U = w.flowAt(at, z);
            } else {   // independent of flowAt: f(d) = smoothstep(2d/D) on the 2-D current
                const float f = (z <= 0.0f) ? 0.0f : (z >= 120.0f) ? 1.0f : (z == 60.0f ? 0.5f : -1.0f);
                U.x = f * cur.x;
                U.y = f * cur.y;
            }
            const Vec2 r{-fwd[k].y, fwd[k].x};
            const float eF = evomath::tanhf((U.x * fwd[k].x + U.y * fwd[k].y) / 100.0f);
            const float eL = evomath::tanhf((U.x * r.x + U.y * r.y) / 100.0f);
            t.expect(s[CURRENT_FWD] == eF && s[CURRENT_LR] == eL, "%s, %s: FWD %.9g LR %.9g, expected %.9g %.9g",
                     waves ? "waves" : "deep current", fx[k].what, s[CURRENT_FWD], s[CURRENT_LR], eF, eL);
        }
        const float* along = w.creatures[idx[0]].lastSensors;
        const float* against = w.creatures[idx[1]].lastSensors;
        const float* right = w.creatures[idx[2]].lastSensors;
        const float* surf = w.creatures[idx[3]].lastSensors;
        t.expect(along[CURRENT_FWD] > 0.1f && against[CURRENT_FWD] < -0.1f && right[CURRENT_LR] > 0.05f,
                 "%s: signs: along FWD %.6f, against FWD %.6f, on the right LR %.6f", waves ? "waves" : "deep current",
                 along[CURRENT_FWD], against[CURRENT_FWD], right[CURRENT_LR]);
        if (!waves) {
            t.expect(surf[CURRENT_FWD] == 0.0f && surf[CURRENT_LR] == 0.0f, "deep current at the surface: FWD %.9g LR %.9g",
                     surf[CURRENT_FWD], surf[CURRENT_LR]);
            t.expect(along[CURRENT_FWD] == evomath::tanhf(len / 100.0f) || std::abs(along[CURRENT_FWD] - evomath::tanhf(len / 100.0f)) < 1e-6f,
                     "along the current: FWD %.9g, expected tanh(|U|/100) %.9g", along[CURRENT_FWD], evomath::tanhf(len / 100.0f));
            t.note("strongest current %.2f at (%g, %g): FWD along %.4f, against %.4f; LR on the right %.4f; surface 0",
                   len, at.x, at.y, along[CURRENT_FWD], against[CURRENT_FWD], right[CURRENT_LR]);
        }
    }
}

// The depth wall bands, one step, exactly as specified.
void testWalls() {
    Test t("walls");
    struct V { float D, wallBand, z; int sign; };
    const V vs[] = {
        {240.0f, 12.0f, 2.0f, +1}, {240.0f, 12.0f, 238.0f, -1}, {240.0f, 12.0f, 120.0f, 0},
        {240.0f, 12.0f, 0.0f, +1}, {240.0f, 12.0f, 240.0f, -1},
        // A shallow sea: each band is capped at D/2, so depth 9 of 10 is in the FLOOR band.
        {10.0f, 12.0f, 9.0f, -1}, {10.0f, 12.0f, 1.0f, +1},
    };
    long pushes = 0;
    for (const V& v : vs) {
        OceanConfig oc;
        oc.kBuoy = 0.0f;
        oc.vDrag = 0.0f;
        oc.wallBand = v.wallBand;
        OceanWorld w;
        initQuiet(w, 1321, v.D, oc);
        const int i = addFish(w, {500, 500}, v.z, {}, 0.5f);
        const float speed = w.creatures[i].speed, dt = 0.016f;
        w.update(dt);
        const Fish& c = w.creatures[i];
        const float wb = std::min(v.wallBand, 0.5f * v.D);
        float vz = 0.0f;
        if (v.z < wb) vz += (1.0f - v.z / wb) * 3.0f * dt;
        else if (v.z > v.D - wb) vz -= (1.0f - (v.D - v.z) / wb) * 3.0f * dt;
        vz = std::clamp(vz, -oc.pitchMax, oc.pitchMax);
        const float z = std::clamp(v.z + vz * speed * dt, 0.0f, v.D);
        t.expect(c.vz == vz && c.depth == z, "D %g band %g depth %g: vz %.9g depth %.9g, expected %.9g %.9g", v.D, v.wallBand,
                 v.z, c.vz, c.depth, vz, z);
        t.expect((c.vz > 0.0f) - (c.vz < 0.0f) == v.sign, "D %g depth %g: the wall pushed the wrong way (vz %.9g)", v.D, v.z, c.vz);
        pushes += (c.vz != 0.0f);
    }
    t.expect(pushes == 6, "%ld of 6 band fixtures were pushed", pushes);
}

// Stage B predator economy, one step each, bit for bit.
void testPredatorEconomy() {
    Test t("pred_econ");
    const float dt = 0.016f;
    auto world = [&](OceanWorld& w, unsigned seed) {
        initQuiet(w, seed, 240.0f, OceanConfig{});
        w.cfg.predatorEconomy = true;
        w.cfg.moveCost = 7e-5f;
        w.cfg.predatorMetabolism = 2.0f;
        w.cfg.predatorReproEnergy = 1e6f;   // no budding: one predator, its energy as computed
    };
    // Burn: metabolism + coefficient·speed², the coefficient predatorMoveCost (>= 0) or moveCost.
    float energyAfter[2];
    for (int k = 0; k < 2; ++k) {
        const float pmc = k ? 3e-4f : -1.0f;
        OceanWorld w;
        world(w, 1401);
        w.cfg.predatorMoveCost = pmc;
        OceanPredator p;
        p.pos = {500, 500};
        p.depth = 100.0f;
        p.energy = 40.0f;
        p.speedFactor = 1.25f;
        w.predators.push_back(p);
        w.update(dt);
        const float coef = (pmc >= 0.0f) ? pmc : 7e-5f;
        const float speed = w.cfg.predatorSpeed * 1.25f;
        const float burn = w.cfg.predatorMetabolism + coef * speed * speed;
        const float e = 40.0f - burn * dt;
        energyAfter[k] = w.predators[0].energy;
        t.expect(w.predators[0].energy == e && w.predators[0].age == dt, "predatorMoveCost %g: energy %.9g age %.9g, expected %.9g %.9g",
                 pmc, w.predators[0].energy, w.predators[0].age, e, dt);
    }
    t.expect(energyAfter[0] != energyAfter[1], "an explicit predatorMoveCost changed nothing");
    // A capture: + predatorMeal when >= 0, else + the prey's reserves.
    for (float meal : {9.0f, -1.0f}) {
        OceanWorld w;
        world(w, 1402);
        w.cfg.moveCost = 0.0f;   // the prey's reserves stay what they were placed with
        w.cfg.predatorMeal = meal;
        const int prey = addFish(w, {500, 500}, 100.0f, {}, 100.0f / 240.0f);
        w.creatures[prey].foodReserves = 33.0f;
        OceanPredator p;
        p.pos = {505, 500};
        p.depth = 100.0f;
        p.energy = 40.0f;
        w.predators.push_back(p);
        w.update(dt);
        const float burn = w.cfg.predatorMetabolism + 0.0f * w.cfg.predatorSpeed * w.cfg.predatorSpeed;
        const float e = (40.0f - burn * dt) + (meal >= 0.0f ? meal : 33.0f);
        t.expect(w.totalDeathsPredation == 1 && w.creatures.empty() && w.predators.size() == 1,
                 "meal %g: no capture (%lld), %zu predators", meal, w.totalDeathsPredation, w.predators.size());
        t.expect(w.predators[0].energy == e && w.predators[0].digestTimer == w.cfg.predatorCooldown,
                 "meal %g: energy %.9g, expected %.9g", meal, w.predators[0].energy, e);
    }
    // Old age: dead the step its age reaches predatorMaxAge, not before.
    for (float age0 : {0.98f, 0.995f}) {
        OceanWorld w;
        world(w, 1403);
        w.cfg.predatorMaxAge = 1.0f;
        OceanPredator p;
        p.pos = {500, 500};
        p.depth = 100.0f;
        p.energy = 40.0f;
        p.age = age0;
        w.predators.push_back(p);
        w.update(dt);
        const bool dies = age0 + dt >= 1.0f;
        t.expect(w.predators.size() == (dies ? 0u : 1u) && w.totalPredatorDeathsAge == (dies ? 1 : 0) &&
                     w.totalPredatorDeathsStarvation == 0,
                 "age %g + dt vs max 1: %zu predators, %lld age deaths", age0, w.predators.size(), w.totalPredatorDeathsAge);
    }
}

// Two adult, well-fed fish wired to pair, one step, under the pairing gates.
struct GateRun { bool born = false; long draws = -1; long long grace = 0, soft = 0; };
GateRun pairStep(unsigned seed, int maxPop, float T, float W, float grace, bool farGenomes, bool founders, float age) {
    OceanWorld w;
    initQuiet(w, seed, 240.0f, OceanConfig{});
    w.cfg.maxPopulation = maxPop;
    w.cfg.reproMaxDistance = T;
    w.cfg.reproSoftWidth = W;
    w.cfg.reproGracePeriod = grace;
    std::vector<Gene> g1 = {mkGene(false, 0, INTERACT_REPRODUCE, kStrong)}, g2 = g1;
    // An extra wire (its sensor is 0 in the vote) puts (32767/8192)^2 ~ 16 between the genomes.
    if (farGenomes) g2.push_back(mkGene(true, LOC_X, STEER_X, kStrong));
    const int a = addFish(w, {960, 540}, 100.0f, g1, 0.4f), b = addFish(w, {961, 541}, 101.0f, g2, 0.4f);
    for (int i : {a, b}) {
        Fish& f = w.creatures[i];
        f.age = age;
        f.foodReserves = 100.0f;
        f.reproTimer = 0.0f;
        f.interactTimer = 0.0f;
        if (!founders) f.lineageId = f.id + 1000;   // not its own line: no grace
    }
    GateRun r;
    const RandomGen before = rng;
    w.update(0.016f);
    r.draws = drawsBetween(before, rng);
    r.born = w.creatures.size() == 3;
    r.grace = w.totalGraceBirths;
    r.soft = w.totalSoftBirths;
    return r;
}

void testPairingGates() {
    Test t("pair_gates");
    for (unsigned seed : {1501u, 1502u, 1503u}) {
        // The maxPopulation cap: 2 fish + 0 newborns must be < the cap.
        const GateRun capped = pairStep(seed, 2, -1.0f, 0.0f, 0.0f, false, false, 10.0f);
        const GateRun under = pairStep(seed, 3, -1.0f, 0.0f, 0.0f, false, false, 10.0f);
        t.expect(!capped.born && under.born, "seed %u: maxPopulation 2 born %d, 3 born %d", seed, capped.born, under.born);
        // The hard gate (soft width 0): identical genomes pass, far ones do not, and the gate draws nothing.
        const GateRun near = pairStep(seed, 0, 1.0f, 0.0f, 0.0f, false, false, 10.0f);
        const GateRun far = pairStep(seed, 0, 1.0f, 0.0f, 0.0f, true, false, 10.0f);
        const GateRun farCapped = pairStep(seed, 2, 1.0f, 0.0f, 0.0f, true, false, 10.0f);   // refused before the gate
        const GateRun farOff = pairStep(seed, 0, -1.0f, 0.0f, 0.0f, true, false, 10.0f);
        t.expect(near.born && !far.born && farOff.born, "seed %u: hard gate T 1: near born %d, far born %d (gate off: %d)",
                 seed, near.born, far.born, farOff.born);
        t.expect(far.draws == farCapped.draws && near.soft == 0 && near.grace == 0,
                 "seed %u: the hard gate drew (%ld vs %ld draws) or counted soft/grace", seed, far.draws, farCapped.draws);
        // Grace: a young founder bypasses the gate (counted); an older one, or a non-founder, does not.
        const GateRun young = pairStep(seed, 0, 1.0f, 0.0f, 20.0f, true, true, 10.0f);
        const GateRun old = pairStep(seed, 0, 1.0f, 0.0f, 20.0f, true, true, 25.0f);
        const GateRun notFounder = pairStep(seed, 0, 1.0f, 0.0f, 20.0f, true, false, 10.0f);
        t.expect(young.born && young.grace == 1 && !old.born && !notFounder.born,
                 "seed %u: grace 20: young founders born %d (grace %lld), age 25 born %d, non-founders born %d", seed,
                 young.born, young.grace, old.born, notFounder.born);
    }
}

// cfg.fastTanh reaches the Ocean world's net (stepFish -> feedForward).
void testFastTanh() {
    Test t("fast_tanh");
    int differ = 0;
    for (int wgt : {4096, 9000, 16384, 30000}) {
        float act[2];
        for (int fast = 0; fast < 2; ++fast) {
            OceanConfig oc;
            oc.kBuoy = 0.0f;
            OceanWorld w;
            initQuiet(w, 1601, 240.0f, oc, /*capture*/ true);
            w.cfg.fastTanh = (fast == 1);
            const int i = addFish(w, {500, 500}, 120.0f, {mkGene(false, 0, STEER_Z, wgt)}, 0.5f);
            w.update(0.016f);
            act[fast] = w.creatures[i].lastActions[STEER_Z];
        }
        const float x = 0.5f * (float)wgt / 8192.0f;   // undriven neuron 0 (0.5) x weight
        t.expect(act[0] == evomath::tanhf(x) && act[1] == fastTanhApprox(x), "weight %d: actions %.9g / %.9g, expected %.9g / %.9g",
                 wgt, act[0], act[1], evomath::tanhf(x), fastTanhApprox(x));
        differ += (act[0] != act[1]);
    }
    t.expect(differ > 0, "fastTanh on and off gave identical actions: the flag does not reach the net");
}

void testCaptureNeutral() {
    Test t("capture");
    const char* cfg = R"({"predatorCount":3,"currentStrength":50,"foodPatches":3})";
    ofp::Fingerprint fp[2];
    for (int k = 0; k < 2; ++k) {
        OceanWorld w;
        std::string err;
        ofp::initWorld(w, 41, 1920, 1080, 240.0f, 300, cfg, err);
        w.cfg.captureActivations = (k == 1);
        for (int s = 0; s < 800; ++s) w.update(0.016f);
        fp[k] = ofp::fingerprint(w, /*withActivations*/ false);
    }
    t.expect(sameDynamics(fp[0], fp[1]), "captureActivations changed the run (fish %016llx vs %016llx)",
             (unsigned long long)fp[0].fish, (unsigned long long)fp[1].fish);
}

std::vector<ofp::Fingerprint> runPrints(unsigned seed, const char* cfg, int pop, int steps, int every) {
    std::vector<ofp::Fingerprint> out;
    OceanWorld w;
    std::string err;
    if (!ofp::initWorld(w, seed, 1920, 1080, 240.0f, pop, cfg, err)) return out;
    out.push_back(ofp::fingerprint(w));
    for (int s = 1; s <= steps; ++s) {
        w.update(0.016f);
        if (s % every == 0) out.push_back(ofp::fingerprint(w));
    }
    return out;
}

void testReproducible() {
    Test t("reproducible");
    const char* cfgs[] = {nullptr,
                          R"({"predatorCount":5,"predatorEconomy":true,"predatorInheritance":true,"currentStrength":70,"foodDrift":true,"foodPatches":5,"foodDepthMode":1,"toroidal":true,"maxNeighbors":7,"bodyModel":true})"};
    for (const char* cfg : cfgs) {
        const auto a = runPrints(51, cfg, 300, 1000, 100);
        const auto b = runPrints(51, cfg, 300, 1000, 100);
        t.expect(a.size() == 11 && a.size() == b.size(), "fingerprint count %zu / %zu", a.size(), b.size());
        for (size_t i = 0; i < a.size() && i < b.size(); ++i)
            t.expect(a[i] == b[i], "cfg %s: same seed differs at print %zu", cfg ? "rich" : "default", i);
        const auto c = runPrints(52, cfg, 300, 0, 1);
        t.expect(!c.empty() && c[0].fish != a[0].fish && c[0].rng != a[0].rng, "another seed gave the same world");
    }
    // The hash sees a 1-ulp change in each section (and only that section),
    // including the private state the harness reads through ofp::peek.
    OceanWorld w;
    std::string err;
    ofp::initWorld(w, 53, 1920, 1080, 240.0f, 200,
                   R"({"predatorCount":2,"currentStrength":40,"foodPatches":3,"foodDepthMode":1})", err);
    for (int s = 0; s < 50; ++s) w.update(0.016f);
    const ofp::Fingerprint base = ofp::fingerprint(w);
    auto bump = [](float& f) { f = std::nextafter(f, 1e30f); };
    int bumps = 0;
    auto only = [&](const char* what, std::uint64_t ofp::Fingerprint::*field) {
        ++bumps;
        const ofp::Fingerprint p = ofp::fingerprint(w);
        bool othersSame = true;
        for (auto f : ofp::kSections)
            if (f != field) othersSame &= (p.*f == base.*f);
        t.expect(p.*field != base.*field && p.all != base.all && othersSame, "a 1-ulp change in %s: hash %s", what,
                 p.*field == base.*field ? "unchanged" : "changed other sections too");
    };
    using FP = ofp::Fingerprint;
    namespace pk = ofp::peek;
    { const float keep = w.creatures[7].depth; bump(w.creatures[7].depth); only("fish depth", &FP::fish); w.creatures[7].depth = keep; }
    { const float keep = w.creatures[3].bodyBuoy; bump(w.creatures[3].bodyBuoy); only("fish bodyBuoy", &FP::fish); w.creatures[3].bodyBuoy = keep; }
    { const float keep = w.creatures[5].body[4].y; bump(w.creatures[5].body[4].y); only("fish body trail", &FP::fish); w.creatures[5].body[4].y = keep; }
    { const float keep = w.food[11].depth; bump(w.food[11].depth); only("food depth", &FP::food); w.food[11].depth = keep; }
    { const float keep = w.predators[1].pitch; bump(w.predators[1].pitch); only("predator pitch", &FP::pred); w.predators[1].pitch = keep; }
    { const float keep = w.ocfg.vDrag; bump(w.ocfg.vDrag); only("ocean config", &FP::cfg); w.ocfg.vDrag = keep; }
    { const float keep = w.cfg.moveCost; bump(w.cfg.moveCost); only("SimConfig moveCost (0 -> denormal)", &FP::cfg); w.cfg.moveCost = keep; }
    { const int keep = w.cfg.clusterEpoch; w.cfg.clusterEpoch++; only("clusterEpoch (no checkpoint key)", &FP::cfg); w.cfg.clusterEpoch = keep; }
    { const auto keep = w.nextId_; w.nextId_++; only("nextId_", &FP::ctr); w.nextId_ = keep; }
    { auto& t1 = ofp::field<pk::OwWaves>(w)[1]; const double keep = t1.phase; t1.phase = std::nextafter(keep, 10.0);
      only("a wave phase", &FP::ctr); t1.phase = keep; }
    { std::vector<std::uint8_t> g = w.signals.data(); const std::vector<std::uint8_t> keep = g; g[100] ^= 1;
      w.signals.restore(g); only("a pheromone byte", &FP::sig); w.signals.restore(keep); }
    {   // a vortex frequency (vortexState() index 5 = vortex 0's fx), centres kept
        CurrentField& cf = ofp::field<pk::OwCurrent>(w);
        const std::vector<float> keepV = cf.vortexState();
        const std::vector<Vec2> keepC = ofp::field<pk::CfCenters>(cf);
        std::vector<float> v = keepV;
        bump(v[5]);
        cf.restoreVortexState(v);
        ofp::field<pk::CfCenters>(cf) = keepC;
        only("a vortex frequency", &FP::cur);
        cf.restoreVortexState(keepV);
        ofp::field<pk::CfCenters>(cf) = keepC;
    }
    { float& c = ofp::field<pk::CfCore>(ofp::field<pk::OwCurrent>(w)); const float keep = c; bump(c); only("the eddy core", &FP::cur); c = keep; }
    { float& d = ofp::field<pk::OwPatchDepth>(w)[1]; const float keep = d; bump(d); only("a food-patch depth", &FP::geo); d = keep; }
    { float& x = ofp::field<pk::OwPatches>(w)[2].x; const float keep = x; bump(x); only("a food-patch centre", &FP::geo); x = keep; }
    { float& ic = ofp::field<pk::SgInvCell>(ofp::field<pk::OwFoodGrid>(w)); const float keep = ic; bump(ic);
      only("the food grid's cell size", &FP::geo); ic = keep; }
    { bool& tor = ofp::field<pk::SigToroidal>(w.signals); tor = !tor; only("the signal grid's wrap flag", &FP::geo); tor = !tor; }
    { int& wc = ofp::field<pk::SigWrapCols>(w.signals); ++wc; only("the signal grid's seam period", &FP::geo); --wc; }
    { const RandomGen keep = rng; rng.uint(); only("rng (one draw)", &FP::rng); rng = keep; }
    t.expect(ofp::fingerprint(w) == base, "restoring every bump did not restore the hash");
    t.note("%d single-value changes, each seen by its own section only", bumps);
}

#ifndef OCEAN_TESTS_NO_SHIM
void testReset() {
    Test t("reset");
    const char* cfg = R"({"predatorCount":4,"predatorEconomy":true,"currentStrength":60,"foodPatches":4,"foodDepthMode":1,"kBuoy":2})";
    if (!t.expect(oc_init(77, 1920, 1080, 240.0, 300, cfg) == 1, "oc_init refused: %s", oc_last_error())) return;
    const ofp::Fingerprint f0 = ofp::fingerprint(*g_world);
    oc_step(400);
    const ofp::Fingerprint f400 = ofp::fingerprint(*g_world);
    t.expect(oc_set_param("kBuoy", 5.0) == 1 && oc_set_param("pitchMax", 0.2) == 1, "oc_set_param refused: %s", oc_last_error());
    oc_step(100);
    t.expect(oc_reset() == 1, "oc_reset failed: %s", oc_last_error());
    t.expect(ofp::fingerprint(*g_world) == f0, "oc_reset did not restore the initial world");
    oc_step(400);
    t.expect(ofp::fingerprint(*g_world) == f400, "after oc_reset, 400 steps differ from the first 400");
    // A second reset, and an oc_init with the same arguments, replay too.
    oc_reset();
    oc_step(400);
    t.expect(ofp::fingerprint(*g_world) == f400, "second oc_reset differs");
    oc_init(77, 1920, 1080, 240.0, 300, cfg);
    oc_step(400);
    t.expect(ofp::fingerprint(*g_world) == f400, "a repeated oc_init differs");

    // The harness's world (ofp::initWorld, what ocean_parity_driver runs) is the
    // shim's. fingerprint() hashes the GLOBAL rng, so the harness's prints (rng
    // included) are taken while the rng is the harness's — before the shim
    // touches it — and the shim's are compared against those snapshots: one
    // extra draw anywhere in oc_init / oc_step, even after its last step, fails.
    for (const char* c : {(const char*)nullptr, cfg,
                          R"({"pitchMax":3,"vDrag":0,"wallBand":80,"buoyMutation":0.4,"toroidal":true,"bodyModel":true,"triangularMotion":true})"}) {
        OceanWorld w;
        std::string err;
        if (!t.expect(ofp::initWorld(w, 78, 1600, 900, 300.0f, 250, c, err), "initWorld: %s", err.c_str())) continue;
        const ofp::Fingerprint h0 = ofp::fingerprint(w);
        const std::string r0 = rng.state();
        for (int s = 0; s < 300; ++s) w.update(0.016f);
        const ofp::Fingerprint h300 = ofp::fingerprint(w);
        const std::string r300 = rng.state();
        rng.seed(999);   // the shim must not depend on what the rng held before oc_init
        if (!t.expect(oc_init(78, 1600, 900, 300.0, 250, c) == 1, "oc_init: %s", oc_last_error())) continue;
        t.expect(ofp::fingerprint(*g_world) == h0 && rng.state() == r0, "shim vs harness differ after init (cfg %s)",
                 c ? c : "{}");
        oc_step(300);
        const ofp::Fingerprint s300 = ofp::fingerprint(*g_world);
        t.expect(s300 == h300 && rng.state() == r300, "shim vs harness differ after 300 steps (cfg %s; rng %s)",
                 c ? c : "{}", s300.rng == h300.rng ? "same" : "differs");
    }

    // Re-init. A fresh OceanWorld re-inited from the same seed replays. Reusing
    // ONE object keeps WorldCommon's run state — frame, nextId_ (so the ids),
    // the totals and the pairing window — as Classic's World::init does; the
    // shim never reuses a world (oc_init / oc_reset allocate a new one). Asserted
    // exactly: the reused world differs from a fresh one in that state and in
    // nothing else.
    {
        OceanWorld a;
        std::string err;
        ofp::initWorld(a, 79, 1920, 1080, 240.0f, 200, nullptr, err);
        const ofp::Fingerprint a0 = ofp::fingerprint(a);
        for (int s = 0; s < 200; ++s) a.update(0.016f);
        const unsigned long long frameBefore = a.frame;
        const std::uint64_t idBefore = a.nextId_;
        const long long birthsBefore = a.totalBirths, deathsBefore = a.totalDeaths;
        const std::size_t windowBefore = a.reproWindow_.size();
        ofp::initWorld(a, 79, 1920, 1080, 240.0f, 200, nullptr, err);
        const ofp::Fingerprint a1 = ofp::fingerprint(a);
        OceanWorld b;
        ofp::initWorld(b, 79, 1920, 1080, 240.0f, 200, nullptr, err);
        const ofp::Fingerprint b0 = ofp::fingerprint(b);
        t.expect(b0 == a0, "a fresh OceanWorld re-inited from the same seed does not replay");
        t.expect(a.frame == frameBefore && a.nextId_ == idBefore + 200 && a.totalBirths == birthsBefore &&
                     a.totalDeaths == deathsBefore && a.reproWindow_.size() == windowBefore && a.totalReseeds == 0,
                 "reused world: frame %llu (was %llu), nextId %llu (was %llu + 200 founders), births %lld (was %lld)",
                 a.frame, frameBefore, (unsigned long long)a.nextId_, (unsigned long long)idBefore, a.totalBirths, birthsBefore);
        t.expect(frameBefore == 200 && idBefore >= 201, "the first run did not get anywhere to carry over");
        t.expect(a1.cfg == a0.cfg && a1.food == a0.food && a1.pred == a0.pred && a1.sig == a0.sig && a1.cur == a0.cur &&
                     a1.geo == a0.geo && a1.rng == a0.rng && a1.ctr != a0.ctr && a1.fish != a0.fish,
                 "reused world: only ctr and fish may differ from a fresh one");
        // The fish are the fresh world's, but for ids shifted by the ids already used.
        bool sameFish = a.creatures.size() == b.creatures.size();
        for (std::size_t i = 0; sameFish && i < a.creatures.size(); ++i) {
            Fish x = a.creatures[i];
            const Fish& y = b.creatures[i];
            sameFish &= (x.id == y.id + (idBefore - 1) && x.lineageId == y.lineageId + (idBefore - 1));
            x.id = y.id;
            x.lineageId = y.lineageId;
            ofp::Hasher hx, hy;
            ofp::hashFish(hx, x);
            ofp::hashFish(hy, y);
            sameFish &= (hx.value() == hy.value());
        }
        t.expect(sameFish, "reused world: the fish differ from a fresh world's by more than their ids");
    }
}
#endif

struct BirthResult {
    bool born = false;
    long draws = -1;
    float childDepth = 0, mid = 0, childBuoy = 0, b1 = 0, b2 = 0, vz = 1;
    Vec2 childPos;
    std::uint64_t childLineage = 0, p1Lineage = 0;
    std::uint32_t gen = 0;
};

// Two adult, well-fed fish wired to vote INTERACT_REPRODUCE, one step.
BirthResult runBirth(unsigned seed, float m, float b1, float b2, float z1, float z2, Vec2 p1, Vec2 p2,
                     bool toroidal = false, float D = 240.0f) {
    OceanConfig oc;
    oc.buoyMutation = m;
    OceanWorld w;
    initQuiet(w, seed, D, oc, false, toroidal);
    const std::vector<Gene> g = {mkGene(false, 0, INTERACT_REPRODUCE, kStrong)};
    const int i1 = addFish(w, p1, z1, g, b1), i2 = addFish(w, p2, z2, g, b2);
    for (int i : {i1, i2}) {
        Fish& f = w.creatures[i];
        f.age = 10.0f;
        f.foodReserves = 100.0f;
        f.reproTimer = 0.0f;
        f.interactTimer = 0.0f;
    }
    BirthResult r;
    const RandomGen before = rng;
    w.update(0.016f);
    r.draws = drawsBetween(before, rng);
    if (w.creatures.size() != 3) return r;
    const Fish &a = w.creatures[i1], &b = w.creatures[i2], &c = w.creatures[2];
    r.born = true;
    r.childDepth = c.depth;
    r.mid = (a.depth + b.depth) * 0.5f;
    r.childBuoy = c.bodyBuoy;
    r.b1 = a.bodyBuoy;
    r.b2 = b.bodyBuoy;
    r.vz = c.vz;
    r.childPos = c.pos;
    r.childLineage = c.lineageId;
    r.p1Lineage = a.lineageId;
    r.gen = c.generation;
    return r;
}

void testChild() {
    Test t("child");
    float lo = 1, hi = -1;
    int born = 0;
    for (unsigned s = 0; s < 300; ++s) {
        const float m = 0.05f;
        const BirthResult r = runBirth(1000 + s, m, 0.3f, 0.6f, 100.0f + (float)(s % 7), 120.0f - (float)(s % 5),
                                       {960, 540}, {962, 541});
        if (!t.expect(r.born, "seed %u: no birth", 1000 + s)) continue;
        ++born;
        t.expect(r.childDepth == r.mid, "child depth %.9g != parents' midpoint %.9g", r.childDepth, r.mid);
        const float mean = 0.5f * (r.b1 + r.b2), d = r.childBuoy - mean;
        t.expect(std::abs(d) <= m + 1e-6f, "child bodyBuoy %.9g is %.9g from the parents' mean (m = %g)", r.childBuoy, d, m);
        t.expect(r.vz == 0.0f && r.gen == 1 && r.childLineage == r.p1Lineage, "child vz %.9g gen %u lineage %llu/%llu",
                 r.vz, r.gen, (unsigned long long)r.childLineage, (unsigned long long)r.p1Lineage);
        lo = std::min(lo, d);
        hi = std::max(hi, d);
    }
    t.expect(lo < -0.04f && hi > 0.04f, "bodyBuoy draws do not span ±m: [%.4f, %.4f]", lo, hi);
    t.note("%d births; child bodyBuoy - mean in [%.4f, %.4f] (m = 0.05)", born, lo, hi);

    // m = 0: the mean exactly. Clamped at the ends.
    const BirthResult z = runBirth(7, 0.0f, 0.3f, 0.6f, 100, 110, {960, 540}, {961, 540});
    t.expect(z.born && z.childBuoy == 0.5f * (z.b1 + z.b2), "m = 0: child bodyBuoy %.9g != mean %.9g", z.childBuoy,
             0.5f * (z.b1 + z.b2));
    for (unsigned s = 0; s < 40; ++s) {
        const BirthResult top = runBirth(50 + s, 0.3f, 1.0f, 1.0f, 230, 235, {960, 540}, {961, 540});
        const BirthResult bot = runBirth(90 + s, 0.3f, 0.0f, 0.0f, 3, 8, {960, 540}, {961, 540});
        t.expect(top.born && top.childBuoy <= 1.0f && top.childBuoy >= 0.7f - 1e-6f, "clamp at 1: %.9g", top.childBuoy);
        t.expect(bot.born && bot.childBuoy >= 0.0f && bot.childBuoy <= 0.3f + 1e-6f, "clamp at 0: %.9g", bot.childBuoy);
        t.expect(top.childDepth == top.mid && bot.childDepth == bot.mid, "midpoint near surface / floor");
    }
    // Across the toroidal seam: placed at the wrapped midpoint, depth still the mean.
    const BirthResult seam = runBirth(8, 0.05f, 0.4f, 0.5f, 60, 80, {3, 540}, {1917, 540}, /*toroidal*/ true);
    t.expect(seam.born && seam.childDepth == seam.mid && (seam.childPos.x < 10.0f || seam.childPos.x > 1910.0f),
             "toroidal seam birth: born %d depth %.9g mid %.9g x %.4f", seam.born, seam.childDepth, seam.mid, seam.childPos.x);
}

void testDrawsMotion() {
    Test t("draws_motion");
    struct V { const char* name; OceanConfig oc; float D; bool tri; };
    std::vector<V> vs;
    auto add = [&](const char* n, auto&& set, float D = 240.0f, bool tri = false) {
        OceanConfig o;
        set(o);
        vs.push_back({n, o, D, tri});
    };
    add("default", [](OceanConfig&) {});
    add("pitchMax 0.01", [](OceanConfig& o) { o.pitchMax = 0.01f; });
    add("pitchMax 5", [](OceanConfig& o) { o.pitchMax = 5.0f; });
    add("kBuoy -5", [](OceanConfig& o) { o.kBuoy = -5.0f; });
    add("kBuoy 30", [](OceanConfig& o) { o.kBuoy = 30.0f; });
    add("wallBand 0", [](OceanConfig& o) { o.wallBand = 0.0f; });
    add("wallBand 120", [](OceanConfig& o) { o.wallBand = 120.0f; });
    add("vDrag 0", [](OceanConfig& o) { o.vDrag = 0.0f; });
    add("vDrag 300", [](OceanConfig& o) { o.vDrag = 300.0f; });
    add("costs", [](OceanConfig& o) { o.cHold = 0.5f; o.cPress = 0.5f; });
    add("D 10", [](OceanConfig&) {}, 10.0f);
    add("D 3000", [](OceanConfig&) {}, 3000.0f);
    add("triangular (no MOVE_RANDOM)", [](OceanConfig&) {}, 240.0f, true);
    for (const V& v : vs) {
        OceanWorld w;
        initQuiet(w, 61, v.D, v.oc);
        w.cfg.triangularMotion = v.tri;
        int nRandom = 0;
        for (int i = 0; i < 40; ++i) {
            // Depths at the surface, the floor and between; bodyBuoy 0, 1 and between;
            // STEER_Z wires of both signs and none; half the fish wired to MOVE_RANDOM.
            const float z = (i % 5 == 0) ? 0.0f : (i % 5 == 1) ? v.D : v.D * (float)(i % 7) / 7.0f;
            std::vector<Gene> g;
            if (i % 3 != 2) g.push_back(mkGene(false, 0, STEER_Z, (i % 3 == 0) ? kStrong : -kStrong));
            const bool rnd = (i % 2 == 0);
            if (rnd) g.push_back(mkGene(false, 0, MOVE_RANDOM, kStrong));
            const int k = addFish(w, {40.0f + 45.0f * i, 100.0f + 20.0f * (i % 40)}, z, g, (float)(i % 3) * 0.5f);
            w.creatures[k].responsiveness = 1.0f;   // |MOVE_RANDOM| = 0.964 > 0.5: its draws fire
            nRandom += rnd;
        }
        const long N = (long)w.creatures.size();
        const long expected = N + (v.tri ? 0 : 3L * nRandom);   // RANDOM_SENSOR each + MOVE_RANDOM's three
        for (int s = 1; s <= 150; ++s) {
            const RandomGen before = rng;
            w.update(0.016f);
            const long d = drawsBetween(before, rng, 10000);
            t.expect(d == expected, "%s step %d: %ld draws, expected %ld", v.name, s, d, expected);
            t.expect((long)w.creatures.size() == N, "%s: population changed", v.name);
        }
    }
    t.note("%zu variants x 150 steps: draws/step == N + 3 x (fish firing MOVE_RANDOM), always", vs.size());
}

void testDrawsBirth() {
    Test t("draws_birth");
    struct V { float m, b1, b2, z1, z2; };
    const V vs[] = {
        {0.05f, 0.3f, 0.6f, 100, 110}, {0.0f, 0.3f, 0.6f, 100, 110}, {0.5f, 0.3f, 0.6f, 100, 110},
        {0.05f, 0.0f, 0.0f, 0, 10},    {0.05f, 1.0f, 1.0f, 240, 230}, {1e-6f, 0.9f, 0.1f, 50, 70},
    };
    for (unsigned seed : {201u, 202u, 203u, 204u}) {
        long ref = -2;
        for (const V& v : vs) {
            const BirthResult r = runBirth(seed, v.m, v.b1, v.b2, v.z1, v.z2, {960, 540}, {961, 541});
            t.expect(r.born && r.draws > 0, "seed %u: born %d draws %ld", seed, r.born, r.draws);
            if (ref == -2) ref = r.draws;
            t.expect(r.draws == ref, "seed %u m %g buoy %g/%g depth %g/%g: %ld draws, the first variant took %ld",
                     seed, v.m, v.b1, v.b2, v.z1, v.z2, r.draws, ref);
        }
        t.note("seed %u: %ld draws in the birth step for every variant", seed, ref);
    }
}

void testDrawsPredators() {
    Test t("draws_pred");
    // Wander only (no fish): two draws per predator per step, whatever its depth / pitch / D.
    for (float D : {10.0f, 240.0f, 3000.0f}) {
        for (bool econ : {false, true}) {
            OceanWorld w;
            initQuiet(w, 71, D, OceanConfig{});
            w.cfg.predatorEconomy = econ;
            w.cfg.predatorMetabolism = 0.0f;   // nobody starves, nobody buds
            w.cfg.predatorCount = 9;
            for (int i = 0; i < 9; ++i) {
                OceanPredator p;
                p.pos = {100.0f + 200.0f * i, 540.0f};
                p.depth = (i % 3 == 0) ? 0.0f : (i % 3 == 1) ? D : D * 0.37f;
                p.pitch = (i % 3 - 1) * 0.3f;
                p.energy = 30.0f;
                w.predators.push_back(p);
            }
            for (int s = 1; s <= 200; ++s) {
                const RandomGen before = rng;
                w.update(0.016f);
                const long d = drawsBetween(before, rng, 1000);
                t.expect(d == 18, "D %g econ %d step %d: %ld draws, expected 18", D, econ, s, d);
            }
        }
    }
    // Buds: four draws each (position x2, heading, depth), six with inheritance.
    for (bool tor : {false, true}) {
        for (bool inherit : {false, true}) {
            OceanWorld w;
            initQuiet(w, 72, 240.0f, OceanConfig{}, false, tor);
            w.cfg.predatorEconomy = true;
            w.cfg.predatorInheritance = inherit;
            for (int i = 0; i < 6; ++i) {
                OceanPredator p;
                p.pos = {(i == 0) ? 2.0f : 150.0f + 300.0f * i, (i == 1) ? 1078.0f : 540.0f};
                p.depth = (i == 2) ? 0.0f : (i == 3) ? 240.0f : 100.0f;   // a bud at the surface / floor clamps
                p.energy = (i < 4) ? 500.0f : 30.0f;                      // four bud, two do not
                w.predators.push_back(p);
            }
            const RandomGen before = rng;
            w.update(0.016f);
            const long d = drawsBetween(before, rng, 1000);
            const long expected = 6 * 2 + 4 * (inherit ? 6 : 4);
            t.expect(d == expected && w.predators.size() == 10, "buds (toroidal %d, inherit %d): %ld draws, expected %ld; %zu predators",
                     tor, inherit, d, expected, w.predators.size());
            for (const OceanPredator& p : w.predators)
                t.expect(p.depth >= 0.0f && p.depth <= 240.0f, "bud depth %.9g", p.depth);
        }
    }
    // Floor refill: everyone starves this step; four draws per refilled predator.
    {
        OceanWorld w;
        initQuiet(w, 73, 240.0f, OceanConfig{});
        w.cfg.predatorEconomy = true;
        w.cfg.predatorMinPopulation = 5;
        for (int i = 0; i < 3; ++i) {
            OceanPredator p;
            p.pos = {300.0f + 500.0f * i, 300.0f};
            p.depth = 120.0f;
            p.energy = -1.0f;
            w.predators.push_back(p);
        }
        const RandomGen before = rng;
        w.update(0.016f);
        const long d = drawsBetween(before, rng, 1000);
        t.expect(d == 3 * 2 + 5 * 4 && w.predators.size() == 5 && w.totalPredatorReseeds == 5,
                 "floor refill: %ld draws (expected 26), %zu predators, %lld reseeds", d, w.predators.size(),
                 w.totalPredatorReseeds);
    }
}

void testDrawsFood() {
    Test t("draws_food");
    struct V { int patches, mode; float spread; };
    const V vs[] = {{0, 0, 30}, {0, 1, 30}, {5, 0, 30}, {5, 1, 0}, {5, 1, 30}, {5, 1, 10000}};
    RandomGen common = rng;
    bool haveCommon = false;
    std::string endState[2];
    for (const V& v : vs) {
        OceanConfig oc;
        oc.foodDepthMode = v.mode;
        oc.foodDepthSpread = v.spread;
        OceanWorld w;
        initQuiet(w, 81, 240.0f, oc);
        w.cfg.foodPatches = v.patches;
        w.init(1920, 1080, 240.0f, 0);   // re-init so the patches (and their depths) are drawn
        if (!haveCommon) { common = rng; haveCommon = true; }
        rng = common;                    // every variant spawns from the same engine state
        const long perSpawn = v.patches > 0 ? 4 : 3;
        std::set<std::uint32_t> depths;
        for (int i = 0; i < 500; ++i) {
            const RandomGen before = rng;
            w.spawnFood();
            const long d = drawsBetween(before, rng, 100);
            t.expect(d == perSpawn, "patches %d mode %d spread %g: spawn %d took %ld draws, expected %ld",
                     v.patches, v.mode, v.spread, i, d, perSpawn);
            const float z = w.food.back().depth;
            t.expect(z >= 0.0f && z <= 240.0f, "food depth %.9g", z);
            depths.insert(ofp::bits(z));
        }
        if (v.patches > 0 && v.mode == 1 && v.spread == 0.0f)
            t.expect(depths.size() <= 5, "spread 0: %zu distinct food depths from 5 patches", depths.size());
        // Same draw count per spawn => the same engine state after 500 spawns,
        // with or without patches (within each group).
        std::string& ref = endState[v.patches > 0];
        if (ref.empty()) ref = rng.state();
        t.expect(rng.state() == ref, "patches %d mode %d spread %g: engine state after 500 spawns differs", v.patches,
                 v.mode, v.spread);
    }
}

// ============================================================================
// E1 physics
// ============================================================================

// A tiny local generator for probe points (the engine's rng stays untouched).
struct Lcg {
    std::uint32_t s;
    explicit Lcg(std::uint32_t x) : s(x) {}
    float unit() { s = s * 1664525u + 1013904223u; return (float)(s >> 8) * (1.0f / 16777216.0f); }
};

// Central-difference divergence of one flow field at (p, d), step h.
template <class F>
void divergenceAt(F&& f, Vec2 p, float d, float h, double& div, double& sumAbs) {
    const OceanWorld::Flow xp = f(Vec2{p.x + h, p.y}, d), xm = f(Vec2{p.x - h, p.y}, d);
    const OceanWorld::Flow yp = f(Vec2{p.x, p.y + h}, d), ym = f(Vec2{p.x, p.y - h}, d);
    const OceanWorld::Flow dp = f(p, d + h), dm = f(p, d - h);
    const double ax = ((double)xp.x - (double)xm.x) / (2.0 * h);
    const double ay = ((double)yp.y - (double)ym.y) / (2.0 * h);
    const double ad = ((double)dp.d - (double)dm.d) / (2.0 * h);
    div = ax + ay + ad;
    sumAbs = std::abs(ax) + std::abs(ay) + std::abs(ad);
}

void testDivergence() {
    Test t("divergence");
    OceanConfig oc;
    oc.waveSpeed = 6.0f;
    oc.deepCurrent = true;
    OceanWorld w;
    initQuiet(w, 301, 240.0f, oc, false, false, /*current*/ 40.0f);
    for (int s = 0; s < 37; ++s) w.update(0.016f);   // the clocks run: phases != 0
    const float D = w.D, b = std::min(oc.waveBand, 0.25f * D);
    using Fl = OceanWorld::Flow;
    struct Field { const char* name; std::function<Fl(Vec2, float)> f; };
    const std::vector<Field> fields = {
        {"train0", [&](Vec2 p, float d) { return w.waveTrainAt(0, p, d); }},
        {"train1", [&](Vec2 p, float d) { return w.waveTrainAt(1, p, d); }},
        {"train2", [&](Vec2 p, float d) { return w.waveTrainAt(2, p, d); }},
        {"waves", [&](Vec2 p, float d) {
             Fl u;
             for (int i = 0; i < OceanWorld::kWaveTrains; ++i) {
                 const Fl v = w.waveTrainAt(i, p, d);
                 u.x += v.x; u.y += v.y; u.d += v.d;
             }
             return u; }},
        {"deepcur", [&](Vec2 p, float d) { return w.currentFlowAt(p, d); }},
        {"flow", [&](Vec2 p, float d) { return w.flowAt(p, d); }},
    };
    // Probe sets: the bulk (clear of the taper's band edges, where S is only
    // C¹ and the difference converges as O(h)) and the two boundary bands.
    struct Set { const char* name; float lo, hi; };
    const Set sets[] = {{"bulk", b + 4.0f, D - b - 4.0f}, {"top", 2.5f, b + 4.0f}, {"floor", D - b - 4.0f, D - 2.5f}};
    const float hs[] = {2.0f, 1.0f, 0.5f, 0.25f, 0.125f, 0.0625f, 0.03125f, 0.015625f};
    constexpr int NH = 8;
    for (const Set& st : sets) {
        Lcg g(1234);
        std::vector<std::pair<Vec2, float>> probes;
        for (int k = 0; k < 300; ++k)
            probes.push_back({Vec2{20.0f + 1880.0f * g.unit(), 20.0f + 1040.0f * g.unit()}, st.lo + (st.hi - st.lo) * g.unit()});
        for (const Field& fd : fields) {
            double res[NH];
            for (int hi = 0; hi < NH; ++hi) {
                double maxDiv = 0.0, maxSum = 0.0;
                for (const auto& pr : probes) {
                    double dv, sa;
                    divergenceAt(fd.f, pr.first, pr.second, hs[hi], dv, sa);
                    maxDiv = std::max(maxDiv, std::abs(dv));
                    maxSum = std::max(maxSum, sa);
                }
                res[hi] = maxSum > 0.0 ? maxDiv / maxSum : 0.0;
            }
            // The best residual over h must be < 1e-3 and come from shrinking h
            // (truncation error falling before float noise takes over).
            int best = 0;
            for (int hi = 1; hi < NH; ++hi) if (res[hi] < res[best]) best = hi;
            const bool bulk = (st.name[0] == 'b');
            t.expect(res[best] < 1e-3, "%s %s: best normalised residual %.3g (h %g)", st.name, fd.name, res[best], hs[best]);
            if (bulk) t.expect(res[1] < 1e-3, "%s %s: residual %.3g at h = 1", st.name, fd.name, res[1]);
            t.expect(res[best] <= res[0] || res[0] < 1e-6, "%s %s: no convergence (h 2: %.3g, best %.3g)", st.name, fd.name, res[0], res[best]);
            if (fd.name[0] == 'f' || fd.name[0] == 'w')
                t.note("%-5s %-7s residual h=2 %.1e  h=1 %.1e  h=1/4 %.1e  h=1/16 %.1e  h=1/32 %.1e", st.name, fd.name,
                       res[0], res[1], res[3], res[5], res[6]);
        }
    }
    // No flow through the surface or the floor: exactly zero normal component.
    Lcg g(99);
    int zeroN = 0, stillSurface = 0;
    for (int k = 0; k < 500; ++k) {
        const Vec2 p = {1920.0f * g.unit(), 1080.0f * g.unit()};
        const Fl s = w.flowAt(p, 0.0f), f = w.flowAt(p, D);
        bool ok = s.d == 0.0f && f.d == 0.0f;
        for (int i = 0; i < OceanWorld::kWaveTrains; ++i)
            ok &= w.waveTrainAt(i, p, 0.0f).d == 0.0f && w.waveTrainAt(i, p, D).d == 0.0f;
        zeroN += ok;
        // the deep current is still at the surface and full from mid-depth
        const Fl c0 = w.currentFlowAt(p, 0.0f), cm = w.currentFlowAt(p, 0.5f * D + 3.0f);
        const Vec2 c = w.currentAt(p);
        stillSurface += (c0.x == 0.0f && c0.y == 0.0f && cm.x == c.x && cm.y == c.y && cm.d == 0.0f);
    }
    t.expect(zeroN == 500, "normal flow at surface / floor not exactly 0 at %d of 500 points", 500 - zeroN);
    t.expect(stillSurface == 500, "deep current: f(0) = 0, f(>= D/2) = 1 failed at %d points", 500 - stillSurface);
    // flowAt (one shared taper per call) == the current + each train on its
    // own (waveTrainAt(i, p, d)), in flowAt's order, bit for bit: in the bulk,
    // across both taper bands and their edges, at surface and floor.
    int sameBits = 0, nPts = 0;
    for (int k = 0; k < 2000; ++k) {
        const Vec2 p = {1920.0f * g.unit(), 1080.0f * g.unit()};
        const float edges[] = {0.0f, b, D - b, D, std::nextafter(b, 0.0f), std::nextafter(D - b, D)};
        const float d = (k < 600) ? edges[k % 6] : D * g.unit();
        const Fl f = w.flowAt(p, d);
        Fl s = w.currentFlowAt(p, d);
        for (int i = 0; i < OceanWorld::kWaveTrains; ++i) {
            const Fl u = w.waveTrainAt(i, p, d);
            s.x += u.x; s.y += u.y; s.d += u.d;
        }
        ++nPts;
        sameBits += (std::memcmp(&f.x, &s.x, 4) == 0 && std::memcmp(&f.y, &s.y, 4) == 0 && std::memcmp(&f.d, &s.d, 4) == 0);
    }
    t.expect(sameBits == nPts, "flowAt != current + the trains one by one at %d of %d points", nPts - sameBits, nPts);
}

void testE1Buoyancy() {
    Test t("e1_buoyancy");
    OceanConfig oc;   // kBuoy 1, vDrag 2, pitchMax 0.5; no walls; flow off (no waves, no current)
    oc.wallBand = 0.0f;
    oc.lightEnabled = true;
    oc.depthCostScaled = true;
    oc.cHold = 0.05f;
    oc.cPress = 0.03f;
    oc.sizeDrift = true;
    oc.sizeFight = true;
    oc.mouthGate = true;
    OceanWorld w;
    initQuiet(w, 311, 240.0f, oc);
    struct F { int idx; float target; };
    const F fs[] = {
        {addFish(w, {300, 300}, 24.0f, {}, 0.5f), 120.0f},
        {addFish(w, {900, 700}, 216.0f, {}, 0.5f), 120.0f},
        {addFish(w, {1500, 500}, 192.0f, {}, 0.2f), 48.0f},
        {addFish(w, {600, 200}, 100.0f, {}, 0.0f), 0.0f},     // neutral AT the surface
        {addFish(w, {1200, 900}, 100.0f, {}, 1.0f), 240.0f},  // neutral ON the floor
    };
    constexpr int NF = 5;
    float off[NF];
    for (int k = 0; k < NF; ++k) off[k] = w.creatures[fs[k].idx].depth - fs[k].target;
    for (int s = 1; s <= 3000; ++s) {
        w.update(0.016f);
        if (!t.expect(w.creatures.size() == (size_t)NF, "step %d: %zu fish", s, w.creatures.size())) return;
        for (int k = 0; k < NF; ++k) {
            const float o = w.creatures[fs[k].idx].depth - fs[k].target;
            t.expect(std::abs(o) <= std::abs(off[k]), "fish %d step %d: |offset| grew %.9g -> %.9g", k, s, off[k], o);
            t.expect(o == 0.0f || (o > 0.0f) == (off[k] > 0.0f) || off[k] == 0.0f, "fish %d step %d: overshot", k, s);
            off[k] = o;
        }
    }
    for (int k = 0; k < NF; ++k) t.expect(std::abs(off[k]) < 0.5f, "fish %d still %.4f from bodyBuoy·D", k, off[k]);
    t.note("offsets after 3000 steps: %.4f %.4f %.4f %.4f %.4f", off[0], off[1], off[2], off[3], off[4]);
}

void testDrift() {
    Test t("drift");
    const float sizes[3] = {0.5f, 1.0f, 2.0f};
    for (bool sized : {true, false}) {
        OceanConfig oc;
        oc.waveSpeed = 6.0f;
        oc.deepCurrent = true;
        oc.sizeDrift = sized;
        oc.kBuoy = 0.0f;
        oc.wallBand = 0.0f;
        OceanWorld w;
        initQuiet(w, 401, 240.0f, oc, false, false, /*current*/ 60.0f);
        w.cfg.minSpeed = 0.0f;   // no swimming: the flow is all that moves them
        for (int s = 0; s < 20; ++s) w.update(0.016f);
        struct P { Vec2 pos; float d; int idx[3]; };
        std::vector<P> ps;
        Lcg g(7);
        for (int k = 0; k < 24; ++k) {
            P p;
            p.pos = {100.0f + 1700.0f * g.unit(), 100.0f + 880.0f * g.unit()};
            p.d = 40.0f + 180.0f * g.unit();
            for (int j = 0; j < 3; ++j) {
                p.idx[j] = addFish(w, p.pos, p.d, {}, p.d / 240.0f);
                w.creatures[p.idx[j]].bodySize = sizes[j];
                w.creatures[p.idx[j]].speed = 0.0f;
            }
            ps.push_back(p);
        }
        w.update(0.016f);
        double sumRatio = 0.0;
        int nRatio = 0;
        for (const P& p : ps) {
            const OceanWorld::Flow U = w.flowAt(p.pos, p.d);   // the flow this step sampled
            for (int j = 0; j < 3; ++j) {
                const Fish& f = w.creatures[p.idx[j]];
                const float wgt = sized ? std::min(1.0f, 1.0f / sizes[j]) : 1.0f;
                const float ex = OceanWorld::wrapCoord(p.pos.x + wgt * U.x * 0.016f, 1920.0f);
                const float ey = OceanWorld::wrapCoord(p.pos.y + wgt * U.y * 0.016f, 1080.0f);
                const float ed = std::clamp(p.d + wgt * U.d * 0.016f, 0.0f, 240.0f);
                t.expect(f.pos.x == ex && f.pos.y == ey && f.depth == ed,
                         "sized %d size %g: moved to (%.9g, %.9g, %.9g), expected (%.9g, %.9g, %.9g)", sized, sizes[j],
                         f.pos.x, f.pos.y, f.depth, ex, ey, ed);
            }
            const Fish &a = w.creatures[p.idx[0]], &m = w.creatures[p.idx[1]], &z = w.creatures[p.idx[2]];
            t.expect(a.pos.x == m.pos.x && a.pos.y == m.pos.y && a.depth == m.depth, "size .5 and 1 drifted differently");
            const double dm = std::hypot((double)m.pos.x - p.pos.x, (double)m.pos.y - p.pos.y);
            const double dz = std::hypot((double)z.pos.x - p.pos.x, (double)z.pos.y - p.pos.y);
            if (dm > 1e-3) { sumRatio += dz / dm; ++nRatio; }
        }
        const double r1 = nRatio ? sumRatio / nRatio : 0.0;
        if (sized) t.expect(std::abs(r1 - 0.5) < 0.01, "one step: size-2 / size-1 drift %.4f, expected 0.5", r1);
        else       t.expect(std::abs(r1 - 1.0) < 1e-6, "sizeDrift off: size-2 / size-1 drift %.4f, expected 1", r1);
        // 200 more steps: how far each has been carried.
        for (int s = 0; s < 200; ++s) w.update(0.016f);
        double far[3] = {0, 0, 0};
        for (const P& p : ps)
            for (int j = 0; j < 3; ++j) {
                const Fish& f = w.creatures[p.idx[j]];
                far[j] += std::hypot((double)w.wrapDx(f.pos.x - p.pos.x), (double)w.wrapDy(f.pos.y - p.pos.y));
            }
        const double r200 = far[2] / far[1];
        if (sized) t.expect(far[0] == far[1] && r200 > 0.35 && r200 < 0.65, "200 steps: drift .5 / 1 / 2 = %.1f / %.1f / %.1f", far[0], far[1], far[2]);
        else       t.expect(far[0] == far[1] && far[1] == far[2], "sizeDrift off, 200 steps: %.1f / %.1f / %.1f", far[0], far[1], far[2]);
        t.note("sizeDrift %s: one step ratio (2 : 1) %.4f; 200 steps mean drift .5 / 1 / 2 = %.1f / %.1f / %.1f",
               sized ? "on " : "off", r1, far[0] / ps.size(), far[1] / ps.size(), far[2] / ps.size());
    }
}

void testCosts() {
    Test t("costs");
    const float dt = 0.016f, D = 240.0f, bb = 0.4f;
    const float depths[3] = {96.0f, 0.0f, 240.0f};   // neutral (0.4·D), surface, floor
    const char* where[3] = {"neutral", "surface", "deep"};
    float costL1[3] = {0, 0, 0};
    for (bool scaled : {true, false}) {
        for (float L : {1.0f, 2.0f}) {
            Fish after[2][3];
            for (int on = 0; on < 2; ++on) {
                OceanConfig oc;
                oc.kBuoy = 0.0f;
                oc.wallBand = 0.0f;
                oc.depthCostScaled = scaled;
                oc.cHold = on ? 0.05f : 0.0f;
                oc.cPress = on ? 0.03f : 0.0f;
                OceanWorld w;
                initQuiet(w, 501, D, oc);
                w.cfg.metabolicRate = 1.5f;
                w.cfg.geneCost = 0.01f;
                w.cfg.moveCost = 7e-5f;
                const std::vector<Gene> g = {mkGene(false, 0, INTERACT_IGNORE, 1000), mkGene(false, 1, INTERACT_IGNORE, 900),
                                             mkGene(false, 2, INTERACT_IGNORE, 800)};
                for (int k = 0; k < 3; ++k) {
                    const int i = addFish(w, {300.0f + 600.0f * k, 500.0f}, depths[k], g, bb);
                    w.creatures[i].bodySize = L;
                    w.creatures[i].speed = 40.0f;
                    w.creatures[i].foodReserves = 20.0f;
                }
                w.update(dt);
                for (int k = 0; k < 3; ++k) after[on][k] = w.creatures[k];
            }
            for (int k = 0; k < 3; ++k) {
                const Fish &base = after[0][k], &c = after[1][k];
                t.expect(c.depth == depths[k] && base.depth == depths[k], "fixture %s moved in depth", where[k]);
                const float H = std::abs(c.depth - c.bodyBuoy * D) / D, P = c.depth / D;
                float expect;
                if (scaled) {
                    const float M = 1.5f * L;   // metabolicRate · massFactor (= bodySize)
                    expect = base.foodReserves - dt * M * (0.05f * H + 0.03f * P);
                } else {
                    expect = base.foodReserves - dt * (0.05f * std::abs(c.depth - c.bodyBuoy * D) / D + 0.03f * c.depth / D);
                }
                t.expect(c.foodReserves == expect, "%s %s L %g: reserves %.9g, expected %.9g", scaled ? "scaled" : "preview",
                         where[k], L, c.foodReserves, expect);
                t.expect(c.age == base.age && c.reproTimer == base.reproTimer && c.interactTimer == base.interactTimer,
                         "%s: timers changed by the depth cost", where[k]);
                if (scaled) {
                    const float cost = dt * (1.5f * L) * (0.05f * H + 0.03f * P);
                    if (L == 1.0f) costL1[k] = cost;
                    else t.expect(cost == 2.0f * costL1[k], "%s: cost at L 2 %.9g != 2 x L 1 %.9g", where[k], cost, costL1[k]);
                }
            }
            // where H or P is 0, that term is absent: neutral pays pressure only, the surface holding only
            t.expect(std::abs(after[1][0].depth - bb * D) == 0.0f && after[1][1].depth == 0.0f, "fixture depths");
        }
    }
    t.note("scaled, L 1: neutral %.3g, surface %.3g, deep %.3g energy/step (gene + move costs apart, x2 at L 2)",
           costL1[0], costL1[1], costL1[2]);
}

void testFight() {
    Test t("fight");
    auto curve = [](float age) { return (age < 30.0f) ? age / 30.0f : 1.0f - (age - 30.0f) / 90.0f; };
    Lcg g(31);
    rng.seed(601);
    // (1, 1) is the Classic two-argument fight, bit for bit, rng included.
    int same = 0;
    for (int k = 0; k < 300; ++k) {
        Fish a1, b1;
        a1.age = 150.0f * g.unit(); b1.age = 150.0f * g.unit();
        a1.foodReserves = 40.0f * g.unit(); b1.foodReserves = 40.0f * g.unit();
        Fish a2 = a1, b2 = b1;
        const RandomGen r0 = rng;
        kernel::fightOutcome(a1, b1);
        const RandomGen r1 = rng;
        rng = r0;
        kernel::fightOutcome(a2, b2, 1.0f, 1.0f);
        same += (ofp::bits(a1.foodReserves) == ofp::bits(a2.foodReserves) &&
                 ofp::bits(b1.foodReserves) == ofp::bits(b2.foodReserves) && sameEngine(r1, rng) &&
                 drawsBetween(r0, rng, 10) == 2);
    }
    t.expect(same == 300, "unit scales differ from the two-argument fight in %d of 300", 300 - same);
    // Known draws: the winner follows curve·L²·u, two draws, ±15.
    const float Ls[] = {0.5f, 0.8f, 1.0f, 1.3f, 2.0f};
    int known = 0, bigWins = 0, bigFights = 0;
    for (int k = 0; k < 500; ++k) {
        Fish a, b;
        a.age = 10.0f + 90.0f * g.unit(); b.age = 10.0f + 90.0f * g.unit();
        a.foodReserves = 30.0f; b.foodReserves = 30.0f;
        const float La = Ls[k % 5], Lb = Ls[(k / 5) % 5];
        const float sa = La * La, sb = Lb * Lb;
        RandomGen r = rng;
        const float u1 = r.unit(), u2 = r.unit();
        float s1 = curve(a.age), s2 = curve(b.age);
        if (sa != 1.0f) s1 *= sa;
        if (sb != 1.0f) s2 *= sb;
        s1 *= u1; s2 *= u2;
        const bool aWins = s1 > s2;
        kernel::fightOutcome(a, b, sa, sb);
        known += (sameEngine(r, rng) && a.foodReserves == (aWins ? 45.0f : 15.0f) && b.foodReserves == (aWins ? 15.0f : 45.0f));
        if (La == 2.0f && Lb == 0.5f) { ++bigFights; bigWins += aWins; }
    }
    t.expect(known == 500, "known-draw fixtures: %d of 500 as predicted", known);
    // Size 2 vs .5 (strength x16 before the draws): P(win) = 1 − 1/(2r) for
    // r = 16·curve ratio, ~0.95 over ages 10-100.
    for (int k = 0; k < 400; ++k) {
        Fish a, b;
        a.age = 10.0f + 90.0f * g.unit(); b.age = 10.0f + 90.0f * g.unit();
        a.foodReserves = b.foodReserves = 30.0f;
        kernel::fightOutcome(a, b, 4.0f, 0.25f);
        ++bigFights;
        bigWins += (a.foodReserves > 30.0f);
    }
    t.expect(bigWins * 100 >= bigFights * 88, "size 2 vs .5 won only %d of %d", bigWins, bigFights);
    // Ties (both strengths 0: ages 0 and 120) go to c2, scaled or not.
    for (int k = 0; k < 4; ++k) {
        Fish a, b;
        a.age = (k & 1) ? 120.0f : 0.0f; b.age = (k & 2) ? 120.0f : 0.0f;
        a.foodReserves = b.foodReserves = 30.0f;
        Fish a2 = a, b2 = b;
        kernel::fightOutcome(a, b);
        kernel::fightOutcome(a2, b2, 4.0f, 0.25f);
        t.expect(a.foodReserves == 15.0f && b.foodReserves == 45.0f && a2.foodReserves == 15.0f && b2.foodReserves == 45.0f,
                 "tie (ages %g, %g): c2 must win", a.age, b.age);
    }
    t.note("size 2 vs size .5: the big fish won %d of %d fights", bigWins, bigFights);
}

void testMouth() {
    Test t("mouth");
    struct V { float L, kMouth, below, equal, above; };
    const V vs[] = {{1.0f, 2.0f, 1.9f, 2.0f, 2.1f}, {0.5f, 3.0f, 1.4f, 1.5f, 1.6f}, {2.0f, 0.75f, 1.4f, 1.5f, 1.6f}};
    for (const V& v : vs) {
        for (bool gate : {true, false}) {
            OceanConfig oc;
            oc.mouthGate = gate;
            oc.kMouth = v.kMouth;
            oc.kBuoy = 0.0f;
            oc.wallBand = 0.0f;
            OceanWorld w;
            initQuiet(w, 701, 240.0f, oc, /*capture*/ true);
            w.cfg.minSpeed = 0.0f;
            const int i = addFish(w, {500, 500}, 100.0f, {}, 100.0f / 240.0f);
            w.creatures[i].bodySize = v.L;
            w.creatures[i].speed = 0.0f;
            w.creatures[i].foodReserves = 50.0f;
            const float sz[3] = {v.below, v.equal, v.above};
            const Vec2 at[3] = {{505, 500}, {500, 505}, {500, 500}};
            const float az[3] = {100.0f, 100.0f, 105.0f};
            for (int k = 0; k < 3; ++k) {
                OceanFood f;
                f.pos = at[k]; f.depth = az[k]; f.size = sz[k];
                w.food.push_back(f);
            }
            w.update(0.016f);
            const Fish& c = w.creatures[i];
            if (gate) {
                t.expect(w.food.size() == 1 && w.food[0].size == v.above,
                         "L %g kMouth %g: %zu food left (expected only the size-%g one)", v.L, v.kMouth, w.food.size(), v.above);
                t.expect(c.foodReserves == 60.0f, "L %g: reserves %.9g, expected 50 + 2 x 5", v.L, c.foodReserves);
            } else {
                t.expect(w.food.empty() && c.foodReserves == 65.0f, "gate off: %zu food left, reserves %.9g", w.food.size(),
                         c.foodReserves);
            }
            // Smell: a lone too-big particle is no food to this fish.
            OceanWorld w2;
            initQuiet(w2, 702, 240.0f, oc, true);
            w2.cfg.minSpeed = 0.0f;
            const int j = addFish(w2, {500, 500}, 100.0f, {}, 100.0f / 240.0f);
            w2.creatures[j].bodySize = v.L;
            w2.creatures[j].speed = 0.0f;
            OceanFood big;
            big.pos = {530, 500}; big.depth = 100.0f; big.size = v.above;   // 30 away: smelled, out of reach
            w2.food.push_back(big);
            w2.update(0.016f);
            const float* S = w2.creatures[j].lastSensors;
            if (gate) t.expect(S[FOOD_DENSITY] == 0.0f && S[NEAREST_FOOD_DIST] == 1.0f && w2.food.size() == 1,
                               "gated food smelled: density %.6g dist %.6g", S[FOOD_DENSITY], S[NEAREST_FOOD_DIST]);
            else t.expect(S[FOOD_DENSITY] > 0.0f && S[NEAREST_FOOD_DIST] < 0.31f, "gate off: food not smelled");
        }
    }
}

// Fingerprints every 100 steps of a world built from cfg (the harness's way).
std::vector<ofp::Fingerprint> runPrintsCfg(unsigned seed, const std::string& cfg, int pop, int steps) {
    std::vector<ofp::Fingerprint> out;
    OceanWorld w;
    std::string err;
    if (!ofp::initWorld(w, seed, 1920, 1080, 240.0f, pop, cfg.c_str(), err)) return out;
    out.push_back(ofp::fingerprint(w));
    for (int s = 1; s <= steps; ++s) {
        w.update(0.016f);
        if (s % 100 == 0) out.push_back(ofp::fingerprint(w));
    }
    return out;
}

void testE1Neutral() {
    Test t("e1_neutral");
    // Each variant adds keys that must not change the run: their dynamics
    // hashes (all but cfg) equal the base's at every print.
    struct Base { const char* name; std::string cfg; std::vector<std::pair<const char*, const char*>> vars; };
    const std::string rich = R"("currentStrength":60,"foodDrift":true,"foodPatches":4,"predatorCount":3,"predatorEconomy":true,"maxNeighbors":7)";
    const std::vector<Base> bases = {
        {"bodyModel", rich + R"(,"bodyModel":true)", {
            {"light", R"("lightEnabled":true,"L0":3,"kWater":0.02)"},
            {"wave shape, no waves", R"("waveBand":8,"waveGravity":55)"},
            {"scaled costs at 0", R"("depthCostScaled":true)"},
            {"mouthGate kMouth 2 (size 1 <= 2 x .5)", R"("mouthGate":true,"kMouth":2)"},
        }},
        {"size 1", rich, {
            {"sizeFight", R"("sizeFight":true)"},
            {"sizeDrift (legacy current through the E1 path)", R"("sizeDrift":true)"},
            {"all neutral knobs", R"("sizeFight":true,"sizeDrift":true,"mouthGate":true,"lightEnabled":true,"depthCostScaled":true,"waveBand":5)"},
        }},
    };
    int variants = 0;
    for (const Base& b : bases) {
        const auto ref = runPrintsCfg(91, "{" + b.cfg + "}", 250, 800);
        t.expect(ref.size() == 9, "%s: base run failed", b.name);
        for (const auto& v : b.vars) {
            const auto got = runPrintsCfg(91, "{" + b.cfg + "," + v.second + "}", 250, 800);
            ++variants;
            if (!t.expect(got.size() == ref.size(), "%s / %s: run failed", b.name, v.first)) continue;
            int diff = -1;
            for (size_t i = 0; i < got.size() && diff < 0; ++i) if (!sameDynamics(got[i], ref[i])) diff = (int)i;
            t.expect(diff < 0, "%s / %s: dynamics differ from print %d (step %d)", b.name, v.first, diff, diff * 100);
            t.expect(got[0].cfg != ref[0].cfg, "%s / %s: the cfg hash missed the E1 keys", b.name, v.first);
        }
    }
    t.note("%d variants x 800 steps: every dynamics hash equal to the base's", variants);
}

void testE1Bounds() {
    Test t("e1_bounds");
    struct V { const char* name; unsigned seed; float D; const char* cfg; };
    const V vs[] = {
        {"preset", 1, 240.0f, kE1PresetJson},
        {"strong waves + deep eddies + sizes", 2, 240.0f,
         R"({"waveSpeed":40,"waveBand":6,"deepCurrent":true,"currentStrength":120,"foodDrift":true,"bodyModel":true,"sizeDrift":true,"sizeFight":true,"mouthGate":true,"kMouth":1.5,"predatorCount":4,"predatorEconomy":true,"predatorMinPopulation":2})"},
        {"shallow, max waves", 3, 10.0f,
         R"({"waveSpeed":100,"waveBand":1,"waveGravity":10000,"deepCurrent":true,"currentStrength":80,"foodDrift":true,"predatorCount":3})"},
        {"gyre, toroidal, costs", 4, 600.0f,
         R"({"waveSpeed":12,"deepCurrent":true,"currentStrength":50,"currentMode":1,"toroidal":true,"foodDrift":true,"depthCostScaled":true,"cHold":0.5,"cPress":0.5,"foodDepthMode":1,"foodPatches":4})"},
    };
    long fishSteps = 0, foodMoved = 0;
    for (const V& v : vs) {
        OceanWorld w;
        std::string err;
        if (!t.expect(ofp::initWorld(w, v.seed, 1920, 1080, v.D, 300, v.cfg, err), "%s: %s", v.name, err.c_str())) continue;
        for (int s = 1; s <= 1200; ++s) {
            std::vector<float> fz;
            for (const OceanFood& f : w.food) fz.push_back(f.depth);
            w.update(0.016f);
            for (const Fish& c : w.creatures) {
                ++fishSteps;
                t.expect(c.depth >= 0.0f && c.depth <= w.D && c.pos.x >= 0.0f && c.pos.x < 1920.0f && c.pos.y >= 0.0f &&
                             c.pos.y < 1080.0f, "%s step %d: fish at (%.9g, %.9g, %.9g)", v.name, s, c.pos.x, c.pos.y, c.depth);
            }
            for (size_t k = 0; k < w.food.size(); ++k) {
                const OceanFood& f = w.food[k];
                t.expect(f.depth >= 0.0f && f.depth <= w.D && f.pos.x >= 0.0f && f.pos.x < 1920.0f && f.pos.y >= 0.0f &&
                             f.pos.y < 1080.0f, "%s step %d: food at (%.9g, %.9g, %.9g)", v.name, s, f.pos.x, f.pos.y, f.depth);
            }
            for (const OceanPredator& p : w.predators)
                t.expect(p.depth >= 0.0f && p.depth <= w.D, "%s step %d: predator depth %.9g", v.name, s, p.depth);
            if (s % 100 == 0) t.expect(ofp::countNonFinite(w) == 0, "%s step %d: non-finite state", v.name, s);
            if (s == 1) for (size_t k = 0; k < fz.size() && k < w.food.size(); ++k) foodMoved += (w.food[k].depth != fz[k]);
        }
    }
    t.note("%zu E1 worlds x 1200 steps, %ld fish-steps in bounds; food drifting in depth: %ld", sizeof vs / sizeof vs[0],
           fishSteps, foodMoved);
    t.expect(foodMoved > 0, "the flow never moved food in depth");
}

void testDrawsE1() {
    Test t("draws_e1");
    OceanConfig oc;
    oc.waveSpeed = 8.0f;
    oc.deepCurrent = true;
    oc.sizeDrift = true;
    oc.sizeFight = true;
    oc.mouthGate = true;
    oc.kMouth = 0.7f;
    oc.depthCostScaled = true;
    oc.cHold = 0.3f;
    oc.cPress = 0.2f;
    oc.lightEnabled = true;
    OceanWorld w;
    initQuiet(w, 801, 240.0f, oc, false, false, /*current*/ 50.0f);
    w.cfg.foodDrift = true;
    w.cfg.predatorCount = 4;
    w.cfg.predatorPerception = 0.0f;      // blind: they always wander (two draws each), never chase
    w.cfg.predatorCaptureRadius = 0.0f;
    int nRandom = 0;
    for (int i = 0; i < 40; ++i) {
        std::vector<Gene> g;
        if (i % 3 != 2) g.push_back(mkGene(false, 0, STEER_Z, (i % 3 == 0) ? kStrong : -kStrong));
        const bool rnd = (i % 2 == 0);
        if (rnd) g.push_back(mkGene(false, 0, MOVE_RANDOM, kStrong));
        const int k = addFish(w, {120.0f + 240.0f * (i % 8), 100.0f + 216.0f * (i / 8)}, 240.0f * (float)(i % 9) / 8.0f, g,
                              (float)(i % 3) * 0.5f);
        w.creatures[k].responsiveness = 1.0f;
        w.creatures[k].bodySize = 0.5f + 0.25f * (float)(i % 7);
        nRandom += rnd;
    }
    for (int i = 0; i < 4; ++i) {
        OceanPredator p;
        p.pos = {200.0f + 450.0f * i, 900.0f};
        p.depth = 60.0f * i;
        p.energy = 30.0f;
        w.predators.push_back(p);
    }
    Lcg g(5);
    for (int i = 0; i < 300; ++i) {   // food the flow carries (and some fish eat)
        OceanFood f;
        f.pos = {1920.0f * g.unit(), 1080.0f * g.unit()};
        f.depth = 240.0f * g.unit();
        w.food.push_back(f);
    }
    const long N = (long)w.creatures.size();
    const long expected = N + 3L * nRandom + 2L * 4;   // RANDOM_SENSOR, MOVE_RANDOM x3, predator wander x2
    int bad = 0;
    for (int s = 1; s <= 150; ++s) {
        const RandomGen before = rng;
        w.update(0.016f);
        const long d = drawsBetween(before, rng, 10000);
        bad += !t.expect(d == expected && (long)w.creatures.size() == N, "step %d: %ld draws, expected %ld (pop %zu)", s, d,
                         expected, w.creatures.size());
    }
    // A birth with every E1 knob on draws exactly what it draws without.
    for (unsigned seed : {211u, 212u}) {
        long draws[2] = {-1, -1};
        for (int on = 0; on < 2; ++on) {
            OceanConfig o = on ? oc : OceanConfig{};
            OceanWorld wb;
            initQuiet(wb, seed, 240.0f, o, false, false, 30.0f);
            const std::vector<Gene> gg = {mkGene(false, 0, INTERACT_REPRODUCE, kStrong)};
            const int i1 = addFish(wb, {960, 540}, 100.0f, gg, 0.4f), i2 = addFish(wb, {961, 541}, 110.0f, gg, 0.5f);
            for (int i : {i1, i2}) {
                Fish& f = wb.creatures[i];
                f.age = 10.0f; f.foodReserves = 100.0f; f.reproTimer = 0.0f; f.interactTimer = 0.0f;
            }
            const RandomGen before = rng;
            wb.update(0.016f);
            draws[on] = (wb.creatures.size() == 3) ? drawsBetween(before, rng) : -1;
        }
        t.expect(draws[0] > 0 && draws[0] == draws[1], "seed %u: birth step draws %ld (off) vs %ld (E1 on)", seed, draws[0], draws[1]);
    }
    t.note("150 steps with every E1 feature on: %ld draws/step == N + 3 x MOVE_RANDOM + 2 x predators", expected);
}

void testLight() {
    Test t("light");
    OceanConfig oc;
    oc.lightEnabled = true;
    oc.L0 = 1.7f;
    oc.kWater = 0.006f;
    OceanWorld w;
    initQuiet(w, 901, 240.0f, oc);
    t.expect(w.lightAt(0.0f) == 1.7f, "lightAt(0) %.9g != L0", w.lightAt(0.0f));
    float prev = w.lightAt(0.0f);
    double worst = 0.0;
    for (int k = 1; k <= 240; ++k) {
        const float L = w.lightAt((float)k);
        t.expect(L < prev && L > 0.0f, "light not falling at depth %d", k);
        worst = std::max(worst, std::abs((double)L - 1.7 * std::exp(-0.006 * k)) / 1.7);
        prev = L;
    }
    t.expect(worst < 2e-6, "lightAt off L0·exp(-kWater·d) by %.3g (relative)", worst);
    t.note("L(240) / L0 = %.4f (exp(-1.44) = 0.2369)", w.lightAt(240.0f) / 1.7f);
#ifndef OCEAN_TESTS_NO_SHIM
    t.expect(oc_init(902, 1920, 1080, 240.0, 50, R"({"lightEnabled":true,"L0":2.5,"kWater":0.01})") == 1, "oc_init: %s", oc_last_error());
    t.expect(oc_light_enabled() == 1 && oc_light_l0() == 2.5 && oc_light_kwater() == (double)0.01f, "light exports %d %g %g",
             oc_light_enabled(), oc_light_l0(), oc_light_kwater());
    t.expect(oc_set_param("lightEnabled", 0.0) == 1 && oc_light_enabled() == 0, "lightEnabled 0 via oc_set_param");
    t.expect(oc_init(902, 1920, 1080, 240.0, 50, nullptr) == 1 && oc_light_enabled() == 0, "the default page has light off");
#endif
}

#ifndef OCEAN_TESTS_NO_SHIM
void testRanges() {
    Test t("ranges");
    struct K { const char* key; double lo, hi; const char* with; };
    const K ks[] = {
        {"kBuoy", -1000, 1000, ""}, {"buoyMutation", 0, 1, ""}, {"pitchMax", 0.001, 100, ""},
        {"vDrag", 0, 10000, ""}, {"wallBand", 0, 100000, ""},
        {"cHold", 0, 100, R"("depthCostScaled":true,)"}, {"cPress", 0, 100, ""},
        {"foodDepthSpread", 0, 1e6, R"("foodDepthMode":1,"foodPatches":3,)"},
        {"waveSpeed", 0, 100, R"("deepCurrent":true,"currentStrength":80,"foodDrift":true,)"},
        {"waveBand", 1, 100000, R"("waveSpeed":100,)"}, {"waveGravity", 0.01, 10000, R"("waveSpeed":100,)"},
        {"L0", 0, 100, R"("lightEnabled":true,)"}, {"kWater", 0, 1, R"("lightEnabled":true,)"},
        {"kMouth", 0.01, 100, R"("mouthGate":true,"bodyModel":true,)"},
        // The Ocean's ranges for SimConfig keys (oceanSimConfigError).
        {"currentStrength", 0, 1000, R"("deepCurrent":true,"waveSpeed":6,"currentMode":1,"foodDrift":true,)"},
        {"currentCore", 0, 100000, R"("currentStrength":80,"foodDrift":true,)"},
        {"currentDrift", -100, 100, R"("currentStrength":80,"foodDrift":true,"deepCurrent":true,)"},
        {"currentGyreEps", -1, 1, R"("currentStrength":80,"currentMode":1,"deepCurrent":true,"foodDrift":true,)"},
        {"foodPatchRadius", 0, 100000, R"("toroidal":true,"foodPatches":3,)"},
    };
    auto cfgOf = [](const K& k, double v) {
        char num[40];
        std::snprintf(num, sizeof num, "%.9g", v);
        return std::string("{") + k.with + R"("predatorCount":2,")" + k.key + "\":" + num + "}";
    };
    int accepted = 0, refused = 0, runs = 0;
    for (const K& k : ks) {
        for (double v : {k.lo, k.hi}) {
            const std::string c = cfgOf(k, v);
            if (!t.expect(oc_init(1001, 1920, 1080, 240.0, 100, c.c_str()) == 1, "%s refused: %s", c.c_str(), oc_last_error())) continue;
            ++accepted;
            oc_step(300);
            ++runs;
            t.expect(ofp::countNonFinite(*g_world) == 0, "%s: non-finite state after 300 steps", c.c_str());
            for (const Fish& f : g_world->creatures)
                if (!t.expect(f.depth >= 0.0f && f.depth <= g_world->D, "%s: depth %.9g", c.c_str(), f.depth)) break;
        }
        const double below = k.lo - std::max(1e-6, 1e-3 * std::abs(k.lo)), above = k.hi + std::max(1e-6, 1e-3 * std::abs(k.hi));
        for (double v : {below, above, 1e39, -1e39}) {
            const std::string c = cfgOf(k, v);
            refused += t.expect(oc_init(1001, 1920, 1080, 240.0, 100, c.c_str()) == 0, "%s accepted", c.c_str());
        }
    }
    // The hole the harness found, and the bool / mode keys.
    for (const char* c : {R"({"kBuoy":-1e37})", R"({"kBuoy":1e37})", R"({"vDrag":1e30})", R"({"pitchMax":1e-9})",
                          R"({"deepCurrent":2})", R"({"sizeDrift":"yes"})", R"({"mouthGate":0.5})", R"({"foodDepthMode":2})",
                          R"({"foodDepthMode":0.5})", R"({"foodDepthMode":1e300})", R"({"waveBand":0})",
                          // the current's int keys, and the config that hung wrapCoord
                          R"({"currentMode":2})", R"({"currentMode":-1})", R"({"currentEddies":101})", R"({"currentEddies":-1})",
                          R"({"currentMode":1,"currentStrength":1e20,"deepCurrent":true,"foodDrift":true,"targetFood":1})"})
        refused += t.expect(oc_init(1002, 1920, 1080, 240.0, 100, c) == 0, "%s accepted", c);
    for (const char* c : {R"({"deepCurrent":1,"sizeDrift":true,"mouthGate":0,"foodDepthMode":1})"})
        accepted += t.expect(oc_init(1002, 1920, 1080, 240.0, 100, c) == 1, "%s refused: %s", c, oc_last_error());
    // The current's int keys at their ends, run.
    for (const char* c : {R"({"currentEddies":100,"currentStrength":1000,"foodDrift":true,"targetFood":300,"deepCurrent":true})",
                          R"({"currentEddies":0,"currentStrength":1000,"foodDrift":true,"currentMode":0})"}) {
        if (!t.expect(oc_init(1006, 1920, 1080, 240.0, 100, c) == 1, "%s refused: %s", c, oc_last_error())) continue;
        ++accepted;
        oc_step(300);
        ++runs;
        t.expect(ofp::countNonFinite(*g_world) == 0, "%s: non-finite state after 300 steps", c);
    }
    // The sea's depth (oc_init's argument): [10, 100000].
    refused += t.expect(oc_init(1003, 1920, 1080, 100001.0, 50, nullptr) == 0, "depth 100001 accepted");
    accepted += t.expect(oc_init(1003, 1920, 1080, 100000.0, 50, nullptr) == 1, "depth 100000 refused: %s", oc_last_error());
    // Live: refused as stored, the world unchanged.
    t.expect(oc_init(1004, 1920, 1080, 240.0, 100, nullptr) == 1, "oc_init");
    const ofp::Fingerprint before = ofp::fingerprint(*g_world);
    struct S { const char* k; double v; };
    for (const S& s : {S{"kBuoy", 1e37}, S{"kBuoy", 1000.5}, S{"waveSpeed", 101}, S{"waveBand", 0.5}, S{"mouthGate", 0.5},
                       S{"deepCurrent", 2}, S{"kWater", 1.5}, S{"cHold", 1e9}, S{"pitchMax", 1e-5}})
        refused += t.expect(oc_set_param(s.k, s.v) == 0, "oc_set_param(%s, %g) accepted", s.k, s.v);
    t.expect(ofp::fingerprint(*g_world) == before, "a refused oc_set_param changed the world");
    for (const S& s : {S{"kBuoy", 1000}, S{"waveSpeed", 100}, S{"waveBand", 1}, S{"mouthGate", 1}, S{"deepCurrent", 1},
                       S{"kWater", 1}, S{"cHold", 100}, S{"sizeFight", 0}})
        accepted += t.expect(oc_set_param(s.k, s.v) == 1, "oc_set_param(%s, %g) refused: %s", s.k, s.v, oc_last_error());
    oc_step(300);
    t.expect(ofp::countNonFinite(*g_world) == 0, "live extremes: non-finite state");
    // The flow's extreme corners: the smallest / largest world, waves at their limits.
    for (int big = 0; big < 2; ++big) {
        const char* c = R"({"waveSpeed":100,"waveBand":1,"waveGravity":10000,"deepCurrent":true,"currentStrength":80,"foodDrift":true,"predatorCount":2})";
        const int W = big ? 20000 : 100;
        if (!t.expect(oc_init(1005, W, W, big ? 100000.0 : 10.0, 60, c) == 1, "corner %d: %s", big, oc_last_error())) continue;
        oc_step(200);
        ++runs;
        t.expect(ofp::countNonFinite(*g_world) == 0, "corner world %dx%d: non-finite state", W, W);
        for (const OceanFood& f : g_world->food)
            if (!t.expect(f.pos.x >= 0.0f && f.pos.x < (float)W && f.pos.y >= 0.0f && f.pos.y < (float)W, "corner %d: food off the map", big)) break;
    }
    t.note("%d configs accepted, %d refused; %d runs at range ends, all finite", accepted, refused, runs);
}
#endif

void testE1Hash() {
    Test t("e1_hash");
    OceanWorld w;
    std::string err;
    ofp::initWorld(w, 1101, 1920, 1080, 240.0f, 150, R"({"waveSpeed":5,"deepCurrent":true,"currentStrength":30,"predatorCount":2})", err);
    for (int s = 0; s < 40; ++s) w.update(0.016f);
    const ofp::Fingerprint base = ofp::fingerprint(w);
    t.expect(w.waveClockStarted(), "the wave clock did not run");
    auto only = [&](const char* what, std::uint64_t ofp::Fingerprint::*field) {
        const ofp::Fingerprint p = ofp::fingerprint(w);
        bool othersSame = true;
        for (auto f : ofp::kSections)   // all but cur (a wave knob also moves the sampled flow)
            if (f != field && f != &ofp::Fingerprint::cur) othersSame &= (p.*f == base.*f);
        t.expect(p.*field != base.*field && othersSame, "a 1-ulp change in %s: hash %s", what,
                 p.*field == base.*field ? "unchanged" : "changed other sections too");
    };
    // An E1 knob that does not move the flow (light): cfg only.
    { const float keep = w.ocfg.kWater; w.ocfg.kWater = std::nextafter(keep, 1.0f); only("kWater", &ofp::Fingerprint::cfg); w.ocfg.kWater = keep; }
    { const bool keep = w.ocfg.mouthGate; w.ocfg.mouthGate = !keep; only("mouthGate", &ofp::Fingerprint::cfg); w.ocfg.mouthGate = keep; }
    { const float keep = w.food[3].size; w.food[3].size = 1.5f; only("food size", &ofp::Fingerprint::food); w.food[3].size = keep; }
    t.expect(ofp::fingerprint(w) == base, "restoring did not restore the hash");
    // The wave clock is in ctr (and moves the sampled flow in cur).
    OceanWorld w2;
    ofp::initWorld(w2, 1101, 1920, 1080, 240.0f, 150, R"({"waveSpeed":5,"deepCurrent":true,"currentStrength":30,"predatorCount":2})", err);
    for (int s = 0; s < 40; ++s) w2.update(0.016f);
    t.expect(ofp::fingerprint(w2) == base, "same world twice differs");
    // An OFF world never hashes E1: a fresh preview world == its own print (no 0xE1 tails).
    OceanWorld w3;
    ofp::initWorld(w3, 1102, 1920, 1080, 240.0f, 100, nullptr, err);
    for (int s = 0; s < 30; ++s) w3.update(0.016f);
    t.expect(!w3.waveClockStarted() && !w3.flowE1() && oceanE1AtDefaults(w3.ocfg), "a preview world has E1 state");
}

// The loops OceanWorld::wrapCoord replaced (capped: n counts the passes).
float wrapLoops(float v, float span, long& n) {
    n = 0;
    while (v < 0.0f && n < 100000000) { v += span; ++n; }
    while (v >= span && n < 100000000) { v -= span; ++n; }
    return v;
}

void testWrap() {
    Test t("wrap");
    long compared = 0, multi = 0;
    auto check = [&](float v, float span) {
        long n = 0;
        const float want = wrapLoops(v, span, n);
        const float got = OceanWorld::wrapCoord(v, span);
        ++compared;
        multi += (n > 1);
        t.expect(std::memcmp(&got, &want, sizeof got) == 0, "wrapCoord(%a, %g) = %a, the loops gave %a (%ld passes)", (double)v,
                 (double)span, (double)got, (double)want, n);
    };
    std::uint32_t s = 12345u;
    auto u01 = [&] { s = s * 1664525u + 1013904223u; return (float)(s >> 8) * (1.0f / 16777216.0f); };
    for (float span : {100.0f, 333.25f, 1080.0f, 1440.0f, 1920.0f, 2560.0f, 20000.0f, 100000.0f}) {
        // The edges of the one-pass range [-span, 2·span) and of [0, span), each ± 1 ulp.
        for (float x : {-span, 0.0f, -0.0f, span, 2.0f * span})
            for (float v : {x, std::nextafter(x, -INFINITY), std::nextafter(x, INFINITY)}) check(v, span);
        // Tiny negatives (the add rounds up to span: the loops end at 0).
        for (float v : {-1e-30f, -std::numeric_limits<float>::denorm_min(), -1e-8f, -1e-6f, -0.5f * span}) check(v, span);
        // A sweep of the one-pass range, any span ...
        for (int k = 0; k < 20000; ++k) check(-span + 3.0f * span * u01(), span);
        // ... and, for a whole-number span, |v| < 2^23 (many passes, each exact).
        if (span == std::floor(span))
            for (int k = 0; k < 300; ++k) check((u01() - 0.5f) * 16777216.0f, span);
    }
    // Far out (the loops never ended there): in [0, span), the exact remainder.
    for (float v : {7.9e17f, 1e18f, 1.5e19f, -7.9e17f, -1e18f}) {
        const std::uint64_t m = (std::uint64_t)std::abs(v) % 1920u;   // v is a whole number here
        const float want = (v >= 0.0f) ? (float)m : (m ? 1920.0f - (float)m : 0.0f);
        const float got = OceanWorld::wrapCoord(v, 1920.0f);
        t.expect(got == want && !std::signbit(got), "wrapCoord(%.9g, 1920) = %.9g, want %.9g", (double)v, (double)got, (double)want);
    }
    for (float v : {1e20f, -1e20f, 3.4e38f, -3.4e38f}) {
        const float got = OceanWorld::wrapCoord(v, 1080.0f);
        t.expect(got >= 0.0f && got < 1080.0f, "wrapCoord(%.9g, 1080) = %.9g", (double)v, (double)got);
    }
    for (float v : {INFINITY, -INFINITY, NAN}) t.expect(std::isnan(OceanWorld::wrapCoord(v, 1920.0f)), "wrapCoord(%g) not NaN", (double)v);
    // The config that hung (validation bypassed): it now steps, and the flow
    // keeps every position in the world.
    {
        OceanWorld w;
        SimConfig sc;
        applyOceanDefaults(sc);
        sc.currentMode = 1;
        sc.currentStrength = 1e20f;
        sc.foodDrift = true;
        sc.targetFood = 1;
        OceanConfig oc;
        oc.deepCurrent = true;
        rng.seed(1401);
        w.cfg = sc;
        w.ocfg = oc;
        w.init(1920, 1080, 240.0f, 0);
        for (int k = 0; k < 20; ++k) w.update(0.016f);
        bool inside = !w.creatures.empty();
        for (const Fish& f : w.creatures) inside &= f.pos.x >= 0.0f && f.pos.x < 1920.0f && f.pos.y >= 0.0f && f.pos.y < 1080.0f;
        for (const OceanFood& f : w.food) inside &= f.pos.x >= 0.0f && f.pos.x < 1920.0f && f.pos.y >= 0.0f && f.pos.y < 1080.0f;
        t.expect(inside, "currentStrength 1e20 (unvalidated): a position left the world (%zu fish, %zu food)", w.creatures.size(),
                 w.food.size());
    }
    t.note("%ld values == the loops bit for bit (%ld of them more than one pass); far / non-finite values end", compared, multi);
}

#ifndef OCEAN_TESTS_NO_SHIM
void testPreset() {
    Test t("preset");
    SimConfig sc;
    applyOceanDefaults(sc);
    OceanConfig oc;
    std::string err;
    t.expect(readOceanJson(kE1PresetJson, sc, oc, err), "kE1PresetJson: %s", err.c_str());
    t.expect(oc.waveSpeed > 0.0f && oc.deepCurrent && oc.lightEnabled && oc.depthCostScaled && sc.currentStrength > 0.0f &&
             sc.foodDrift, "the preset does not turn on waves, deep currents, light and scaled costs");
    t.expect(std::strcmp(oc_e1_preset(), kE1PresetJson) == 0, "oc_e1_preset() is not kE1PresetJson");
    t.expect(oc_init(1201, 1920, 1080, 240.0, 300, kE1PresetJson) == 1, "oc_init refused the preset: %s", oc_last_error());
    oc_step(500);
    t.expect(ofp::countNonFinite(*g_world) == 0 && g_world->flowE1() && g_world->waveClockStarted(), "preset world");
    t.note("kE1PresetJson = %s", kE1PresetJson);
}
#endif

#ifndef OCEAN_TESTS_NO_SHIM
// ocean_tests --accepts SEED STEPS [ocean_parity_driver args]: would the page
// accept this harness case? oc_init with the case's world and cfg JSON, then
// oc_set_param for each --set. wasm/ocean_parity.sh asks this of every case,
// so "extreme but valid" means valid to the shim, not just to the driver.
int acceptsMain(int argc, char** argv) {
    if (argc < 4) { std::printf("usage: --accepts SEED STEPS [driver args]\n"); return 2; }
    const unsigned seed = (unsigned)std::strtoul(argv[2], nullptr, 10);
    int W = 1920, H = 1080, pop = 300;
    double depth = 240.0;
    const char* cfg = nullptr;
    std::vector<std::pair<std::string, double>> sets;
    for (int i = 4; i < argc; ++i) {
        auto is = [&](const char* k) { return std::strcmp(argv[i], k) == 0 && i + 1 < argc; };
        if (is("--w")) W = std::atoi(argv[++i]);
        else if (is("--h")) H = std::atoi(argv[++i]);
        else if (is("--depth")) depth = std::atof(argv[++i]);
        else if (is("--pop")) pop = std::atoi(argv[++i]);
        else if (is("--cfg")) cfg = argv[++i];
        else if (is("--set")) {
            const char* s = argv[++i];
            const char* eq = std::strchr(s, '=');
            if (!eq) { std::printf("bad --set %s\n", s); return 2; }
            sets.emplace_back(std::string(s, eq - s), std::strtod(eq + 1, nullptr));
        }
        else if (is("--every") || is("--dump") || is("--dump-step") || is("--expect") || is("--logs")) ++i;
        else if (!std::strcmp(argv[i], "--capture") || !std::strcmp(argv[i], "--trace") || !std::strcmp(argv[i], "--sparse")) {}
        else { std::printf("unknown driver argument %s\n", argv[i]); return 2; }
    }
    if (!oc_init(seed, W, H, depth, pop, cfg)) { std::printf("refused by oc_init: %s\n", oc_last_error()); return 1; }
    for (const auto& s : sets)
        if (!oc_set_param(s.first.c_str(), s.second)) {
            std::printf("refused by oc_set_param(%s, %g): %s\n", s.first.c_str(), s.second, oc_last_error());
            return 1;
        }
    std::printf("accepted\n");
    return 0;
}
#endif

}  // namespace ot

int main(int argc, char** argv) {
#ifndef OCEAN_TESTS_NO_SHIM
    if (argc > 1 && std::strcmp(argv[1], "--accepts") == 0) return ot::acceptsMain(argc, argv);
#else
    (void)argc; (void)argv;
#endif
    std::printf("ocean_tests: NUM_SENSES %d, NUM_ACTIONS %d\n", (int)NUM_SENSES, (int)NUM_ACTIONS);
    ot::testBounds();
    ot::testBuoyancy();
    ot::testSteerZ();
    ot::testSensors();
    ot::testSensorSigns();
    ot::testCurrentSensors();
    ot::testWalls();
    ot::testCaptureNeutral();
    ot::testReproducible();
#ifndef OCEAN_TESTS_NO_SHIM
    ot::testReset();
#endif
    ot::testPredatorEconomy();
    ot::testPairingGates();
    ot::testFastTanh();
    ot::testChild();
    ot::testDrawsMotion();
    ot::testDrawsBirth();
    ot::testDrawsPredators();
    ot::testDrawsFood();
    // E1 physics
    ot::testDivergence();
    ot::testE1Buoyancy();
    ot::testDrift();
    ot::testCosts();
    ot::testFight();
    ot::testMouth();
    ot::testE1Neutral();
    ot::testE1Bounds();
    ot::testDrawsE1();
    ot::testLight();
#ifndef OCEAN_TESTS_NO_SHIM
    ot::testRanges();
#endif
    ot::testE1Hash();
#ifndef OCEAN_TESTS_NO_SHIM
    ot::testPreset();
#endif
    ot::testWrap();
    std::printf("OCEAN TESTS: %d passed, %d failed\n", ot::g_passed, ot::g_failed);
    return ot::g_failed ? 1 : 0;
}
