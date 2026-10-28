#pragma once
// ----------------------------------------------------------------------------
// OceanWorld — the 3-D preview world (PLAN_ocean_preview.md). Serial only.
//
// Adapted from Classic's World (src/core/world.hpp) with the thread paths and
// the profiler dropped. (x, y) is Classic's wrapping plane, unchanged; depth is
// a third coordinate in [0, D], clamped (0 = surface). Everything that does not
// depend on geometry is the kernel's (kernel.hpp) and WorldCommon's, called the
// way World calls it; what is here is what depth changes:
//
//   * fish move horizontally exactly as Classic's creatures, then vertically:
//       vz += steerZ·3·dt + kBuoy·(bodyBuoy·D − depth)/D·dt (+ MOVE_RANDOM's
//       third draw, + a soft push inside wallBand of surface / floor);
//       vz −= vz·vDrag·dt; |vz| ≤ pitchMax; depth += vz·speed·dt
//   * depth costs (OceanConfig cHold / cPress) after kernel::stepMetabolism
//   * eating, interaction, sensing and the predators' capture use 3-D
//     distance: the SpatialGrid is still on (x, y), its candidates are
//     filtered by 3-D distance (a shallow sea: candidates per query stay
//     close to Classic's)
//   * five more sensors (LOC_Z, OWN_VEL_Z, NEAREST_FOOD_DZ, AVG_NEIGHBOR_DZ,
//     BOUNDARY_DIST_Z) and one more action (STEER_Z)
//   * predators (Stage A, and the Stage B economy as Classic has it) chase in
//     depth; a child predator buds at its parent's depth ± 8
//   * bodyBuoy: founders are neutral where they are dropped (depth / D);
//     a child gets its parents' mean + one U(±buoyMutation) draw, after
//     kernel::inheritFromPair; a child starts at its parents' midpoint depth
//
// Reused unchanged: SignalGrid (a column: the pheromone layer has no depth),
// CurrentField (horizontal), SpatialGrid, Predator (+ depth, pitch).
//
// The compound eye (build variant -DEVO_OCEAN_EYE, switched by OceanConfig::eye;
// PLAN_fish_patterns §7). One general sense: every object within the
// perception radius R — fish, food, predators — is binned by the dominant
// axis of its offset in the fish's LEVEL frame (forward = the horizontal
// heading, right = its perpendicular, down = depth) into six sectors: ahead,
// behind, left, right, up, down. Per sector, the NEAREST object gives three
// inputs (18 sensors, EYE_*):
//     PROX = 1 − d/R
//     SIZE = s / (s + d)       apparent size s/d, squashed to [0, 1)
//     CON  = b − L(z)          brightness against the ambient light at the
//                              object's depth, L(z) = exp(−kEyeWater·z)
// Objects differ only in (s, b): food glows (small, b = 1), predators are
// dark and large (b = 0.05), fish are b = 0.5 and s = bodySize x the body
// length (a placeholder until patterns give fish an appearance). The eye is
// computed inside the existing neighbour, food and predator loops — no extra
// queries — and draws no random numbers. Off (or in the plain Ocean build)
// the 18 inputs read 0, the same as six empty sectors.
//
// Eye-only mode (OceanConfig::eyeOnly, the eye build only): the 23 LABELED
// exteroceptive senses the eye replaces (ocean::kLabeledSenses) read 0 and are
// not computed:
//     food:       NEAREST_FOOD_DX/DY/DZ, NEAREST_FOOD_DIST, FOOD_DENSITY
//     predators:  PREDATOR_FWD/LR/PROX
//     neighbours: NEIGHBOR_COUNT, AVG_NEIGHBOR_DX/DY/DZ,
//                 AVG_NEIGHBOR_HEADING_X/Y, NEAREST_NEIGHBOR_DIST,
//                 POPULATION_DENSITY/FWD/LR, LONGPROBE_POP_FWD,
//                 NEAREST_NEIGHBOR_AGE/FOOD, GENETIC_SIM_NEAREST/FWD
// Kept (23): own state and proprioception (LOC_X/Y/Z, BOUNDARY_DIST_X/Y/Z,
// BOUNDARY_DIST, OWN_VEL_X/Y/Z, OWN_SPEED, LAST_MOVE_DIR_X/Y, AGE,
// FOOD_RESERVES, OSCILLATOR, RESPONSIVENESS), the non-visual senses
// (SIGNAL0/_FWD/_LR, CURRENT_FWD/LR) and RANDOM_SENSOR; plus the 18 EYE_*
// inputs when the eye is on (eye 0 + eyeOnly 1 = "blind"). The eye sees the
// same objects in the same order as with the labels on, so its 18 inputs are
// bit-identical between the two modes for the same state. The pairwise
// interaction vote (kernel::interactionVote) builds its own contact inputs and
// is unchanged. No draws are added or removed (RANDOM_SENSOR is still drawn).
//
// Build note: the eye build has 64 sensors, so genes decode with % 64 instead
// of % 46 and its colour / SimHash planes are its own (STATS_DIM 1900); that is
// a different genetics from the plain Ocean build even with the eye off, which
// is why it is a separate build and the plain one is untouched.
//
// -DEVO_OCEAN_PROF adds a phase profiler (oprof: sensor gather, feedForward,
// movement + metabolism, the grid build and the serial phase) for the eye
// benchmark (tools/ocean_eye_bench.cpp); it changes no state.
// ----------------------------------------------------------------------------
#ifndef EVO_WORLD_OCEAN
#error "ocean_world.hpp needs the Ocean sensor list: compile with -DEVO_WORLD_OCEAN"
#endif
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <vector>
#ifdef EVO_OCEAN_PROF
#include <chrono>
#endif
#include "../core/world.hpp"   // SignalGrid, MAX_TOPOLOGICAL_NEIGHBORS, and through it the
                               // kernel, WorldCommon, SpatialGrid, CurrentField, Predator
#include "ocean_config.hpp"
#include "fish.hpp"

struct OceanFood {
    Vec2  pos;
    float depth = 0.0f;
    bool  alive = true;
#ifdef EVO_OCEAN_EYE
    float eyeCon = 0.0f;   // what the eye sees: glow − ambient light at its depth
                           // (set at spawn; food never changes depth)
#endif
};

struct OceanPredator : Predator {
    float depth = 0.0f;
    float pitch = 0.0f;   // wander: depth rate / speed, in [-0.3, 0.3]
};

