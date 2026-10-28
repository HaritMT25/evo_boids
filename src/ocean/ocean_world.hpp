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
// E1 physics (OceanConfig, every knob OFF by default; OFF = the preview's
// arithmetic, bit for bit — each old block is kept as the fallback branch):
//   * flowAt(p, d): a 3-D, divergence-free flow = the current (× f(d) =
//     smoothstep(2d/D) when deepCurrent) + three tapered deep-water wave
//     trains (waveSpeed > 0). It replaces the horizontal current wherever
//     flowE1(): food (with foodDrift), predators and fish move by dt·U after
//     their own motion, a fish by min(1, 1/bodySize)·dt·U with sizeDrift;
//     depth is clamped after, x / y wrap. CURRENT_FWD / LR read flowAt.
//     While flowE1() it is evaluated from a derived cache (ocean_flow.hpp).
//   * lightAt(d) = L0·exp(−kWater·d) (oceanmath::expf, ocean_math.hpp):
//     display only (the page's shading); not part of the state.
//   * depthCostScaled: cHold / cPress as multiples of metabolicRate·massFactor.
//   * sizeFight: kernel::fightOutcome's strengths × bodySize²; mouthGate:
//     food (OceanFood::size, 1 for now) only if size <= kMouth·bodySize, for
//     eating and for the smell sensors alike.
// No E1 feature draws from the rng.
// ----------------------------------------------------------------------------
#ifndef EVO_WORLD_OCEAN
#error "ocean_world.hpp needs the Ocean sensor list: compile with -DEVO_WORLD_OCEAN"
#endif
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include "../core/world.hpp"   // SignalGrid, MAX_TOPOLOGICAL_NEIGHBORS, and through it the
                               // kernel, WorldCommon, SpatialGrid, CurrentField, Predator
#include "ocean_config.hpp"
#include "ocean_flow.hpp"
#include "ocean_math.hpp"
#include "fish.hpp"

struct OceanFood {
    Vec2  pos;
    float depth = 0.0f;
    bool  alive = true;
    float size  = 1.0f;   // E1: what mouthGate compares with kMouth·bodySize (always 1
                          // until E2 sizes the particles)
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
    // v wrapped into [0, span), span > 0, in constant time. It replaces the
    // loops `while (v < 0) v += span; while (v >= span) v -= span;`, which
    // never ended once v + span == v (currentStrength 1e20 moved food by ~1e17
    // per step), and gives their bits wherever they ended:
    //   * v in [-span, 2·span): the loops' one add / subtract, as they did it
    //     (an add that rounds up to span still ends at 0);
    //   * further out: std::fmod, which is exact (any correct libm, native or
    //     wasm, returns the same bits), then the loops' last add. With span a
    //     whole number <= 2^24 (a world size) and |v| < 2^24, every step of the
    //     loops was exact too, so this equals them there as well (pinned by
    //     tests/ocean_tests.cpp testWrap); a non-finite v gives NaN, not a hang.
    // The bounded configs (ocean_config.hpp) keep every caller in the first case.
    static float wrapCoord(float v, float span) {
        if (v >= -span && v < 2.0f * span) {
            if (v < 0.0f) v += span;
            if (v >= span) v -= span;
            return v;
        }
        float r = std::fmod(v, span);   // exact; the sign of v, |r| < span
        if (r < 0.0f) r += span;        // the loops' last add
        else if (r == 0.0f) r = 0.0f;   // -0 -> +0, as that add gives
        if (r >= span) r -= span;
        return r;
    }
    // The preview's single-pass wrap of food and predators (`if (v < 0) v +=
    // span; else if (v >= span) v -= span;`), exactly as it was for every v in
    // [-span, 2·span) — an add that rounds up still leaves span there, as it
    // always did — and wrapCoord further out, where one pass left the value
    // outside the world (100 eddies at strength 1000 in a 100-wide world move
    // food ~150 in a step). The bounded cases never go further out: their
    // results are unchanged.
    static float wrapPreview(float v, float span) {
        if (v < -span || v >= 2.0f * span) return wrapCoord(v, span);
        if (v < 0.0f) v += span;
        else if (v >= span) v -= span;
        return v;
    }
    float clampDepth(float z) const { return std::clamp(z, 0.0f, D); }

    // ---- E1: flow and light ------------------------------------------------
    struct Flow { float x = 0.0f, y = 0.0f, d = 0.0f; };   // d: + = sinking

    static constexpr int kWaveTrains = 3;
    bool wavesOn() const { return ocfg.waveSpeed > 0.0f; }
    // The 3-D flow replaces the preview's horizontal current (food, predators,
    // fish, the CURRENT sensors). Off: the preview's blocks run unchanged.
    bool flowE1() const { return wavesOn() || ocfg.deepCurrent; }

