#pragma once
// ----------------------------------------------------------------------------
// checkpoint_io — save a running World to a JSON file and resume from it.
//
// WHY JSON. The requirement that decides it is that a browser has to be able to
// read these (the planned wasm build), and JSON.parse is built in — HDF5 and
// protobuf would each need a library on both sides, and YAML needs one on the C++
// side with no gain. Written with %.9g floats it is exactly as lossless as a
// binary dump (see json_min.hpp), it diffs in git, and the config block stays
// hand-editable, which is the point of "resume with different settings".
// The cost is size: roughly 1 KB per creature. A 500-creature checkpoint is
// ~600 KB, which gzips to a fraction of that if it ever matters.
//
// WHAT A RESUME GUARANTEES. Loading a checkpoint and stepping forward reproduces
// the original run BIT FOR BIT, in the serial build (-DEVO_NO_OPENMP=ON). That
// is a stronger promise than "close enough" and it is enforced by a test:
// core_tests runs N steps, checkpoints, runs M more, then reloads and runs M and
// compares whole-world fingerprints. It holds only for the serial build because
// the OpenMP build has never been run-to-run reproducible (per-thread rngs), and
// only within one engine build, since a different -march or compiler can change
// float rounding — which is exactly why -ffp-contract=off is now a build flag.
//
// WHAT IS DELIBERATELY NOT SAVED:
//   - the spatial grids: rebuilt from scratch at the top of every update().
//   - CurrentField::centers_: recomputed by advance(frame) from the vortices.
//   - Creature::nnet: rebuilt from the genome by buildNeuralNet(), EXCEPT the
//     recurrent neuron outputs, which are live state and ARE saved.
//   - Creature::color / body[]: derived from the genome and pos/vel.
//   - the death-cluster log (cfg.clusterMap): an analysis artifact that can run
//     to hundreds of thousands of entries. A run resumed from a checkpoint
//     therefore produces a cluster map covering only the post-resume deaths.
//     The counters that drive its decimation ARE saved, so it stays consistent.
//   - the pairing graph (repro_graph.hpp): same rule as the death log. Neither
//     the births archive (cfg.reproLog) nor the always-maintained trailing
//     window behind the repro_components CSV column is world state; a resumed
//     run restarts the window empty, so that column needs up to
//     cfg.reproWindow post-resume pairing events before it is comparable to an
//     uninterrupted run's. The archive's decimation counters ARE saved, like
//     the death log's, so a resumed --repro-log run keeps consistent ordinals.
//   - the phase profiler: diagnostics, reset per run by design.
// ----------------------------------------------------------------------------
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <cstdint>
#include "world.hpp"
#include "json_min.hpp"