namespace ocean {

// Names in enum order — Ocean's own copy (Classic's wiring::kSensorNames is
// sized by Classic's list and is not part of this build).
inline constexpr const char* kSensorNames[] = {
    "LOC_X","LOC_Y","BOUNDARY_DIST_X","BOUNDARY_DIST_Y","BOUNDARY_DIST",
    "OWN_VEL_X","OWN_VEL_Y","OWN_SPEED","LAST_MOVE_DIR_X","LAST_MOVE_DIR_Y",
    "AGE","FOOD_RESERVES","OSCILLATOR","RESPONSIVENESS",
    "NEIGHBOR_COUNT","AVG_NEIGHBOR_DX","AVG_NEIGHBOR_DY",
    "AVG_NEIGHBOR_HEADING_X","AVG_NEIGHBOR_HEADING_Y","NEAREST_NEIGHBOR_DIST",
    "POPULATION_DENSITY","POPULATION_FWD","POPULATION_LR",
    "NEAREST_NEIGHBOR_AGE","NEAREST_NEIGHBOR_FOOD","GENETIC_SIM_NEAREST","GENETIC_SIM_FWD",
    "NEAREST_FOOD_DX","NEAREST_FOOD_DY","NEAREST_FOOD_DIST","FOOD_DENSITY",
    "SIGNAL0","SIGNAL0_FWD","SIGNAL0_LR","LONGPROBE_POP_FWD",
    "CURRENT_FWD","CURRENT_LR",
    "PREDATOR_FWD","PREDATOR_LR","PREDATOR_PROX","RANDOM_SENSOR",
    "LOC_Z","OWN_VEL_Z","NEAREST_FOOD_DZ","AVG_NEIGHBOR_DZ","BOUNDARY_DIST_Z",
#ifdef EVO_OCEAN_EYE
    "EYE_AHEAD_PROX","EYE_AHEAD_SIZE","EYE_AHEAD_CON",
    "EYE_BEHIND_PROX","EYE_BEHIND_SIZE","EYE_BEHIND_CON",
    "EYE_LEFT_PROX","EYE_LEFT_SIZE","EYE_LEFT_CON",
    "EYE_RIGHT_PROX","EYE_RIGHT_SIZE","EYE_RIGHT_CON",
    "EYE_UP_PROX","EYE_UP_SIZE","EYE_UP_CON",
    "EYE_DOWN_PROX","EYE_DOWN_SIZE","EYE_DOWN_CON",
#endif
};
static_assert(sizeof(kSensorNames) / sizeof(kSensorNames[0]) == NUM_SENSES,
              "ocean::kSensorNames is out of sync with the Sensor enum");
inline constexpr const char* kActionNames[] = {
    "STEER_X","STEER_Y","MOVE_FORWARD","MOVE_RANDOM","SPEED_ADJUST","EMIT_SIGNAL0",
    "SET_OSCILLATOR_PERIOD","SET_RESPONSIVENESS",
    "INTERACT_SHARE","INTERACT_REPRODUCE","INTERACT_FIGHT","INTERACT_IGNORE","STEER_Z",
};
static_assert(sizeof(kActionNames) / sizeof(kActionNames[0]) == NUM_ACTIONS,
              "ocean::kActionNames is out of sync with the Action enum");

// Does sensor s drive action a through the wired net, directly or through any
// chain of hidden neurons? (Topology only; weights are not inspected.)
inline bool sensorReachesAction(const NeuralNet& nn, int s, int a) {
    bool reach[MAX_INTERNAL_NEURONS] = {};
    for (bool changed = true; changed;) {
        changed = false;
        for (const NeuralNet::FlatConn& fc : nn.flat) {
            const int src = fc.src(), sink = fc.sink();
            const bool on = (src == s) || (src >= NUM_SENSES && reach[src - NUM_SENSES]);
            if (!on) continue;
            if (sink == a) return true;
            if (sink >= NUM_ACTIONS && !reach[sink - NUM_ACTIONS]) {
                reach[sink - NUM_ACTIONS] = true;
                changed = true;
            }
        }
    }
    return false;
}

// Direct sensor -> action wire only.
inline bool sensorWiresAction(const NeuralNet& nn, int s, int a) {
    for (const NeuralNet::FlatConn& fc : nn.flat)
        if (fc.src() == s && fc.sink() == a) return true;
    return false;
}

constexpr int kDepthBins = 8;

#ifdef EVO_OCEAN_EYE
// ---- the compound eye's constants (see the header comment) -----------------
constexpr int   kEyeSectors    = 6;        // ahead, behind, left, right, up, down
constexpr int   kEyeFirst      = EYE_AHEAD_PROX;
constexpr int   kEyeInputs     = 3 * kEyeSectors;
static_assert(EYE_DOWN_CON == kEyeFirst + kEyeInputs - 1, "eye sensors must be contiguous");
constexpr float kEyeWater      = 0.01f;    // ambient light L(z) = exp(−kEyeWater·z): 0.09 at z = 240
constexpr float kEyeFishLen    = 20.0f;    // a fish's size in world units per unit bodySize
                                           // (its 6-point spine: 5 segments of 4)
constexpr float kEyeFishBright = 0.5f;     // placeholder until patterns exist
constexpr float kEyeFoodSize   = 4.0f;     // food glows: small and bright
constexpr float kEyeFoodBright = 1.0f;
constexpr float kEyePredSize   = 60.0f;    // predators: large and dark
constexpr float kEyePredBright = 0.05f;

// L(z) = exp(−kEyeWater·z), through evomath::expm1f (bit-identical native / wasm).
inline float eyeAmbient(float z) { return 1.0f + evomath::expm1f(-kEyeWater * z); }
inline float eyeContrast(float bright, float z) { return bright - eyeAmbient(z); }

// The labeled exteroceptive senses eye-only mode zeroes (OceanConfig::eyeOnly;
// see the header comment). Everything else that is not EYE_* is kept.
inline constexpr int kLabeledSenses[] = {
    NEAREST_FOOD_DX, NEAREST_FOOD_DY, NEAREST_FOOD_DIST, FOOD_DENSITY, NEAREST_FOOD_DZ,
    PREDATOR_FWD, PREDATOR_LR, PREDATOR_PROX,
    NEIGHBOR_COUNT, AVG_NEIGHBOR_DX, AVG_NEIGHBOR_DY, AVG_NEIGHBOR_DZ,
    AVG_NEIGHBOR_HEADING_X, AVG_NEIGHBOR_HEADING_Y, NEAREST_NEIGHBOR_DIST,
    POPULATION_DENSITY, POPULATION_FWD, POPULATION_LR, LONGPROBE_POP_FWD,
    NEAREST_NEIGHBOR_AGE, NEAREST_NEIGHBOR_FOOD, GENETIC_SIM_NEAREST, GENETIC_SIM_FWD,
};
inline constexpr int kNumLabeledSenses = (int)(sizeof(kLabeledSenses) / sizeof(kLabeledSenses[0]));
static_assert(kNumLabeledSenses == 23, "eye-only mode's labeled-sense list changed");
inline constexpr bool isLabeledSense(int s) {
    for (int l : kLabeledSenses) if (l == s) return true;
    return false;
}
inline constexpr bool isEyeSense(int s) { return s >= kEyeFirst && s < kEyeFirst + kEyeInputs; }
#endif

}  // namespace ocean

class OceanWorld : public WorldCommon {
public:
    int width = 0, height = 0;
    float D = 240.0f;                       // == ocfg.depth
    OceanConfig ocfg;
    std::vector<Fish> creatures;
    std::vector<OceanPredator> predators;
    std::vector<OceanFood> food;
    SignalGrid signals;
    long long totalReseeds = 0;             // founders the minPopulation floor added (not births)
    long long totalFoodEaten = 0;           // food items eaten by fish (a statistic: not
                                            // part of the fingerprint, changes nothing)

    static constexpr float LONGPROBE_DIST = 200.0f;
    static constexpr float LONGPROBE_RADIUS = 30.0f;   // the probe's horizontal reach

    // Every radius this world queries or tests with must be finite and >= 0.
    // In a toroidal world each one must also be < min(w, h) / 2: that is the
    // bound SpatialGrid::queryRadiusWrapped / forEachInRadiusWrapped assume
    // (spatial_grid.hpp) — a wider disk overlaps its own image across the seam
    // and lists the same entity twice. Capture is a minimum-image distance
    // test, not a grid query, and is held to the same bound. Returns "" when a
    // w x h world fits them all, else why not.
    static std::string radiusError(const SimConfig& c, int w, int h) {
        const struct { const char* name; float r; } radii[] = {
            {"perceptionRadius", c.perceptionRadius},
            {"interactRadius", c.interactRadius},
            {"pickupRadius", c.pickupRadius},
            {"predatorPerception", c.predatorPerception},
            {"predatorCaptureRadius", c.predatorCaptureRadius},
            {"the long probe radius", LONGPROBE_RADIUS},
        };
        for (const auto& e : radii) {
            if (!std::isfinite(e.r) || e.r < 0.0f)
                return std::string(e.name) + " must be finite and >= 0";
            if (c.toroidal && !(2.0f * e.r < (float)std::min(w, h)))
                return std::string(e.name) + " " + std::to_string(e.r) +
                       " needs a toroidal world wider and taller than twice it (got " +
                       std::to_string(w) + " x " + std::to_string(h) + ")";
        }
        return "";
    }

