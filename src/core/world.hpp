#pragma once
#include <vector>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <cstring>
#include <cstdint>
#include <chrono>
#include <cstdio>
#include <utility>
#include "vec2.hpp"
#include "creature.hpp"
#include "predator.hpp"
#include "quadtree.hpp"
#include "spatial_grid.hpp"
#include "config.hpp"
#include "current.hpp"
#include "cluster_projection.hpp"

struct Food {
    Vec2 pos;
    bool alive = true;
};

// Pheromone signal layer overlaying the world (from biosim4)
class SignalGrid {
    int w_, h_, cellSize_;
    int cols_, rows_;
    std::vector<uint8_t> grid_;

public:
    void init(int worldW, int worldH, int cellSize = 8) {
        w_ = worldW; h_ = worldH; cellSize_ = cellSize;
        cols_ = worldW / cellSize + 1;
        rows_ = worldH / cellSize + 1;
        grid_.assign(cols_ * rows_, 0);
    }

    void emit(Vec2 pos, int radius = 2) {
        int cx = static_cast<int>(pos.x) / cellSize_;
        int cy = static_cast<int>(pos.y) / cellSize_;
        for (int dy = -radius; dy <= radius; ++dy) {
            for (int dx = -radius; dx <= radius; ++dx) {
                int x = cx + dx, y = cy + dy;
                if (x >= 0 && x < cols_ && y >= 0 && y < rows_) {
                    int idx = y * cols_ + x;
                    grid_[idx] = static_cast<uint8_t>(std::min(255, grid_[idx] + 64));
                }
            }
        }
    }

    float read(Vec2 pos) const {
        int x = std::clamp(static_cast<int>(pos.x) / cellSize_, 0, cols_ - 1);
        int y = std::clamp(static_cast<int>(pos.y) / cellSize_, 0, rows_ - 1);
        return grid_[y * cols_ + x] / 255.0f;
    }

    float readInDir(Vec2 pos, Vec2 dir, float dist = 24.0f) const {
        return read({pos.x + dir.x * dist, pos.y + dir.y * dist});
    }

    void decay() {
        for (auto& v : grid_) {
            if (v > 0) v = static_cast<uint8_t>(v * 0.97f);
        }
    }

    // --- Checkpoint access ---
    // The pheromone layer is live state, not a derived view: creatures emit into
    // it, it decays every third frame, and three sensors read it. A checkpoint
    // that skipped it would resume into a world where the trails had vanished.
    const std::vector<uint8_t>& data() const { return grid_; }
    int cellSize() const { return cellSize_; }
    int cols() const { return cols_; }
    int rows() const { return rows_; }
    bool restore(const std::vector<uint8_t>& g) {
        if (g.size() != grid_.size()) return false;   // geometry mismatch
        grid_ = g;
        return true;
    }
};


// Reusable scratch buffers for the per-creature spatial queries. Kept alive
// across creatures (and one set per thread under OpenMP) so neighbour/food/
// probe queries don't each heap-allocate a fresh std::vector every tick.
// One attended neighbour, with the offset already computed so the second pass
// never re-reads its position. Used only when SimConfig::maxNeighbors > 0.
struct NeighborCandidate {
    float distSq;
    float dx, dy;
    int   index;
};

// Upper bound on the topological attention cap. Real animals attend to a
// handful (see SimConfig::maxNeighbors), so this is far above any meaningful
// setting; it exists so the nearest-N selection can use a fixed stack array
// and never allocate. Materialising the candidate list instead measured
// SLOWER than no cap at all, because writing ~150 candidates per creature per
// step costs more memory traffic than the per-neighbour work it skips.
constexpr int MAX_TOPOLOGICAL_NEIGHBORS = 64;

struct SensorScratch {
    std::vector<int> neighbors;
    std::vector<int> nearbyFood;
    std::vector<int> longProbe;
};


class World {
public:
    int width, height;
    std::vector<Creature> creatures;
    std::vector<Predator> predators;   // scripted hunters (empty unless cfg.predatorCount>0).
                                       // Fixed-count/immortal in Stage A; a self-regulating
                                       // population with an energy economy in Stage B.
    std::vector<Food> food;
    SignalGrid signals;

    unsigned long long frame = 0;
    int births = 0;          // this-frame only; reset every update()
    int deaths = 0;          // this-frame only; reset every update()
    long long totalBirths = 0;  // cumulative over the whole run (never reset)
    long long totalDeaths = 0;  // cumulative over the whole run (never reset)

    // Cumulative deaths split by cause (sum == totalDeaths). "prey deaths up" is
    // meaningless until you know which kind, so the logger differences these into
    // per-interval predation / starvation / age columns.
    long long totalDeathsPredation  = 0;
    long long totalDeathsStarvation = 0;
    long long totalDeathsAge        = 0;

    // Predator economy bookkeeping (Stage B). Cumulative over the run; the logger
    // differences them into per-interval columns, exactly like the prey deaths.
    // Births and the two death causes are tracked separately so a predator crash
    // can be diagnosed (starved out vs aged out) and recovery (births) watched.
    // All stay 0 in Stage A (cfg.predatorEconomy == false).
    long long totalPredatorBirths           = 0;
    long long totalPredatorDeathsStarvation = 0;
    long long totalPredatorDeathsAge        = 0;

    // Death-cluster map (cfg.clusterMap). One point per creature death: the 2-D
    // projection of its network plus the death ordinal (so colour can band by
    // epoch) and a few attributes. Memory is bounded to cfg.clusterMax by uniform
    // decimation (when full, drop every other kept point and double the stride),
    // which keeps an evenly-spaced sample across the whole run. Empty / untouched
    // unless cfg.clusterMap is on, so default runs are unaffected.
    struct DeathRecord {
        float x = 0.0f, y = 0.0f;   // fixed 2-D projection (the non-PCA map point)
        float sketch[CLUSTER_SKETCH_DIM] = {};  // K-dim sketch, for end-of-run PCA
        long long index = 0;        // death ordinal over the run (0-based)
        std::uint64_t lineageId = 0;
        float age = 0.0f;           // age at death
        int genomeLen = 0;
        char cause = '?';           // 'p' predation, 's' starvation, 'a' age
    };
    std::vector<DeathRecord> deathLog;
    long long totalDeathsLogged_ = 0;   // every death seen (not just kept)
    long long clusterStride_     = 1;   // keep a death iff (ordinal % stride == 0)

