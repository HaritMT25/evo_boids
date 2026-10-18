// evo_wasm.cpp — C ABI shim: the browser-facing surface of evo_core.
//
// Compiled with Emscripten into web/evo_boids.{js,wasm} (see build_wasm.sh).
// The JS renderer (web/renderer.js) drives it: init(seed, config) once, then
// per animation frame step(n) -> sync() -> read the packed buffers.
//
// Determinism contract: single-threaded (no USE_OPENMP), -ffp-contract=off,
// fixed dt 0.016f per step — under those, this module reproduces the native
// serial build bit for bit (proven by wasm/parity.sh; the checkpoint JSON is
// the oracle). step() takes a STEP COUNT, never wall-clock time.
//
// The same source also builds the threaded module (EVO_POOL_THREADS, see
// wasm/wasm.cmake): identical API, but the per-creature phase runs on a thread
// pool, so a run is NOT reproducible — the single-threaded module stays the
// reference.
//
// Buffer lifetime: getters return pointers into buffers repacked by
// evo_sync(); pointers are valid until the next evo_sync/evo_init call, and
// wasm memory growth can move the heap — JS must re-create typed-array views
// after every sync and never cache them.
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cmath>
#include <string>
#include <vector>

#include "core/world.hpp"
#include "core/checkpoint_io.hpp"
#include "core/random.hpp"
#include "core/stats.hpp"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#define EVO_EXPORT extern "C" EMSCRIPTEN_KEEPALIVE
#else
#define EVO_EXPORT extern "C"  // native build of the shim, for smoke tests
#endif

#ifndef EVO_VERSION
#define EVO_VERSION "dev"
#endif

namespace {

World* g_world = nullptr;
std::string g_error;
std::string g_checkpoint;
std::string g_defaultConfig;

std::vector<float>   g_segments;   // N*12: 6 body segments x,y
std::vector<float>   g_bodyParams; // N*2: bodySize, bodyElong
std::vector<uint8_t> g_colors;     // N*3
std::vector<double>  g_ids;        // N
std::vector<float>   g_food;       // F*2
std::vector<float>   g_predators;  // P*3: x, y, digestTimer
std::vector<float>   g_current;    // cols*rows*2
// evo_stats() layout — web/renderer.js reads STATS_LEN of them; change both:
// 0 creatures, 1 food, 2 predators, 3 frame, 4 births (last step), 5 deaths
// (last step), 6 total births, 7 total deaths, 8 mean genome length,
// 9 mean generation, 10 max generation
constexpr int kStatsLen = 11;
double g_stats[kStatsLen] = {};
int g_currentCols = 0, g_currentRows = 0;

// The last successful evo_init arguments, so evo_reset() can replay them.
struct InitArgs {
    unsigned seed = 0;
    int width = 0, height = 0, population = 0;
    std::string cfgJson;
    bool valid = false;
} g_lastInit;

constexpr int kCurrentStep = 64;   // overlay lattice pitch, mirrors SfmlRenderer

// The same defensive clamps main.cpp applies before init.
void clampConfig(SimConfig& c) {
    if (c.minSpeed > c.maxSpeed) std::swap(c.minSpeed, c.maxSpeed);
    if (c.genomeMinLength > c.genomeMaxLength) std::swap(c.genomeMinLength, c.genomeMaxLength);
    if (c.genomeInitMin > c.genomeInitMax) std::swap(c.genomeInitMin, c.genomeInitMax);
    if (c.perceptionRadius < 1.0f) c.perceptionRadius = 1.0f;
    if (c.maxNeighbors < 0) c.maxNeighbors = 0;
    if (c.maxNeighbors > MAX_TOPOLOGICAL_NEIGHBORS)
        c.maxNeighbors = MAX_TOPOLOGICAL_NEIGHBORS;
}

}  // namespace

