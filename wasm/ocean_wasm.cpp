// ocean_wasm.cpp — C ABI shim for the Ocean preview (PLAN_ocean_preview.md §4).
//
// CONTRACT DEVIATIONS: one. oc_init refuses a reversed minSpeed/maxSpeed or
// genome-length pair, where evo_init swaps it (see the refusals below).
// Choices the contract leaves open, fixed here:
//   * oc_init: depth <= 0 means the default sea depth (OceanConfig{}.depth,
//     240); a non-finite depth or one in (0, 10) is refused like a bad width.
//   * oc_stats: births, deaths and reseeds are CUMULATIVE totals over the run
//     (reseeds = founders the minPopulation floor added); stats_len = 15.
//   * oc_init also refuses (configOk; the previous world is kept): genome
//     lengths outside 16 <= genomeMinLength <= genomeInitMin <= genomeInitMax
//     <= genomeMaxLength <= 80 (GENOME_*); speeds other than finite
//     0 <= minSpeed <= maxSpeed with maxSpeed > 0; a reversed min/max pair is
//     refused, not swapped as evo_init does; a radius that is not finite and
//     >= 0, whose square is not a finite float (>= ~1.8e19: every radius is
//     used squared), or, toroidal, not < min(width, height) / 2 (the wrapped
//     grid query's bound; OceanWorld::radiusError).
//   * oc_set_param: the Ocean keys kBuoy, pitchMax, vDrag, wallBand,
//     buoyMutation, cHold, cPress, foodDepthSpread, eye, eyeOnly, eyeCode,
//     steerFrame, foodLife, plus every key evo_set_param accepts. pitchMax must
//     be > 0. The changed config must pass oc_init's checks as stored (after
//     the float / int conversion).
//   * eye (OceanConfig::eye, the compound eye) and eyeOnly (the labeled senses
//     off; with eye 0, "blind") are 0 or 1 in the eye build (-DEVO_OCEAN_EYE)
//     and must stay 0 in the plain one; oc_has_eye() says which build this is.
//     eyeCode (the eye's encoding, a bitmask: 1 faded contrast, 2 opponent
//     pairs, 4 summed sectors) is 0..7 in the eye build and must stay 0 in the
//     plain one.
//   * steerFrame (OceanConfig::steerFrame: STEER_X / STEER_Y in world x / y, or
//     in the fish's own frame) is 0 or 1 in both builds.
//   * With no cfgJson the world is the tuned preview (applyOceanDefaults +
//     OceanConfig defaults, src/ocean/ocean_config.hpp).
//
// ADDITIONS beyond §4 (not deviations; the page feature-detects them):
//   * const float* oc_creature_vz() — N floats, same order as
//     oc_creature_segments, refreshed by oc_sync: each fish's Fish::vz as the
//     engine integrates it (depth += vz·speed·dt; + = sinking; |vz| <=
//     pitchMax). The fish swims horizontally at `speed` along its unit (x, y)
//     heading and vertically at vz·speed, so its pitch is atan(vz) (+ = nose
//     down). The current's horizontal drift is not included.
//
// Compiled with Emscripten into web/evo_ocean.{js,wasm} by wasm/build_ocean.sh
// (-DEVO_WORLD_OCEAN, single-threaded). The page (web/ocean.js) drives it:
// oc_init once, then per frame oc_step(n) -> oc_sync() -> read the buffers.
// Every pointer is valid until the next oc_sync / oc_init; memory can grow, so
// the page re-creates its typed-array views after every sync.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "ocean/ocean_config.hpp"
#include "ocean/ocean_world.hpp"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#define EVO_EXPORT extern "C" EMSCRIPTEN_KEEPALIVE
#else
#define EVO_EXPORT extern "C"  // native build of the shim, for smoke tests
#endif