    // Stable identity counter (see Creature::id). Assigned single-threaded at every
    // creation site, so it never touches the parallel phase or the global rng — the
    // simulation stays byte-for-byte and deterministic regardless of ids.
    std::uint64_t nextId_ = 1;

    // ---- Phase profiler (cfg.profile). Accumulates wall-time per phase plus a
    // CPU-time split inside the parallel loop (gather vs feedForward vs apply,
    // summed over threads). Zero cost when off (no clock calls). buildNs/parWallNs/
    // serialNs are WALL time and sum to ~the step; gather/ff/apply are thread-summed
    // CPU time, so on T threads they total ~T * parWallNs — read them as a ratio.
    struct PhaseProfile {
        double buildNs = 0, parWallNs = 0, serialNs = 0, totalNs = 0;
        double gatherNs = 0, ffNs = 0, applyNs = 0;   // thread-summed, within parallel
        long long steps = 0;
    } prof;

    void dumpProfile() const {
        if (prof.steps == 0) { std::printf("(no profiled steps)\n"); return; }
        const double s = (double)prof.steps, us = 1e-3;
        const double step = prof.totalNs / s * us;
        auto pct = [&](double ns){ return prof.totalNs > 0 ? 100.0 * ns / prof.totalNs : 0.0; };
        const double parCpu = prof.gatherNs + prof.ffNs + prof.applyNs;
        auto cpu = [&](double ns){ return parCpu > 0 ? 100.0 * ns / parCpu : 0.0; };
        std::printf("=== phase profile over %lld steps (%.3f ms/step avg) ===\n", prof.steps, step);
        std::printf("  build        %7.3f us/step  (%4.1f%% wall)\n", prof.buildNs / s * us, pct(prof.buildNs));
        std::printf("  parallel     %7.3f us/step  (%4.1f%% wall)\n", prof.parWallNs / s * us, pct(prof.parWallNs));
        std::printf("  serial       %7.3f us/step  (%4.1f%% wall)\n", prof.serialNs / s * us, pct(prof.serialNs));
        std::printf("  -- inside parallel (thread-summed CPU, share of parallel work) --\n");
        std::printf("     sensor-gather %5.1f%%   feedForward %5.1f%%   apply/metab %5.1f%%\n",
                    cpu(prof.gatherNs), cpu(prof.ffNs), cpu(prof.applyNs));
    }

    // Record one death into the cluster map (no-op unless cfg.clusterMap). Called
    // from the single-threaded death paths, so it is deterministic and thread-safe.
    void logDeath(const Creature& c, char cause) {
        if (!cfg.clusterMap) return;
        const long long ordinal = totalDeathsLogged_++;
        if (ordinal % clusterStride_ != 0) return;          // decimated out
        DeathRecord rec;
        clusterSketch(c.genome, rec.sketch);                 // K-dim, for optional PCA
        const Vec2 p = sketchTo2D(rec.sketch);               // fixed 2-D map point
        rec.x = p.x; rec.y = p.y;
        rec.index = ordinal; rec.lineageId = c.lineageId;
        rec.age = c.age; rec.genomeLen = (int)c.genome.size(); rec.cause = cause;
        deathLog.push_back(rec);
        if (cfg.clusterMax > 0 && (int)deathLog.size() >= 2 * cfg.clusterMax) {
            // Halve: keep every other kept point, then require twice the spacing.
            const long long newStride = clusterStride_ * 2;
            std::vector<DeathRecord> kept;
            kept.reserve(deathLog.size() / 2 + 1);
            for (const DeathRecord& d : deathLog)
                if (d.index % newStride == 0) kept.push_back(d);
            deathLog.swap(kept);
            clusterStride_ = newStride;
        }
    }

    // Runtime-tunable parameters (see SimConfig). Set World::cfg before init().
    SimConfig cfg;

    // Structural constant, kept compile-time (not a tuning knob).
    static constexpr float LONGPROBE_DIST = 200.0f;

    void init(int w, int h, int numCreatures) {
        width = w;
        height = h;
        signals.init(w, h);
        creatureGrid_.init(w, h, cfg.perceptionRadius);
        foodGrid_.init(w, h, cfg.perceptionRadius);

        CurrentField::Mode cmode = (cfg.currentMode == 1) ? CurrentField::Mode::Gyre
                                                           : CurrentField::Mode::Eddies;
        current_.init(cmode, w, h, cfg.currentEddies, cfg.currentStrength,
                      cfg.currentCore, cfg.currentDrift, cfg.currentGyreEps);
        foodPatches_.clear();
        if (cfg.foodPatches > 0)
            for (int i = 0; i < cfg.foodPatches; ++i)
                foodPatches_.push_back({rng.range(0.0f, (float)w), rng.range(0.0f, (float)h)});

        creatures.clear();
        creatures.reserve(numCreatures * 2);
        for (int i = 0; i < numCreatures; ++i) {
            Vec2 pos = {rng.range(10.0f, (float)w - 10.0f),
                        rng.range(10.0f, (float)h - 10.0f)};
            creatures.push_back(makeRandomCreature(pos, cfg.genomeInitMin, cfg.genomeInitMax));
            creatures.back().id = nextId_++;
            creatures.back().lineageId = creatures.back().id;   // a new founder line
        }

        // Predators: a fixed handful dropped at random positions with a random
        // heading. No genome / neural net (that is Stage C). The energy field is
        // seeded to predatorNewbornEnergy for Stage B; Stage A never reads it.
        predators.clear();
        for (int i = 0; i < cfg.predatorCount; ++i) {
            Predator p;
            p.pos = {rng.range(10.0f, (float)w - 10.0f),
                     rng.range(10.0f, (float)h - 10.0f)};
            const float a = rng.range(0.0f, 6.2832f);
            p.vel = {std::cos(a), std::sin(a)};
            p.energy = cfg.predatorNewbornEnergy;
            predators.push_back(p);
        }

        food.clear();
        for (int i = 0; i < cfg.targetFood; ++i) spawnFood();
    }

