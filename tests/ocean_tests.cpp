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
//   capture       captureActivations changes no dynamics
//   reproducible  same seed twice -> identical fingerprints; another seed and a
//                 1-ulp change in any section change the hash
//   reset         oc_init / oc_reset (the shim, wasm/ocean_wasm.cpp) replay
//                 exactly, live oc_set_param changes are dropped, and the shim's
//                 world == the harness's ofp::initWorld world
//   child         child depth == the parents' midpoint, bodyBuoy within
//                 ±buoyMutation of their mean (clamped to [0, 1]), on and
//                 across the toroidal seam
//   draws_*       the rng draw count per step does not depend on values: fish
//                 motion (RANDOM_SENSOR + MOVE_RANDOM's three draws, across
//                 depth / vz / bodyBuoy / every vertical knob), a birth (across
//                 buoyMutation incl. 0, bodyBuoy, depth), predators (wander,
//                 bud, floor refill, across depth / pitch / D), food spawn
//                 (across foodDepthMode / spread)
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
               bool toroidal = false) {
    SimConfig sc;
    applyOceanDefaults(sc);
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
           a.cur == b.cur && a.rng == b.rng;
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
    // The hash sees a 1-ulp change in each section (and only that section).
    OceanWorld w;
    std::string err;
    ofp::initWorld(w, 53, 1920, 1080, 240.0f, 200, R"({"predatorCount":2})", err);
    for (int s = 0; s < 50; ++s) w.update(0.016f);
    const ofp::Fingerprint base = ofp::fingerprint(w);
    auto bump = [](float& f) { f = std::nextafter(f, 1e30f); };
    auto only = [&](const char* what, std::uint64_t ofp::Fingerprint::*field) {
        const ofp::Fingerprint p = ofp::fingerprint(w);
        bool othersSame = true;
        for (auto f : {&ofp::Fingerprint::cfg, &ofp::Fingerprint::ctr, &ofp::Fingerprint::fish, &ofp::Fingerprint::food,
                       &ofp::Fingerprint::pred, &ofp::Fingerprint::sig, &ofp::Fingerprint::cur, &ofp::Fingerprint::rng})
            if (f != field) othersSame &= (p.*f == base.*f);
        t.expect(p.*field != base.*field && p.all != base.all && othersSame, "a 1-ulp change in %s: hash %s", what,
                 p.*field == base.*field ? "unchanged" : "changed other sections too");
    };
    { const float keep = w.creatures[7].depth; bump(w.creatures[7].depth); only("fish depth", &ofp::Fingerprint::fish); w.creatures[7].depth = keep; }
    { const float keep = w.creatures[3].bodyBuoy; bump(w.creatures[3].bodyBuoy); only("fish bodyBuoy", &ofp::Fingerprint::fish); w.creatures[3].bodyBuoy = keep; }
    { const float keep = w.creatures[5].body[4].y; bump(w.creatures[5].body[4].y); only("fish body trail", &ofp::Fingerprint::fish); w.creatures[5].body[4].y = keep; }
    { const float keep = w.food[11].depth; bump(w.food[11].depth); only("food depth", &ofp::Fingerprint::food); w.food[11].depth = keep; }
    { const float keep = w.predators[1].pitch; bump(w.predators[1].pitch); only("predator pitch", &ofp::Fingerprint::pred); w.predators[1].pitch = keep; }
    { const float keep = w.ocfg.vDrag; bump(w.ocfg.vDrag); only("ocean config", &ofp::Fingerprint::cfg); w.ocfg.vDrag = keep; }
    { const auto keep = w.nextId_; w.nextId_++; only("nextId_", &ofp::Fingerprint::ctr); w.nextId_ = keep; }
    { const RandomGen keep = rng; rng.uint(); only("rng (one draw)", &ofp::Fingerprint::rng); rng = keep; }
    t.expect(ofp::fingerprint(w) == base, "restoring every bump did not restore the hash");
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

    // The harness's world (ofp::initWorld, what ocean_parity_driver runs) is the shim's.
    for (const char* c : {(const char*)nullptr, cfg,
                          R"({"pitchMax":3,"vDrag":0,"wallBand":80,"buoyMutation":0.4,"toroidal":true,"bodyModel":true,"triangularMotion":true})"}) {
        if (!t.expect(oc_init(78, 1600, 900, 300.0, 250, c) == 1, "oc_init: %s", oc_last_error())) continue;
        OceanWorld w;
        std::string err;
        ofp::initWorld(w, 78, 1600, 900, 300.0f, 250, c, err);
        t.expect(ofp::fingerprint(w) == ofp::fingerprint(*g_world), "shim vs harness differ after init (cfg %s)", c ? c : "{}");
        for (int s = 0; s < 300; ++s) w.update(0.016f);
        // fingerprint() hashes the global rng: re-run the shim from its seed so both end at the same rng state.
        oc_reset();
        oc_step(300);
        t.expect(ofp::fingerprint(w) == ofp::fingerprint(*g_world), "shim vs harness differ after 300 steps (cfg %s)", c ? c : "{}");
    }

    // A fresh OceanWorld re-inited from the same seed replays; reusing ONE
    // object does not (init() leaves WorldCommon's frame / nextId_ / totals),
    // which oc_reset avoids by always allocating a new world.
    {
        OceanWorld a;
        std::string err;
        ofp::initWorld(a, 79, 1920, 1080, 240.0f, 200, nullptr, err);
        const ofp::Fingerprint a0 = ofp::fingerprint(a);
        for (int s = 0; s < 200; ++s) a.update(0.016f);
        ofp::initWorld(a, 79, 1920, 1080, 240.0f, 200, nullptr, err);
        const ofp::Fingerprint a1 = ofp::fingerprint(a);
        if (a1 == a0) t.note("re-init of a used OceanWorld object == fresh");
        else t.note("re-init of a USED OceanWorld object != fresh (frame %llu, nextId %llu carried over; fish %s, "
                    "food %s) - as Classic's World::init; the shim never reuses a world",
                    a.frame, (unsigned long long)a.nextId_, a1.fish == a0.fish ? "same" : "differ",
                    a1.food == a0.food ? "same" : "differ");
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
        else if (is("--every") || is("--dump") || is("--dump-step")) ++i;
        else if (!std::strcmp(argv[i], "--capture") || !std::strcmp(argv[i], "--trace")) {}
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
    ot::testCaptureNeutral();
    ot::testReproducible();
#ifndef OCEAN_TESTS_NO_SHIM
    ot::testReset();
#endif
    ot::testChild();
    ot::testDrawsMotion();
    ot::testDrawsBirth();
    ot::testDrawsPredators();
    ot::testDrawsFood();
    std::printf("OCEAN TESTS: %d passed, %d failed\n", ot::g_passed, ot::g_failed);
    return ot::g_failed ? 1 : 0;
}