namespace {

OceanWorld* g_world = nullptr;
std::string g_error;

std::vector<float>   g_segments;   // N*12: 6 spine points x, y  (= evo_creature_segments)
std::vector<float>   g_depth;      // N
std::vector<float>   g_vz;         // N: Fish::vz (pitch = atan(vz))
std::vector<float>   g_bodyParams; // N*2: bodySize, bodyElong  (= evo_body_params)
std::vector<uint8_t> g_colors;     // N*3: family colour         (= evo_creature_colors)
std::vector<double>  g_ids;        // N                           (= evo_creature_ids)
std::vector<float>   g_food;       // F*2: x, y                   (= evo_food_xy)
std::vector<float>   g_foodDepth;  // F
std::vector<float>   g_predators;  // P*3: x, y, digestTimer      (= evo_predator_data)
std::vector<float>   g_predDepth;  // P
// [pop, frame, births, deaths, reseeds, depthMean, depthSd, hist0 … hist7]
constexpr int kStatsLen = 7 + ocean::kDepthBins;
double g_stats[kStatsLen] = {};

struct InitArgs {
    unsigned seed = 0;
    int width = 0, height = 0, population = 0;
    double depth = 0.0;
    std::string cfgJson;
    bool valid = false;
} g_lastInit;

// Copied from wasm/evo_wasm.cpp (display only): a family line's colour from
// its id alone, the same bytes in every build.
void familyColor(std::uint64_t id, std::uint8_t* out) {
    std::uint32_t h = (std::uint32_t)(id ^ (id >> 32));
    h ^= h >> 16; h *= 0x7feb352dU; h ^= h >> 15; h *= 0x846ca68bU; h ^= h >> 16;
    static const int kSat[4] = {200, 165, 225, 185}, kVal[4] = {240, 215, 195, 255};
    const int tone = (int)((h >> 12) & 3u);
    const int s = kSat[tone], v = kVal[tone];
    constexpr int kRedGap = 85;
    const int hue = kRedGap + (int)(h % (1536u - 2 * kRedGap)), sector = hue >> 8, f = hue & 255;
    const int p = v * (255 - s) / 255;
    const int q = v * (65025 - s * f) / 65025;
    const int t = v * (65025 - s * (255 - f)) / 65025;
    int r, g, b;
    switch (sector) {
        case 0: r = v; g = t; b = p; break;
        case 1: r = q; g = v; b = p; break;
        case 2: r = p; g = v; b = t; break;
        case 3: r = p; g = q; b = v; break;
        case 4: r = t; g = p; b = v; break;
        default: r = v; g = p; b = q; break;
    }
    out[0] = (std::uint8_t)r; out[1] = (std::uint8_t)g; out[2] = (std::uint8_t)b;
}

// The defensive clamps main.cpp applies (copied from wasm/evo_wasm.cpp) minus
// its min/max swaps: Ocean refuses a reversed speed or genome-length pair
// (configOk) instead of reordering it, so a live change to one bound can never
// silently move the other.
void clampConfig(SimConfig& c) {
    if (c.perceptionRadius < 1.0f) c.perceptionRadius = 1.0f;
    if (c.maxNeighbors < 0) c.maxNeighbors = 0;
    if (c.maxNeighbors > MAX_TOPOLOGICAL_NEIGHBORS) c.maxNeighbors = MAX_TOPOLOGICAL_NEIGHBORS;
    if (c.predatorMinPopulation < 0) c.predatorMinPopulation = 0;
    if (c.predatorMinPopulation > 2000) c.predatorMinPopulation = 2000;
    if (c.predatorMaxPopulation < 0) c.predatorMaxPopulation = 0;
}

// Every refusal oc_init and oc_set_param share, on the config exactly as the
// engine will hold it (after the JSON's double -> float / int conversion and
// clampConfig): evo_init's (same reasons; see wasm/evo_wasm.cpp), then genome
// lengths, speeds, the live floats, radii vs the world size, and the Ocean
// knobs. Nothing here touches the world; callers replace or change it only
// after this passes.
bool configOk(const SimConfig& cfg, const OceanConfig& oc, int width, int height) {
    if (cfg.reproMaxDistance >= 0.0f && cfg.genomeMaxLength > isolation_detail::ISO_MAX_GENES) {
        g_error = "reproMaxDistance requires genomeMaxLength <= "
                + std::to_string(isolation_detail::ISO_MAX_GENES);
        return false;
    }
    if (!(cfg.bodyMutation >= 0.0f) || !std::isfinite(cfg.bodyMutation)) {
        g_error = "bodyMutation must be finite and >= 0";
        return false;
    }
    if (!(cfg.predatorTraitMutation >= 0.0f) || !std::isfinite(cfg.predatorTraitMutation)) {
        g_error = "predatorTraitMutation must be finite and >= 0";
        return false;
    }
    if (!std::isfinite(cfg.reproSoftWidth) || !std::isfinite(cfg.reproGracePeriod) ||
        !std::isfinite(cfg.reproMaxDistance)) {
        g_error = "reproMaxDistance, reproSoftWidth and reproGracePeriod must be finite (negative = off)";
        return false;
    }
    // Genome lengths: makeRandomGenome reserves genomeInitMin..Max genes and
    // children are held to genomeMinLength..MaxLength.
    if (!(GENOME_MIN_LENGTH <= cfg.genomeMinLength && cfg.genomeMinLength <= cfg.genomeMaxLength &&
          cfg.genomeMaxLength <= GENOME_MAX_LENGTH)) {
        g_error = "genome lengths need " + std::to_string(GENOME_MIN_LENGTH) +
                  " <= genomeMinLength <= genomeMaxLength <= " + std::to_string(GENOME_MAX_LENGTH);
        return false;
    }
    if (!(cfg.genomeMinLength <= cfg.genomeInitMin && cfg.genomeInitMin <= cfg.genomeInitMax &&
          cfg.genomeInitMax <= cfg.genomeMaxLength)) {
        g_error = "genome lengths need genomeMinLength <= genomeInitMin <= genomeInitMax <= genomeMaxLength";
        return false;
    }
    // Speeds: the OWN_SPEED sensor is speed / maxSpeed.
    if (!std::isfinite(cfg.minSpeed) || !std::isfinite(cfg.maxSpeed) || !(cfg.minSpeed >= 0.0f) ||
        !(cfg.maxSpeed > 0.0f) || !(cfg.minSpeed <= cfg.maxSpeed)) {
        g_error = "speeds need finite 0 <= minSpeed <= maxSpeed and maxSpeed > 0";
        return false;
    }
    // The other floats oc_set_param can change: a finite double can still
    // overflow to an infinite float.
    const struct { const char* name; float v; } liveFloats[] = {
        {"foodSpawnBase", cfg.foodSpawnBase}, {"foodSpawnAmp", cfg.foodSpawnAmp},
        {"foodEnergy", cfg.foodEnergy}, {"metabolicRate", cfg.metabolicRate},
        {"moveCost", cfg.moveCost}, {"geneCost", cfg.geneCost}, {"maxAge", cfg.maxAge},
        {"reproEnergy", cfg.reproEnergy}, {"newbornEnergy", cfg.newbornEnergy},
        {"mutationMultiplier", cfg.mutationMultiplier},
    };
    for (const auto& f : liveFloats)
        if (!std::isfinite(f.v)) { g_error = std::string(f.name) + " must be finite"; return false; }
    // Radii: finite, >= 0, a finite square, and in a toroidal world
    // < min(width, height) / 2.
    g_error = OceanWorld::radiusError(cfg, width, height);
    if (!g_error.empty()) return false;
    if (!oceanConfigValid(oc)) {
        g_error = (oc.eye != 0 && !(kOceanHasEye && oc.eye == 1))
                      ? (kOceanHasEye ? "eye must be 0 or 1"
                                      : "eye needs the eye build (-DEVO_OCEAN_EYE); this build has no eye")
                  : (oc.eyeOnly != 0 && !(kOceanHasEye && oc.eyeOnly == 1))
                      ? (kOceanHasEye ? "eyeOnly must be 0 or 1"
                                      : "eyeOnly needs the eye build (-DEVO_OCEAN_EYE); this build has no eye")
                  : !eyeCodeValid(oc.eyeCode)
                      ? (kOceanHasEye ? "eyeCode must be 0..7 (bits: 1 faded contrast, 2 opponent pairs, 4 summed sectors)"
                                      : "eyeCode needs the eye build (-DEVO_OCEAN_EYE); this build has no eye")
                  : (oc.steerFrame != 0 && oc.steerFrame != 1)
                      ? "steerFrame must be 0 or 1"
                      : "ocean config: kBuoy, vDrag, wallBand, cHold, cPress must be finite; "
                        "buoyMutation, vDrag, wallBand, foodDepthSpread, foodLife >= 0; pitchMax > 0";
        return false;
    }
    return true;
}

}  // namespace