    void spawnFood() {
        Food f;
        if (!foodPatches_.empty()) {
            // Cluster around a random patch centre, so spatial position matters
            // (a current that pushes a creature off a patch now costs it).
            const Vec2& c = foodPatches_[rng.uint((uint32_t)foodPatches_.size() - 1)];
            const float R = cfg.foodPatchRadius;
            f.pos = { std::clamp(c.x + rng.range(-R, R), 0.0f, (float)width),
                      std::clamp(c.y + rng.range(-R, R), 0.0f, (float)height) };
        } else {
            f.pos = {rng.range(10.0f, (float)width - 10.0f),
                     rng.range(10.0f, (float)height - 10.0f)};
        }
        food.push_back(f);
    }

    void update(float dt) {
        using clk = std::chrono::steady_clock;
        const bool prof_ = cfg.profile;
        const auto tTotal0 = prof_ ? clk::now() : clk::time_point{};

        frame++;
        births = 0;
        deaths = 0;

        current_.advance(frame);  // refresh vortex centres before any queries

        // Drift food with the current (divergence-free => food stretches into
        // filaments rather than piling up). Done before the grid rebuild so the
        // spatial index reflects the new positions.
        if (cfg.foodDrift && current_.active()) {
            for (auto& f : food) {
                if (!f.alive) continue;
                Vec2 v = current_.velocityAt(f.pos);
                f.pos.x += v.x * dt;  f.pos.y += v.y * dt;
                if (f.pos.x < 0) f.pos.x += width;  else if (f.pos.x >= width)  f.pos.x -= width;
                if (f.pos.y < 0) f.pos.y += height; else if (f.pos.y >= height) f.pos.y -= height;
            }
        }

        // Food spawning with abundance cycles (from learning_boids)
        float foodRate = cfg.foodSpawnBase + cfg.foodSpawnAmp * std::sin(frame * 0.001f);
        for (int i = 0; i < static_cast<int>(foodRate); ++i) {
            if (static_cast<int>(food.size()) < cfg.targetFood * 2) spawnFood();
        }

        // Decay pheromones
        if (frame % 3 == 0) signals.decay();

        // Rebuild spatial indices. The grids are members reused across frames:
        // clear() empties the buckets but keeps their capacity, so the steady
        // state performs no per-tick heap allocation (unlike the old QuadTrees,
        // which were new/delete-constructed every update).
        creatureGrid_.clear();
        foodGrid_.clear();

        int numCreatures = (int)creatures.size();
        int numFood = (int)food.size();

        const auto tBuild0 = prof_ ? clk::now() : clk::time_point{};
        for (int i = 0; i < numCreatures; ++i) {
            if (creatures[i].alive) creatureGrid_.insert(creatures[i].pos, i);
        }
        for (int i = 0; i < numFood; ++i) {
            if (food[i].alive) foodGrid_.insert(food[i].pos, i);
        }
        if (prof_) prof.buildNs += std::chrono::duration<double, std::nano>(clk::now() - tBuild0).count();

        // Per-creature pending actions (for thread safety)
        struct PendingActions {
            bool wantsSignal = false;
            Vec2 signalPos;
        };
        std::vector<PendingActions> pending(numCreatures);

        // Thread-summed CPU time for the three sub-steps inside the parallel loop
        // (reduction => safe across OpenMP threads; plain accumulation when serial).
        double gNs = 0, fNs = 0, mNs = 0;
        const auto tPar0 = prof_ ? clk::now() : clk::time_point{};

        // ============================================================
        // PARALLEL PHASE: sensors + neural net + movement
        // Each creature only modifies itself. Read-only on quadtrees.
        // ============================================================
        #ifdef USE_OPENMP
        #pragma omp parallel
        #endif
        {
        // Per-thread scratch: one set per thread under OpenMP, a single reused
        // set in the serial build. The spatial queries inside computeSensors
        // reuse these buffers instead of allocating per creature each tick.
        SensorScratch scratch;

        #ifdef USE_OPENMP
        #pragma omp for schedule(dynamic, 16) reduction(+:gNs,fNs,mNs)
        #endif
        for (int i = 0; i < numCreatures; ++i) {
            Creature& c = creatures[i];
            if (!c.alive) continue;

            std::chrono::steady_clock::time_point ta, tb, tc;
            if (prof_) ta = std::chrono::steady_clock::now();

            // Compute all sensors
            float sensors[NUM_SENSES];
            std::memset(sensors, 0, sizeof(sensors));
            computeSensors(c, i, creatureGrid_, foodGrid_, sensors, scratch);

            if (prof_) tb = std::chrono::steady_clock::now();

            // Feed through neural net
            float actions[NUM_ACTIONS];
            std::memset(actions, 0, sizeof(actions));
            feedForward(c.nnet, sensors, actions, cfg.fastTanh);

            if (prof_) tc = std::chrono::steady_clock::now();

            // Capture neural I/O for the live inspector, only when asked (off in
            // headless). Each creature writes its own buffers, so this is safe in
            // the parallel phase and never perturbs the dynamics.
            if (cfg.captureActivations) {
                std::memcpy(c.lastSensors, sensors, sizeof(sensors));
                std::memcpy(c.lastActions, actions, sizeof(actions));
            }

            // Apply responsiveness scaling
            float resp = c.responsiveness;

          if (cfg.triangularMotion) {
            // --- Triangular action space: forward drive throttles turn authority ---
            // drive in [0,1] from MOVE_FORWARD; turn in [-1,1] from STEER_X. The
            // coupling turnEff = turn*(1-drive) is the whole point: a fast strike
            // (drive->1) cannot also turn, a reorient (drive->0) turns freely.
            float drive = std::clamp(0.5f * (actions[MOVE_FORWARD] + 1.0f), 0.0f, 1.0f);
            float turn  = actions[STEER_X];
            float turnEff = turn * (1.0f - drive) * resp;

            float theta = std::atan2(c.vel.y, c.vel.x);
            theta += turnEff * cfg.maxTurnRate * dt;
            c.vel.x = std::cos(theta);
            c.vel.y = std::sin(theta);

            c.speed = std::clamp(cfg.minSpeed + drive * (cfg.maxSpeed - cfg.minSpeed),
                                 cfg.minSpeed, cfg.maxSpeed);

            c.lastMoveDir = c.vel;
            c.pos.x += c.vel.x * c.speed * dt;
            c.pos.y += c.vel.y * c.speed * dt;
          } else {
            // Movement actions (original decoupled model)
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
            }

            float vlen = std::sqrt(c.vel.x * c.vel.x + c.vel.y * c.vel.y);
            if (vlen > 0.001f) { c.vel.x /= vlen; c.vel.y /= vlen; }

            c.speed += speedAdj * 50.0f * dt;
            c.speed = std::clamp(c.speed, cfg.minSpeed, cfg.maxSpeed);

            c.lastMoveDir = c.vel;

            c.pos.x += c.vel.x * c.speed * dt;
            c.pos.y += c.vel.y * c.speed * dt;
          }

            // Ocean current pushes the creature (read-only on the cached field).
            if (current_.active()) {
                Vec2 cur = current_.velocityAt(c.pos);
                c.pos.x += cur.x * dt;
                c.pos.y += cur.y * dt;
            }

            if (c.pos.x < 0) c.pos.x += width;
            if (c.pos.x >= width) c.pos.x -= width;
            if (c.pos.y < 0) c.pos.y += height;
            if (c.pos.y >= height) c.pos.y -= height;

            c.updateBody();

            // Internal state actions
            c.oscPeriod = std::clamp(c.oscPeriod + actions[SET_OSCILLATOR_PERIOD] * 5.0f * dt,
                                     4.0f, 200.0f);
            c.responsiveness = std::clamp(
                c.responsiveness + actions[SET_RESPONSIVENESS] * 0.5f * dt,
                0.01f, 1.0f);

            c.oscPhase += dt * (6.28f / c.oscPeriod);
            if (c.oscPhase > 6.28f) c.oscPhase -= 6.28f;

            // Age and metabolism
            c.age += dt;
            // Metabolism: a fixed burn, an optional per-gene surcharge (longer
            // genomes must earn their length), plus an optional kinetic cost so
            // thrust isn't free — moveCost*speed^2 makes fast movement (e.g.
            // fighting a current) expensive and gives an interior optimal speed.
            c.foodReserves -= dt * (cfg.metabolicRate
                                    + cfg.geneCost * (float)c.genome.size()
                                    + cfg.moveCost * c.speed * c.speed);
            c.reproTimer -= dt;
            c.interactTimer -= dt;

            // Defer signal emission (shared grid, not thread-safe)
            if (actions[EMIT_SIGNAL0] > 0.0f) {
                pending[i].wantsSignal = true;
                pending[i].signalPos = c.pos;
            }

            if (prof_) {
                const auto td = std::chrono::steady_clock::now();
                gNs += std::chrono::duration<double, std::nano>(tb - ta).count();
                fNs += std::chrono::duration<double, std::nano>(tc - tb).count();
                mNs += std::chrono::duration<double, std::nano>(td - tc).count();
            }
        }
        } // end parallel region