// Initialise (or re-initialise) the world. cfgJson may be null/empty for
// defaults, or a flat JSON object using the same field names as a
// checkpoint's "config" block; any subset of keys is accepted.
EVO_EXPORT int evo_init(unsigned seed, int width, int height, int population,
                        const char* cfgJson) {
    g_error.clear();
    SimConfig cfg;
    if (cfgJson && cfgJson[0]) {
        // readConfig expects the config under a "config" key; wrap the flat
        // object so the shim accepts the checkpoint schema directly.
        const std::string wrapped = std::string("{\"config\":") + cfgJson + "}";
        jsonm::Value root;
        std::string err;
        if (!jsonm::Parser::parse(wrapped, root, err)) {
            g_error = "config parse error: " + err;
            return 0;
        }
        checkpoint::readConfig(root, cfg);
    }
    clampConfig(cfg);
    if (width < 100 || height < 100 || population < 0) {
        g_error = "bad world dimensions or population";
        return 0;
    }
    // Same refusal main.cpp makes: the genetic gate folds a genome into a fixed
    // stack buffer, so an oversized genomeMaxLength would silently gate on a
    // truncated genome. Reject rather than clamp, so the two frontends agree.
    if (cfg.reproMaxDistance >= 0.0f &&
        cfg.genomeMaxLength > isolation_detail::ISO_MAX_GENES) {
        g_error = "reproMaxDistance requires genomeMaxLength <= "
                + std::to_string(isolation_detail::ISO_MAX_GENES);
        return 0;
    }
    // Same refusal main.cpp makes, for the same reason and on the same terms:
    // a negative half-width runs rng.range backwards, and a non-finite one
    // makes rng.range yield NaN, which the inheritance clamp does not filter —
    // every newborn body goes NaN. The config here arrives as parsed JSON, so
    // both are reachable from the page. Reject rather than clamp, so the two
    // frontends agree. Negated test: NaN compares false to everything.
    if (!(cfg.bodyMutation >= 0.0f) || !std::isfinite(cfg.bodyMutation)) {
        g_error = "bodyMutation must be finite and >= 0";
        return 0;
    }
    // Same refusal main.cpp makes for the gate knobs, so the frontends agree
    // there too. Negative is legal and means off (the reproMaxDistance
    // convention); non-finite is not: every comparison against NaN is false, so
    // a NaN soft width rejects EVERY pair while still burning a draw, and a NaN
    // threshold ARMS the gate (NaN < 0 is false) and then refuses everything —
    // both the opposite of what the knob advertises.
    if (!std::isfinite(cfg.reproSoftWidth) || !std::isfinite(cfg.reproGracePeriod) ||
        !std::isfinite(cfg.reproMaxDistance)) {
        g_error = "reproMaxDistance, reproSoftWidth and reproGracePeriod must be finite (negative = off)";
        return 0;
    }

    // Always capture per-creature sensor/action activations for the neural
    // inspector. Dynamics-neutral (a pure memcpy in World::update, no rng),
    // and not serialized into checkpoints — parity with the native CLI holds.
    cfg.captureActivations = true;

    rng.seed(seed);
    delete g_world;
    g_world = new World();
    g_world->cfg = cfg;
    g_world->init(width, height, population);

    g_lastInit.seed = seed;
    g_lastInit.width = width;
    g_lastInit.height = height;
    g_lastInit.population = population;
    g_lastInit.cfgJson = (cfgJson && cfgJson[0]) ? cfgJson : "";
    g_lastInit.valid = true;
    return 1;
}

// Re-run the last evo_init with the same arguments: same seed and config, so in
// the single-threaded module the run replays exactly. Live parameter changes
// made since (evo_set_param) are dropped.
EVO_EXPORT int evo_reset() {
    if (!g_lastInit.valid) { g_error = "evo_reset before any successful evo_init"; return 0; }
    const InitArgs a = g_lastInit;  // evo_init overwrites g_lastInit
    return evo_init(a.seed, a.width, a.height, a.population,
                    a.cfgJson.empty() ? nullptr : a.cfgJson.c_str());
}

// Change one parameter of the running world. Only parameters the step reads
// fresh every tick are accepted; anything baked in at init (world size,
// currents, food patches, predator count, the toroidal grid) needs a restart.
// All accepted values are non-negative. Returns 0 and sets evo_last_error() on
// refusal; the config is left unchanged.
EVO_EXPORT int evo_set_param(const char* name, double value) {
    g_error.clear();
    if (!g_world) { g_error = "no world (call evo_init first)"; return 0; }
    static const char* const kLive[] = {
        "maxPopulation", "minPopulation", "targetFood", "foodSpawnBase", "foodSpawnAmp",
        "foodEnergy", "metabolicRate", "moveCost", "geneCost", "maxAge", "reproEnergy",
        "newbornEnergy", "mutationMultiplier", "minSpeed", "maxSpeed",
    };
    bool live = false;
    for (const char* k : kLive) if (name && std::strcmp(name, k) == 0) { live = true; break; }
    if (!live) {
        g_error = std::string("not a live parameter (change it and restart): ") + (name ? name : "(null)");
        return 0;
    }
    if (!std::isfinite(value) || value < 0.0) {
        g_error = std::string(name) + " must be finite and >= 0";
        return 0;
    }
    // Route through the checkpoint config reader so the field mapping (and the
    // int/float typing) is the one place that already defines it.
    char num[40];
    std::snprintf(num, sizeof num, "%.9g", value);
    const std::string json = std::string("{\"config\":{\"") + name + "\":" + num + "}}";
    jsonm::Value root;
    std::string err;
    if (!jsonm::Parser::parse(json, root, err)) { g_error = "internal: " + err; return 0; }
    SimConfig cfg = g_world->cfg;
    checkpoint::readConfig(root, cfg);
    clampConfig(cfg);
    g_world->cfg = cfg;
    return 1;
}