    // smoothstep S(r) = 3r² − 2r³ on r clamped to [0, 1], and dS/dr.
    static float smooth01(float r) {
        r = std::clamp(r, 0.0f, 1.0f);
        return r * r * (3.0f - 2.0f * r);
    }
    static float smooth01d(float r) {
        r = std::clamp(r, 0.0f, 1.0f);
        return 6.0f * r * (1.0f - r);
    }

    // The flow at (p, depth d), units/s. Divergence-free:
    //  * the current C(p) (CurrentField, 2-D, from streamfunctions), times
    //    f(d) = S(2d/D) with deepCurrent (∇·(f C) = f ∇·C = 0), else times 1;
    //  * per wave train (K = 2π(m/W, n/H), k = |K|, q = K/k, θ = K·p − φ(t) + φ0):
    //    ψ = (A/k) e^{−kd} B(d) cos θ, B = S(d/b)·S((D−d)/b), b = min(waveBand, D/4);
    //    (u_x, u_y) = q·A e^{−kd} (B − B'/k) cos θ,  u_d = −A e^{−kd} B sin θ.
    //    B and B' vanish at surface and floor: no flow through either.
    // θ is formed and reduced to [0, 2π) in double (exact IEEE ops), then the
    // portable float sin / cos / expm1: the same bits native and wasm.
    // The depth taper B(d), B'(d) depends on d alone, so it is formed once per
    // call and shared by the three trains (the same expressions as
    // waveTrainExactAt(i, p, d): the same bits).
    //
    // Evaluation (ocean_flow.hpp): while flowE1() the flow comes from the E1
    // cache (flowFast) — the eddy current from the Ocean's own copy of the
    // vortices; off (sizeDrift alone reaches flowAt then) the exact formulas
    // above (flowExactAt), the preview's arithmetic. The public readers below
    // (flowAt, currentFlowAt, waveTrainAt, currentAt) give the field the step
    // actually uses; the *Exact* ones are the formulas, for the tests.
    Flow flowAt(Vec2 p, float d) const {
        if (!flowE1()) return flowExactAt(p, d);
        ensureFlow();
        return flowFast(p, d);
    }
    Flow flowExactAt(Vec2 p, float d) const {
        Flow U = currentFlowExactAt(p, d);
        if (wavesOn()) {
            const Taper tp = taperAt(d);
            for (int i = 0; i < kWaveTrains; ++i) {
                const Flow u = waveTrainExactAt(i, p, d, tp);
                U.x += u.x;
                U.y += u.y;
                U.d += u.d;
            }
        }
        return U;
    }

    // flowAt's parts (public for the divergence probes in tests/ocean_tests.cpp).
    // The current, weighted by f(d) with deepCurrent; no vertical part.
    Flow currentFlowAt(Vec2 p, float d) const {
        if (!flowE1()) return currentFlowExactAt(p, d);
        ensureFlow();
        return currentFlowFast(p, d);
    }
    Flow currentFlowExactAt(Vec2 p, float d) const {
        Flow U;
        if (current_.active()) {
            const Vec2 c = current_.velocityAt(p);
            const float f = ocfg.deepCurrent ? smooth01(2.0f * d / D) : 1.0f;
            U.x = f * c.x;
            U.y = f * c.y;
        }
        return U;
    }
    // The trains' shared depth taper B = S(d/b)·S((D−d)/b) and its d-derivative.
    struct Taper { float B, Bp; };
    Taper taperAt(float d) const {
        const float b = std::min(ocfg.waveBand, 0.25f * D);
        const float r1 = d / b, r2 = (D - d) / b;
        const float s1 = smooth01(r1), s2 = smooth01(r2);
        Taper tp;
        tp.B = s1 * s2;
        tp.Bp = (smooth01d(r1) * s2 - s1 * smooth01d(r2)) / b;
        return tp;
    }
    // Wave train i alone (whatever waveSpeed is; 0 gives zero flow).
    Flow waveTrainAt(int i, Vec2 p, float d) const {
        if (!flowE1()) return waveTrainExactAt(i, p, d);
        ensureFlow();
        return waveTrainFast(i, p, d);
    }
    Flow waveTrainExactAt(int i, Vec2 p, float d) const { return waveTrainExactAt(i, p, d, taperAt(d)); }
    Flow waveTrainExactAt(int i, Vec2 p, float d, const Taper& tp) const {
        const WaveTrain& t = waves_[i];
        const float B = tp.B, Bp = tp.Bp;
        const float A = t.frac * ocfg.waveSpeed;
        // e^{−kd} as 1 / e^{kd}: full relative precision at depth, where
        // expm1f(−kd) + 1 would keep only ~1e-7 absolute (e^{kd} = inf -> 0).
        const float E = 1.0f / (evomath::expm1f(t.k * d) + 1.0f);
        double th = t.kx * (double)p.x + t.ky * (double)p.y - t.phase + t.phi;
        th -= kTwoPi * std::floor(th / kTwoPi);
        const float tf = (float)th;
        const float sn = evomath::sinf(tf), cs = evomath::cosf(tf);
        const float uh = A * E * (B - Bp / t.k) * cs;
        Flow u;
        u.x = t.qx * uh;
        u.y = t.qy * uh;
        u.d = -A * E * B * sn;
        return u;
    }