namespace checkpoint {

inline constexpr int FORMAT_VERSION = 1;
inline constexpr const char* FORMAT_ID = "evo_boids.checkpoint";

// Genes are bitfields, whose in-memory layout is implementation-defined, so
// they are packed with explicit arithmetic instead of being memcpy'd. Two ints
// per gene: the wiring word, then the weight.
inline long long packGene(const Gene& g) {
    return ((long long)g.sourceType << 15) | ((long long)g.sourceNum << 8)
         | ((long long)g.sinkType   << 7)  |  (long long)g.sinkNum;
}
inline Gene unpackGene(long long wiring, long long weight) {
    Gene g;
    g.sourceType = (uint16_t)((wiring >> 15) & 0x1);
    g.sourceNum  = (uint16_t)((wiring >> 8)  & 0x7F);
    g.sinkType   = (uint16_t)((wiring >> 7)  & 0x1);
    g.sinkNum    = (uint16_t)( wiring        & 0x7F);
    g.weight     = (int16_t)weight;
    return g;
}

// The pheromone layer is mostly zero, so run-length encoding it keeps a
// 240x135 grid to a few dozen numbers instead of 32,400.
inline std::vector<long long> rleEncode(const std::vector<uint8_t>& in) {
    std::vector<long long> out;
    size_t i = 0;
    while (i < in.size()) {
        const uint8_t v = in[i];
        size_t j = i;
        while (j < in.size() && in[j] == v) ++j;
        out.push_back((long long)v);
        out.push_back((long long)(j - i));
        i = j;
    }
    return out;
}
inline bool rleDecode(const std::vector<long long>& in, std::vector<uint8_t>& out, size_t expected) {
    out.clear();
    out.reserve(expected);
    if (in.size() % 2 != 0) return false;
    for (size_t i = 0; i < in.size(); i += 2) {
        const long long v = in[i], n = in[i + 1];
        if (v < 0 || v > 255 || n < 0) return false;
        if (out.size() + (size_t)n > expected) return false;
        out.insert(out.end(), (size_t)n, (uint8_t)v);
    }
    return out.size() == expected;
}

// ------------------------------------------------------------------ config --
inline void writeConfig(jsonm::Writer& w, const SimConfig& c) {
    w.key("config");
    w.beginObject();
    w.field("toroidal", c.toroidal);
    w.field("reproMaxDistance", c.reproMaxDistance);
    w.field("reproWindow", c.reproWindow);
    w.field("reproMinCluster", c.reproMinCluster);
    w.field("minPopulation", c.minPopulation);
    w.field("maxPopulation", c.maxPopulation);
    w.field("targetFood", c.targetFood);
    w.field("foodSpawnBase", c.foodSpawnBase);
    w.field("foodSpawnAmp", c.foodSpawnAmp);
    w.field("foodEnergy", c.foodEnergy);
    w.field("pickupRadius", c.pickupRadius);
    w.field("metabolicRate", c.metabolicRate);
    w.field("geneCost", c.geneCost);
    w.field("moveCost", c.moveCost);
    w.field("maxAge", c.maxAge);
    w.field("reproEnergy", c.reproEnergy);
    w.field("newbornEnergy", c.newbornEnergy);
    w.field("perceptionRadius", c.perceptionRadius);
    w.field("interactRadius", c.interactRadius);
    w.field("maxNeighbors", c.maxNeighbors);
    w.field("reproCooldown", c.reproCooldown);
    w.field("interactCooldown", c.interactCooldown);
    w.field("minSpeed", c.minSpeed);
    w.field("maxSpeed", c.maxSpeed);
    w.field("triangularMotion", c.triangularMotion);
    w.field("maxTurnRate", c.maxTurnRate);
    w.field("genomeMinLength", c.genomeMinLength);
    w.field("genomeMaxLength", c.genomeMaxLength);
    w.field("genomeInitMin", c.genomeInitMin);
    w.field("genomeInitMax", c.genomeInitMax);
    w.field("mutationMultiplier", c.mutationMultiplier);
    w.field("currentStrength", c.currentStrength);
    w.field("currentMode", c.currentMode);
    w.field("currentEddies", c.currentEddies);
    w.field("currentCore", c.currentCore);
    w.field("currentDrift", c.currentDrift);
    w.field("currentGyreEps", c.currentGyreEps);
    w.field("foodPatches", c.foodPatches);
    w.field("foodPatchRadius", c.foodPatchRadius);
    w.field("foodDrift", c.foodDrift);
    w.field("predatorCount", c.predatorCount);
    w.field("predatorPerception", c.predatorPerception);
    w.field("predatorSpeed", c.predatorSpeed);
    w.field("predatorCaptureRadius", c.predatorCaptureRadius);
    w.field("predatorCooldown", c.predatorCooldown);
    w.field("predatorEconomy", c.predatorEconomy);
    w.field("predatorMetabolism", c.predatorMetabolism);
    w.field("predatorMoveCost", c.predatorMoveCost);
    w.field("predatorReproEnergy", c.predatorReproEnergy);
    w.field("predatorNewbornEnergy", c.predatorNewbornEnergy);
    w.field("predatorMeal", c.predatorMeal);
    w.field("predatorMaxAge", c.predatorMaxAge);
    w.field("fastTanh", c.fastTanh);
    w.endObject();
}

// Every field defaults to the value already in `c`, so a hand-edited checkpoint
// may omit anything it doesn't want to change — that is what makes "resume with
// different settings" a matter of editing one line.
inline void readConfig(const jsonm::Value& j, SimConfig& c) {
    const jsonm::Value* o = j.find("config");
    if (!o || !o->isObject()) return;
    c.toroidal = o->boolOr("toroidal", c.toroidal);
    // Absent in checkpoints written before v0.21.0; the default (-1 = gate off)
    // is what those runs actually used, so they resume unchanged.
    c.reproMaxDistance = o->floatOr("reproMaxDistance", c.reproMaxDistance);
    // reproLog / reproLogMax are deliberately NOT serialized, matching deathLog:
    // they select a per-run analysis output, not world state.
    c.reproWindow = o->intOr("reproWindow", c.reproWindow);
    c.reproMinCluster = o->intOr("reproMinCluster", c.reproMinCluster);
    c.minPopulation = o->intOr("minPopulation", c.minPopulation);
    c.maxPopulation = o->intOr("maxPopulation", c.maxPopulation);
    c.targetFood = o->intOr("targetFood", c.targetFood);
    c.foodSpawnBase = o->floatOr("foodSpawnBase", c.foodSpawnBase);
    c.foodSpawnAmp = o->floatOr("foodSpawnAmp", c.foodSpawnAmp);
    c.foodEnergy = o->floatOr("foodEnergy", c.foodEnergy);
    c.pickupRadius = o->floatOr("pickupRadius", c.pickupRadius);
    c.metabolicRate = o->floatOr("metabolicRate", c.metabolicRate);
    c.geneCost = o->floatOr("geneCost", c.geneCost);
    c.moveCost = o->floatOr("moveCost", c.moveCost);
    c.maxAge = o->floatOr("maxAge", c.maxAge);
    c.reproEnergy = o->floatOr("reproEnergy", c.reproEnergy);
    c.newbornEnergy = o->floatOr("newbornEnergy", c.newbornEnergy);
    c.perceptionRadius = o->floatOr("perceptionRadius", c.perceptionRadius);
    c.interactRadius = o->floatOr("interactRadius", c.interactRadius);
    c.maxNeighbors = o->intOr("maxNeighbors", c.maxNeighbors);
    c.reproCooldown = o->floatOr("reproCooldown", c.reproCooldown);
    c.interactCooldown = o->floatOr("interactCooldown", c.interactCooldown);
    c.minSpeed = o->floatOr("minSpeed", c.minSpeed);
    c.maxSpeed = o->floatOr("maxSpeed", c.maxSpeed);
    c.triangularMotion = o->boolOr("triangularMotion", c.triangularMotion);
    c.maxTurnRate = o->floatOr("maxTurnRate", c.maxTurnRate);
    c.genomeMinLength = o->intOr("genomeMinLength", c.genomeMinLength);
    c.genomeMaxLength = o->intOr("genomeMaxLength", c.genomeMaxLength);
    c.genomeInitMin = o->intOr("genomeInitMin", c.genomeInitMin);
    c.genomeInitMax = o->intOr("genomeInitMax", c.genomeInitMax);
    c.mutationMultiplier = o->floatOr("mutationMultiplier", c.mutationMultiplier);
    c.currentStrength = o->floatOr("currentStrength", c.currentStrength);
    c.currentMode = o->intOr("currentMode", c.currentMode);
    c.currentEddies = o->intOr("currentEddies", c.currentEddies);
    c.currentCore = o->floatOr("currentCore", c.currentCore);
    c.currentDrift = o->floatOr("currentDrift", c.currentDrift);
    c.currentGyreEps = o->floatOr("currentGyreEps", c.currentGyreEps);
    c.foodPatches = o->intOr("foodPatches", c.foodPatches);
    c.foodPatchRadius = o->floatOr("foodPatchRadius", c.foodPatchRadius);
    c.foodDrift = o->boolOr("foodDrift", c.foodDrift);
    c.predatorCount = o->intOr("predatorCount", c.predatorCount);
    c.predatorPerception = o->floatOr("predatorPerception", c.predatorPerception);
    c.predatorSpeed = o->floatOr("predatorSpeed", c.predatorSpeed);
    c.predatorCaptureRadius = o->floatOr("predatorCaptureRadius", c.predatorCaptureRadius);
    c.predatorCooldown = o->floatOr("predatorCooldown", c.predatorCooldown);
    c.predatorEconomy = o->boolOr("predatorEconomy", c.predatorEconomy);
    c.predatorMetabolism = o->floatOr("predatorMetabolism", c.predatorMetabolism);
    c.predatorMoveCost = o->floatOr("predatorMoveCost", c.predatorMoveCost);
    c.predatorReproEnergy = o->floatOr("predatorReproEnergy", c.predatorReproEnergy);
    c.predatorNewbornEnergy = o->floatOr("predatorNewbornEnergy", c.predatorNewbornEnergy);
    c.predatorMeal = o->floatOr("predatorMeal", c.predatorMeal);
    c.predatorMaxAge = o->floatOr("predatorMaxAge", c.predatorMaxAge);
    c.fastTanh = o->boolOr("fastTanh", c.fastTanh);
}

// -------------------------------------------------------------------- save --
inline std::string serialize(const World& world, const char* engineVersion) {
    std::string out;
    out.reserve(1024 * 64);
    jsonm::Writer w(out);

    w.beginObject();
    w.field("format", FORMAT_ID);
    w.field("formatVersion", FORMAT_VERSION);
    w.field("engineVersion", engineVersion ? engineVersion : "unknown");
    // Recorded because a resume is only bit-exact within the same numerics.
    // A checkpoint written by an FMA-contracted or OpenMP build can still be
    // loaded; it just cannot promise an identical continuation.
#ifdef USE_OPENMP
    w.field("determinism", "openmp-build (resume is NOT bit-exact)");
#else
    w.field("determinism", "serial build (resume is bit-exact)");
#endif
    w.newline();

    w.key("world");
    w.beginObject();
    w.field("width", world.width);
    w.field("height", world.height);
    w.field("frame", (unsigned long long)world.frame);
    w.field("nextId", (unsigned long long)world.nextId_);
    w.endObject();
    w.newline();

    w.key("counters");
    w.beginObject();
    w.field("totalBirths", world.totalBirths);
    w.field("totalDeaths", world.totalDeaths);
    w.field("totalDeathsPredation", world.totalDeathsPredation);
    w.field("totalDeathsStarvation", world.totalDeathsStarvation);
    w.field("totalDeathsAge", world.totalDeathsAge);
    w.field("totalPredatorBirths", world.totalPredatorBirths);
    w.field("totalPredatorDeathsStarvation", world.totalPredatorDeathsStarvation);
    w.field("totalPredatorDeathsAge", world.totalPredatorDeathsAge);
    w.field("totalDeathsLogged", world.totalDeathsLogged_);
    w.field("clusterStride", world.clusterStride_);
    w.field("totalBirthsLogged", world.totalBirthsLogged_);
    w.field("reproStride", world.reproStride_);
    w.endObject();
    w.newline();

    // The rng engine state: without it a resume continues a different random
    // sequence and diverges immediately, even though everything else matches.
    w.field("rngState", rng.state());
    w.newline();

    writeConfig(w, world.cfg);
    w.newline();

    // --- ocean + food patches: rng-seeded at init, so they must be verbatim ---
    jsonm::writeFloats(w, "currentVortices", world.currentVortexState());
    {
        std::vector<float> patches;
        patches.reserve(world.foodPatches().size() * 2);
        for (const Vec2& p : world.foodPatches()) { patches.push_back(p.x); patches.push_back(p.y); }
        jsonm::writeFloats(w, "foodPatchCenters", patches);
    }
    w.newline();

    // --- pheromone layer (run-length encoded; it is mostly zero) ---
    w.key("signals");
    w.beginObject();
    w.field("cols", world.signals.cols());
    w.field("rows", world.signals.rows());
    w.field("cellSize", world.signals.cellSize());
    jsonm::writeInts(w, "rle", rleEncode(world.signals.data()));
    w.endObject();
    w.newline();

    // --- food ---
    w.key("food");
    w.beginObject();
    w.field("count", (long long)world.food.size());
    w.key("x"); w.beginArray(); for (const Food& f : world.food) w.value(f.pos.x); w.endArray();
    w.key("y"); w.beginArray(); for (const Food& f : world.food) w.value(f.pos.y); w.endArray();
    w.key("alive"); w.beginArray(); for (const Food& f : world.food) w.value((int)(f.alive ? 1 : 0)); w.endArray();
    w.endObject();
    w.newline();

    // --- predators ---
    w.key("predators");
    w.beginObject();
    w.field("count", (long long)world.predators.size());
    w.key("posX"); w.beginArray(); for (const Predator& p : world.predators) w.value(p.pos.x); w.endArray();
    w.key("posY"); w.beginArray(); for (const Predator& p : world.predators) w.value(p.pos.y); w.endArray();
    w.key("velX"); w.beginArray(); for (const Predator& p : world.predators) w.value(p.vel.x); w.endArray();
    w.key("velY"); w.beginArray(); for (const Predator& p : world.predators) w.value(p.vel.y); w.endArray();
    w.key("digestTimer"); w.beginArray(); for (const Predator& p : world.predators) w.value(p.digestTimer); w.endArray();
    w.key("energy"); w.beginArray(); for (const Predator& p : world.predators) w.value(p.energy); w.endArray();
    w.key("age"); w.beginArray(); for (const Predator& p : world.predators) w.value(p.age); w.endArray();
    w.key("alive"); w.beginArray(); for (const Predator& p : world.predators) w.value((int)(p.alive ? 1 : 0)); w.endArray();
    w.endObject();
    w.newline();

    // --- creatures (columnar: far smaller than one object per creature) ---
    w.key("creatures");
    w.beginObject();
    w.field("count", (long long)world.creatures.size());
    w.field("neuronsPerCreature", MAX_INTERNAL_NEURONS);
    w.field("bodyLen", Creature::BODY_LEN);

    const auto& cs = world.creatures;
    w.key("id"); w.beginArray(); for (const auto& c : cs) w.value((unsigned long long)c.id); w.endArray();
    w.key("lineageId"); w.beginArray(); for (const auto& c : cs) w.value((unsigned long long)c.lineageId); w.endArray();
    w.key("alive"); w.beginArray(); for (const auto& c : cs) w.value((int)(c.alive ? 1 : 0)); w.endArray();
    w.newline();
    w.key("posX"); w.beginArray(); for (const auto& c : cs) w.value(c.pos.x); w.endArray();
    w.key("posY"); w.beginArray(); for (const auto& c : cs) w.value(c.pos.y); w.endArray();
    w.key("velX"); w.beginArray(); for (const auto& c : cs) w.value(c.vel.x); w.endArray();
    w.key("velY"); w.beginArray(); for (const auto& c : cs) w.value(c.vel.y); w.endArray();
    w.key("lastMoveDirX"); w.beginArray(); for (const auto& c : cs) w.value(c.lastMoveDir.x); w.endArray();
    w.key("lastMoveDirY"); w.beginArray(); for (const auto& c : cs) w.value(c.lastMoveDir.y); w.endArray();
    w.newline();
    w.key("speed"); w.beginArray(); for (const auto& c : cs) w.value(c.speed); w.endArray();
    w.key("age"); w.beginArray(); for (const auto& c : cs) w.value(c.age); w.endArray();
    w.key("foodReserves"); w.beginArray(); for (const auto& c : cs) w.value(c.foodReserves); w.endArray();
    w.key("reproTimer"); w.beginArray(); for (const auto& c : cs) w.value(c.reproTimer); w.endArray();
    w.key("interactTimer"); w.beginArray(); for (const auto& c : cs) w.value(c.interactTimer); w.endArray();
    w.key("responsiveness"); w.beginArray(); for (const auto& c : cs) w.value(c.responsiveness); w.endArray();
    w.key("oscPeriod"); w.beginArray(); for (const auto& c : cs) w.value(c.oscPeriod); w.endArray();
    w.key("oscPhase"); w.beginArray(); for (const auto& c : cs) w.value(c.oscPhase); w.endArray();
    w.newline();

    // Recurrent state. The net itself is rebuilt from the genome, but these
    // carry across steps, so a resume without them starts every brain from the
    // 0.5 default and diverges.
    w.key("neuronOutputs");
    w.beginArray();
    for (const auto& c : cs)
        for (int n = 0; n < MAX_INTERNAL_NEURONS; ++n)
            w.value(c.nnet.neurons[n].output);
    w.endArray();
    w.newline();

    // Visual trail. Not read by any sensor, but saving it keeps a resumed run
    // identical in every observable, including what the window draws.
    w.key("body");
    w.beginArray();
    for (const auto& c : cs)
        for (int b = 0; b < Creature::BODY_LEN; ++b) { w.value(c.body[b].x); w.value(c.body[b].y); }
    w.endArray();
    w.newline();

    // Genomes: variable length, so lengths first, then one flat stream of
    // (wiring, weight) pairs.
    w.key("genomeLengths");
    w.beginArray();
    for (const auto& c : cs) w.value((long long)c.genome.size());
    w.endArray();
    w.key("genes");
    w.beginArray();
    for (const auto& c : cs)
        for (const Gene& g : c.genome) { w.value(packGene(g)); w.value((long long)g.weight); }
    w.endArray();

    w.endObject();   // creatures
    w.endObject();   // root
    out += '\n';
    return out;
}

inline bool save(const World& world, const std::string& path,
                 const char* engineVersion, std::string& error) {
    std::ofstream f(path, std::ios::binary);
    if (!f) { error = "cannot open '" + path + "' for writing"; return false; }
    f << serialize(world, engineVersion);
    if (!f) { error = "write failed for '" + path + "'"; return false; }
    return true;
}

// -------------------------------------------------------------------- load --
// Restores `world` in place. On failure returns false with `error` set and
// leaves the world untouched-but-unusable, so callers should treat a failed
// load as fatal rather than continuing with a half-built world.
inline bool deserialize(const std::string& text, World& world, std::string& error) {
    jsonm::Value j;
    if (!jsonm::Parser::parse(text, j, error)) return false;
    if (!j.isObject()) { error = "checkpoint root is not an object"; return false; }
    if (j.stringOr("format", "") != FORMAT_ID) { error = "not an evo_boids checkpoint"; return false; }
    const int ver = j.intOr("formatVersion", -1);
    if (ver != FORMAT_VERSION) {
        error = "checkpoint format version " + std::to_string(ver) +
                ", this build reads version " + std::to_string(FORMAT_VERSION);
        return false;
    }

    const jsonm::Value* jw = j.find("world");
    if (!jw || !jw->isObject()) { error = "missing 'world'"; return false; }
    const int width  = jw->intOr("width", 0);
    const int height = jw->intOr("height", 0);
    if (width <= 0 || height <= 0) { error = "bad world dimensions"; return false; }

    // Config first: init() sizes the spatial grids from cfg.perceptionRadius.
    readConfig(j, world.cfg);

    // Build an empty world of the right geometry, then overwrite every piece of
    // state. init() draws from the rng (vortices, food patches), which is
    // exactly why the rng state is restored LAST, below.
    world.init(width, height, 0);
    world.food.clear();
    world.predators.clear();
    world.creatures.clear();
    // Pairing-graph state is per-run (see the header list): a resume starts the
    // window and archive empty, with the counters restored below if present.
    world.birthLog.clear();
    world.reproWindow_.clear();
    world.reproWindowNext_ = 0;
    world.totalBirthsLogged_ = 0;
    world.reproStride_ = 1;

    world.frame  = jw->ullOr("frame", 0);
    world.nextId_ = jw->ullOr("nextId", 1);

    if (const jsonm::Value* jc = j.find("counters")) {
        world.totalBirths = jc->llOr("totalBirths", 0);
        world.totalDeaths = jc->llOr("totalDeaths", 0);
        world.totalDeathsPredation = jc->llOr("totalDeathsPredation", 0);
        world.totalDeathsStarvation = jc->llOr("totalDeathsStarvation", 0);
        world.totalDeathsAge = jc->llOr("totalDeathsAge", 0);
        world.totalPredatorBirths = jc->llOr("totalPredatorBirths", 0);
        world.totalPredatorDeathsStarvation = jc->llOr("totalPredatorDeathsStarvation", 0);
        world.totalPredatorDeathsAge = jc->llOr("totalPredatorDeathsAge", 0);
        world.totalDeathsLogged_ = jc->llOr("totalDeathsLogged", 0);
        world.clusterStride_ = jc->llOr("clusterStride", 1);
        world.totalBirthsLogged_ = jc->llOr("totalBirthsLogged", 0);
        world.reproStride_ = jc->llOr("reproStride", 1);
    }

    // ocean + food patches
    {
        std::vector<float> v;
        if (jsonm::readFloats(j, "currentVortices", v, (size_t)-1)) {
            if (!world.restoreCurrentVortexState(v)) { error = "bad currentVortices length"; return false; }
        }
        std::vector<float> p;
        if (jsonm::readFloats(j, "foodPatchCenters", p, (size_t)-1)) {
            if (p.size() % 2 != 0) { error = "bad foodPatchCenters length"; return false; }
            std::vector<Vec2> patches;
            patches.reserve(p.size() / 2);
            for (size_t i = 0; i < p.size(); i += 2) patches.push_back(Vec2{p[i], p[i + 1]});
            world.restoreFoodPatches(std::move(patches));
        }
    }

    // pheromone layer
    if (const jsonm::Value* js = j.find("signals")) {
        std::vector<long long> rle;
        if (jsonm::readInts(*js, "rle", rle, (size_t)-1)) {
            std::vector<uint8_t> grid;
            if (!rleDecode(rle, grid, world.signals.data().size()) || !world.signals.restore(grid)) {
                error = "signal grid does not match this world's geometry";
                return false;
            }
        }
    }

    // food
    if (const jsonm::Value* jf = j.find("food")) {
        const size_t n = (size_t)jf->llOr("count", 0);
        std::vector<float> x, y;
        std::vector<long long> alive;
        if (!jsonm::readFloats(*jf, "x", x, n) || !jsonm::readFloats(*jf, "y", y, n) ||
            !jsonm::readInts(*jf, "alive", alive, n)) { error = "malformed 'food' arrays"; return false; }
        world.food.resize(n);
        for (size_t i = 0; i < n; ++i) { world.food[i].pos = Vec2{x[i], y[i]}; world.food[i].alive = alive[i] != 0; }
    }

    // predators
    if (const jsonm::Value* jp = j.find("predators")) {
        const size_t n = (size_t)jp->llOr("count", 0);
        std::vector<float> px, py, vx, vy, dt, en, ag;
        std::vector<long long> alive;
        if (!jsonm::readFloats(*jp, "posX", px, n) || !jsonm::readFloats(*jp, "posY", py, n) ||
            !jsonm::readFloats(*jp, "velX", vx, n) || !jsonm::readFloats(*jp, "velY", vy, n) ||
            !jsonm::readFloats(*jp, "digestTimer", dt, n) || !jsonm::readFloats(*jp, "energy", en, n) ||
            !jsonm::readFloats(*jp, "age", ag, n) || !jsonm::readInts(*jp, "alive", alive, n)) {
            error = "malformed 'predators' arrays"; return false;
        }
        world.predators.resize(n);
        for (size_t i = 0; i < n; ++i) {
            Predator& p = world.predators[i];
            p.pos = Vec2{px[i], py[i]};
            p.vel = Vec2{vx[i], vy[i]};
            p.digestTimer = dt[i];
            p.energy = en[i];
            p.age = ag[i];
            p.alive = alive[i] != 0;
        }
    }

    // creatures
    if (const jsonm::Value* jcs = j.find("creatures")) {
        const size_t n = (size_t)jcs->llOr("count", 0);
        const int nper = jcs->intOr("neuronsPerCreature", MAX_INTERNAL_NEURONS);
        const int blen = jcs->intOr("bodyLen", Creature::BODY_LEN);
        if (nper != MAX_INTERNAL_NEURONS) {
            error = "checkpoint has " + std::to_string(nper) + " neurons/creature, this build has " +
                    std::to_string(MAX_INTERNAL_NEURONS);
            return false;
        }
        if (blen != Creature::BODY_LEN) { error = "body length mismatch"; return false; }

        std::vector<long long> id, lineage, alive, glen, genes;
        std::vector<float> posX, posY, velX, velY, lmdX, lmdY, speed, age, food_, repro, inter, resp, oscP, oscPh, neurons, body;
        const bool ok =
            jsonm::readInts(*jcs, "id", id, n) && jsonm::readInts(*jcs, "lineageId", lineage, n) &&
            jsonm::readInts(*jcs, "alive", alive, n) &&
            jsonm::readFloats(*jcs, "posX", posX, n) && jsonm::readFloats(*jcs, "posY", posY, n) &&
            jsonm::readFloats(*jcs, "velX", velX, n) && jsonm::readFloats(*jcs, "velY", velY, n) &&
            jsonm::readFloats(*jcs, "lastMoveDirX", lmdX, n) && jsonm::readFloats(*jcs, "lastMoveDirY", lmdY, n) &&
            jsonm::readFloats(*jcs, "speed", speed, n) && jsonm::readFloats(*jcs, "age", age, n) &&
            jsonm::readFloats(*jcs, "foodReserves", food_, n) &&
            jsonm::readFloats(*jcs, "reproTimer", repro, n) && jsonm::readFloats(*jcs, "interactTimer", inter, n) &&
            jsonm::readFloats(*jcs, "responsiveness", resp, n) && jsonm::readFloats(*jcs, "oscPeriod", oscP, n) &&
            jsonm::readFloats(*jcs, "oscPhase", oscPh, n) &&
            jsonm::readFloats(*jcs, "neuronOutputs", neurons, n * MAX_INTERNAL_NEURONS) &&
            jsonm::readFloats(*jcs, "body", body, n * Creature::BODY_LEN * 2) &&
            jsonm::readInts(*jcs, "genomeLengths", glen, n) &&
            jsonm::readInts(*jcs, "genes", genes, (size_t)-1);
        if (!ok) { error = "malformed 'creatures' arrays (length mismatch?)"; return false; }

        size_t needed = 0;
        for (long long L : glen) {
            if (L < 0) { error = "negative genome length"; return false; }
            needed += (size_t)L * 2;
        }
        if (genes.size() != needed) { error = "'genes' length does not match 'genomeLengths'"; return false; }

        world.creatures.resize(n);
        size_t gi = 0;
        for (size_t i = 0; i < n; ++i) {
            Creature& c = world.creatures[i];
            c.id = (std::uint64_t)id[i];
            c.lineageId = (std::uint64_t)lineage[i];
            c.alive = alive[i] != 0;
            c.pos = Vec2{posX[i], posY[i]};
            c.vel = Vec2{velX[i], velY[i]};
            c.lastMoveDir = Vec2{lmdX[i], lmdY[i]};
            c.speed = speed[i];
            c.age = age[i];
            c.foodReserves = food_[i];
            c.reproTimer = repro[i];
            c.interactTimer = inter[i];
            c.responsiveness = resp[i];
            c.oscPeriod = oscP[i];
            c.oscPhase = oscPh[i];

            c.genome.clear();
            c.genome.reserve((size_t)glen[i]);
            for (long long k = 0; k < glen[i]; ++k, gi += 2)
                c.genome.push_back(unpackGene(genes[gi], genes[gi + 1]));

            // Net wiring is derived, so rebuild it — then put the recurrent
            // outputs back, which buildNeuralNet resets to the 0.5 default.
            buildNeuralNet(c.genome, c.nnet);
            for (int nn = 0; nn < MAX_INTERNAL_NEURONS; ++nn)
                c.nnet.neurons[nn].output = neurons[i * MAX_INTERNAL_NEURONS + nn];

            c.color = genomeColor(c.genome);
            for (int b = 0; b < Creature::BODY_LEN; ++b) {
                const size_t bi = (i * Creature::BODY_LEN + (size_t)b) * 2;
                c.body[b] = Vec2{body[bi], body[bi + 1]};
            }
        }
    }

    // LAST: the rng. init() above consumed draws building an empty world, so
    // this has to overwrite it or the resumed run follows a different sequence.
    const std::string state = j.stringOr("rngState", "");
    if (state.empty() || !rng.setState(state)) { error = "missing or unreadable rngState"; return false; }

    return true;
}

inline bool load(const std::string& path, World& world, std::string& error) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { error = "cannot open '" + path + "' for reading"; return false; }
    std::ostringstream ss;
    ss << f.rdbuf();
    return deserialize(ss.str(), world, error);
}

}  // namespace checkpoint