// Initialise (or re-initialise) the world. cfgJson may be null/empty for the
// preview defaults, or a flat JSON object of SimConfig keys (the checkpoint
// "config" names) and OceanConfig keys; any subset is accepted. Returns 1, or
// 0 with oc_last_error() set (the previous world, if any, is kept).
EVO_EXPORT int oc_init(unsigned seed, int width, int height, double depth, int population,
                       const char* cfgJson) {
    g_error.clear();
    SimConfig cfg;
    applyOceanDefaults(cfg);
    OceanConfig oc;
    if (!readOceanJson(cfgJson, cfg, oc, g_error)) return 0;
    clampConfig(cfg);
    if (std::isfinite(depth) && depth <= 0.0) depth = OceanConfig{}.depth;
    if (width < 100 || height < 100 || population < 0 || !std::isfinite(depth) || depth < 10.0) {
        g_error = "bad world dimensions or population";
        return 0;
    }
    oc.depth = (float)depth;
    if (!configOk(cfg, oc, width, height)) return 0;

    // Every refusal is above this line: the old world goes only for a config
    // that passed them all.
    rng.seed(seed);
    delete g_world;
    g_world = new OceanWorld();
    g_world->cfg = cfg;
    g_world->ocfg = oc;
    g_world->init(width, height, (float)depth, population);

    g_lastInit.seed = seed;
    g_lastInit.width = width;
    g_lastInit.height = height;
    g_lastInit.depth = depth;
    g_lastInit.population = population;
    g_lastInit.cfgJson = (cfgJson && cfgJson[0]) ? cfgJson : "";
    g_lastInit.valid = true;
    return 1;
}