        if (prof_) {
            prof.parWallNs += std::chrono::duration<double, std::nano>(clk::now() - tPar0).count();
            prof.gatherNs += gNs; prof.ffNs += fNs; prof.applyNs += mNs;
        }
        const auto tSerial0 = prof_ ? clk::now() : clk::time_point{};

        // ============================================================
        // SINGLE-THREADED PHASE: shared-state modifications
        // ============================================================
        std::vector<Creature> newborns;

        // Reused across the loop (this phase is single-threaded) so the
        // eat/interact neighbour queries don't allocate per creature.
        std::vector<int> nearbyFood;
        std::vector<int> nearby;

        for (int i = 0; i < numCreatures; ++i) {
            Creature& c = creatures[i];
            if (!c.alive) continue;

            // Process deferred signal emissions
            if (pending[i].wantsSignal) {
                signals.emit(pending[i].signalPos);
            }

            // Death check (split by cause for the deaths decomposition).
            if (c.foodReserves <= 0.0f) {
                c.alive = false;
                deaths++; totalDeaths++; totalDeathsStarvation++;
                logDeath(c, 's');
                continue;
            }
            if (c.age > cfg.maxAge) {
                c.alive = false;
                deaths++; totalDeaths++; totalDeathsAge++;
                logDeath(c, 'a');
                continue;
            }

            // Eat nearby food (modifies food[].alive)
            nearbyFood.clear();
            foodGrid_.queryRadius(c.pos, cfg.pickupRadius, nearbyFood);
            for (int fi : nearbyFood) {
                if (food[fi].alive) {
                    food[fi].alive = false;
                    c.foodReserves += cfg.foodEnergy;
                }
            }

            // Social interactions
            if (c.interactTimer <= 0.0f) {
                nearby.clear();
                creatureGrid_.queryRadius(c.pos, cfg.interactRadius, nearby);
                for (int ni : nearby) {
                    if (ni == i) continue;
                    Creature& other = creatures[ni];
                    if (!other.alive || other.interactTimer > 0.0f) continue;

                    doInteraction(c, other, newborns);
                    c.interactTimer = cfg.interactCooldown;
                    other.interactTimer = cfg.interactCooldown;
                    break;
                }
            }
        }

        for (auto& baby : newborns) { baby.id = nextId_++; creatures.push_back(std::move(baby)); }

        // Predator hunt + (Stage B) energy economy. Runs AFTER prey have moved
        // (parallel phase) and after the prey single-threaded pass, while
        // creatureGrid_ indices still line up with `creatures` (the dead-removal
        // erase below would invalidate them). No-op when there are no predators.
        updatePredators(dt);

        creatures.erase(
            std::remove_if(creatures.begin(), creatures.end(),
                           [](const Creature& c) { return !c.alive; }),
            creatures.end());
        food.erase(
            std::remove_if(food.begin(), food.end(),
                           [](const Food& f) { return !f.alive; }),
            food.end());

