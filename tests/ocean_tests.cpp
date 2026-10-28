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
//   steer_frame   steerFrame 1: STEER_X turns every heading right (+) / left (−)
//                 by the same angle, STEER_Y does not turn; steerFrame 0 pulls
//                 toward world +x / +y; triangular motion ignores it; (eye build)
//                 an EYE_RIGHT / EYE_LEFT -> STEER_X wire turns toward the food
//   sensors       LOC_Z, OWN_VEL_Z, BOUNDARY_DIST_Z equal their defining
//                 expressions; NEAREST_FOOD_DZ / AVG_NEIGHBOR_DZ in range; the
//                 food direction is a unit vector; every sensor finite
//   capture       captureActivations changes no dynamics
//   eye           (eye build) a hand-built scene's 18 eye inputs bit for bit;
//                 off = 0; in range; no side effects (eyeCode 0 and 7)
//   eye_only      (eye build) eyeOnly: the 23 labeled inputs 0, every other
//                 input bit-identical to the labeled run's; blind likewise;
//                 also with eyeCode 7
//   eye_code      (eye build) eyeCode: a scene with several objects per sector,
//                 the 18 inputs bit for bit for every code 0..7 (and 4 / 7
//                 eye-only); non-eye inputs unchanged; faded contrast 0 at R;
//                 ranges in populated worlds; 0..7 valid, live via oc_set_param
//                 (plain build: eyeCode must stay 0)
//   food_life     foodLife: an item goes on the step its age reaches foodLife;
//                 off, none do; the first stock's ages spread; draws unchanged
//   reproducible  same seed twice -> identical fingerprints; another seed and a
//                 1-ulp change in any section change the hash (eye build: also a
//                 food item's eyeCon, which countNonFinite checks too)
//   radii         every radius must be finite, >= 0 and have a finite float
//                 square (oc_init refuses 1e20, accepts the largest float below
//                 2^64); (eye build) at that perceptionRadius every input is finite
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