// Re-run the last successful oc_init (same seed and config: the run replays
// exactly). Live changes made since with oc_set_param are dropped.
EVO_EXPORT int oc_reset() {
    if (!g_lastInit.valid) { g_error = "oc_reset before any successful oc_init"; return 0; }
    const InitArgs a = g_lastInit;
    return oc_init(a.seed, a.width, a.height, a.depth, a.population,
                   a.cfgJson.empty() ? nullptr : a.cfgJson.c_str());
}

// Advance n fixed steps of dt 0.016 (a step count, never wall-clock time).
EVO_EXPORT void oc_step(int n) {
    if (!g_world) return;
    for (int i = 0; i < n; ++i) g_world->update(0.016f);
}

// Repack every export buffer from the current world state.
EVO_EXPORT void oc_sync() {
    if (!g_world) return;
    const OceanWorld& w = *g_world;
    const size_t n = w.creatures.size();
    g_segments.resize(n * 12);
    g_depth.resize(n);
    g_vz.resize(n);
    g_bodyParams.resize(n * 2);
    g_colors.resize(n * 3);
    g_ids.resize(n);
    for (size_t i = 0; i < n; ++i) {
        const Fish& c = w.creatures[i];
        for (int s = 0; s < Fish::BODY_LEN; ++s) {
            g_segments[i * 12 + s * 2 + 0] = c.body[s].x;
            g_segments[i * 12 + s * 2 + 1] = c.body[s].y;
        }
        g_depth[i] = c.depth;
        g_vz[i] = c.vz;
        g_bodyParams[i * 2 + 0] = c.bodySize;
        g_bodyParams[i * 2 + 1] = c.bodyElong;
        familyColor(c.lineageId, &g_colors[i * 3]);
        g_ids[i] = (double)c.id;
    }

    const size_t nf = w.food.size();
    g_food.resize(nf * 2);
    g_foodDepth.resize(nf);
    for (size_t i = 0; i < nf; ++i) {
        g_food[i * 2 + 0] = w.food[i].pos.x;
        g_food[i * 2 + 1] = w.food[i].pos.y;
        g_foodDepth[i] = w.food[i].depth;
    }

    const size_t np = w.predators.size();
    g_predators.resize(np * 3);
    g_predDepth.resize(np);
    for (size_t i = 0; i < np; ++i) {
        g_predators[i * 3 + 0] = w.predators[i].pos.x;
        g_predators[i * 3 + 1] = w.predators[i].pos.y;
        g_predators[i * 3 + 2] = w.predators[i].digestTimer;
        g_predDepth[i] = w.predators[i].depth;
    }

    double mean, sd, hist[ocean::kDepthBins];
    w.depthStats(mean, sd, hist);
    g_stats[0] = (double)n;
    g_stats[1] = (double)w.frame;
    g_stats[2] = (double)w.totalBirths;
    g_stats[3] = (double)w.totalDeaths;
    g_stats[4] = (double)w.totalReseeds;
    g_stats[5] = mean;
    g_stats[6] = sd;
    for (int b = 0; b < ocean::kDepthBins; ++b) g_stats[7 + b] = hist[b];
}