        if ((int)creatures.size() < cfg.minPopulation) {
            int toAdd = cfg.minPopulation - (int)creatures.size();
            for (int j = 0; j < toAdd; ++j) {
                Vec2 pos = {rng.range(10.0f, (float)width - 10.0f),
                            rng.range(10.0f, (float)height - 10.0f)};
                creatures.push_back(makeRandomCreature(pos, cfg.genomeInitMin, cfg.genomeInitMax));
                creatures.back().id = nextId_++;
                creatures.back().lineageId = creatures.back().id;   // new founder line
            }
        }

        if (prof_) {
            prof.serialNs += std::chrono::duration<double, std::nano>(clk::now() - tSerial0).count();
            prof.totalNs  += std::chrono::duration<double, std::nano>(clk::now() - tTotal0).count();
            ++prof.steps;
        }
    }

    // --- read-only access to the current field (used by the renderer overlay) ---
    bool currentActive() const { return current_.active(); }
    Vec2 currentAt(Vec2 p) const { return current_.velocityAt(p); }

    // --- Checkpoint access (see checkpoint_io.hpp) ---
    // Both of these are seeded from the rng during init(), so a resumed run
    // cannot regenerate them — they have to be restored verbatim or the world
    // is subtly different (a different ocean, food patches in different places).
    std::vector<float> currentVortexState() const { return current_.vortexState(); }
    bool restoreCurrentVortexState(const std::vector<float>& v) { return current_.restoreVortexState(v); }
    const std::vector<Vec2>& foodPatches() const { return foodPatches_; }
    void restoreFoodPatches(std::vector<Vec2> p) { foodPatches_ = std::move(p); }

