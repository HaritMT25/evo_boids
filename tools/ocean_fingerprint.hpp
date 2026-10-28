#pragma once
// ----------------------------------------------------------------------------
// ocean_fingerprint — an exact, platform-independent hash of an OceanWorld's
// whole state, for the native-vs-wasm determinism harness
// (tools/ocean_parity_driver.cpp, wasm/ocean_parity.sh) and tests/ocean_tests.cpp.
//
// Every value is fed to FNV-1a 64 through an explicit little-endian encoding
// (floats by their bit pattern, bools as one byte, sizes as u64), never as raw
// struct memory, so padding and bit-field layout cannot leak in and the same
// state hashes the same on x86-64 and wasm32. Sections are hashed separately so
// a mismatch says WHAT diverged first:
//
//   cfg   SimConfig (the checkpoint writer's canonical text + the keys it does
//         not carry) and every OceanConfig field
//   ctr   frame, world size, D, every WorldCommon counter, nextId_, the
//         pairing-graph window and the death / birth logs, totalReseeds
//   fish  every Fish: all AgentCore fields (ids, energy, timers, internal
//         state, body params, genome gene by gene, the built net's neurons and
//         flat connections, lastSensors / lastActions, colour) + pos, vel,
//         lastMoveDir, depth, vz, bodyBuoy and the 6-point body trail
//   food  every OceanFood: pos, depth, alive (+ age with foodLife on; + eyeCon,
//         what the eye sees, in the eye build)
//   pred  every OceanPredator: Predator fields + depth, pitch
//   sig   the pheromone grid bytes
//   cur   the current field sampled on a fixed 8 x 5 lattice (its vortices
//         are private; the samples pin them and the frame's centres)
//   rng   the global engine's full state text (624 words + position)
//
// Not hashed: the world's private scratch (spatial grids, hot_ / pending_
// arrays — rebuilt from the hashed state every step) and the food-patch
// centres (private, fixed after init; every food spawn depends on them, so a
// difference shows up in `food` on the first spawn).
// ----------------------------------------------------------------------------
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <string>
#include <utility>
#include "ocean/ocean_config.hpp"
#include "ocean/ocean_world.hpp"