EVO_EXPORT int oc_creature_count()             { return g_world ? (int)g_world->creatures.size() : 0; }
EVO_EXPORT const float* oc_creature_segments() { return g_segments.data(); }
EVO_EXPORT const float* oc_creature_depth()    { return g_depth.data(); }
EVO_EXPORT const float* oc_creature_vz()       { return g_vz.data(); }   // addition, see top
EVO_EXPORT const float* oc_body_params()       { return g_bodyParams.data(); }
EVO_EXPORT const uint8_t* oc_creature_colors() { return g_colors.data(); }
EVO_EXPORT const double* oc_creature_ids()     { return g_ids.data(); }
EVO_EXPORT int oc_food_count()                 { return g_world ? (int)g_world->food.size() : 0; }
EVO_EXPORT const float* oc_food_xy()           { return g_food.data(); }
EVO_EXPORT const float* oc_food_depth()        { return g_foodDepth.data(); }
EVO_EXPORT int oc_predator_count()             { return g_world ? (int)g_world->predators.size() : 0; }
EVO_EXPORT const float* oc_predator_data()     { return g_predators.data(); }
EVO_EXPORT const float* oc_predator_depth()    { return g_predDepth.data(); }
EVO_EXPORT const double* oc_stats()            { return g_stats; }
EVO_EXPORT int oc_stats_len()                  { return kStatsLen; }
EVO_EXPORT int oc_width()                      { return g_world ? g_world->width : 0; }
EVO_EXPORT int oc_height()                     { return g_world ? g_world->height : 0; }
EVO_EXPORT double oc_depth_max()               { return g_world ? (double)g_world->D : 0.0; }

// Change one parameter of the running world: the Ocean knobs listed at the
// top, or any key evo_set_param accepts (read fresh every step). Values must
// be finite and >= 0, and the config they make must pass oc_init's checks AS
// STORED (pitchMax and maxSpeed > 0 after the float conversion, every float
// finite, minSpeed <= maxSpeed; an integer knob must fit an int). Returns 0
// and sets oc_last_error() on refusal, leaving the config unchanged.
EVO_EXPORT int oc_set_param(const char* name, double value) {
    g_error.clear();
    if (!g_world) { g_error = "no world (call oc_init first)"; return 0; }
    static const char* const kOcean[] = {
        "kBuoy", "pitchMax", "vDrag", "wallBand", "buoyMutation", "cHold", "cPress",
        "foodDepthSpread", "eye", "eyeOnly", "eyeCode", "steerFrame", "foodLife",
    };
    static const char* const kLive[] = {
        "maxPopulation", "minPopulation", "targetFood", "foodSpawnBase", "foodSpawnAmp",
        "foodEnergy", "metabolicRate", "moveCost", "geneCost", "maxAge", "reproEnergy",
        "newbornEnergy", "mutationMultiplier", "minSpeed", "maxSpeed",
        "predatorMinPopulation", "predatorMaxPopulation", "predatorTraitMutation",
    };
    static const char* const kLiveInt[] = {   // the int-typed keys of kLive (and kOcean)
        "maxPopulation", "minPopulation", "targetFood", "predatorMinPopulation",
        "predatorMaxPopulation", "eye", "eyeOnly", "eyeCode", "steerFrame",
    };
    bool live = false, isInt = false;
    for (const char* k : kOcean) if (name && std::strcmp(name, k) == 0) { live = true; break; }
    for (const char* k : kLive)  if (name && std::strcmp(name, k) == 0) { live = true; break; }
    for (const char* k : kLiveInt) if (name && std::strcmp(name, k) == 0) { isInt = true; break; }
    if (!live) {
        g_error = std::string("not a live parameter (change it and restart): ") + (name ? name : "(null)");
        return 0;
    }
    if (!std::isfinite(value) || value < 0.0) {
        g_error = std::string(name) + " must be finite and >= 0";
        return 0;
    }
    if (isInt && !(value <= (double)std::numeric_limits<int>::max())) {
        g_error = std::string(name) + " must fit an int";   // the JSON reader's (int) cast
        return 0;
    }
    if (std::strcmp(name, "pitchMax") == 0 && !(value > 0.0)) {
        g_error = "pitchMax must be > 0";
        return 0;
    }
    char num[40];
    std::snprintf(num, sizeof num, "%.9g", value);
    const std::string json = std::string("{\"") + name + "\":" + num + "}";
    SimConfig cfg = g_world->cfg;
    OceanConfig oc = g_world->ocfg;
    std::string err;
    if (!readOceanJson(json.c_str(), cfg, oc, err)) { g_error = "internal: " + err; return 0; }
    clampConfig(cfg);
    // Check what was stored, not the double: 1e-100 is a 0 float, 1e300 an
    // infinite one.
    if (!configOk(cfg, oc, g_world->width, g_world->height)) {
        g_error = std::string(name) + " = " + num + " refused (as stored): " + g_error;
        return 0;
    }
    g_world->cfg = cfg;
    g_world->ocfg = oc;
    return 1;
}

EVO_EXPORT const char* oc_last_error() { return g_error.c_str(); }

// 1 in the eye build (the EYE_* sensors exist; "eye" may be 1), else 0.
EVO_EXPORT int oc_has_eye() { return kOceanHasEye ? 1 : 0; }