// OceanConfig::steerFrame: 0 = STEER_X / STEER_Y push along world x / y, 1 = along
// the fish's right axis / heading.
void testSteerFrame() {
    Test t("steer_frame");
    {
        OceanConfig oc;
        t.expect(oceanConfigValid(oc) && oc.steerFrame == 0, "default steerFrame %d (or refused)", oc.steerFrame);
        oc.steerFrame = 1;
        t.expect(oceanConfigValid(oc), "steerFrame 1 refused");
        oc.steerFrame = 2;
        t.expect(!oceanConfigValid(oc), "steerFrame 2 accepted");
        oc.steerFrame = -1;
        t.expect(!oceanConfigValid(oc), "steerFrame -1 accepted");
    }
    const float kPi = 3.14159265f;
    auto angle = [](Vec2 v) { return std::atan2(v.y, v.x); };
    auto turned = [&](Vec2 a, Vec2 b) {   // signed heading change a -> b, + = toward a's right
        float d = angle(b) - angle(a);
        if (d > kPi) d -= 2.0f * kPi;
        if (d < -kPi) d += 2.0f * kPi;
        return d;
    };
    // One world per frame: fish at 8 headings wired neuron 0 -> STEER_X (+), at 8
    // wired -> STEER_X (−), at 8 wired -> STEER_Y (+), plus an unwired one.
    auto build = [&](OceanWorld& w, int frame) {
        OceanConfig oc;
        oc.kBuoy = 0.0f;
        oc.wallBand = 0.0f;
        oc.steerFrame = frame;
        initQuiet(w, 23, 240.0f, oc, /*capture*/ true);
        for (int set = 0; set < 3; ++set)
            for (int h = 0; h < 8; ++h) {
                const Gene g = (set == 2) ? mkGene(false, 0, STEER_Y, kStrong)
                                          : mkGene(false, 0, STEER_X, set == 0 ? kStrong : -kStrong);
                const int k = addFish(w, {150.0f + 200.0f * h, 200.0f + 300.0f * set}, 120.0f, {g}, 0.5f);
                const float a = 2.0f * kPi * (float)h / 8.0f + 0.2f;
                Fish& f = w.creatures[k];
                f.vel = {std::cos(a), std::sin(a)};
                f.lastMoveDir = f.vel;
                f.responsiveness = 1.0f;
            }
        addFish(w, {960.0f, 1000.0f}, 120.0f, {}, 0.5f);
    };
    OceanWorld own, wld;
    build(own, 1);
    build(wld, 0);
    const int N = (int)own.creatures.size();
    std::vector<Vec2> h0(N), w0(N);
    for (int i = 0; i < N; ++i) { h0[i] = own.creatures[i].vel; w0[i] = wld.creatures[i].vel; }
    own.update(0.016f);
    wld.update(0.016f);
    // Own frame: STEER_X (+) turns every heading right by the same angle, (−) left by
    // the same angle; STEER_Y (along the heading) does not turn it.
    const float ref = turned(h0[0], own.creatures[0].vel);
    t.expect(ref > 0.03f && ref < 0.06f, "own frame: STEER_X (+) turned %.6f rad in one step (expected ~0.046)", ref);
    for (int h = 0; h < 8; ++h) {
        const float r = turned(h0[h], own.creatures[h].vel);
        const float l = turned(h0[8 + h], own.creatures[8 + h].vel);
        const float y = turned(h0[16 + h], own.creatures[16 + h].vel);
        t.expect(std::abs(r - ref) < 1e-5f, "own frame, heading %d: STEER_X (+) turned %.7f, heading 0 turned %.7f", h, r, ref);
        t.expect(std::abs(l + ref) < 1e-5f, "own frame, heading %d: STEER_X (-) turned %.7f, expected %.7f", h, l, -ref);
        t.expect(std::abs(y) < 1e-6f, "own frame, heading %d: STEER_Y (ahead) turned the fish %.9f", h, y);
        // The eye's and PREDATOR_LR's right is (-fwd.y, fwd.x): the new heading leans that way.
        const Vec2 rht = {-h0[h].y, h0[h].x};
        const Vec2 v = own.creatures[h].vel;
        t.expect(v.x * rht.x + v.y * rht.y > 0.0f, "own frame, heading %d: STEER_X (+) did not lean toward (-fwd.y, fwd.x)", h);
    }
    t.expect(own.creatures[N - 1].vel.x == h0[N - 1].x && own.creatures[N - 1].vel.y == h0[N - 1].y,
             "own frame: the unwired fish turned");
    // World frame (Classic): STEER_X (+) pulls every heading toward +x, STEER_Y (+)
    // toward +y; the angle depends on the heading.
    float wmin = 1e9f, wmax = -1e9f;
    for (int h = 0; h < 8; ++h) {
        const Vec2 a = w0[h], b = wld.creatures[h].vel, c = w0[16 + h], d = wld.creatures[16 + h].vel;
        t.expect(b.x > a.x, "world frame, heading %d: STEER_X (+) did not pull toward +x (%.6f -> %.6f)", h, a.x, b.x);
        t.expect(d.y > c.y, "world frame, heading %d: STEER_Y (+) did not pull toward +y (%.6f -> %.6f)", h, c.y, d.y);
        const float r = std::abs(turned(a, b));
        wmin = std::min(wmin, r); wmax = std::max(wmax, r);
    }
    t.expect(wmax - wmin > 0.01f, "world frame: STEER_X turned every heading alike (%.6f..%.6f)", wmin, wmax);
    // A STEER_X (+) fish in its own frame circles: after 200 steps it has turned
    // right every step (no reversal).
    {
        bool allRight = true;
        for (int s = 0; s < 200; ++s) {
            const Vec2 a = own.creatures[3].vel;
            own.update(0.016f);
            allRight &= turned(a, own.creatures[3].vel) > 0.0f;
        }
        t.expect(allRight, "own frame: a STEER_X (+) fish did not turn right on every step");
    }
    t.note("one step: own frame %.5f rad for every heading (STEER_Y 0); world frame %.5f..%.5f by heading", ref, wmin, wmax);

    // Triangular motion already turns by STEER_X in the fish's frame and ignores
    // steerFrame: the same fish, the same state, step for step.
    {
        OceanWorld a, b;
        for (OceanWorld* w : {&a, &b}) {
            OceanConfig oc;
            oc.steerFrame = (w == &b) ? 1 : 0;
            initQuiet(*w, 24, 240.0f, oc);
            w->cfg.triangularMotion = true;
            for (int i = 0; i < 12; ++i)
                addFish(*w, {100.0f + 140.0f * i, 300.0f + 40.0f * i}, 20.0f * i,
                        {mkGene(false, 0, STEER_X, (i % 2) ? kStrong : -kStrong),
                         mkGene(false, 0, MOVE_FORWARD, (i % 3) * 8000 - 8000)}, 0.5f);
        }
        // Each world steps on its own copy of the rng (initQuiet seeded both alike),
        // so a draw difference shows as different engines.
        RandomGen ra = rng, rb = rng;
        bool same = true, sameDraws = true;
        for (int s = 0; s < 300 && same && sameDraws; ++s) {
            rng = ra; a.update(0.016f); ra = rng;
            rng = rb; b.update(0.016f); rb = rng;
            const ofp::Fingerprint fa = ofp::fingerprint(a), fb = ofp::fingerprint(b);
            same = fa.fish == fb.fish && fa.ctr == fb.ctr;
            sameDraws = sameEngine(ra, rb);
        }
        t.expect(same, "triangular motion: steerFrame 1 changed the fish");
        t.expect(sameDraws, "triangular motion: steerFrame 1 changed the draws");
    }
#ifdef EVO_OCEAN_EYE
    // The compound eye and the own-frame push agree on "right": an EYE_RIGHT_PROX ->
    // STEER_X (+) wire turns a fish toward food on its right, and EYE_LEFT_PROX ->
    // STEER_X (−) toward food on its left; with steerFrame 0 the same wires do not.
    {
        int toward[2][2] = {};   // [frame][side]
        for (int frame = 0; frame < 2; ++frame)
            for (int side = 0; side < 2; ++side)
                for (int h = 0; h < 8; ++h) {
                    OceanConfig oc;
                    oc.kBuoy = 0.0f;
                    oc.wallBand = 0.0f;
                    oc.eye = 1;
                    oc.steerFrame = frame;
                    OceanWorld w;
                    initQuiet(w, 25, 240.0f, oc);
                    const Gene g = side ? mkGene(true, EYE_RIGHT_PROX, STEER_X, kStrong)
                                        : mkGene(true, EYE_LEFT_PROX, STEER_X, -kStrong);
                    const int k = addFish(w, {960.0f, 540.0f}, 120.0f, {g}, 0.5f);
                    const float a = 2.0f * kPi * (float)h / 8.0f + 0.2f;
                    Fish& f = w.creatures[k];
                    f.vel = {std::cos(a), std::sin(a)};
                    f.lastMoveDir = f.vel;
                    f.responsiveness = 1.0f;
                    const Vec2 rht = {-f.vel.y, f.vel.x};
                    const float sgn = side ? 1.0f : -1.0f;
                    OceanFood fd;
                    fd.pos = {960.0f + sgn * 60.0f * rht.x, 540.0f + sgn * 60.0f * rht.y};
                    fd.depth = 120.0f;
                    fd.eyeCon = ocean::eyeContrast(ocean::kEyeFoodBright, fd.depth);
                    w.food.push_back(fd);
                    const Vec2 v0 = f.vel;
                    for (int s = 0; s < 3; ++s) w.update(0.016f);
                    const float tr = turned(v0, w.creatures[k].vel);
                    toward[frame][side] += (sgn * tr > 0.01f);
                }
        t.expect(toward[1][0] == 8 && toward[1][1] == 8,
                 "own frame: the eye-wired fish turned toward food on the left %d/8, right %d/8", toward[1][0],
                 toward[1][1]);
        t.expect(toward[0][0] < 8 || toward[0][1] < 8,
                 "world frame: the same wires turned toward the food at every heading (left %d/8, right %d/8)",
                 toward[0][0], toward[0][1]);
        t.note("eye -> STEER_X toward food: own frame left %d/8 right %d/8; world frame left %d/8 right %d/8",
               toward[1][0], toward[1][1], toward[0][0], toward[0][1]);
    }
#endif
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

// ---- the compound eye -------------------------------------------------------
#ifdef EVO_OCEAN_EYE
// One sector's three inputs, as eyeWrite computes them, for an object at
// squared distance d2 with world size s and contrast con.
void eyeExpect(float d2, float s, float con, float R, float out[3]) {
    const float d = std::sqrt(d2);
    out[0] = 1.0f - d / R;
    out[1] = s / (s + d);
    out[2] = con;
}

void testEye() {
    Test t("eye");
    // (1) A hand-built scene, checked bit for bit: a viewer heading +x at depth
    // 120, and around it food ahead (30) with a fish further ahead (60), a fish
    // behind (50), food to the left (-y, 20), a predator to the right (+y, 70),
    // a fish above (25 shallower) and a predator below (40 deeper), plus a food
    // item beyond R that must not count. Nothing is wired, so nothing moves the
    // viewer's state before it senses (it steps first).
    {
        OceanConfig oc;
        oc.eye = 1;
        OceanWorld w;
        initQuiet(w, 61, 240.0f, oc, /*capture*/ true);
        const Vec2 P = {600.0f, 500.0f};
        const float z = 120.0f;
        const int v = addFish(w, P, z, {}, 0.5f);
        w.creatures[v].vel = {1.0f, 0.0f};
        const int fAhead = addFish(w, {P.x + 60.0f, P.y}, z, {}, 0.5f);
        const int fBehind = addFish(w, {P.x - 50.0f, P.y}, z, {}, 0.5f);
        const int fUp = addFish(w, P, z - 25.0f, {}, 0.5f);
        w.creatures[fBehind].bodySize = 1.5f;   // a bigger fish looks bigger
        auto food = [&](float x, float y, float fz) {
            OceanFood f;
            f.pos = {x, y};
            f.depth = fz;
            f.eyeCon = ocean::eyeContrast(ocean::kEyeFoodBright, fz);
            w.food.push_back(f);
        };
        food(P.x + 30.0f, P.y, z);
        food(P.x, P.y - 20.0f, z);
        food(P.x + 101.0f, P.y, z);   // beyond R = 100
        auto pred = [&](float x, float y, float pz) {
            OceanPredator p;
            p.pos = {x, y};
            p.vel = {1.0f, 0.0f};
            p.depth = pz;
            p.digestTimer = 1e9f;   // never eats in this test
            w.predators.push_back(p);
        };
        pred(P.x, P.y + 70.0f, z);
        pred(P.x, P.y, z + 40.0f);
        (void)fAhead; (void)fUp;
        const float R = w.cfg.perceptionRadius;
        w.update(0.016f);
        const float* S = w.creatures[v].lastSensors;
        float want[ocean::kEyeSectors][3];
        eyeExpect(900.0f, ocean::kEyeFoodSize, ocean::eyeContrast(ocean::kEyeFoodBright, z), R, want[0]);
        eyeExpect(2500.0f, ocean::kEyeFishLen * 1.5f, ocean::eyeContrast(ocean::kEyeFishBright, z), R, want[1]);
        eyeExpect(400.0f, ocean::kEyeFoodSize, ocean::eyeContrast(ocean::kEyeFoodBright, z), R, want[2]);
        eyeExpect(4900.0f, ocean::kEyePredSize, ocean::eyeContrast(ocean::kEyePredBright, z), R, want[3]);
        eyeExpect(625.0f, ocean::kEyeFishLen * 1.0f, ocean::eyeContrast(ocean::kEyeFishBright, z - 25.0f), R, want[4]);
        eyeExpect(1600.0f, ocean::kEyePredSize, ocean::eyeContrast(ocean::kEyePredBright, z + 40.0f), R, want[5]);
        static const char* kSec[] = {"ahead", "behind", "left", "right", "up", "down"};
        for (int k = 0; k < ocean::kEyeSectors; ++k)
            for (int f = 0; f < 3; ++f) {
                const float got = S[ocean::kEyeFirst + 3 * k + f];
                t.expect(got == want[k][f], "scene: %s feature %d = %.9g, expected %.9g", kSec[k], f,
                         got, want[k][f]);
            }
        t.note("scene: ahead food %.3f/%.3f/%.3f, behind fish %.3f/%.3f/%.3f, down predator %.3f/%.3f/%.3f",
               S[EYE_AHEAD_PROX], S[EYE_AHEAD_SIZE], S[EYE_AHEAD_CON], S[EYE_BEHIND_PROX],
               S[EYE_BEHIND_SIZE], S[EYE_BEHIND_CON], S[EYE_DOWN_PROX], S[EYE_DOWN_SIZE], S[EYE_DOWN_CON]);
    }
    // (2) Populated worlds, every fish-step: off -> the 18 inputs are exactly 0;
    // on -> each sector is empty (0, 0, 0) or holds an object (PROX in [0, 1],
    // SIZE in (0, 1], CON in [-1, 1]); every input finite.
    {
        long onSteps = 0, offSteps = 0, seen = 0, slots = 0;
        for (int eyeOn = 0; eyeOn < 2; ++eyeOn) {
            const char* cfgs[] = {
                R"({"predatorCount":4,"foodPatches":3,"foodDepthMode":1})",
                R"({"maxNeighbors":7,"toroidal":true,"predatorCount":6,"predatorEconomy":true,"currentStrength":60})",
            };
            for (const char* base : cfgs) {
                std::string cfg = std::string(base);
                cfg.back() = ',';
                cfg += eyeOn ? "\"eye\":1}" : "\"eye\":0}";
                OceanWorld w;
                std::string err;
                if (!t.expect(ofp::initWorld(w, 62 + eyeOn, 1920, 1080, 240.0f, 500, cfg.c_str(), err),
                              "init: %s", err.c_str()))
                    continue;
                w.cfg.captureActivations = true;
                for (int s = 0; s < 300; ++s) {
                    w.update(0.016f);
                    for (const Fish& c : w.creatures) {
                        if (c.age <= 0.0f) continue;   // reseeded / born after sensing
                        const float* E = c.lastSensors + ocean::kEyeFirst;
                        if (!eyeOn) {
                            ++offSteps;
                            bool zero = true;
                            for (int k = 0; k < ocean::kEyeInputs; ++k) zero &= (E[k] == 0.0f);
                            t.expect(zero, "eye off but an eye input is not 0");
                            continue;
                        }
                        ++onSteps;
                        for (int k = 0; k < ocean::kEyeSectors; ++k) {
                            const float p = E[3 * k], sz = E[3 * k + 1], cn = E[3 * k + 2];
                            ++slots;
                            t.expect(finite(p) && finite(sz) && finite(cn), "non-finite eye input");
                            if (p == 0.0f && sz == 0.0f && cn == 0.0f) continue;
                            ++seen;
                            t.expect(p >= 0.0f && p <= 1.0f && sz > 0.0f && sz <= 1.0f && cn >= -1.0f &&
                                         cn <= 1.0f,
                                     "eye sector %d out of range: %.9g %.9g %.9g", k, p, sz, cn);
                        }
                    }
                }
            }
        }
        t.expect(onSteps > 10000 && offSteps > 10000 && seen > slots / 10,
                 "too little to test: on %ld off %ld, %ld of %ld sectors saw something", onSteps,
                 offSteps, seen, slots);
        t.note("populated: %ld fish-steps on, %ld off; %.1f%% of sectors held an object", onSteps,
               offSteps, slots ? 100.0 * seen / slots : 0.0);
    }
    // (3) The eye has no side effects: fish with no wiring act the same whatever
    // they see, so a world of them (with food and wandering predators that draw
    // and capture) must run identically with the eye on and off — same state,
    // same draws — and differ only in lastSensors. (k = 2: the eye on with every
    // eyeCode bit set.)
    {
        ofp::Fingerprint fp[3];
        long nonzero = 0;
        for (int k = 0; k < 3; ++k) {
            OceanConfig oc;
            oc.eye = k ? 1 : 0;
            oc.eyeCode = (k == 2) ? kEyeCodeAll : 0;
            OceanWorld w;
            initQuiet(w, 64, 240.0f, oc, /*capture*/ true);
            w.cfg.predatorCount = 3;
            for (int j = 0; j < 400; ++j) {
                const Vec2 p = {rng.range(10.0f, 1910.0f), rng.range(10.0f, 1070.0f)};
                addFish(w, p, rng.range(0.0f, 240.0f), {}, 0.5f);
            }
            for (int j = 0; j < 600; ++j) w.spawnFood();
            for (int j = 0; j < 3; ++j) {
                OceanPredator p;
                p.pos = {rng.range(10.0f, 1910.0f), rng.range(10.0f, 1070.0f)};
                p.vel = {1.0f, 0.0f};
                p.depth = rng.range(0.0f, 240.0f);
                w.predators.push_back(p);
            }
            for (int s = 0; s < 400; ++s) w.update(0.016f);
            fp[k] = ofp::fingerprint(w, /*withActivations*/ false);
            if (k == 1)
                for (const Fish& c : w.creatures)
                    for (int e = 0; e < ocean::kEyeInputs; ++e) nonzero += (c.lastSensors[ocean::kEyeFirst + e] != 0.0f);
        }
        for (int k = 1; k < 3; ++k)
            t.expect(fp[0].ctr == fp[k].ctr && fp[0].fish == fp[k].fish && fp[0].food == fp[k].food &&
                         fp[0].pred == fp[k].pred && fp[0].rng == fp[k].rng,
                     "the eye (eyeCode %d) changed an unwired world (fish %016llx vs %016llx, rng %016llx vs %016llx)",
                     k == 2 ? kEyeCodeAll : 0, (unsigned long long)fp[0].fish, (unsigned long long)fp[k].fish,
                     (unsigned long long)fp[0].rng, (unsigned long long)fp[k].rng);
        t.expect(nonzero > 0, "the eye saw nothing in the side-effect world");
    }
}

// Eye-only mode (OceanConfig::eyeOnly). An unwired world (as in testEye (3):
// fish act the same whatever they sense) is run with the labels on and off,
// eye on and off: every run must have the same state and draws; in eye-only
// mode the 23 labeled inputs are exactly 0 every fish-step, and every other
// input (own state, pheromones, current, RANDOM_SENSOR, and the 18 eye inputs)
// is bit-identical to the labeled run's, fish-step by fish-step.
void testEyeOnly() {
    Test t("eye_only");
    t.expect(ocean::kNumLabeledSenses == 23, "labeled list has %d senses", ocean::kNumLabeledSenses);
    {
        OceanConfig oc;
        oc.eyeOnly = 1;
        t.expect(oceanConfigValid(oc), "eyeOnly = 1 refused by the eye build");
        oc.eye = 1;
        t.expect(oceanConfigValid(oc), "eye 1 + eyeOnly 1 refused");
        oc.eyeOnly = 2;
        t.expect(!oceanConfigValid(oc), "eyeOnly = 2 accepted");
    }
    struct Run { ofp::Fingerprint fp; std::vector<std::uint64_t> kept; long labeledNonZero = 0, fishSteps = 0; };
    auto run = [&](int eye, int eyeOnly, bool toroidal, int maxNeighbors, int eyeCode) {
        Run r;
        OceanConfig oc;
        oc.eye = eye;
        oc.eyeOnly = eyeOnly;
        oc.eyeCode = eyeCode;
        OceanWorld w;
        initQuiet(w, 65, 240.0f, oc, /*capture*/ true, toroidal);
        w.cfg.maxNeighbors = maxNeighbors;
        w.cfg.predatorCount = 3;
        w.cfg.currentStrength = 60.0f;   // CURRENT_FWD / LR non-zero (the field is built at
        w.init(1920, 1080, 240.0f, 0);   // init: rebuild; init also makes the 3 hunters)
        for (int j = 0; j < 400; ++j) {
            const Vec2 p = {rng.range(10.0f, 1910.0f), rng.range(10.0f, 1070.0f)};
            // One EMIT_SIGNAL0 wire from an undriven neuron (0.5): pheromones,
            // the same in every mode; nothing else is wired.
            addFish(w, p, rng.range(0.0f, 240.0f), {mkGene(false, 0, EMIT_SIGNAL0, kStrong)}, 0.5f);
        }
        for (int j = 0; j < 600; ++j) w.spawnFood();
        for (int s = 0; s < 300; ++s) {
            w.update(0.016f);
            ofp::Fnv h;
            for (const Fish& c : w.creatures) {
                if (c.age <= 0.0f) continue;
                ++r.fishSteps;
                for (int k = 0; k < NUM_SENSES; ++k) {
                    if (ocean::isLabeledSense(k)) { r.labeledNonZero += (c.lastSensors[k] != 0.0f); continue; }
                    h.f32(c.lastSensors[k]);
                }
            }
            r.kept.push_back(h.h);
        }
        r.fp = ofp::fingerprint(w, /*withActivations*/ false);
        return r;
    };
    struct Case { bool toroidal; int maxNeighbors; };
    for (const Case cs : {Case{false, 0}, Case{true, 7}}) {
        for (int mode = 0; mode < 3; ++mode) {   // blind, eye-only, eye-only with eyeCode 7
            const int eye = mode ? 1 : 0, code = (mode == 2) ? kEyeCodeAll : 0;
            const Run lab = run(eye, 0, cs.toroidal, cs.maxNeighbors, code);
            const Run only = run(eye, 1, cs.toroidal, cs.maxNeighbors, code);
            const char* name = (mode == 0) ? "blind" : (mode == 1) ? "eye-only" : "eye-only (eyeCode 7)";
            t.expect(only.fp.ctr == lab.fp.ctr && only.fp.fish == lab.fp.fish && only.fp.food == lab.fp.food &&
                         only.fp.pred == lab.fp.pred && only.fp.sig == lab.fp.sig && only.fp.rng == lab.fp.rng,
                     "%s (toroidal %d, topo %d) changed an unwired world (fish %016llx vs %016llx, rng %016llx vs %016llx)",
                     name, cs.toroidal, cs.maxNeighbors, (unsigned long long)only.fp.fish,
                     (unsigned long long)lab.fp.fish, (unsigned long long)only.fp.rng, (unsigned long long)lab.fp.rng);
            t.expect(only.labeledNonZero == 0, "%s: %ld labeled inputs not 0", name, only.labeledNonZero);
            t.expect(lab.labeledNonZero > lab.fishSteps, "labels on but the labeled inputs are nearly all 0 (%ld)",
                     lab.labeledNonZero);
            t.expect(only.kept == lab.kept && only.fishSteps == lab.fishSteps && only.fishSteps > 100000,
                     "%s (toroidal %d, topo %d): a kept / eye input differs from the labeled run's", name,
                     cs.toroidal, cs.maxNeighbors);
        }
    }
    t.note("23 labeled inputs 0 in eye-only and blind; kept + eye inputs bit-identical to the labeled runs"
           " (eyeCode 0 and 7)");
}

// OceanConfig::eyeCode (bit 0 faded contrast, bit 1 opponent pairs, bit 2
// summed sectors). (1) A hand-built scene with several objects per sector, for
// every code 0..7 (and code 4 / 7 in eye-only mode): the 18 inputs bit for bit
// against the definitions, computed here the same way. The engine visits fish
// (grid order), then food (grid order), then predators (index order); float
// sums depend on order, so every sector holds at most two objects of a group
// whose order this test does not fix, and only as its first terms (a + b ==
// b + a), plus objects of later groups one at a time or in index order.
// (2) Faded contrast is 0 at R. (3) Ranges in populated worlds. (4) Validity.
void testEyeCode() {
    Test t("eye_code");
    const float R = 100.0f;   // applyOceanDefaults' perceptionRadius
    const Vec2 P = {600.0f, 500.0f};
    const float z = 120.0f;
    enum { kFish, kFood, kPred };
    struct Obj { int type; float dx, dy, dz, bodySize; int sector; };
    // Listed in the engine's group order (fish, food, predators by index).
    const Obj objs[] = {
        {kFish, 60, 0, 0, 1.0f, 0},   {kFish, 45, 10, 5, 0.8f, 0},   // ahead: 2 fish,
        {kFish, 5, -40, 0, 1.5f, 2},                                  // left
        {kFish, 10, 35, -5, 1.2f, 3},                                 // right
        {kFish, 0, 0, -25, 1.0f, 4},                                  // up
        {kFish, 2, 2, 30, 1.0f, 5},                                   // down
        {kFish, 0, 0, 101, 1.0f, -1},                                 // beyond R
        {kFood, 30, 0, 0, 0, 0},                                      // ahead: + 1 food
        {kFood, -70, 5, 0, 0, 1},     {kFood, -20, -3, 10, 0, 1},     // behind: 2 food
        {kFood, 0, -20, 0, 0, 2},                                     // left: fish + food
        {kFood, 3, 2, -60, 0, 4},                                     // up: fish + food
        {kFood, 101, 0, 0, 0, -1},                                    // beyond R
        {kPred, 85, -20, 0, 0, 0},                                    // ahead: + 1 predator
        {kPred, -90, 0, 10, 0, 1},                                    // behind: + 1 predator
        {kPred, 0, 70, 0, 0, 3},                                      // right: fish + predator
        {kPred, 0, 0, 40, 0, 5},      {kPred, 5, -5, 90, 0, 5},       // down: fish + 2 predators
    };
    const int nObj = (int)(sizeof objs / sizeof objs[0]);
    // Build the scene; returns the viewer's index.
    auto build = [&](OceanWorld& w, int code, int eyeOnly) {
        OceanConfig oc;
        oc.eye = 1;
        oc.eyeCode = code;
        oc.eyeOnly = eyeOnly;
        initQuiet(w, 66, 240.0f, oc, /*capture*/ true);
        const int v = addFish(w, P, z, {}, 0.5f);
        w.creatures[v].vel = {1.0f, 0.0f};
        for (const Obj& o : objs) {
            const Vec2 pos = {P.x + o.dx, P.y + o.dy};
            const float oz = z + o.dz;
            if (o.type == kFish) {
                const int k = addFish(w, pos, oz, {}, 0.5f);
                w.creatures[k].bodySize = o.bodySize;
            } else if (o.type == kFood) {
                OceanFood f;
                f.pos = pos;
                f.depth = oz;
                f.eyeCon = ocean::eyeContrast(ocean::kEyeFoodBright, oz);
                w.food.push_back(f);
            } else {
                OceanPredator p;
                p.pos = pos;
                p.vel = {1.0f, 0.0f};
                p.depth = oz;
                p.digestTimer = 1e9f;
                w.predators.push_back(p);
            }
        }
        return v;
    };
    // The expected 18 inputs for a code, from the definitions.
    auto expected = [&](int code, float out[ocean::kEyeInputs]) {
        float nd2[6], nsize[6], ncon[6], sp[6] = {}, ss[6] = {}, sc[6] = {};
        for (int k = 0; k < 6; ++k) { nd2[k] = std::numeric_limits<float>::infinity(); nsize[k] = ncon[k] = 0.0f; }
        for (const Obj& o : objs) {
            const Vec2 pos = {P.x + o.dx, P.y + o.dy};
            const float oz = z + o.dz;
            const float dx = pos.x - P.x, dy = pos.y - P.y, dz = oz - z;
            const float d2 = dx * dx + dy * dy + dz * dz;
            if (d2 > R * R) { t.expect(o.sector < 0, "object (%g, %g, %g) is beyond R", o.dx, o.dy, o.dz); continue; }
            // The sector rule (ahead +x, right +y, down +depth), checked against the plan.
            const float a = std::abs(dx), b = std::abs(dy), c = std::abs(dz);
            const int k = (a >= b && a >= c) ? (dx >= 0.0f ? 0 : 1) : (b >= c) ? (dy <= 0.0f ? 2 : 3) : (dz <= 0.0f ? 4 : 5);
            t.expect(k == o.sector, "object (%g, %g, %g) falls in sector %d, planned %d", o.dx, o.dy, o.dz, k, o.sector);
            const float s = (o.type == kFish) ? ocean::kEyeFishLen * o.bodySize
                          : (o.type == kFood) ? ocean::kEyeFoodSize : ocean::kEyePredSize;
            const float con = ocean::eyeContrast(o.type == kFish ? ocean::kEyeFishBright
                                                 : o.type == kFood ? ocean::kEyeFoodBright : ocean::kEyePredBright, oz);
            if (d2 < nd2[k]) { nd2[k] = d2; nsize[k] = s; ncon[k] = con; }
            const float d = std::sqrt(d2), p = 1.0f - d / R;
            sp[k] += p;
            ss[k] += s / (s + d);
            sc[k] += p * con;
        }
        auto q = [](float x) { return x / (1.0f + std::abs(x)); };
        for (int k = 0; k < 6; ++k) {
            float* e = out + 3 * k;
            if (code & kEyeCodeSummed) { e[0] = q(sp[k]); e[1] = q(ss[k]); e[2] = q(sc[k]); continue; }
            if (!(nd2[k] <= R * R)) { e[0] = e[1] = e[2] = 0.0f; continue; }
            const float d = std::sqrt(nd2[k]);
            e[0] = 1.0f - d / R;
            e[1] = nsize[k] / (nsize[k] + d);
            e[2] = (code & kEyeCodeFaded) ? e[0] * ncon[k] : ncon[k];
        }
        if (code & kEyeCodeOpponent)
            for (int pr : {2, 4})
                for (int f = 0; f < 3; ++f) {
                    const float lo = out[3 * pr + f], hi = out[3 * pr + 3 + f];
                    out[3 * pr + f] = lo + hi;
                    out[3 * pr + 3 + f] = hi - lo;
                }
    };
    static const char* kSec[] = {"ahead", "behind", "left", "right", "up", "down"};
    std::vector<float> rest0;   // code 0's non-eye inputs
    float got7[ocean::kEyeInputs] = {}, got0[ocean::kEyeInputs] = {};
    struct V { int code, eyeOnly; };
    std::vector<V> vs;
    for (int c = 0; c <= kEyeCodeAll; ++c) vs.push_back({c, 0});
    vs.push_back({kEyeCodeSummed, 1});
    vs.push_back({kEyeCodeAll, 1});
    for (const V& vv : vs) {
        OceanWorld w;
        const int v = build(w, vv.code, vv.eyeOnly);
        t.expect((int)w.creatures.size() == 1 + 7 && (int)w.food.size() + (int)w.predators.size() == nObj - 7,
                 "scene has %zu fish, %zu food, %zu predators", w.creatures.size(), w.food.size(), w.predators.size());
        w.update(0.016f);
        const float* S = w.creatures[v].lastSensors;
        float want[ocean::kEyeInputs];
        expected(vv.code, want);
        for (int k = 0; k < ocean::kEyeSectors; ++k)
            for (int f = 0; f < 3; ++f) {
                const float g = S[ocean::kEyeFirst + 3 * k + f];
                t.expect(g == want[3 * k + f] && finite(g), "eyeCode %d eyeOnly %d: %s feature %d = %.9g, expected %.9g",
                         vv.code, vv.eyeOnly, kSec[k], f, g, want[3 * k + f]);
            }
        if (vv.eyeOnly) continue;
        std::vector<float> rest;
        for (int k = 0; k < NUM_SENSES; ++k) if (!ocean::isEyeSense(k)) rest.push_back(S[k]);
        if (vv.code == 0) { rest0 = rest; std::memcpy(got0, S + ocean::kEyeFirst, sizeof got0); }
        else t.expect(rest == rest0, "eyeCode %d changed a non-eye input", vv.code);
        if (vv.code == kEyeCodeAll) std::memcpy(got7, S + ocean::kEyeFirst, sizeof got7);
    }
    t.note("scene, code 0: ahead %.4f/%.4f/%.4f left %.4f/%.4f/%.4f right %.4f/%.4f/%.4f", got0[0], got0[1], got0[2],
           got0[6], got0[7], got0[8], got0[9], got0[10], got0[11]);
    t.note("scene, code 7: ahead %.4f/%.4f/%.4f left+right %.4f/%.4f/%.4f right-left %.4f/%.4f/%.4f", got7[0],
           got7[1], got7[2], got7[6], got7[7], got7[8], got7[9], got7[10], got7[11]);

    // (2) Faded contrast fades to exactly 0 at R: one food item 100 ahead.
    for (int code : {0, kEyeCodeFaded, kEyeCodeSummed}) {
        OceanConfig oc;
        oc.eye = 1;
        oc.eyeCode = code;
        OceanWorld w;
        initQuiet(w, 67, 240.0f, oc, /*capture*/ true);
        const int v = addFish(w, P, z, {}, 0.5f);
        w.creatures[v].vel = {1.0f, 0.0f};
        OceanFood f;
        f.pos = {P.x + R, P.y};
        f.depth = z;
        f.eyeCon = ocean::eyeContrast(ocean::kEyeFoodBright, z);
        w.food.push_back(f);
        w.update(0.016f);
        const float* S = w.creatures[v].lastSensors;
        const float size = (code & kEyeCodeSummed) ? ocean::kEyeFoodSize / (ocean::kEyeFoodSize + R) /
                                                         (1.0f + ocean::kEyeFoodSize / (ocean::kEyeFoodSize + R))
                                                   : ocean::kEyeFoodSize / (ocean::kEyeFoodSize + R);
        t.expect(S[EYE_AHEAD_PROX] == 0.0f && S[EYE_AHEAD_SIZE] == size &&
                     S[EYE_AHEAD_CON] == (code ? 0.0f : f.eyeCon),
                 "eyeCode %d, food at R: %.9g/%.9g/%.9g", code, S[EYE_AHEAD_PROX], S[EYE_AHEAD_SIZE], S[EYE_AHEAD_CON]);
    }

    // (3) Populated worlds (fish wired at random, so behaviour differs by code):
    // every input finite and in its range, fish-step by fish-step.
    {
        long fishSteps = 0, occupied = 0, slots = 0;
        for (int code : {kEyeCodeSummed, kEyeCodeAll, kEyeCodeFaded | kEyeCodeOpponent}) {
            OceanWorld w;
            std::string err;
            char cfg[200];
            std::snprintf(cfg, sizeof cfg,
                          R"({"eye":1,"eyeCode":%d,"maxNeighbors":7,"toroidal":true,"predatorCount":6,"predatorEconomy":true,"foodPatches":3})",
                          code);
            if (!t.expect(ofp::initWorld(w, 68 + code, 1920, 1080, 240.0f, 500, cfg, err), "init: %s", err.c_str()))
                continue;
            w.cfg.captureActivations = true;
            const bool opp = code & kEyeCodeOpponent, sum = code & kEyeCodeSummed;
            for (int s = 0; s < 200; ++s) {
                w.update(0.016f);
                for (const Fish& c : w.creatures) {
                    if (c.age <= 0.0f) continue;
                    ++fishSteps;
                    const float* E = c.lastSensors + ocean::kEyeFirst;
                    for (int k = 0; k < ocean::kEyeSectors; ++k) {
                        ++slots;
                        const float p = E[3 * k], sz = E[3 * k + 1], cn = E[3 * k + 2];
                        occupied += (p != 0.0f || sz != 0.0f || cn != 0.0f);
                        bool ok = finite(p) && finite(sz) && finite(cn);
                        const bool sumSlot = opp && (k == 2 || k == 4), diffSlot = opp && (k == 3 || k == 5);
                        if (sumSlot) ok &= p >= 0.0f && p <= 2.0f && sz >= 0.0f && sz <= 2.0f && std::abs(cn) <= 2.0f;
                        else if (diffSlot) ok &= std::abs(p) <= 1.0f && std::abs(sz) <= 1.0f && std::abs(cn) <= 2.0f;
                        else ok &= p >= 0.0f && p <= 1.0f && sz >= 0.0f && sz <= 1.0f && std::abs(cn) <= 1.0f;
                        if (sum) ok &= (sumSlot ? p < 2.0f && sz < 2.0f : std::abs(p) < 1.0f && std::abs(sz) < 1.0f) &&
                                       (opp ? std::abs(cn) < 2.0f : std::abs(cn) < 1.0f);
                        t.expect(ok, "eyeCode %d sector %d out of range: %.9g %.9g %.9g", code, k, p, sz, cn);
                    }
                }
            }
        }
        t.expect(fishSteps > 100000 && occupied > slots / 10, "too little to test: %ld fish-steps, %ld of %ld sectors",
                 fishSteps, occupied, slots);
        t.note("populated, eyeCode 4 / 7 / 3: %ld fish-steps, %.1f%% of sector slots non-zero, all in range", fishSteps,
               slots ? 100.0 * occupied / slots : 0.0);
    }

    // (4) Validity: 0..7 in the eye build; live through oc_set_param.
    {
        OceanConfig oc;
        bool ok = oceanConfigValid(oc) && oc.eyeCode == 0;
        for (int c = 0; c <= kEyeCodeAll; ++c) { oc.eyeCode = c; ok &= oceanConfigValid(oc); }
        t.expect(ok, "an eyeCode in 0..7 refused (or the default is not 0)");
        for (int c : {-1, 8, 16, 1 << 30}) { oc.eyeCode = c; t.expect(!oceanConfigValid(oc), "eyeCode %d accepted", c); }
#ifndef OCEAN_TESTS_NO_SHIM
        t.expect(oc_init(69, 1920, 1080, 240.0, 50, R"({"eye":1,"eyeCode":7})") == 1, "oc_init eyeCode 7: %s",
                 oc_last_error());
        t.expect(g_world && g_world->ocfg.eyeCode == 7, "eyeCode not stored");
        t.expect(oc_set_param("eyeCode", 5.0) == 1 && g_world->ocfg.eyeCode == 5, "oc_set_param eyeCode 5: %s",
                 oc_last_error());
        t.expect(oc_set_param("eyeCode", 8.0) == 0 && g_world->ocfg.eyeCode == 5, "oc_set_param eyeCode 8 accepted");
        t.note("oc_set_param(eyeCode, 8): %s", oc_last_error());
        t.expect(oc_init(69, 1920, 1080, 240.0, 50, R"({"eye":1,"eyeCode":9})") == 0, "oc_init eyeCode 9 accepted");
#endif
    }
}
#else
// The plain Ocean build has no eye: the key is accepted only as 0.
void testEye() {
    Test t("eye(absent)");
    OceanConfig oc;
    t.expect(oceanConfigValid(oc), "default config refused");
    oc.eye = 1;
    t.expect(!oceanConfigValid(oc), "eye = 1 accepted by a build without the eye");
    oc.eye = 0;
    oc.eyeOnly = 1;
    t.expect(!oceanConfigValid(oc), "eyeOnly = 1 accepted by a build without the eye");
    oc.eyeOnly = 0;
    for (int c = 1; c <= kEyeCodeAll; ++c) { oc.eyeCode = c; t.expect(!oceanConfigValid(oc), "eyeCode = %d accepted by a build without the eye", c); }
#ifndef OCEAN_TESTS_NO_SHIM
    t.expect(oc_init(69, 1920, 1080, 240.0, 50, R"({"eyeCode":1})") == 0, "oc_init eyeCode 1 accepted without the eye");
    t.expect(oc_init(69, 1920, 1080, 240.0, 50, nullptr) == 1 && oc_set_param("eyeCode", 4.0) == 0,
             "oc_set_param eyeCode 4 accepted without the eye");
#endif
    t.expect(NUM_SENSES == 46, "NUM_SENSES %d, expected 46", (int)NUM_SENSES);
}
void testEyeOnly() {}
void testEyeCode() {}
#endif

// OceanConfig::foodLife: uneaten food is removed foodLife seconds after it appeared.
void testFoodLife() {
    Test t("food_life");
    {
        OceanConfig oc;
        t.expect(oceanConfigValid(oc) && oc.foodLife == 0.0f, "default foodLife %g (or refused)", oc.foodLife);
        oc.foodLife = 7.5f;
        t.expect(oceanConfigValid(oc), "foodLife 7.5 refused");
        oc.foodLife = -1.0f;
        t.expect(!oceanConfigValid(oc), "foodLife -1 accepted");
        oc.foodLife = std::numeric_limits<float>::infinity();
        t.expect(!oceanConfigValid(oc), "foodLife inf accepted");
    }
    // Hand-placed items in a quiet world (no fish, no spawn): with foodLife 1 each one
    // goes on the first step its age reaches 1 — an item placed at age a lasts
    // ceil((1 - a) / dt) steps, within one step of float rounding; off, nothing goes.
    const float dt = 0.016f;
    for (int on = 0; on < 2; ++on) {
        OceanConfig oc;
        oc.foodLife = on ? 1.0f : 0.0f;
        OceanWorld w;
        initQuiet(w, 26, 240.0f, oc);
        const float ages[] = {0.0f, 0.25f, 0.5f, 0.9f, 0.99f};
        for (float a : ages) {
            OceanFood f;
            f.pos = {300.0f + 1000.0f * a, 500.0f};
            f.depth = 120.0f;
            f.age = a;
            w.food.push_back(f);
        }
        int goneAt[5] = {-1, -1, -1, -1, -1};
        for (int s = 1; s <= 100; ++s) {
            std::vector<float> xs;
            for (const OceanFood& f : w.food) xs.push_back(f.pos.x);
            w.update(dt);
            for (int k = 0; k < 5; ++k) {
                const float x = 300.0f + 1000.0f * ages[k];
                const bool here = std::any_of(w.food.begin(), w.food.end(), [&](const OceanFood& f) { return f.pos.x == x; });
                if (!here && goneAt[k] < 0) goneAt[k] = s;
            }
        }
        for (int k = 0; k < 5; ++k) {
            if (!on) { t.expect(goneAt[k] < 0, "foodLife 0: item %d vanished at step %d", k, goneAt[k]); continue; }
            const int expect = (int)std::ceil((1.0f - ages[k]) / dt);
            t.expect(goneAt[k] >= expect - 1 && goneAt[k] <= expect + 1, "foodLife 1: item aged %.2f went at step %d, expected ~%d",
                     ages[k], goneAt[k], expect);
        }
        if (on) t.expect(w.totalFoodExpired == 5, "totalFoodExpired %lld, expected 5", w.totalFoodExpired);
        else    for (const OceanFood& f : w.food) t.expect(f.age == ages[&f - w.food.data()], "foodLife 0: an age changed");
    }
    // The first stock's ages are spread over [0, foodLife); spawned items start at 0.
    {
        OceanWorld w;
        std::string err;
        t.expect(ofp::initWorld(w, 27, 1920, 1080, 240.0f, 0,
                                R"({"foodLife":8,"targetFood":2000,"foodSpawnBase":3,"foodSpawnAmp":0,"minPopulation":0})", err),
                 "init refused: %s", err.c_str());
        bool spread = w.food.size() == 2000;
        for (std::size_t i = 0; i < w.food.size(); ++i)
            spread &= w.food[i].age == 8.0f * ((float)i / 2000.0f);
        t.expect(spread, "initial ages not spread evenly over [0, 8)");
        w.update(dt);
        t.expect(w.food.back().age == 0.0f, "a newly spawned item's age is %g", w.food.back().age);
        // Steady state with no fish: 3 items a step, each lasting 8 / 0.016 = 500 steps
        // (one more if the float ages fall just short): a stock of 1500..1503, the
        // whole first stock expired.
        for (int s = 0; s < 2000; ++s) w.update(dt);
        t.note("no fish, foodLife 8, 3 items/step: stock %zu after 2001 steps, %lld expired", w.food.size(),
               w.totalFoodExpired);
        t.expect(w.food.size() >= 1500 && w.food.size() <= 1503, "stock %zu, expected 1500..1503", w.food.size());
        t.expect(w.totalFoodExpired >= 2000, "only %lld expired (the first stock alone is 2000)", w.totalFoodExpired);
    }
    // Draws do not depend on foodLife (spawn cap not reached).
    {
        OceanWorld a, b;
        std::string err;
        ofp::initWorld(a, 28, 1920, 1080, 240.0f, 0, R"({"targetFood":10000,"minPopulation":0})", err);
        ofp::initWorld(b, 28, 1920, 1080, 240.0f, 0, R"({"targetFood":10000,"minPopulation":0,"foodLife":2})", err);
        // initWorld seeds the global rng; run a then b from the same post-init state.
        RandomGen r0;
        r0.seed(99);
        rng = r0;
        for (int s = 0; s < 300; ++s) a.update(dt);
        const RandomGen ra = rng;
        rng = r0;
        for (int s = 0; s < 300; ++s) b.update(dt);
        t.expect(sameEngine(ra, rng), "foodLife changed the draws in 300 steps");
    }
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
#ifdef EVO_OCEAN_EYE
    { const float keep = w.food[12].eyeCon; bump(w.food[12].eyeCon); only("food eyeCon", &ofp::Fingerprint::food); w.food[12].eyeCon = keep; }
    { long nf = ofp::countNonFinite(w); const float keep = w.food[13].eyeCon;
      w.food[13].eyeCon = std::numeric_limits<float>::infinity();
      t.expect(ofp::countNonFinite(w) == nf + 1, "a non-finite food eyeCon is not counted"); w.food[13].eyeCon = keep; }
#endif
    { const float keep = w.predators[1].pitch; bump(w.predators[1].pitch); only("predator pitch", &ofp::Fingerprint::pred); w.predators[1].pitch = keep; }
    { const float keep = w.ocfg.vDrag; bump(w.ocfg.vDrag); only("ocean config", &ofp::Fingerprint::cfg); w.ocfg.vDrag = keep; }
    { const auto keep = w.nextId_; w.nextId_++; only("nextId_", &ofp::Fingerprint::ctr); w.nextId_ = keep; }
    { const RandomGen keep = rng; rng.uint(); only("rng (one draw)", &ofp::Fingerprint::rng); rng = keep; }
    t.expect(ofp::fingerprint(w) == base, "restoring every bump did not restore the hash");
}

// OceanWorld::radiusError (the shim's oc_init check): every radius must be
// finite, >= 0 and have a finite float square — every radius is used squared,
// and with perceptionRadius² = inf an empty eye sector read PROX = −inf.
void testRadii() {
    Test t("radii");
    struct Key { const char* name; float SimConfig::*field; };
    const Key keys[] = {
        {"perceptionRadius", &SimConfig::perceptionRadius}, {"interactRadius", &SimConfig::interactRadius},
        {"pickupRadius", &SimConfig::pickupRadius}, {"predatorPerception", &SimConfig::predatorPerception},
        {"predatorCaptureRadius", &SimConfig::predatorCaptureRadius},
    };
    const float big = std::nextafter(18446744073709551616.0f, 0.0f);   // the largest float below 2^64
    t.expect(std::isfinite(big * big) && !std::isfinite(18446744073709551616.0f * 18446744073709551616.0f),
             "2^64 is not the float square's overflow bound");
    for (const Key& k : keys) {
        SimConfig c;
        applyOceanDefaults(c);
        t.expect(OceanWorld::radiusError(c, 1920, 1080).empty(), "defaults refused");
        for (float bad : {1e20f, 18446744073709551616.0f, 1e38f, std::numeric_limits<float>::infinity(),
                          std::numeric_limits<float>::quiet_NaN(), -1.0f}) {
            c.*k.field = bad;
            const std::string e = OceanWorld::radiusError(c, 1920, 1080);
            t.expect(!e.empty() && e.find(k.name) == 0, "%s = %g accepted (or the error names another key: %s)",
                     k.name, (double)bad, e.c_str());
        }
        c.*k.field = big;   // non-toroidal: no size bound
        t.expect(OceanWorld::radiusError(c, 1920, 1080).empty(), "%s = %g refused", k.name, (double)big);
    }
#ifndef OCEAN_TESTS_NO_SHIM
    // oc_init refuses each (and keeps the previous world); the largest accepted
    // value is accepted.
    if (t.expect(oc_init(91, 1920, 1080, 240.0, 60, nullptr) == 1, "oc_init: %s", oc_last_error())) {
        oc_step(5);
        const ofp::Fingerprint before = ofp::fingerprint(*g_world);
        std::string firstError;
        for (const Key& k : keys) {
            const std::string cfg = std::string("{\"") + k.name + "\":1e20}";
            t.expect(oc_init(92, 1920, 1080, 240.0, 60, cfg.c_str()) == 0 &&
                         std::strstr(oc_last_error(), k.name) != nullptr,
                     "oc_init accepted %s (error: %s)", cfg.c_str(), oc_last_error());
            t.expect(ofp::fingerprint(*g_world) == before, "a refused oc_init (%s) changed the world", cfg.c_str());
            if (firstError.empty()) firstError = oc_last_error();
        }
        t.note("oc_init {\"perceptionRadius\":1e20}: %s", firstError.c_str());
        t.expect(oc_init(93, 1920, 1080, 240.0, 60, R"({"perceptionRadius":1.8e19})") == 1,
                 "perceptionRadius 1.8e19 refused: %s", oc_last_error());
    }
#endif
#ifdef EVO_OCEAN_EYE
    // The largest accepted perceptionRadius, eye on: every fish sees everything,
    // every sensor stays finite and an empty sector still reads 0.
    {
        OceanWorld w;
        std::string err;
        char cfg[160];
        std::snprintf(cfg, sizeof cfg, R"({"perceptionRadius":%.9g,"eye":1,"predatorCount":2,"targetFood":100})",
                      (double)big);
        if (t.expect(ofp::initWorld(w, 94, 1920, 1080, 240.0f, 60, cfg, err), "init: %s", err.c_str()) &&
            t.expect(w.cfg.perceptionRadius == big && OceanWorld::radiusError(w.cfg, 1920, 1080).empty(),
                     "perceptionRadius %.9g stored", (double)w.cfg.perceptionRadius)) {
            w.cfg.captureActivations = true;
            long nonFinite = 0, empty = 0, full = 0;
            for (int s = 0; s < 20; ++s) {
                w.update(0.016f);
                for (const Fish& c : w.creatures) {
                    if (c.age <= 0.0f) continue;
                    for (int k = 0; k < NUM_SENSES; ++k) nonFinite += !finite(c.lastSensors[k]);
                    for (int k = 0; k < ocean::kEyeSectors; ++k) {
                        const float* e = c.lastSensors + ocean::kEyeFirst + 3 * k;
                        if (e[0] == 0.0f && e[1] == 0.0f && e[2] == 0.0f) ++empty; else ++full;
                    }
                }
            }
            t.expect(nonFinite == 0, "%ld non-finite sensors with perceptionRadius %.9g", nonFinite, (double)big);
            t.expect(empty > 0 && full > 0, "sectors: %ld empty, %ld full (expected both)", empty, full);
            t.note("perceptionRadius %.9g, eye on, 20 steps: all sensors finite; %ld empty / %ld full sectors",
                   (double)big, empty, full);
        }
    }
#endif
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
    add("steerFrame 1", [](OceanConfig& o) { o.steerFrame = 1; });
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
    ot::testSteerFrame();
    ot::testSensors();
    ot::testCaptureNeutral();
    ot::testEye();
    ot::testEyeOnly();
    ot::testEyeCode();
    ot::testFoodLife();
    ot::testReproducible();
    ot::testRadii();
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