// ---- Threading -------------------------------------------------------------
// 1 in the pthreads module, 0 in the single-threaded one.
EVO_EXPORT int evo_threaded() {
#ifdef EVO_POOL_THREADS
    return 1;
#else
    return 0;
#endif
}
EVO_EXPORT int evo_max_threads() {
#ifdef EVO_POOL_THREADS
    return evo_par::Pool::maxThreads();
#else
    return 1;
#endif
}
EVO_EXPORT int evo_threads() {
#ifdef EVO_POOL_THREADS
    return evo_par::Pool::instance().threads();
#else
    return 1;
#endif
}
// Set the thread count (clamped to [1, evo_max_threads()]); returns the count
// in effect. A no-op in the single-threaded module.
EVO_EXPORT int evo_set_threads(int n) {
#ifdef EVO_POOL_THREADS
    evo_par::Pool::instance().setThreads(n);
#else
    (void)n;
#endif
    return evo_threads();
}

// Remove creatures uniformly at random until at most n remain; returns how many
// were removed. Used by the page's frame-budget governor (web/renderer.js) to
// shed agents immediately instead of waiting for the birth cap to act through
// deaths. Uniform, so it favours no trait. Not a death: no death log entry and
// no counters. It draws from its own SplitMix64 seeded by the frame number, so
// the simulation's rng stream is untouched — but the run itself has been
// changed, so it no longer replays from its seed (the page says so).
// Safe between steps: nothing in World keeps creature indices across update()
// (the pairing log is keyed by Creature::id).
EVO_EXPORT int evo_cull_to(int n) {
    if (!g_world || n < 0) return 0;
    std::vector<Creature>& cs = g_world->creatures;
    const size_t total = cs.size();
    if (total <= (size_t)n) return 0;
    const size_t k = total - (size_t)n;

    std::uint64_t s = 0x9E3779B97F4A7C15ull ^ (std::uint64_t)g_world->frame;
    auto next = [&s]() {
        std::uint64_t z = (s += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    };
    std::vector<size_t> idx(total);
    for (size_t i = 0; i < total; ++i) idx[i] = i;
    std::vector<char> drop(total, 0);
    for (size_t i = 0; i < k; ++i) {  // partial Fisher-Yates: the first k picks
        const size_t j = i + (size_t)(next() % (total - i));
        std::swap(idx[i], idx[j]);
        drop[idx[i]] = 1;
    }
    size_t w = 0;
    for (size_t r = 0; r < total; ++r) {
        if (drop[r]) continue;
        if (w != r) cs[w] = std::move(cs[r]);
        ++w;
    }
    cs.resize(w);
    return (int)k;
}

// Advance n fixed steps. dt is pinned to the native headless path's 0.016f —
// passing wall-clock time here would destroy reproducibility.
EVO_EXPORT void evo_step(int n) {
    if (!g_world) return;
    for (int i = 0; i < n; ++i) g_world->update(0.016f);
}

// Repack all export buffers from the current world state.
EVO_EXPORT void evo_sync() {
    if (!g_world) return;
    const World& w = *g_world;
    const size_t n = w.creatures.size();

    g_segments.resize(n * 12);
    g_bodyParams.resize(n * 2);
    g_colors.resize(n * 3);
    g_ids.resize(n);
    for (size_t i = 0; i < n; ++i) {
        const Creature& c = w.creatures[i];
        for (int s = 0; s < Creature::BODY_LEN; ++s) {
            g_segments[i * 12 + s * 2 + 0] = c.body[s].x;
            g_segments[i * 12 + s * 2 + 1] = c.body[s].y;
        }
        // Filled in the same pass as the segments so the two buffers can never
        // disagree about creature order or count.
        g_bodyParams[i * 2 + 0] = c.bodySize;
        g_bodyParams[i * 2 + 1] = c.bodyElong;
        g_colors[i * 3 + 0] = c.color.r;
        g_colors[i * 3 + 1] = c.color.g;
        g_colors[i * 3 + 2] = c.color.b;
        g_ids[i] = (double)c.id;
    }

    g_food.resize(w.food.size() * 2);
    for (size_t i = 0; i < w.food.size(); ++i) {
        g_food[i * 2 + 0] = w.food[i].pos.x;
        g_food[i * 2 + 1] = w.food[i].pos.y;
    }

    g_predators.resize(w.predators.size() * 3);
    for (size_t i = 0; i < w.predators.size(); ++i) {
        g_predators[i * 3 + 0] = w.predators[i].pos.x;
        g_predators[i * 3 + 1] = w.predators[i].pos.y;
        g_predators[i * 3 + 2] = w.predators[i].digestTimer;
    }

    if (w.currentActive()) {
        g_currentCols = (w.width  > kCurrentStep / 2) ? (w.width  - kCurrentStep / 2 + kCurrentStep - 1) / kCurrentStep : 0;
        g_currentRows = (w.height > kCurrentStep / 2) ? (w.height - kCurrentStep / 2 + kCurrentStep - 1) / kCurrentStep : 0;
        g_current.resize((size_t)g_currentCols * g_currentRows * 2);
        size_t k = 0;
        for (int j = 0; j < g_currentRows; ++j) {
            const float y = kCurrentStep / 2.0f + (float)j * kCurrentStep;
            for (int i = 0; i < g_currentCols; ++i) {
                const float x = kCurrentStep / 2.0f + (float)i * kCurrentStep;
                const Vec2 v = w.currentAt({x, y});
                g_current[k++] = v.x;
                g_current[k++] = v.y;
            }
        }
    } else {
        g_currentCols = g_currentRows = 0;
        g_current.clear();
    }

    double avgGenome = 0.0;
    for (const auto& c : w.creatures) avgGenome += (double)c.genome.size();
    if (n) avgGenome /= (double)n;
    g_stats[0] = (double)n;
    g_stats[1] = (double)w.food.size();
    g_stats[2] = (double)w.predators.size();
    g_stats[3] = (double)w.frame;
    g_stats[4] = (double)w.births;
    g_stats[5] = (double)w.deaths;
    g_stats[6] = (double)w.totalBirths;
    g_stats[7] = (double)w.totalDeaths;
    g_stats[8] = avgGenome;
    const GenerationSpan gen = generationSpan(w);
    g_stats[9] = gen.mean;
    g_stats[10] = (double)gen.max;
}

EVO_EXPORT int evo_creature_count()             { return g_world ? (int)g_world->creatures.size() : 0; }
EVO_EXPORT const float* evo_creature_segments() { return g_segments.data(); }
// N*2 (bodySize, bodyElong), creature order identical to evo_creature_segments.
// Separate buffer, not extra columns: the segments stride of 12 is relied on by
// every reader.
EVO_EXPORT const float* evo_body_params()       { return g_bodyParams.data(); }
EVO_EXPORT const uint8_t* evo_creature_colors() { return g_colors.data(); }
EVO_EXPORT const double* evo_creature_ids()     { return g_ids.data(); }
EVO_EXPORT int evo_food_count()                 { return g_world ? (int)g_world->food.size() : 0; }
EVO_EXPORT const float* evo_food_xy()           { return g_food.data(); }
EVO_EXPORT int evo_predator_count()             { return g_world ? (int)g_world->predators.size() : 0; }
EVO_EXPORT const float* evo_predator_data()     { return g_predators.data(); }
EVO_EXPORT int evo_current_active()             { return (g_world && g_world->currentActive()) ? 1 : 0; }
EVO_EXPORT int evo_current_cols()               { return g_currentCols; }
EVO_EXPORT int evo_current_rows()               { return g_currentRows; }
EVO_EXPORT const float* evo_current_grid()      { return g_current.data(); }
EVO_EXPORT const double* evo_stats()            { return g_stats; }
EVO_EXPORT int evo_stats_len()                  { return kStatsLen; }
EVO_EXPORT int evo_width()                      { return g_world ? g_world->width : 0; }
EVO_EXPORT int evo_height()                     { return g_world ? g_world->height : 0; }

// Default config as JSON ({"config":{...}} — the checkpoint schema), for UIs
// that want to show every knob with its default.
EVO_EXPORT const char* evo_default_config() {
    std::string out;
    jsonm::Writer w(out);
    w.beginObject();
    checkpoint::writeConfig(w, SimConfig{});
    w.endObject();
    g_defaultConfig = out;
    return g_defaultConfig.c_str();
}

// Full checkpoint JSON of the current state. Identical bytes to what the
// native serial CLI would write for the same seed/config/steps.
EVO_EXPORT const char* evo_checkpoint() {
    if (!g_world) return "";
    g_checkpoint = checkpoint::serialize(*g_world, EVO_VERSION);
    return g_checkpoint.c_str();
}

// Restore from checkpoint JSON (replaces world, config, dimensions, and the
// rng stream — the resumed run continues bit-exactly).
EVO_EXPORT int evo_load_checkpoint(const char* json) {
    g_error.clear();
    if (!json || !json[0]) { g_error = "empty checkpoint"; return 0; }
    World* w = new World();
    std::string err;
    if (!checkpoint::deserialize(json, *w, err)) {
        delete w;
        g_error = "checkpoint load error: " + err;
        return 0;
    }
    delete g_world;
    g_world = w;
    g_world->cfg.captureActivations = true;  // see evo_init
    return 1;
}

EVO_EXPORT const char* evo_last_error() { return g_error.c_str(); }

// ---- Neural inspector ------------------------------------------------------
// evo_inspect(i) snapshots creature i (index into this frame's arrays, valid
// until the next evo_step/evo_sync) for the click-to-inspect panel. Buffers
// mirror what the native SFML overlay reads (sfml_renderer.cpp
// drawNeuralOverlay): lastSensors / neuron outputs / lastActions, and the
// flattened connection list decoded via asGene().
namespace {
// 7 doubles, in this order — web/renderer.js reads INSPECT_INFO_LEN of them and
// the two must be changed together.
double g_inspInfo[7] = {};                 // id, speed, foodReserves, age, bodySize, bodyElong, generation
std::vector<float> g_inspSensors;          // NUM_SENSES
std::vector<float> g_inspHidden;           // MAX_INTERNAL_NEURONS
std::vector<float> g_inspActions;          // NUM_ACTIONS
std::vector<float> g_inspConns;            // K*5: srcIsSensor, srcNum, sinkIsAction, sinkNum, weight
}

EVO_EXPORT int evo_inspect(int index) {
    if (!g_world || index < 0 || (size_t)index >= g_world->creatures.size()) return 0;
    const Creature& c = g_world->creatures[(size_t)index];

    g_inspInfo[0] = (double)c.id;
    g_inspInfo[1] = (double)c.speed;
    g_inspInfo[2] = (double)c.foodReserves;
    g_inspInfo[3] = (double)c.age;
    g_inspInfo[4] = (double)c.bodySize;
    g_inspInfo[5] = (double)c.bodyElong;
    g_inspInfo[6] = (double)c.generation;

    g_inspSensors.assign(c.lastSensors, c.lastSensors + NUM_SENSES);
    g_inspActions.assign(c.lastActions, c.lastActions + NUM_ACTIONS);
    g_inspHidden.resize(MAX_INTERNAL_NEURONS);
    for (int i = 0; i < MAX_INTERNAL_NEURONS; ++i)
        g_inspHidden[i] = c.nnet.neurons[i].output;

    g_inspConns.resize(c.nnet.flat.size() * 5);
    size_t k = 0;
    for (const auto& fc : c.nnet.flat) {
        const Gene g = asGene(fc);
        g_inspConns[k++] = (g.sourceType == SOURCE_SENSOR) ? 1.0f : 0.0f;
        g_inspConns[k++] = (float)g.sourceNum;
        g_inspConns[k++] = (g.sinkType == SINK_ACTION) ? 1.0f : 0.0f;
        g_inspConns[k++] = (float)g.sinkNum;
        g_inspConns[k++] = g.weightAsFloat();
    }
    return 1;
}

EVO_EXPORT const double* evo_inspect_info()   { return g_inspInfo; }
EVO_EXPORT const float* evo_inspect_sensors() { return g_inspSensors.data(); }
EVO_EXPORT const float* evo_inspect_hidden()  { return g_inspHidden.data(); }
EVO_EXPORT const float* evo_inspect_actions() { return g_inspActions.data(); }
EVO_EXPORT int evo_inspect_conn_count()       { return (int)(g_inspConns.size() / 5); }
EVO_EXPORT const float* evo_inspect_conns()   { return g_inspConns.data(); }