    // Light at depth d (display only in E1): L0·exp(−kWater·d), the exp taken
    // directly (oceanmath::expf: full relative precision at every depth). It
    // was L0·(1 + expm1f(−kWater·d)), which keeps only ~6e-8 absolute and is
    // exactly 0 below kWater·d = 17.3 (e.g. depth 18 at kWater 1).
    float lightAt(float d) const { return ocfg.L0 * oceanmath::expf(-ocfg.kWater * d); }

    // A wave train's travelled phase φ(t) in [0, 2π) (state; 0 until waves run).
    double wavePhase(int i) const { return waves_[i].phase; }
    bool waveClockStarted() const {
        for (const WaveTrain& t : waves_) if (t.phase != 0.0) return true;
        return false;
    }

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
        prepareWaves();
        initFlowCache();
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
        food.push_back(f);
    }

    void update(float dt) {
        frame++;
        births = 0;
        deaths = 0;

        current_.advance(frame);
        flow_.curAdvanced = true;
        if (wavesOn()) advanceWaves(dt);   // the wave clock runs only while waves are on

        if (flowE1()) {
            ensureFlow();   // the E1 cache, for this frame / phase / config (read by every flowFast below)
            // E1: food rides the 3-D flow (depth too), sampled where it is.
            if (cfg.foodDrift) {
                for (auto& f : food) {
                    if (!f.alive) continue;
                    const Flow U = flowFast(f.pos, f.depth);
                    f.pos.x = wrapCoord(f.pos.x + U.x * dt, (float)width);
                    f.pos.y = wrapCoord(f.pos.y + U.y * dt, (float)height);
                    f.depth = clampDepth(f.depth + U.d * dt);
                }
            }
        } else if (cfg.foodDrift && current_.active()) {
            for (auto& f : food) {
                if (!f.alive) continue;
                Vec2 v = current_.velocityAt(f.pos);
                f.pos.x += v.x * dt;  f.pos.y += v.y * dt;
                f.pos.x = wrapPreview(f.pos.x, (float)width);
                f.pos.y = wrapPreview(f.pos.y, (float)height);
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

        // ---- per-fish phase: sensors, network, movement, metabolism ----------
        for (int i = 0; i < numCreatures; ++i) stepFish(i, dt);

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
                if (ocfg.mouthGate && f.size > ocfg.kMouth * c.bodySize) continue;   // E1: too big to swallow
                f.alive = false;
                c.foodReserves += cfg.foodEnergy;
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
    }

    bool currentActive() const { return current_.active(); }
    // The 2-D current the step uses: while flowE1() with eddies, the Ocean's
    // evaluator (ocean_flow.hpp); otherwise CurrentField's own.
    Vec2 currentAt(Vec2 p) const {
        if (!flowE1() || !flow_.curOn) return current_.velocityAt(p);
        ensureFlow();
        return eddyVelocity(p);
    }
    Vec2 currentExactAt(Vec2 p) const { return current_.velocityAt(p); }   // CurrentField's own (tests)

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
    struct FishHot { Vec2 pos; Vec2 vel; float depth; };
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

    // E1 wave trains: fixed modes over the world (no draws). kx, ky = K
    // (double, for θ); k, q = |K|, K/|K|; phase = the travelled ∫ω dt, kept in
    // [0, 2π) (the only wave state that changes); ω = sqrt(waveGravity·k) is
    // read fresh each step, so a live waveGravity change is smooth.
    static constexpr double kTwoPi = 6.283185307179586;
    struct WaveTrain {
        double kx = 0.0, ky = 0.0, phase = 0.0, phi = 0.0;
        float k = 1.0f, qx = 1.0f, qy = 0.0f, frac = 0.0f;
    };
    WaveTrain waves_[kWaveTrains];

    void prepareWaves() {
        const auto& kModes = oceanflow::kWaveModes;   // the flow tables are built from the same modes
        static const float kFrac[kWaveTrains] = {0.5f, 0.3f, 0.2f};
        for (int i = 0; i < kWaveTrains; ++i) {
            WaveTrain& t = waves_[i];
            t.kx = kTwoPi * kModes[i][0] / (double)width;
            t.ky = kTwoPi * kModes[i][1] / (double)height;
            const double k = std::sqrt(t.kx * t.kx + t.ky * t.ky);
            t.k = (float)k;
            t.qx = (float)(t.kx / k);
            t.qy = (float)(t.ky / k);
            t.frac = kFrac[i];
            t.phi = (double)i;   // 0, 1, 2 rad
            t.phase = 0.0;
        }
    }
    void advanceWaves(float dt) {
        for (WaveTrain& t : waves_) {
            t.phase += std::sqrt((double)ocfg.waveGravity * (double)t.k) * (double)dt;
            while (t.phase >= kTwoPi) t.phase -= kTwoPi;
        }
    }

    // ---- the E1 flow cache (ocean_flow.hpp) --------------------------------
    // Derived scratch: rebuilt from hashed state (cfg / ocfg / the world size
    // at init, the CurrentField's vortices, the frame, the live wave knobs
    // and phases) by ensureFlow before it is read, on bitwise keys: init
    // clears it; a waveSpeed / waveBand change rebuilds the depth pieces
    // (the phases are kept); waveGravity only changes the clock. ensureFlow
    // runs once per step (update, after the clocks advance) and in every
    // public reader, so the shim, the harness driver (which assigns configs
    // directly) and the tests all get it; a refused oc_set_param changes no
    // key.
    static_assert(kWaveTrains == oceanflow::kTrains, "ocean_flow.hpp's train count");
    mutable oceanflow::FlowCache flow_;

    void initFlowCache() {
        flow_ = oceanflow::FlowCache{};
        // CurrentField::init's own: eddies unless mode 1, nothing at strength <= 0,
        // core_ = max(core, 1); velocityAt's inv2c2 and scale.
        if (cfg.currentMode != 1 && cfg.currentStrength > 0.0f) {
            flow_.curOn = true;
            const float core = (cfg.currentCore > 1.0f) ? cfg.currentCore : 1.0f;
            flow_.inv2c2 = 1.0f / (2.0f * core * core);
            const float scale = cfg.currentStrength / core * 1.6487212707f;
            flow_.vort = current_.vortexState();
            flow_.eddies.resize(flow_.vort.size() / CurrentField::VORTEX_FLOATS);
            for (std::size_t i = 0; i < flow_.eddies.size(); ++i)
                flow_.eddies[i].signScale = flow_.vort[i * CurrentField::VORTEX_FLOATS + 2] * scale;
        }
        // The waves' T = K / |K|² (prepareWaves has set K for this world), and
        // in cell units (∂/∂x = d/dt · kXCells / W).
        flow_.xScale = (double)oceanflow::kXCells / (double)width;
        flow_.yScale = (double)oceanflow::kYCells / (double)height;
        for (int i = 0; i < kWaveTrains; ++i) {
            const WaveTrain& t = waves_[i];
            const double kk = t.kx * t.kx + t.ky * t.ky;
            flow_.Tx[i] = (float)(t.kx / kk);
            flow_.Ty[i] = (float)(t.ky / kk);
            flow_.TxS[i] = (float)(t.kx / kk * flow_.xScale);
            flow_.TyS[i] = (float)(t.ky / kk * flow_.yScale);
        }
        flow_.yRot.assign(oceanflow::kYCells, oceanflow::TrigV{});
    }

    static std::uint32_t floatKey(float v) { std::uint32_t u; std::memcpy(&u, &v, 4); return u; }

    // The depth pieces (ocean_flow.hpp) for the current waveSpeed, waveBand, D.
    void buildDepth() const {
        oceanflow::FlowCache& c = flow_;
        c.depthOn = false;
        const float b = std::min(ocfg.waveBand, 0.25f * D);   // taperAt's
        const float Dh = 0.5f * D, Dmb = D - b;
        const float lo[4] = {0.0f, b, Dh, Dmb}, hi[4] = {b, Dh, Dmb, D};
        float kMax = 0.0f;
        for (const WaveTrain& t : waves_) kMax = std::max(kMax, t.k);
        // Cell counts in double, checked before any conversion or allocation.
        double cells[4], total = 0.0;
        for (int q = 0; q < 4; ++q) {
            const double L = (double)hi[q] - (double)lo[q];
            if (!(L > 0.0)) return;   // a degenerate piece (unvalidated band): the exact formulas
            const int least = (q == 0 || q == 3) ? oceanflow::kDepthCellsBand : oceanflow::kDepthCellsMin;
            cells[q] = std::max((double)least, std::ceil((double)kMax * L / oceanflow::kDepthKCell));
            total += cells[q];
        }
        if (!(total <= oceanflow::kDepthCellsMax)) return;   // NaN too
        for (int q = 0; q < 4; ++q) {
            oceanflow::DepthPiece& pc = c.piece[q];
            const int n = (int)cells[q];
            const double L = (double)hi[q] - (double)lo[q], h = L / (double)n;
            pc.lo = lo[q];
            pc.n = n;
            pc.scale = (double)n / L;
            pc.dScale = (float)pc.scale;
            // waveTrainExactAt's own expressions at depth x: H = A e^{−kx} (B − B'/k)
            // (u_h without cos θ) and V = −A e^{−kx} B; both exactly 0 at the
            // surface and the floor (no flow through either).
            auto HV = [&](int i, float x, float& H, float& V) {
                const WaveTrain& t = waves_[i];
                const Taper tp = taperAt(x);
                const float A = t.frac * ocfg.waveSpeed;
                const float E = 1.0f / (evomath::expm1f(t.k * x) + 1.0f);
                H = A * E * (tp.B - tp.Bp / t.k);
                V = -A * E * tp.B;
                if (x == 0.0f || x == D) { H = 0.0f; V = 0.0f; }
            };
            auto at = [&](int j) { return (j == 0) ? lo[q] : (j == n) ? hi[q] : (float)((double)lo[q] + h * (double)j); };
            pc.cell.assign((std::size_t)n, oceanflow::DepthCell{});
            for (int j = 0; j < n; ++j) {
                const float d0 = at(j), d1 = at(j + 1);
                oceanflow::DepthCell& cl = pc.cell[j];
                for (int i = 0; i < kWaveTrains; ++i) {
                    const double k = (double)waves_[i].k;
                    float H0, V0, H1, V1;
                    HV(i, d0, H0, V0);
                    HV(i, d1, H1, V1);
                    // The rise V(d1) − V(d0) = ∫ k·H: from H, never from the two
                    // rounded V values (see oceanflow::hval).
                    const double rise = oceanflow::gauss3((double)d0, (double)d1, [&](double x) {
                        float Hx, Vx;
                        HV(i, (float)x, Hx, Vx);
                        return k * (double)Hx;
                    });
                    cl.v[i] = V0;
                    cl.dv[i] = (float)rise;
                    cl.m0[i] = (float)(h * k * (double)H0);   // V' = k·H, per unit t
                    cl.m1[i] = (float)(h * k * (double)H1);
                }
            }
        }
        c.b = b;
        c.Dh = Dh;
        c.Dmb = Dmb;
        c.depthOn = true;
    }

    void ensureFlow() const {
        oceanflow::FlowCache& c = flow_;
        // The waves' depth pieces: rebuilt when waveSpeed, waveBand or D move.
        {
            const std::uint32_t ks = floatKey(ocfg.waveSpeed), kb = floatKey(ocfg.waveBand), kd = floatKey(D);
            if (!c.depthKeyed || ks != c.keySpeed || kb != c.keyBand || kd != c.keyD) {
                buildDepth();
                c.depthKeyed = true;
                c.keySpeed = ks;
                c.keyBand = kb;
                c.keyD = kd;
            }
        }
        // The phase factors: regenerated when a phase moves (every step while
        // the waves run; frozen with the clock), from the static base.
        {
            bool same = c.phaseKeyed;
            std::uint64_t kp[kWaveTrains];
            for (int i = 0; i < kWaveTrains; ++i) {
                std::memcpy(&kp[i], &waves_[i].phase, 8);
                same = same && kp[i] == c.keyPhase[i];
            }
            if (!same) {
                for (int i = 0; i < kWaveTrains; ++i) {
                    oceanflow::cosSin(waves_[i].phi - waves_[i].phase, c.cp[i], c.sp[i]);
                    c.keyPhase[i] = kp[i];
                }
                const oceanflow::TrigV* base = oceanflow::yBaseTable().cell;
                const oceanflow::TrigCell lane0 = {c.cp[0], 0.0f, 0.0f, 0.0f, c.sp[0], 0.0f, 0.0f, 0.0f};
                for (int j = 0; j < oceanflow::kYCells; ++j) {
                    oceanflow::TrigV& y = c.yRot[j];
                    oceanflow::setLane(y, 0, lane0);   // train 0 (n = 0): its constant y factor
                    for (int i = 1; i < kWaveTrains; ++i) {
                        oceanflow::TrigCell r = oceanflow::getLane(base[j], i);
                        const float cr = c.cp[i], sr = c.sp[i];
                        oceanflow::rotate(r.c, r.s, cr, sr);
                        oceanflow::rotate(r.dc, r.ds, cr, sr);
                        oceanflow::rotate(r.mc0, r.ms0, cr, sr);
                        oceanflow::rotate(r.mc1, r.ms1, cr, sr);
                        oceanflow::setLane(y, i, r);
                    }
                }
                c.phaseKeyed = true;
            }
        }
        if (c.curOn && (!c.curReady || c.curFrame != frame || c.curAdvKey != c.curAdvanced)) {
            // CurrentField::advance(frame)'s centres (vortexState(): base x, y,
            // sign, ax, ay, fx, fy, px, py), or 0 before the first advance.
            const float t = (float)frame;
            for (std::size_t i = 0; i < c.eddies.size(); ++i) {
                const float* v = &c.vort[i * CurrentField::VORTEX_FLOATS];
                oceanflow::Eddy& e = c.eddies[i];
                if (c.curAdvanced) {
                    e.cx = v[0] + v[3] * evomath::sinf(v[5] * t + v[7]);
                    e.cy = v[1] + v[4] * evomath::sinf(v[6] * t + v[8]);
                } else {
                    e.cx = 0.0f;
                    e.cy = 0.0f;
                }
            }
            c.curReady = true;
            c.curFrame = frame;
            c.curAdvKey = c.curAdvanced;
        }
    }

    // CurrentField::velocityAt (Mode::Eddies) with e^{−u} from the radial
    // table (ocean_flow.hpp): same vortex order, prefactor, raw differences
    // and accumulation; a vortex at u >= 16 adds nothing.
    Vec2 eddyVelocity(Vec2 p) const {
        Vec2 v{0.0f, 0.0f};
        const float inv2c2 = flow_.inv2c2;
        const float* E = oceanflow::radialTable().E;
        for (const oceanflow::Eddy& e : flow_.eddies) {
            const float dx = p.x - e.cx;
            const float dy = p.y - e.cy;
            float g;
            if (!oceanflow::radial(E, (dx * dx + dy * dy) * inv2c2, g)) continue;
            const float f = e.signScale * g;
            v.x += f * (-dy);
            v.y += f * dx;
        }
        return v;
    }

    // The E1 flow from the cache (flowE1(), ensureFlow() done).
    Flow currentFlowFast(Vec2 p, float d) const {
        Flow U;
        if (current_.active()) {
            const Vec2 c = flow_.curOn ? eddyVelocity(p) : current_.velocityAt(p);
            const float f = ocfg.deepCurrent ? smooth01(2.0f * d / D) : 1.0f;
            U.x = f * c.x;
            U.y = f * c.y;
        }
        return U;
    }
    // Every train's V and V' = dV/dd at depth d from the pieces (a train per
    // lane); false = not from the tables (none built, or d is NaN).
    bool depthProfiles(float d, oceanflow::v4f& V, oceanflow::v4f& Vd) const {
        const oceanflow::FlowCache& c = flow_;
        if (!c.depthOn) return false;
        if (!(d > 0.0f && d < D)) {
            if (!(d <= 0.0f || d >= D)) return false;   // NaN
            V = oceanflow::v4f{};                        // surface, floor, outside
            Vd = oceanflow::v4f{};
            return true;
        }
        const int q = (d <= c.b) ? 0 : (d <= c.Dh) ? 1 : (d <= c.Dmb) ? 2 : 3;
        const oceanflow::DepthPiece& pc = c.piece[q];
        const double s = ((double)d - (double)pc.lo) * pc.scale;   // in [0, n] (rounding aside)
        if (!(s >= 0.0 && s <= (double)pc.n + 0.5)) return false;
        int j = (int)s;
        if (j > pc.n - 1) j = pc.n - 1;
        const oceanflow::BasisV B = oceanflow::basisV((float)(s - (double)j));
        const oceanflow::DepthCell& cl = pc.cell[j];
        V = oceanflow::hval(B, cl.v, cl.dv, cl.m0, cl.m1);
        Vd = oceanflow::hder(B, cl.dv, cl.m0, cl.m1) * oceanflow::splat(pc.dScale);
        return true;
    }
    // A periodic cell coordinate: v·scale in [0, N) after at most one add /
    // subtract of a period (ordinary positions, swimming or just spawned at
    // an inclusive edge); false for anything further out or not finite.
    static bool cellCoord(float v, double scale, int N, int& i, float& t) {
        double s = (double)v * scale;
        if (s < 0.0) s += (double)N;
        if (s >= (double)N) s -= (double)N;   // also s == N (the periodic end) -> 0
        if (!(s >= 0.0 && s < (double)N)) return false;
        i = (int)s;
        t = (float)(s - (double)i);
        return true;
    }
    // The three trains at (p, d) from the tables (ocean_flow.hpp):
    //   u_x = (T_x V') P, u_y = (T_y V') P, u_d = (−V)(T_x ∂x P + T_y ∂y P),
    // P = cos θ from the cos / sin tables; false = use the exact formulas.
    bool wavesTable(Vec2 p, float d, Flow u[kWaveTrains]) const {
        using oceanflow::v4f;
        const oceanflow::FlowCache& c = flow_;
        v4f V, Vd;
        if (!depthProfiles(d, V, Vd)) return false;
        int ix, iy;
        float tx, ty;
        if (!cellCoord(p.x, c.xScale, oceanflow::kXCells, ix, tx)) return false;
        if (!cellCoord(p.y, c.yScale, oceanflow::kYCells, iy, ty)) return false;
        const oceanflow::BasisV bx = oceanflow::basisV(tx), by = oceanflow::basisV(ty);
        const oceanflow::TrigV& X = oceanflow::xTable().cell[ix];
        const oceanflow::TrigV& Y = c.yRot[iy];   // lane 0: train 0's constant factor
        const v4f cx = oceanflow::hval(bx, X.c, X.dc, X.mc0, X.mc1);
        const v4f sx = oceanflow::hval(bx, X.s, X.ds, X.ms0, X.ms1);
        const v4f cxd = oceanflow::hder(bx, X.dc, X.mc0, X.mc1);   // per unit t
        const v4f sxd = oceanflow::hder(bx, X.ds, X.ms0, X.ms1);
        const v4f cy = oceanflow::hval(by, Y.c, Y.dc, Y.mc0, Y.mc1);
        const v4f sy = oceanflow::hval(by, Y.s, Y.ds, Y.ms0, Y.ms1);
        const v4f cyd = oceanflow::hder(by, Y.dc, Y.mc0, Y.mc1);
        const v4f syd = oceanflow::hder(by, Y.ds, Y.ms0, Y.ms1);
        const v4f P = cx * cy - sx * sy;
        const v4f gradT = c.TxS * (cxd * cy - sxd * sy) + c.TyS * (cx * cyd - sx * syd);   // T·∇P
        const v4f ux = (c.Tx * Vd) * P, uy = (c.Ty * Vd) * P, ud = (-V) * gradT;
        for (int i = 0; i < kWaveTrains; ++i) {
            u[i].x = ux[i];
            u[i].y = uy[i];
            u[i].d = ud[i];
        }
        return true;
    }
    void wavesExact(Vec2 p, float d, Flow u[kWaveTrains]) const {
        const Taper tp = taperAt(d);
        for (int i = 0; i < kWaveTrains; ++i) u[i] = waveTrainExactAt(i, p, d, tp);
    }
    Flow waveTrainFast(int i, Vec2 p, float d) const {
        Flow u[kWaveTrains];
        if (!wavesTable(p, d, u)) return waveTrainExactAt(i, p, d);
        return u[i];
    }
    Flow flowFast(Vec2 p, float d) const {
        Flow U = currentFlowFast(p, d);
        if (wavesOn()) {
            Flow u[kWaveTrains];
            if (!wavesTable(p, d, u)) wavesExact(p, d, u);
            for (int i = 0; i < kWaveTrains; ++i) {
                U.x += u[i].x;
                U.y += u[i].y;
                U.d += u[i].d;
            }
        }
        return U;
    }
    // What the step's fish read: the E1 cache while flowE1(), else the exact
    // formulas (sizeDrift alone: the preview's current, unweighted).
    Flow flowStep(Vec2 p, float d) const { return flowE1() ? flowFast(p, d) : flowExactAt(p, d); }

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

    void stepFish(int i, float dt) {
        Fish& c = creatures[i];
        if (!c.alive) return;

        float sensors[NUM_SENSES];
        std::memset(sensors, 0, sizeof(sensors));
        computeSensors(c, i, sensors);

        float actions[NUM_ACTIONS];
        std::memset(actions, 0, sizeof(actions));
        feedForward(c.nnet, sensors, actions, cfg.fastTanh);

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

        if (flowE1() || ocfg.sizeDrift) {
            // E1: carried by the 3-D flow where the fish now is; with sizeDrift
            // a big fish by less (drag / mass ∝ 1/L). Not swimming: no speed,
            // no move cost. Depth clamped after (the walls' vz response above
            // stays as it was).
            const Flow U = flowStep(c.pos, c.depth);
            const float w = ocfg.sizeDrift ? std::min(1.0f, 1.0f / c.bodySize) : 1.0f;
            c.pos.x = wrapCoord(c.pos.x + w * U.x * dt, (float)width);
            c.pos.y = wrapCoord(c.pos.y + w * U.y * dt, (float)height);
            c.depth = clampDepth(c.depth + w * U.d * dt);
        } else if (current_.active()) {
            Vec2 cur = current_.velocityAt(c.pos);
            c.pos.x += cur.x * dt;
            c.pos.y += cur.y * dt;
        }
        // The preview's two-step wrap (`if (x < 0) x += W; if (x >= W) x -= W;`)
        // is wrapCoord's own one-pass branch, so wrapCoord gives the same bits
        // for every x in [-W, 2·W), and wraps a multi-span excursion too.
        c.pos.x = wrapCoord(c.pos.x, (float)width);
        c.pos.y = wrapCoord(c.pos.y, (float)height);
        hot_[i] = {c.pos, c.vel, c.depth};

        c.updateBody();

        kernel::stepInternalState(c, actions, dt);
        kernel::stepMetabolism(c, cfg, dt);
        // Depth costs (0 by default): holding off neutral depth, and pressure.
        if (ocfg.cHold != 0.0f || ocfg.cPress != 0.0f) {
            if (ocfg.depthCostScaled) {
                // E1: dimensionless multiples of this fish's metabolism.
                const float H = std::abs(c.depth - c.bodyBuoy * D) / D;
                const float P = c.depth / D;
                const float M = cfg.metabolicRate * massFactor(c);
                c.foodReserves -= dt * M * (ocfg.cHold * H + ocfg.cPress * P);
            } else {
                // The preview's: energy/s per unit H / P.
                c.foodReserves -= dt * (ocfg.cHold * std::abs(c.depth - c.bodyBuoy * D) / D
                                        + ocfg.cPress * c.depth / D);
            }
        }

        if (actions[EMIT_SIGNAL0] > 0.0f) pending_[i] = {true, c.pos};
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
            if (ocfg.sizeFight)   // E1: strength ∝ L² (same two draws)
                kernel::fightOutcome(c1, c2, c1.bodySize * c1.bodySize, c2.bodySize * c2.bodySize);
            else
                kernel::fightOutcome(c1, c2);
            break;
        default: break;
        }
    }

    void computeSensors(const Fish& c, int selfIdx, float sensors[]) {
        const Vec2 fwd = c.forward();
        const Vec2 rht = {-fwd.y, fwd.x};
        const float R = cfg.perceptionRadius, R2 = R * R;

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
            if (ocfg.mouthGate && food[fi].size > ocfg.kMouth * c.bodySize) return;   // E1: not food for me
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
        sensors[SIGNAL0] = signals.read(c.pos);
        sensors[SIGNAL0_FWD] = signals.readInDir(c.pos, fwd) - signals.readInDir(c.pos, {-fwd.x, -fwd.y});
        sensors[SIGNAL0_LR] = signals.readInDir(c.pos, rht) - signals.readInDir(c.pos, {-rht.x, -rht.y});
        if (flowE1()) {
            // E1: the water's horizontal flow here (deep current × f(d) + waves).
            const Flow U = flowFast(c.pos, c.depth);
            sensors[CURRENT_FWD] = evomath::tanhf((U.x * fwd.x + U.y * fwd.y) / 100.0f);
            sensors[CURRENT_LR]  = evomath::tanhf((U.x * rht.x + U.y * rht.y) / 100.0f);
        } else if (current_.active()) {
            Vec2 cur = current_.velocityAt(c.pos);
            sensors[CURRENT_FWD] = evomath::tanhf((cur.x * fwd.x + cur.y * fwd.y) / 100.0f);
            sensors[CURRENT_LR]  = evomath::tanhf((cur.x * rht.x + cur.y * rht.y) / 100.0f);
        } else {
            sensors[CURRENT_FWD] = 0.0f;
            sensors[CURRENT_LR]  = 0.0f;
        }

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
            if (flowE1()) {
                // E1: carried by the 3-D flow (weight 1), depth clamped after.
                const Flow U = flowFast(p.pos, p.depth);
                p.pos.x = wrapCoord(p.pos.x + U.x * dt, (float)width);
                p.pos.y = wrapCoord(p.pos.y + U.y * dt, (float)height);
                p.depth = clampDepth(p.depth + U.d * dt);
            } else if (current_.active()) {
                const Vec2 cur = current_.velocityAt(p.pos);
                p.pos.x += cur.x * dt;
                p.pos.y += cur.y * dt;
            }
            p.pos.x = wrapPreview(p.pos.x, (float)width);
            p.pos.y = wrapPreview(p.pos.y, (float)height);

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