    static float wrapDelta(float d, float span) {
        const float half = 0.5f * span;
        if (d > half) return d - span;
        if (d < -half) return d + span;
        return d;
    }
    float wrapDx(float d) const { return wrapDelta(d, (float)width); }
    float wrapDy(float d) const { return wrapDelta(d, (float)height); }
    static float wrapCoord(float v, float span) {
        while (v < 0.0f) v += span;
        while (v >= span) v -= span;
        return v;
    }
    float clampDepth(float z) const { return std::clamp(z, 0.0f, D); }

    // Set cfg and ocfg first (ocfg.depth is overwritten by `depth`).
    void init(int w, int h, float depth, int numCreatures) {
        width = w;
        height = h;
        D = depth;
        ocfg.depth = depth;
        signals.init(w, h, 8, cfg.toroidal);
        creatureGrid_.init(w, h, cfg.perceptionRadius);
        foodGrid_.init(w, h, cfg.perceptionRadius);

        CurrentField::Mode cmode = (cfg.currentMode == 1) ? CurrentField::Mode::Gyre
                                                           : CurrentField::Mode::Eddies;
        current_.init(cmode, w, h, cfg.currentEddies, cfg.currentStrength,
                      cfg.currentCore, cfg.currentDrift, cfg.currentGyreEps);
        foodPatches_.clear();
        foodPatchDepth_.clear();
        for (int i = 0; i < cfg.foodPatches; ++i) {
            foodPatches_.push_back({rng.range(0.0f, (float)w), rng.range(0.0f, (float)h)});
            if (ocfg.foodDepthMode == 1) foodPatchDepth_.push_back(rng.range(0.0f, D));
        }

        creatures.clear();
        creatures.reserve(numCreatures * 2);
        for (int i = 0; i < numCreatures; ++i) addFounder();

        predators.clear();
        for (int i = 0; i < cfg.predatorCount; ++i) predators.push_back(makePredator());

        food.clear();
        for (int i = 0; i < cfg.targetFood; ++i) spawnFood();
        totalReseeds = 0;
        totalFoodEaten = 0;
    }

    void spawnFood() {
        OceanFood f;
        int patch = -1;
        if (!foodPatches_.empty()) {
            patch = (int)rng.uint((uint32_t)foodPatches_.size() - 1);
            const Vec2& c = foodPatches_[patch];
            const float R = cfg.foodPatchRadius;
            if (cfg.toroidal) {
                f.pos = { wrapCoord(c.x + rng.range(-R, R), (float)width),
                          wrapCoord(c.y + rng.range(-R, R), (float)height) };
            } else {
                f.pos = { std::clamp(c.x + rng.range(-R, R), 0.0f, (float)width),
                          std::clamp(c.y + rng.range(-R, R), 0.0f, (float)height) };
            }
        } else {
            f.pos = {rng.range(10.0f, (float)width - 10.0f),
                     rng.range(10.0f, (float)height - 10.0f)};
        }
        if (ocfg.foodDepthMode == 1 && patch >= 0 && patch < (int)foodPatchDepth_.size()) {
            const float S = ocfg.foodDepthSpread;
            f.depth = clampDepth(foodPatchDepth_[patch] + rng.range(-S, S));
        } else {
            f.depth = rng.range(0.0f, D);
        }
#ifdef EVO_OCEAN_EYE
        f.eyeCon = ocean::eyeContrast(ocean::kEyeFoodBright, f.depth);   // no draws
#endif
        food.push_back(f);
    }

    void update(float dt) {
#ifdef EVO_OCEAN_PROF
        using pclk = std::chrono::steady_clock;
        const auto tStep0 = pclk::now();
#endif
        frame++;
        births = 0;
        deaths = 0;

        current_.advance(frame);

        if (cfg.foodDrift && current_.active()) {
            for (auto& f : food) {
                if (!f.alive) continue;
                Vec2 v = current_.velocityAt(f.pos);
                f.pos.x += v.x * dt;  f.pos.y += v.y * dt;
                if (f.pos.x < 0) f.pos.x += width;  else if (f.pos.x >= width)  f.pos.x -= width;
                if (f.pos.y < 0) f.pos.y += height; else if (f.pos.y >= height) f.pos.y -= height;
            }
        }

        float foodRate = cfg.foodSpawnBase + cfg.foodSpawnAmp * evomath::sinf(frame * 0.001f);
        for (int i = 0; i < static_cast<int>(foodRate); ++i) {
            if (static_cast<int>(food.size()) < cfg.targetFood * 2) spawnFood();
        }

        if (frame % 3 == 0) signals.decay();

        creatureGrid_.clear();
        foodGrid_.clear();
        const int numCreatures = (int)creatures.size();
        const int numFood = (int)food.size();
        hot_.resize(numCreatures);
        for (int i = 0; i < numCreatures; ++i) {
            const Fish& c = creatures[i];
            hot_[i] = {c.pos, c.vel, c.depth};
            if (c.alive) creatureGrid_.insert(c.pos, i);
        }
        for (int i = 0; i < numFood; ++i)
            if (food[i].alive) foodGrid_.insert(food[i].pos, i);
        creatureGrid_.build();
        foodGrid_.build();

        pending_.assign(numCreatures, PendingSignal{});
#ifdef EVO_OCEAN_EYE
        if (ocfg.eye) eyePrepare();
#endif
#ifdef EVO_OCEAN_PROF
        const auto tFish0 = pclk::now();
        oprof.buildNs += std::chrono::duration<double, std::nano>(tFish0 - tStep0).count();
#endif

        // ---- per-fish phase: sensors, network, movement, metabolism ----------
#ifdef EVO_OCEAN_EYE
        if (ocfg.eyeOnly) {   // labels off: the eye alone, or nothing ("blind")
            if (ocfg.eye) { for (int i = 0; i < numCreatures; ++i) stepFish<true, false>(i, dt); }
            else          { for (int i = 0; i < numCreatures; ++i) stepFish<false, false>(i, dt); }
        } else
        if (ocfg.eye) { for (int i = 0; i < numCreatures; ++i) stepFish<true>(i, dt); }
        else
#endif
        for (int i = 0; i < numCreatures; ++i) stepFish<false>(i, dt);
#ifdef EVO_OCEAN_PROF
        const auto tSerial0 = pclk::now();
        oprof.fishPhaseNs += std::chrono::duration<double, std::nano>(tSerial0 - tFish0).count();
#endif

        // ---- serial phase: signals, deaths, eating, interaction --------------
        std::vector<Fish> newborns;
        const float pr2 = cfg.pickupRadius * cfg.pickupRadius;
        const float ir2 = cfg.interactRadius * cfg.interactRadius;
        for (int i = 0; i < numCreatures; ++i) {
            Fish& c = creatures[i];
            if (!c.alive) continue;

            if (pending_[i].wants) signals.emit(pending_[i].pos);

            if (const char cause = kernel::deathCause(c, cfg)) {
                recordDeath(c, cause);
                continue;
            }

            nearbyFood_.clear();
            if (cfg.toroidal) foodGrid_.queryRadiusWrapped(c.pos, cfg.pickupRadius, nearbyFood_);
            else              foodGrid_.queryRadius(c.pos, cfg.pickupRadius, nearbyFood_);
            for (int fi : nearbyFood_) {
                OceanFood& f = food[fi];
                if (!f.alive) continue;
                if (dist2(c.pos, c.depth, f.pos, f.depth) > pr2) continue;
                f.alive = false;
                c.foodReserves += cfg.foodEnergy;
                ++totalFoodEaten;
            }

            if (c.interactTimer <= 0.0f) {
                nearby_.clear();
                if (cfg.toroidal) creatureGrid_.queryRadiusWrapped(c.pos, cfg.interactRadius, nearby_);
                else              creatureGrid_.queryRadius(c.pos, cfg.interactRadius, nearby_);
                for (int ni : nearby_) {
                    if (ni == i) continue;
                    Fish& other = creatures[ni];
                    if (!other.alive || other.interactTimer > 0.0f) continue;
                    if (dist2(c.pos, c.depth, other.pos, other.depth) > ir2) continue;
                    doInteraction(c, other, newborns);
                    c.interactTimer = cfg.interactCooldown;
                    other.interactTimer = cfg.interactCooldown;
                    break;
                }
            }
        }

        for (auto& baby : newborns) { baby.id = nextId_++; creatures.push_back(std::move(baby)); }

        updatePredators(dt);
        if (cfg.predatorEconomy && cfg.predatorMinPopulation > 0) refillPredators();

        creatures.erase(std::remove_if(creatures.begin(), creatures.end(),
                                       [](const Fish& c) { return !c.alive; }),
                        creatures.end());
        food.erase(std::remove_if(food.begin(), food.end(),
                                  [](const OceanFood& f) { return !f.alive; }),
                   food.end());

        if ((int)creatures.size() < cfg.minPopulation) {
            const int toAdd = cfg.minPopulation - (int)creatures.size();
            for (int j = 0; j < toAdd; ++j) { addFounder(); totalReseeds++; }
        }
#ifdef EVO_OCEAN_PROF
        const auto tEnd = pclk::now();
        oprof.serialNs += std::chrono::duration<double, std::nano>(tEnd - tSerial0).count();
        oprof.totalNs  += std::chrono::duration<double, std::nano>(tEnd - tStep0).count();
        oprof.steps++;
        oprof.fishSteps += numCreatures;
#endif
    }

#ifdef EVO_OCEAN_PROF
    // Wall time per phase, summed over steps (single-threaded, so the per-fish
    // split sens + ff + move is a share of fishPhaseNs; the clock calls in it
    // cost a little, which is why it is a separate build).
    struct OceanProf {
        double buildNs = 0, fishPhaseNs = 0, serialNs = 0, totalNs = 0;
        double sensNs = 0, ffNs = 0, moveNs = 0;   // inside the per-fish phase
        long long steps = 0, fishSteps = 0;
    } oprof;
#endif