namespace ofp {

struct Fnv {
    std::uint64_t h = 1469598103934665603ull;
    void byte(std::uint8_t b) { h ^= b; h *= 1099511628211ull; }
    void u32(std::uint32_t v) { for (int i = 0; i < 4; ++i) byte((std::uint8_t)(v >> (8 * i))); }
    void u64(std::uint64_t v) { for (int i = 0; i < 8; ++i) byte((std::uint8_t)(v >> (8 * i))); }
    void i64(long long v) { u64((std::uint64_t)v); }
    void i32(int v) { u32((std::uint32_t)v); }
    void b(bool v) { byte(v ? 1 : 0); }
    void f32(float f) { std::uint32_t x; std::memcpy(&x, &f, 4); u32(x); }
    void f64(double d) { std::uint64_t x; std::memcpy(&x, &d, 8); u64(x); }
    void v2(Vec2 v) { f32(v.x); f32(v.y); }
    void str(const std::string& s) { u64(s.size()); for (char c : s) byte((std::uint8_t)c); }
};

inline std::uint32_t bits(float f) { std::uint32_t x; std::memcpy(&x, &f, 4); return x; }

struct Fingerprint {
    std::uint64_t all = 0, cfg = 0, ctr = 0, fish = 0, food = 0, pred = 0, sig = 0, cur = 0, rng = 0;
    bool operator==(const Fingerprint& o) const {
        return all == o.all && cfg == o.cfg && ctr == o.ctr && fish == o.fish && food == o.food &&
               pred == o.pred && sig == o.sig && cur == o.cur && rng == o.rng;
    }
    bool operator!=(const Fingerprint& o) const { return !(*this == o); }
};

inline std::string configText(const SimConfig& c) {
    std::string out;
    jsonm::Writer w(out);
    w.beginObject();
    checkpoint::writeConfig(w, c);
    w.endObject();
    return out;
}

inline void hashConfig(Fnv& h, const SimConfig& c, const OceanConfig& o) {
    h.str(configText(c));
    // SimConfig keys the checkpoint does not carry but that change a run's state.
    h.b(c.captureActivations); h.b(c.reproLog); h.i32(c.reproLogMax);
    h.b(c.clusterMap); h.i32(c.clusterMax);
    h.f32(o.depth); h.f32(o.kBuoy); h.f32(o.buoyMutation); h.f32(o.pitchMax); h.f32(o.vDrag);
    h.f32(o.wallBand); h.f32(o.cHold); h.f32(o.cPress); h.i32(o.foodDepthMode);
    h.f32(o.foodDepthSpread);
    // Hashed only when on, so with steerFrame 0 the bytes are the ones before it existed.
    if (o.steerFrame != 0) h.i32(0x53460000 | o.steerFrame);
    if (o.foodLife != 0.0f) { h.i32(0x464c0000); h.f32(o.foodLife); }   // likewise
#ifdef EVO_OCEAN_EYE
    // The eye build only: the plain build's eye is always 0 and its fingerprints
    // stay those of the build before the eye existed. eyeOnly shares the word
    // (bit 1), so with eyeOnly 0 the hashed bytes are the ones before it existed.
    h.i32(o.eye | (o.eyeOnly << 1));
#endif
}

inline void hashCounters(Fnv& h, const OceanWorld& w) {
    h.u64(w.frame); h.i32(w.width); h.i32(w.height); h.f32(w.D);
    h.i32(w.births); h.i32(w.deaths); h.i64(w.totalBirths); h.i64(w.totalDeaths);
    h.i64(w.totalGraceBirths); h.i64(w.totalSoftBirths);
    h.i64(w.totalDeathsPredation); h.i64(w.totalDeathsStarvation); h.i64(w.totalDeathsAge);
    h.i64(w.totalPredatorBirths); h.i64(w.totalPredatorDeathsStarvation);
    h.i64(w.totalPredatorDeathsAge); h.i64(w.totalPredatorReseeds);
    h.i64(w.totalReseeds); h.u64(w.nextId_);
    h.i64(w.totalDeathsLogged_); h.i64(w.clusterStride_);
    h.i64(w.totalBirthsLogged_); h.i64(w.reproStride_);
    h.u64(w.reproWindow_.size()); h.u64(w.reproWindowNext_);
    for (const auto& e : w.reproWindow_) { h.u64(e.a); h.u64(e.b); }
    h.u64(w.deathLog.size());
    for (const auto& d : w.deathLog) {
        h.f32(d.x); h.f32(d.y);
        for (float s : d.sketch) h.f32(s);
        h.i64(d.index); h.u64(d.lineageId); h.f32(d.age); h.i32(d.genomeLen); h.byte((std::uint8_t)d.cause);
    }
    h.u64(w.birthLog.size());
    for (const auto& r : w.birthLog) {
        h.i64(r.index); h.u64(r.parent1); h.u64(r.parent2); h.u64(r.lineage1); h.u64(r.lineage2);
        h.u64(r.frame);
    }
}

// withActivations = false leaves lastSensors / lastActions out (the dynamics
// alone), for checking that captureActivations does not change a run.
inline void hashFish(Fnv& h, const Fish& c, bool withActivations = true) {
    h.b(c.alive); h.u64(c.id); h.u64(c.lineageId); h.u32(c.generation);
    h.f32(c.speed); h.f32(c.age); h.f32(c.foodReserves); h.f32(c.reproTimer); h.f32(c.interactTimer);
    h.f32(c.responsiveness); h.f32(c.oscPeriod); h.f32(c.oscPhase);
    h.f32(c.bodySize); h.f32(c.bodyElong);
    h.u64(c.genome.size());
    for (const Gene& g : c.genome) {
        h.byte((std::uint8_t)g.sourceType); h.byte((std::uint8_t)g.sourceNum);
        h.byte((std::uint8_t)g.sinkType);   h.byte((std::uint8_t)g.sinkNum);
        h.u32((std::uint32_t)(std::uint16_t)g.weight);
    }
    h.u64(c.nnet.neurons.size());
    for (const auto& n : c.nnet.neurons) { h.f32(n.output); h.b(n.driven); }
    h.u64(c.nnet.flat.size());
    for (const auto& fc : c.nnet.flat) { h.u32(fc.srcSink); h.u32((std::uint32_t)(std::uint16_t)fc.weight); }
    if (withActivations) {
        for (int s = 0; s < NUM_SENSES; ++s) h.f32(c.lastSensors[s]);
        for (int a = 0; a < NUM_ACTIONS; ++a) h.f32(c.lastActions[a]);
    }
    h.byte(c.color.r); h.byte(c.color.g); h.byte(c.color.b); h.byte(c.color.a);
    h.v2(c.pos); h.v2(c.vel); h.v2(c.lastMoveDir);
    h.f32(c.depth); h.f32(c.vz); h.f32(c.bodyBuoy);
    for (int i = 0; i < Fish::BODY_LEN; ++i) h.v2(c.body[i]);
}

inline void hashPredator(Fnv& h, const OceanPredator& p) {
    h.v2(p.pos); h.v2(p.vel); h.f32(p.digestTimer); h.f32(p.energy); h.f32(p.age); h.b(p.alive);
    h.f32(p.speedFactor); h.f32(p.buddingFactor); h.u32(p.generation);
    h.f32(p.depth); h.f32(p.pitch);
}

// The fixed lattice the current is sampled on.
template <class F>
inline void forCurrentLattice(const OceanWorld& w, F&& f) {
    for (int j = 0; j < 5; ++j)
        for (int i = 0; i < 8; ++i)
            f(Vec2{((float)i + 0.5f) * (float)w.width / 8.0f, ((float)j + 0.5f) * (float)w.height / 5.0f});
}

inline Fingerprint fingerprint(const OceanWorld& w, bool withActivations = true) {
    Fingerprint fp;
    { Fnv h; hashConfig(h, w.cfg, w.ocfg); fp.cfg = h.h; }
    { Fnv h; hashCounters(h, w); fp.ctr = h.h; }
    { Fnv h; h.u64(w.creatures.size());
      for (const Fish& c : w.creatures) hashFish(h, c, withActivations);
      fp.fish = h.h; }
    { Fnv h; h.u64(w.food.size());
      const bool ages = w.ocfg.foodLife > 0.0f;   // ages count only then (else always 0)
      for (const OceanFood& f : w.food) {
          h.v2(f.pos); h.f32(f.depth); h.b(f.alive); if (ages) h.f32(f.age);
#ifdef EVO_OCEAN_EYE
          h.f32(f.eyeCon);   // the eye build only (set at every spawn, eye on or off)
#endif
      }
      fp.food = h.h; }
    { Fnv h; h.u64(w.predators.size());
      for (const OceanPredator& p : w.predators) hashPredator(h, p);
      fp.pred = h.h; }
    { Fnv h; const auto& d = w.signals.data(); h.u64(d.size());
      for (std::uint8_t b : d) h.byte(b);
      fp.sig = h.h; }
    { Fnv h; h.b(w.currentActive());
      if (w.currentActive()) forCurrentLattice(w, [&](Vec2 p) { h.v2(w.currentAt(p)); });
      fp.cur = h.h; }
    { Fnv h; h.str(rng.state()); fp.rng = h.h; }
    Fnv a;
    for (std::uint64_t s : {fp.cfg, fp.ctr, fp.fish, fp.food, fp.pred, fp.sig, fp.cur, fp.rng}) a.u64(s);
    fp.all = a.h;
    return fp;
}

// Non-finite floats in the state. The harness requires 0: a NaN's sign and
// payload are left to the host by the wasm spec (x86 makes 0xffc00000, ARM
// 0x7fc00000), so a run that carries a NaN can match native on one machine
// and not on another, and a NaN depth also slips past every 3-D range test.
inline long countNonFinite(const OceanWorld& w) {
    long n = 0;
    auto chk = [&](float v) { n += !std::isfinite(v); };
    for (const Fish& c : w.creatures) {
        for (float v : {c.speed, c.age, c.foodReserves, c.reproTimer, c.interactTimer, c.responsiveness,
                        c.oscPeriod, c.oscPhase, c.bodySize, c.bodyElong, c.pos.x, c.pos.y, c.vel.x, c.vel.y,
                        c.lastMoveDir.x, c.lastMoveDir.y, c.depth, c.vz, c.bodyBuoy})
            chk(v);
        for (const Vec2& b : c.body) { chk(b.x); chk(b.y); }
        for (const auto& nr : c.nnet.neurons) chk(nr.output);
        for (float s : c.lastSensors) chk(s);
        for (float a : c.lastActions) chk(a);
    }
    for (const OceanFood& f : w.food) {
        chk(f.pos.x); chk(f.pos.y); chk(f.depth); chk(f.age);
#ifdef EVO_OCEAN_EYE
        chk(f.eyeCon);
#endif
    }
    for (const OceanPredator& p : w.predators)
        for (float v : {p.pos.x, p.pos.y, p.vel.x, p.vel.y, p.digestTimer, p.energy, p.age, p.speedFactor,
                        p.buddingFactor, p.depth, p.pitch})
            chk(v);
    return n;
}

// One fingerprint line: the step, the combined hash, a few readable counts,
// then every section's hash.
inline std::string fingerprintLine(const char* tag, long long step, const OceanWorld& w,
                                   const Fingerprint& fp) {
    char buf[512];
    std::snprintf(buf, sizeof buf,
                  "%s %lld %016llx pop=%zu food=%zu pred=%zu births=%lld deaths=%lld reseeds=%lld"
                  " | cfg=%016llx ctr=%016llx fish=%016llx food=%016llx pred=%016llx"
                  " sig=%016llx cur=%016llx rng=%016llx\n",
                  tag, step, (unsigned long long)fp.all, w.creatures.size(), w.food.size(),
                  w.predators.size(), w.totalBirths, w.totalDeaths, w.totalReseeds,
                  (unsigned long long)fp.cfg, (unsigned long long)fp.ctr,
                  (unsigned long long)fp.fish, (unsigned long long)fp.food,
                  (unsigned long long)fp.pred, (unsigned long long)fp.sig,
                  (unsigned long long)fp.cur, (unsigned long long)fp.rng);
    return buf;
}

// ---- raw dump: the hashed state as text, one record per line, for diffing ----
// Floats print as "%.9g/%08x" (value, bit pattern): the value to read, the bits
// to compare (%.9g round-trips a float, so equal bits <=> equal text anyway).
struct Dumper {
    std::FILE* f;
    void fl(const char* k, float v) { std::fprintf(f, " %s=%.9g/%08x", k, (double)v, (unsigned)bits(v)); }
    void v2(const char* k, Vec2 v) {
        std::fprintf(f, " %s=(%.9g/%08x,%.9g/%08x)", k, (double)v.x, (unsigned)bits(v.x),
                     (double)v.y, (unsigned)bits(v.y));
    }
    void ll(const char* k, long long v) { std::fprintf(f, " %s=%lld", k, v); }
    void ull(const char* k, unsigned long long v) { std::fprintf(f, " %s=%llu", k, v); }
};

inline void dumpState(const OceanWorld& w, std::FILE* out) {
    Dumper d{out};
    std::fprintf(out, "world");
    d.ull("frame", w.frame); d.ll("width", w.width); d.ll("height", w.height); d.fl("D", w.D);
    d.ull("fish", w.creatures.size()); d.ull("food", w.food.size()); d.ull("pred", w.predators.size());
    std::fprintf(out, "\nconfig %s\nocean", configText(w.cfg).c_str());
    d.fl("kBuoy", w.ocfg.kBuoy); d.fl("buoyMutation", w.ocfg.buoyMutation);
    d.fl("pitchMax", w.ocfg.pitchMax); d.fl("vDrag", w.ocfg.vDrag); d.fl("wallBand", w.ocfg.wallBand);
    d.fl("cHold", w.ocfg.cHold); d.fl("cPress", w.ocfg.cPress); d.ll("foodDepthMode", w.ocfg.foodDepthMode);
    d.fl("foodDepthSpread", w.ocfg.foodDepthSpread);
    d.ll("steerFrame", w.ocfg.steerFrame); d.fl("foodLife", w.ocfg.foodLife);
#ifdef EVO_OCEAN_EYE
    d.ll("eye", w.ocfg.eye);
    d.ll("eyeOnly", w.ocfg.eyeOnly);
#endif
    std::fprintf(out, "\ncounters");
    d.ll("births", w.births); d.ll("deaths", w.deaths); d.ll("totalBirths", w.totalBirths);
    d.ll("totalDeaths", w.totalDeaths); d.ll("grace", w.totalGraceBirths); d.ll("soft", w.totalSoftBirths);
    d.ll("dPred", w.totalDeathsPredation); d.ll("dStarve", w.totalDeathsStarvation); d.ll("dAge", w.totalDeathsAge);
    d.ll("pBirths", w.totalPredatorBirths); d.ll("pStarve", w.totalPredatorDeathsStarvation);
    d.ll("pAge", w.totalPredatorDeathsAge); d.ll("pReseeds", w.totalPredatorReseeds);
    d.ll("reseeds", w.totalReseeds); d.ull("nextId", w.nextId_);
    d.ull("reproWindow", w.reproWindow_.size()); d.ull("reproWindowNext", w.reproWindowNext_);
    d.ull("deathLog", w.deathLog.size()); d.ull("birthLog", w.birthLog.size());
    std::fprintf(out, "\n");
    for (std::size_t i = 0; i < w.creatures.size(); ++i) {
        const Fish& c = w.creatures[i];
        std::fprintf(out, "fish %zu", i);
        d.ll("alive", c.alive); d.ull("id", c.id); d.ull("lineage", c.lineageId); d.ull("gen", c.generation);
        d.v2("pos", c.pos); d.fl("depth", c.depth); d.fl("vz", c.vz); d.fl("bodyBuoy", c.bodyBuoy);
        d.v2("vel", c.vel); d.v2("lastMoveDir", c.lastMoveDir);
        d.fl("speed", c.speed); d.fl("age", c.age); d.fl("food", c.foodReserves);
        d.fl("reproTimer", c.reproTimer); d.fl("interactTimer", c.interactTimer);
        d.fl("resp", c.responsiveness); d.fl("oscPeriod", c.oscPeriod); d.fl("oscPhase", c.oscPhase);
        d.fl("bodySize", c.bodySize); d.fl("bodyElong", c.bodyElong);
        std::fprintf(out, " color=%02x%02x%02x%02x\n  body", c.color.r, c.color.g, c.color.b, c.color.a);
        for (int b = 0; b < Fish::BODY_LEN; ++b) d.v2("", c.body[b]);
        std::fprintf(out, "\n  genome %zu:", c.genome.size());
        for (const Gene& g : c.genome)
            std::fprintf(out, " %u%02x%u%02x:%04x", (unsigned)g.sourceType, (unsigned)g.sourceNum,
                         (unsigned)g.sinkType, (unsigned)g.sinkNum, (unsigned)(std::uint16_t)g.weight);
        std::fprintf(out, "\n  neurons");
        for (const auto& n : c.nnet.neurons) std::fprintf(out, " %08x%s", (unsigned)bits(n.output), n.driven ? "d" : "");
        std::fprintf(out, "\n  flat");
        for (const auto& fc : c.nnet.flat) std::fprintf(out, " %04x:%04x", (unsigned)fc.srcSink, (unsigned)(std::uint16_t)fc.weight);
        std::fprintf(out, "\n  sensors");
        for (int s = 0; s < NUM_SENSES; ++s) std::fprintf(out, " %08x", (unsigned)bits(c.lastSensors[s]));
        std::fprintf(out, "\n  actions");
        for (int a = 0; a < NUM_ACTIONS; ++a) std::fprintf(out, " %08x", (unsigned)bits(c.lastActions[a]));
        std::fprintf(out, "\n");
    }
    for (std::size_t i = 0; i < w.food.size(); ++i) {
        const OceanFood& f = w.food[i];
        std::fprintf(out, "food %zu", i);
        d.v2("pos", f.pos); d.fl("depth", f.depth); d.ll("alive", f.alive); d.fl("age", f.age);
#ifdef EVO_OCEAN_EYE
        d.fl("eyeCon", f.eyeCon);
#endif
        std::fprintf(out, "\n");
    }
    for (std::size_t i = 0; i < w.predators.size(); ++i) {
        const OceanPredator& p = w.predators[i];
        std::fprintf(out, "pred %zu", i);
        d.v2("pos", p.pos); d.v2("vel", p.vel); d.fl("depth", p.depth); d.fl("pitch", p.pitch);
        d.fl("digest", p.digestTimer); d.fl("energy", p.energy); d.fl("age", p.age); d.ll("alive", p.alive);
        d.fl("speedF", p.speedFactor); d.fl("buddingF", p.buddingFactor); d.ull("gen", p.generation);
        std::fprintf(out, "\n");
    }
    {
        const auto& g = w.signals.data();
        std::fprintf(out, "signals %zu", g.size());
        for (std::size_t i = 0; i < g.size(); ++i) {
            if (i % 64 == 0) std::fprintf(out, "\n ");
            std::fprintf(out, "%02x", g[i]);
        }
        std::fprintf(out, "\n");
    }
    std::fprintf(out, "current active=%d", (int)w.currentActive());
    if (w.currentActive()) forCurrentLattice(w, [&](Vec2 p) { d.v2("", w.currentAt(p)); });
    std::fprintf(out, "\nrng %s\n", rng.state().c_str());
}

// ---- the world as the page builds it (wasm/ocean_wasm.cpp, oc_init) --------
// Mirrors oc_init's config path: applyOceanDefaults, then the cfg JSON
// (SimConfig + OceanConfig keys), then clampConfig (a copy of the shim's),
// then rng.seed and init. Only oceanConfigValid is checked here; the shim's
// full refusal list stays the shim's: wasm/ocean_parity.sh runs every case
// through the real oc_init / oc_set_param first (ocean_tests --accepts), and
// tests/ocean_tests.cpp checks that this path and the shim's give the same
// fingerprints.
inline void clampConfigLikeShim(SimConfig& c) {
    // (No min/max swaps: the shim refuses a reversed pair instead.)
    if (c.perceptionRadius < 1.0f) c.perceptionRadius = 1.0f;
    if (c.maxNeighbors < 0) c.maxNeighbors = 0;
    if (c.maxNeighbors > MAX_TOPOLOGICAL_NEIGHBORS) c.maxNeighbors = MAX_TOPOLOGICAL_NEIGHBORS;
    if (c.predatorMinPopulation < 0) c.predatorMinPopulation = 0;
    if (c.predatorMinPopulation > 2000) c.predatorMinPopulation = 2000;
    if (c.predatorMaxPopulation < 0) c.predatorMaxPopulation = 0;
}

// Returns false with `err` set on a malformed JSON or an invalid Ocean config.
inline bool buildConfigs(const char* cfgJson, float depth, SimConfig& sc, OceanConfig& oc,
                         std::string& err) {
    sc = SimConfig{};
    applyOceanDefaults(sc);
    oc = OceanConfig{};
    if (!readOceanJson(cfgJson, sc, oc, err)) return false;
    clampConfigLikeShim(sc);
    oc.depth = depth;
    if (!oceanConfigValid(oc)) { err = "invalid ocean config"; return false; }
    return true;
}

inline bool initWorld(OceanWorld& w, unsigned seed, int width, int height, float depth, int pop,
                      const char* cfgJson, std::string& err) {
    SimConfig sc;
    OceanConfig oc;
    if (!buildConfigs(cfgJson, depth, sc, oc, err)) return false;
    rng.seed(seed);
    w.cfg = sc;
    w.ocfg = oc;
    w.init(width, height, depth, pop);
    return true;
}

}  // namespace ofp
