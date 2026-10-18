#pragma once
#include <vector>
#include <cstdint>
#include <cmath>
#include "vec2.hpp"
#include "portable_math.hpp"
#include "color.hpp"
#include "genome_color.hpp"
#include "genome.hpp"
#include "sensors_actions.hpp"

struct Creature {
    bool alive = true;
    std::uint64_t id = 0;  // stable identity (assigned by World on creation), so a
                           // click-selected creature can be tracked as the vector
                           // is reordered by death/reproduction. 0 = unassigned.
    std::uint64_t lineageId = 0;  // founder id of this creature's maternal line:
                           // own id for a spawned/reseeded founder, else inherited
                           // from parent1 at birth. Lets the cluster map colour by
                           // clade and lets selective sweeps be measured. Dynamics-
                           // neutral (integer copy only; never touches the rng).
    std::uint32_t generation = 0;  // 0 for a spawned/reseeded founder, else
                           // max(source records' generation) + 1 at birth.
                           // Generations overlap (continuous birth and death),
                           // so this is a depth count, not a synchronised epoch.
                           // Display-only and dynamics-neutral, like lineageId.
    Vec2 pos;
    Vec2 vel;
    Vec2 lastMoveDir = {1.0f, 0.0f}; // biosim4: last movement direction
    float speed = 80.0f;
    float age = 0.0f;
    float foodReserves = 30.0f;
    float reproTimer = 0.0f;
    float interactTimer = 0.0f;  // cooldown between social interactions

    // biosim4 internal state
    float responsiveness = 0.5f;  // 0..1, scales action outputs
    float oscPeriod = 34.0f;      // oscillator period
    float oscPhase = 0.0f;        // current oscillator phase

    // --- Tier-1 body model (v0.23.0; see body_model.hpp) -------------------
    // Heritable morphology, held OUTSIDE the neural genome on purpose so the
    // body can vary independently of the wiring. Both default to 1.0, which
    // makes every derived factor exactly 1.0 and every apply site bit-identical
    // to the pre-0.23 code. They only ever move when SimConfig::bodyModel is on
    // (the inheritance step in World::doInteraction is the sole writer; a
    // founder or a reseeded creature keeps the default).
    float bodySize  = 1.0f;   // mass / scale proxy, clamped to [0.5, 2.0]
    float bodyElong = 1.0f;   // elongation,         clamped to [0.5, 2.0]

    Genome genome;
    NeuralNet nnet;

    // Last neural I/O, captured each step ONLY when SimConfig.captureActivations
    // is on (i.e. when there's a window to inspect them; off in headless so it
    // costs nothing). The hidden-neuron outputs already persist in nnet.neurons[];
    // sensors and actions are otherwise transient, so the live overlay needs these.
    float lastSensors[NUM_SENSES] = {};
    float lastActions[NUM_ACTIONS] = {};

    // Visual (geometry/colour only — no rendering dependency)
    static constexpr int BODY_LEN = 6;
    Vec2 body[BODY_LEN];
    Color color;

    void init(Vec2 startPos) {
        pos = startPos;
        vel = {rng.range(-1.0f, 1.0f), rng.range(-1.0f, 1.0f)};
        float len = std::sqrt(vel.x * vel.x + vel.y * vel.y);
        if (len > 0.001f) { vel.x /= len; vel.y /= len; }
        lastMoveDir = vel;
        oscPhase = rng.range(0.0f, 6.28f);

        buildNeuralNet(genome, nnet);

        // Locality-preserving colour from the genome (see genome_color.hpp):
        // similar genomes -> similar colours, so lineages drift smoothly.
        color = genomeColor(genome);

        for (int i = 0; i < BODY_LEN; ++i) {
            body[i] = {pos.x - vel.x * i * 4.0f, pos.y - vel.y * i * 4.0f};
        }
    }

    float getOscillator() const {
        return evomath::sinf(oscPhase);
    }

    // Forward direction unit vector
    Vec2 forward() const {
        float len = std::sqrt(vel.x * vel.x + vel.y * vel.y);
        if (len < 0.001f) return {1.0f, 0.0f};
        return {vel.x / len, vel.y / len};
    }

    // Right-perpendicular direction
    Vec2 right() const {
        auto fwd = forward();
        return {-fwd.y, fwd.x};
    }

    void updateBody() {
        body[0] = pos;
        for (int i = 1; i < BODY_LEN; ++i) {
            float dx = body[i].x - body[i - 1].x;
            float dy = body[i].y - body[i - 1].y;
            float dist = std::sqrt(dx * dx + dy * dy);
            // Visual only — body[] feeds the renderer and the checkpoint, and no
            // sensor reads it. A bigger/longer creature draws a longer trail.
            // At the defaults this is 4.0f * 1.0f * 1.0f, which IEEE-754
            // evaluates to exactly 4.0f, so the off-path geometry is unchanged.
            // (Creature::init()'s initial spacing stays a flat 4.0f: the body
            // parameters are assigned after construction, and updateBody catches
            // the trail up on the next step.)
            float segLen = 4.0f * bodySize * bodyElong;
            if (dist > segLen) {
                float ratio = segLen / dist;
                body[i].x = body[i - 1].x + dx * ratio;
                body[i].y = body[i - 1].y + dy * ratio;
            }
        }
    }
};

inline Creature makeRandomCreature(Vec2 pos,
                                   int initMin = GENOME_INITIAL_MIN,
                                   int initMax = GENOME_INITIAL_MAX) {
    Creature c;
    c.genome = makeRandomGenome(initMin, initMax);
    c.init(pos);
    return c;
}

inline Creature makeChild(const Creature& p1, const Creature& p2,
                          int   minLen        = GENOME_MIN_LENGTH,
                          int   maxLen        = GENOME_MAX_LENGTH,
                          float mutMultiplier = 1.0f,
                          float newbornEnergy = 25.0f) {
    Creature child;
    child.genome = crossover(p1.genome, p2.genome);

    float mut = rng.unit();
    float rate;
    if (mut < 0.03f) rate = 0.5f;
    else if (mut < 0.10f) rate = 0.05f;
    else if (mut < 0.30f) rate = 0.01f;
    else rate = 0.002f;
    mutateGenome(child.genome, rate * mutMultiplier, minLen, maxLen);

    Vec2 midpoint = {
        (p1.pos.x + p2.pos.x) * 0.5f,
        (p1.pos.y + p2.pos.y) * 0.5f
    };
    child.init(midpoint);
    child.foodReserves = newbornEnergy;
    return child;
}