    bool currentActive() const { return current_.active(); }
    Vec2 currentAt(Vec2 p) const { return current_.velocityAt(p); }

    // Mean and SD of the live fish's depth, and the fraction per depth eighth
    // (surface first). Read-only.
    void depthStats(double& mean, double& sd, double hist[ocean::kDepthBins]) const {
        for (int b = 0; b < ocean::kDepthBins; ++b) hist[b] = 0.0;
        double s = 0.0, s2 = 0.0;
        long n = 0;
        for (const Fish& c : creatures) {
            if (!c.alive) continue;
            const double z = c.depth;
            s += z; s2 += z * z; ++n;
            int b = (int)(z / D * ocean::kDepthBins);
            hist[std::clamp(b, 0, ocean::kDepthBins - 1)] += 1.0;
        }
        mean = n ? s / n : 0.0;
        sd = n ? std::sqrt(std::max(0.0, s2 / n - mean * mean)) : 0.0;
        if (n) for (int b = 0; b < ocean::kDepthBins; ++b) hist[b] /= (double)n;
    }

private:
    struct FishHot {
        Vec2 pos; Vec2 vel; float depth;
#ifdef EVO_OCEAN_EYE
        float eyeSize, eyeCon;   // how the eye sees this fish (set only while it is on)
#endif
    };
    struct PendingSignal { bool wants = false; Vec2 pos; };
    struct Candidate { float distSq, dx, dy, dz; int index; };

    SpatialGrid creatureGrid_;
    SpatialGrid foodGrid_;
    std::vector<FishHot> hot_;
    std::vector<PendingSignal> pending_;
    std::vector<int> neighbors_, nearbyFood_, nearby_, near_;
    CurrentField current_;
    std::vector<Vec2> foodPatches_;
    std::vector<float> foodPatchDepth_;   // foodDepthMode 1: one depth per patch

#ifdef EVO_OCEAN_EYE
    // ---- the compound eye ---------------------------------------------------
    std::vector<float> predCon_;   // per predator, this step: dark − ambient light

    struct EyeBin { float d2, size, con; };   // the nearest object in one sector

    // The step's looks, before the per-fish phase: every fish's size and
    // contrast (stepFish refreshes its own after it moves, as hot_ does), and
    // every predator's contrast (predators move after the per-fish phase).
    void eyePrepare() {
        for (std::size_t i = 0; i < creatures.size(); ++i) {
            hot_[i].eyeSize = ocean::kEyeFishLen * creatures[i].bodySize;
            hot_[i].eyeCon  = ocean::eyeContrast(ocean::kEyeFishBright, hot_[i].depth);
        }
        predCon_.resize(predators.size());
        for (std::size_t k = 0; k < predators.size(); ++k)
            predCon_[k] = ocean::eyeContrast(ocean::kEyePredBright, predators[k].depth);
    }

    // One object at offset (dx, dy, dz), d2 = its squared distance (<= R²): its
    // sector is the dominant axis of the offset in the level frame (fwd, rht,
    // depth), and it replaces the sector's object if strictly nearer (ties: the
    // first seen, so the result follows the deterministic visit order).
    static void eyeSee(EyeBin* eye, Vec2 fwd, Vec2 rht, float dx, float dy, float dz, float d2,
                       float size, float con) {
        const float a = dx * fwd.x + dy * fwd.y;   // + ahead
        const float b = dx * rht.x + dy * rht.y;   // + right (as PREDATOR_LR)
        const float aa = std::abs(a), ab = std::abs(b), az = std::abs(dz);
        // (A branch-free form of this pick — selects for the sector and for the
        // nearest-so-far update — gives the same bits but measured no faster:
        // +1-2 points of step time natively, +4-7 under wasm; 2026-10-27.)
        int k;
        if (aa >= ab && aa >= az) k = (a >= 0.0f) ? 0 : 1;    // ahead / behind
        else if (ab >= az)        k = (b <= 0.0f) ? 2 : 3;    // left / right
        else                      k = (dz <= 0.0f) ? 4 : 5;   // up (shallower) / down
        EyeBin& e = eye[k];
        if (d2 < e.d2) { e.d2 = d2; e.size = size; e.con = con; }
    }

    static void eyeWrite(const EyeBin* eye, float R, float sensors[]) {
        const float R2 = R * R;
        for (int k = 0; k < ocean::kEyeSectors; ++k) {
            float* s = sensors + ocean::kEyeFirst + 3 * k;
            const EyeBin& e = eye[k];
            if (!(e.d2 <= R2)) { s[0] = s[1] = s[2] = 0.0f; continue; }   // empty sector
            const float d = std::sqrt(e.d2);
            s[0] = 1.0f - d / R;            // proximity
            s[1] = e.size / (e.size + d);   // apparent size s/d, squashed: x / (1 + x)
            s[2] = e.con;                   // brightness contrast
        }
    }
#endif

    // Squared 3-D distance; (x, y) by minimum image when the senses wrap.
    float dist2(Vec2 a, float za, Vec2 b, float zb) const {
        float dx = b.x - a.x, dy = b.y - a.y;
        if (cfg.toroidal) { dx = wrapDx(dx); dy = wrapDy(dy); }
        const float dz = zb - za;
        return dx * dx + dy * dy + dz * dz;
    }

    void addFounder() {
        Vec2 pos = {rng.range(10.0f, (float)width - 10.0f),
                    rng.range(10.0f, (float)height - 10.0f)};
        const float z = rng.range(0.0f, D);
        creatures.push_back(makeRandomFish(pos, z, D, cfg.genomeInitMin, cfg.genomeInitMax));
        markFounder(creatures.back());
    }

    OceanPredator makePredator() {
        OceanPredator p;
        p.pos = {rng.range(10.0f, (float)width - 10.0f),
                 rng.range(10.0f, (float)height - 10.0f)};
        const float a = rng.range(0.0f, 6.2832f);
        p.vel = {evomath::cosf(a), evomath::sinf(a)};
        p.depth = rng.range(0.0f, D);
        p.energy = cfg.predatorNewbornEnergy;
        return p;
    }