private:
    // Spatial indices, reused across frames (see update()).
    SpatialGrid creatureGrid_;
    SpatialGrid foodGrid_;
    CurrentField current_;          // ocean flow (off unless cfg.currentStrength>0)
    std::vector<Vec2> foodPatches_; // patch centres (empty => uniform food spawn)

    // The scripted hunt + (Stage B) energy economy. For each predator: find the
    // nearest LIVING prey in its detection radius, steer toward it (else wander),
    // advance at predator cruise, and — if a prey is within the capture radius and
    // the predator is not digesting — kill it (instant capture) and start the
    // digestion cooldown. Runs single-threaded (a handful of predators), reading
    // the creature grid built earlier this frame.
    //
    // Stage A (cfg.predatorEconomy == false): no energy, no death, no births —
    // the block below collapses to exactly the original Stage A behaviour.
    // Stage B (cfg.predatorEconomy == true): each predator also ages, burns
    // metabolism + a kinetic move-cost, gains energy = the victim's reserves (or
    // a fixed --predator-meal) on a capture, buds a child above predatorReproEnergy
    // (paying predatorNewbornEnergy, so budding conserves energy), and dies below
    // 0 energy (starvation) or past predatorMaxAge (age). Newborns are appended and
    // the dead erased AFTER the loop (the prey remove_if pattern) so neither
    // invalidates the predator we're iterating.
    void updatePredators(float dt) {
        if (predators.empty()) return;
        const bool  econ     = cfg.predatorEconomy;
        // Predator kinetic coefficient: reuse the prey --move-cost when <0.
        const float pmc      = (cfg.predatorMoveCost >= 0.0f) ? cfg.predatorMoveCost
                                                              : cfg.moveCost;
        // Predators cruise at a fixed speed (Stage A/B), so the per-second burn is
        // constant: fixed metabolism + the quadratic kinetic cost of that cruise.
        const float burnRate = cfg.predatorMetabolism
                             + pmc * cfg.predatorSpeed * cfg.predatorSpeed;
        const float detect2  = cfg.predatorPerception * cfg.predatorPerception;
        const float capture2 = cfg.predatorCaptureRadius * cfg.predatorCaptureRadius;

        std::vector<Predator> newborns;   // budded this frame (appended after the loop)
        std::vector<int> near;
        for (Predator& p : predators) {
            p.digestTimer -= dt;
            if (econ) {
                p.age    += dt;
                p.energy -= burnRate * dt;   // metabolism + kinetic move-cost
            }

            // Nearest living prey within the detection radius.
            near.clear();
            creatureGrid_.queryRadius(p.pos, cfg.predatorPerception, near);
            int   bestIdx = -1;
            float bestD2  = detect2;
            for (int ci : near) {
                const Creature& prey = creatures[ci];
                if (!prey.alive) continue;
                const float dx = prey.pos.x - p.pos.x;
                const float dy = prey.pos.y - p.pos.y;
                const float d2 = dx * dx + dy * dy;
                if (d2 < bestD2) { bestD2 = d2; bestIdx = ci; }
            }

            // Steer toward the target, or wander if none is in range.
            Vec2 dir = p.forward();
            if (bestIdx >= 0) {
                dir = normalized(creatures[bestIdx].pos - p.pos);
            } else {
                const float jitter = rng.range(-0.3f, 0.3f);
                const float cs = std::cos(jitter), sn = std::sin(jitter);
                dir = {dir.x * cs - dir.y * sn, dir.x * sn + dir.y * cs};
            }
            p.vel = dir;

            // Advance at a fixed cruise speed. The current carries the predator
            // too, so phase 2 (current on) stays kinematically consistent.
            p.pos.x += dir.x * cfg.predatorSpeed * dt;
            p.pos.y += dir.y * cfg.predatorSpeed * dt;
            if (current_.active()) {
                const Vec2 cur = current_.velocityAt(p.pos);
                p.pos.x += cur.x * dt;
                p.pos.y += cur.y * dt;
            }
            if (p.pos.x < 0) p.pos.x += width;  else if (p.pos.x >= width)  p.pos.x -= width;
            if (p.pos.y < 0) p.pos.y += height; else if (p.pos.y >= height) p.pos.y -= height;

            // Capture: nearest prey within reach, predator not digesting.
            if (bestIdx >= 0 && p.digestTimer <= 0.0f && creatures[bestIdx].alive) {
                const float dx = creatures[bestIdx].pos.x - p.pos.x;
                const float dy = creatures[bestIdx].pos.y - p.pos.y;
                if (dx * dx + dy * dy <= capture2) {
                    if (econ) {
                        // Carnivory is calorie-dense: absorb the victim's reserves
                        // (or a fixed meal when --predator-meal >= 0).
                        const float meal = (cfg.predatorMeal >= 0.0f)
                                           ? cfg.predatorMeal
                                           : creatures[bestIdx].foodReserves;
                        p.energy += meal;
                    }
                    creatures[bestIdx].alive = false;
                    deaths++; totalDeaths++; totalDeathsPredation++;
                    logDeath(creatures[bestIdx], 'p');
                    p.digestTimer = cfg.predatorCooldown;
                }
            }

            // Stage B life/death: starve, age out, or bud. (Order: a predator that
            // just ate may have crossed back above 0 this frame and survives; a
            // dying or aging predator does not reproduce this frame.)
            if (econ) {
                if (p.energy < 0.0f) {
                    p.alive = false;
                    totalPredatorDeathsStarvation++;
                } else if (cfg.predatorMaxAge > 0.0f && p.age >= cfg.predatorMaxAge) {
                    p.alive = false;
                    totalPredatorDeathsAge++;
                } else if (p.energy >= cfg.predatorReproEnergy) {
                    Predator child;
                    // Place the child a hair off the parent with a random heading.
                    child.pos = { std::clamp(p.pos.x + rng.range(-8.0f, 8.0f), 0.0f, (float)width  - 1.0f),
                                  std::clamp(p.pos.y + rng.range(-8.0f, 8.0f), 0.0f, (float)height - 1.0f) };
                    const float a = rng.range(0.0f, 6.2832f);
                    child.vel    = {std::cos(a), std::sin(a)};
                    child.energy = cfg.predatorNewbornEnergy;
                    p.energy    -= cfg.predatorNewbornEnergy;   // energy-conserving
                    newborns.push_back(child);
                    totalPredatorBirths++;
                }
            }
        }

        // Apply births and remove the dead (Stage B only; Stage A leaves the
        // predator vector untouched, so its size is constant as before).
        if (econ) {
            for (auto& baby : newborns) predators.push_back(std::move(baby));
            predators.erase(
                std::remove_if(predators.begin(), predators.end(),
                               [](const Predator& p) { return !p.alive; }),
                predators.end());
        }
    }

    void doInteraction(Creature& c1, Creature& c2, std::vector<Creature>& newborns) {
        float s1[NUM_SENSES], s2[NUM_SENSES];
        std::memset(s1, 0, sizeof(s1));
        std::memset(s2, 0, sizeof(s2));

        s1[AGE] = std::tanh((c1.age - 50.0f) / 50.0f);
        s1[NEAREST_NEIGHBOR_AGE] = std::tanh((c2.age - 50.0f) / 50.0f);
        s1[FOOD_RESERVES] = std::tanh((c1.foodReserves - 25.0f) / 25.0f);
        s1[NEAREST_NEIGHBOR_FOOD] = std::tanh((c2.foodReserves - 25.0f) / 25.0f);
        s1[GENETIC_SIM_NEAREST] = genomeSimilarity(c1.genome, c2.genome);

        s2[AGE] = std::tanh((c2.age - 50.0f) / 50.0f);
        s2[NEAREST_NEIGHBOR_AGE] = std::tanh((c1.age - 50.0f) / 50.0f);
        s2[FOOD_RESERVES] = std::tanh((c2.foodReserves - 25.0f) / 25.0f);
        s2[NEAREST_NEIGHBOR_FOOD] = std::tanh((c1.foodReserves - 25.0f) / 25.0f);
        s2[GENETIC_SIM_NEAREST] = genomeSimilarity(c2.genome, c1.genome);

        float a1[NUM_ACTIONS], a2[NUM_ACTIONS];
        feedForward(c1.nnet, s1, a1);
        feedForward(c2.nnet, s2, a2);

        float sShare = a1[INTERACT_SHARE] + a2[INTERACT_SHARE];
        float sRepro = a1[INTERACT_REPRODUCE] + a2[INTERACT_REPRODUCE];
        float sFight = a1[INTERACT_FIGHT] + a2[INTERACT_FIGHT];
        float sIgnore = a1[INTERACT_IGNORE] + a2[INTERACT_IGNORE];

        float maxScore = sIgnore;
        int choice = 0;
        if (sShare > maxScore) { maxScore = sShare; choice = 1; }
        if (sRepro > maxScore) { maxScore = sRepro; choice = 2; }
        if (sFight > maxScore) { maxScore = sFight; choice = 3; }

        switch (choice) {
        case 1: {
            float total = c1.foodReserves + c2.foodReserves;
            c1.foodReserves = total * 0.5f;
            c2.foodReserves = total * 0.5f;
            break;
        }
        case 2: {
            if (c1.foodReserves > cfg.reproEnergy && c2.foodReserves > cfg.reproEnergy &&
                c1.reproTimer <= 0.0f && c2.reproTimer <= 0.0f &&
                c1.age > 5.0f && c2.age > 5.0f &&
                (cfg.maxPopulation <= 0 ||
                 (int)creatures.size() + (int)newborns.size() < cfg.maxPopulation)) {
                c1.foodReserves -= 12.0f;
                c2.foodReserves -= 12.0f;
                c1.reproTimer = cfg.reproCooldown;
                c2.reproTimer = cfg.reproCooldown;
                newborns.push_back(makeChild(c1, c2, cfg.genomeMinLength,
                                             cfg.genomeMaxLength,
                                             cfg.mutationMultiplier,
                                             cfg.newbornEnergy));
                newborns.back().lineageId = c1.lineageId;   // inherit maternal line
                births++;
                totalBirths++;
            }
            break;
        }
        case 3: {
            float str1 = (c1.age < 30.0f) ? c1.age / 30.0f : 1.0f - (c1.age - 30.0f) / 90.0f;
            float str2 = (c2.age < 30.0f) ? c2.age / 30.0f : 1.0f - (c2.age - 30.0f) / 90.0f;
            str1 *= rng.unit();
            str2 *= rng.unit();
            if (str1 > str2) {
                c1.foodReserves += 15.0f;
                c2.foodReserves -= 15.0f;
            } else {
                c2.foodReserves += 15.0f;
                c1.foodReserves -= 15.0f;
            }
            break;
        }
        default: break;
        }
    }

    void computeSensors(const Creature& c, int selfIdx,
                        const SpatialGrid& creatureGrid, const SpatialGrid& foodGrid,
                        float sensors[], SensorScratch& scratch) {
        auto fwd = c.forward();
        auto rht = c.right();

        // Position & boundaries
        sensors[LOC_X] = c.pos.x / width;
        sensors[LOC_Y] = c.pos.y / height;
        float bx = std::min(c.pos.x, (float)width - c.pos.x);
        float by = std::min(c.pos.y, (float)height - c.pos.y);
        sensors[BOUNDARY_DIST_X] = std::tanh(bx / 100.0f);
        sensors[BOUNDARY_DIST_Y] = std::tanh(by / 100.0f);
        sensors[BOUNDARY_DIST] = std::tanh(std::min(bx, by) / 100.0f);

        // Own state
        sensors[OWN_VEL_X] = c.vel.x;
        sensors[OWN_VEL_Y] = c.vel.y;
        sensors[OWN_SPEED] = c.speed / cfg.maxSpeed;
        sensors[LAST_MOVE_DIR_X] = c.lastMoveDir.x;
        sensors[LAST_MOVE_DIR_Y] = c.lastMoveDir.y;
        sensors[AGE] = std::tanh(c.age / 60.0f);
        sensors[FOOD_RESERVES] = std::tanh((c.foodReserves - 25.0f) / 25.0f);
        sensors[OSCILLATOR] = c.getOscillator();
        sensors[RESPONSIVENESS] = c.responsiveness;

        // Neighbor queries (reuse scratch buffer instead of allocating)
        std::vector<int>& neighbors = scratch.neighbors;
        neighbors.clear();
        creatureGrid.queryRadius(c.pos, cfg.perceptionRadius, neighbors);

        float avgDx = 0, avgDy = 0, avgHx = 0, avgHy = 0;
        float nearestDist = cfg.perceptionRadius;
        int nearestIdx = -1;
        int neighborCount = 0;
        float popFwd = 0, popLR = 0;

        // One neighbour's contribution to the social percepts. Shared by both
        // paths below so the maths exists once; with cfg.maxNeighbors == 0 the
        // call order is exactly the old inline loop's, hence bit-identical.
        auto attendTo = [&](int ni, const Creature& other, float dx, float dy, float dist) {
            avgDx += dx; avgDy += dy;
            avgHx += other.vel.x; avgHy += other.vel.y;

            float dotFwd = dx * fwd.x + dy * fwd.y;
            float dotLR = dx * rht.x + dy * rht.y;
            if (dotFwd > 0) popFwd += 1.0f; else popFwd -= 1.0f;
            if (dotLR > 0) popLR += 1.0f; else popLR -= 1.0f;

            if (dist < nearestDist) { nearestDist = dist; nearestIdx = ni; }
            neighborCount++;
        };

        // True crowding inside the radius, independent of the attention cap:
        // density is a percept in its own right (see SimConfig::maxNeighbors).
        int inRadiusCount = 0;

        if (cfg.maxNeighbors <= 0) {
            // Metric interaction: attend to everything in range (historical).
            for (int ni : neighbors) {
                if (ni == selfIdx) continue;
                const Creature& other = creatures[ni];
                if (!other.alive) continue;

                float dx = other.pos.x - c.pos.x;
                float dy = other.pos.y - c.pos.y;
                float dist = std::sqrt(dx * dx + dy * dy);
                attendTo(ni, other, dx, dy, dist);
            }
            inRadiusCount = neighborCount;
        } else {
            // Topological interaction: rank candidates by squared distance and
            // attend to the nearest N. Ordering by (distanceSq, index) is a
            // total order, so the selection is deterministic even when two
            // neighbours are exactly equidistant, and independent of the order
            // the spatial index happened to return candidates in.
            // Selection is by (distanceSq, index), a total order, so both the
            // chosen set and the order it is summed in are uniquely determined
            // — no dependence on library sort internals, which is what keeps a
            // run reproducible on another toolchain or under wasm.
            //
            // Kept in a small sorted stack array rather than a candidate list:
            // once it is full, a neighbour that is farther than the current
            // worst is rejected on a single comparison, and in a dense school
            // that is almost every one of them.
            const int keepMax = std::min(cfg.maxNeighbors, MAX_TOPOLOGICAL_NEIGHBORS);
            NeighborCandidate best[MAX_TOPOLOGICAL_NEIGHBORS];
            int have = 0;

            for (int ni : neighbors) {
                if (ni == selfIdx) continue;
                const Creature& other = creatures[ni];
                if (!other.alive) continue;
                ++inRadiusCount;

                const float dx = other.pos.x - c.pos.x;
                const float dy = other.pos.y - c.pos.y;
                const float d2 = dx * dx + dy * dy;

                if (have == keepMax) {
                    const NeighborCandidate& worst = best[have - 1];
                    if (d2 > worst.distSq ||
                        (d2 == worst.distSq && ni > worst.index)) continue;
                }

                int at = (have < keepMax) ? have : have - 1;
                while (at > 0) {
                    const NeighborCandidate& prev = best[at - 1];
                    if (prev.distSq < d2 ||
                        (prev.distSq == d2 && prev.index < ni)) break;
                    best[at] = prev;
                    --at;
                }
                best[at] = { d2, dx, dy, ni };
                if (have < keepMax) ++have;
            }

            for (int j = 0; j < have; ++j) {
                attendTo(best[j].index, creatures[best[j].index],
                         best[j].dx, best[j].dy, std::sqrt(best[j].distSq));
            }
        }

        if (neighborCount > 0) {
            avgDx /= neighborCount; avgDy /= neighborCount;
            avgHx /= neighborCount; avgHy /= neighborCount;
        }

        sensors[NEIGHBOR_COUNT] = std::tanh(inRadiusCount / 10.0f);
        sensors[AVG_NEIGHBOR_DX] = std::tanh(avgDx / cfg.perceptionRadius);
        sensors[AVG_NEIGHBOR_DY] = std::tanh(avgDy / cfg.perceptionRadius);
        sensors[AVG_NEIGHBOR_HEADING_X] = avgHx;
        sensors[AVG_NEIGHBOR_HEADING_Y] = avgHy;
        sensors[NEAREST_NEIGHBOR_DIST] = nearestDist / cfg.perceptionRadius;
        sensors[POPULATION_DENSITY] = std::tanh(inRadiusCount / 15.0f);
        sensors[POPULATION_FWD] = std::tanh(popFwd / 5.0f);
        sensors[POPULATION_LR] = std::tanh(popLR / 5.0f);

        // Nearest neighbor attributes
        if (nearestIdx >= 0) {
            const Creature& nearest = creatures[nearestIdx];
            sensors[NEAREST_NEIGHBOR_AGE] = std::tanh((nearest.age - 50.0f) / 50.0f);
            sensors[NEAREST_NEIGHBOR_FOOD] = std::tanh((nearest.foodReserves - 25.0f) / 25.0f);
            sensors[GENETIC_SIM_NEAREST] = genomeSimilarity(c.genome, nearest.genome);
            float dx = nearest.pos.x - c.pos.x;
            float dy = nearest.pos.y - c.pos.y;
            float dotFwd = dx * fwd.x + dy * fwd.y;
            sensors[GENETIC_SIM_FWD] = (dotFwd > 0)
                ? genomeSimilarity(c.genome, nearest.genome) : 0.5f;
        } else {
            sensors[NEAREST_NEIGHBOR_AGE] = 0.0f;
            sensors[NEAREST_NEIGHBOR_FOOD] = 0.0f;
            sensors[GENETIC_SIM_NEAREST] = 0.0f;
            sensors[GENETIC_SIM_FWD] = 0.5f;
        }

        // Food sensors (reuse scratch buffer)
        std::vector<int>& nearbyFood = scratch.nearbyFood;
        nearbyFood.clear();
        foodGrid.queryRadius(c.pos, cfg.perceptionRadius, nearbyFood);
        float foodDx = 0, foodDy = 0, foodDist = cfg.perceptionRadius;
        for (int fi : nearbyFood) {
            float dx = food[fi].pos.x - c.pos.x;
            float dy = food[fi].pos.y - c.pos.y;
            float dist = std::sqrt(dx * dx + dy * dy);
            if (dist < foodDist) {
                foodDist = dist;
                float invD = (dist > 0.001f) ? 1.0f / dist : 0.0f;
                foodDx = dx * invD;
                foodDy = dy * invD;
            }
        }
        sensors[NEAREST_FOOD_DX] = foodDx;
        sensors[NEAREST_FOOD_DY] = foodDy;
        sensors[NEAREST_FOOD_DIST] = foodDist / cfg.perceptionRadius;
        sensors[FOOD_DENSITY] = std::tanh((float)nearbyFood.size() / 10.0f);

        // Pheromone sensors
        sensors[SIGNAL0] = signals.read(c.pos);
        sensors[SIGNAL0_FWD] = signals.readInDir(c.pos, fwd) - signals.readInDir(c.pos, {-fwd.x, -fwd.y});
        sensors[SIGNAL0_LR] = signals.readInDir(c.pos, rht) - signals.readInDir(c.pos, {-rht.x, -rht.y});

        // Current / flow sensors — local flow projected onto the creature's own
        // forward and right axes (0 everywhere when currents are disabled).
        Vec2 cur = current_.velocityAt(c.pos);
        sensors[CURRENT_FWD] = std::tanh((cur.x * fwd.x + cur.y * fwd.y) / 100.0f);
        sensors[CURRENT_LR]  = std::tanh((cur.x * rht.x + cur.y * rht.y) / 100.0f);

        // Predator threat sensors — nearest predator within the prey's OWN
        // perception radius (the predator's larger detection radius is the ambush
        // asymmetry, handled on the predator side). Bearing is relative to heading;
        // proximity ramps 0 (at the perception edge / none) -> 1 (on contact).
        {
            float bestD2 = cfg.perceptionRadius * cfg.perceptionRadius;
            Vec2  off    = {0.0f, 0.0f};
            bool  seen   = false;
            for (const Predator& p : predators) {
                const float dx = p.pos.x - c.pos.x;
                const float dy = p.pos.y - c.pos.y;
                const float d2 = dx * dx + dy * dy;
                if (d2 < bestD2) { bestD2 = d2; off = {dx, dy}; seen = true; }
            }
            if (seen) {
                const float d    = std::sqrt(bestD2);
                const float invD = (d > 1e-3f) ? 1.0f / d : 0.0f;
                const Vec2  u    = {off.x * invD, off.y * invD};  // unit vector TOWARD predator
                sensors[PREDATOR_FWD]  = u.x * fwd.x + u.y * fwd.y;
                sensors[PREDATOR_LR]   = u.x * rht.x + u.y * rht.y;
                sensors[PREDATOR_PROX] = 1.0f - d / cfg.perceptionRadius;
            } else {
                sensors[PREDATOR_FWD]  = 0.0f;
                sensors[PREDATOR_LR]   = 0.0f;
                sensors[PREDATOR_PROX] = 0.0f;
            }
        }

        // Long probe (reuse scratch buffer)
        std::vector<int>& longProbe = scratch.longProbe;
        longProbe.clear();
        Vec2 probePos = {c.pos.x + fwd.x * LONGPROBE_DIST, c.pos.y + fwd.y * LONGPROBE_DIST};
        creatureGrid.queryRadius(probePos, 30.0f, longProbe);
        sensors[LONGPROBE_POP_FWD] = std::tanh((float)longProbe.size() / 5.0f);

        // Random
        sensors[RANDOM_SENSOR] = rng.range(-1.0f, 1.0f);
    }
};