    // Eye: the compound eye is on this step (only ever true in the eye build).
    // Labels: the labeled exteroceptive senses are computed (false only in the
    // eye build's eye-only mode, OceanConfig::eyeOnly).
    template <bool Eye, bool Labels = true>
    void stepFish(int i, float dt) {
        Fish& c = creatures[i];
        if (!c.alive) return;
#ifdef EVO_OCEAN_PROF
        using pclk = std::chrono::steady_clock;
        const auto p0 = pclk::now();
#endif

        float sensors[NUM_SENSES];
        std::memset(sensors, 0, sizeof(sensors));
        computeSensors<Eye, Labels>(c, i, sensors);
#ifdef EVO_OCEAN_PROF
        const auto p1 = pclk::now();
#endif

        float actions[NUM_ACTIONS];
        std::memset(actions, 0, sizeof(actions));
        feedForward(c.nnet, sensors, actions, cfg.fastTanh);
#ifdef EVO_OCEAN_PROF
        const auto p2 = pclk::now();
        oprof.sensNs += std::chrono::duration<double, std::nano>(p1 - p0).count();
        oprof.ffNs   += std::chrono::duration<double, std::nano>(p2 - p1).count();
#endif

        if (cfg.captureActivations) {
            std::memcpy(c.lastSensors, sensors, sizeof(sensors));
            std::memcpy(c.lastActions, actions, sizeof(actions));
        }

        const float resp = c.responsiveness;
        const float effMax = std::max(cfg.minSpeed, cfg.maxSpeed * speedFactor(c));
        float rndZ = 0.0f;   // MOVE_RANDOM's third draw, applied to vz below

        // ---- horizontal: Classic's two motion models, unchanged ----
        if (cfg.triangularMotion) {
            float drive = std::clamp(0.5f * (actions[MOVE_FORWARD] + 1.0f), 0.0f, 1.0f);
            float turn  = actions[STEER_X];
            float turnEff = turn * (1.0f - drive) * resp;
            float theta = evomath::atan2f(c.vel.y, c.vel.x);
            theta += turnEff * (cfg.maxTurnRate * turnFactor(c)) * dt;
            c.vel.x = evomath::cosf(theta);
            c.vel.y = evomath::sinf(theta);
            c.speed = std::clamp(cfg.minSpeed + drive * (effMax - cfg.minSpeed),
                                 cfg.minSpeed, effMax);
            c.lastMoveDir = c.vel;
            c.pos.x += c.vel.x * c.speed * dt;
            c.pos.y += c.vel.y * c.speed * dt;
        } else {
            float steerX = actions[STEER_X] * resp;
            float steerY = actions[STEER_Y] * resp;
            float moveFwd = actions[MOVE_FORWARD] * resp;
            float moveRnd = actions[MOVE_RANDOM] * resp;
            float speedAdj = actions[SPEED_ADJUST] * resp;

            c.vel.x += steerX * 3.0f * dt;
            c.vel.y += steerY * 3.0f * dt;
            auto fwd = c.forward();
            c.vel.x += fwd.x * moveFwd * 2.0f * dt;
            c.vel.y += fwd.y * moveFwd * 2.0f * dt;
            if (std::abs(moveRnd) > 0.5f) {
                c.vel.x += rng.range(-1.0f, 1.0f) * std::abs(moveRnd) * dt;
                c.vel.y += rng.range(-1.0f, 1.0f) * std::abs(moveRnd) * dt;
                rndZ = rng.range(-1.0f, 1.0f) * std::abs(moveRnd) * dt;
            }
            float vlen = std::sqrt(c.vel.x * c.vel.x + c.vel.y * c.vel.y);
            if (vlen > 0.001f) { c.vel.x /= vlen; c.vel.y /= vlen; }
            c.speed += speedAdj * 50.0f * dt;
            c.speed = std::clamp(c.speed, cfg.minSpeed, effMax);
            c.lastMoveDir = c.vel;
            c.pos.x += c.vel.x * c.speed * dt;
            c.pos.y += c.vel.y * c.speed * dt;
        }

        // ---- vertical: steering, buoyancy, soft walls, drag ----
        {
            float vz = c.vz + actions[STEER_Z] * resp * 3.0f * dt
                     + ocfg.kBuoy * (c.bodyBuoy * D - c.depth) / D * dt + rndZ;
            // Each wall's band is capped at half the depth: in a sea shallower
            // than 2·wallBand the two bands would overlap and the else-if
            // below would apply only the surface push. min() returns wallBand
            // itself whenever D >= 2·wallBand, so a normal sea is unchanged.
            const float wb = std::min(ocfg.wallBand, 0.5f * D);
            if (wb > 0.0f) {
                if (c.depth < wb)           vz += (1.0f - c.depth / wb) * 3.0f * dt;
                else if (c.depth > D - wb)  vz -= (1.0f - (D - c.depth) / wb) * 3.0f * dt;
            }
            vz -= vz * ocfg.vDrag * dt;
            vz = std::clamp(vz, -ocfg.pitchMax, ocfg.pitchMax);
            c.depth += vz * c.speed * dt;
            if (c.depth < 0.0f)  { c.depth = 0.0f; if (vz < 0.0f) vz = 0.0f; }
            else if (c.depth > D) { c.depth = D;   if (vz > 0.0f) vz = 0.0f; }
            c.vz = vz;
        }

        if (current_.active()) {
            Vec2 cur = current_.velocityAt(c.pos);
            c.pos.x += cur.x * dt;
            c.pos.y += cur.y * dt;
        }
        if (c.pos.x < 0) c.pos.x += width;
        if (c.pos.x >= width) c.pos.x -= width;
        if (c.pos.y < 0) c.pos.y += height;
        if (c.pos.y >= height) c.pos.y -= height;
        hot_[i] = {c.pos, c.vel, c.depth};
#ifdef EVO_OCEAN_EYE
        if constexpr (Eye) {
            hot_[i].eyeSize = ocean::kEyeFishLen * c.bodySize;
            hot_[i].eyeCon  = ocean::eyeContrast(ocean::kEyeFishBright, c.depth);
        }
#endif

        c.updateBody();

        kernel::stepInternalState(c, actions, dt);
        kernel::stepMetabolism(c, cfg, dt);
        // Depth costs (0 by default): holding off neutral depth, and pressure.
        if (ocfg.cHold != 0.0f || ocfg.cPress != 0.0f)
            c.foodReserves -= dt * (ocfg.cHold * std::abs(c.depth - c.bodyBuoy * D) / D
                                    + ocfg.cPress * c.depth / D);

        if (actions[EMIT_SIGNAL0] > 0.0f) pending_[i] = {true, c.pos};
#ifdef EVO_OCEAN_PROF
        oprof.moveNs += std::chrono::duration<double, std::nano>(pclk::now() - p2).count();
#endif
    }

    void doInteraction(Fish& c1, Fish& c2, std::vector<Fish>& newborns) {
        switch (kernel::interactionVote(c1, c2)) {
        case kernel::Interaction::Share:
            kernel::shareReserves(c1, c2);
            break;
        case kernel::Interaction::Pair: {
            bool viaGrace = false, viaSoft = false;
            if (kernel::pairingAllowed(c1, c2, cfg,
                                       (int)creatures.size() + (int)newborns.size()) &&
                kernel::reproGatePasses(c1, c2, cfg, viaGrace, viaSoft)) {
                kernel::payForPairing(c1, c2, cfg);
                newborns.push_back(makeChildFish(c1, c2, cfg.genomeMinLength,
                                                 cfg.genomeMaxLength,
                                                 cfg.mutationMultiplier,
                                                 cfg.newbornEnergy));
                Fish& baby = newborns.back();
                if (cfg.toroidal) {
                    Vec2 m = { wrapCoord(c1.pos.x + 0.5f * wrapDx(c2.pos.x - c1.pos.x), (float)width),
                               wrapCoord(c1.pos.y + 0.5f * wrapDy(c2.pos.y - c1.pos.y), (float)height) };
                    baby.pos = m;
                    for (int bi = 0; bi < Fish::BODY_LEN; ++bi)
                        baby.body[bi] = { m.x - baby.vel.x * bi * 4.0f,
                                          m.y - baby.vel.y * bi * 4.0f };
                }
                kernel::inheritFromPair(baby, c1, c2, cfg);
                const float m = ocfg.buoyMutation;
                baby.bodyBuoy = std::clamp(0.5f * (c1.bodyBuoy + c2.bodyBuoy) + rng.range(-m, m),
                                           0.0f, 1.0f);
                recordPairing(c1, c2, viaGrace, viaSoft);
            }
            break;
        }
        case kernel::Interaction::Fight:
            kernel::fightOutcome(c1, c2);
            break;
        default: break;
        }
    }

    // The pheromone column and the current: horizontal, as Classic (kept in
    // eye-only mode: they are not what the eye replaces).
    void writeColumnSensors(const Fish& c, Vec2 fwd, Vec2 rht, float sensors[]) {
        sensors[SIGNAL0] = signals.read(c.pos);
        sensors[SIGNAL0_FWD] = signals.readInDir(c.pos, fwd) - signals.readInDir(c.pos, {-fwd.x, -fwd.y});
        sensors[SIGNAL0_LR] = signals.readInDir(c.pos, rht) - signals.readInDir(c.pos, {-rht.x, -rht.y});
        if (current_.active()) {
            Vec2 cur = current_.velocityAt(c.pos);
            sensors[CURRENT_FWD] = evomath::tanhf((cur.x * fwd.x + cur.y * fwd.y) / 100.0f);
            sensors[CURRENT_LR]  = evomath::tanhf((cur.x * rht.x + cur.y * rht.y) / 100.0f);
        } else {
            sensors[CURRENT_FWD] = 0.0f;
            sensors[CURRENT_LR]  = 0.0f;
        }
    }

#ifdef EVO_OCEAN_EYE
    // Eye-only mode: the eye's half of computeSensors' neighbour, food and
    // predator loops — the same queries, every object within R in the same
    // visit order (so the same 18 inputs, ties included), none of the labeled
    // sums, the topological cut, the nearest-neighbour genome compare or the
    // long probe.
    void eyeScan(const Fish& c, int selfIdx, Vec2 fwd, Vec2 rht, EyeBin* eye) {
        const float R = cfg.perceptionRadius, R2 = R * R;
        std::vector<int>& neighbors = neighbors_;
        neighbors.clear();
        if (cfg.toroidal) creatureGrid_.queryRadiusWrapped(c.pos, R, neighbors);
        else              creatureGrid_.queryRadius(c.pos, R, neighbors);
        for (int ni : neighbors) {
            if (ni == selfIdx) continue;
            const FishHot& other = hot_[ni];
            float dx = other.pos.x - c.pos.x;
            float dy = other.pos.y - c.pos.y;
            if (cfg.toroidal) { dx = wrapDx(dx); dy = wrapDy(dy); }
            const float dz = other.depth - c.depth;
            const float d2 = dx * dx + dy * dy + dz * dz;
            if (d2 > R2) continue;
            eyeSee(eye, fwd, rht, dx, dy, dz, d2, other.eyeSize, other.eyeCon);
        }
        auto look = [&](int fi, Vec2 fp) {
            float dx = fp.x - c.pos.x;
            float dy = fp.y - c.pos.y;
            if (cfg.toroidal) { dx = wrapDx(dx); dy = wrapDy(dy); }
            const float dz = food[fi].depth - c.depth;
            const float d2 = dx * dx + dy * dy + dz * dz;
            if (d2 > R2) return;
            eyeSee(eye, fwd, rht, dx, dy, dz, d2, ocean::kEyeFoodSize, food[fi].eyeCon);
        };
        if (cfg.toroidal) foodGrid_.forEachInRadiusWrapped(c.pos, R, look);
        else              foodGrid_.forEachInRadius(c.pos, R, look);
        for (std::size_t k = 0; k < predators.size(); ++k) {
            const OceanPredator& p = predators[k];
            float dx = p.pos.x - c.pos.x;
            float dy = p.pos.y - c.pos.y;
            if (cfg.toroidal) { dx = wrapDx(dx); dy = wrapDy(dy); }
            const float dz = p.depth - c.depth;
            const float d2 = dx * dx + dy * dy + dz * dz;
            if (d2 <= R2) eyeSee(eye, fwd, rht, dx, dy, dz, d2, ocean::kEyePredSize, predCon_[k]);
        }
    }
#endif

    template <bool Eye, bool Labels = true>
    void computeSensors(const Fish& c, int selfIdx, float sensors[]) {
        const Vec2 fwd = c.forward();
        const Vec2 rht = {-fwd.y, fwd.x};
        const float R = cfg.perceptionRadius, R2 = R * R;
#ifdef EVO_OCEAN_EYE
        // The compound eye: filled by the neighbour, food and predator loops
        // below (every object within R, before any topological cut), written
        // at the end.
        [[maybe_unused]] EyeBin eye[ocean::kEyeSectors];
        if constexpr (Eye)
            for (EyeBin& e : eye) e = {std::numeric_limits<float>::infinity(), 0.0f, 0.0f};
#endif

        sensors[LOC_X] = c.pos.x / width;
        sensors[LOC_Y] = c.pos.y / height;
        if (cfg.toroidal) {
            sensors[BOUNDARY_DIST_X] = 1.0f;
            sensors[BOUNDARY_DIST_Y] = 1.0f;
            sensors[BOUNDARY_DIST] = 1.0f;
        } else {
            float bx = std::min(c.pos.x, (float)width - c.pos.x);
            float by = std::min(c.pos.y, (float)height - c.pos.y);
            sensors[BOUNDARY_DIST_X] = evomath::tanhf(bx / 100.0f);
            sensors[BOUNDARY_DIST_Y] = evomath::tanhf(by / 100.0f);
            sensors[BOUNDARY_DIST] = std::min(sensors[BOUNDARY_DIST_X], sensors[BOUNDARY_DIST_Y]);
        }

        sensors[OWN_VEL_X] = c.vel.x;
        sensors[OWN_VEL_Y] = c.vel.y;
        sensors[LAST_MOVE_DIR_X] = c.lastMoveDir.x;
        sensors[LAST_MOVE_DIR_Y] = c.lastMoveDir.y;
        kernel::writeOwnStateSensors(c, cfg, sensors);

        // Depth: where am I, which way am I going, how far to surface / floor.
        sensors[LOC_Z] = c.depth / D;
        sensors[OWN_VEL_Z] = c.vz / ocfg.pitchMax;
        sensors[BOUNDARY_DIST_Z] = evomath::tanhf(std::min(c.depth, D - c.depth) / (0.25f * D));

#ifdef EVO_OCEAN_EYE
        if constexpr (!Labels) {
            // Eye-only mode: the labeled senses (ocean::kLabeledSenses) stay 0
            // (stepFish's memset) and are not computed; the eye, if on, looks.
            if constexpr (Eye) eyeScan(c, selfIdx, fwd, rht, eye);
            writeColumnSensors(c, fwd, rht, sensors);
            if constexpr (Eye) eyeWrite(eye, R, sensors);
            sensors[RANDOM_SENSOR] = rng.range(-1.0f, 1.0f);
            return;
        }
#endif

        // ---- neighbours: (x, y) grid candidates, kept if within R in 3-D ----
        std::vector<int>& neighbors = neighbors_;
        neighbors.clear();
        if (cfg.toroidal) creatureGrid_.queryRadiusWrapped(c.pos, R, neighbors);
        else              creatureGrid_.queryRadius(c.pos, R, neighbors);

        float avgDx = 0, avgDy = 0, avgDz = 0, avgHx = 0, avgHy = 0;
        float nearestDist = R;
        int nearestIdx = -1, neighborCount = 0, popFwd = 0, popLR = 0, inRadiusCount = 0;

        auto attendTo = [&](int ni, const FishHot& other, float dx, float dy, float dz, float dist) {
            avgDx += dx; avgDy += dy; avgDz += dz;
            avgHx += other.vel.x; avgHy += other.vel.y;
            const float dotFwd = dx * fwd.x + dy * fwd.y;
            const float dotLR = dx * rht.x + dy * rht.y;
            popFwd += (dotFwd > 0) ? 1 : -1;
            popLR  += (dotLR > 0) ? 1 : -1;
            if (dist < nearestDist) { nearestDist = dist; nearestIdx = ni; }
            neighborCount++;
        };

        if (cfg.maxNeighbors <= 0) {
            for (int ni : neighbors) {
                if (ni == selfIdx) continue;
                const FishHot& other = hot_[ni];
                float dx = other.pos.x - c.pos.x;
                float dy = other.pos.y - c.pos.y;
                if (cfg.toroidal) { dx = wrapDx(dx); dy = wrapDy(dy); }
                const float dz = other.depth - c.depth;
                const float d2 = dx * dx + dy * dy + dz * dz;
                if (d2 > R2) continue;
#ifdef EVO_OCEAN_EYE
                if constexpr (Eye) eyeSee(eye, fwd, rht, dx, dy, dz, d2, other.eyeSize, other.eyeCon);
#endif
                attendTo(ni, other, dx, dy, dz, std::sqrt(d2));
            }
            inRadiusCount = neighborCount;
        } else {
            // Topological: the nearest N by (3-D distance², index), a total order.
            const int keepMax = std::min(cfg.maxNeighbors, MAX_TOPOLOGICAL_NEIGHBORS);
            Candidate best[MAX_TOPOLOGICAL_NEIGHBORS];
            int have = 0;
            for (int ni : neighbors) {
                if (ni == selfIdx) continue;
                const FishHot& other = hot_[ni];
                float dx = other.pos.x - c.pos.x;
                float dy = other.pos.y - c.pos.y;
                if (cfg.toroidal) { dx = wrapDx(dx); dy = wrapDy(dy); }
                const float dz = other.depth - c.depth;
                const float d2 = dx * dx + dy * dy + dz * dz;
                if (d2 > R2) continue;
                ++inRadiusCount;
#ifdef EVO_OCEAN_EYE
                if constexpr (Eye) eyeSee(eye, fwd, rht, dx, dy, dz, d2, other.eyeSize, other.eyeCon);
#endif
                if (have == keepMax) {
                    const Candidate& worst = best[have - 1];
                    if (d2 > worst.distSq || (d2 == worst.distSq && ni > worst.index)) continue;
                }
                int at = (have < keepMax) ? have : have - 1;
                while (at > 0) {
                    const Candidate& prev = best[at - 1];
                    if (prev.distSq < d2 || (prev.distSq == d2 && prev.index < ni)) break;
                    best[at] = prev;
                    --at;
                }
                best[at] = {d2, dx, dy, dz, ni};
                if (have < keepMax) ++have;
            }
            for (int j = 0; j < have; ++j)
                attendTo(best[j].index, hot_[best[j].index], best[j].dx, best[j].dy, best[j].dz,
                         std::sqrt(best[j].distSq));
        }

        if (neighborCount > 0) {
            avgDx /= neighborCount; avgDy /= neighborCount; avgDz /= neighborCount;
            avgHx /= neighborCount; avgHy /= neighborCount;
        }
        sensors[NEIGHBOR_COUNT] = tanhCountOver10(inRadiusCount);
        sensors[AVG_NEIGHBOR_DX] = evomath::tanhf(avgDx / R);
        sensors[AVG_NEIGHBOR_DY] = evomath::tanhf(avgDy / R);
        sensors[AVG_NEIGHBOR_DZ] = evomath::tanhf(avgDz / R);
        sensors[AVG_NEIGHBOR_HEADING_X] = avgHx;
        sensors[AVG_NEIGHBOR_HEADING_Y] = avgHy;
        sensors[NEAREST_NEIGHBOR_DIST] = nearestDist / R;
        sensors[POPULATION_DENSITY] = tanhCountOver15(inRadiusCount);
        sensors[POPULATION_FWD] = tanhCountOver5(popFwd);
        sensors[POPULATION_LR] = tanhCountOver5(popLR);

        if (nearestIdx >= 0) {
            const Fish& nearest = creatures[nearestIdx];
            const float sim = kernel::writeNearestSensors(c, nearest, sensors);
            float dx = nearest.pos.x - c.pos.x;
            float dy = nearest.pos.y - c.pos.y;
            if (cfg.toroidal) { dx = wrapDx(dx); dy = wrapDy(dy); }
            sensors[GENETIC_SIM_FWD] = (dx * fwd.x + dy * fwd.y > 0) ? sim : 0.5f;
        } else {
            kernel::clearNearestSensors(sensors);
            sensors[GENETIC_SIM_FWD] = 0.5f;
        }

        // ---- food: nearest within R in 3-D; DX/DY/DZ = the unit vector to it ----
        float foodDx = 0, foodDy = 0, foodDz = 0, foodDist = R;
        int foodCount = 0;
        auto smell = [&](int fi, Vec2 fp) {
            float dx = fp.x - c.pos.x;
            float dy = fp.y - c.pos.y;
            if (cfg.toroidal) { dx = wrapDx(dx); dy = wrapDy(dy); }
            const float dz = food[fi].depth - c.depth;
            const float d2 = dx * dx + dy * dy + dz * dz;
            if (d2 > R2) return;
#ifdef EVO_OCEAN_EYE
            if constexpr (Eye) eyeSee(eye, fwd, rht, dx, dy, dz, d2, ocean::kEyeFoodSize, food[fi].eyeCon);
#endif
            ++foodCount;
            const float dist = std::sqrt(d2);
            if (dist < foodDist) {
                foodDist = dist;
                const float invD = (dist > 0.001f) ? 1.0f / dist : 0.0f;
                foodDx = dx * invD; foodDy = dy * invD; foodDz = dz * invD;
            }
        };
        if (cfg.toroidal) foodGrid_.forEachInRadiusWrapped(c.pos, R, smell);
        else              foodGrid_.forEachInRadius(c.pos, R, smell);
        sensors[NEAREST_FOOD_DX] = foodDx;
        sensors[NEAREST_FOOD_DY] = foodDy;
        sensors[NEAREST_FOOD_DZ] = foodDz;
        sensors[NEAREST_FOOD_DIST] = foodDist / R;
        sensors[FOOD_DENSITY] = tanhCountOver10(foodCount);

        // ---- pheromone column and current: horizontal, as Classic ----
        writeColumnSensors(c, fwd, rht, sensors);

        // ---- nearest predator within R in 3-D; bearing from its horizontal part ----
        {
            float bestD2 = R2, ox = 0, oy = 0;
            bool seen = false;
            for (const OceanPredator& p : predators) {
                float dx = p.pos.x - c.pos.x;
                float dy = p.pos.y - c.pos.y;
                if (cfg.toroidal) { dx = wrapDx(dx); dy = wrapDy(dy); }
                const float dz = p.depth - c.depth;
                const float d2 = dx * dx + dy * dy + dz * dz;
                if (d2 < bestD2) { bestD2 = d2; ox = dx; oy = dy; seen = true; }
#ifdef EVO_OCEAN_EYE
                if constexpr (Eye)
                    if (d2 <= R2)
                        eyeSee(eye, fwd, rht, dx, dy, dz, d2, ocean::kEyePredSize,
                               predCon_[&p - predators.data()]);
#endif
            }
            if (seen) {
                const float d = std::sqrt(bestD2);
                const float invD = (d > 1e-3f) ? 1.0f / d : 0.0f;
                const float ux = ox * invD, uy = oy * invD;
                sensors[PREDATOR_FWD]  = ux * fwd.x + uy * fwd.y;
                sensors[PREDATOR_LR]   = ux * rht.x + uy * rht.y;
                sensors[PREDATOR_PROX] = 1.0f - d / R;
            } else {
                sensors[PREDATOR_FWD] = sensors[PREDATOR_LR] = sensors[PREDATOR_PROX] = 0.0f;
            }
        }

        // ---- long probe: fish within 30 (horizontally) and ±30 (depth) of a
        // point 200 ahead at my own depth ----
        int probeCount = 0;
        Vec2 probePos = {c.pos.x + fwd.x * LONGPROBE_DIST, c.pos.y + fwd.y * LONGPROBE_DIST};
        auto probe = [&](int ni, Vec2) { probeCount += (std::abs(hot_[ni].depth - c.depth) <= 30.0f); };
        if (cfg.toroidal) {
            probePos = {wrapCoord(probePos.x, (float)width), wrapCoord(probePos.y, (float)height)};
            creatureGrid_.forEachInRadiusWrapped(probePos, LONGPROBE_RADIUS, probe);
        } else {
            creatureGrid_.forEachInRadius(probePos, LONGPROBE_RADIUS, probe);
        }
        sensors[LONGPROBE_POP_FWD] = tanhCountOver5(probeCount);

#ifdef EVO_OCEAN_EYE
        if constexpr (Eye) eyeWrite(eye, R, sensors);   // off: stays 0 (stepFish's memset)
#endif

        sensors[RANDOM_SENSOR] = rng.range(-1.0f, 1.0f);
    }

    // Classic's predator hunt and Stage B economy (World::updatePredators), in
    // 3-D: detection and capture by 3-D distance, the chase steers in depth,
    // the wander drifts in depth.
    void updatePredators(float dt) {
        if (predators.empty()) return;
        const bool  econ     = cfg.predatorEconomy;
        const float pmc      = (cfg.predatorMoveCost >= 0.0f) ? cfg.predatorMoveCost : cfg.moveCost;
        const float detect2  = cfg.predatorPerception * cfg.predatorPerception;
        const float capture2 = cfg.predatorCaptureRadius * cfg.predatorCaptureRadius;

        std::vector<OceanPredator> newborns;
        for (OceanPredator& p : predators) {
            const float speed    = cfg.predatorSpeed * p.speedFactor;
            const float burnRate = cfg.predatorMetabolism + pmc * speed * speed;
            p.digestTimer -= dt;
            if (econ) {
                p.age    += dt;
                p.energy -= burnRate * dt;
            }

            near_.clear();
            if (cfg.toroidal) creatureGrid_.queryRadiusWrapped(p.pos, cfg.predatorPerception, near_);
            else              creatureGrid_.queryRadius(p.pos, cfg.predatorPerception, near_);
            int   bestIdx = -1;
            float bestD2  = detect2;
            for (int ci : near_) {
                const Fish& prey = creatures[ci];
                if (!prey.alive) continue;
                const float d2 = dist2(p.pos, p.depth, prey.pos, prey.depth);
                if (d2 < bestD2) { bestD2 = d2; bestIdx = ci; }
            }

            Vec2 dir = p.forward();
            float hs = 1.0f, vr;   // horizontal share and depth rate, per unit speed
            if (bestIdx >= 0) {
                const Fish& prey = creatures[bestIdx];
                float dx = prey.pos.x - p.pos.x, dy = prey.pos.y - p.pos.y;
                if (cfg.toroidal) { dx = wrapDx(dx); dy = wrapDy(dy); }
                const float dz = prey.depth - p.depth;
                const float len = std::sqrt(dx * dx + dy * dy + dz * dz);
                const float inv = (len > 1e-6f) ? 1.0f / len : 0.0f;
                const float h = std::sqrt(dx * dx + dy * dy) * inv;
                if (h > 1e-6f) dir = normalized({dx, dy});
                hs = h;
                vr = dz * inv;
                p.pitch = std::clamp(vr, -0.3f, 0.3f);
            } else {
                const float jitter = rng.range(-0.3f, 0.3f);
                const float cs = evomath::cosf(jitter), sn = evomath::sinf(jitter);
                dir = {dir.x * cs - dir.y * sn, dir.x * sn + dir.y * cs};
                p.pitch = std::clamp(p.pitch + rng.range(-0.05f, 0.05f), -0.3f, 0.3f);
                vr = p.pitch;
            }
            p.vel = dir;

            p.pos.x += dir.x * hs * speed * dt;
            p.pos.y += dir.y * hs * speed * dt;
            p.depth += vr * speed * dt;
            if (p.depth < 0.0f) { p.depth = 0.0f; p.pitch = std::abs(p.pitch); }
            else if (p.depth > D) { p.depth = D; p.pitch = -std::abs(p.pitch); }
            if (current_.active()) {
                const Vec2 cur = current_.velocityAt(p.pos);
                p.pos.x += cur.x * dt;
                p.pos.y += cur.y * dt;
            }
            if (p.pos.x < 0) p.pos.x += width;  else if (p.pos.x >= width)  p.pos.x -= width;
            if (p.pos.y < 0) p.pos.y += height; else if (p.pos.y >= height) p.pos.y -= height;

            if (bestIdx >= 0 && p.digestTimer <= 0.0f && creatures[bestIdx].alive) {
                Fish& prey = creatures[bestIdx];
                if (dist2(p.pos, p.depth, prey.pos, prey.depth) <= capture2) {
                    if (econ) p.energy += (cfg.predatorMeal >= 0.0f) ? cfg.predatorMeal
                                                                     : prey.foodReserves;
                    recordDeath(prey, 'p');
                    p.digestTimer = cfg.predatorCooldown;
                }
            }

            if (econ) {
                if (p.energy < 0.0f) {
                    p.alive = false;
                    totalPredatorDeathsStarvation++;
                } else if (cfg.predatorMaxAge > 0.0f && p.age >= cfg.predatorMaxAge) {
                    p.alive = false;
                    totalPredatorDeathsAge++;
                } else if (p.energy >= cfg.predatorReproEnergy * p.buddingFactor &&
                           (cfg.predatorMaxPopulation <= 0 ||
                            (int)predators.size() + (int)newborns.size() < cfg.predatorMaxPopulation)) {
                    OceanPredator child;
                    if (cfg.toroidal) {
                        child.pos = { wrapCoord(p.pos.x + rng.range(-8.0f, 8.0f), (float)width),
                                      wrapCoord(p.pos.y + rng.range(-8.0f, 8.0f), (float)height) };
                    } else {
                        child.pos = { std::clamp(p.pos.x + rng.range(-8.0f, 8.0f), 0.0f, (float)width  - 1.0f),
                                      std::clamp(p.pos.y + rng.range(-8.0f, 8.0f), 0.0f, (float)height - 1.0f) };
                    }
                    const float a = rng.range(0.0f, 6.2832f);
                    child.vel    = {evomath::cosf(a), evomath::sinf(a)};
                    child.depth  = clampDepth(p.depth + rng.range(-8.0f, 8.0f));
                    child.energy = cfg.predatorNewbornEnergy;
                    p.energy    -= cfg.predatorNewbornEnergy;
                    child.generation = p.generation + 1;
                    if (cfg.predatorInheritance) {
                        const float m = cfg.predatorTraitMutation;
                        child.speedFactor   = std::clamp(p.speedFactor + rng.range(-m, m),
                                                         PRED_TRAIT_MIN, PRED_TRAIT_MAX);
                        child.buddingFactor = std::clamp(p.buddingFactor + rng.range(-m, m),
                                                         PRED_TRAIT_MIN, PRED_TRAIT_MAX);
                    }
                    newborns.push_back(child);
                    totalPredatorBirths++;
                }
            }
        }

        if (econ) {
            for (auto& baby : newborns) predators.push_back(std::move(baby));
            predators.erase(std::remove_if(predators.begin(), predators.end(),
                                           [](const OceanPredator& p) { return !p.alive; }),
                            predators.end());
        }
    }

    void refillPredators() {
        while ((int)predators.size() < cfg.predatorMinPopulation) {
            predators.push_back(makePredator());
            totalPredatorReseeds++;
        }
    }
};
